// SPDX-License-Identifier: GPL-2.0
/*
 * msgring_test - functional and stress tests for the msgring driver.
 *
 * Output is TAP (Test Anything Protocol). Exit status is 0 only if every
 * test passed.
 *
 * Usage: msgring_test [device]     (default /dev/msgring0)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "msgring.h"

static const char *dev_path = "/dev/msgring0";

/* Fail the current test, reporting where and why. */
#define CHECK(cond)							\
	do {								\
		if (!(cond)) {						\
			printf("# %s:%d: check failed: %s (errno=%d %s)\n", \
			       __FILE__, __LINE__, #cond, errno,	\
			       strerror(errno));			\
			return -1;					\
		}							\
	} while (0)

#define CHECK_ERRNO(expr, err)						\
	do {								\
		errno = 0;						\
		CHECK((expr) == -1);					\
		CHECK(errno == (err));					\
	} while (0)

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Open the device and bring it to a known state: empty, default capacity. */
static int open_clean(int flags)
{
	uint32_t cap = MSGRING_DEFAULT_CAPACITY;
	int fd = open(dev_path, flags);

	if (fd < 0)
		return -1;
	if (ioctl(fd, MSGRING_IOC_FLUSH) ||
	    ioctl(fd, MSGRING_IOC_SET_CAPACITY, &cap)) {
		close(fd);
		return -1;
	}
	return fd;
}

static int set_timeout(int fd, uint32_t ms)
{
	return ioctl(fd, MSGRING_IOC_SET_TIMEOUT, &ms);
}

/* ---------------------------------------------------------------- tests */

static int test_open_close(void)
{
	int fd = open(dev_path, O_RDWR);

	CHECK(fd >= 0);
	CHECK(close(fd) == 0);
	return 0;
}

static int test_roundtrip(void)
{
	char buf[64] = { 0 };
	int fd = open_clean(O_RDWR);

	CHECK(fd >= 0);
	CHECK(write(fd, "hello", 5) == 5);
	CHECK(read(fd, buf, sizeof(buf)) == 5);
	CHECK(memcmp(buf, "hello", 5) == 0);
	close(fd);
	return 0;
}

/* Three writes must come back as three reads, in order, never merged. */
static int test_boundaries_and_fifo(void)
{
	char buf[64];
	int fd = open_clean(O_RDWR);

	CHECK(fd >= 0);
	CHECK(write(fd, "a", 1) == 1);
	CHECK(write(fd, "bb", 2) == 2);
	CHECK(write(fd, "ccc", 3) == 3);

	CHECK(read(fd, buf, sizeof(buf)) == 1 && buf[0] == 'a');
	CHECK(read(fd, buf, sizeof(buf)) == 2 && !memcmp(buf, "bb", 2));
	CHECK(read(fd, buf, sizeof(buf)) == 3 && !memcmp(buf, "ccc", 3));
	close(fd);
	return 0;
}

/* Messages outlive the file descriptor that wrote them. */
static int test_persist_across_open(void)
{
	char buf[8];
	int fd = open_clean(O_RDWR);

	CHECK(fd >= 0);
	CHECK(write(fd, "keep", 4) == 4);
	close(fd);

	fd = open(dev_path, O_RDONLY | O_NONBLOCK);
	CHECK(fd >= 0);
	CHECK(read(fd, buf, sizeof(buf)) == 4 && !memcmp(buf, "keep", 4));
	close(fd);
	return 0;
}

static int test_nonblock_empty(void)
{
	char buf[8];
	int fd = open_clean(O_RDWR | O_NONBLOCK);

	CHECK(fd >= 0);
	CHECK_ERRNO(read(fd, buf, sizeof(buf)), EAGAIN);
	close(fd);
	return 0;
}

static int test_nonblock_full(void)
{
	uint32_t cap = 4;
	int fd = open_clean(O_RDWR | O_NONBLOCK);
	int i;

	CHECK(fd >= 0);
	CHECK(ioctl(fd, MSGRING_IOC_SET_CAPACITY, &cap) == 0);
	for (i = 0; i < 4; i++)
		CHECK(write(fd, "x", 1) == 1);
	CHECK_ERRNO(write(fd, "x", 1), EAGAIN);
	close(fd);
	return 0;
}

