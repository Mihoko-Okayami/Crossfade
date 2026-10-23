#ifndef RING_H
#define RING_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

#define MAX_MARKS 128

struct mark {
	uint64_t at;
	uint64_t time;
	uint32_t ts[2];
	unsigned char cc[2];
};

struct ring {
	unsigned char *data;
	uint64_t size;
	_Atomic uint64_t head;
	_Atomic uint64_t rate;
	_Atomic uint64_t fed;
	_Atomic unsigned int epoch;
	pthread_mutex_t lock;
	uint64_t written;
	uint64_t window_start;
	uint64_t window_written;
	struct mark marks[MAX_MARKS];
	unsigned int mark_count;
};

int ring_open(struct ring *ring, uint64_t size);
void ring_close(struct ring *ring);
void ring_shrink(struct ring *ring);
void ring_write(struct ring *ring, const void *data, size_t size);
void ring_mark(struct ring *ring, const struct mark *mark);
void ring_forget(struct ring *ring);
void ring_release(struct ring *ring, uint64_t now);
void ring_measure(struct ring *ring, uint64_t now);
uint64_t ring_limit(struct ring *ring);
int ring_find(struct ring *ring, int backlog, struct mark *mark, unsigned int *epoch);
int ring_head(struct ring *ring, struct mark *mark, unsigned int *epoch);
void ring_peek(const struct ring *ring, uint64_t at, unsigned char *out, size_t size);
size_t ring_slice(const struct ring *ring, uint64_t at, uint64_t size, struct iovec *parts);

#endif
