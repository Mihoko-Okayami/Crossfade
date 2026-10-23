#define _GNU_SOURCE

#include <string.h>
#include <sys/mman.h>

#include "config.h"
#include "main.h"
#include "ring.h"

int ring_open(struct ring *ring, uint64_t size) {
	pthread_mutex_init(&ring->lock, NULL);
	ring->window_start = msec();
	if (!size) return 1;
	ring->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ring->data == MAP_FAILED) {
		ring->data = NULL;
		return 0;
	}
	ring->size = size;
	return 1;
}

void ring_close(struct ring *ring) {
	if (ring->data) {
		munmap(ring->data, ring->size);
	}
	ring->data = NULL;
	ring->size = 0;
}

void ring_shrink(struct ring *ring) {
	if (ring->data) {
		madvise(ring->data, ring->size, MADV_DONTNEED);
	}
}

void ring_write(struct ring *ring, const void *data, size_t size) {
	uint64_t at = ring->written % ring->size, first = size < ring->size - at ? size : ring->size - at;
	memcpy(ring->data + at, data, first);
	memcpy(ring->data, (const unsigned char *)data + first, size - first);
	ring->written += size;
	if (ring->written - atomic_load_explicit(&ring->head, memory_order_relaxed) > ring->size / 4) {
		ring_release(ring, msec());
	}
}

void ring_mark(struct ring *ring, const struct mark *mark) {
	pthread_mutex_lock(&ring->lock);
	ring->marks[ring->mark_count % MAX_MARKS] = *mark;
	ring->mark_count++;
	pthread_mutex_unlock(&ring->lock);
}

void ring_forget(struct ring *ring) {
	pthread_mutex_lock(&ring->lock);
	ring->mark_count = 0;
	atomic_fetch_add(&ring->epoch, 1);
	pthread_mutex_unlock(&ring->lock);
}

void ring_release(struct ring *ring, uint64_t now) {
	if (atomic_load_explicit(&ring->head, memory_order_relaxed) == ring->written) return;
	atomic_store_explicit(&ring->fed, now, memory_order_relaxed);
	atomic_store_explicit(&ring->head, ring->written, memory_order_release);
}

void ring_measure(struct ring *ring, uint64_t now) {
	atomic_store(&ring->rate, (ring->written - ring->window_written) * 1000 / (now - ring->window_start));
	ring->window_start = now;
	ring->window_written = ring->written;
}

uint64_t ring_limit(struct ring *ring) {
	uint64_t bytes = atomic_load_explicit(&ring->rate, memory_order_relaxed) * (uint64_t)config.lag / 1000;
	return bytes && bytes < ring->size / 4 ? bytes : ring->size / 4;
}

int ring_find(struct ring *ring, int backlog, struct mark *mark, unsigned int *epoch) {
	uint64_t now, head = atomic_load_explicit(&ring->head, memory_order_acquire), oldest, behind = ring_limit(ring);
	const struct mark *candidate;
	unsigned int i, count;
	int found = 0;
	oldest = head > ring->size / 2 ? head - ring->size / 2 : 0;
	pthread_mutex_lock(&ring->lock);
	now = msec();
	count = ring->mark_count < MAX_MARKS ? ring->mark_count : MAX_MARKS;
	for (i = 1; i <= count; i++) {
		candidate = &ring->marks[(ring->mark_count - i) % MAX_MARKS];
		if (candidate->at >= head) continue;
		if (candidate->at < oldest || (found && head - candidate->at > behind)) break;
		*mark = *candidate;
		found = 1;
		if (now - candidate->time >= (uint64_t)backlog) break;
	}
	*epoch = atomic_load(&ring->epoch);
	pthread_mutex_unlock(&ring->lock);
	return found;
}

int ring_head(struct ring *ring, struct mark *mark, unsigned int *epoch) {
	uint64_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
	if (!head) return 0;
	memset(mark, 0, sizeof(*mark));
	mark->at = head;
	*epoch = atomic_load(&ring->epoch);
	return 1;
}

void ring_peek(const struct ring *ring, uint64_t at, unsigned char *out, size_t size) {
	uint64_t offset = at % ring->size, first = size < ring->size - offset ? size : ring->size - offset;
	memcpy(out, ring->data + offset, first);
	memcpy(out + first, ring->data, size - first);
}

size_t ring_slice(const struct ring *ring, uint64_t at, uint64_t size, struct iovec *parts) {
	uint64_t offset = at % ring->size, first = size < ring->size - offset ? size : ring->size - offset;
	parts[0].iov_base = ring->data + offset;
	parts[0].iov_len = first;
	parts[1].iov_base = ring->data;
	parts[1].iov_len = size - first;
	return size > first ? 2 : 1;
}