static int test_size_limits(void)
{
	static char big[MSGRING_MAX_MSG_SIZE + 1];
	static char out[MSGRING_MAX_MSG_SIZE];
	char small[2];
	int fd = open_clean(O_RDWR | O_NONBLOCK);

	CHECK(fd >= 0);
	CHECK_ERRNO(write(fd, big, sizeof(big)), EMSGSIZE);
	CHECK_ERRNO(write(fd, big, 0), EINVAL);

	/* The maximum size itself is accepted and returned intact. */
	memset(big, 0x5a, MSGRING_MAX_MSG_SIZE);
	CHECK(write(fd, big, MSGRING_MAX_MSG_SIZE) == MSGRING_MAX_MSG_SIZE);
	CHECK(read(fd, out, sizeof(out)) == MSGRING_MAX_MSG_SIZE);
	CHECK(memcmp(big, out, MSGRING_MAX_MSG_SIZE) == 0);

	/* A too-small read buffer fails and leaves the message queued. */
	CHECK(write(fd, "abcd", 4) == 4);
	CHECK_ERRNO(read(fd, small, sizeof(small)), EMSGSIZE);
	CHECK(read(fd, out, sizeof(out)) == 4 && !memcmp(out, "abcd", 4));
	close(fd);
	return 0;
}

/* A bad user pointer must fail cleanly and not lose the message. */
static int test_bad_pointer(void)
{
	char buf[8];
	/* An inaccessible page: any kernel copy to or from it must fault. */
	void *bad = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
			 -1, 0);
	int fd = open_clean(O_RDWR | O_NONBLOCK);

	CHECK(fd >= 0 && bad != MAP_FAILED);
	CHECK_ERRNO(write(fd, bad, 4), EFAULT);
	CHECK(write(fd, "safe", 4) == 4);
	CHECK_ERRNO(read(fd, bad, 8), EFAULT);
	CHECK(read(fd, buf, sizeof(buf)) == 4 && !memcmp(buf, "safe", 4));
	munmap(bad, 4096);
	close(fd);
	return 0;
}

static int test_poll(void)
{
	struct pollfd pfd = { .events = POLLIN | POLLOUT };
	uint32_t cap = 1;
	char buf[8];
	int fd = open_clean(O_RDWR | O_NONBLOCK);

	CHECK(fd >= 0);
	pfd.fd = fd;
	CHECK(ioctl(fd, MSGRING_IOC_SET_CAPACITY, &cap) == 0);

	/* empty: writable only */
	CHECK(poll(&pfd, 1, 0) == 1);
	CHECK(pfd.revents == POLLOUT);

	/* full (capacity 1): readable only */
	CHECK(write(fd, "x", 1) == 1);
	CHECK(poll(&pfd, 1, 0) == 1);
	CHECK(pfd.revents == POLLIN);

	/* drained again: writable only */
	CHECK(read(fd, buf, sizeof(buf)) == 1);
	CHECK(poll(&pfd, 1, 0) == 1);
	CHECK(pfd.revents == POLLOUT);
	close(fd);
	return 0;
}

static void *delayed_writer(void *arg)
{
	int fd = *(int *)arg;

	usleep(100 * 1000);
	if (write(fd, "wake", 4) != 4)
		return (void *)1;
	return NULL;
}

/* A sleeping reader is woken by a writer in another thread. */
static int test_blocking_read_wakeup(void)
{
	pthread_t thr;
	void *res;
	char buf[8];
	int64_t t0;
	int fd = open_clean(O_RDWR);

	CHECK(fd >= 0);
	CHECK(set_timeout(fd, 10000) == 0);	/* safety net, not expected */
	CHECK(pthread_create(&thr, NULL, delayed_writer, &fd) == 0);

	t0 = now_ms();
	CHECK(read(fd, buf, sizeof(buf)) == 4 && !memcmp(buf, "wake", 4));
	CHECK(now_ms() - t0 >= 50);		/* it really did block */

	CHECK(pthread_join(thr, &res) == 0 && res == NULL);
	close(fd);
	return 0;
}

static void *delayed_reader(void *arg)
{
	int fd = *(int *)arg;
	char buf[8];

	usleep(100 * 1000);
	if (read(fd, buf, sizeof(buf)) != 1)
		return (void *)1;
	return NULL;
}

