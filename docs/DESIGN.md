# msgring design notes

## Data model

Each device (`struct msgring_dev`) owns a linked list of messages. A message
(`struct msgring_msg`) is a single allocation: list node, length, then the
payload as a flexible array. The queue is bounded by a message count
(`capacity`), not by bytes; each message is capped at 4096 bytes, so memory
use is bounded by `capacity * 4096`.

Messages belong to the device, not to the file descriptor that wrote them.
Closing a writer does not discard what it queued.

Per-open state (`struct msgring_file`) holds only the timeout, so two
processes sharing one device can choose different timeouts.

## Locking

One mutex per device protects the list and every counter. A mutex (not a
spinlock) is used because the read path calls `copy_to_user()` while holding
it, and that can sleep on a page fault.

Rules followed throughout:

1. Never sleep waiting for a condition while holding the mutex. The wait
   loops drop it, sleep, then re-take it.
2. Always re-check the condition after re-taking the mutex. Another reader
   may have taken the message that woke us.
3. Allocate and copy from userspace before taking the mutex in `write()`,
   so the critical section is only the list insertion.
4. Free memory after dropping the mutex (`read()` and flush).

`depth` and `capacity` are also read without the mutex inside the
`wait_event` condition. Those reads use `READ_ONCE()` and the writes use
`WRITE_ONCE()`. A stale value there is harmless because of rule 2.

## Avoiding lost wakeups

Two wait queues: readers sleep on `read_wq` ("not empty"), writers on
`write_wq` ("not full"). `wait_event_interruptible_timeout()` puts the task on
the wait queue *before* evaluating the condition, so a wakeup that arrives
between the check and the sleep is not lost.

Every state change that could satisfy a sleeper issues a wakeup:

| Change | Wakes |
| --- | --- |
| message enqueued | readers |
| message dequeued | writers |
| flush | writers |
| capacity raised | writers |

## Timeouts

`wait_event_interruptible_timeout()` returns the remaining jiffies. The loop
feeds that value back in, so a reader that is woken but loses the race for
the message goes back to sleep for the remaining time only, not a fresh full
timeout.

## Error paths

- `read()` with a buffer smaller than the message returns `EMSGSIZE` and
  leaves the message at the head of the queue. Truncating silently would
  lose data.
- `read()` copies to userspace *before* unlinking the message. If the user
  pointer is bad (`EFAULT`) the message is still queued.
- `write()` frees its allocation on every failure path (`goto err_free`).
- A signal during a blocking call returns `-ERESTARTSYS`, which the kernel
  turns into `EINTR` or a transparent restart depending on `SA_RESTART`.
- `GET_STATS` zeroes the struct before filling it, so no uninitialised kernel
  stack bytes reach userspace.

## Lifetime

The devices are registered as misc devices, which gives dynamic minor numbers
and automatic `/dev` nodes. `fops.owner = THIS_MODULE` makes the kernel hold a
module reference for every open file, so `rmmod` fails with `EBUSY` while
anything has the device open. By the time `msgring_exit()` runs, no task can
be sleeping in the driver, and the remaining messages are freed.

`msgring_init()` unwinds in reverse order if registering any device fails.

## Testing

`tests/msgring_test.c` has 17 functional tests (boundaries, FIFO order,
non-blocking, poll, timeouts, signals, ioctls, bad pointers) and one stress
test: 4 writers and 4 readers push 4000 checksummed messages through a queue
of depth 8, which forces constant blocking on both sides. It verifies that no
message is lost, duplicated or corrupted, and that each reader sees any one
writer's messages in order.

`scripts/qemu-test.sh` runs the suite in a VM on both devices, unloads the
module, and fails if the kernel log contains BUG, WARNING, Oops or KASAN
output.

## Possible extensions

- Byte-based capacity limit in addition to the message count
- Priority messages
- `fasync` / `SIGIO` notification
- Run CI on a kernel built with KASAN and lockdep
