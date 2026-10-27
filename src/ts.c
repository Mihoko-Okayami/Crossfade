#define _GNU_SOURCE

#include <string.h>

#include "main.h"
#include "stream.h"

#define START 90000
#define TABLE_INTERVAL 400

static unsigned char pat[PACKET], pmt[PACKET];

int pid_of(const unsigned char *p) {
	return (p[1] & 0x1f) << 8 | p[2];
}

int payload_of(const unsigned char *p) {
	if (!(p[3] & 0x10)) return PACKET;
	if (!(p[3] & 0x20)) return 4;
	return p[4] < PACKET - 5 ? 5 + p[4] : PACKET;
}

int has_pcr(const unsigned char *p) {
	return (p[3] & 0x20) && p[4] >= 7 && p[4] <= PACKET - 5 && (p[5] & 0x10);
}

uint64_t get_pcr(const unsigned char *p) {
	return (uint64_t)p[6] << 25 | (uint64_t)p[7] << 17 | (uint64_t)p[8] << 9 | (uint64_t)p[9] << 1 | p[10] >> 7;
}

static void set_pcr(unsigned char *p, uint64_t base) {
	p[6] = (unsigned char)(base >> 25);
	p[7] = (unsigned char)(base >> 17);
	p[8] = (unsigned char)(base >> 9);
	p[9] = (unsigned char)(base >> 1);
	p[10] = (unsigned char)(((base & 1) << 7) | (p[10] & 0x7f));
}

uint64_t get_stamp(const unsigned char *b) {
	return (uint64_t)((b[0] >> 1) & 7) << 30 | (uint64_t)b[1] << 22 | (uint64_t)(b[2] >> 1) << 15 | (uint64_t)b[3] << 7 | b[4] >> 1;
}

static void set_stamp(unsigned char *b, uint64_t t) {
	b[0] = (unsigned char)((b[0] & 0xf0) | ((t >> 29) & 0x0e) | 1);
	b[1] = (unsigned char)(t >> 22);
	b[2] = (unsigned char)(((t >> 14) & 0xfe) | 1);
	b[3] = (unsigned char)(t >> 7);
	b[4] = (unsigned char)(((t << 1) & 0xfe) | 1);
}

unsigned char *stamps(unsigned char *p, int *count) {
	int at = payload_of(p);
	unsigned char *pes = p + at;
	*count = 0;
	if (!(p[1] & 0x40) || at + 14 > PACKET || pes[0] || pes[1] || pes[2] != 1) return NULL;
	if (pes[3] == 0xbc || pes[3] == 0xbe || pes[3] == 0xbf || (pes[3] >= 0xf0 && pes[3] <= 0xf2) || pes[3] == 0xf8 || pes[3] == 0xff) return NULL;
	if (pes[7] >> 6 == 2) {
		*count = 1;
	} else if (pes[7] >> 6 == 3 && at + 19 <= PACKET) {
		*count = 2;
	}
	return *count ? pes + 9 : NULL;
}

static uint64_t later(uint64_t a, uint64_t b) {
	return ((a - b) & (WRAP - 1)) < WRAP / 2 ? a : b;
}

static const unsigned char *section_of(const unsigned char *p, int table, int minimum, int *length) {
	int at = payload_of(p);
	if (!(p[1] & 0x40) || at >= PACKET) return NULL;
	at += 1 + p[at];
	if (at + 3 > PACKET || p[at] != table) return NULL;
	*length = ((p[at + 1] & 0x0f) << 8) | p[at + 2];
	return *length >= minimum && at + 3 + *length <= PACKET ? p + at : NULL;
}

static void read_pat(struct feed *feed, const unsigned char *p) {
	const unsigned char *section;
	int length, i;
	section = section_of(p, 0, 9, &length);
	for (i = 8; section && i + 4 <= length - 1; i += 4) {
		if (!((section[i] << 8) | section[i + 1])) continue;
		feed->pmt_pid = ((section[i + 2] & 0x1f) << 8) | section[i + 3];
		feed->ready |= 1;
		return;
	}
}

static void read_pmt(struct feed *feed, const unsigned char *p) {
	const unsigned char *section;
	int length, i, pid, video = -1, audio = -1;
	section = section_of(p, 2, 13, &length);
	if (!section) return;
	for (i = 12 + (((section[10] & 0x0f) << 8) | section[11]); i + 5 <= length - 1; i += 5 + (((section[i + 3] & 0x0f) << 8) | section[i + 4])) {
		pid = ((section[i + 1] & 0x1f) << 8) | section[i + 2];
		if (video < 0 && section[i] == 0x1b) {
			video = pid;
		} else if (audio < 0 && section[i] == 0x0f) {
			audio = pid;
		}
	}
	if (video < 0 || audio < 0 || video == audio || (((section[8] & 0x1f) << 8) | section[9]) != video) return;
	feed->video_pid = video;
	feed->audio_pid = audio;
	feed->ready |= 2;
}

