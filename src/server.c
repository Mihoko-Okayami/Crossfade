#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "http.h"
#include "log.h"
#include "main.h"
#include "rtsp.h"
#include "server.h"

#define MAX_EVENTS 256
#define FLUSH_INTERVAL 20
#define REST_INTERVAL 500
#define FIRST_REQUEST 5000
#define DEADLINE 15000
#define UNSENT (128 << 10)
#define READ_BUDGET (64 << 10)
#define ACCEPT_BATCH 64
#define MAX_BACKOFF 1000
#define SPARE_FILES 64
#define RESERVE 4
#define MAX_SOURCES 32768
#define STUCK 2000
#define LATE 500

struct worker {
	pthread_t thread;
	int epoll;
	int listeners[2];
	int timer;
	int interval;
	int paused;
	uint64_t now;
	uint64_t resume;
	uint64_t backoff;
	_Atomic uint64_t beat;
	struct viewer *viewers;
};

struct source {
	struct peer peer;
	unsigned int count;
	unsigned int refused;
};

_Atomic uint64_t ingest_beat;
unsigned int viewer_limit;
static atomic_uint viewer_count;
static struct worker workers[MAX_WORKERS];
static unsigned int worker_count;
static struct {
	pthread_mutex_t lock;
	struct source *slots;
	size_t mask;
	size_t used;
	uint64_t seed;
} sources = { .lock = PTHREAD_MUTEX_INITIALIZER };

int queue(struct viewer *viewer, const char *format, ...) {
	size_t room = viewer->out_room - viewer->out_size;
	va_list args;
	int n;
	va_start(args, format);
	n = vsnprintf(viewer->out + viewer->out_size, room, format, args);
	va_end(args);
	if (n < 0 || (size_t)n >= room) return 0;
	viewer->out_size += (size_t)n;
	return 1;
}

const char *field(const char *headers, const char *name) {
	size_t length = strlen(name);
	const char *line;
	for (line = headers; line && (line = strstr(line, "\r\n")); ) {
		line += 2;
		if (!strncasecmp(line, name, length) && line[length] == ':') return line + length + 1 + strspn(line + length + 1, " \t");
	}
	return NULL;
}

void copy_field(char *out, size_t size, const char *value) {
	size_t length = value ? strcspn(value, "\r\n") : 0;
	if (length >= size) {
		length = size - 1;
	}
	if (value && length) {
		memcpy(out, value, length);
	}
	out[length] = '\0';
}

void format_date(char *text, size_t size) {
	time_t now = time(NULL);
	struct tm utc;
	if (!gmtime_r(&now, &utc) || !strftime(text, size, "%a, %b %d %Y %H:%M:%S GMT", &utc)) {
		*text = '\0';
	}
}

void format_host(const struct sockaddr *address, char *text, size_t size) {
	const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)address;
	const struct sockaddr_in *in4 = (const struct sockaddr_in *)address;
	*text = '\0';
	if (address->sa_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
		inet_ntop(AF_INET, in6->sin6_addr.s6_addr + 12, text, (socklen_t)size);
	} else if (address->sa_family == AF_INET6) {
		inet_ntop(AF_INET6, &in6->sin6_addr, text, (socklen_t)size);
	} else if (address->sa_family == AF_INET) {
		inet_ntop(AF_INET, &in4->sin_addr, text, (socklen_t)size);
	} else {
		snprintf(text, size, "unknown");
	}
}

void format_address(const struct sockaddr *address, char *text, size_t size) {
	const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)address;
	const struct sockaddr_in *in4 = (const struct sockaddr_in *)address;
	char host[INET6_ADDRSTRLEN];
	format_host(address, host, sizeof(host));
	if (address->sa_family == AF_INET6) {
		snprintf(text, size, IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr) ? "%s:%d" : "[%s]:%d", host, ntohs(in6->sin6_port));
	} else if (address->sa_family == AF_INET) {
		snprintf(text, size, "%s:%d", host, ntohs(in4->sin_port));
	} else {
		snprintf(text, size, "%s", host);
	}
}

