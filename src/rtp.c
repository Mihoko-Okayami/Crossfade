#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "main.h"
#include "stream.h"

#define VIDEO_PAYLOAD 1388

static const char base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };

void put16(unsigned char *p, unsigned int v) {
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)v;
}

void put32(unsigned char *p, uint32_t v) {
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

uint32_t get32(const unsigned char *p) {
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void encode(char *out, const unsigned char *in, size_t size) {
	uint32_t v;
	size_t i;
	for (i = 0; i < size; i += 3) {
		v = (uint32_t)in[i] << 16 | (i + 1 < size ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < size ? in[i + 2] : 0);
		*out++ = base64[(v >> 18) & 63];
		*out++ = base64[(v >> 12) & 63];
		*out++ = i + 1 < size ? base64[(v >> 6) & 63] : '=';
		*out++ = i + 2 < size ? base64[v & 63] : '=';
	}
	*out = '\0';
}

static void update_sdp(struct stream *stream, int audio, int reset) {
	struct packer *packer = &stream->packer;
	char sps[sizeof(packer->sps) * 4 / 3 + 4], pps[sizeof(packer->pps) * 4 / 3 + 4], sdp[sizeof(stream->sdp)];
	int n, live = atomic_load(&stream->state) == LIVE, channels = packer->channels == 7 ? 8 : packer->channels;
	uint64_t now = (uint64_t)time(NULL) + 2208988800u, id = stream->sdp_size ? stream->sdp_id : now, version = stream->sdp_size && stream->sdp_version >= now ? stream->sdp_version + 1 : now;
	if (!packer->sps_size || !packer->pps_size || !packer->rate) return;
	encode(sps, packer->sps, packer->sps_size);
	encode(pps, packer->pps, packer->pps_size);
	n = snprintf(sdp, sizeof(sdp), "v=0\r\no=- %llu %llu IN IP4 0.0.0.0\r\ns=Streamed by crossfade %s\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\na=range:npt=now-\r\na=control:*\r\nm=video 0 RTP/AVP 96\r\na=fmtp:96 packetization-mode=1; profile-level-id=%02X%02X%02X; sprop-parameter-sets=%s,%s\r\na=rtpmap:96 H264/90000\r\na=control:trackID=0\r\nm=audio 0 RTP/AVP 97\r\na=fmtp:97 streamtype=5;profile-level-id=%d;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=%02X%02X\r\na=rtpmap:97 mpeg4-generic/%d/%d\r\na=control:trackID=1\r\n", (unsigned long long)id, (unsigned long long)version, VERSION, packer->sps[1], packer->sps[2], packer->sps[3], sps, pps, channels > 6 ? (packer->rate > 48000 ? 81 : 80) : packer->rate > 48000 ? 43 : channels > 2 ? 42 : 41, packer->config[0], packer->config[1], packer->rate, channels);
	if (n <= 0 || (size_t)n >= sizeof(sdp) || (stream->sdp_size && !strcmp(strstr(sdp, "\r\ns="), strstr(stream->sdp, "\r\ns=")))) return;
	if (reset && stream->sdp_size && audio) {
		tell(CHANGED, live ? "%s: publisher %s changed the audio to %d Hz, %d channel%s, RTSP viewers reconnect" : "%s: the offline video%.0s changed the audio to %d Hz, %d channel%s, RTSP viewers reconnect", stream->name, stream->peer, packer->rate, channels, channels > 1 ? "s" : "");
	} else if (reset && stream->sdp_size) {
		tell(CHANGED, live ? "%s: publisher %s changed the video settings, RTSP viewers reconnect" : "%s: the offline video%.0s changed the video settings, RTSP viewers reconnect", stream->name, stream->peer);
	}
	pthread_mutex_lock(&stream->rtp.lock);
	memcpy(stream->sdp, sdp, (size_t)n + 1);
	stream->sdp_size = (size_t)n;
	stream->sdp_id = id;
	stream->sdp_version = version;
	stream->rate = packer->rate;
	if (reset) {
		stream->rtp.mark_count = 0;
		atomic_fetch_add(&stream->rtp.epoch, 1);
	}
	pthread_mutex_unlock(&stream->rtp.lock);
}

static uint64_t unwrap(struct packer *packer, uint64_t pts) {
	uint64_t delta = (pts - packer->clock) & (WRAP - 1);
	packer->clock += delta < WRAP / 2 ? delta : delta - WRAP;
	return packer->clock;
}

static void record(struct stream *stream, int track, int flags, uint32_t time, const unsigned char *head, size_t head_size, const unsigned char *data, size_t size) {
	unsigned char header[RECORD + 4];
	put16(header, (unsigned int)(head_size + size));
	header[2] = (unsigned char)track;
	header[3] = (unsigned char)flags;
	put32(header + 4, time);
	memcpy(header + RECORD, head, head_size);
	ring_write(&stream->rtp, header, RECORD + head_size);
	ring_write(&stream->rtp, data, size);
}

static void release(struct stream *stream, uint32_t limit, int all) {
	struct packer *packer = &stream->packer;
	size_t at = 0;
	while (at < packer->held_size && (all || (int32_t)(get32(packer->held + at + 4) - limit) < 0)) {
		at += RECORD + ((size_t)packer->held[at] << 8 | packer->held[at + 1]);
	}
	if (!at) return;
	ring_write(&stream->rtp, packer->held, at);
	memmove(packer->held, packer->held + at, packer->held_size - at);
	packer->held_size -= at;
}

static void hold(struct stream *stream, uint32_t time, const unsigned char *header, const unsigned char *data, size_t size) {
	struct packer *packer = &stream->packer;
	unsigned char *p;
	if (packer->held_size + RECORD + 4 + size > sizeof(packer->held)) {
		release(stream, 0, 1);
	}
	p = packer->held + packer->held_size;
	put16(p, (unsigned int)(4 + size));
	p[2] = 1;
	p[3] = MARKER;
	put32(p + 4, time);
	memcpy(p + RECORD, header, 4);
	memcpy(p + RECORD + 4, data, size);
	packer->held_size += RECORD + 4 + size;
}

static const unsigned char *next_nal(const unsigned char **cursor, const unsigned char *end, size_t *size) {
	const unsigned char *p = *cursor, *start;
	for (;;) {
		while (p + 3 <= end && (p[0] || p[1] || p[2] != 1)) {
			p++;
		}
		if (p + 3 > end) return NULL;
		start = p + 3;
		for (p = start; p + 3 <= end && (p[0] || p[1] || p[2] != 1); p++) continue;
		if (p + 3 > end) {
			p = end;
		}
		*cursor = p;
		*size = (size_t)(p - start);
		while (*size && !start[*size - 1]) {
			(*size)--;
		}
		if (*size) return start;
	}
}

static int wanted(int type, int *sliced) {
	if (type >= 1 && type <= 5) return *sliced = 1;
	return (type >= 6 && type <= 8 && !*sliced) || type == 12;
}

static int keep(unsigned char *store, size_t capacity, size_t *stored, const unsigned char *nal, size_t size, int *changed) {
	if (size > capacity) return 0;
	if (size != *stored || memcmp(store, nal, size)) {
		memcpy(store, nal, size);
		*stored = size;
		*changed = 1;
	}
	return 1;
}

static void send_nal(struct stream *stream, const unsigned char *nal, size_t length, uint32_t time, int last) {
	int type = nal[0] & 0x1f, flags = last ? MARKER : 0;
	unsigned char head[3];
	size_t offset, chunk;
	if (length < VIDEO_PAYLOAD) {
		head[0] = (unsigned char)((nal[0] & 0xe0) | 24);
		put16(head + 1, (unsigned int)length);
		record(stream, 0, flags, time, head, 3, nal, length);
		return;
	}
	head[0] = (unsigned char)((nal[0] & 0xe0) | 28);
	for (offset = 1; offset < length; offset += chunk) {
		chunk = length - offset < VIDEO_PAYLOAD - 2 ? length - offset : VIDEO_PAYLOAD - 2;
		head[1] = (unsigned char)((offset == 1 ? 0x80 : 0) | (offset + chunk == length ? 0x40 : 0) | type);
		record(stream, 0, offset + chunk == length ? flags : 0, time, head, 2, nal + offset, chunk);
	}
}

static void pack_video(struct stream *stream, const unsigned char *data, size_t size, uint64_t pts) {
	struct packer *packer = &stream->packer;
	const unsigned char *end = data + size, *cursor, *nal;
	struct mark mark;
	int type, key = 0, sps = 0, pps = 0, sliced = 0, changed = 0, total = 0, index = 0, configured = 0;
	uint64_t clock = unwrap(packer, pts);
	size_t length;
	for (cursor = data; (nal = next_nal(&cursor, end, &length));) {
		type = nal[0] & 0x1f;
		if (type == 7 && !sliced && !sps++ && !keep(packer->sps, sizeof(packer->sps), &packer->sps_size, nal, length, &changed)) {
			tell(OVERSIZED, "%s: a %zu-byte SPS exceeds %zu bytes, RTSP %s", stream->name, length, sizeof(packer->sps), packer->sps_size ? "keeps the previous one" : "waits for a smaller one");
		} else if (type == 8 && !sliced && !pps++ && !keep(packer->pps, sizeof(packer->pps), &packer->pps_size, nal, length, &changed)) {
			tell(OVERSIZED, "%s: a %zu-byte PPS exceeds %zu bytes, RTSP %s", stream->name, length, sizeof(packer->pps), packer->pps_size ? "keeps the previous one" : "waits for a smaller one");
		}
		key |= type == 5;
		total += wanted(type, &sliced) && type != 7 && type != 8;
	}
	if (changed) {
		update_sdp(stream, 0, config.reconnect);
	}
	if (!sliced || !stream->sdp_size) return;
	release(stream, (uint32_t)(clock * (uint64_t)packer->rate / 90000), 0);
	if (key) {
		mark = (struct mark){ .at = stream->rtp.written, .time = msec(), .ts = { (uint32_t)clock, (uint32_t)(clock * (uint64_t)packer->rate / 90000) } };
		ring_mark(&stream->rtp, &mark, packer->onset);
		packer->onset = 0;
	}
	for (sliced = 0, cursor = data; (nal = next_nal(&cursor, end, &length));) {
		type = nal[0] & 0x1f;
		if (!wanted(type, &sliced) || type == 7 || type == 8) continue;
		if (key && !configured++) {
			send_nal(stream, packer->sps, packer->sps_size, (uint32_t)clock, 0);
			send_nal(stream, packer->pps, packer->pps_size, (uint32_t)clock, 0);
		}
		send_nal(stream, nal, length, (uint32_t)clock, ++index == total);
	}
}

static void pack_audio(struct stream *stream, const unsigned char *data, size_t size, uint64_t pts) {
	struct packer *packer = &stream->packer;
	unsigned char header[4] = { 0, 16 }, setup[2];
	uint64_t clock = unwrap(packer, pts);
	size_t frame, head, au;
	unsigned int count = 0;
	int index, channels;
	while (size >= 7 && data[0] == 0xff && (data[1] & 0xf6) == 0xf0 && !(data[6] & 3)) {
		frame = ((size_t)(data[3] & 3) << 11) | ((size_t)data[4] << 3) | (data[5] >> 5);
		head = data[1] & 1 ? 7 : 9;
		index = (data[2] >> 2) & 15;
		channels = ((data[2] & 1) << 2) | (data[3] >> 6);
		if (frame <= head || frame > size || index >= 13 || !channels) return;
		setup[0] = (unsigned char)((((data[2] >> 6) + 1) << 3) | (index >> 1));
		setup[1] = (unsigned char)(((index & 1) << 7) | (channels << 3));
		if (memcmp(setup, packer->config, sizeof(setup))) {
			memcpy(packer->config, setup, sizeof(setup));
			packer->rate = rates[index];
			packer->channels = channels;
			update_sdp(stream, 1, 1);
		}
		au = frame - head;
		packer->au_bytes += au;
		packer->au_count++;
		header[2] = (unsigned char)(au >> 5);
		header[3] = (unsigned char)((au << 3) & 0xf8);
		if (stream->sdp_size) {
			hold(stream, (uint32_t)(clock * (uint64_t)packer->rate / 90000 + count * 1024u), header, data + head, au);
		}
		data += frame;
		size -= frame;
		count++;
	}
}

static int append(struct track *track, const unsigned char *data, size_t size) {
	if (track->used + size > track->size) {
		track->open = 0;
		return 0;
	}
	memcpy(track->data + track->used, data, size);
	track->used += size;
	return 1;
}

static void close_track(struct stream *stream, struct track *track) {
	size_t size = track->length && track->length < track->used ? track->length : track->used;
	track->open = 0;
	if (track == &stream->packer.video) {
		pack_video(stream, track->data, size, track->pts);
	} else {
		pack_audio(stream, track->data, size, track->pts);
	}
}

static void open_track(struct track *track, const unsigned char *pes, size_t size) {
	size_t length, skip;
	track->open = 0;
	track->used = 0;
	if (size < 14 || pes[0] || pes[1] || pes[2] != 1 || !(pes[7] & 0x80) || (skip = 9 + (size_t)pes[8]) > size) return;
	length = (size_t)pes[4] << 8 | pes[5];
	if (length && length < 3 + (size_t)pes[8]) return;
	track->length = length ? length - 3 - pes[8] : 0;
	track->pts = get_stamp(pes + 9);
	track->open = 1;
	append(track, pes + skip, size - skip);
}

void pack(struct stream *stream, const unsigned char *p) {
	struct packer *packer = &stream->packer;
	int pid = pid_of(p), at = payload_of(p), cc = p[3] & 15;
	struct track *track = pid == VIDEO_PID ? &packer->video : pid == AUDIO_PID ? &packer->audio : NULL;
	if (!track) return;
	if (p[3] & 0x10) {
		if (track->cc >= 0 && cc != ((track->cc + 1) & 15)) {
			track->open = 0;
		}
		track->cc = cc;
	}
	if (at >= PACKET) return;
	if (p[1] & 0x40) {
		if (track->open) {
			close_track(stream, track);
		}
		open_track(track, p + at, (size_t)(PACKET - at));
	} else if (track->open && !append(track, p + at, (size_t)(PACKET - at))) {
		tell(OVERFLOWED, "%s: a%s frame exceeds %zu KiB, RTSP skips such frames", stream->name, track == &packer->video ? " video" : "n audio", track->size >> 10);
	}
	if (track->open && track->length && track->used >= track->length) {
		close_track(stream, track);
	}
}

void reset_packer(struct stream *stream, int whole) {
	if (whole && stream->packer.video.open) {
		close_track(stream, &stream->packer.video);
	}
	if (whole && stream->packer.audio.open) {
		close_track(stream, &stream->packer.audio);
	}
	stream->packer.video.open = 0;
	stream->packer.audio.open = 0;
	release(stream, 0, 1);
}
