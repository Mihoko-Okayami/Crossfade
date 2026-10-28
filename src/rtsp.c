#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#include "main.h"
#include "rtsp.h"

#define INPUT_SIZE 4096
#define OUTPUT_SIZE 4096
#define URI_SIZE 256
#define STALL 15000
#define OUTAGE 6000
#define STEP 3000
#define BATCH 64
#define TIMEOUT 60
#define RTCP_INTERVAL 5000

struct rtsp_viewer {
	struct viewer base;
	int paused;
	int pending;
	int inside;
	int filter;
	int tagged;
	int rate;
	int channel[2];
	uint16_t seq[2];
	uint32_t origin[2];
	uint32_t sent[2];
	uint32_t octets[2];
	uint32_t last[2];
	uint32_t step[2];
	uint64_t position[2];
	uint64_t anchor[2];
	uint64_t anchor_ntp;
	int anchored;
	uint32_t from;
	uint64_t clock;
	uint64_t request;
	uint64_t reported;
	size_t partial;
	size_t skip;
	char session[13];
	char cseq[16];
	char root[URI_SIZE];
	char in[INPUT_SIZE];
	char out[OUTPUT_SIZE];
};

struct item {
	uint64_t start;
	uint64_t end;
	size_t size;
	uint32_t time;
	int track;
	int marker;
};

struct viewer *rtsp_new(void) {
	struct rtsp_viewer *rtsp = calloc(1, sizeof(*rtsp));
	if (!rtsp) return NULL;
	rtsp->base.in = rtsp->in;
	rtsp->base.out = rtsp->out;
	rtsp->base.in_room = sizeof(rtsp->in);
	rtsp->base.out_room = sizeof(rtsp->out);
	rtsp->channel[0] = rtsp->channel[1] = -1;
	return &rtsp->base;
}

static void restart(struct rtsp_viewer *rtsp, const struct mark *mark) {
	start(&rtsp->base, &rtsp->base.stream->rtp, mark);
	rtsp->inside = 0;
	rtsp->filter = 1;
	rtsp->from = mark->ts[1];
	rtsp->origin[0] = mark->ts[0] - (uint32_t)rtsp->clock;
	rtsp->origin[1] = mark->ts[1] - (uint32_t)(rtsp->clock * (uint64_t)rtsp->rate / 90000);
}

static int before(const char *a, const char *b) {
	for (; *a != ':' && *b != ':' && tolower((unsigned char)*a) == tolower((unsigned char)*b); a++, b++) continue;
	if (*a == ':' || *b == ':') return *a == ':' && *b != ':';
	return tolower((unsigned char)*a) < tolower((unsigned char)*b);
}

static void reply(struct rtsp_viewer *rtsp, const char *status, const char *headers) {
	char text[OUTPUT_SIZE / 2], date[64], *lines[16], *line, *end, *swap;
	size_t count = 0, i, j;
	int tagged = rtsp->tagged, n;
	format_date(date, sizeof(date));
	n = snprintf(text, sizeof(text), "CSeq: %s\r\nDate: %s\r\nServer: crossfade %s\r\n%s%s%s%s", rtsp->cseq, date, VERSION, tagged ? "Session: " : "", tagged ? rtsp->session : "", tagged ? "\r\n" : "", headers);
	if (n < 0 || (size_t)n >= sizeof(text)) return;
	for (line = text; count < sizeof(lines) / sizeof(lines[0]) && (end = strstr(line, "\r\n")); line = end + 2) {
		*end = '\0';
		lines[count++] = line;
	}
	for (i = 1; i < count; i++) {
		for (j = i; j && before(lines[j], lines[j - 1]); j--) {
			swap = lines[j];
			lines[j] = lines[j - 1];
			lines[j - 1] = swap;
		}
	}
	queue(&rtsp->base, "RTSP/1.0 %s\r\n", status);
	for (i = 0; i < count; i++) {
		queue(&rtsp->base, "%s\r\n", lines[i]);
	}
	queue(&rtsp->base, "\r\n");
}