/* A writer blocked on a full queue is woken when a reader makes room. */
static int test_blocking_write_wakeup(void)
{
	uint32_t cap = 1;
	pthread_t thr;
	void *res;
	int64_t t0;
	int fd = open_clean(O_RDWR);

	CHECK(fd >= 0);
	CHECK(ioctl(fd, MSGRING_IOC_SET_CAPACITY, &cap) == 0);
	CHECK(set_timeout(fd, 10000) == 0);
	CHECK(write(fd, "1", 1) == 1);
	CHECK(pthread_create(&thr, NULL, delayed_reader, &fd) == 0);

	t0 = now_ms();
	CHECK(write(fd, "2", 1) == 1);
	CHECK(now_ms() - t0 >= 50);

	CHECK(pthread_join(thr, &res) == 0 && res == NULL);
	close(fd);
	return 0;
}

static int test_timeout(void)
{
	uint32_t cap = 1;
	char buf[8];
	int64_t t0, dt;
	int fd = open_clean(O_RDWR);

	CHECK(fd >= 0);
	CHECK(set_timeout(fd, 200) == 0);

	t0 = now_ms();
	CHECK_ERRNO(read(fd, buf, sizeof(buf)), ETIMEDOUT);
	dt = now_ms() - t0;
	CHECK(dt >= 180 && dt < 5000);

	/* The same timeout applies to a writer facing a full queue. */
	CHECK(ioctl(fd, MSGRING_IOC_SET_CAPACITY, &cap) == 0);
	CHECK(write(fd, "x", 1) == 1);
	t0 = now_ms();
	CHECK_ERRNO(write(fd, "y", 1), ETIMEDOUT);
	dt = now_ms() - t0;
	CHECK(dt >= 180 && dt < 5000);
	close(fd);
	return 0;
}

/* The timeout belongs to the open file, not to the device. */
static int test_timeout_is_per_open(void)
{
	char buf[8];
	int64_t t0;
	int fd1 = open_clean(O_RDWR);
	int fd2 = open(dev_path, O_RDWR);

	CHECK(fd1 >= 0 && fd2 >= 0);
	CHECK(set_timeout(fd1, 5000) == 0);
	CHECK(set_timeout(fd2, 100) == 0);

	t0 = now_ms();
	CHECK_ERRNO(read(fd2, buf, sizeof(buf)), ETIMEDOUT);
	CHECK(now_ms() - t0 < 3000);
	close(fd1);
	close(fd2);
	return 0;
}

static void on_alarm(int sig)
{
	(void)sig;
}

/* A signal interrupts a blocked reader with EINTR. */
static int test_signal_interrupts(void)
{
	struct sigaction sa = { .sa_handler = on_alarm };	/* no SA_RESTART */
	struct itimerval it = { .it_value = { .tv_usec = 100 * 1000 } };
	char buf[8];
	int fd = open_clean(O_RDWR);

	CHECK(fd >= 0);
	CHECK(sigaction(SIGALRM, &sa, NULL) == 0);
	CHECK(setitimer(ITIMER_REAL, &it, NULL) == 0);
	CHECK_ERRNO(read(fd, buf, sizeof(buf)), EINTR);
	close(fd);
	return 0;
}

static int test_flush_and_stats(void)
{
	struct msgring_stats before, after;
	char buf[8];
	int fd = open_clean(O_RDWR | O_NONBLOCK);

	CHECK(fd >= 0);
	CHECK(ioctl(fd, MSGRING_IOC_GET_STATS, &before) == 0);
	CHECK(before.depth == 0);
	CHECK(before.capacity == MSGRING_DEFAULT_CAPACITY);

	CHECK(write(fd, "12345", 5) == 5);
	CHECK(write(fd, "678", 3) == 3);
	CHECK(write(fd, "9", 1) == 1);
	CHECK(read(fd, buf, sizeof(buf)) == 5);

	CHECK(ioctl(fd, MSGRING_IOC_GET_STATS, &after) == 0);
	CHECK(after.depth == 2);
	CHECK(after.enqueued - before.enqueued == 3);
	CHECK(after.dequeued - before.dequeued == 1);
	CHECK(after.bytes_in - before.bytes_in == 9);
	CHECK(after.bytes_out - before.bytes_out == 5);
	CHECK(after.high_watermark >= 3);

	CHECK(ioctl(fd, MSGRING_IOC_FLUSH) == 0);
	CHECK(ioctl(fd, MSGRING_IOC_GET_STATS, &after) == 0);
	CHECK(after.depth == 0);
	CHECK(after.flushed - before.flushed == 2);
	CHECK_ERRNO(read(fd, buf, sizeof(buf)), EAGAIN);
	close(fd);
	return 0;
}