static const unsigned char mapped[12] = { [10] = 0xff, [11] = 0xff };

void peer_of(const struct sockaddr *address, struct peer *peer) {
	const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)address;
	const struct sockaddr_in *in4 = (const struct sockaddr_in *)address;
	memset(peer, 0, sizeof(*peer));
	if (address->sa_family == AF_INET6 && !IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
		memcpy(peer->key, in6->sin6_addr.s6_addr, 8);
		return;
	}
	peer->key[10] = peer->key[11] = 0xff;
	if (address->sa_family == AF_INET6) {
		memcpy(peer->key + 12, in6->sin6_addr.s6_addr + 12, 4);
	} else if (address->sa_family == AF_INET) {
		memcpy(peer->key + 12, &in4->sin_addr, 4);
	}
}

static int local(int fd, const struct sockaddr_storage *peer) {
	struct sockaddr_storage self;
	socklen_t length = sizeof(self);
	if (getsockname(fd, (struct sockaddr *)&self, &length) < 0) return 0;
	if (self.ss_family == AF_INET6) return !memcmp(&((const struct sockaddr_in6 *)&self)->sin6_addr, &((const struct sockaddr_in6 *)peer)->sin6_addr, 16);
	return !memcmp(&((const struct sockaddr_in *)&self)->sin_addr, &((const struct sockaddr_in *)peer)->sin_addr, 4);
}

static int routable(const struct peer *peer) {
	const unsigned char *a = peer->key;
	if (!memcmp(a, mapped, sizeof(mapped))) return a[12] && a[12] != 10 && a[12] != 127 && !(a[12] == 100 && (a[13] & 0xc0) == 64) && !(a[12] == 169 && a[13] == 254) && !(a[12] == 172 && (a[13] & 0xf0) == 16) && !(a[12] == 192 && a[13] == 168);
	return memcmp(a, mapped, 8) && (a[0] & 0xfe) != 0xfc && !(a[0] == 0xfe && (a[1] & 0xc0) == 0x80);
}

static void format_peer(const struct peer *peer, char *text, size_t size) {
	size_t n;
	if (!memcmp(peer->key, mapped, sizeof(mapped))) {
		inet_ntop(AF_INET, peer->key + 12, text, (socklen_t)size);
		return;
	}
	if (!inet_ntop(AF_INET6, peer->key, text, (socklen_t)size)) {
		*text = '\0';
		return;
	}
	n = strlen(text);
	snprintf(text + n, size - n, "/64");
}

static uint64_t mix(uint64_t x) {
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9u;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebu;
	return x ^ (x >> 31);
}

uint64_t peer_hash(const struct peer *peer, uint64_t seed) {
	uint64_t a, b;
	memcpy(&a, peer->key, sizeof(a));
	memcpy(&b, peer->key + 8, sizeof(b));
	return mix(mix(a ^ seed) ^ b);
}

static int taken(size_t i) {
	return sources.slots[i].count || sources.slots[i].refused;
}

static size_t source_slot(const struct peer *peer) {
	size_t i = peer_hash(peer, sources.seed) & sources.mask;
	while (taken(i) && memcmp(&sources.slots[i].peer, peer, sizeof(*peer))) {
		i = (i + 1) & sources.mask;
	}
	return i;
}

static void vacate(size_t i) {
	size_t j, home;
	sources.used--;
	for (j = (i + 1) & sources.mask; taken(j); j = (j + 1) & sources.mask) {
		home = peer_hash(&sources.slots[j].peer, sources.seed) & sources.mask;
		if (((j - home) & sources.mask) < ((j - i) & sources.mask)) continue;
		sources.slots[i] = sources.slots[j];
		i = j;
	}
	sources.slots[i].count = sources.slots[i].refused = 0;
}