void scan(struct feed *feed, const unsigned char *p) {
	int pid = pid_of(p);
	if (pid == 0) {
		read_pat(feed, p);
	} else if (pid == feed->pmt_pid) {
		read_pmt(feed, p);
	}
}

int is_key(const struct feed *feed, const unsigned char *p) {
	int at = payload_of(p), i, type;
	if (pid_of(p) != feed->video_pid || !(p[1] & 0x40) || at + 9 > PACKET) return 0;
	if ((p[3] & 0x20) && p[4] && (p[5] & 0x40)) return 1;
	for (i = at + 9 + p[at + 8]; i + 3 < PACKET; i++) {
		type = p[i + 3] & 0x1f;
		if (!p[i] && !p[i + 1] && p[i + 2] == 1 && (type == 5 || type == 7)) return 1;
	}
	return 0;
}

void reset_feed(struct feed *feed) {
	memset(feed, 0, sizeof(*feed));
	feed->pmt_pid = -1;
	feed->video_pid = -1;
	feed->audio_pid = -1;
	feed->cc[0] = feed->cc[1] = -1;
}

static void table(unsigned char *p, int pid, const unsigned char *section, size_t size) {
	uint32_t crc = 0xffffffff;
	size_t i;
	int bit;
	for (i = 0; i < size; i++) {
		crc ^= (uint32_t)section[i] << 24;
		for (bit = 0; bit < 8; bit++) {
			crc = crc & 0x80000000 ? (crc << 1) ^ 0x04c11db7 : crc << 1;
		}
	}
	memset(p, 0xff, PACKET);
	p[0] = 0x47;
	p[1] = (unsigned char)(0x40 | pid >> 8);
	p[2] = (unsigned char)pid;
	p[3] = 0x10;
	p[4] = 0;
	memcpy(p + 5, section, size);
	put32(p + 5 + size, crc);
}

void make_tables(void) {
	static const unsigned char association[] = { 0x00, 0xb0, 0x0d, 0x00, 0x01, 0xc1, 0x00, 0x00, 0x00, 0x01, 0xe0 | PMT_PID >> 8, PMT_PID & 0xff };
	static const unsigned char map[] = { 0x02, 0xb0, 0x17, 0x00, 0x01, 0xc5, 0x00, 0x00, 0xe0 | VIDEO_PID >> 8, VIDEO_PID & 0xff, 0xf0, 0x00, 0x1b, 0xe0 | VIDEO_PID >> 8, VIDEO_PID & 0xff, 0xf0, 0x00, 0x0f, 0xe0 | AUDIO_PID >> 8, AUDIO_PID & 0xff, 0xf0, 0x00 };
	table(pat, 0, association, sizeof(association));
	table(pmt, PMT_PID, map, sizeof(map));
}

void make_sdt(unsigned char *p, const char *name) {
	static const char provider[] = "crossfade";
	unsigned char section[128] = { 0x42, 0xf0, 0x00, 0x00, 0x01, 0xc1, 0x00, 0x00, 0x00, 0x01, 0xff, 0x00, 0x01, 0xfc, 0x80, 0x00, 0x48, 0x00, 0x01 };
	size_t length = strlen(name), at = 19;
	section[at++] = sizeof(provider) - 1;
	memcpy(section + at, provider, sizeof(provider) - 1);
	at += sizeof(provider) - 1;
	section[at++] = (unsigned char)length;
	memcpy(section + at, name, length);
	at += length;
	section[17] = (unsigned char)(at - 18);
	section[15] = (unsigned char)(at - 16);
	section[2] = (unsigned char)(at - 3 + 4);
	table(p, 0x11, section, at);
}

static void emit(struct stream *stream, const struct feed *feed, const unsigned char *packet, int pid) {
	unsigned char p[PACKET], *stamp;
	uint64_t t = 0;
	int count, i;
	memcpy(p, packet, PACKET);
	p[1] = (unsigned char)((p[1] & 0xe0) | pid >> 8);
	p[2] = (unsigned char)pid;
	if (has_pcr(p)) {
		stream->last_pcr = (get_pcr(p) + feed->offset) & (WRAP - 1);
		set_pcr(p, stream->last_pcr);
	}
	stamp = stamps(p, &count);
	for (i = 0; i < count; i++) {
		t = (get_stamp(stamp + 5 * i) + feed->offset) & (WRAP - 1);
		set_stamp(stamp + 5 * i, t);
		stream->last_ts = later(t, stream->last_ts);
	}
	if (count && pid == VIDEO_PID) {
		stream->frame_step = (t - stream->frame_ts) & (WRAP - 1);
		stream->frame_ts = t;
	}
	if (p[3] & 0x10) {
		stream->cc[pid] = (stream->cc[pid] + 1) & 15;
	}
	p[3] = (unsigned char)((p[3] & 0xf0) | stream->cc[pid]);
	if (stream->ts.data) {
		ring_write(&stream->ts, p, PACKET);
	}
	if (stream->rtp.data) {
		pack(stream, p);
	}
}