static int test_capacity_ioctl(void)
{
	struct msgring_stats st;
	uint32_t v;
	int fd = open_clean(O_RDWR | O_NONBLOCK);

	CHECK(fd >= 0);
	v = 0;
	CHECK_ERRNO(ioctl(fd, MSGRING_IOC_SET_CAPACITY, &v), EINVAL);
	v = MSGRING_MAX_CAPACITY + 1;
	CHECK_ERRNO(ioctl(fd, MSGRING_IOC_SET_CAPACITY, &v), EINVAL);

	/* Shrinking below the current depth would strand messages. */
	CHECK(write(fd, "a", 1) == 1);
	CHECK(write(fd, "b", 1) == 1);
	v = 1;
	CHECK_ERRNO(ioctl(fd, MSGRING_IOC_SET_CAPACITY, &v), EBUSY);

	v = 2;
	CHECK(ioctl(fd, MSGRING_IOC_SET_CAPACITY, &v) == 0);
	CHECK(ioctl(fd, MSGRING_IOC_GET_STATS, &st) == 0);
	CHECK(st.capacity == 2);
	CHECK_ERRNO(write(fd, "c", 1), EAGAIN);

	/* Growing makes room immediately. */
	v = 3;
	CHECK(ioctl(fd, MSGRING_IOC_SET_CAPACITY, &v) == 0);
	CHECK(write(fd, "c", 1) == 1);
	close(fd);
	return 0;
}

static int test_bad_ioctl(void)
{
	int fd = open_clean(O_RDWR);

	CHECK(fd >= 0);
	CHECK_ERRNO(ioctl(fd, _IO(MSGRING_IOC_MAGIC, 99)), ENOTTY);
	CHECK_ERRNO(ioctl(fd, MSGRING_IOC_GET_STATS, NULL), EFAULT);
	close(fd);
	return 0;
}

/* --------------------------------------------------------------- stress
 *
 * Several writers and readers hammer one small queue. Every message carries
 * (writer id, sequence number, checksummed payload). We verify:
 *   - no message is lost or delivered twice,
 *   - payloads arrive uncorrupted,
 *   - each reader sees any one writer's messages in increasing order (FIFO).
 */
#define STRESS_WRITERS	4
#define STRESS_READERS	4
#define STRESS_MSGS	1000		/* per writer */
#define STRESS_TOTAL	(STRESS_WRITERS * STRESS_MSGS)

struct stress_msg {
	uint32_t writer;
	uint32_t seq;
	uint32_t len;			/* payload bytes that follow */
	uint32_t sum;
	uint8_t payload[240];
};

static atomic_uchar stress_seen[STRESS_WRITERS][STRESS_MSGS];
static atomic_int stress_received;
static atomic_int stress_errors;

static uint32_t checksum(const uint8_t *p, uint32_t n)
{
	uint32_t s = 0;

	while (n--)
		s = s * 31 + *p++;
	return s;
}

static void *stress_writer(void *arg)
{
	uint32_t id = (uint32_t)(uintptr_t)arg;
	struct stress_msg m;
	uint32_t i, j;
	size_t total;
	int fd = open(dev_path, O_WRONLY);

	if (fd < 0 || set_timeout(fd, 30000)) {
		atomic_fetch_add(&stress_errors, 1);
		return NULL;
	}

	for (i = 0; i < STRESS_MSGS; i++) {
		m.writer = id;
		m.seq = i;
		m.len = (i * 7 + id * 13) % sizeof(m.payload);
		for (j = 0; j < m.len; j++)
			m.payload[j] = (uint8_t)(i + j + id);
		m.sum = checksum(m.payload, m.len);

		total = offsetof(struct stress_msg, payload) + m.len;
		if (write(fd, &m, total) != (ssize_t)total) {
			atomic_fetch_add(&stress_errors, 1);
			break;
		}
	}
	close(fd);
	return NULL;
}