static void rtcp(struct rtsp_viewer *rtsp, uint64_t now) {
	static const char name[] = "crossfade " VERSION;
	struct viewer *viewer = &rtsp->base;
	size_t length = sizeof(name) - 1 < 63 ? sizeof(name) - 1 : 63, chunk = (4 + 2 + length + 1 + 3) & ~(size_t)3, total = 28 + 4 + chunk;
	uint64_t next[2], units, elapsed, ntp;
	struct timespec real;
	unsigned char *p;
	int track;
	rtsp->reported = now;
	next[0] = rtsp->sent[0] ? rtsp->position[0] + rtsp->step[0] : rtsp->clock;
	next[1] = rtsp->sent[1] ? rtsp->position[1] + rtsp->step[1] : rtsp->clock * (uint64_t)rtsp->rate / 90000;
	if (!rtsp->anchored) {
		clock_gettime(CLOCK_REALTIME, &real);
		rtsp->anchor_ntp = (uint64_t)((uint32_t)real.tv_sec + 2208988800u) << 32 | (uint64_t)real.tv_nsec * 4294967296u / 1000000000u;
		rtsp->anchor[0] = next[0];
		rtsp->anchor[1] = next[1];
		rtsp->anchored = 1;
	}
	for (track = 0; track < 2; track++) {
		units = track ? (uint64_t)rtsp->rate : 90000;
		elapsed = next[track] > rtsp->anchor[track] ? next[track] - rtsp->anchor[track] : 0;
		ntp = rtsp->anchor_ntp + (elapsed / units << 32) + ((elapsed % units) << 32) / units;
		if (rtsp->channel[track] < 0 || viewer->out_room - viewer->out_size < 4 + total) continue;
		p = (unsigned char *)viewer->out + viewer->out_size;
		p[0] = '$';
		p[1] = (unsigned char)(rtsp->channel[track] + 1);
		put16(p + 2, (unsigned int)total);
		p += 4;
		p[0] = 0x80;
		p[1] = 200;
		put16(p + 2, 6);
		put32(p + 4, viewer->stream->ssrc[track]);
		put32(p + 8, (uint32_t)(ntp >> 32));
		put32(p + 12, (uint32_t)ntp);
		put32(p + 16, (uint32_t)next[track]);
		put32(p + 20, rtsp->sent[track]);
		put32(p + 24, rtsp->octets[track]);
		p += 28;
		memset(p, 0, 4 + chunk);
		p[0] = 0x81;
		p[1] = 202;
		put16(p + 2, (unsigned int)((4 + chunk) / 4 - 1));
		put32(p + 4, viewer->stream->ssrc[track]);
		p[8] = 1;
		p[9] = (unsigned char)length;
		memcpy(p + 10, name, length);
		viewer->out_size += 4 + total;
	}
}

static void fail(struct rtsp_viewer *rtsp, const char *status) {
	reply(rtsp, status, "");
	rtsp->base.closing = 1;
}

static int clean(const char *uri) {
	for (; *uri; uri++) {
		if ((unsigned char)*uri <= ' ' || *uri == 127 || *uri == ',' || *uri == ';') return 0;
	}
	return 1;
}

static struct stream *locate(const char *uri, int *track, unsigned int *version) {
	const char *path = strstr(uri, "://"), *id;
	*track = -1;
	path = path ? strchr(path + 3, '/') : *uri == '/' ? uri : NULL;
	if (!path) return NULL;
	id = strstr(path, "trackID=");
	if (id && (id[8] == '0' || id[8] == '1') && (id[9] < '0' || id[9] > '9')) {
		*track = id[8] - '0';
	}
	return find_stream(path + 1, strcspn(path + 1, "/?#"), version);
}

static size_t snapshot(struct rtsp_viewer *rtsp, struct stream *stream, unsigned int version, char *sdp) {
	unsigned int epoch = 0;
	size_t size = 0;
	int rate = 0;
	pthread_mutex_lock(&stream->rtp.lock);
	if (stream->sdp_size && stream->rtp.mark_count) {
		size = stream->sdp_size;
		if (sdp) {
			memcpy(sdp, stream->sdp, size + 1);
		}
		epoch = atomic_load(&stream->rtp.epoch);
		rate = stream->rate;
	}
	pthread_mutex_unlock(&stream->rtp.lock);
	if (!size || atomic_load(&stream->version) != version || !on_air(stream)) return 0;
	rtsp->base.epoch = epoch;
	rtsp->rate = rate;
	return size;
}

