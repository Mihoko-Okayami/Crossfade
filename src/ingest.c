#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <srt/access_control.h>

#include "ingest.h"
#include "log.h"
#include "main.h"
#include "server.h"
#include "stream.h"

#define INGEST_INTERVAL 10
#define REST_INTERVAL 500
#define RECEIVE_BATCH 256
#define VIDEO_LEAD 100
#define PLAY_BURST (1 << 20)
#define STATS_INTERVAL 1000
#define REPORT_INTERVAL 10000
#define UNUSABLE_DELAY 5000
#define REPORT_GAP 1000
#define CALLERS 4096
#define CALLER_TRIES 5
#define CALLER_WINDOW 10000
#define HANDSHAKES 20
#define ATTEMPTS 128
#define SILENCE 1000

enum refusal {
	FREE,
	WAITING,
	NAMELESS,
	UNKNOWN,
	MISUSED,
	COOLING,
	SWAMPED,
};

struct caller {
	struct peer peer;
	unsigned int tries;
	uint64_t at;
};

struct attempt {
	SRTSOCKET sock;
	enum refusal kind;
	uint64_t at;
	char name[64];
	char host[INET6_ADDRSTRLEN];
};

static SRTSOCKET srt_listener = -1;
static int srt_poll = -1;
static int wake = -1;
static struct plan *_Atomic pending;
static pthread_mutex_t secrets = PTHREAD_MUTEX_INITIALIZER;
static struct caller callers[CALLERS];
static uint64_t caller_seed;
static uint64_t budget_at;
static unsigned int budget;
static struct attempt attempts[ATTEMPTS];
static unsigned int oldest, newest;

static void finish(struct stream *stream) {
	struct packer *packer = &stream->packer;
	atomic_store(&stream->state, IDLE);
	ring_forget(&stream->ts);
	ring_forget(&stream->rtp);
	ring_shrink(&stream->ts);
	ring_shrink(&stream->rtp);
	pthread_mutex_lock(&stream->rtp.lock);
	stream->sdp_size = 0;
	stream->rate = 0;
	pthread_mutex_unlock(&stream->rtp.lock);
	packer->video.open = packer->audio.open = 0;
	packer->video.cc = packer->audio.cc = -1;
	packer->sps_size = packer->pps_size = packer->held_size = 0;
	packer->rate = packer->channels = 0;
	packer->au_bytes = packer->au_count = 0;
	atomic_store(&stream->audio_kbps, 0);
	memset(packer->config, 0, sizeof(packer->config));
	stream->timeline = 0;
	stream->aired = 0;
	note(ON_STREAM, "%s: stream ended", stream->name);
}

static void rewind_video(struct stream *stream, uint64_t now) {
	reset_feed(&stream->still);
	stream->whole = 0;
	stream->video_pos = 0;
	stream->video_started = 0;
	stream->video_clocked = 0;
	stream->video_ticks = 0;
	stream->video_base = now;
	stream->video_at = 0;
}

static void leave(struct stream *stream, uint64_t now) {
	if (!stream->offline) {
		finish(stream);
		return;
	}
	atomic_store(&stream->state, OFFLINE);
	stream->offline_until = stream->offline == ALWAYS ? UINT64_MAX : now + (uint64_t)stream->offline * 1000;
	rewind_video(stream, now);
	if (stream->offline == ALWAYS) {
		note(ON_STREAM, "%s: showing the offline video until a publisher is live", stream->name);
	} else if (stream->video) {
		note(ON_STREAM, "%s: showing the offline video for %d s", stream->name, stream->offline);
	} else {
		note(ON_STREAM, config.ts_enabled ? "%s: keeping .ts viewers %d s for the publisher" : "%s: waiting %d s for the publisher", stream->name, stream->offline);
	}
}

static void take_live(struct stream *stream, unsigned char *p) {
	int synced = stream->live.synced;
	take(stream, &stream->live, p);
	if (synced || !stream->live.synced) return;
	atomic_store(&stream->state, LIVE);
	note(ON_STREAM, stream->aired ? "%s: live again" : "%s: live", stream->name);
	stream->aired = 1;
}

