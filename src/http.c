#define _GNU_SOURCE

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "log.h"
#include "main.h"

#define INPUT_SIZE 2048
#define OUTPUT_SIZE (8 * PACKET)
#define STALL 3600000
#define BEFORE 180

static atomic_int unnamed;

struct http_viewer {
	struct viewer base;
	int head_only;
	int align;
	int cc[2];
	uint64_t first;
	char in[INPUT_SIZE];
	char out[OUTPUT_SIZE];
};

struct viewer *http_new(void) {
	struct http_viewer *http = calloc(1, sizeof(*http));
	if (!http) return NULL;
	http->base.in = http->in;
	http->base.out = http->out;
	http->base.in_room = sizeof(http->in);
	http->base.out_room = sizeof(http->out);
	return &http->base;
}

static void refuse(struct viewer *viewer, const char *status) {
	char date[64];
	format_date(date, sizeof(date));
	queue(viewer, "HTTP/1.1 %s\r\nConnection: close\r\nContent-Length: 0\r\nDate: %s\r\nServer: crossfade %s\r\n\r\n", status, date, VERSION);
	viewer->closing = 1;
}

static int forwarded(const char *headers, struct peer *peer) {
	struct sockaddr_storage address = { 0 };
	struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&address;
	struct sockaddr_in *in4 = (struct sockaddr_in *)&address;
	const char *value = NULL, *line, *start, *end;
	char entry[INET6_ADDRSTRLEN + 8], *text = entry, *colon;
	size_t length;
	for (line = field(headers, "X-Forwarded-For"); line; line = field(line, "X-Forwarded-For")) {
		value = line;
	}
	if (!value) return 0;
	end = value + strcspn(value, "\r\n");
	for (start = end; start > value && start[-1] != ','; start--) continue;
	while (start < end && (*start == ' ' || *start == '\t')) {
		start++;
	}
	while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
		end--;
	}
	length = (size_t)(end - start);
	if (!length || length >= sizeof(entry)) return 0;
	memcpy(entry, start, length);
	entry[length] = '\0';
	if (*text == '[') {
		text++;
		text[strcspn(text, "]")] = '\0';
	} else if ((colon = strchr(text, ':')) && colon == strrchr(text, ':')) {
		*colon = '\0';
	}
	if (inet_pton(AF_INET, text, &in4->sin_addr) == 1) {
		in4->sin_family = AF_INET;
	} else if (inet_pton(AF_INET6, text, &in6->sin6_addr) == 1) {
		in6->sin6_family = AF_INET6;
	} else {
		return 0;
	}
	peer_of((const struct sockaddr *)&address, peer);
	return 1;
}

static void health(struct viewer *viewer) {
	char date[64];
	format_date(date, sizeof(date));
	queue(viewer, "HTTP/1.1 %s\r\nConnection: close\r\nContent-Length: 0\r\nDate: %s\r\nServer: crossfade %s\r\n\r\n", healthy() ? "200 OK" : "503 Service Unavailable", date, VERSION);
	viewer->closing = 1;
}

static int begin(struct http_viewer *http) {
	struct viewer *viewer = &http->base;
	int backlog = config.backlog ? config.backlog : LIVE_EDGE;
	struct mark mark;
	char date[64];
	if (!choose(viewer, &viewer->stream->ts, backlog, &mark, &viewer->epoch)) return 0;
	format_date(date, sizeof(date));
	queue(viewer, "HTTP/1.1 200 OK\r\nAccept-Ranges: none\r\nCache-Control: no-cache, no-store\r\nConnection: close\r\nContent-Type: video/mp2t\r\nDate: %s\r\nPragma: no-cache\r\nServer: crossfade %s\r\n\r\n", date, VERSION);
	if (http->head_only) {
		viewer->closing = 1;
		return 1;
	}
	if (viewer->out_room - viewer->out_size >= PACKET) {
		copy_sdt(viewer->stream, (unsigned char *)viewer->out + viewer->out_size);
		viewer->out_size += PACKET;
	}
	http->align = backlog == LIVE_EDGE;
	http->first = mark.at;
	http->cc[0] = http->cc[1] = -1;
	start(viewer, &viewer->stream->ts, &mark);
	return 1;
}

