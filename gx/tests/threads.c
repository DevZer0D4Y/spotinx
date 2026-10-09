/*
 * threads - 8 pthreads fighting over a mutex, a condition variable used as a
 * start gate, thread-local storage and a pthread_cond_timedwait that must
 * time out.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define THREADS 8
#define ROUNDS 20000

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t ready_cond = PTHREAD_COND_INITIALIZER;
static int ready, go;

static __thread long tls_value = 42;
static __thread char tls_buffer[64];

static int failures;

static void fail(const char *what)
{
	printf("FAIL: %s\n", what);
	__atomic_add_fetch(&failures, 1, __ATOMIC_SEQ_CST);
}

static void *worker(void *arg)
{
	long id = (long)arg;
	void *block;

	if (tls_value != 42)
		fail("TLS initial value");
	tls_value = id * 1000;
	snprintf(tls_buffer, sizeof(tls_buffer), "thread-%ld", id);

	pthread_mutex_lock(&gate_lock);
	ready++;
	pthread_cond_signal(&ready_cond);
	while (!go)
		pthread_cond_wait(&gate_cond, &gate_lock);
	pthread_mutex_unlock(&gate_lock);

	for (int i = 0; i < ROUNDS; i++) {
		pthread_mutex_lock(&counter_lock);
		counter++;
		pthread_mutex_unlock(&counter_lock);
		tls_value++;
		if (i % 4096 == 0) {
			block = malloc(100 + id);
			if (!block)
				fail("malloc in a thread");
			free(block);
		}
	}

	if (tls_value != id * 1000 + ROUNDS)
		fail("TLS value after the loop");
	{
		char expected[64];

		snprintf(expected, sizeof(expected), "thread-%ld", id);
		for (int i = 0; expected[i] || tls_buffer[i]; i++) {
			if (expected[i] != tls_buffer[i]) {
				fail("TLS buffer");
				break;
			}
		}
	}
	return (void *)(id + 1);
}

static long elapsed_ms(const struct timespec *a, const struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) * 1000 + (b->tv_nsec - a->tv_nsec) / 1000000;
}

int main(void)
{
	pthread_t threads[THREADS];
	struct timespec start, deadline, end;
	int ret;

	tls_value = -1;
	for (long i = 0; i < THREADS; i++) {
		if (pthread_create(&threads[i], NULL, worker, (void *)i)) {
			printf("FAIL: pthread_create %ld\n", i);
			return 1;
		}
	}

	/* Wait until every thread is parked on the gate, then open it. */
	pthread_mutex_lock(&gate_lock);
	while (ready < THREADS)
		pthread_cond_wait(&ready_cond, &gate_lock);
	go = 1;
	pthread_cond_broadcast(&gate_cond);
	pthread_mutex_unlock(&gate_lock);

	for (long i = 0; i < THREADS; i++) {
		void *result = NULL;

		if (pthread_join(threads[i], &result))
			fail("pthread_join");
		if ((long)result != i + 1)
			fail("thread return value");
	}
	if (counter != (long)THREADS * ROUNDS) {
		printf("counter is %ld, expected %ld\n", counter, (long)THREADS * ROUNDS);
		fail("contended mutex");
	}
	if (tls_value != -1)
		fail("TLS of the main thread");

	/* Nobody signals this one: it has to time out, and not too early. */
	pthread_mutex_lock(&gate_lock);
	clock_gettime(CLOCK_REALTIME, &start);
	deadline = start;
	deadline.tv_nsec += 200 * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	ret = pthread_cond_timedwait(&ready_cond, &gate_lock, &deadline);
	clock_gettime(CLOCK_REALTIME, &end);
	pthread_mutex_unlock(&gate_lock);
	if (ret != ETIMEDOUT) {
		printf("pthread_cond_timedwait returned %d\n", ret);
		fail("pthread_cond_timedwait result");
	}
	if (elapsed_ms(&start, &end) < 190 || elapsed_ms(&start, &end) > 5000) {
		printf("pthread_cond_timedwait took %ld ms\n", elapsed_ms(&start, &end));
		fail("pthread_cond_timedwait duration");
	}

	printf("threads: counter %ld, timedwait %ld ms\n", counter, elapsed_ms(&start, &end));
	printf("%s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