static void feed_bytes(struct stream *stream, unsigned char *data, size_t length) {
	struct feed *feed = &stream->live;
	unsigned char *next;
	size_t n;
	while (length) {
		if (feed->partial_used) {
			n = PACKET - feed->partial_used < length ? PACKET - feed->partial_used : length;
			memcpy(feed->partial + feed->partial_used, data, n);
			feed->partial_used += n;
			data += n;
			length -= n;
			if (feed->partial_used < PACKET) return;
			feed->partial_used = 0;
			take_live(stream, feed->partial);
		} else if (*data != 0x47) {
			next = memchr(data, 0x47, length);
			n = next ? (size_t)(next - data) : length;
			stream->skipped += n;
			data += n;
			length -= n;
		} else if (length < PACKET) {
			memcpy(feed->partial, data, length);
			feed->partial_used = length;
			return;
		} else {
			take_live(stream, data);
			data += PACKET;
			length -= PACKET;
		}
	}
}

static void disconnect(struct stream *stream) {
	srt_epoll_remove_usock(srt_poll, stream->publisher);
	srt_close(stream->publisher);
	stream->publisher = SRT_INVALID_SOCK;
	reset_feed(&stream->live);
}

static void lose(struct stream *stream, uint64_t now) {
	int64_t silent = (int64_t)(now - stream->last_data);
	disconnect(stream);
	if (silent >= SILENCE) {
		note(ON_STREAM, "%s: publisher %s disconnected after %lld ms without data", stream->name, stream->peer, (long long)silent + stream->latency);
	} else {
		note(ON_STREAM, "%s: publisher %s disconnected", stream->name, stream->peer);
	}
	if (atomic_load(&stream->state) == LIVE) {
		leave(stream, now);
	}
}

static void receive(struct stream *stream, uint64_t now) {
	static unsigned char buffer[1500];
	int n, count;
	for (count = 0; count < RECEIVE_BATCH; count++) {
		n = srt_recvmsg2(stream->publisher, (char *)buffer, sizeof(buffer), NULL);
		if (n <= 0) {
			if (n == 0 || srt_getlasterror(NULL) != SRT_EASYNCRCV) {
				lose(stream, now);
			}
			break;
		}
		feed_bytes(stream, buffer, (size_t)n);
		stream->last_data = now;
		publish(stream, now);
	}
}

static int jumps(const struct stream *stream, unsigned char *p) {
	uint64_t gap;
	int count;
	unsigned char *stamp = stamps(p, &count);
	if (has_pcr(p)) {
		gap = (get_pcr(p) + stream->still.offset - stream->last_pcr) & (WRAP - 1);
		if (gap > VIDEO_JUMP && gap < WRAP - VIDEO_JUMP) return 1;
	}
	if (!stamp) return 0;
	gap = (get_stamp(stamp + 5 * (count - 1)) + stream->still.offset - stream->last_ts) & (WRAP - 1);
	return gap > VIDEO_JUMP && gap < WRAP - VIDEO_JUMP;
}

static int rewinds(const struct stream *stream, const unsigned char *p) {
	return has_pcr(p) && stream->video_clocked && ((get_pcr(p) - stream->video_pcr) & (WRAP - 1)) >= WRAP / 2;
}