static int sent(const struct http_viewer *http, int pid, int fallback) {
	const struct viewer *viewer = &http->base;
	unsigned char header[4];
	uint64_t at = viewer->pos;
	while (at >= http->first + PACKET && atomic_load_explicit(&viewer->ring->head, memory_order_acquire) - at < viewer->ring->size / 2) {
		at -= PACKET;
		ring_peek(viewer->ring, at, header, sizeof(header));
		if (pid_of(header) == pid && (header[3] & 0x10)) return header[3] & 15;
	}
	return fallback;
}

static unsigned char *blank(struct viewer *viewer, int pid, int control, int cc) {
	unsigned char *p = (unsigned char *)viewer->out + viewer->out_size;
	memset(p, 0xff, PACKET);
	p[0] = 0x47;
	p[1] = (unsigned char)(pid >> 8);
	p[2] = (unsigned char)pid;
	p[3] = (unsigned char)(control | (cc & 15));
	viewer->out_size += PACKET;
	return p;
}

static void bridge(struct http_viewer *http, const struct mark *mark) {
	static const int pids[] = { VIDEO_PID, AUDIO_PID };
	static const unsigned char empty[] = { 0, 0, 1, 0, 0, 3, 0x80, 0, 0 };
	struct viewer *viewer = &http->base;
	unsigned char *p;
	int i, k, last, declared, cc, count;
	for (i = 0; i < 2; i++) {
		last = sent(http, pids[i], http->cc[i]);
		declared = !i && mark->declared;
		cc = declared ? mark->cc[i] + 9 : mark->cc[i];
		count = declared ? last >= 0 && ((mark->cc[i] - last) & 15) <= 1 : last >= 0 && ((cc - 1 - last) & 15) <= 1 ? 3 : 1;
		for (k = count; k; k--) {
			p = blank(viewer, pids[i], 0x30, cc - k);
			p[1] |= 0x40;
			p[4] = PACKET - 5 - sizeof(empty);
			p[5] = k == count ? 0x80 : 0;
			if (!i && k == count && !declared && mark->clocked) {
				p[5] |= 0x10;
				p[10] = 0x7e;
				p[11] = 0;
				set_pcr(p, (mark->pcr - BEFORE) & (WRAP - 1));
			}
			memcpy(p + PACKET - sizeof(empty), empty, sizeof(empty));
			p[PACKET - sizeof(empty) + 3] = (unsigned char)(i ? 0xc0 : 0xe0);
		}
		http->cc[i] = count ? (cc - 1) & 15 : last;
	}
	p = blank(viewer, 0, 0x20, mark->cc[2] - 1);
	p[4] = PACKET - 5;
	p[5] = 0x80;
	p = blank(viewer, PMT_PID, 0x20, mark->cc[3] - 1);
	p[4] = PACKET - 5;
	p[5] = 0x80;
	http->first = mark->at;
}

int http_input(struct viewer *viewer, uint64_t now) {
	struct http_viewer *http = (struct http_viewer *)viewer;
	char *end, *line, *target, *version, text[INET6_ADDRSTRLEN];
	struct sockaddr_storage address = { .ss_family = AF_UNSPEC };
	socklen_t size = sizeof(address);
	struct peer peer;
	size_t length;
	int named;
	if (viewer->stage != REQUEST || viewer->closing) {
		viewer->used = 0;
		return 0;
	}
	end = memmem(http->in, viewer->used, "\r\n\r\n", 4);
	if (!end) {
		if (viewer->used == sizeof(http->in)) {
			refuse(viewer, "431 Request Header Fields Too Large");
		}
		return 0;
	}
	viewer->used = 0;
	viewer->asked = now;
	if (memchr(http->in, '\0', (size_t)(end - http->in))) {
		refuse(viewer, "400 Bad Request");
		return 0;
	}
	end[2] = '\0';
	line = strstr(http->in, "\r\n");
	named = forwarded(line, &peer);
	*line = '\0';
	target = strchr(http->in, ' ');
	version = target ? strchr(target + 1, ' ') : NULL;
	if (!version || (strcmp(version + 1, "HTTP/1.1") && strcmp(version + 1, "HTTP/1.0"))) {
		refuse(viewer, "400 Bad Request");
		return 0;
	}
	*target++ = '\0';
	*version = '\0';
	http->head_only = !strcmp(http->in, "HEAD");
	if (!http->head_only && strcmp(http->in, "GET")) {
		refuse(viewer, "405 Method Not Allowed");
		return 0;
	}
	target[strcspn(target, "?")] = '\0';
	if (!strcmp(target, "/health")) {
		health(viewer);
		return 0;
	}
	if (viewer->spare) {
		refuse(viewer, "503 Service Unavailable");
		return 0;
	}
	if (named) {
		viewer->peer = peer;
		if (!admit_source(viewer)) {
			refuse(viewer, "429 Too Many Requests");
			return 0;
		}
	} else if (config.clients && !atomic_exchange(&unnamed, 1)) {
		getpeername(viewer->fd, (struct sockaddr *)&address, &size);
		format_host((struct sockaddr *)&address, text, sizeof(text));
		note(ON_VIEWER | WARN, "a .ts request from %s came without X-Forwarded-For, the [general] clients limit cannot count such requests, see HTTPS in the README", text);
	}
	length = strlen(target);
	viewer->stream = length > 4 && target[0] == '/' && !strcmp(target + length - 3, ".ts") ? find_stream(target + 1, length - 4, &viewer->version) : NULL;
	if (!viewer->stream) {
		refuse(viewer, "404 Not Found");
	} else if (!begin(http) && !config.wait) {
		refuse(viewer, "503 Service Unavailable");
	} else if (viewer->stage == REQUEST && !viewer->closing) {
		viewer->stage = WAIT;
	}
	return 0;
}

