#ifndef STREAM_H
#define STREAM_H

#include <srt/srt.h>

#include "config.h"
#include "ring.h"
#include "rtp.h"
#include "ts.h"

#define PEER_SIZE (INET6_ADDRSTRLEN + 8)

enum state {
	IDLE,
	LIVE,
	OFFLINE,
};

struct stream {
	char name[64];
	unsigned char sdt[PACKET];
	_Atomic unsigned int version;
	_Atomic int removed;
	_Atomic int state;
	struct video *_Atomic video;
	_Atomic unsigned int audio_kbps;
	_Atomic unsigned int lagging;
	uint32_t ssrc[2];
	struct ring ts;
	struct ring rtp;
	char passphrase[80];
	char sdp[2048];
	size_t sdp_size;
	int rate;
	SRTSOCKET publisher;
	char peer[PEER_SIZE];
	int latency;
	int aired;
	struct feed live;
	struct feed still;
	struct packer packer;
	uint64_t connected;
	int warned;
	int crowded;
	uint64_t last_data;
	uint64_t offline_until;
	uint64_t skipped;
	unsigned int lost;
	int offline;
	size_t video_pos;
	uint64_t video_base;
	uint64_t video_ticks;
	uint64_t video_pcr;
	uint64_t video_at;
	int video_clocked;
	int video_started;
	int timeline;
	int whole;
	uint64_t last_ts;
	uint64_t last_pcr;
	uint64_t frame_ts;
	uint64_t frame_step;
	uint64_t tabled;
	unsigned char cc[TOP_PID + 1];
};

extern struct stream streams[MAX_STREAMS];
extern _Atomic unsigned int stream_count;

int open_stream(struct stream *stream, const char *name);
void rename_stream(struct stream *stream, const char *name);
struct stream *find_stream(const char *name, size_t length, unsigned int *version);
void copy_sdt(struct stream *stream, unsigned char *out);
int on_air(struct stream *stream);
void publish(struct stream *stream, uint64_t now);

#endif