static size_t bandwidth(struct stream *stream, char *sdp, size_t size, size_t room) {
	static const char *const media[] = { "\r\nm=video ", "\r\nm=audio " };
	uint64_t total = atomic_load_explicit(&stream->rtp.rate, memory_order_relaxed) * 8 / 1000;
	unsigned int audio = atomic_load(&stream->audio_kbps), kbps[2];
	char line[32], *at;
	int i, n;
	if (!total || !audio || audio >= total) return size;
	kbps[0] = (unsigned int)(total - audio);
	kbps[1] = audio;
	for (i = 0; i < 2; i++) {
		at = strstr(sdp, media[i]);
		at = at ? strstr(at + 2, "\r\n") : NULL;
		if (!at) continue;
		at += 2;
		n = snprintf(line, sizeof(line), "b=AS:%u\r\n", kbps[i]);
		if (n <= 0 || size + (size_t)n >= room) return size;
		memmove(at + n, at, size - (size_t)(at - sdp) + 1);
		memcpy(at, line, (size_t)n);
		size += (size_t)n;
	}
	return size;
}

static int describe(struct rtsp_viewer *rtsp) {
	char sdp[sizeof(rtsp->base.stream->sdp)], headers[URI_SIZE + 256];
	size_t size = snapshot(rtsp, rtsp->base.stream, rtsp->base.version, sdp);
	if (!size) return 0;
	size = bandwidth(rtsp->base.stream, sdp, size, sizeof(sdp));
	snprintf(headers, sizeof(headers), "Content-Base: %s/\r\nContent-Type: application/sdp\r\nContent-Length: %zu\r\nx-Accept-Dynamic-Rate: 1\r\nx-Accept-Retransmit: our-retransmit\r\n", rtsp->root, size);
	reply(rtsp, "200 OK", headers);
	queue(&rtsp->base, "%s", sdp);
	return 1;
}

static int identify(struct rtsp_viewer *rtsp) {
	static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	unsigned char random[sizeof(rtsp->session) - 1];
	size_t i;
	if (*rtsp->session) return 1;
	if (getrandom(random, sizeof(random), 0) != (ssize_t)sizeof(random)) return 0;
	for (i = 0; i < sizeof(random); i++) {
		rtsp->session[i] = digits[random[i] % (sizeof(digits) - 1)];
	}
	rtsp->session[i] = '\0';
	rtsp->tagged = 1;
	return 1;
}

static void lookup(struct rtsp_viewer *rtsp, const char *uri) {
	struct stream *stream;
	unsigned int version = 0;
	size_t length;
	int track;
	stream = locate(uri, &track, &version);
	if (!stream || (rtsp->base.stream && stream != rtsp->base.stream)) {
		reply(rtsp, "404 Not Found", "");
		return;
	}
	if (!identify(rtsp)) {
		fail(rtsp, "500 Internal Server Error");
		return;
	}
	rtsp->base.stream = stream;
	rtsp->base.version = version;
	length = strcspn(uri, "?#");
	while (length && uri[length - 1] == '/') {
		length--;
	}
	memcpy(rtsp->root, uri, length);
	rtsp->root[length] = '\0';
	if (describe(rtsp)) return;
	if (!config.wait) {
		fail(rtsp, "503 Service Unavailable");
	} else {
		rtsp->base.stage = WAIT;
	}
}

