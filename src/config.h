#ifndef CONFIG_H
#define CONFIG_H

#include <netinet/in.h>
#include <stdatomic.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>

#define CONFIG_PATH "/etc/crossfade/crossfade.conf"
#define MAX_STREAMS 256
#define VIDEO_TRACK (4 << 20)
#define AUDIO_TRACK (64 << 10)
#define MAX_WORKERS 64
#define ALWAYS (-1)

struct endpoint {
	struct sockaddr_storage address;
	socklen_t length;
	char text[80];
};

struct video {
	char path[256];
	unsigned char *data;
	size_t size;
	dev_t device;
	ino_t inode;
	struct timespec modified;
	uint64_t tail;
};

struct config {
	struct endpoint srt;
	struct endpoint ts;
	struct endpoint rtsp;
	int workers;
	int buffer;
	int lag;
	int wait;
	int keylen;
	int latency;
	int timeout;
	int ts_enabled;
	int backlog;
	int rtsp_enabled;
	int rtsp_backlog;
	int reconnect;
	int clients;
};

struct entry {
	char name[64];
	char passphrase[80];
	int offline;
	struct video *video;
};

struct plan {
	struct config config;
	struct entry entries[MAX_STREAMS];
	unsigned int count;
};

extern struct config config;
extern atomic_int contained;

struct plan *read_plan(const char *path, int probe);
void free_plan(struct plan *plan);
void free_video(struct video *video);
unsigned int stream_room(void);
void note_restart(const struct config *next);

#endif