static void play(struct stream *stream, uint64_t now) {
	struct video *video = stream->video;
	uint64_t pcr, step, before;
	unsigned char *p;
	size_t sent;
	int synced, own;
	if (now - stream->video_at < INGEST_INTERVAL) return;
	stream->video_at = now;
	for (sent = 0; sent < PLAY_BURST; sent += PACKET) {
		p = video->data + stream->video_pos;
		own = pid_of(p) == stream->still.video_pid || pid_of(p) == stream->still.audio_pid;
		synced = stream->still.synced;
		if (synced && own && (jumps(stream, p) || rewinds(stream, p))) {
			stream->still.synced = synced = 0;
			stream->still.cc[0] = stream->still.cc[1] = -1;
		}
		if (own && has_pcr(p)) {
			stream->video_pcr = get_pcr(p);
			stream->video_clocked = 1;
		}
		if (synced && own && has_pcr(p)) {
			pcr = (get_pcr(p) + stream->still.offset) & (WRAP - 1);
			step = (pcr - stream->last_pcr) & (WRAP - 1);
			if (step >= WRAP / 2) {
				step = 0;
			}
			if (stream->video_base + (stream->video_ticks + step) / 90 > now + VIDEO_LEAD) break;
			stream->video_ticks += step;
		}
		before = stream->last_pcr;
		take(stream, &stream->still, p);
		if (!synced && stream->still.synced && stream->video_started) {
			stream->video_ticks += (stream->last_pcr - before) & (WRAP - 1);
		}
		stream->video_started |= stream->still.synced;
		stream->video_pos += PACKET;
		if (stream->video_pos < video->size) continue;
		stream->video_pos = 0;
		stream->video_clocked = 0;
		stream->whole = stream->still.synced;
		if (stream->still.synced) {
			reset_feed(&stream->still);
		}
		break;
	}
	publish(stream, now);
}

static void tick(struct stream *stream, uint64_t now) {
	SRT_TRACEBSTATS stats;
	if (atomic_load(&stream->removed)) return;
	if (atomic_load(&stream->state) == IDLE && stream->offline == ALWAYS) {
		leave(stream, now);
	}
	if (stream->publisher != SRT_INVALID_SOCK && (int64_t)(now - stream->last_data) >= config.timeout) {
		lose(stream, now);
	}
	if (atomic_load(&stream->state) == OFFLINE && now >= stream->offline_until) {
		finish(stream);
	} else if (atomic_load(&stream->state) == OFFLINE && stream->video) {
		play(stream, now);
	} else if (atomic_load(&stream->state) == OFFLINE && !stream->video) {
		idle(stream, now);
	}
	if (now - stream->ts.window_start < STATS_INTERVAL) return;
	ring_measure(&stream->ts, now);
	ring_measure(&stream->rtp, now);
	if (stream->packer.au_count) {
		atomic_store(&stream->audio_kbps, (unsigned int)(stream->packer.au_bytes * 8 * (uint64_t)stream->packer.rate / 1024 / stream->packer.au_count / 1000));
	}
	stream->packer.au_bytes = stream->packer.au_count = 0;
	if (stream->publisher != SRT_INVALID_SOCK && srt_bstats(stream->publisher, &stats, 1) == 0) {
		stream->lost += (unsigned int)stats.pktRcvDrop;
		if (!stream->crowded && stats.byteAvailRcvBuf < stats.byteRcvBuf / 3) {
			stream->crowded = 1;
			note(ON_STREAM | WARN, "%s: SRT receive buffer nearly full at %d ms latency, the publisher must lower its latency or bitrate", stream->name, stats.msRcvTsbPdDelay);
		}
	}
	if (stream->publisher == SRT_INVALID_SOCK || stream->live.synced || stream->warned || (int64_t)(now - stream->connected) < UNUSABLE_DELAY) return;
	stream->warned = 1;
	note(ON_STREAM | WARN, stream->live.ready == 3 ? "%s: no keyframe from publisher %s in 5 s, set its keyframe interval to 1 s" : "%s: nothing usable from publisher %s in 5 s, it must send H.264 video and AAC audio", stream->name, stream->peer);
}

static void tally(struct stream *stream) {
	unsigned int lagged = atomic_exchange(&stream->lagging, 0);
	if (stream->lost) {
		note(ON_STREAM | WARN, "%s: %u packet%s lost by SRT in the last 10 s at %d ms latency, raise [srt] latency or the publisher's latency if it goes on", stream->name, stream->lost, stream->lost > 1 ? "s" : "", stream->latency);
	}
	if (stream->skipped) {
		note(ON_STREAM | WARN, "%s: %llu byte%s skipped to find MPEG-TS packets in the last 10 s", stream->name, (unsigned long long)stream->skipped, stream->skipped > 1 ? "s" : "");
	}
	if (lagged) {
		note(ON_VIEWER, "%s: %u time%s a viewer fell behind and skipped ahead to live in the last 10 s", stream->name, lagged, lagged > 1 ? "s" : "");
	}
	stream->skipped = stream->lost = 0;
}

