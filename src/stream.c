#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#include "stream.h"

struct stream streams[MAX_STREAMS];
_Atomic unsigned int stream_count;
static pthread_rwlock_t names = PTHREAD_RWLOCK_INITIALIZER;

int open_stream(struct stream *stream, const char *name) {
	struct packer *packer = &stream->packer;
	uint64_t size = (uint64_t)config.buffer * 1048576 / PACKET * PACKET;
	snprintf(stream->name, sizeof(stream->name), "%s", name);
	make_sdt(stream->sdt, stream->name);
	stream->publisher = SRT_INVALID_SOCK;
	reset_feed(&stream->live);
	reset_feed(&stream->still);
	memset(stream->cc, 15, sizeof(stream->cc));
	if (ring_open(&stream->ts, config.ts_enabled ? size : 0) && ring_open(&stream->rtp, config.rtsp_enabled ? size : 0)) {
		if (!config.rtsp_enabled) return 1;
		packer->video.size = VIDEO_TRACK;
		packer->audio.size = AUDIO_TRACK;
		packer->video.cc = packer->audio.cc = -1;
		packer->video.data = malloc(packer->video.size);
		packer->audio.data = malloc(packer->audio.size);
		if (packer->video.data && packer->audio.data && getrandom(stream->ssrc, sizeof(stream->ssrc), 0) == (ssize_t)sizeof(stream->ssrc)) return 1;
	}
	ring_close(&stream->ts);
	ring_close(&stream->rtp);
	free(packer->video.data);
	free(packer->audio.data);
	packer->video.data = packer->audio.data = NULL;
	return 0;
}

void rename_stream(struct stream *stream, const char *name) {
	pthread_rwlock_wrlock(&names);
	atomic_fetch_add(&stream->version, 1);
	snprintf(stream->name, sizeof(stream->name), "%s", name);
	make_sdt(stream->sdt, stream->name);
	pthread_rwlock_unlock(&names);
}

struct stream *find_stream(const char *name, size_t length, unsigned int *version) {
	unsigned int i, count = atomic_load_explicit(&stream_count, memory_order_acquire);
	struct stream *found = NULL;
	pthread_rwlock_rdlock(&names);
	for (i = 0; i < count && !found; i++) {
		if (atomic_load(&streams[i].removed) || strlen(streams[i].name) != length || memcmp(streams[i].name, name, length)) continue;
		found = &streams[i];
		if (version) {
			*version = atomic_load(&found->version);
		}
	}
	pthread_rwlock_unlock(&names);
	return found;
}

void copy_sdt(struct stream *stream, unsigned char *out) {
	pthread_rwlock_rdlock(&names);
	memcpy(out, stream->sdt, PACKET);
	pthread_rwlock_unlock(&names);
}

int on_air(struct stream *stream) {
	int state = atomic_load(&stream->state);
	return state == LIVE || (state == OFFLINE && stream->video);
}

void publish(struct stream *stream, uint64_t now) {
	ring_release(&stream->ts, now);
	ring_release(&stream->rtp, now);
}