static void tables(struct stream *stream) {
	emit(stream, &stream->live, pat, 0);
	emit(stream, &stream->live, pmt, PMT_PID);
}

void idle(struct stream *stream, uint64_t now) {
	if ((int64_t)(now - stream->tabled) < TABLE_INTERVAL) return;
	tables(stream);
	stream->tabled = now;
	publish(stream, now);
}

static void skip_count(struct stream *stream, int pid) {
	stream->cc[pid] = (stream->cc[pid] + 1) & 15;
}

static void follow(struct stream *stream, struct feed *feed, const unsigned char *p, int pid) {
	int track = pid == AUDIO_PID, cc = p[3] & 15, last = feed->cc[track];
	if (!(p[3] & 0x10)) return;
	feed->cc[track] = cc;
	if (last >= 0 && !((p[3] & 0x20) && p[4] && (p[5] & 0x80)) && cc != ((last + 1) & 15) && feed->synced) {
		skip_count(stream, pid);
	}
}

static int sync_feed(struct stream *stream, struct feed *feed, unsigned char *p) {
	unsigned char *stamp;
	uint64_t dts, by_ts, by_pcr, margin;
	int count, whole = stream->whole && feed == &stream->still;
	stamp = stamps(p, &count);
	if (!count) return 0;
	dts = get_stamp(stamp + 5 * (count - 1));
	if (!stream->timeline) {
		feed->offset = (has_pcr(p) ? START - get_pcr(p) : 2 * START - dts) & (WRAP - 1);
		stream->last_ts = (dts + feed->offset) & (WRAP - 1);
		stream->last_pcr = START;
		stream->packer.clock = stream->last_ts;
		stream->timeline = 1;
	} else {
		margin = whole ? 1 : GAP;
		by_ts = (stream->last_ts + margin - (whole ? get_stamp(stamp) : dts)) & (WRAP - 1);
		by_pcr = has_pcr(p) ? (stream->last_pcr + margin - get_pcr(p)) & (WRAP - 1) : by_ts;
		feed->offset = later(by_ts, by_pcr);
		if (whole) {
			feed->offset = later(feed->offset, (stream->frame_ts + stream->frame_step + stream->video->tail - dts) & (WRAP - 1));
		} else {
			skip_count(stream, VIDEO_PID);
			skip_count(stream, AUDIO_PID);
		}
	}
	reset_packer(stream, whole);
	stream->packer.onset = feed == &stream->live;
	if (!whole) {
		atomic_store(&stream->ts.rate, 0);
		atomic_store(&stream->rtp.rate, 0);
	}
	stream->whole = 0;
	feed->synced = 1;
	return 1;
}

void take(struct stream *stream, struct feed *feed, unsigned char *p) {
	struct mark mark = { 0 };
	uint64_t now;
	int pid, key, onset;
	scan(feed, p);
	pid = pid_of(p);
	pid = pid == feed->video_pid ? VIDEO_PID : pid == feed->audio_pid ? AUDIO_PID : -1;
	if (pid < 0) return;
	follow(stream, feed, p, pid);
	key = feed->ready == 3 && is_key(feed, p);
	if (!feed->synced && !key) return;
	if (key) {
		onset = !feed->synced && feed == &stream->live;
		if (!feed->synced && !sync_feed(stream, feed, p)) return;
		mark.at = stream->ts.written;
		mark.time = msec();
		mark.cc[0] = (unsigned char)((stream->cc[VIDEO_PID] + 1) & 15);
		mark.cc[1] = (unsigned char)((stream->cc[AUDIO_PID] + 1) & 15);
		if (stream->ts.data) {
			ring_mark(&stream->ts, &mark, onset);
		}
		tables(stream);
		stream->tabled = mark.time;
	} else if (p[1] & 0x40) {
		now = msec();
		if ((int64_t)(now - stream->tabled) >= TABLE_INTERVAL) {
			tables(stream);
			stream->tabled = now;
		}
	}
	emit(stream, feed, p, pid);
}