static void retire(struct stream *stream) {
	if (stream->publisher != SRT_INVALID_SOCK) {
		disconnect(stream);
	}
	if (atomic_load(&stream->state) != IDLE) {
		finish(stream);
	}
	stream->offline = 0;
	stream->video = NULL;
	atomic_store(&stream->removed, 1);
	tally(stream);
	note(ON_CONFIG, "%s: removed", stream->name);
}

static void recycle(struct stream *stream, const char *name) {
	rename_stream(stream, name);
	memset(stream->cc, 15, sizeof(stream->cc));
	atomic_store(&stream->lagging, 0);
	ring_forget(&stream->ts);
	ring_forget(&stream->rtp);
}

static void update(struct stream *stream, const struct entry *entry, uint64_t now, int reload) {
	struct video *video = stream->video;
	int state = atomic_load(&stream->state), changed = stream->offline != entry->offline || video != entry->video;
	if (strcmp(stream->passphrase, entry->passphrase)) {
		if (*stream->passphrase && !atomic_load(&stream->removed)) {
			note(ON_CONFIG, "%s: new passphrase, for the publisher's next connection", stream->name);
		}
		pthread_mutex_lock(&secrets);
		snprintf(stream->passphrase, sizeof(stream->passphrase), "%s", entry->passphrase);
		pthread_mutex_unlock(&secrets);
	}
	stream->offline = entry->offline;
	stream->video = entry->video;
	if (state == OFFLINE && video != entry->video) {
		rewind_video(stream, now);
	}
	if (state == OFFLINE && changed) {
		stream->offline_until = entry->offline == ALWAYS ? UINT64_MAX : now + (uint64_t)entry->offline * 1000;
	}
	if (state == OFFLINE && !entry->offline) {
		finish(stream);
	}
	if (!changed || !reload) return;
	if (entry->video && entry->offline == ALWAYS) {
		note(ON_CONFIG, "%s: offline video %s, shown whenever no publisher is live", stream->name, entry->video->path);
	} else if (entry->video) {
		note(ON_CONFIG, "%s: offline video %s, shown for %d s after the publisher stops", stream->name, entry->video->path, entry->offline);
	} else if (entry->offline) {
		note(ON_CONFIG, config.ts_enabled ? "%s: no offline video, .ts viewers kept %d s after the publisher stops" : "%s: no offline video, waiting %d s for the publisher after it stops", stream->name, entry->offline);
	} else {
		note(ON_CONFIG, "%s: no offline video", stream->name);
	}
}

static void forget_videos(struct video **videos, unsigned int count) {
	unsigned int i, j, n = atomic_load(&stream_count);
	for (i = 0; i < count; i++) {
		for (j = 0; videos[i] && j < n && streams[j].video != videos[i]; j++) continue;
		if (!videos[i] || j < n) continue;
		free_video(videos[i]);
		for (j = count; j-- > i;) {
			if (videos[j] == videos[i]) {
				videos[j] = NULL;
			}
		}
	}
}

static int same_video(const struct video *kept, const struct video *loaded) {
	return kept && loaded && kept->size == loaded->size && kept->device == loaded->device && kept->inode == loaded->inode && kept->modified.tv_sec == loaded->modified.tv_sec && kept->modified.tv_nsec == loaded->modified.tv_nsec && !strcmp(kept->path, loaded->path);
}