static int count_source(struct viewer *viewer) {
	struct source *slot;
	int admitted = 1;
	pthread_mutex_lock(&sources.lock);
	slot = &sources.slots[source_slot(&viewer->peer)];
	if (slot->count >= (unsigned int)config.clients) {
		slot->refused++;
		admitted = 0;
	} else if (slot->count || slot->refused || sources.used < (sources.mask + 1) / 2) {
		if (!slot->count && !slot->refused) {
			slot->peer = viewer->peer;
			sources.used++;
		}
		slot->count++;
		viewer->limited = 1;
	}
	pthread_mutex_unlock(&sources.lock);
	return admitted;
}

static void uncount_source(const struct viewer *viewer) {
	size_t i;
	pthread_mutex_lock(&sources.lock);
	i = source_slot(&viewer->peer);
	if (sources.slots[i].count && !--sources.slots[i].count && !sources.slots[i].refused) {
		vacate(i);
	}
	pthread_mutex_unlock(&sources.lock);
}

void report_refusals(void) {
	char text[INET6_ADDRSTRLEN + 3];
	struct source found;
	size_t i = 0;
	if (!sources.slots) return;
	pthread_mutex_lock(&sources.lock);
	while (i <= sources.mask) {
		found = sources.slots[i];
		if (!found.refused) {
			i++;
			continue;
		}
		sources.slots[i].refused = 0;
		if (found.count) {
			i++;
		} else {
			vacate(i);
		}
		pthread_mutex_unlock(&sources.lock);
		format_peer(&found.peer, text, sizeof(text));
		note(ON_VIEWER | WARN, "%u connection%s from %s refused in the last 10 s, [general] clients allows %d per address", found.refused, found.refused > 1 ? "s" : "", text, config.clients);
		pthread_mutex_lock(&sources.lock);
	}
	pthread_mutex_unlock(&sources.lock);
}

static int open_sources(void) {
	size_t size = 1, expected = viewer_limit < MAX_SOURCES ? viewer_limit : MAX_SOURCES;
	while (size < 2 * expected) {
		size <<= 1;
	}
	sources.slots = calloc(size, sizeof(*sources.slots));
	if (!sources.slots || getrandom(&sources.seed, sizeof(sources.seed), 0) != (ssize_t)sizeof(sources.seed)) return 0;
	sources.mask = size - 1;
	return 1;
}

void start(struct viewer *viewer, struct ring *ring, const struct mark *mark) {
	viewer->ring = ring;
	viewer->pos = mark->at;
	viewer->stage = STREAM;
	viewer->jump = 0;
}

int choose(const struct viewer *viewer, struct ring *ring, int backlog, struct mark *mark, unsigned int *epoch) {
	struct stream *stream = viewer->stream;
	int found = backlog == LIVE_EDGE ? ring_head(ring, mark, epoch) : ring_find(ring, backlog, mark, epoch);
	return found && atomic_load(&stream->version) == viewer->version && on_air(stream);
}

static int rejoin(struct viewer *viewer, uint64_t head) {
	unsigned int epoch;
	if (atomic_load(&viewer->ring->onset_at) <= viewer->pos) return 0;
	ring_onset(viewer->ring, &viewer->target, &epoch);
	return epoch == viewer->epoch && viewer->target.at > viewer->pos && viewer->target.at < head && viewer->target.time > viewer->since && msec() - viewer->target.time >= LATE;
}

void catch_up(struct viewer *viewer, uint64_t head) {
	unsigned int epoch;
	if (viewer->jump) return;
	if ((head - viewer->pos <= ring_limit(viewer->ring) || !choose(viewer, viewer->ring, 0, &viewer->target, &epoch) || epoch != viewer->epoch || viewer->target.at <= viewer->pos + ring_limit(viewer->ring) / 2) && !rejoin(viewer, head)) return;
	viewer->jump = 1;
	atomic_fetch_add(&viewer->stream->lagging, 1);
}

int overrun(const struct viewer *viewer) {
	return atomic_load_explicit(&viewer->ring->head, memory_order_acquire) - viewer->pos > viewer->ring->size / 2;
}

