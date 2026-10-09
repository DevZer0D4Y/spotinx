/*
 * timers - how long the ways a program has of waiting really take. A browser
 * paces its frames with these: one that takes 40 ms when asked for 16 caps
 * everything at 25 frames a second.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define ROUNDS 40

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int failures;

static void wait_nanosleep(int ms)
{
	struct timespec ts = { ms / 1000, ms % 1000 * 1000000L };

	clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
}

static void wait_poll(int ms) { poll(NULL, 0, ms); }

static void wait_select(int ms)
{
	struct timeval tv = { ms / 1000, ms % 1000 * 1000 };

	select(0, NULL, NULL, NULL, &tv);
}

static int epoll_fd = -1;

static void wait_epoll(int ms)
{
	struct epoll_event event;

	epoll_wait(epoll_fd, &event, 1, ms);
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond;

/* What Chromium's threads sleep with: a futex with a deadline. */
static void wait_cond(int ms)
{
	struct timespec deadline;

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_nsec += ms * 1000000L;
	deadline.tv_sec += deadline.tv_nsec / 1000000000L;
	deadline.tv_nsec %= 1000000000L;
	pthread_mutex_lock(&lock);
	pthread_cond_timedwait(&cond, &lock, &deadline);
	pthread_mutex_unlock(&lock);
}

static void measure(const char *name, void (*wait)(int), int ms)
{
	double total = 0, least = 1e9, most = 0;

	for (int i = 0; i < ROUNDS; i++) {
		double start = now_ms(), took;

		wait(ms);
		took = now_ms() - start;
		total += took;
		if (took < least)
			least = took;
		if (took > most)
			most = took;
	}
	printf("%-12s asked %2d ms: %6.2f on average (%.2f to %.2f)\n", name, ms, total / ROUNDS,
	       least, most);
	/* Early is a bug; late by more than a frame is what we are after. */
	if (total / ROUNDS < ms * 0.95 || total / ROUNDS > ms + 12)
		failures++;
}

static void measure_timerfd(int ms)
{
	struct itimerspec spec = { { 0, ms * 1000000L }, { 0, ms * 1000000L } };
	int fd = timerfd_create(CLOCK_MONOTONIC, 0);
	unsigned long long expirations;
	double start;

	if (fd < 0 || timerfd_settime(fd, 0, &spec, NULL)) {
		printf("timerfd      not available (errno %d)\n", errno);
		failures++;
		return;
	}
	start = now_ms();
	for (int i = 0; i < ROUNDS; i++)
		if (read(fd, &expirations, sizeof(expirations)) != sizeof(expirations))
			break;
	printf("%-12s every %2d ms: %6.2f on average\n", "timerfd", ms, (now_ms() - start) / ROUNDS);
	if ((now_ms() - start) / ROUNDS > ms + 12)
		failures++;
	close(fd);
}

int main(void)
{
	static const int periods[] = { 1, 4, 16 };
	pthread_condattr_t attr;
	struct timespec res;

	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&cond, &attr);
	epoll_fd = epoll_create1(0);
	clock_getres(CLOCK_MONOTONIC, &res);
	printf("clock resolution: %ld ns\n", res.tv_nsec);

	for (unsigned i = 0; i < sizeof(periods) / sizeof(periods[0]); i++) {
		measure("nanosleep", wait_nanosleep, periods[i]);
		measure("poll", wait_poll, periods[i]);
		measure("select", wait_select, periods[i]);
		measure("epoll_wait", wait_epoll, periods[i]);
		measure("cond (futex)", wait_cond, periods[i]);
		measure_timerfd(periods[i]);
	}
	printf("%s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
