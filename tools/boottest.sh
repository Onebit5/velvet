#!/bin/sh
# boot the image headless, let it come all the way up and go all the way
# down again, and read the serial log for each step of both. then check,
# from the host, the disk it ran on.
#
# the kernel has no shell of its own, and the medium this image carries
# has no programs on it but init and the selftests, so the one thing typed
# at the machine is the answer /init asks for: `none`, which runs no
# selftests and powers the machine off. what is checked is the kernel and
# nothing else: the boot, the root it hands /init, the disk /init mounts
# and moves into, the memory, the scheduler, the journal, a shutdown that
# closes them, and filesystems a checker that is not the driver calls
# clean afterwards. ring 3 is checked by the selftests, `make kselftest`,
# and by whatever system ships this kernel with programs on it.
#
#   usage: tools/boottest.sh [velvet.img]

set -eu

BOOTIMG="${1:-velvet.img}"

# a disk of its own, built fresh, because the machine writes to it and a
# run that only passes on the leavings of the last one proves nothing.
#
# ext4 and built by the machine's own formatter rather than the python
# one, so what is under test includes the filesystem the machine
# actually runs on.
#
# two partitions: the system on one and somewhere to work on the other,
# which is what an installed disk looks like. the second filesystem is
# smaller than its partition, 8 MiB of ext4 inside 24 MiB, which is the
# situation an image written onto a bigger drive is in
HERE="$(dirname "$0")"
DISK="$(mktemp -t velvet-boottest-XXXXXX.img)"
SYSIMG="$(mktemp -t velvet-boottest-sys-XXXXXX.img)"
WORKIMG="$(mktemp -t velvet-boottest-work-XXXXXX.img)"
EMPTY="$(mktemp -d -t velvet-boottest-XXXXXX)"
trap 'rm -rf "$DISK" "$SYSIMG" "$WORKIMG" "$EMPTY"' EXIT

"$HERE/../bin/tests/mkext4" "$SYSIMG" "$HERE/../base/diskroot" 48 >/dev/null
echo "somewhere to work that is not the system" > "$EMPTY/readme.txt"
"$HERE/../bin/tests/mkext4" "$WORKIMG" "$EMPTY" 8 work >/dev/null
dd if=/dev/zero of="$WORKIMG" bs=1M count=0 seek=24 2>/dev/null
python3 "$HERE/mkdisk.py" "$DISK" mbr "$SYSIMG:linux" "$WORKIMG:linux" \
        >/dev/null
# kept rather than thrown away. a boot test that fails and deletes the
# only record of why is a boot test you debug by running it again and
# squinting at a terminal.
LOG="bin/$(basename "$BOOTIMG" .img)-boottest.log"
mkdir -p bin

if [ ! -f "$BOOTIMG" ]; then
    echo "no such image: $BOOTIMG (run 'make' first)" >&2
    exit 1
fi

echo "booting $BOOTIMG, and powering it off again..."

# the sleep matters: the kernel has to get all the way to /init's
# question before the answer means anything. and qemu does not stop when
# its input ends, so it is the machine powering itself off that ends this,
# and the timeout is only for a machine that never does
rc=0
{
    sleep 8
    printf 'none\r'
} | timeout 120 qemu-system-x86_64 \
        -M q35 -m 2G -smp "${CPUS:-2}" -boot order=c \
        -drive id=boot,file="$BOOTIMG",format=raw,if=none \
        -device ide-hd,drive=boot,bus=ide.0,bootindex=0 \
        -drive id=data,file="$DISK",format=raw,if=none \
        -device ide-hd,drive=data,bus=ide.1,bootindex=1 \
        -display none -serial stdio -no-reboot \
        > "$LOG" 2>&1 || rc=$?

fail=0
check() {
    if grep -qF "$2" "$LOG"; then
        printf '  ok    %s\n' "$1"
    else
        printf '  FAIL  %s (no "%s" in the log)\n' "$1" "$2"
        fail=1
    fi
}

# did it boot at all
check 'kernel banner'      'velvet v'
check 'framebuffer found'  'framebuffer :'   # serial keeps everything
check 'idt armed'          '256 gates armed'
check 'acpi parsed'        'acpi       :'
check 'interrupts routed'  'interrupts :'
check 'pci scanned'        'pci        :'
check 'memory map parsed'  'memory map, as the loader found it'
check 'memory selftest'    'books balance'
check 'blocks merge'       'blocks merge'
check 'own page tables'    "cr3 is the kernel's"
check 'W^X applied'        'W^X on .text'
check 'tss loaded'         'tss loaded'
check 'ramdisk mounted'    'ramdisk    :'
check 'root is the archive' 'root       : the archive'
# the disk the machine boots has a log on it, and mounting it is
# supposed to replay that log and then write through it. a machine that
# quietly fell back to mounting read-only would pass every check that
# reads and fail only the ones that write. it is /init that mounts it
# and moves into it, from the archive the kernel made the root
check 'disk mounted'       'mount      : ext4 "velvet" on'
check 'journal started'    'journalled: every change is written down'
check '/init pivots'       'init: the disk is the root now'
check 'work mounted'       'is mounted at /work'
check 'boot thread left'   '[boot] hath returned'
check 'memory reclaimed'   'reclaimed'
check 'scheduler started'  'the wheel turns'
check '/init started'      'handing the machine to /init'
check '/init asks'         'run which selftests?'
check '/init runs none'    'no selftests. powering off'

# and did it come down the way it went up, rather than falling over
check 'machine down'       'The Velvet Room fades'
if [ "$rc" -eq 124 ]; then
    echo '  FAIL  it never powered off, and the timeout stopped it'
    fail=1
else
    echo '  ok    it powered itself off'
fi
if grep -qF 'KERNEL PANIC' "$LOG"; then
    echo '  FAIL  it panicked somewhere:'
    grep -A6 'KERNEL PANIC' "$LOG" | sed 's/^/        /'
    fail=1
else
    echo '  ok    no panic'
fi

# the disk it ran on, read back by a checker that shares no line with the
# driver. a machine that mounted both filesystems, replayed and wrote its
# journal and closed it on the way down has left two filesystems with
# nothing wrong with them, and an open log, a lost block or a count that
# disagrees is a shutdown that was not one
for n in 1 2; do
    PART="$(mktemp -t velvet-boottest-part-XXXXXX.img)"
    python3 - "$DISK" "$n" "$PART" <<'EOF'
import struct, sys
disk, n, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
with open(disk, 'rb') as f:
    mbr = f.read(512)
    first, count = struct.unpack_from('<II', mbr, 446 + (n - 1) * 16 + 8)
    f.seek(first * 512)
    data = f.read(count * 512)
with open(out, 'wb') as f:
    f.write(data)
EOF
    if out=$("$HERE/../bin/tests/fsckext4" -n "$PART" 2>&1) \
            && python3 "$HERE/readext4.py" "$PART" >/dev/null 2>&1; then
        printf '  ok    partition %s is clean afterwards\n' "$n"
    else
        printf '  FAIL  partition %s is not clean afterwards:\n' "$n"
        echo "$out" | sed 's/^/        /'
        fail=1
    fi
    rm -f "$PART"
done

if [ "$fail" -ne 0 ]; then
    echo
    echo "the whole serial log is in $LOG ($(wc -l < "$LOG") lines)"
    echo '--- the last 40 lines of it ---'
    tail -40 "$LOG"
    exit 1
fi

echo 'boot test passed'