ssize_t transmit(struct viewer *viewer, struct iovec *parts, size_t count, uint64_t now) {
	struct msghdr message = { .msg_iov = parts, .msg_iovlen = count };
	ssize_t n;
	do {
		n = sendmsg(viewer->fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
	} while (n < 0 && errno == EINTR);
	if (n > 0) {
		viewer->moved = now;
	}
	if (n >= 0 || errno != EAGAIN) return n;
	viewer->writable = 0;
	return 0;
}

int drain(struct viewer *viewer, uint64_t now) {
	struct iovec part = { viewer->out + viewer->out_sent, viewer->out_size - viewer->out_sent };
	ssize_t n = transmit(viewer, &part, 1, now);
	if (n < 0) return -1;
	viewer->out_sent += (size_t)n;
	if (viewer->out_sent == viewer->out_size) {
		viewer->out_sent = viewer->out_size = 0;
	}
	return 0;
}

int admit_source(struct viewer *viewer) {
	return !config.clients || !routable(&viewer->peer) || count_source(viewer);
}

static void turn_away(int fd) {
	static const struct linger reset = { .l_onoff = 1, .l_linger = 0 };
	setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
	close(fd);
}

static void drop(struct worker *worker, struct viewer *viewer, int abort) {
	if (abort) {
		turn_away(viewer->fd);
	} else {
		close(viewer->fd);
	}
	if (viewer->limited) {
		uncount_source(viewer);
	}
	atomic_fetch_sub(&viewer_count, 1);
	if (worker->viewers == viewer) {
		worker->viewers = viewer->next;
	} else if (viewer->prev) {
		viewer->prev->next = viewer->next;
	}
	if (viewer->next) {
		viewer->next->prev = viewer->prev;
	}
	free(viewer);
}

static void step(struct worker *worker, struct viewer *viewer) {
	int result = viewer->protocol == RTSP ? rtsp_flush(viewer, worker->now) : http_flush(viewer, worker->now);
	if (result != KEEP) {
		drop(worker, viewer, result == ABORT);
	}
}

static int receive(struct worker *worker, struct viewer *viewer) {
	size_t taken = 0;
	ssize_t n;
	viewer->readable = 0;
	for (;;) {
		if (viewer->closing) {
			viewer->used = 0;
		}
		if (viewer->used == viewer->in_room) {
			drop(worker, viewer, 1);
			return -1;
		}
		if (taken >= READ_BUDGET) {
			viewer->readable = 1;
			return 0;
		}
		n = recv(viewer->fd, viewer->in + viewer->used, viewer->in_room - viewer->used, 0);
		if (n < 0 && errno == EINTR) continue;
		if (n < 0 && errno == EAGAIN) return 0;
		if (n < 0) {
			drop(worker, viewer, 0);
			return -1;
		}
		if (!n) {
			if (viewer->stage == REQUEST) {
				viewer->closing = 1;
			}
			return 0;
		}
		viewer->used += (size_t)n;
		taken += (size_t)n;
		if ((viewer->protocol == RTSP ? rtsp_input(viewer, worker->now) : http_input(viewer, worker->now)) < 0) {
			drop(worker, viewer, 1);
			return -1;
		}
	}
}

static void handle(struct worker *worker, struct viewer *viewer, uint32_t events) {
	if (events & (EPOLLERR | EPOLLHUP)) {
		drop(worker, viewer, 0);
		return;
	}
	if ((events & EPOLLIN) && receive(worker, viewer) < 0) return;
	if (events & EPOLLOUT) {
		viewer->writable = 1;
	}
	step(worker, viewer);
}

static void tune(int fd, enum protocol protocol) {
	static const int one = 1, unsent = UNSENT, idle = 30, interval = 10, count = 3;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &unsent, sizeof(unsent));
	if (protocol != RTSP) return;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
}

static void stop_listening(struct worker *worker) {
	int protocol;
	for (protocol = HTTP; protocol <= RTSP; protocol++) {
		if (worker->listeners[protocol] >= 0) {
			epoll_ctl(worker->epoll, EPOLL_CTL_DEL, worker->listeners[protocol], NULL);
		}
	}
	worker->paused = 1;
	worker->resume = worker->now + worker->backoff;
	worker->backoff = worker->backoff * 2 < MAX_BACKOFF ? worker->backoff * 2 : MAX_BACKOFF;
}

