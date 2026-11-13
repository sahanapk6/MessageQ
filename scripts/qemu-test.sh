#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Boot a kernel in QEMU with a tiny busybox initramfs, load msgring.ko, run
# the test suite, unload the module and check the kernel log for problems.
# Nothing touches the host kernel, so no root is needed and a driver bug
# cannot crash your machine.
#
# Usage: scripts/qemu-test.sh [kernel-version]   (default: uname -r)
# Needs: qemu-system-x86_64, static busybox, cpio, a readable kernel image.
set -euo pipefail

KVER="${1:-$(uname -r)}"
KERNEL="${KERNEL:-/boot/vmlinuz-$KVER}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUSYBOX="${BUSYBOX:-$(command -v busybox)}"
TIMEOUT="${TIMEOUT:-600}"

[ -r "$KERNEL" ] || { echo "cannot read $KERNEL (try: sudo chmod +r $KERNEL)"; exit 2; }
[ -f "$ROOT/driver/msgring.ko" ] || { echo "build first: make KVER=$KVER"; exit 2; }
[ -x "$ROOT/tests/msgring_test" ] || { echo "build first: make tests STATIC=1"; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/root"/{bin,dev,proc,sys}
cp "$BUSYBOX" "$WORK/root/bin/busybox"
cp "$ROOT/driver/msgring.ko" "$ROOT/tests/msgring_test" "$WORK/root/"

cat > "$WORK/root/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount -t debugfs debugfs /sys/kernel/debug

insmod /msgring.ko ndevs=2;        echo "QEMU_INSMOD: $?"
/msgring_test /dev/msgring0;       echo "QEMU_TESTS: $?"
/msgring_test /dev/msgring1 >/dev/null; echo "QEMU_TESTS_DEV1: $?"
echo "--- debugfs stats"; cat /sys/kernel/debug/msgring/msgring0
rmmod msgring;                     echo "QEMU_RMMOD: $?"
echo "--- kernel log"
dmesg | grep -E "msgring|BUG|WARNING|Oops|KASAN|kmemleak|Call Trace"
poweroff -f
INIT
chmod +x "$WORK/root/init"

(cd "$WORK/root" && find . | cpio -o -H newc --quiet | gzip) > "$WORK/initrd.gz"

ACCEL=()
[ -w /dev/kvm ] && ACCEL=(-enable-kvm -cpu host)

timeout "$TIMEOUT" qemu-system-x86_64 "${ACCEL[@]}" \
	-m 512 -smp 4 -nographic -no-reboot \
	-kernel "$KERNEL" -initrd "$WORK/initrd.gz" \
	-append "console=ttyS0 quiet panic=-1" | tee "$WORK/log" || true

ok=1
for marker in "QEMU_INSMOD: 0" "QEMU_TESTS: 0" "QEMU_TESTS_DEV1: 0" "QEMU_RMMOD: 0"; do
	grep -q "$marker" "$WORK/log" || { echo "MISSING: $marker"; ok=0; }
done
if grep -E "BUG|WARNING|Oops|KASAN|kmemleak|Call Trace" "$WORK/log"; then
	echo "kernel reported a problem"; ok=0
fi

[ "$ok" = 1 ] && echo "qemu-test: PASS" || { echo "qemu-test: FAIL"; exit 1; }