static int apply(struct plan *plan, uint64_t now, int reload) {
	struct video *videos[MAX_STREAMS * 2];
	unsigned int i, j, count = atomic_load(&stream_count), seen = 0;
	struct stream *stream;
	struct entry *entry;
	for (i = 0; i < count; i++) {
		videos[seen++] = streams[i].video;
	}
	for (j = 0; j < plan->count; j++) {
		videos[seen++] = plan->entries[j].video;
		for (i = 0; i < count; i++) {
			if (same_video(streams[i].video, plan->entries[j].video)) {
				plan->entries[j].video = streams[i].video;
			}
		}
	}
	for (i = 0; i < count; i++) {
		stream = &streams[i];
		for (j = 0; j < plan->count && strcmp(plan->entries[j].name, stream->name); j++) continue;
		if (j == plan->count && !atomic_load(&stream->removed)) {
			retire(stream);
		}
		if (j == plan->count) continue;
		if (atomic_load(&stream->removed)) {
			note(ON_CONFIG, "%s: new stream added, ready for a publisher", stream->name);
		}
		update(stream, &plan->entries[j], now, reload);
		atomic_store(&stream->removed, 0);
	}
	for (j = 0; j < plan->count; j++) {
		entry = &plan->entries[j];
		for (i = 0; i < count && strcmp(streams[i].name, entry->name); i++) continue;
		if (i < count) continue;
		for (i = 0; i < count && !atomic_load(&streams[i].removed); i++) continue;
		if (i < count) {
			recycle(&streams[i], entry->name);
		} else if (count == MAX_STREAMS || !open_stream(&streams[count], entry->name)) {
			note(ON_CONFIG | FAIL, "%s: cannot add the stream, out of memory", entry->name);
			if (!reload) return 0;
			continue;
		}
		stream = &streams[i];
		if (reload) {
			note(ON_CONFIG, "%s: new stream added, ready for a publisher", entry->name);
		}
		update(stream, entry, now, reload);
		if (i < count) {
			atomic_store(&stream->removed, 0);
		} else {
			atomic_store_explicit(&stream_count, ++count, memory_order_release);
		}
	}
	forget_videos(videos, seen);
	free(plan);
	return 1;
}

int setup_streams(struct plan *plan) {
	return apply(plan, msec(), 0);
}

void reload_streams(struct plan *plan) {
	free_plan(atomic_exchange(&pending, plan));
	eventfd_write(wake, 1);
}

static struct caller *caller_of(const struct peer *peer) {
	return &callers[peer_hash(peer, caller_seed) & (CALLERS - 1)];
}

static struct caller *find_caller(const struct peer *peer, uint64_t now) {
	struct caller *entry = caller_of(peer);
	if (entry->tries && now - entry->at >= CALLER_WINDOW) {
		entry->tries = 0;
	}
	if (entry->tries && memcmp(&entry->peer, peer, sizeof(*peer))) return NULL;
	entry->peer = *peer;
	return entry;
}

static int afford(uint64_t now) {
	if (now - budget_at >= 1000) {
		budget_at = now;
		budget = 0;
	}
	return budget++ < HANDSHAKES;
}

static void forget_caller(const struct peer *peer) {
	struct caller *entry = caller_of(peer);
	if (!memcmp(&entry->peer, peer, sizeof(*peer))) {
		entry->tries = 0;
	}
}

static struct stream *resolve(const char *id, int *publish, char *name) {
	const char *resource = id, *item, *end;
	size_t i, length = strlen(id);
	*publish = 1;
	if (!strncmp(id, "#!::", 4)) {
		resource = NULL;
		*publish = 0;
		for (item = id + 4; *item; item = *end ? end + 1 : end) {
			end = item + strcspn(item, ",");
			if (!strncmp(item, "r=", 2)) {
				resource = item + 2;
				length = (size_t)(end - resource);
			} else if (!strncmp(item, "m=", 2)) {
				*publish = end - item == 9 && !strncmp(item + 2, "publish", 7);
			}
		}
		if (!resource) return NULL;
	}
	for (i = 0; i < length && i < sizeof(((struct stream *)0)->name) - 1; i++) {
		name[i] = isalnum((unsigned char)resource[i]) || resource[i] == '-' || resource[i] == '_' ? resource[i] : '?';
	}
	name[i] = '\0';
	return find_stream(resource, length, NULL);
}