static void accept_viewers(struct worker *worker, enum protocol protocol) {
	struct epoll_event event = { .events = EPOLLIN | EPOLLOUT | EPOLLET };
	struct sockaddr_storage address;
	struct viewer *viewer;
	struct peer peer;
	socklen_t length;
	unsigned int seat;
	int fd, round;
	for (round = 0; round < ACCEPT_BATCH && !worker->paused; round++) {
		length = sizeof(address);
		fd = accept4(worker->listeners[protocol], (struct sockaddr *)&address, &length, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0 && (errno == EINTR || errno == ECONNABORTED || errno == EPROTO || errno == ENETDOWN || errno == ENETUNREACH || errno == EHOSTDOWN || errno == EHOSTUNREACH || errno == ENONET || errno == ENOPROTOOPT || errno == EOPNOTSUPP)) continue;
		if (fd < 0 && errno != EAGAIN) {
			stop_listening(worker);
			tell(UNACCEPTED, "cannot accept viewers: %s", strerror(errno));
		}
		if (fd < 0) return;
		worker->backoff = FLUSH_INTERVAL;
		peer_of((const struct sockaddr *)&address, &peer);
		seat = atomic_fetch_add(&viewer_count, 1);
		if (seat >= viewer_limit && (seat >= viewer_limit + RESERVE || !local(fd, &address))) {
			atomic_fetch_sub(&viewer_count, 1);
			turn_away(fd);
			tell(FULL, "the server cannot accept more viewers: %u connected, the most the open file limit allows, raise it (ulimits nofile in docker-compose.yml)", viewer_limit);
			continue;
		}
		viewer = protocol == RTSP ? rtsp_new() : http_new();
		if (!viewer) {
			atomic_fetch_sub(&viewer_count, 1);
			close(fd);
			tell(STARVED, atomic_load(&contained) ? "the server cannot accept more viewers: out of memory, raise the container's memory limit (mem_limit and memswap_limit in docker-compose.yml)" : "the server cannot accept more viewers: out of memory, lower [general] buffer and restart");
			continue;
		}
		viewer->peer = peer;
		viewer->spare = seat >= viewer_limit;
		if (protocol == RTSP && !admit_source(viewer)) {
			atomic_fetch_sub(&viewer_count, 1);
			turn_away(fd);
			free(viewer);
			continue;
		}
		tune(fd, protocol);
		viewer->fd = fd;
		viewer->protocol = protocol;
		viewer->writable = 1;
		viewer->since = viewer->moved = worker->now;
		viewer->prev = NULL;
		viewer->next = worker->viewers;
		if (viewer->next) {
			viewer->next->prev = viewer;
		}
		worker->viewers = viewer;
		event.data.ptr = viewer;
		if (epoll_ctl(worker->epoll, EPOLL_CTL_ADD, fd, &event) < 0) {
			drop(worker, viewer, 1);
		}
	}
}

static void listen_again(struct worker *worker) {
	struct epoll_event event = { .events = EPOLLIN };
	int protocol;
	worker->paused = 0;
	for (protocol = HTTP; protocol <= RTSP; protocol++) {
		if (worker->listeners[protocol] < 0) continue;
		event.data.ptr = &worker->listeners[protocol];
		if (epoll_ctl(worker->epoll, EPOLL_CTL_ADD, worker->listeners[protocol], &event) < 0 && errno != EEXIST) {
			stop_listening(worker);
			return;
		}
	}
}

