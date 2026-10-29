#ifndef RTP_H
#define RTP_H

#include <stddef.h>
#include <stdint.h>

#define RECORD 8
#define MARKER 1

struct stream;

struct track {
	unsigned char *data;
	size_t size;
	size_t used;
	size_t length;
	uint64_t pts;
	int open;
	int cc;
};

struct packer {
	struct track video;
	struct track audio;
	unsigned char sps[512];
	unsigned char pps[512];
	size_t sps_size;
	size_t pps_size;
	unsigned char config[2];
	int rate;
	int channels;
	int onset;
	uint64_t au_bytes;
	uint64_t au_count;
	uint64_t clock;
	unsigned char held[16384];
	size_t held_size;
};

void put16(unsigned char *p, unsigned int v);
uint32_t get32(const unsigned char *p);
void put32(unsigned char *p, uint32_t v);
void pack(struct stream *stream, const unsigned char *p);
void reset_packer(struct stream *stream, int whole);

#endif