static void report_refusal(const struct attempt *attempt) {
	static const char *const reasons[] = { [WAITING] = "wrong or missing passphrase", [MISUSED] = "not a publish request", [COOLING] = "blocked for up to 10 s after 5 failed attempts", [SWAMPED] = "more than 20 publisher attempts per second on the server" };
	if (attempt->kind == NAMELESS) {
		tell(UNMATCHED, "connection refused from %s, no streamid", attempt->host);
	} else if (attempt->kind == UNKNOWN) {
		tell(UNMATCHED, "connection refused from %s, unknown stream \"%s\"", attempt->host, attempt->name);
	} else if (attempt->kind == WAITING) {
		note(ON_STREAM | WARN, "%s: connection refused from %s, %s", attempt->name, attempt->host, reasons[attempt->kind]);
	} else {
		tell(BARRED, "%s: connection refused from %s, %s", attempt->name, attempt->host, reasons[attempt->kind]);
	}
}

static int hold(const struct attempt *attempt) {
	int held;
	pthread_mutex_lock(&secrets);
	held = newest - oldest < (attempt->kind == WAITING ? ATTEMPTS : ATTEMPTS - 2 * HANDSHAKES);
	if (held) {
		attempts[newest++ % ATTEMPTS] = *attempt;
	}
	pthread_mutex_unlock(&secrets);
	if (held) {
		eventfd_write(wake, 1);
	}
	return held;
}

static int report_attempts(uint64_t now, int closing) {
	struct attempt due[ATTEMPTS], *attempt;
	unsigned int i, n = 0;
	int young, held;
	pthread_mutex_lock(&secrets);
	for (; oldest != newest; oldest++) {
		attempt = &attempts[oldest % ATTEMPTS];
		young = attempt->kind == WAITING && (int64_t)(now - attempt->at) < REPORT_GAP;
		if (young && !closing) break;
		if (attempt->kind != FREE && !young) {
			due[n++] = *attempt;
		}
	}
	held = oldest != newest;
	pthread_mutex_unlock(&secrets);
	for (i = 0; i < n; i++) {
		report_refusal(&due[i]);
	}
	return held;
}

static int screen(void *opaque, SRTSOCKET sock, int version, const struct sockaddr *address, const char *id) {
	struct attempt attempt = { .sock = sock, .kind = NAMELESS, .at = msec() };
	struct stream *stream = NULL;
	char passphrase[80] = "";
	struct caller *entry;
	struct peer source;
	int publish = 0;
	(void)opaque;
	format_host(address, attempt.host, sizeof(attempt.host));
	if (version >= 5 && id && *id) {
		stream = resolve(id, &publish, attempt.name);
		attempt.kind = !stream ? *attempt.name ? UNKNOWN : NAMELESS : publish ? WAITING : MISUSED;
	}
	if (attempt.kind == WAITING) {
		peer_of(address, &source);
		pthread_mutex_lock(&secrets);
		entry = find_caller(&source, attempt.at);
		attempt.kind = entry && entry->tries >= CALLER_TRIES ? COOLING : !afford(attempt.at) ? SWAMPED : WAITING;
		if (entry && attempt.kind == WAITING) {
			entry->tries++;
			entry->at = attempt.at;
		}
		if (attempt.kind == WAITING) {
			memcpy(passphrase, stream->passphrase, sizeof(passphrase));
		}
		pthread_mutex_unlock(&secrets);
	}
	if (attempt.kind == WAITING && srt_setsockflag(sock, SRTO_PASSPHRASE, passphrase, (int)strlen(passphrase)) == 0) {
		if (hold(&attempt)) return 0;
		attempt.kind = SWAMPED;
	}
	if (!hold(&attempt)) {
		report_refusal(&attempt);
	}
	srt_setrejectreason(sock, attempt.kind == COOLING || attempt.kind == SWAMPED ? SRT_REJX_OVERLOAD : stream ? SRT_REJX_BAD_MODE : SRT_REJX_NOTFOUND);
	return -1;
}