static void sweep(struct worker *worker) {
	struct viewer *viewer, *next;
	uint64_t now = msec();
	int expired;
	atomic_store(&worker->beat, now);
	if (worker->paused && now >= worker->resume) {
		listen_again(worker);
	}
	for (viewer = worker->viewers; viewer; viewer = next) {
		next = viewer->next;
		if (viewer->readable && receive(worker, viewer) < 0) continue;
		expired = now - viewer->asked >= (uint64_t)config.wait * 1000;
		if (viewer->stage == WAIT && !viewer->closing && viewer->protocol == RTSP) {
			rtsp_retry(viewer, expired);
		} else if (viewer->stage == WAIT && !viewer->closing) {
			http_retry(viewer, expired);
		}
		if (viewer->stage == STREAM ? (viewer->protocol == RTSP ? rtsp_idle(viewer, now) : http_idle(viewer, now)) : now - viewer->since >= (viewer->asked ? DEADLINE : FIRST_REQUEST)) {
			drop(worker, viewer, 1);
		} else {
			step(worker, viewer);
		}
	}
}

int healthy(void) {
	uint64_t now = msec(), beat;
	unsigned int i;
	for (i = 0; i < worker_count; i++) {
		beat = atomic_load(&workers[i].beat);
		if ((int64_t)(now - beat) < STUCK) continue;
		tell(STALLED, "health check failed: worker %u last ran %lld ms ago", i + 1, (long long)(now - beat));
		return 0;
	}
	beat = atomic_load(&ingest_beat);
	if ((int64_t)(now - beat) < STUCK) return 1;
	tell(STALLED, "health check failed: the SRT receiver last ran %lld ms ago", (long long)(now - beat));
	return 0;
}

static void pace(struct worker *worker, int swept) {
	int interval = worker->viewers || worker->paused ? FLUSH_INTERVAL : swept ? REST_INTERVAL : worker->interval;
	struct itimerspec period = { .it_interval.tv_nsec = interval * 1000000L, .it_value.tv_nsec = interval * 1000000L };
	if (interval != worker->interval && timerfd_settime(worker->timer, 0, &period, NULL) == 0) {
		worker->interval = interval;
	}
}

static void *serve(void *argument) {
	struct worker *worker = argument;
	struct epoll_event events[MAX_EVENTS];
	uint64_t ticks;
	int n, i, swept;
	while (atomic_load(&running)) {
		n = epoll_wait(worker->epoll, events, MAX_EVENTS, -1);
		worker->now = msec();
		swept = 0;
		for (i = 0; i < n; i++) {
			if (events[i].data.ptr == &worker->listeners[HTTP]) {
				accept_viewers(worker, HTTP);
			} else if (events[i].data.ptr == &worker->listeners[RTSP]) {
				accept_viewers(worker, RTSP);
			} else if (events[i].data.ptr == &worker->timer) {
				swept = read(worker->timer, &ticks, sizeof(ticks)) > 0;
			} else {
				handle(worker, events[i].data.ptr, events[i].events);
			}
		}
		if (swept) {
			sweep(worker);
		}
		pace(worker, swept);
	}
	while (worker->viewers) {
		drop(worker, worker->viewers, 1);
	}
	return NULL;
}

static int open_listener(const struct endpoint *endpoint, int shared) {
	int fd = socket(endpoint->address.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0), one = 1, zero = 0, error;
	if (fd < 0) return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &shared, sizeof(shared));
	if (endpoint->address.ss_family == AF_INET6) {
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
	}
	if (bind(fd, (const struct sockaddr *)&endpoint->address, endpoint->length) == 0 && listen(fd, SOMAXCONN) == 0) return fd;
	error = errno;
	close(fd);
	errno = error;
	return -1;
}

static int listener(const struct endpoint *endpoint, int first) {
	int fd = -1;
	if (first && (fd = open_listener(endpoint, 0)) < 0) return -1;
	if (first) {
		close(fd);
	}
	return open_listener(endpoint, 1);
}

static void cannot_listen(const char *section, const struct endpoint *endpoint) {
	int error = errno;
	note(ON_SERVER | FAIL, "[%s] listen %s: %s%s", section, endpoint->text, strerror(error), error == EACCES ? ", ports below 1024 need the NET_BIND_SERVICE capability" : "");
}