static void *stress_reader(void *arg)
{
	int64_t last[STRESS_WRITERS];
	struct stress_msg m;
	ssize_t n;
	int i;
	int fd = open(dev_path, O_RDONLY);

	(void)arg;
	for (i = 0; i < STRESS_WRITERS; i++)
		last[i] = -1;

	if (fd < 0 || set_timeout(fd, 100)) {
		atomic_fetch_add(&stress_errors, 1);
		return NULL;
	}

	while (atomic_load(&stress_received) < STRESS_TOTAL &&
	       !atomic_load(&stress_errors)) {
		n = read(fd, &m, sizeof(m));
		if (n < 0) {
			if (errno == ETIMEDOUT)
				continue;	/* re-check the exit condition */
			atomic_fetch_add(&stress_errors, 1);
			break;
		}

		if (n != (ssize_t)(offsetof(struct stress_msg, payload) + m.len) ||
		    m.writer >= STRESS_WRITERS || m.seq >= STRESS_MSGS ||
		    m.sum != checksum(m.payload, m.len) ||
		    (int64_t)m.seq <= last[m.writer] ||
		    atomic_exchange(&stress_seen[m.writer][m.seq], 1)) {
			atomic_fetch_add(&stress_errors, 1);
			break;
		}
		last[m.writer] = m.seq;
		atomic_fetch_add(&stress_received, 1);
	}
	close(fd);
	return NULL;
}

static int test_stress_mpmc(void)
{
	pthread_t w[STRESS_WRITERS], r[STRESS_READERS];
	struct msgring_stats st;
	uint32_t cap = 8;	/* small on purpose: forces constant blocking */
	int i, j;
	int fd = open_clean(O_RDWR);

	CHECK(fd >= 0);
	CHECK(ioctl(fd, MSGRING_IOC_SET_CAPACITY, &cap) == 0);

	for (i = 0; i < STRESS_READERS; i++)
		CHECK(pthread_create(&r[i], NULL, stress_reader, NULL) == 0);
	for (i = 0; i < STRESS_WRITERS; i++)
		CHECK(pthread_create(&w[i], NULL, stress_writer,
				     (void *)(uintptr_t)i) == 0);
	for (i = 0; i < STRESS_WRITERS; i++)
		pthread_join(w[i], NULL);
	for (i = 0; i < STRESS_READERS; i++)
		pthread_join(r[i], NULL);

	errno = 0;
	CHECK(atomic_load(&stress_errors) == 0);
	CHECK(atomic_load(&stress_received) == STRESS_TOTAL);
	for (i = 0; i < STRESS_WRITERS; i++)
		for (j = 0; j < STRESS_MSGS; j++)
			CHECK(atomic_load(&stress_seen[i][j]) == 1);

	CHECK(ioctl(fd, MSGRING_IOC_GET_STATS, &st) == 0);
	CHECK(st.depth == 0);
	CHECK(st.high_watermark <= MSGRING_DEFAULT_CAPACITY);
	close(fd);
	return 0;
}

/* --------------------------------------------------------------- runner */

static const struct {
	const char *name;
	int (*fn)(void);
} tests[] = {
	{ "open_close",			test_open_close },
	{ "roundtrip",			test_roundtrip },
	{ "boundaries_and_fifo",	test_boundaries_and_fifo },
	{ "persist_across_open",	test_persist_across_open },
	{ "nonblock_empty",		test_nonblock_empty },
	{ "nonblock_full",		test_nonblock_full },
	{ "size_limits",		test_size_limits },
	{ "bad_pointer",		test_bad_pointer },
	{ "poll",			test_poll },
	{ "blocking_read_wakeup",	test_blocking_read_wakeup },
	{ "blocking_write_wakeup",	test_blocking_write_wakeup },
	{ "timeout",			test_timeout },
	{ "timeout_is_per_open",	test_timeout_is_per_open },
	{ "signal_interrupts",		test_signal_interrupts },
	{ "flush_and_stats",		test_flush_and_stats },
	{ "capacity_ioctl",		test_capacity_ioctl },
	{ "bad_ioctl",			test_bad_ioctl },
	{ "stress_mpmc",		test_stress_mpmc },
};

int main(int argc, char **argv)
{
	size_t i, n = sizeof(tests) / sizeof(tests[0]);
	int failed = 0;

	if (argc > 1)
		dev_path = argv[1];

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("TAP version 13\n1..%zu\n", n);
	for (i = 0; i < n; i++) {
		int rc = tests[i].fn();

		printf("%s %zu - %s\n", rc ? "not ok" : "ok", i + 1,
		       tests[i].name);
		failed += rc != 0;
	}
	printf("# %zu passed, %d failed\n", n - failed, failed);
	return failed ? 1 : 0;
}