void http_retry(struct viewer *viewer, int expired) {
	if (!begin((struct http_viewer *)viewer) && expired) {
		refuse(viewer, "503 Service Unavailable");
	}
}

int http_flush(struct viewer *viewer, uint64_t now) {
	struct http_viewer *http = (struct http_viewer *)viewer;
	struct iovec parts[2];
	struct ring *ring;
	uint64_t head, end;
	unsigned char header[4];
	unsigned int epoch;
	ssize_t n;
	while (viewer->writable) {
		if (viewer->out_size) {
			if (drain(viewer, now) < 0) return CLOSE;
			continue;
		}
		if (viewer->closing) return CLOSE;
		if (viewer->stage != STREAM) return KEEP;
		ring = viewer->ring;
		head = atomic_load_explicit(&ring->head, memory_order_acquire);
		if (atomic_load(&ring->epoch) != viewer->epoch) return ABORT;
		while (http->align && viewer->pos < head) {
			ring_peek(ring, viewer->pos, header, sizeof(header));
			if ((header[1] & 0x5f) == 0x40 && !header[2]) {
				http->align = 0;
				http->first = viewer->pos;
			} else {
				viewer->pos += PACKET;
			}
		}
		if (http->align) return KEEP;
		if (head - viewer->pos > ring->size / 2) {
			if (viewer->pos % PACKET || !choose(viewer, ring, 0, &viewer->target, &epoch) || epoch != viewer->epoch) return ABORT;
			viewer->jump = 1;
			atomic_fetch_add(&viewer->stream->lagging, 1);
		}
		catch_up(viewer, head);
		if (viewer->jump && !(viewer->pos % PACKET)) {
			bridge(http, &viewer->target);
			start(viewer, ring, &viewer->target);
			continue;
		}
		end = viewer->jump ? viewer->pos + PACKET - viewer->pos % PACKET : head;
		if (viewer->pos >= end) return KEEP;
		n = transmit(viewer, parts, ring_slice(ring, viewer->pos, end - viewer->pos, parts), now);
		if (n < 0) return CLOSE;
		if (overrun(viewer)) return ABORT;
		viewer->pos += (uint64_t)n;
		if (!viewer->writable && viewer->pos % PACKET) {
			viewer->out_size = PACKET - viewer->pos % PACKET;
			ring_peek(ring, viewer->pos, (unsigned char *)viewer->out, viewer->out_size);
			if (overrun(viewer)) return ABORT;
			viewer->pos += viewer->out_size;
		}
		if (!viewer->writable) {
			http->cc[0] = sent(http, VIDEO_PID, http->cc[0]);
			http->cc[1] = sent(http, AUDIO_PID, http->cc[1]);
			http->first = viewer->pos;
		}
	}
	return KEEP;
}

int http_idle(struct viewer *viewer, uint64_t now) {
	if (atomic_load(&viewer->ring->epoch) != viewer->epoch) return 1;
	return !viewer->writable && now - viewer->moved >= STALL;
}
