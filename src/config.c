#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "ts.h"

#define MAX_VIDEO (64 << 20)
#define MIN_BUFFER 4

enum kind {
	NUMBER,
	SWITCH,
	ADDRESS,
	KEY,
};

struct setting {
	const char *section;
	const char *key;
	enum kind kind;
	size_t offset;
	int low;
	int high;
};

struct rule {
	char stream[64];
	int time;
	char video[256];
};

struct reader {
	const char *section;
	struct rule rules[MAX_STREAMS + 1];
	unsigned int count;
};

struct span {
	uint64_t first;
	uint64_t last;
	uint64_t total;
	uint64_t step;
	int seen;
};

struct config config;
atomic_int contained;
static uint64_t videos;

static const char *const sections[] = { "general", "srt", "ts", "rtsp", "offline", "streams" };
static const struct setting settings[] = {
	{ "general", "workers", NUMBER, offsetof(struct config, workers), 0, MAX_WORKERS },
	{ "general", "buffer", NUMBER, offsetof(struct config, buffer), MIN_BUFFER, 1024 },
	{ "general", "lag", NUMBER, offsetof(struct config, lag), 1000, 120000 },
	{ "general", "wait", NUMBER, offsetof(struct config, wait), 0, 5 },
	{ "general", "clients", NUMBER, offsetof(struct config, clients), 0, 65535 },
	{ "srt", "listen", ADDRESS, offsetof(struct config, srt), 0, 0 },
	{ "srt", "keylen", KEY, offsetof(struct config, keylen), 16, 32 },
	{ "srt", "latency", NUMBER, offsetof(struct config, latency), 20, 10000 },
	{ "srt", "timeout", NUMBER, offsetof(struct config, timeout), 1000, 5000 },
	{ "ts", "enabled", SWITCH, offsetof(struct config, ts_enabled), 0, 1 },
	{ "ts", "listen", ADDRESS, offsetof(struct config, ts), 0, 0 },
	{ "ts", "backlog", NUMBER, offsetof(struct config, backlog), 0, 30000 },
	{ "rtsp", "enabled", SWITCH, offsetof(struct config, rtsp_enabled), 0, 1 },
	{ "rtsp", "listen", ADDRESS, offsetof(struct config, rtsp), 0, 0 },
	{ "rtsp", "backlog", NUMBER, offsetof(struct config, rtsp_backlog), 0, 30000 },
	{ "rtsp", "reconnect", SWITCH, offsetof(struct config, reconnect), 0, 1 },
};

static int parse_address(const char *text, struct sockaddr_storage *address, socklen_t *length) {
	struct sockaddr_in *in4 = (struct sockaddr_in *)address;
	struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)address;
	const char *colon;
	char host[64], *end;
	size_t n;
	long port;
	colon = *text == '[' ? strstr(text, "]:") : strrchr(text, ':');
	if (!colon) return 0;
	n = (size_t)(colon - text) - (*text == '[');
	if (n >= sizeof(host)) return 0;
	memcpy(host, text + (*text == '['), n);
	host[n] = '\0';
	colon += *text == '[' ? 2 : 1;
	errno = 0;
	port = strtol(colon, &end, 10);
	if (end == colon || *end || errno || port < 1 || port > 65535) return 0;
	memset(address, 0, sizeof(*address));
	if (inet_pton(AF_INET, host, &in4->sin_addr) == 1) {
		in4->sin_family = AF_INET;
		in4->sin_port = htons((uint16_t)port);
		*length = sizeof(*in4);
		return 1;
	}
	memset(address, 0, sizeof(*address));
	if (inet_pton(AF_INET6, host, &in6->sin6_addr) == 1) {
		in6->sin6_family = AF_INET6;
		in6->sin6_port = htons((uint16_t)port);
		*length = sizeof(*in6);
		return 1;
	}
	return 0;
}

static int set_address(struct endpoint *endpoint, const char *text) {
	if (strlen(text) >= sizeof(endpoint->text) || !parse_address(text, &endpoint->address, &endpoint->length)) return 0;
	snprintf(endpoint->text, sizeof(endpoint->text), "%s", text);
	return 1;
}

static int number(const char *text, int low, int high, int *value) {
	char *end;
	long n;
	errno = 0;
	n = strtol(text, &end, 10);
	if (end == text || *end || errno || n < low || n > high) return 0;
	*value = (int)n;
	return 1;
}