static void setup(struct rtsp_viewer *rtsp, const char *uri, const char *transport) {
	char headers[384], *end;
	const char *spec, *at;
	struct stream *stream;
	unsigned int version = 0;
	int track, channel;
	size_t length = 0;
	stream = locate(uri, &track, &version);
	if (!stream || track < 0 || (rtsp->base.stream && stream != rtsp->base.stream)) {
		reply(rtsp, "404 Not Found", "");
		return;
	}
	if (!*transport) {
		reply(rtsp, "400 Bad Request", "");
		return;
	}
	if (!rtsp->rate && !snapshot(rtsp, stream, version, NULL)) {
		fail(rtsp, "503 Service Unavailable");
		return;
	}
	for (spec = transport; *spec; spec += length + (spec[length] == ',')) {
		length = strcspn(spec, ",");
		if (memmem(spec, length, "/TCP", 4)) break;
	}
	if (!*spec) {
		reply(rtsp, "461 Unsupported Transport", "");
		return;
	}
	at = memmem(spec, length, "interleaved=", 12);
	channel = at ? (int)strtol(at + 12, &end, 10) : track * 2;
	if (channel < 0 || channel > 254 || channel % 2 || (at && end == at + 12) || channel == rtsp->channel[!track]) {
		reply(rtsp, "400 Bad Request", "");
		return;
	}
	snprintf(headers, sizeof(headers), "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d;ssrc=%08X\r\nx-Dynamic-Rate: 1\r\nx-Transport-Options: late-tolerance=1.400000\r\n", channel, channel + 1, stream->ssrc[track]);
	if (!identify(rtsp)) {
		fail(rtsp, "500 Internal Server Error");
		return;
	}
	rtsp->base.stream = stream;
	rtsp->base.version = version;
	rtsp->channel[track] = channel;
	if (!*rtsp->root) {
		for (length = (size_t)(strstr(uri, "trackID=") - uri); length && uri[length - 1] == '/'; length--) continue;
		memcpy(rtsp->root, uri, length);
		rtsp->root[length] = '\0';
	}
	reply(rtsp, "200 OK", headers);
}

static void edge(const struct ring *ring, struct mark *mark, int rate) {
	uint64_t head = atomic_load_explicit(&ring->head, memory_order_acquire), at;
	uint32_t base = mark->ts[0], ts = 0, previous = 0;
	unsigned char record[RECORD];
	size_t length;
	int found = 0;
	for (at = mark->at; head - at >= RECORD; at += RECORD + length) {
		ring_peek(ring, at, record, RECORD);
		length = (size_t)record[0] << 8 | record[1];
		if (head - at < RECORD + length) break;
		if (record[2] & 1 || !(record[3] & UNIT_END)) continue;
		previous = ts;
		ts = get32(record + 4);
		mark->at = at + RECORD + length;
		found++;
	}
	if (!found) return;
	mark->ts[0] = ts + (found > 1 ? ts - previous : 0);
	mark->ts[1] += (uint32_t)((int64_t)(int32_t)(mark->ts[0] - base) * rate / 90000);
}

static void play(struct rtsp_viewer *rtsp, uint64_t now) {
	struct viewer *viewer = &rtsp->base;
	struct mark mark;
	unsigned int epoch;
	char headers[2 * URI_SIZE + 192];
	int track, n;
	if (rtsp->channel[0] < 0 && rtsp->channel[1] < 0) {
		reply(rtsp, "455 Method Not Valid in This State", "");
		return;
	}
	if (!choose(viewer, &viewer->stream->rtp, viewer->stage == STREAM ? 0 : config.rtsp_backlog, &mark, &epoch) || epoch != viewer->epoch) {
		fail(rtsp, "503 Service Unavailable");
		return;
	}
	if (viewer->stage != STREAM && !config.rtsp_backlog) {
		edge(&viewer->stream->rtp, &mark, rtsp->rate);
	}
	if (viewer->stage != STREAM || mark.at > viewer->pos) {
		restart(rtsp, &mark);
	}
	rtsp->paused = 0;
	n = snprintf(headers, sizeof(headers), "Range: npt=%llu.%03llu-\r\nRTP-Info: ", (unsigned long long)(rtsp->clock / 90000), (unsigned long long)(rtsp->clock % 90000 / 90));
	for (track = 0; track < 2; track++) {
		if (rtsp->channel[track] < 0) continue;
		n += snprintf(headers + n, sizeof(headers) - (size_t)n, "%surl=%s/trackID=%d;seq=%u;rtptime=%u", headers[n - 1] == ' ' ? "" : ",", rtsp->root, track, (unsigned int)rtsp->seq[track], track ? (uint32_t)(rtsp->clock * (uint64_t)rtsp->rate / 90000) : (uint32_t)rtsp->clock);
	}
	snprintf(headers + n, sizeof(headers) - (size_t)n, "\r\n");
	reply(rtsp, "200 OK", headers);
	rtcp(rtsp, now);
}

