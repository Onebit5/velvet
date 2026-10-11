#!/bin/sh
# boot the image headless and actually use the thing: type commands at
# the shell over the serial line and check the answers come back.
#
# this is only possible because com1 is wired to the input queue, so the
# shell cant tell the difference between a keyboard and a pipe.
#
# what is checked is the kernel and nothing else. the medium this image
# carries has no programs on it, so every command below is one the
# kernel answers itself: the boot, the memory, the scheduler, the
# login, the disks, the journal, a resize, fsck, and the source on the
# medium. ring 3 is checked by the selftests, `make kselftest`, and by
# whatever system ships this kernel with programs on it, velvetOS's boot
# test being the first of those.
#
#   usage: tools/boottest.sh [velvet.img]

set -eu

BOOTIMG="${1:-velvet.img}"

# a disk of its own, built fresh, because this test writes to it and a
# run that only passes on the leavings of the last one proves nothing.
#
# ext4 and built by the machine's own formatter rather than the python
# one, so what is under test includes the filesystem the machine
# actually runs on.
#
# two partitions: the system on one and somewhere to work on the other,
# which is what an installed disk looks like. and the second filesystem
# is deliberately *smaller than its partition*, 8 MiB of ext4 inside 24
# MiB of partition, because that is the exact situation `resize` exists
# for, and it is the one an image written onto a bigger drive is in
HERE="$(dirname "$0")"
DISK="$(mktemp -t velvet-boottest-XXXXXX.img)"
SYSIMG="$(mktemp -t velvet-boottest-sys-XXXXXX.img)"
WORKIMG="$(mktemp -t velvet-boottest-work-XXXXXX.img)"
EMPTY="$(mktemp -d -t velvet-boottest-XXXXXX)"
trap 'rm -rf "$DISK" "$SYSIMG" "$WORKIMG" "$EMPTY"' EXIT

"$HERE/../bin/tests/mkext4" "$SYSIMG" "$HERE/../base/diskroot" 48 >/dev/null
echo "somewhere to work that is not the system" > "$EMPTY/readme.txt"
"$HERE/../bin/tests/mkext4" "$WORKIMG" "$EMPTY" 8 work >/dev/null
# and the room to grow into, which is a partition longer than the
# filesystem in it rather than anything the filesystem knows about
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

echo "booting $BOOTIMG and driving the shell over serial..."

# the sleeps matter: the kernel has to get all the way to a prompt
# before it can hear me, and each command needs a beat to answer
{
    sleep 8
    printf 'igor\r';   sleep 1
    printf 'velvet\r'; sleep 2
    printf 'help\r';   sleep 1
    printf 'mem\r';    sleep 1
    printf 'ps\r';     sleep 1
    printf 'summon pixie\r'; sleep 3
    printf 'ps\r';     sleep 2
    printf 'vmm\r';    sleep 2
    printf 'bt\r';     sleep 2
    printf 'date\r';   sleep 1
    printf 'history\r'; sleep 1
    printf 'whoami\r';  sleep 1
    printf 'lspci\r';   sleep 2
    printf 'slabs\r';   sleep 2
    printf 'disk\r';    sleep 2
    printf 'mount\r';   sleep 2
    printf 'logout\r';  sleep 1
    printf 'guest\r';   sleep 1
    printf 'guest\r';   sleep 2
    printf 'whoami\r';  sleep 1
    printf 'arcana\r';  sleep 1
    printf 'persona\r'; sleep 2
    printf 'dmesg\r';   sleep 2
    printf 'logout\r';  sleep 1
    printf 'igor\r';    sleep 1
    printf 'velvet\r';  sleep 2
    printf 'sync\r';    sleep 2
    printf 'fsck\r';    sleep 5
    printf 'fsck\r';    sleep 5
    printf 'mount\r';   sleep 2
    printf 'resize work\r'; sleep 6
    printf 'fsck work\r'; sleep 6
    # the source is on the boot medium, which is drive 0, while
    # the filesystem is on drive 1, so every one of these reads the
    # other drive and puts the mounted one back, in the middle of a
    # machine that is using it
    printf 'src\r';   sleep 2
    printf 'src verify\r'; sleep 20
    printf 'src unpack /work/src\r'; sleep 12
    printf 'sync\r';   sleep 2
    # and what the unpack wrote is checked by the same fsck that has
    # been checking the disk all along
    printf 'fsck work\r'; sleep 6
} | timeout 300 qemu-system-x86_64 \
        -M q35 -m 2G -smp "${CPUS:-2}" -boot order=c \
        -drive id=boot,file="$BOOTIMG",format=raw,if=none \
        -device ide-hd,drive=boot,bus=ide.0,bootindex=0 \
        -drive id=data,file="$DISK",format=raw,if=none \
        -device ide-hd,drive=data,bus=ide.1,bootindex=1 \
        -display none -serial stdio -no-reboot \
        > "$LOG" 2>&1 || true

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
check 'memory map parsed'  'memory map, as philemon found it'
check 'memory selftest'    'books balance'
check 'own page tables'    "cr3 is the kernel's"
check 'W^X applied'        'W^X on .text'
check 'tss loaded'         'tss loaded'
check 'boot thread left'   '[boot] hath returned'
check 'memory reclaimed'   'reclaimed'
check 'scheduler started'  'the wheel turns'
check 'login prompt'       'name the guest'
check 'login works'        'welcome, igor'
check 'reached the prompt' 'igor@velvet[1]:/#'