static void admit(uint64_t now) {
	struct sockaddr_storage address;
	int length = sizeof(address), size, secured = 0, keylen = 0, latency = 0, publish, events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
	char id[513], name[64] = "", peer[PEER_SIZE], host[INET6_ADDRSTRLEN];
	struct stream *stream;
	struct peer source;
	unsigned int i;
	SRTSOCKET sock;
	while ((sock = srt_accept(srt_listener, (struct sockaddr *)&address, &length)) != SRT_INVALID_SOCK) {
		format_address((struct sockaddr *)&address, peer, sizeof(peer));
		format_host((struct sockaddr *)&address, host, sizeof(host));
		peer_of((struct sockaddr *)&address, &source);
		length = sizeof(address);
		size = sizeof(id);
		if (srt_getsockflag(sock, SRTO_STREAMID, id, &size) == SRT_ERROR) {
			size = 0;
		}
		id[size] = '\0';
		stream = resolve(id, &publish, name);
		pthread_mutex_lock(&secrets);
		for (i = oldest; i != newest; i++) {
			if (attempts[i % ATTEMPTS].kind == WAITING && attempts[i % ATTEMPTS].sock == sock) {
				attempts[i % ATTEMPTS].kind = FREE;
			}
		}
		if (stream) {
			forget_caller(&source);
		}
		pthread_mutex_unlock(&secrets);
		size = sizeof(secured);
		srt_getsockflag(sock, SRTO_RCVKMSTATE, &secured, &size);
		size = sizeof(keylen);
		srt_getsockflag(sock, SRTO_PBKEYLEN, &keylen, &size);
		size = sizeof(latency);
		srt_getsockflag(sock, SRTO_RCVLATENCY, &latency, &size);
		if (!stream) {
			note(ON_SERVER | WARN, "connection refused from %s, unknown stream \"%s\"", host, name);
		} else if (secured != SRT_KM_S_SECURED) {
			note(ON_STREAM | WARN, "%s: connection refused from %s, not encrypted", stream->name, host);
		} else if (keylen < config.keylen) {
			note(ON_STREAM | WARN, "%s: connection refused from %s, AES-%d while [srt] keylen %d requires AES-%d%s, add pbkeylen=%d to the publisher's URL", stream->name, host, keylen * 8, config.keylen, config.keylen * 8, config.keylen == 32 ? "" : " or stronger", config.keylen);
		} else if (stream->publisher != SRT_INVALID_SOCK && (stream->live.synced || (int64_t)(now - stream->connected) < UNUSABLE_DELAY)) {
			note(ON_STREAM | WARN, "%s: connection refused from %s, publisher %s is %s", stream->name, host, stream->peer, stream->live.synced ? "live" : "starting");
		} else if (srt_epoll_add_usock(srt_poll, sock, &events) == SRT_ERROR) {
			note(ON_STREAM | FAIL, "%s: cannot take publisher %s: %s", stream->name, peer, srt_getlasterror_str());
		} else {
			if (stream->publisher != SRT_INVALID_SOCK) {
				note(ON_STREAM, "%s: publisher %s sent %s within 5 s, replaced by publisher %s", stream->name, stream->peer, stream->live.ready == 3 ? "no keyframe" : "nothing usable", peer);
				disconnect(stream);
			}
			stream->publisher = sock;
			snprintf(stream->peer, sizeof(stream->peer), "%s", peer);
			stream->latency = latency;
			stream->last_data = stream->connected = now + (uint64_t)latency;
			atomic_store_explicit(&stream->rtp.fed, stream->connected, memory_order_relaxed);
			stream->warned = stream->crowded = 0;
			note(ON_STREAM, "%s: publisher %s connected with AES-%d, %d ms latency", stream->name, peer, keylen * 8, latency);
			continue;
		}
		srt_close(sock);
	}
}

