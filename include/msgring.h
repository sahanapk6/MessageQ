/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * msgring - userspace API shared between the driver and its users.
 */
#ifndef MSGRING_H
#define MSGRING_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Largest single message accepted by write(). */
#define MSGRING_MAX_MSG_SIZE		4096
/* Queue depth (in messages) a device starts with. */
#define MSGRING_DEFAULT_CAPACITY	64
/* Upper bound for MSGRING_IOC_SET_CAPACITY. */
#define MSGRING_MAX_CAPACITY		4096

/**
 * struct msgring_stats - snapshot of one device's counters
 * @enqueued:       messages accepted by write() since module load
 * @dequeued:       messages delivered by read()
 * @flushed:        messages discarded by MSGRING_IOC_FLUSH
 * @bytes_in:       payload bytes accepted
 * @bytes_out:      payload bytes delivered
 * @depth:          messages queued right now
 * @capacity:       maximum messages the queue holds
 * @high_watermark: largest depth ever observed
 * @reserved:       zero, keeps the layout identical for 32/64-bit callers
 */
struct msgring_stats {
	__u64 enqueued;
	__u64 dequeued;
	__u64 flushed;
	__u64 bytes_in;
	__u64 bytes_out;
	__u32 depth;
	__u32 capacity;
	__u32 high_watermark;
	__u32 reserved;
};

#define MSGRING_IOC_MAGIC	'M'

/* Copy a struct msgring_stats snapshot to userspace. */
#define MSGRING_IOC_GET_STATS	_IOR(MSGRING_IOC_MAGIC, 1, struct msgring_stats)
/* Resize the queue. -EINVAL if out of range, -EBUSY if below current depth. */
#define MSGRING_IOC_SET_CAPACITY _IOW(MSGRING_IOC_MAGIC, 2, __u32)
/* Drop every queued message. */
#define MSGRING_IOC_FLUSH	_IO(MSGRING_IOC_MAGIC, 3)
/*
 * Per-open blocking timeout in milliseconds for read() and write().
 * 0 (the default) waits forever. On expiry the call fails with ETIMEDOUT.
 */
#define MSGRING_IOC_SET_TIMEOUT	_IOW(MSGRING_IOC_MAGIC, 4, __u32)

#endif /* MSGRING_H */