static int start_worker(struct worker *worker) {
	struct itimerspec period = { .it_interval.tv_nsec = FLUSH_INTERVAL * 1000000, .it_value.tv_nsec = FLUSH_INTERVAL * 1000000 };
	struct epoll_event event = { .events = EPOLLIN };
	worker->backoff = worker->interval = FLUSH_INTERVAL;
	worker->listeners[HTTP] = config.ts_enabled ? listener(&config.ts, worker == workers) : -1;
	if (config.ts_enabled && worker->listeners[HTTP] < 0) {
		cannot_listen("ts", &config.ts);
		return 0;
	}
	worker->listeners[RTSP] = config.rtsp_enabled ? listener(&config.rtsp, worker == workers) : -1;
	if (config.rtsp_enabled && worker->listeners[RTSP] < 0) {
		cannot_listen("rtsp", &config.rtsp);
		return 0;
	}
	worker->epoll = epoll_create1(EPOLL_CLOEXEC);
	worker->timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	event.data.ptr = &worker->timer;
	if (worker->epoll < 0 || worker->timer < 0 || timerfd_settime(worker->timer, 0, &period, NULL) < 0 || epoll_ctl(worker->epoll, EPOLL_CTL_ADD, worker->timer, &event) < 0) {
		note(ON_SERVER | FAIL, "cannot start a worker: %s", strerror(errno));
		return 0;
	}
	listen_again(worker);
	if (pthread_create(&worker->thread, NULL, serve, worker)) {
		note(ON_SERVER | FAIL, "cannot start a worker");
		return 0;
	}
	return 1;
}

static long read_long(const char *path) {
	FILE *file = fopen(path, "r");
	long value = -1;
	if (file && fscanf(file, "%ld", &value) != 1) {
		value = -1;
	}
	if (file) {
		fclose(file);
	}
	return value;
}

static unsigned int cpus(void) {
	cpu_set_t set;
	long quota = -1, period = -1;
	FILE *limit = fopen("/sys/fs/cgroup/cpu.max", "r");
	int n = sched_getaffinity(0, sizeof(set), &set) == 0 ? CPU_COUNT(&set) : (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (limit) {
		if (fscanf(limit, "%ld %ld", &quota, &period) != 2) {
			quota = -1;
		}
		fclose(limit);
	} else {
		quota = read_long("/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
		period = read_long("/sys/fs/cgroup/cpu/cpu.cfs_period_us");
	}
	if (quota > 0 && period > 0 && (quota + period - 1) / period < n) {
		n = (int)((quota + period - 1) / period);
	}
	return n < 1 ? 1 : n > MAX_WORKERS ? MAX_WORKERS : (unsigned int)n;
}

static unsigned int seats(void) {
	uint64_t spare = SPARE_FILES + 4 * (uint64_t)worker_count, room;
	struct rlimit files;
	if (getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur < MAX_SOURCES) {
		files.rlim_cur = files.rlim_max < MAX_SOURCES ? files.rlim_max : MAX_SOURCES;
		setrlimit(RLIMIT_NOFILE, &files);
	}
	if (getrlimit(RLIMIT_NOFILE, &files) < 0 || files.rlim_cur == RLIM_INFINITY) return UINT_MAX / 2;
	room = files.rlim_cur > spare ? files.rlim_cur - spare : 0;
	return room < UINT_MAX / 2 ? (unsigned int)room : UINT_MAX / 2;
}

unsigned int start_workers(void) {
	unsigned int i;
	worker_count = config.workers ? (unsigned int)config.workers : cpus();
	viewer_limit = seats();
	if (!viewer_limit) {
		note(ON_SERVER | FAIL, "the open file limit leaves no room for viewers, raise it (ulimits nofile in docker-compose.yml)");
		return 0;
	}
	if (config.clients && !open_sources()) {
		note(ON_SERVER | FAIL, "cannot start the workers: out of memory");
		return 0;
	}
	for (i = 0; i < worker_count; i++) {
		if (!start_worker(&workers[i])) return 0;
	}
	return worker_count;
}

void stop_workers(void) {
	unsigned int i;
	for (i = 0; i < worker_count; i++) {
		pthread_join(workers[i].thread, NULL);
	}
}
