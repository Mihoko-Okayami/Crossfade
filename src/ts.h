#ifndef TS_H
#define TS_H

#include <stddef.h>
#include <stdint.h>

#define PACKET 188
#define WRAP ((uint64_t)1 << 33)
#define GAP 27000
#define VIDEO_JUMP 180000
#define PMT_PID 0x100
#define VIDEO_PID 0x101
#define AUDIO_PID 0x102
#define TOP_PID AUDIO_PID

struct stream;

struct feed {
	unsigned char partial[PACKET];
	size_t partial_used;
	int pmt_pid;
	int video_pid;
	int audio_pid;
	int ready;
	int synced;
	int cc[2];
	uint64_t offset;
};

int pid_of(const unsigned char *p);
int payload_of(const unsigned char *p);
int has_pcr(const unsigned char *p);
uint64_t get_pcr(const unsigned char *p);
void set_pcr(unsigned char *p, uint64_t base);
uint64_t get_stamp(const unsigned char *b);
unsigned char *stamps(unsigned char *p, int *count);
void make_tables(void);
void make_sdt(unsigned char *p, const char *name);
void reset_feed(struct feed *feed);
void scan(struct feed *feed, const unsigned char *p);
int is_key(const struct feed *feed, const unsigned char *p);
void take(struct stream *stream, struct feed *feed, unsigned char *p);
void idle(struct stream *stream, uint64_t now);

#endif