static int is_name(const char *name) {
	size_t i, length = strlen(name);
	if (!length || length >= sizeof(((struct entry *)0)->name)) return 0;
	for (i = 0; i < length; i++) {
		if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '-' || name[i] == '_')) return 0;
	}
	return 1;
}

static struct entry *find_entry(struct plan *plan, const char *name) {
	unsigned int i;
	for (i = 0; i < plan->count; i++) {
		if (!strcmp(plan->entries[i].name, name)) return &plan->entries[i];
	}
	return NULL;
}

static void defaults(struct config *values) {
	values->buffer = 64;
	values->lag = 10000;
	values->wait = 5;
	values->keylen = 32;
	values->latency = 1000;
	values->timeout = 5000;
	values->ts_enabled = 1;
	values->rtsp_enabled = 1;
	values->clients = 32;
	set_address(&values->srt, "[::]:8890");
	set_address(&values->ts, "127.0.0.1:8080");
	set_address(&values->rtsp, "[::]:554");
}

static const char *add_stream(struct plan *plan, const char *name, const char *passphrase, const char *extra) {
	static char problem[192];
	struct entry *entry = &plan->entries[plan->count];
	size_t length = passphrase ? strlen(passphrase) : 0;
	if (!is_name(name)) return "stream names use letters, digits, - and _ only, 63 at most";
	if (extra) return "expected: <name> <passphrase of 16 to 79 letters, digits, -, _, . or ~>";
	if (!passphrase) {
		snprintf(problem, sizeof(problem), "%s has no passphrase, expected: <name> <passphrase of 16 to 79 letters, digits, -, _, . or ~>", name);
		return problem;
	}
	if (length < 16 || length > 79) {
		snprintf(problem, sizeof(problem), "%s's passphrase has %zu characters, 16 to 79 expected", name, length);
		return problem;
	}
	if (passphrase[strspn(passphrase, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~")]) {
		snprintf(problem, sizeof(problem), "%s's passphrase has a character other than letters, digits, -, _, . or ~", name);
		return problem;
	}
	if (find_entry(plan, name)) return "stream already defined";
	if (plan->count == MAX_STREAMS) return "too many streams";
	snprintf(entry->name, sizeof(entry->name), "%s", name);
	snprintf(entry->passphrase, sizeof(entry->passphrase), "%s", passphrase);
	plan->count++;
	return NULL;
}

static const char *add_rule(struct reader *reader, const char *name, const char *time, const char *video) {
	struct rule *rule = &reader->rules[reader->count];
	unsigned int i;
	if ((strcmp(name, "*") && !is_name(name)) || !time) return "expected: <stream or *> <seconds, always or off> [video.ts or none]";
	if (video && strlen(video) >= sizeof(rule->video)) return "path too long";
	for (i = 0; i < reader->count; i++) {
		if (!strcmp(reader->rules[i].stream, name)) return "this stream already has an offline line";
	}
	if (reader->count == MAX_STREAMS + 1) return "too many offline lines";
	if (!strcmp(time, "always")) {
		rule->time = ALWAYS;
	} else if (!strcmp(time, "off")) {
		rule->time = 0;
	} else if (!number(time, 1, 86400, &rule->time)) {
		return "expected a time from 1 to 86400 s, always or off";
	}
	snprintf(rule->stream, sizeof(rule->stream), "%s", name);
	snprintf(rule->video, sizeof(rule->video), "%s", video ? video : "");
	reader->count++;
	return NULL;
}

static const char *apply(struct reader *reader, struct plan *plan, const char *key, const char *first, const char *second) {
	static char problem[192];
	const struct setting *setting = NULL;
	char range[48];
	void *value;
	unsigned int i;
	if (!reader->section) return "setting outside a [section]";
	if (!strcmp(reader->section, "streams")) return add_stream(plan, key, first, second);
	if (!strcmp(reader->section, "offline")) return add_rule(reader, key, first, second);
	for (i = 0; !setting && i < sizeof(settings) / sizeof(settings[0]); i++) {
		if (!strcmp(reader->section, settings[i].section) && !strcmp(key, settings[i].key)) {
			setting = &settings[i];
		}
	}
	if (!setting) {
		snprintf(problem, sizeof(problem), "unknown setting %.24s in [%s]", key, reader->section);
		return problem;
	}
	if (second) return "too many values";
	value = (char *)&plan->config + setting->offset;
	if (first && setting->kind == ADDRESS && set_address(value, first)) return NULL;
	if (first && setting->kind == SWITCH && (!strcmp(first, "yes") || !strcmp(first, "no"))) {
		*(int *)value = *first == 'y';
		return NULL;
	}
	if (first && setting->kind == NUMBER && number(first, setting->low, setting->high, value)) return NULL;
	if (first && setting->kind == KEY && number(first, setting->low, setting->high, value) && !(*(int *)value % 8)) return NULL;
	snprintf(range, sizeof(range), "a number from %d to %d", setting->low, setting->high);
	snprintf(problem, sizeof(problem), first ? "[%s] %s %.40s, expected %s" : "[%s] %s%.0s has no value, expected %s", setting->section, setting->key, first ? first : "", setting->kind == ADDRESS ? "an address like 127.0.0.1:8080 or [::]:8890" : setting->kind == SWITCH ? "yes or no" : setting->kind == KEY ? "16, 24 or 32 bytes (AES-128, AES-192 or AES-256)" : range);
	return problem;
}

static const char *enter(struct reader *reader, const char *header) {
	size_t length = strlen(header);
	unsigned int i;
	for (i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
		if (length != strlen(sections[i]) + 2 || header[length - 1] != ']' || strncmp(header + 1, sections[i], length - 2)) continue;
		reader->section = sections[i];
		return NULL;
	}
	return "unknown section, expected [general], [srt], [ts], [rtsp], [offline] or [streams]";
}

static const char *check(struct reader *reader, struct plan *plan) {
	static char problem[256];
	const struct config *values = &plan->config;
	unsigned int i;
	int ts = values->ts_enabled && values->lag <= values->backlog;
	if (!plan->count) return "no stream defined in [streams]";
	if (!values->ts_enabled && !values->rtsp_enabled) return "[ts] and [rtsp] are both disabled, enable one";
	if (values->ts_enabled && values->rtsp_enabled && ((const struct sockaddr_in *)&values->ts.address)->sin_port == ((const struct sockaddr_in *)&values->rtsp.address)->sin_port) {
		snprintf(problem, sizeof(problem), "[ts] listen %s and [rtsp] listen %s use the same port", values->ts.text, values->rtsp.text);
		return problem;
	}
	if (ts || (values->rtsp_enabled && values->lag <= values->rtsp_backlog)) {
		snprintf(problem, sizeof(problem), "[general] lag %d must be longer than [%s] backlog %d", values->lag, ts ? "ts" : "rtsp", ts ? values->backlog : values->rtsp_backlog);
		return problem;
	}
	for (i = 0; i < reader->count; i++) {
		if (strcmp(reader->rules[i].stream, "*") && !find_entry(plan, reader->rules[i].stream)) {
			snprintf(problem, sizeof(problem), "[offline] line for %s, which is not in [streams]", reader->rules[i].stream);
			return problem;
		}
	}
	return NULL;
}

static int not_48k(const unsigned char *p) {
	int at = payload_of(p), adts;
	if (!(p[1] & 0x40) || at + 9 >= PACKET) return 0;
	adts = at + 9 + p[at + 8];
	return adts + 3 <= PACKET && p[adts] == 0xff && ((p[adts + 2] >> 2) & 15) != 3;
}

void free_video(struct video *video) {
	if (!video) return;
	free(video->data);
	free(video);
}

static uint64_t memory(void) {
	char group[256] = "", path[300];
	struct sysinfo info;
	uint64_t total = sysinfo(&info) == 0 ? (uint64_t)info.totalram * info.mem_unit : 0;
	unsigned long long limit = 0;
	FILE *file = fopen("/proc/self/cgroup", "re");
	if (file && fscanf(file, "0::%255s", group) != 1) {
		group[0] = '\0';
	}
	if (file) {
		fclose(file);
	}
	snprintf(path, sizeof(path), "/sys/fs/cgroup%s/memory.max", group);
	file = fopen(path, "re");
	if (!file) {
		file = fopen("/sys/fs/cgroup/memory/memory.limit_in_bytes", "re");
	}
	if (file && fscanf(file, "%llu", &limit) != 1) {
		limit = 0;
	}
	if (file) {
		fclose(file);
	}
	atomic_store(&contained, limit && limit < total);
	return limit && limit < total ? limit : total ? total : UINT64_MAX;
}

static uint64_t footprint(const struct config *values) {
	return (uint64_t)values->buffer * 1048576 * (unsigned int)(values->ts_enabled + values->rtsp_enabled) + (values->rtsp_enabled ? VIDEO_TRACK + AUDIO_TRACK : 0);
}

static unsigned long long mib(uint64_t bytes) {
	return (unsigned long long)((bytes + 524288) >> 20);
}

unsigned int stream_room(void) {
	uint64_t half = memory() / 2, room = half > videos ? (half - videos) / footprint(&config) : 0;
	return room < MAX_STREAMS ? (unsigned int)room : MAX_STREAMS;
}

static int fits(const struct plan *plan) {
	uint64_t total = memory(), half = total / 2, own = footprint(&plan->config), running = footprint(&config), each = own > running ? own : running, rtsp = plan->config.rtsp_enabled ? VIDEO_TRACK + AUDIO_TRACK : 0, need, spare, buffer, room;
	unsigned int i, j, count = plan->count;
	int limited = atomic_load(&contained), used = 0;
	char fix[320];
	for (videos = 0, i = 0; i < count; i++) {
		for (j = 0; j < i && plan->entries[j].video != plan->entries[i].video; j++) continue;
		if (j == i && plan->entries[i].video) {
			videos += plan->entries[i].video->size;
		}
	}
	need = count * each + videos;
	if (need <= half) return 1;
	spare = half > videos ? (half - videos) / count : 0;
	buffer = spare > rtsp ? (spare - rtsp) / (unsigned int)(plan->config.ts_enabled + plan->config.rtsp_enabled) >> 20 : 0;
	room = half > videos ? (half - videos) / each : 0;
	if (limited) {
		used += snprintf(fix, sizeof(fix), "raise the container's memory limit to %llu GiB (mem_limit and memswap_limit in docker-compose.yml)", (unsigned long long)((2 * need + (1 << 30) - 1) >> 30));
	}
	if (buffer >= MIN_BUFFER) {
		used += snprintf(fix + used, sizeof(fix) - (size_t)used, "%slower [general] buffer to %llu%s", !used ? "" : room ? ", " : ", or ", (unsigned long long)buffer, running ? " and restart" : "");
	}
	if (room) {
		used += snprintf(fix + used, sizeof(fix) - (size_t)used, "%sremove %llu stream%s", used ? ", or " : "", (unsigned long long)(count - room), count - room > 1 ? "s" : "");
	}
	flockfile(stderr);
	note(ON_CONFIG | FAIL, "%u stream%s not fit in memory%s: %s %llu MiB, half of the %s %llu MiB%s is %llu MiB", count, count > 1 ? "s do" : " does", each > own ? " with the running settings" : "", count > 1 ? "they need" : "it needs", mib(need), limited ? "container's" : "host's", mib(total), limited ? " limit" : "", mib(half));
	if (count * own + videos <= half) {
		note(ON_CONFIG, "with the new settings %s %llu MiB, so %s after a restart", count > 1 ? "they need" : "it needs", mib(count * own + videos), count > 1 ? "they fit" : "it fits");
	} else if (used) {
		note(ON_CONFIG, "to fit %s, %s", count > 1 ? "them" : "it", fix);
	}
	funlockfile(stderr);
	return 0;
}

static const char *shown(const struct setting *setting, const struct config *values, char *text, size_t size) {
	const char *value = (const char *)values + setting->offset;
	if (setting->kind == ADDRESS) return ((const struct endpoint *)value)->text;
	if (setting->kind == SWITCH) return *(const int *)value ? "yes" : "no";
	snprintf(text, size, "%d", *(const int *)value);
	return text;
}

void note_restart(const struct config *next) {
	char was[16], now[16];
	const char *before, *after;
	unsigned int i;
	for (i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
		before = shown(&settings[i], &config, was, sizeof(was));
		after = shown(&settings[i], next, now, sizeof(now));
		if (strcmp(before, after)) {
			note(ON_CONFIG | WARN, "[%s] %s %s → %s takes effect after a restart", settings[i].section, settings[i].key, before, after);
		}
	}
}

static void extend(struct span *span, uint64_t time) {
	uint64_t step = (time - span->last) & (WRAP - 1);
	if (span->seen && step <= VIDEO_JUMP) {
		span->total += step;
		span->step = step;
	}
	span->first = span->seen ? span->first : time;
	span->last = time;
	span->seen = 1;
}

static unsigned int adts_frames(const struct video *video, int pid, size_t at) {
	unsigned char es[1 << 16];
	const unsigned char *p;
	size_t used = 0, start, n;
	unsigned int frames = 0;
	for (; at < video->size; at += PACKET) {
		p = video->data + at;
		start = (size_t)payload_of(p);
		if (p[1] & 0x40) {
			start = start + 9 < PACKET ? start + 9 + p[start + 8] : PACKET;
		}
		if (pid_of(p) != pid || start >= PACKET) continue;
		n = PACKET - start;
		if (used + n > sizeof(es)) break;
		memcpy(es + used, p + start, n);
		used += n;
	}
	for (start = 0; start + 7 <= used && es[start] == 0xff && (es[start + 1] & 0xf6) == 0xf0; frames++) {
		n = ((size_t)(es[start + 3] & 3) << 11) | ((size_t)es[start + 4] << 3) | (es[start + 5] >> 5);
		if (n < 7 || start + n > used) break;
		start += n;
	}
	return frames;
}

static struct video *load_video(const char *path) {
	struct video *video = calloc(1, sizeof(*video));
	struct span clock = { 0 }, frames = { 0 };
	uint64_t first = 0, last = 0;
	unsigned char *p, *stamp;
	const char *problem = NULL;
	struct feed feed;
	struct stat info;
	size_t done = 0, i = 0, latest = 0;
	ssize_t n = 1;
	int fd, keys = 0, clocks = 0, own, count, heard = 0;
	if (!video) return NULL;
	snprintf(video->path, sizeof(video->path), "%s", path);
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &info) < 0 || (S_ISDIR(info.st_mode) && (errno = EISDIR))) {
		note(ON_CONFIG | FAIL, "offline video %s: %s", path, errno == ENOENT ? "not found in the container" : strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		free_video(video);
		return NULL;
	}
	if (info.st_size > MAX_VIDEO) {
		note(ON_CONFIG | FAIL, "offline video %s: %lld MiB, larger than %d MiB, record fewer seconds", path, (long long)((info.st_size + (1 << 20) - 1) >> 20), MAX_VIDEO >> 20);
		close(fd);
		free_video(video);
		return NULL;
	}
	video->device = info.st_dev;
	video->inode = info.st_ino;
	video->modified = info.st_mtim;
	video->size = info.st_size > 0 ? (size_t)info.st_size / PACKET * PACKET : 0;
	video->data = video->size ? malloc(video->size) : NULL;
	while (video->data && done < video->size && (n = read(fd, video->data + done, video->size - done)) > 0) {
		done += (size_t)n;
	}
	close(fd);
	reset_feed(&feed);
	for (i = 0; video->data && done == video->size && i < video->size && video->data[i] == 0x47; i += PACKET) {
		p = video->data + i;
		scan(&feed, p);
		if (feed.ready == 3 && pid_of(p) == feed.audio_pid && not_48k(p)) break;
		own = feed.ready == 3 && pid_of(p) == feed.video_pid;
		clocks += keys && own && has_pcr(p);
		keys += own && is_key(&feed, p);
		if (keys && own && has_pcr(p)) {
			extend(&clock, get_pcr(p));
		}
		if (keys && own && (stamp = stamps(p, &count))) {
			extend(&frames, get_stamp(stamp + 5 * (count - 1)));
		}
		if (keys && feed.ready == 3 && pid_of(p) == feed.audio_pid && (stamp = stamps(p, &count))) {
			first = heard++ ? first : get_stamp(stamp);
			last = get_stamp(stamp);
			latest = i;
		}
	}
	if (!video->size) {
		problem = "empty";
	} else if (!video->data || done != video->size) {
		problem = strerror(video->data ? EIO : ENOMEM);
	} else if (i != video->size) {
		problem = video->data[i] == 0x47 ? "its audio is not 48 kHz, record it at 48 kHz" : "not MPEG-TS, record it as a .ts file";
	} else if (feed.ready != 3) {
		problem = "no H.264 video with AAC audio, record it with the stream's settings";
	} else if (!keys || !clocks) {
		problem = "no keyframe, record 10 to 20 seconds";
	} else if (frames.total > clock.total + 90000) {
		problem = "its clock does not follow its frames, record it again";
	}
	if (problem) {
		note(ON_CONFIG | FAIL, "offline video %s: %s", path, problem);
		free_video(video);
		return NULL;
	}
	video->tail = heard ? (last + 1920 * (uint64_t)adts_frames(video, feed.audio_pid, latest) - first + frames.first - frames.last - frames.step) & (WRAP - 1) : 0;
	if (video->tail >= 90000) {
		video->tail = 0;
	}
	return video;
}

void free_plan(struct plan *plan) {
	unsigned int i, j;
	if (!plan) return;
	for (i = 0; i < plan->count; i++) {
		for (j = 0; j < i && plan->entries[j].video != plan->entries[i].video; j++) continue;
		if (j == i) {
			free_video(plan->entries[i].video);
		}
	}
	free(plan);
}

static int resolve(struct reader *reader, struct plan *plan) {
	static const struct rule fallback = { "*", 30, "" };
	const struct rule *exact, *any;
	struct entry *entry;
	const char *path;
	unsigned int i, j;
	for (i = 0; i < plan->count; i++) {
		entry = &plan->entries[i];
		exact = NULL;
		any = &fallback;
		for (j = 0; j < reader->count; j++) {
			if (!strcmp(reader->rules[j].stream, entry->name)) {
				exact = &reader->rules[j];
			} else if (!strcmp(reader->rules[j].stream, "*")) {
				any = &reader->rules[j];
			}
		}
		entry->offline = exact ? exact->time : any->time;
		path = exact && *exact->video ? exact->video : any->video;
		if (!entry->offline || !*path || !strcmp(path, "none")) continue;
		for (j = 0; j < i && !(plan->entries[j].video && !strcmp(plan->entries[j].video->path, path)); j++) continue;
		entry->video = j < i ? plan->entries[j].video : load_video(path);
		if (!entry->video) return 0;
	}
	for (i = 0; i < plan->count; i++) {
		if (plan->entries[i].offline == ALWAYS && !plan->entries[i].video) {
			note(ON_CONFIG | FAIL, "%s: an offline time of always needs a video", plan->entries[i].name);
			return 0;
		}
	}
	return 1;
}

struct plan *read_plan(const char *path, int probe) {
	char line[512], *start, *key, *first, *second, *extra, *save;
	struct reader reader = { 0 };
	const char *problem = NULL;
	struct plan *plan;
	FILE *file;
	int count = 0, c;
	plan = calloc(1, sizeof(*plan));
	if (!plan) return NULL;
	defaults(&plan->config);
	file = fopen(path, "re");
	if (!file) {
		note(ON_CONFIG | FAIL, "%s: %s", path, strerror(errno));
		free(plan);
		return NULL;
	}
	while (!problem && fgets(line, sizeof(line), file)) {
		if (!count++ && !strncmp(line, "\xef\xbb\xbf", 3)) {
			memmove(line, line + 3, strlen(line + 3) + 1);
		}
		if (!strchr(line, '\n') && !feof(file) && probe) {
			while ((c = fgetc(file)) != EOF && c != '\n') continue;
			continue;
		}
		if (!strchr(line, '\n') && !feof(file)) {
			problem = "line too long, 510 characters at most";
			break;
		}
		line[strcspn(line, "\r\n")] = '\0';
		start = line + strspn(line, " \t");
		if (!*start || *start == '#') continue;
		key = strtok_r(start, " \t", &save);
		first = strtok_r(NULL, " \t", &save);
		second = strtok_r(NULL, " \t", &save);
		extra = strtok_r(NULL, " \t", &save);
		if ((first && *first == '#') || (second && *second == '#') || (extra && *extra == '#')) {
			problem = "a # comment goes on its own line";
		} else if (*key == '[') {
			reader.section = NULL;
			problem = first ? "expected [section] alone on its line" : enter(&reader, key);
		} else if (extra) {
			problem = "too many values";
		} else {
			problem = apply(&reader, plan, key, first, second);
		}
		if (probe) {
			problem = NULL;
		}
	}
	fclose(file);
	if (probe) return plan;
	if (problem) {
		note(ON_CONFIG | FAIL, "%s:%d: %s", path, count, problem);
	} else if ((problem = check(&reader, plan))) {
		note(ON_CONFIG | FAIL, "%s: %s", path, problem);
	}
	if (problem || !resolve(&reader, plan) || !fits(plan)) {
		free_plan(plan);
		return NULL;
	}
	return plan;
}
