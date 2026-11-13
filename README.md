# msgring

A Linux kernel character-device driver that implements a bounded,
message-oriented queue shared by any number of reader and writer processes,
plus a userspace test suite that exercises it under concurrency.

```
 writer A ─┐                      ┌─ reader X
 writer B ─┼─ write() ─► [ m1 | m2 | m3 | ... ] ─ read() ─┼─ reader Y
 writer C ─┘        /dev/msgringN (in kernel)             └─ reader Z
```

Each `write()` enqueues exactly one message and each `read()` dequeues exactly
one, so message boundaries are preserved, like a datagram socket or a POSIX
message queue.

## Features

- Blocking and non-blocking I/O (`O_NONBLOCK` returns `EAGAIN`)
- `poll()` / `select()` / `epoll` support
- Per-open timeouts for blocking calls (`ETIMEDOUT`)
- Runtime control through `ioctl`: resize, flush, read statistics
- Live counters in debugfs (`/sys/kernel/debug/msgring/msgringN`)
- Multiple independent devices (`ndevs=` module parameter)
- 18-test suite including a multi-producer/multi-consumer stress test
- One-command test run inside QEMU, so a bug cannot crash your machine

## Build and test

Requirements: kernel headers for the target kernel, `gcc`, `make`.

```sh
make                 # builds driver/msgring.ko and tests/msgring_test
make check           # insmod on the running kernel, run tests, rmmod (sudo)
make qemu-test       # same tests in a throwaway VM, no root needed
```

`make qemu-test` additionally needs `qemu-system-x86`, `busybox-static` and
`cpio`. Tested on Linux 7.0 (tests run in QEMU) and build-tested on 6.8.

Expected output:

```
ok 1 - open_close
...
ok 18 - stress_mpmc
# 18 passed, 0 failed
```

## Usage

```sh
sudo insmod driver/msgring.ko ndevs=2 capacity=64
echo -n "hello" > /dev/msgring0
cat /dev/msgring0            # prints "hello", then blocks for the next message
sudo cat /sys/kernel/debug/msgring/msgring0
```

From C:

```c
#include "msgring.h"

int fd = open("/dev/msgring0", O_RDWR);
uint32_t ms = 500;
ioctl(fd, MSGRING_IOC_SET_TIMEOUT, &ms);   /* give up after 500 ms */

write(fd, "ping", 4);                       /* one message in  */
ssize_t n = read(fd, buf, sizeof(buf));     /* one message out */
```

## API

| Call | Behaviour |
| --- | --- |
| `write(fd, buf, n)` | Enqueue one message of `n` bytes (1 to 4096). Blocks while the queue is full. |
| `read(fd, buf, n)` | Dequeue one message. Blocks while the queue is empty. `EMSGSIZE` if `n` is smaller than the message; the message stays queued. |
| `poll()` | `POLLIN` when a message is queued, `POLLOUT` when there is room. |
| `MSGRING_IOC_GET_STATS` | Fill a `struct msgring_stats`. |
| `MSGRING_IOC_SET_CAPACITY` | Resize the queue (1 to 4096 messages). `EBUSY` if smaller than the current depth. |
| `MSGRING_IOC_FLUSH` | Drop all queued messages. |
| `MSGRING_IOC_SET_TIMEOUT` | Timeout in ms for blocking calls on this file descriptor. 0 means wait forever. |

Errors: `EAGAIN` (non-blocking and would block), `ETIMEDOUT`, `EINTR`
(signal), `EMSGSIZE`, `EINVAL`, `EFAULT`, `ENOTTY` (unknown ioctl).

## Layout

```
include/msgring.h         ioctl numbers and structs shared with userspace
driver/msgring.c          the kernel module
tests/msgring_test.c      functional and stress tests (TAP output)
scripts/qemu-test.sh      boots a VM, loads the module, runs the tests
docs/DESIGN.md            design notes: locking, wakeups, error paths
.github/workflows/ci.yml  build, load and test on every push
```

## Design

See [docs/DESIGN.md](docs/DESIGN.md) for the locking model, how lost wakeups
are avoided, and the reasoning behind each error path.

## License

GPL-2.0. See [LICENSE](LICENSE).
