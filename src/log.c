#define _GNU_SOURCE

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "main.h"
#include "server.h"

#define BURST 3

struct burst {
	unsigned int shown, more;
	char last[256];
};

static const char *const names[] = { "server", "config", "stream", "viewer" };
static const int topics[EVENTS] = { [UNACCEPTED] = ON_VIEWER | FAIL, [FULL] = ON_VIEWER | FAIL, [STARVED] = ON_VIEWER | FAIL, [OVERSIZED] = ON_STREAM | WARN, [OVERFLOWED] = ON_STREAM | WARN, [CHANGED] = ON_STREAM, [UNMATCHED] = ON_SERVER | WARN, [BARRED] = ON_STREAM | WARN, [STALLED] = ON_SERVER | FAIL };
static pthread_mutex_t told = PTHREAD_MUTEX_INITIALIZER;
static struct burst bursts[EVENTS];
static int color;

void paint(void) {
	const char *plain = getenv("NO_COLOR");
	color = !plain || !*plain;
}

static void stamp(char *text, size_t size) {
	time_t now = time(NULL);
	struct tm local;
	*text = '\0';
	if (localtime_r(&now, &local)) {
		strftime(text, size, "%Y-%m-%d %H:%M:%S", &local);
	}
}

static void put(int topic, const char *message) {
	char when[32];
	stamp(when, sizeof(when));
	if (topic & (WARN | FAIL)) {
		fprintf(stderr, color ? "%s  \033[1m%s\033[0m  \033[1;%sm%s\033[0m  %s\n" : "%s  %s  %.0s%s  %s\n", when, names[topic & 3], topic & FAIL ? "31" : "33", topic & FAIL ? "FAIL" : "WARN", message);
	} else {
		fprintf(stderr, color ? "%s  \033[1m%s\033[0m        %s\n" : "%s  %s        %s\n", when, names[topic & 3], message);
	}
}

void headline(void) {
	char when[32];
	stamp(when, sizeof(when));
	fprintf(stderr, color ? "%s  \033[1mCrossfade\033[0m %s%s\n" : "%s  Crossfade %s%s\n", when, isdigit((unsigned char)*VERSION) ? "v" : "", VERSION);
}

void note(int topic, const char *format, ...) {
	char message[512];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	put(topic, message);
}

void tell(enum event event, const char *format, ...) {
	struct burst *burst = &bursts[event];
	char message[256];
	va_list args;
	int shown;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	pthread_mutex_lock(&told);
	shown = burst->shown < BURST;
	burst->shown += shown;
	if (!shown) {
		burst->more++;
		snprintf(burst->last, sizeof(burst->last), "%s", message);
	}
	pthread_mutex_unlock(&told);
	if (shown) {
		put(topics[event], message);
	}
}

void settle(void) {
	struct burst copy[EVENTS];
	int event;
	pthread_mutex_lock(&told);
	memcpy(copy, bursts, sizeof(bursts));
	for (event = 0; event < EVENTS; event++) {
		bursts[event].shown = bursts[event].more ? BURST : 0;
		bursts[event].more = 0;
	}
	pthread_mutex_unlock(&told);
	for (event = 0; event < EVENTS; event++) {
		if (copy[event].more == 1) {
			put(topics[event], copy[event].last);
		} else if (copy[event].more) {
			note(topics[event], "%u more in the last 10 s, the last: %s", copy[event].more, copy[event].last);
		}
	}
}

static void title(const char *name, const char *detail) {
	fprintf(stderr, color ? "\n  \033[1m%s\033[0m%s\n" : "\n  %s%s\n", name, detail);
}

void panel(void) {
	unsigned int i, count = atomic_load(&stream_count);
	char detail[32];
	flockfile(stderr);
	title("Input", "");
	fprintf(stderr, "    SRT   %s\n          AES-%d %s\n", config.srt.text, config.keylen * 8, config.keylen == 32 ? "required" : "or stronger");
	snprintf(detail, sizeof(detail), " (up to %u viewers)", viewer_limit);
	title(config.ts_enabled && config.rtsp_enabled ? "Outputs" : "Output", detail);
	if (config.ts_enabled) {
		fprintf(stderr, "    TS    %s\n", config.ts.text);
	}
	if (config.rtsp_enabled) {
		fprintf(stderr, "    RTSP  %s\n", config.rtsp.text);
	}
	snprintf(detail, sizeof(detail), " (up to %u)", stream_room());
	title("Streams", detail);
	for (i = 0; i < count; i++) {
		fprintf(stderr, "    %s\n", streams[i].name);
		if (!streams[i].offline) {
			fprintf(stderr, "      no offline video\n");
		} else if (streams[i].offline == ALWAYS) {
			fprintf(stderr, "      offline video until a publisher is live\n");
		} else {
			fprintf(stderr, atomic_load(&streams[i].video) ? "      offline video for %d s\n" : "      no offline video, waits %d s for a publisher\n", streams[i].offline);
		}
	}
	fputc('\n', stderr);
	funlockfile(stderr);
}