int open_srt(void) {
	int no = 0, yes = 1, window = (config.latency + 1000) * 30000 / 8 / 1316, bytes = window * 1472, events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
	if (getrandom(&caller_seed, sizeof(caller_seed), 0) != (ssize_t)sizeof(caller_seed) || (wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0) {
		note(ON_SERVER | FAIL, "SRT: %s", strerror(errno));
		return 0;
	}
	srt_listener = srt_create_socket();
	srt_poll = srt_epoll_create();
	if (srt_listener == SRT_INVALID_SOCK || srt_poll < 0) {
		note(ON_SERVER | FAIL, "SRT: %s", srt_getlasterror_str());
		return 0;
	}
	srt_setsockflag(srt_listener, SRTO_RCVSYN, &no, sizeof(no));
	srt_setsockflag(srt_listener, SRTO_RCVLATENCY, &config.latency, sizeof(config.latency));
	if (window > 8192) {
		srt_setsockflag(srt_listener, SRTO_FC, &window, sizeof(window));
		srt_setsockflag(srt_listener, SRTO_RCVBUF, &bytes, sizeof(bytes));
	}
	srt_setsockflag(srt_listener, SRTO_PEERIDLETIMEO, &config.timeout, sizeof(config.timeout));
	srt_setsockflag(srt_listener, SRTO_ENFORCEDENCRYPTION, &yes, sizeof(yes));
	if (config.srt.address.ss_family == AF_INET6) {
		srt_setsockflag(srt_listener, SRTO_IPV6ONLY, &no, sizeof(no));
	}
	if (srt_listen_callback(srt_listener, screen, NULL) == SRT_ERROR || srt_bind(srt_listener, (struct sockaddr *)&config.srt.address, (int)config.srt.length) == SRT_ERROR || srt_listen(srt_listener, 16) == SRT_ERROR || srt_epoll_add_usock(srt_poll, srt_listener, &events) == SRT_ERROR) {
		note(ON_SERVER | FAIL, "[srt] listen %s: %s", config.srt.text, srt_getlasterror_str());
		return 0;
	}
	return 1;
}

static void summarize(void) {
	unsigned int i, count = atomic_load(&stream_count);
	for (i = 0; i < count; i++) {
		tally(&streams[i]);
	}
	report_refusals();
	settle();
}

void *ingest(void *argument) {
	struct pollfd rest = { .fd = wake, .events = POLLIN };
	SRT_EPOLL_EVENT events[MAX_STREAMS + 1];
	uint64_t now, reported = msec();
	unsigned int i, count;
	struct plan *plan;
	eventfd_t signals;
	int n, k, busy = 1;
	(void)argument;
	while (atomic_load(&running)) {
		if (!busy && poll(&rest, 1, REST_INTERVAL) > 0) {
			eventfd_read(wake, &signals);
		}
		n = srt_epoll_uwait(srt_poll, events, MAX_STREAMS + 1, busy ? INGEST_INTERVAL : 0);
		now = msec();
		atomic_store(&ingest_beat, now);
		plan = atomic_exchange(&pending, NULL);
		if (plan) {
			apply(plan, now, 1);
			note(ON_CONFIG, "configuration reloaded");
		}
		count = atomic_load(&stream_count);
		for (k = 0; k < n; k++) {
			if (events[k].fd == srt_listener) {
				admit(now);
				continue;
			}
			for (i = 0; i < count; i++) {
				if (streams[i].publisher != events[k].fd) continue;
				if (events[k].events & SRT_EPOLL_IN) {
					receive(&streams[i], now);
				}
				if (streams[i].publisher == events[k].fd && (events[k].events & SRT_EPOLL_ERR)) {
					lose(&streams[i], now);
				}
			}
		}
		for (i = 0, busy = report_attempts(now, 0); i < count; i++) {
			tick(&streams[i], now);
			busy |= atomic_load(&streams[i].state) != IDLE || streams[i].publisher != SRT_INVALID_SOCK;
		}
		if (now - reported < REPORT_INTERVAL) continue;
		reported = now;
		summarize();
	}
	return NULL;
}

void close_srt(void) {
	unsigned int i, count = atomic_load(&stream_count);
	for (i = 0; i < count; i++) {
		if (streams[i].publisher != SRT_INVALID_SOCK) {
			srt_close(streams[i].publisher);
		}
	}
	srt_close(srt_listener);
	srt_epoll_release(srt_poll);
	srt_cleanup();
	report_attempts(msec(), 1);
	summarize();
}
