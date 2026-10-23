#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "ingest.h"
#include "log.h"
#include "main.h"
#include "server.h"
#include "ts.h"

#define PROBE_TIMEOUT 2

atomic_int running = 1;

uint64_t msec(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void reload(const char *path) {
	struct plan *plan = read_plan(path, 0);
	if (!plan) {
		note(ON_CONFIG | WARN, "previous configuration kept");
		return;
	}
	note_restart(&plan->config);
	reload_streams(plan);
}

static int probe(const char *path) {
	static const struct timeval limit = { .tv_sec = PROBE_TIMEOUT };
	struct sockaddr_in loopback = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	struct plan *plan = read_plan(path, 1);
	const struct sockaddr_in6 *in6;
	const struct sockaddr_in *in4;
	const struct sockaddr *to;
	const char *request, *problem = "no answer";
	char reply[64] = "", *status;
	struct endpoint endpoint;
	socklen_t length;
	size_t got = 0;
	ssize_t n;
	int fd, ts;
	if (!plan) return 1;
	ts = plan->config.ts_enabled;
	endpoint = ts ? plan->config.ts : plan->config.rtsp;
	free_plan(plan);
	request = ts ? "GET /health HTTP/1.0\r\n\r\n" : "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n";
	in4 = (const struct sockaddr_in *)&endpoint.address;
	in6 = (const struct sockaddr_in6 *)&endpoint.address;
	to = (const struct sockaddr *)&endpoint.address;
	length = endpoint.length;
	if (to->sa_family == AF_INET ? !in4->sin_addr.s_addr : IN6_IS_ADDR_UNSPECIFIED(&in6->sin6_addr)) {
		loopback.sin_port = to->sa_family == AF_INET ? in4->sin_port : in6->sin6_port;
		to = (const struct sockaddr *)&loopback;
		length = sizeof(loopback);
	}
	fd = socket(to->sa_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit)) < 0 || setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit)) < 0 || connect(fd, to, length) < 0 || send(fd, request, strlen(request), MSG_NOSIGNAL) < 0) {
		if (errno != EINPROGRESS) {
			problem = strerror(errno);
		}
	} else {
		while (got < sizeof(reply) - 1 && !memchr(reply, '\n', got) && (n = recv(fd, reply + got, sizeof(reply) - 1 - got, 0)) > 0) {
			got += (size_t)n;
		}
	}
	reply[strcspn(reply, "\r\n")] = '\0';
	status = strchr(reply, ' ');
	if (status && !strncmp(status, " 200 ", 5)) return 0;
	note(ON_SERVER | FAIL, "%s %s: %s", ts ? "MPEG-TS" : "RTSP", endpoint.text, *reply ? reply : problem);
	return 1;
}

static int halt(void) {
	note(ON_SERVER | FAIL, "not started");
	return 1;
}

int main(int argc, char **argv) {
	static const int signals[] = { SIGHUP, SIGINT, SIGTERM };
	struct sigaction ignore = { .sa_handler = SIG_IGN };
	const char *path = argc > 1 ? argv[1] : CONFIG_PATH;
	unsigned int i;
	struct plan *plan;
	pthread_t thread;
	sigset_t blocked;
	int number;
	if (argc > 1 && !strcmp(argv[1], "--health")) return probe(argc > 2 ? argv[2] : CONFIG_PATH);
	paint();
	sigaction(SIGPIPE, &ignore, NULL);
	sigemptyset(&blocked);
	for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		sigaddset(&blocked, signals[i]);
	}
	pthread_sigmask(SIG_BLOCK, &blocked, NULL);
	headline();
	make_tables();
	plan = read_plan(path, 0);
	if (!plan) return halt();
	memcpy(&config, &plan->config, sizeof(config));
	if (!setup_streams(plan)) return halt();
	srt_startup();
	srt_setloglevel(LOG_CRIT);
	if (!open_srt()) return halt();
	if (!start_workers()) return halt();
	panel();
	if (pthread_create(&thread, NULL, ingest, NULL)) {
		note(ON_SERVER | FAIL, "cannot start the SRT receiver");
		return halt();
	}
	note(ON_SERVER, "ready");
	for (;;) {
		number = sigwaitinfo(&blocked, NULL);
		if (number == SIGHUP) {
			reload(path);
		} else if (number > 0) {
			break;
		}
	}
	atomic_store(&running, 0);
	stop_workers();
	pthread_join(thread, NULL);
	close_srt();
	note(ON_SERVER, "stopped");
	return 0;
}
