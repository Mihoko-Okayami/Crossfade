#ifndef SERVER_H
#define SERVER_H

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>

#include "stream.h"

enum protocol {
	HTTP,
	RTSP,
};

enum stage {
	REQUEST,
	WAIT,
	STREAM,
};

enum result {
	KEEP,
	CLOSE,
	ABORT,
};

struct peer {
	unsigned char key[16];
};

struct viewer {
	struct viewer *prev;
	struct viewer *next;
	struct stream *stream;
	struct ring *ring;
	enum protocol protocol;
	enum stage stage;
	int fd;
	int writable;
	int readable;
	int closing;
	int jump;
	int limited;
	int spare;
	unsigned int epoch;
	unsigned int version;
	struct mark target;
	struct peer peer;
	uint64_t pos;
	uint64_t since;
	uint64_t asked;
	uint64_t moved;
	char *in;
	char *out;
	size_t in_room;
	size_t out_room;
	size_t used;
	size_t out_size;
	size_t out_sent;
};

extern _Atomic uint64_t ingest_beat;
extern unsigned int viewer_limit;

void format_date(char *text, size_t size);
void format_host(const struct sockaddr *address, char *text, size_t size);
void format_address(const struct sockaddr *address, char *text, size_t size);
void peer_of(const struct sockaddr *address, struct peer *peer);
uint64_t peer_hash(const struct peer *peer, uint64_t seed);
int admit_source(struct viewer *viewer);
void report_refusals(void);
__attribute__((format(printf, 2, 3)))
int queue(struct viewer *viewer, const char *format, ...);
const char *field(const char *headers, const char *name);
void copy_field(char *out, size_t size, const char *value);
void start(struct viewer *viewer, struct ring *ring, const struct mark *mark);
#define LIVE_EDGE (-1)
int choose(const struct viewer *viewer, struct ring *ring, int backlog, struct mark *mark, unsigned int *epoch);
void catch_up(struct viewer *viewer, uint64_t head);
int overrun(const struct viewer *viewer);
ssize_t transmit(struct viewer *viewer, struct iovec *parts, size_t count, uint64_t now);
int drain(struct viewer *viewer, uint64_t now);
int healthy(void);
unsigned int start_workers(void);
void stop_workers(void);

#endif
