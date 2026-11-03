#!/bin/sh
# boot the image headless, let it come all the way up and go all the way
# down again, and read the serial log for each step of both. then check,
# from the host, the disk it ran on, and boot what it installed.
#
# the kernel has no shell of its own, and the medium this image carries
# has no programs on it but init, the selftests and a checker, so the one
# thing typed at the machine is the answer /init asks for. the first boot
# answers two root's selftests: one copies the medium onto a blank drive,
# the way an installer does, and one checks the filesystem at / with the
# checker built for the machine. the second boots that copy and nothing
# else, and answers `none`, which runs no selftests and powers the machine
# off. what is checked is the kernel and nothing else: the boot, the root
# it hands /init, the disk /init mounts and moves into, the memory, the
# scheduler, the journal, a shutdown that closes them, filesystems a
# checker that is not the driver calls clean afterwards, the same checker
# saying the same on the machine as off it, and a drive written by the
# byte that starts a machine of its own. ring 3 is checked by the
# selftests, `make kselftest`, and by whatever system ships this kernel
# with programs on it.
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
# situation an image written onto a bigger drive is in.
#
# and a blank drive as big as the medium, for the machine to install
# itself onto
HERE="$(dirname "$0")"
DISK="$(mktemp -t velvet-boottest-XXXXXX.img)"
SYSIMG="$(mktemp -t velvet-boottest-sys-XXXXXX.img)"
WORKIMG="$(mktemp -t velvet-boottest-work-XXXXXX.img)"
BLANK="$(mktemp -t velvet-boottest-blank-XXXXXX.img)"
EMPTY="$(mktemp -d -t velvet-boottest-XXXXXX)"
trap 'rm -rf "$DISK" "$SYSIMG" "$WORKIMG" "$BLANK" "$EMPTY"' EXIT

if [ ! -f "$BOOTIMG" ]; then
    echo "no such image: $BOOTIMG (run 'make' first)" >&2
    exit 1
fi

"$HERE/../bin/tests/mkext4" "$SYSIMG" "$HERE/../base/diskroot" 48 >/dev/null
echo "somewhere to work that is not the system" > "$EMPTY/readme.txt"
"$HERE/../bin/tests/mkext4" "$WORKIMG" "$EMPTY" 8 work >/dev/null
dd if=/dev/zero of="$WORKIMG" bs=1M count=0 seek=24 2>/dev/null
python3 "$HERE/mkdisk.py" "$DISK" mbr "$SYSIMG:linux" "$WORKIMG:linux" \
        >/dev/null
truncate -s "$(stat -c %s "$BOOTIMG")" "$BLANK"
# kept rather than thrown away. a boot test that fails and deletes the
# only record of why is a boot test you debug by running it again and
# squinting at a terminal.
FIRST="bin/$(basename "$BOOTIMG" .img)-boottest.log"
SECOND="bin/$(basename "$BOOTIMG" .img)-boottest-copy.log"
mkdir -p bin

# one boot, into the log named first, with the answer second and the
# drives after it. the sleep matters: the kernel has to get all the way
# to /init's question before the answer means anything. and qemu does
# not stop when its input ends, so it is the machine powering itself off
# that ends this, and the timeout is only for a machine that never does
boot() {
    log="$1"
    answer="$2"
    shift 2
    {
        sleep 8
        printf '%s\r' "$answer"
    } | timeout 120 qemu-system-x86_64 \
            -M q35 -m 2G -smp "${CPUS:-2}" -boot order=c "$@" \
            -display none -serial stdio -no-reboot \
            > "$log" 2>&1
}

fail=0
LOG="$FIRST"
check() {
    if grep -qF "$2" "$LOG"; then
        printf '  ok    %s\n' "$1"
    else
        printf '  FAIL  %s (no "%s" in the log)\n' "$1" "$2"
        fail=1
    fi
}

# and did it come down the way it went up, rather than falling over
check_down() {
    check 'machine down'       'The Velvet Room fades'
    if [ "$1" -eq 124 ]; then
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
}

# partition n of the disk, out into the file named second
partition() {
    python3 - "$DISK" "$1" "$2" <<'EOF'
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
}

# the disk it ran on, read back by a checker that shares no line with the
# driver. a machine that mounted both filesystems, replayed and wrote its
# journal and closed it on the way down has left two filesystems with
# nothing wrong with them, and an open log, a lost block or a count that
# disagrees is a shutdown that was not one
check_partitions() {
    for n in 1 2; do
        PART="$(mktemp -t velvet-boottest-part-XXXXXX.img)"
        partition "$n" "$PART"
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
}

echo "booting $BOOTIMG, installing it, and powering it off again..."
rc=0
boot "$FIRST" 'fs/install fs/fsck' \
        -drive id=boot,file="$BOOTIMG",format=raw,if=none \
        -device ide-hd,drive=boot,bus=ide.0,bootindex=0 \
        -drive id=data,file="$DISK",format=raw,if=none \
        -device ide-hd,drive=data,bus=ide.1,bootindex=1 \
        -drive id=blank,file="$BLANK",format=raw,if=none \
        -device ide-hd,drive=blank,bus=ide.2 || rc=$?

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
check 'medium installed'   'init: selftests/fs/install ended with 0'
check 'root checked'       'init: selftests/fs/fsck ended with 0'
check_down "$rc"
check_partitions

# the checker on the machine read the root's partition with the
# filesystem mounted on it, and the one on the host reads it now, with the
# machine down. nothing wrote to it between the two, so the same program
# has to say the same thing, the device's name aside, down to how many
# blocks and inodes are free
PART="$(mktemp -t velvet-boottest-part-XXXXXX.img)"
partition 1 "$PART"
on=$(grep '^# checker: ' "$FIRST" | tr -d '\r' | sed 's/^# checker: //' \
     | sed '1s/^[^:]*: //')
off=$("$HERE/../bin/tests/fsckext4" -n "$PART" 2>&1 | sed '1s/^[^:]*: //')
rm -f "$PART"
if [ -n "$on" ] && [ "$on" = "$off" ]; then
    echo '  ok    the checker on the machine says what it says off it'
else
    echo '  FAIL  the checker on the machine and off it disagree:'
    echo "$on" | sed 's/^/        on:  /'
    echo "$off" | sed 's/^/        off: /'
    fail=1
fi

echo "booting what it installed, with nothing else to start from..."
LOG="$SECOND"
rc=0
boot "$SECOND" 'none' \
        -drive id=boot,file="$BLANK",format=raw,if=none \
        -device ide-hd,drive=boot,bus=ide.0,bootindex=0 \
        -drive id=data,file="$DISK",format=raw,if=none \
        -device ide-hd,drive=data,bus=ide.1,bootindex=1 || rc=$?
check 'the copy boots'     'velvet v'
check 'from itself'        'boot       : from sda'
check '/init pivots'       'init: the disk is the root now'
check '/init asks'         'run which selftests?'
check '/init runs none'    'no selftests. powering off'
check_down "$rc"
check_partitions

if [ "$fail" -ne 0 ]; then
    for log in "$FIRST" "$SECOND"; do
        [ -f "$log" ] || continue
        echo
        echo "the whole serial log is in $log ($(wc -l < "$log") lines)"
        echo '--- the last 40 lines of it ---'
        tail -40 "$log"
    done
    exit 1
fi

echo 'boot test passed'