# and then use it
check 'help works'    'comes from the program itself'
check 'mem works'     'physical frames'
check 'ps works'      'idle'
check 'summon works'  'has answered thy call'
check 'thread ran'    '[pixie]'
check 'vmm works'     'pml4 at'
check 'bt works'      'call trace:'
check 'symbols work'  'shell_run+'
check 'date works'    ':'
# numbered, because the typed command is echoed back too: a check for
# the words alone passes whether or not history printed anything
check 'history works' '4  summon pixie'
check 'whoami works'  'igor, uid 0'
check 'ramdisk mounted' 'ramdisk    :'
check 'cpu accounted'    '% '
check 'peak memory'      'ever in use at once'
check 'pci scanned'      'pci        :'
check 'lspci works'      'host bridge'
check 'blocks merge'     'blocks merge'
check 'free block shape' 'free blocks, by size'
check 'slab caches'      'kmalloc-'
# the cache's row in the slabs table, by the padding after its name:
# the bare word turns up in the source listing too, and a check that a
# file name can satisfy is not a check of the cache
check 'thread cache'     'thread      '
check 'disk at the root' 'ext4 "velvetos" mounted at /'
# the disk the machine boots has a log on it, and mounting it is
# supposed to replay that log and then write through it. a machine that
# quietly fell back to mounting read-only would pass every check that
# reads and fail only the ones that write
check 'journal started'  'journalled: every change is written down'
# two filesystems under one namespace, and a `resize` that grows
# the second into the partition it is sitting in. the second filesystem
# is made 8 MiB inside a 24 MiB partition on purpose, so the grow has
# somewhere to go and the check is that it went there
check 'work mounted'     '"work" at /work'
check 'resize grows'     'group(s), was'
check 'fsck agrees'      'nothing wrong with it'
# the source tree, out on the medium and not in memory. every one of
# these is a read of a drive that is not the mounted one, from a machine
# that is using the mounted one, and the last two are the whole claim
# of 0.4.0: the source that comes off the medium is the source this
# kernel was built from, and it can be written out as files somebody
# could edit
check 'source found'     'source     :'
check 'source mounted'   '/boot/src'
check 'source verifies'  'this is the source this kernel was built from'
check 'source unpacks'   'they are yours to edit'
# a checker that runs here, and a second run of it that finds nothing:
# a checker that finds something new every time it runs is one that
# does not understand what it sees
check 'fsck runs'        'inodes,'
check 'fsck is happy'    'nothing wrong with it'
check 'mount table'      'a module the bootloader handed the kernel'
check 'logout works'     'fare thee well'
check 'guest logs in'    'thou art a guest'
check 'arcana works'     'COMPUTER ARCANA'
check 'persona works'    'velvet@velvetOS'
check 'dmesg works'      "cr3 is the kernel's"

# and did it stay alive rather than falling over
if grep -qF 'KERNEL PANIC' "$LOG"; then
    echo '  FAIL  it panicked somewhere:'
    grep -A6 'KERNEL PANIC' "$LOG" | sed 's/^/        /'
    fail=1
else
    echo '  ok    no panic'
fi

if [ "$fail" -ne 0 ]; then
    echo
    echo "the whole serial log is in $LOG ($(wc -l < "$LOG") lines)"
    echo '--- the last 40 lines of it ---'
    tail -40 "$LOG"
    exit 1
fi

echo 'boot test passed'