static void answer(struct rtsp_viewer *rtsp, char *method, uint64_t now) {
	char *line = strstr(method, "\r\n"), *uri, *version, transport[256], session[64], date[64];
	const char *given = field(line, "Session");
	size_t length;
	rtsp->request = rtsp->base.asked = now;
	copy_field(rtsp->cseq, sizeof(rtsp->cseq), field(line, "CSeq"));
	rtsp->cseq[strspn(rtsp->cseq, "0123456789")] = '\0';
	copy_field(transport, sizeof(transport), field(line, "Transport"));
	copy_field(session, sizeof(session), given);
	*line = '\0';
	uri = strchr(method, ' ');
	version = uri ? strchr(uri + 1, ' ') : NULL;
	if (version && !strncmp(version + 1, "HTTP/1.", 7)) {
		format_date(date, sizeof(date));
		queue(&rtsp->base, "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\nDate: %s\r\nServer: crossfade %s\r\n\r\n", date, VERSION);
		rtsp->base.closing = 1;
		return;
	}
	if (!version || strcmp(version + 1, "RTSP/1.0") || version - uri > URI_SIZE) {
		fail(rtsp, "400 Bad Request");
		return;
	}
	*uri++ = '\0';
	*version = '\0';
	length = strcspn(session, "; \t");
	if (!clean(uri)) {
		fail(rtsp, "400 Bad Request");
	} else if (rtsp->base.spare && (strcmp(method, "OPTIONS") || strcmp(uri, "*"))) {
		fail(rtsp, "503 Service Unavailable");
	} else if (given && (length != strlen(rtsp->session) || strncmp(session, rtsp->session, length))) {
		rtsp->tagged = 0;
		reply(rtsp, "454 Session Not Found", "");
		rtsp->tagged = *rtsp->session != '\0';
	} else if (!strcmp(method, "OPTIONS") && !strcmp(uri, "*") && !healthy()) {
		reply(rtsp, "503 Service Unavailable", "");
	} else if (!strcmp(method, "OPTIONS")) {
		reply(rtsp, "200 OK", "Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, ANNOUNCE, RECORD, SET_PARAMETER, GET_PARAMETER\r\n");
	} else if (!strcmp(method, "DESCRIBE") && rtsp->base.stage == STREAM) {
		reply(rtsp, "455 Method Not Valid in This State", "");
	} else if (!strcmp(method, "DESCRIBE")) {
		lookup(rtsp, uri);
	} else if (!strcmp(method, "SETUP")) {
		setup(rtsp, uri, transport);
	} else if (!strcmp(method, "PLAY")) {
		play(rtsp, now);
	} else if (!strcmp(method, "PAUSE") && rtsp->channel[0] < 0 && rtsp->channel[1] < 0) {
		reply(rtsp, "455 Method Not Valid in This State", "");
	} else if (!strcmp(method, "PAUSE")) {
		rtsp->paused = rtsp->base.stage == STREAM;
		reply(rtsp, "200 OK", "");
	} else if (!strcmp(method, "TEARDOWN")) {
		fail(rtsp, "200 OK");
	} else if (!strcmp(method, "GET_PARAMETER") || !strcmp(method, "SET_PARAMETER")) {
		reply(rtsp, "200 OK", "");
	} else {
		reply(rtsp, "501 Not Implemented", "");
	}
}

int rtsp_input(struct viewer *viewer, uint64_t now) {
	struct rtsp_viewer *rtsp = (struct rtsp_viewer *)viewer;
	size_t done = 0, left, size, body;
	const char *length;
	char *in, *end;
	while (done < viewer->used && !viewer->closing) {
		in = rtsp->in + done;
		left = viewer->used - done;
		if (rtsp->skip) {
			size = rtsp->skip < left ? rtsp->skip : left;
			rtsp->skip -= size;
		} else if (in[0] == '$') {
			if (left < 4) break;
			rtsp->skip = 4 + ((size_t)(unsigned char)in[2] << 8 | (unsigned char)in[3]);
			rtsp->request = now;
			continue;
		} else if (in[0] == '\r' || in[0] == '\n') {
			size = 1;
		} else if (viewer->stage == WAIT) {
			break;
		} else if (viewer->out_size || rtsp->partial || rtsp->inside) {
			rtsp->pending = 1;
			break;
		} else {
			end = memmem(in, left, "\r\n\r\n", 4);
			if (!end) break;
			size = (size_t)(end - in) + 4;
			if (memchr(in, '\0', size)) return -1;
			end[2] = '\0';
			length = field(in, "Content-Length");
			body = length ? strtoul(length, NULL, 10) : 0;
			if (body > sizeof(rtsp->in) - size) return -1;
			if (left < size + body) {
				end[2] = '\r';
				break;
			}
			answer(rtsp, in, now);
			size += body;
		}
		done += size;
	}
	memmove(rtsp->in, rtsp->in + done, viewer->used - done);
	viewer->used -= done;
	return viewer->used == sizeof(rtsp->in) ? -1 : 0;
}

void rtsp_retry(struct viewer *viewer, int expired) {
	struct rtsp_viewer *rtsp = (struct rtsp_viewer *)viewer;
	if (describe(rtsp)) {
		viewer->stage = REQUEST;
		rtsp->pending = 1;
	} else if (expired) {
		viewer->stage = REQUEST;
		fail(rtsp, "503 Service Unavailable");
	}
}

static size_t batch(struct rtsp_viewer *rtsp, uint64_t head, struct item *items, unsigned char (*headers)[16], struct iovec *parts, size_t *used) {
	struct viewer *viewer = &rtsp->base;
	struct ring *ring = viewer->ring;
	int inside = rtsp->inside, hold = viewer->jump || rtsp->pending, track;
	uint16_t seq[2] = { rtsp->seq[0], rtsp->seq[1] };
	size_t count = 0, length = 0, skip = rtsp->partial;
	unsigned char record[RECORD], *h;
	uint64_t at;
	uint32_t ts;
	for (*used = 0, at = viewer->pos; count < BATCH && head - at >= RECORD; at += RECORD + length) {
		ring_peek(ring, at, record, RECORD);
		length = (size_t)record[0] << 8 | record[1];
		track = record[2] & 1;
		ts = get32(record + 4);
		if (head - at < RECORD + length || (!inside && hold && count)) break;
		if (skip && (rtsp->channel[track] < 0 || 16 + length <= skip)) break;
		if (rtsp->channel[track] < 0 || (track && rtsp->filter && (int32_t)(ts - rtsp->from) < 0)) {
			if (!count) {
				viewer->pos = at + RECORD + length;
			}
			continue;
		}
		h = headers[count];
		h[0] = '$';
		h[1] = (unsigned char)rtsp->channel[track];
		put16(h + 2, (unsigned int)(12 + length));
		h[4] = 0x80;
		h[5] = (unsigned char)((record[3] & MARKER ? 0x80 : 0) | (96 + track));
		put16(h + 6, seq[track]++);
		put32(h + 8, ts - rtsp->origin[track]);
		put32(h + 12, viewer->stream->ssrc[track]);
		items[count] = (struct item){ at, at + RECORD + length, 16 + length, ts - rtsp->origin[track], track, record[3] };
		if (skip < 16) {
			parts[*used].iov_base = h + skip;
			parts[(*used)++].iov_len = 16 - skip;
			skip = 16;
		}
		*used += ring_slice(ring, at + RECORD + skip - 16, length + 16 - skip, parts + *used);
		skip = 0;
		count++;
		if (!track) {
			inside = !(record[3] & UNIT_END);
		}
	}
	return count;
}

static void commit(struct rtsp_viewer *rtsp, const struct item *items, size_t count, size_t sent) {
	const struct item *item;
	int64_t delta, offset;
	uint64_t base;
	size_t i, size;
	int ahead;
	for (i = 0; i < count; i++) {
		item = &items[i];
		size = item->size - (i ? 0 : rtsp->partial);
		if (sent < size) {
			rtsp->partial = (i ? 0 : rtsp->partial) + sent;
			rtsp->base.pos = item->start;
			return;
		}
		sent -= size;
		rtsp->partial = 0;
		rtsp->base.pos = item->end;
		rtsp->seq[item->track]++;
		ahead = !rtsp->sent[item->track] || (int32_t)(item->time - rtsp->last[item->track]) > 0;
		if (ahead) {
			rtsp->step[item->track] = rtsp->sent[item->track] ? item->time - rtsp->last[item->track] : item->track ? 1024 : STEP;
			rtsp->last[item->track] = item->time;
		}
		rtsp->sent[item->track]++;
		rtsp->octets[item->track] += (uint32_t)(item->size - 16);
		if (item->track) {
			rtsp->filter = 0;
			base = rtsp->clock * (uint64_t)rtsp->rate / 90000;
			offset = (int32_t)(item->time - (uint32_t)base);
			if (ahead) {
				rtsp->position[1] = base + (uint64_t)offset;
			}
			delta = offset * 90000 / rtsp->rate;
		} else {
			rtsp->inside = !(item->marker & UNIT_END);
			offset = (int32_t)(item->time - (uint32_t)rtsp->clock);
			if (ahead) {
				rtsp->position[0] = rtsp->clock + (uint64_t)offset;
			}
			delta = offset;
		}
		if (delta + STEP > 0) {
			rtsp->clock += (uint64_t)(delta + STEP);
		}
	}
}

int rtsp_flush(struct viewer *viewer, uint64_t now) {
	struct rtsp_viewer *rtsp = (struct rtsp_viewer *)viewer;
	unsigned char headers[BATCH][16];
	struct iovec parts[BATCH * 3];
	struct item items[BATCH];
	struct ring *ring;
	size_t count, used;
	uint64_t head;
	ssize_t n;
	while (viewer->writable) {
		if (viewer->out_size) {
			if (drain(viewer, now) < 0) return CLOSE;
			continue;
		}
		if (viewer->closing) return CLOSE;
		if (!rtsp->partial && !rtsp->inside && rtsp->pending) {
			rtsp->pending = 0;
			if (rtsp_input(viewer, now) < 0) return CLOSE;
			continue;
		}
		if (viewer->stage != STREAM) return KEEP;
		ring = viewer->ring;
		if (atomic_load(&ring->epoch) != viewer->epoch) return ABORT;
		if (!rtsp->partial && now - rtsp->reported >= RTCP_INTERVAL) {
			rtcp(rtsp, now);
			continue;
		}
		if (rtsp->paused) return KEEP;
		head = atomic_load_explicit(&ring->head, memory_order_acquire);
		if (head - viewer->pos > ring->size / 2) return ABORT;
		catch_up(viewer, head);
		if (!rtsp->partial && !rtsp->inside && viewer->jump) {
			restart(rtsp, &viewer->target);
			continue;
		}
		count = batch(rtsp, head, items, headers, parts, &used);
		if (!count) return overrun(viewer) ? ABORT : KEEP;
		n = transmit(viewer, parts, used, now);
		if (n < 0) return CLOSE;
		if (overrun(viewer)) return ABORT;
		commit(rtsp, items, count, (size_t)n);
	}
	return KEEP;
}

int rtsp_idle(struct viewer *viewer, uint64_t now) {
	struct rtsp_viewer *rtsp = (struct rtsp_viewer *)viewer;
	uint64_t fed = atomic_load_explicit(&viewer->ring->fed, memory_order_relaxed);
	if (atomic_load(&viewer->ring->epoch) != viewer->epoch) return 1;
	if (rtsp->paused) return now - rtsp->request >= TIMEOUT * 1000;
	if (!viewer->writable && now - viewer->moved >= STALL) return 1;
	return now > fed && now - fed >= OUTAGE;
}
