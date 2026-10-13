VERSION = 0
PATCHLEVEL = 4
SUBLEVEL = 0
KERNELRELEASE = $(VERSION).$(PATCHLEVEL).$(SUBLEVEL)

# velvet build. needs gcc, nasm, make and python3. `make run` and
# `make boottest` additionally need qemu.
#
# targets:
#   make            -> the bootable image, velvet.img
#   make kernel     -> just the kernel elf, bin/velvet
#   make philemon   -> just the bootloader, bin/boot/
#   make tools      -> the C build tools, bin/tests/
#   make run        -> boot it in qemu (CPUS=4, MEM=2G, QEMU_EXTRA=...)
#   make test       -> the host suites and every checker
#   make boottest   -> boot it headless and drive the shell over serial
#   make kernelrelease -> print the version
#   make clean
#
# building from somewhere else. velvetOS builds this kernel out of its own
# tree, and says where things go rather than letting them land in this
# checkout:
#
#   make -C obj/velvet BINDIR=... OBJROOT=... SOURCE=... kernel philemon tools
#
# BINDIR and OBJROOT are where the outputs go. SOURCE is the archive whose
# hash is stamped into the kernel, which is the whole tree shipped beside
# it, for velvetOS that is velvetOS, this directory included, and not
# only this directory. left alone, it is this tree's own archive.

KERNEL := velvet

# the bootable image, and the default target. defined up here with the
# other names because a prerequisite is expanded where it is written,
# further down, `all: $(BOOTIMG)` quietly meant `all:` with nothing to do
BOOTIMG := velvet.img

# and the disk `make run` attaches, for the same reason: `run` names it as
# a prerequisite long before the rule that builds it, and defined down
# there it was empty up here, so a fresh checkout's `make run` handed
# qemu a drive nobody had made
DISK := disk.img

BINDIR  ?= bin
OBJROOT ?= obj

# the host suites and the C tools. the suites and the python checkers find
# their images under bin/tests/, so `make test` is run with BINDIR left
# alone; another BINDIR is for building the kernel and its tools, not for
# testing them
T := $(BINDIR)/tests

# knobs for `make run`. they have to be make variables rather than extra
# words on the command line, because `make run -smp 4` hands -s -m -p to
# *make*, and -p means "print the entire database", which is a
# surprising amount of ukrainian
CPUS ?= 4
MEM  ?= 2G
QEMU_EXTRA ?=

NASM := nasm

#
# x86_64 is the only one, and the mechanism for there being more than
# one is kept deliberately.
#
# an aarch64 port was written and removed again: 0.2.20 in velvetOS's
# CHANGELOG.md is where it is told. what
# it found while it existed is still here and is most of why 0.2.20's
# boundary is worth anything: a portable file building an x86 stack
# frame, kmain naming a machine, and a serial driver that turned out to
# be a terminal with a chip stuck to it. none of that needed the port to
# survive in order to stay fixed.
#
# what does still work without one:
#
#   make portable-check LIST=1     the portable half, against no arch
#   make arch-check ARCH=x86_64    one architecture, compiled alone
#
ARCH  ?= x86_64
CROSS ?=

CC := $(CROSS)gcc
LD := $(CROSS)ld

# one object tree per architecture. without this, switching ARCH and
# rebuilding links x86 objects into an arm kernel and the error it gives
# is about relocations rather than about what you actually did
OBJDIR := $(OBJROOT)/$(ARCH)

include sources.mk

# the portable drivers an architecture does not want, because it brings
# its own. empty while there is one architecture, and the mechanism is
# kept because the *reason* for it survived the port that needed it:
# drivers/serial.c is the terminal now and the chip lives in arch/
ARCH_SKIP_x86_64 :=
ARCH_SKIP := $(ARCH_SKIP_$(ARCH))

# freestanding kernel flags. the -mno-* soup is because I cant use fpu/sse
# in the kernel (no context saving yet), and no red zone because interrupts
# would trash it
# what both machines want: no libc, no floating point, no surprises
#
# the headers are found by what they are: <velvet/...> in
# include/, <asm/...> in the one architecture being built, and
# <generated/...> in the object tree, since the build writes it. there
# is no -I. on purpose. a header private to one subsystem is included as
# "name.h" from beside it, so anything still spelled as a path from the
# root is a header in the wrong place, and fails here rather than working
CFLAGS := -g -Wall -Wextra -std=gnu11 \
	-ffreestanding -fno-stack-protector -fno-stack-check -fno-lto -fno-PIC \
	-Iinclude -Iarch/$(ARCH)/include -I$(OBJROOT)/include \
	-MMD -MP -fno-omit-frame-pointer \
	-DVELVETOS_ARCH_$(shell echo $(ARCH) | tr a-z A-Z)

# x86: the -mno-* soup is because I cant use fpu/sse in the kernel (no
# context saving yet), and no red zone because interrupts would trash it
CFLAGS_x86_64 := -m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
                 -mno-red-zone -mcmodel=kernel

CFLAGS += $(CFLAGS_$(ARCH))

LDFLAGS_x86_64  := -z max-page-size=0x1000 -T arch/x86_64/linker.ld
LDFLAGS := -nostdlib -static $(LDFLAGS_$(ARCH))

NASMFLAGS := -f elf64 -g

# the portable half, plus exactly one architecture.
#
# this used to be a plain `find kernel`, which was correct while
# there was one architecture and wrong the instant there were three:
# a build that globs everything is a build that assumes there is only
# one of everything, which stopped being true the moment arch/none
# existed, and it was quietly compiled into the x86 kernel. the portable
# half is sources.mk's list now, for the same reason one level up.
CSRC := $(filter-out $(ARCH_SKIP), \
          $(shell find $(KERNEL_PORTABLE) -name '*.c')) \
        $(shell find arch/$(ARCH) -name '*.c' 2>/dev/null)
ASRC := $(filter-out arch/$(ARCH)/trampoline.asm, \
          $(shell find $(KERNEL_PORTABLE) -name '*.asm') \
          $(shell find arch/$(ARCH) -name '*.asm' 2>/dev/null))

# gnu-as sources. none today (x86 uses nasm), and the rule stays
# because an architecture that wants them should not have to add it back
SSRC := $(shell find arch/$(ARCH) -name '*.S' 2>/dev/null)
OBJ  := $(patsubst %.c,$(OBJDIR)/%.c.o,$(CSRC)) \
        $(patsubst %.asm,$(OBJDIR)/%.asm.o,$(ASRC)) \
        $(patsubst %.S,$(OBJDIR)/%.S.o,$(SSRC)) \
        $(if $(filter x86_64,$(ARCH)),$(OBJDIR)/arch/x86_64/trampoline.c.o,)

.PHONY: all kernel philemon tools run bootimg kernelrelease clean distclean

# the pc boots from an image philemon wrote, and the kernel on its own
# is not something a pc can be handed
all: $(BOOTIMG)

# the `@:` is there so that asking for something already built is quiet.
# a build that includes this one asks every time, and three lines of
# "nothing to be done" on every one of its builds would be three lines
# nobody reads, sitting where a real message would go
kernel: $(BINDIR)/$(KERNEL)
	@:

philemon: $(BINDIR)/boot/philemon.bin $(BINDIR)/boot/philemon64.bin
	@:

kernelrelease:
	@echo $(KERNELRELEASE)

# the version, as C. VERSION, PATCHLEVEL and SUBLEVEL at the top of this
# file are the only place it is written, and the banner, `version`,
# `top` and `arcana` all print this one macro, so they cannot disagree.
#
# rewritten only when what it says changes. it is older than this
# makefile after any edit to it, so the recipe runs every time, and if
# it replaced the file unconditionally then touching a comment here
# would rebuild everything that prints the version
VERSION_H := $(OBJROOT)/include/generated/version.h

$(VERSION_H): GNUmakefile
	@mkdir -p $(@D)
	@echo '#define VELVET_VERSION "$(KERNELRELEASE)"' > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv -f $@.tmp $@

# before anything is compiled, because a fresh tree has no .d files yet
# to say which objects read it
$(OBJ): | $(VERSION_H)

# two passes, because the symbol table describes addresses and linking
# it in changes them. .ksyms sits after .text in the linker script, so
# folding it in shifts .data but cannot move a single function, and
# gensyms --check proves that held instead of me just hoping.
#
# and what was linked is written down beside it, in the order it went
# in, for anything that wants to link the same kernel again with another
# linker: velvetOS's kernelcheck hands exactly this list to its own
$(BINDIR)/$(KERNEL): $(OBJ) $(OBJDIR)/srcstamp.o arch/$(ARCH)/linker.ld \
                      tools/gensyms.py
	@mkdir -p $(@D) $(OBJDIR)
	@python3 tools/gensyms.py --stub > $(OBJDIR)/ksyms.c
	@$(CC) $(CFLAGS) -c $(OBJDIR)/ksyms.c -o $(OBJDIR)/ksyms.o
	@echo '  LD      pass 1 (to find out where everything landed)'
	@$(LD) $(LDFLAGS) $(OBJ) $(OBJDIR)/srcstamp.o $(OBJDIR)/ksyms.o -o $@.pass1
	@echo '  GENSYMS $(OBJDIR)/ksyms.c'
	@python3 tools/gensyms.py $@.pass1 > $(OBJDIR)/ksyms.c
	@$(CC) $(CFLAGS) -c $(OBJDIR)/ksyms.c -o $(OBJDIR)/ksyms.o
	@echo '  LD      pass 2 (with the symbols folded in)'
	@$(LD) $(LDFLAGS) $(OBJ) $(OBJDIR)/srcstamp.o $(OBJDIR)/ksyms.o -o $@
	@python3 tools/gensyms.py --check $@ $(OBJDIR)/ksyms.c
	@echo $(OBJ) $(OBJDIR)/srcstamp.o $(OBJDIR)/ksyms.o > $(OBJDIR)/kernel.objects
	@rm -f $@.pass1

$(OBJDIR)/%.c.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.S.o: %.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.asm.o: %.asm
	@mkdir -p $(@D)
	$(NASM) $(NASMFLAGS) $< -o $@

# the code a second cpu wakes up in. it runs in real mode at a fixed low
# address, which is nowhere the linker would put anything, so it is
# assembled flat and carried inside the kernel as bytes
$(OBJDIR)/arch/x86_64/trampoline.bin: arch/x86_64/trampoline.asm
	@mkdir -p $(@D)
	$(NASM) -f bin $< -o $@

$(OBJDIR)/arch/x86_64/trampoline.c: $(OBJDIR)/arch/x86_64/trampoline.bin tools/bin2c.py
	@python3 tools/bin2c.py smp_trampoline $< > $@

$(OBJDIR)/arch/x86_64/trampoline.c.o: $(OBJDIR)/arch/x86_64/trampoline.c
	$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJ:.o=.d)

# the ramdisk is a plain tar. --format=ustar because thats the one the
# kernel knows how to read, and the flags after it keep the archive
# byte-identical between builds so the image doesnt churn.
#
# this one carries no programs but the kernel's own selftests, below. it
# is the smallest medium the kernel can be booted and checked on (the
# accounts, the profile the shell reads at login, the files the boot test
# reads back, and selftests/), and a system built on this kernel packs
# its own
RAMDISK := $(BINDIR)/ramdisk.tar
RAMDISK_FILES := $(shell find base/ramdisk -type f 2>/dev/null)

#
# the selftests: programs, in tools/testing/selftests/, that check from
# ring 3 what the kernel promises ring 3. a directory is a subsystem and
# a .c file is a test, so tools/testing/selftests/sched/wait.c is
# selftests/sched/wait on the medium.
#
# a test is compiled with -nostdinc against the uapi headers and the
# compiler's freestanding ones and nothing else, the headers a program
# gets. so it cannot lean on how the kernel is built inside, only on what
# it promises, and a test that starts failing is a promise broken
SELFTEST_DIR  := tools/testing/selftests
SELFTESTS     := $(sort $(patsubst $(SELFTEST_DIR)/%.c,%, \
                   $(shell find $(SELFTEST_DIR) -mindepth 2 -name '*.c' \
                           2>/dev/null)))
SELFTEST_BINS := $(addprefix $(BINDIR)/selftests/,$(SELFTESTS))

# what goes on the medium: those tests and the directories they are in,
# named one by one rather than the whole of bin/selftests/, so a binary
# left behind by a test that has since gone does not ship
SELFTEST_TAR := $(sort ./selftests \
                  $(patsubst $(BINDIR)/%/,./%,$(dir $(SELFTEST_BINS))) \
                  $(patsubst $(BINDIR)/%,./%,$(SELFTEST_BINS)))

# and that list written down, rewritten only when it changes. make sees
# a prerequisite that got newer and never one that is gone, so without
# this a test removed from the tree would stay in a ramdisk nothing
# rebuilds
SELFTEST_LIST := $(OBJROOT)/selftests.list

$(SELFTEST_LIST): FORCE
	@mkdir -p $(@D)
	@echo '$(SELFTEST_TAR)' > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv -f $@.tmp $@

.PHONY: FORCE
FORCE:

SELFTEST_CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -ffreestanding \
                   -fno-stack-protector -fno-stack-check -fno-pic -fno-pie \
                   -fno-omit-frame-pointer -m64 -mno-80387 -mno-mmx \
                   -mno-sse -mno-sse2 -mno-red-zone -nostdinc \
                   -isystem $(shell $(CC) -print-file-name=include) \
                   -Iinclude/uapi
SELFTEST_LDFLAGS := -nostdlib -static -no-pie -Wl,--build-id=none \
                    -T $(SELFTEST_DIR)/selftest.ld

$(BINDIR)/selftests/%: $(SELFTEST_DIR)/%.c $(SELFTEST_DIR)/kselftest.h \
                       $(SELFTEST_DIR)/selftest.ld \
                       $(wildcard include/uapi/velvet/*.h)
	@mkdir -p $(@D)
	$(CC) $(SELFTEST_CFLAGS) $(SELFTEST_LDFLAGS) $< -o $@

$(RAMDISK): $(RAMDISK_FILES) $(SELFTEST_BINS) $(SELFTEST_LIST)
	@mkdir -p $(@D)
	@# git only tracks the execute bit, so the modes that matter are set
	@# here rather than trusted to the checkout. velvet-room.txt is the
	@# one the kernel refuses to a guest
	@chmod 644 base/ramdisk/passwd base/ramdisk/*.txt 2>/dev/null || true
	@chmod 600 base/ramdisk/velvet-room.txt
	tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner \
		--mtime=@0 -cf $@ -C base/ramdisk . \
		-C $(abspath $(BINDIR)) --no-recursion $(SELFTEST_TAR)

#
# 0.3.26. everything above builds a machine; this puts the thing it was
# built *from* on the same medium, so that a disk which boots is also a
# disk you could rebuild from.
#
# it is another ustar, for the plainest of reasons: the kernel already
# reads one, and a second container format would be a second parser to
# get wrong. it is *not* compressed, which is a decision rather than an
# oversight: the one moment anybody wants this archive is the moment
# something has gone badly wrong, and a recovery path that needs a
# decompressor between you and the source is a recovery path with one
# more thing in it to be broken.
#
# what goes in: everything the project is made of, which is a longer
# list than "everything the compiler reads". the tests are the reason
# any of this is trustworthy and the prose is where the reasoning
# lives, so both ship. what stays out is anything the build *produces*
# (bin/, obj/, the images), because a source tree carrying its own
# output is a source tree that disagrees with itself the moment you
# build it.
#
# this is the kernel's own archive, used when nobody names another. a
# system that ships the kernel inside a bigger tree passes SOURCE, and
# then this rule is not asked for at all
OWN_SOURCE := $(BINDIR)/source.tar
SOURCE ?= $(OWN_SOURCE)

SOURCE_FILES := GNUmakefile sources.mk README.md CHANGELOG.md ROADMAP.md \
                LICENSE .gitignore \
                CONTRIBUTING.md CODE_OF_CONDUCT.md \
                $(shell find .github $(KERNEL_PORTABLE) \
                        arch boot tests tools docs -type f | sort) \
                $(shell find base/ramdisk base/diskroot -type f | sort)

# --sort=name and a fixed mtime for the same reason the ramdisk has
# them: two builds of the same tree must produce the same bytes, or the
# stamp below is a fact about when I built it rather than about what I
# built.
#
# --mode is there for a subtler version of the same thing. git records
# one bit of a mode, the ramdisk rule above sets others by hand, and the
# result is that base/ramdisk/velvet-room.txt is 0600 after a build and
# 0644 in a fresh checkout, so the archive, and therefore the stamp,
# would depend on whether anybody had built anything yet. it states what
# it wants instead: everyone may read, only the owner may write, and
# whatever was executable stays executable
$(OWN_SOURCE): $(SOURCE_FILES)
	@mkdir -p $(@D)
	@echo '  TAR     $@ ($(words $(SOURCE_FILES)) files)'
	@tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner \
		--mtime=@0 --mode='u+rw,go+r,go-w' -cf $@ $(SOURCE_FILES)

# and the kernel is told what its own source hashes to.
#
# without this the machine can say "there is a source tree here" and
# nothing more, which is the weaker half of what is wanted. a disk that
# survives its author has to be able to answer "is this the source this
# kernel was built from?", and the only way to answer it is to have
# written the answer down at the one moment both halves were in the
# same room, which is now.
#
# the cost is that editing anything at all, a line of the roadmap
# included, relinks the kernel. that is not overhead so much as the
# claim being true: a kernel whose stamp survived an edit to its source
# would be a kernel telling a lie.
$(OBJDIR)/srcstamp.c: $(SOURCE) tools/srcstamp.py
	@mkdir -p $(@D)
	@python3 tools/srcstamp.py $(SOURCE) > $@

$(OBJDIR)/srcstamp.o: $(OBJDIR)/srcstamp.c include/velvet/source.h
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -c $< -o $@

#
# there was a borrowed one here until 0.1.12. writing a bootloader and
# then booting with somebody else's is not much of a bootloader, so it
# is gone, along with the iso, the uefi path and the protocol that
# came with it. the kernel is handed one struct now, in rdi, and knows
# nothing about anybody's boot protocol including mine.

BOOTCC    := gcc
BOOTCFLAGS := -std=gnu11 -ffreestanding -fno-pic -fno-stack-protector \
              -mno-red-zone -mno-sse -mno-mmx -mno-80387 -mcmodel=small \
              -Wall -Wextra -O2 -Iinclude

$(BINDIR)/boot/philemon.bin: boot/philemon.asm boot/philemon.inc
	@mkdir -p $(@D)
	$(NASM) -f bin $< -o $@

$(BINDIR)/boot/philemon64.bin: boot/philemon.c include/velvet/philemon.h boot/philemon.ld
	@mkdir -p $(@D) $(OBJROOT)
	$(BOOTCC) $(BOOTCFLAGS) -c boot/philemon.c -o $(OBJROOT)/philemon.o
	$(LD) -T boot/philemon.ld -nostdlib -static --no-warn-rwx-segments \
		$(OBJROOT)/philemon.o -o $(OBJROOT)/philemon.elf
	objcopy -O binary $(OBJROOT)/philemon.elf $@

$(BOOTIMG): $(BINDIR)/boot/philemon.bin $(BINDIR)/boot/philemon64.bin \
            $(BINDIR)/$(KERNEL) $(RAMDISK) $(SOURCE) tools/mkboot.py \
            boot/philemon.inc include/velvet/philemon.h
	@python3 tools/mkboot.py $@ $(BINDIR)/boot/philemon.bin \
		$(BINDIR)/boot/philemon64.bin $(BINDIR)/$(KERNEL) $(RAMDISK) $(SOURCE)

bootimg: $(BOOTIMG)

# two drives: the one philemon is on, and the one with the files. the
# boot image goes first and is named as the boot device twice over:
# `order=c` so the bios does not go looking for a floppy it has not got,
# and a bootindex on each so the order is not left to chance.
#
# the kernel does not care which is which: it tries every drive until one
# has a filesystem it recognises, and the boot image has none
#   make run            four cpus, 2G
#   make run CPUS=1     one, which must always still work
#   make run MEM=512M QEMU_EXTRA="-d int"
#
# ports 5555 (tcp) and 5556 (udp) are forwarded in from the host, so a
# program running *inside* can be reached from outside. without them the
# guest can only ever make outbound connections, which tests half of tcp
# and none of a server.
#
# and the forward is the *only* way in. qemu's user-mode networking is a
# userspace nat, so 10.0.2.15 is not an address the host can route to:
# `nc -u 10.0.2.15 5556` from outside goes nowhere at all, silently,
# which looks exactly like a guest that is ignoring its own port. it is
# `nc -u localhost 5556` that arrives
run: $(BOOTIMG) $(DISK)
	qemu-system-x86_64 -M q35 -m $(MEM) -smp $(CPUS) -serial stdio \
		-boot order=c \
		-drive id=boot,file=$(BOOTIMG),format=raw,if=none \
		-device ide-hd,drive=boot,bus=ide.0,bootindex=0 \
		-drive id=data,file=$(DISK),format=raw,if=none \
		-device ide-hd,drive=data,bus=ide.1,bootindex=1 \
		-netdev user,id=n0,hostfwd=tcp::5555-:5555,hostfwd=udp::5556-:5556 \
		-device e1000,netdev=n0 \
		$(QEMU_EXTRA)

# the disk, which is a real filesystem rather than an archive: built out
# of whatever is in base/diskroot/, and attached to qemu as a second
# drive. it is deliberately NOT rebuilt by `make run` once it exists:
# the whole point is that what you write to it stays written, and
# regenerating it every boot would quietly undo that. `make disk` starts
# over when you want a clean one.
#
# three partitions: the system, somewhere to *work* that is not the
# system, and a fat32 one, so that `mount <n>` has something of another
# kind to do. the system partition is built by the C formatter, so the
# disk the machine boots is a real ext4, extents and all. the work image
# is made smaller than the partition it goes in, and that is deliberate:
# 8 MiB of filesystem inside 24 MiB of partition is exactly the situation
# `resize` exists for, so `make run` boots a machine with something to
# grow
$(DISK): $(T)/mkext4 tools/mkfat.py tools/mkdisk.py $(shell find base/diskroot -type f 2>/dev/null)
	@mkdir -p $(BINDIR)/workroot
	@echo 'somewhere to work that is not the system' > $(BINDIR)/workroot/readme.txt
	@$(T)/mkext4 $(BINDIR)/diskroot.img base/diskroot 32 >/dev/null
	@$(T)/mkext4 $(BINDIR)/work.img $(BINDIR)/workroot 8 work >/dev/null
	@dd if=/dev/zero of=$(BINDIR)/work.img bs=1M count=0 seek=24 2>/dev/null
	@python3 tools/mkfat.py  $(BINDIR)/compendium.img base/diskroot 24 >/dev/null
	@python3 tools/mkdisk.py $@ gpt $(BINDIR)/diskroot.img:linux $(BINDIR)/work.img:work \
		$(BINDIR)/compendium.img:fat32

.PHONY: disk
disk:
	@rm -f $(DISK)
	@$(MAKE) --no-print-directory $(DISK)

#
# the testable guts of the kernel are deliberately split from the parts
# that touch hardware: pmm_init_from_map() takes a memory map instead of
# asking the loader, keyboard_feed() takes a scancode instead of reading a
# port, and so on. that lets all of this run as ordinary linux programs.
# VELVETOS_HOSTED turns irq_save/irq_restore into no-ops, since userspace
# gets shot for saying cli.

HOSTCC    := gcc
HOSTFLAGS := -std=gnu11 -Wall -Wextra -Wno-stringop-truncation -Wno-stringop-overread -g -DVELVETOS_HOSTED -I. -Iinclude -Iarch/x86_64/include -I$(OBJROOT)/include -DVELVETOS_ARCH_X86_64

TEST_BINS := $(T)/kprintf $(T)/mm $(T)/buddy $(T)/slab \
             $(T)/vmm $(T)/gdt \
             $(T)/ksyms $(T)/rtc $(T)/ramdisk $(T)/elf \
             $(T)/addrspace $(T)/process \
             $(T)/syscall $(T)/tty $(T)/auth $(T)/acpi $(T)/pci \
             $(T)/keyboard $(T)/serial \
             $(T)/fat32 $(T)/vfs $(T)/philemon \
             $(T)/locks $(T)/path \
             $(T)/shell $(T)/pipe $(T)/bcache \
             $(T)/ext4 $(T)/part $(T)/console \
             $(T)/mouse $(T)/init $(T)/mkfs \
             $(T)/install $(T)/net $(T)/dhcp \
             $(T)/tcp $(T)/dns $(T)/http \
             $(T)/signal $(T)/ansi \
             $(T)/termios \
             $(T)/epoch $(T)/pressure $(T)/hash \
             $(T)/inflate $(T)/crash \
             $(T)/resize $(T)/switch $(T)/source \
             $(T)/proc

$(T)/kprintf:  tests/test_kprintf.c  lib/kprintf.c \
               sched/spinlock.c
$(T)/proc:     tests/test_proc.c     fs/proc.c lib/kprintf.c \
               lib/string.c sched/spinlock.c
$(T)/mm:       tests/test_mm.c       sched/spinlock.c \
               mm/pmm.c \
               mm/buddy.c mm/slab.c \
               mm/kmalloc.c lib/string.c
$(T)/buddy:    tests/test_buddy.c    mm/buddy.c \
               lib/string.c
$(T)/slab:     tests/test_slab.c     sched/spinlock.c \
               mm/slab.c \
               mm/pmm.c mm/buddy.c \
               mm/kmalloc.c lib/string.c
$(T)/addrspace: tests/test_addrspace.c mm/addrspace.c mm/pressure.c \
               mm/vmm.c mm/slab.c \
               sched/spinlock.c lib/string.c
$(T)/vmm:      tests/test_vmm.c      mm/vmm.c \
               lib/string.c
$(T)/ksyms:    tests/test_ksyms.c    lib/ksyms.c
$(T)/rtc:      tests/test_rtc.c      drivers/rtc.c lib/epoch.c \
               sched/spinlock.c
$(T)/process:  tests/test_process.c  sched/spinlock.c \
               fs/path.c sched/process.c sched/signal.c \
               lib/env.c lib/string.c
$(T)/pci:      tests/test_pci.c      drivers/pci.c \
               lib/string.c
$(T)/acpi:     tests/test_acpi.c     arch/x86_64/acpi.c \
               lib/string.c
$(T)/auth:     tests/test_auth.c     sched/auth.c \
               lib/string.c
$(T)/tty:      tests/test_tty.c      sched/spinlock.c \
               drivers/tty.c drivers/termios.c \
               sched/process.c sched/signal.c lib/env.c lib/string.c
# the real pipe, not a stub: what is under test here is that stdin and
# stdout end up somewhere other than the terminal when a pipeline says
# they should, and a stubbed pipe could only ever agree with itself
$(T)/syscall:  tests/test_syscall.c  sched/spinlock.c \
               fs/path.c arch/x86_64/syscall.c \
               sched/process.c sched/signal.c lib/string.c \
               lib/env.c \
               fs/vfs.c fs/source.c fs/proc.c \
               lib/hash.c fs/pipe.c
$(T)/elf:      tests/test_elf.c      fs/elf.c \
               lib/string.c
$(T)/ramdisk:  tests/test_ramdisk.c  fs/ramdisk.c \
               lib/string.c
$(T)/fat32:    tests/test_fat32.c    fs/fat32.c \
               lib/string.c
# the one suite that can see a race: real threads through the real
# allocators, so it needs the real thread library
$(T)/locks:    tests/test_locks.c    sched/spinlock.c \
               mm/pmm.c mm/buddy.c \
               mm/slab.c mm/kmalloc.c \
               lib/string.c
$(T)/locks:    LDLIBS = -pthread

$(T)/path:     tests/test_path.c     fs/path.c
$(T)/pipe:     tests/test_pipe.c     fs/pipe.c \
               sched/spinlock.c lib/string.c
$(T)/bcache:   tests/test_bcache.c   fs/bcache.c \
               lib/string.c
$(T)/ext4:     tests/test_ext4.c     fs/ext4.c fs/jbd2.c \
               lib/string.c
# the formatter, checked against the *driver* rather than against itself.
# a formatter and a reader by one author can agree on something wrong and
# both be happy, which is exactly what happened to mkfat.py in 0.1.10
$(T)/mkfs:     tests/test_mkfs.c     fs/mkfs.c \
               fs/ext4.c fs/jbd2.c \
               lib/string.c
# almost all of a network stack is arithmetic over a byte buffer, so
# almost all of it can be checked with no card and nothing on the wire.
# the fixtures that matter are real captured packets: a header I
# invent and a parser I invent agree with each other and with nothing
# else on earth
# a response arrives as whatever pieces tcp felt like delivering, so the
# central test is not "does this parse" but "does the split point
# matter": every fixture is driven through split at every byte offset
$(T)/http:     tests/test_http.c     net/http.c \
               tests/fixtures/http_captured.h \
               net/net.c lib/string.c
# the first protocol here whose other end is somebody else's software,
# so most of the suite is malformed replies, and nearly all of that is
# name compression, where a pointer may point backwards, forwards, or at
# itself, and nothing in the format forbids a cycle
$(T)/dns:      tests/test_dns.c      net/dns.c \
               net/net.c lib/string.c
# the largest state machine in the kernel, and the one whose bugs are
# least visible from outside: a stack that gets the close sequence
# wrong works perfectly for every connection anybody watches
$(T)/tcp:      tests/test_tcp.c      net/tcp.c \
               net/net.c lib/string.c
# the first protocol here that remembers anything, so it gets a suite of
# its own: the interesting behaviour is in the *sequence*, and a clock
# that is an ordinary variable is what makes an eight-second backoff and
# a twelve-hour lease testable at all
$(T)/dhcp:     tests/test_dhcp.c     net/dhcp.c \
               net/net.c lib/string.c
$(T)/net:      tests/test_net.c      net/net.c \
               net/ether.c net/arp.c \
               net/ip.c net/icmp.c \
               net/udp.c net/socket.c \
               lib/string.c
$(T)/install:  tests/test_install.c  fs/install.c \
               fs/mkfs.c drivers/part.c lib/hash.c \
               fs/ext4.c fs/jbd2.c \
               lib/string.c
$(T)/part:     tests/test_part.c     drivers/part.c \
               lib/string.c lib/hash.c
# sha-1, crc32 and adler32. a suite when run with no arguments and a
# hashing tool when run with some, which is how tools/hashcheck.py
# drives it against python, zlib and git
$(T)/hash:     tests/test_hash.c     lib/hash.c \
               lib/string.c
# deflate and inflate. a suite with no arguments, a compressor or a
# decompressor with some, which is how tools/zlibcheck.py runs it
# against python's zlib and against a real git repository
$(T)/inflate:  tests/test_inflate.c  lib/inflate.c \
               lib/deflate.c lib/hash.c \
               lib/string.c
# the six python build tools, rewritten in C. not suites: they are
# programs, and nothing they produce can judge itself, so they build
# outside TEST_BINS and tools/toolcheck.py is what runs them.
#
# and built *beside* the python rather than instead of it, so that
# toolcheck has something to compare against: a swap that changes the
# build's output by accident is the failure this version is most likely
# to have, and it would look exactly like a build that worked
TOOL_BINS := $(T)/bin2c $(T)/checkfmt $(T)/gensyms \
             $(T)/mkboot $(T)/mkext4 $(T)/mkfat \
             $(T)/fsck

tools: $(TOOL_BINS)
	@:

$(T)/bin2c:    tools/bin2c.c
$(T)/checkfmt: tools/checkfmt.c
# gensyms reads the symbol table out of the ELF itself rather than
# asking nm, which is the point: a self-hosting machine has no more
# binutils on it than it has python
$(T)/gensyms:  tools/gensyms.c
$(T)/mkboot:   tools/mkboot.c
# not a rewrite so much as a driver: mkfs.c already formats the
# filesystem and ext4.c already writes files, and both are tested. what
# was missing was a way to point them at a host file and a host
# directory
$(T)/mkext4:   tools/mkext4.c        fs/mkfs.c \
               fs/ext4.c fs/jbd2.c \
               lib/string.c
# the one with code of its own in it: fat32.c mounts, creates and
# writes, but nothing anywhere formatted a fat32 volume. so the boot
# sector, the two tables and the root cluster are laid out here, and the
# files go in through the same driver the kernel uses
$(T)/mkfat:    tools/mkfat.c         fs/fat32.c \
               lib/string.c
# and the checker, the same driver-over-a-host-file shape: fs/fsck.c is
# what the machine runs, this points it at an image
$(T)/fsck:     tools/fsck.c          fs/fsck.c \
               lib/string.c
# pulling the plug at every write there is. the driver, the formatter
# and the checker all at once, because what is under test is not any one
# of them: it is whether the *order* they write in leaves damage that
# has an answer
$(T)/crash:    tests/test_crash.c    fs/ext4.c \
               fs/jbd2.c \
               fs/mkfs.c fs/fsck.c \
               lib/string.c
# growing a filesystem onto a disk bigger than the one it was made for.
# the formatter, the driver and the checker again, and for the same
# reason: what is under test is whether a resize leaves a filesystem,
# which is not a question the thing doing the resizing may answer
$(T)/resize:   tests/test_resize.c   fs/ext4.c \
               fs/jbd2.c \
               fs/mkfs.c fs/fsck.c \
               lib/string.c

# the framebuffer is an array here, so what the screen shows can be read
# back. every path that touches pixels ends up in one place, which is
# the whole reason a console is testable without a machine
$(T)/console:  tests/test_console.c  drivers/console.c \
               drivers/ansi.c \
               drivers/font.c lib/string.c
# only the decoder, which is the half with no hardware in it, and the
# half where losing sync looks like a hardware fault and is not
$(T)/mouse:    tests/test_mouse.c    drivers/mouse.c \
               lib/string.c
# only the policy: what init decides about a service that keeps dying.
# the half that starts threads and takes the machine down is the half
# with a machine in it, and is #ifdef'd out here
$(T)/init:     tests/test_init.c     sched/init.c \
               lib/string.c
# delivery needs a machine and the rules do not, and the rules are
# where signals go wrong: one that can be caught when it must not be, a
# handler re-entering itself, a pending set surviving a fork
$(T)/signal:   tests/test_signal.c   sched/signal.c \
               lib/string.c
# the escape sequences a program written for a real terminal sends. a
# pure state machine, and fed one byte at a time by the suite because
# that is what a program writing \033 and [2J separately produces
$(T)/ansi:     tests/test_ansi.c     drivers/ansi.c
# what a terminal does about a key. the reading needs a machine and
# the deciding does not, and the deciding is where ctrl+c arrives as
# a signal or as the byte 0x03
$(T)/termios:  tests/test_termios.c  drivers/termios.c
# a date into a number. checked against the host's own timegm, which
# is somebody else's implementation and the only opinion worth having
$(T)/epoch:    tests/test_epoch.c    lib/epoch.c
# who gets refused when memory runs out. a policy rather than a data
# structure, and its interesting cases are all at the edges
$(T)/pressure: tests/test_pressure.c mm/pressure.c
# the source mount is part of the namespace, so the suite that is
# responsible for resolution builds the real one and hands it a medium
# made of bytes rather than a drive
$(T)/vfs:      tests/test_vfs.c      fs/vfs.c \
               fs/ramdisk.c fs/source.c \
               lib/hash.c lib/string.c

# and the archive on its own: a tar read a sector at a time off
# something that is not a filesystem, which is the one thing here that
# no other suite covers
$(T)/source:   tests/test_source.c   fs/source.c \
               lib/hash.c lib/string.c
$(T)/philemon: tests/test_philemon.c boot/philemon.c include/velvet/philemon.h
$(T)/gdt:      tests/test_gdt.c      arch/x86_64/gdt.c
$(T)/gdt:      SRCS = tests/test_gdt.c
$(T)/keyboard: tests/test_keyboard.c drivers/keyboard.c \
               drivers/input.c \
               sched/spinlock.c
$(T)/serial:   tests/test_serial.c   drivers/serial.c \
               drivers/input.c \
               sched/spinlock.c
# the real process table, because the environment lives in it and the
# rules about what "already set" means are worth running rather than
# restating in a stub, and the real init policy for the same reason,
# so that what `init` prints is what init would actually have decided
$(T)/shell:    tests/test_shell.c    lib/string.c \
               fs/ramdisk.c sched/auth.c \
               drivers/pci.c drivers/pciinfo.c fs/vfs.c \
               fs/proc.c fs/diskinfo.c mm/meminfo.c \
               fs/source.c \
               sched/spinlock.c fs/path.c \
               lib/env.c sched/init.c \
               fs/install.c fs/mkfs.c \
               drivers/part.c lib/hash.c fs/fsck.c \
               net/net.c net/arp.c \
               net/dhcp.c net/tcp.c \
               net/dns.c net/ip.c mm/pressure.c \
               sched/signal.c \
               net/http.c \
               shell/shell.c
$(T)/shell:    SRCS = tests/test_shell.c lib/string.c \
                      fs/ramdisk.c sched/auth.c \
                      drivers/pci.c drivers/pciinfo.c fs/vfs.c \
                      fs/proc.c fs/diskinfo.c mm/meminfo.c \
                      fs/source.c \
                      sched/spinlock.c fs/path.c \
                      lib/env.c sched/init.c \
                      fs/install.c fs/mkfs.c \
                      drivers/part.c lib/hash.c fs/fsck.c \
                      net/net.c net/arp.c \
                      net/dhcp.c net/tcp.c \
                      net/dns.c net/ip.c \
                      net/http.c sched/signal.c \
                      mm/pressure.c

# every test depends on every kernel header.
#
# coarse on purpose. the kernel's own objects rebuild correctly because
# they are compiled one at a time with -MMD and the .d files are
# included; the tests are compiled straight to a binary in one step and
# had no header prerequisites at all. so editing a header and running
# `make test` ran the *previous* binaries and reported them passing,
# which is the worst possible failure for a test suite, since it is
# indistinguishable from the change being harmless.
#
# found by breaking a function in tcp.h on purpose and watching the
# suite pass anyway. rebuilding every test when any header changes costs
# a few seconds and removes the whole class of it.
TEST_HEADERS := $(shell find $(KERNEL_PORTABLE) arch boot tools tests -name '*.h' 2>/dev/null)
$(TEST_BINS): $(TEST_HEADERS) $(VERSION_H)
$(TOOL_BINS): $(TEST_HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTFLAGS) $(filter %.c,$^) -o $@

# SRCS overrides what gets compiled, for tests that #include a kernel
# .c file directly: that file still belongs in the prerequisites so
# make rebuilds when it changes, but compiling it twice would give me
# duplicate symbols
$(filter-out $(T)/switch,$(TEST_BINS)):
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTFLAGS) $(if $(SRCS),$(SRCS),$(filter %.c,$^)) -o $@ $(LDLIBS)

# the switch test calls into the real switch.asm, and needs -no-pie so
# the `callq switch_context` in its inline asm resolves
$(OBJROOT)/tests/switch.asm.o: arch/x86_64/switch.asm
	@mkdir -p $(@D)
	$(NASM) -f elf64 $< -o $@

$(T)/switch: tests/test_switch.c $(OBJROOT)/tests/switch.asm.o
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTFLAGS) -no-pie $(filter %.c %.o,$^) -o $@

# the C build tools against the python they replace, while both exist
.PHONY: toolcheck
toolcheck: $(TOOL_BINS) $(BINDIR)/$(KERNEL) $(BINDIR)/boot/philemon.bin \
           $(BINDIR)/boot/philemon64.bin $(RAMDISK) $(SOURCE) ext4-image
	@python3 tools/toolcheck.py

# deflate against python's zlib, both ways round, at every compression
# level: the level decides which of the three block types comes out,
# so a decompressor checked only against our own compressor has seen one
# third of the format. plus the loose objects of a real git repository
.PHONY: zlibcheck
zlibcheck: $(T)/inflate
	@python3 tools/zlibcheck.py

# the three sums against python, zlib and git. a published vector only
# ever tests one buffer hashed in one go, which is the case that cannot
# fail, so this feeds three hundred lengths through in pieces of 1, 7,
# 63, 64 and 65 bytes as well
.PHONY: hashcheck
hashcheck: $(T)/hash
	@python3 tools/hashcheck.py

.PHONY: test
# the fat32 suite runs against a real filesystem rather than a fixture,
# so it gets a throwaway image built by the same tool that builds the
# real one. rebuilt every time, without fail: the tests write to it, and
# a suite that passes only on a disk its last run left behind is worse
# than no suite at all
#
# and when the suite has finished writing to it, tools/readfat.py reads
# it back, the `fatfsck` step below. that is not the same check as
# toolcheck's: toolcheck judges what the *formatter* wrote, this judges
# what the *driver* wrote, which since 0.3.21 includes long names it
# mints itself. a long name written wrong reads back perfectly here,
# because the code reading it is the code that wrote it, and is a
# different file on every other machine the disk is plugged into
.PHONY: fat32-image
fat32-image:
	@mkdir -p $(T)
	@rm -f $(T)/fat32.img
	@python3 tools/mkfat.py $(T)/fat32.img base/diskroot 64 >/dev/null

# two partitioned disks for the partition suite: one of each scheme,
# built by tools/mkdisk.py, which was written from the specification
# separately from the parser that reads them back
.PHONY: part-images
part-images: ext4-image
	@mkdir -p $(T)
	@rm -f $(T)/mbr.img $(T)/gpt.img $(T)/small.img
	@python3 tools/mkext4.py $(T)/small.img base/diskroot 4 >/dev/null
	@python3 tools/mkdisk.py $(T)/mbr.img mbr \
		$(T)/small.img:linux $(T)/small.img:fat32 >/dev/null
	@python3 tools/mkdisk.py $(T)/gpt.img gpt \
		$(T)/small.img:linux $(T)/small.img:linux >/dev/null

# the ext4 suite gets a throwaway image too, and one with more in it
# than base/diskroot has: a file past twelve blocks so the indirect block is
# exercised, one past twelve plus two hundred and fifty six so the
# double indirect one is, and symlinks of both kinds, short enough to
# live in the inode, and not
.PHONY: ext4-image
ext4-image: $(T)/mkext4
	@mkdir -p $(T)/ext4root/sub $(T)/ext4root/notes
	@cp -r base/diskroot/. $(T)/ext4root/ 2>/dev/null || true
	@python3 -c "open('$(T)/ext4root/indirect.bin','wb').write(bytes((i*7+3)&0xff for i in range(20000)))"
	@python3 -c "open('$(T)/ext4root/double.bin','wb').write(bytes((i*13+5)&0xff for i in range(400000)))"
	@echo deep > $(T)/ext4root/notes/deep.txt
	@echo nested > $(T)/ext4root/sub/nested.txt
	@rm -f $(T)/ext4root/sub/link $(T)/ext4root/slowlink
	@ln -s ../indirect.bin $(T)/ext4root/sub/link
	@ln -s /a/very/long/target/path/that/will/not/fit/in/sixty/bytes/at/all/no $(T)/ext4root/slowlink
	@rm -f $(T)/ext4.img
# built by the C tool rather than the python, and that is the whole
# point of it since 0.3.22: mkfs.c writes a filesystem with the extents
# feature set and mkext4 populates it through fs/ext4.c, so the image
# the suite runs against is one whose files are mapped by extent trees
# the driver built itself. the python formatter still writes ext2, and
# still matters: toolcheck holds the two against each other, which is
# what keeps the driver honest about reading both
	@$(T)/mkext4 $(T)/ext4.img $(T)/ext4root 16 >/dev/null

# the shell resolves a name by looking for a file of that name under
# PATH, and completes from the same place, so its suite needs a /bin to
# look in. this tree's medium has none, since a system built on the
# kernel brings its own programs, so the suite gets the medium with the
# names velvetOS shipped when the two were split, as empty files.
# resolving a name opens the file and nothing ever reads it, and the
# list is fixed on purpose: completion is checked against exactly which
# names are there, and a /bin that changed under it would change what a
# unique prefix is
SHELL_FIXTURE_PROGS := hello counter fail reader parent ask echo cat \
                       uptime ls whoami mkdir rmdir rm cp mv touch head \
                       wc grep sort margaret gemini chmod chown ln \
                       tartarus hermes theodore patience diff find less \
                       wordcount sha1sum git

$(T)/shell.tar: $(RAMDISK)
	@rm -rf $(T)/shellroot
	@mkdir -p $(T)/shellroot/bin
	@tar -xf $(RAMDISK) -C $(T)/shellroot
	@for p in $(SHELL_FIXTURE_PROGS); do : > $(T)/shellroot/bin/$$p; done
	@tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner \
		--mtime=@0 -cf $@ -C $(T)/shellroot .

# the kernel itself is a prerequisite, and that is not decoration. twice
# now every host suite has passed while the kernel did not link: disk.c
# and shell.c are compiled into no host test, so nothing but building
# the real thing catches a missing function there
# and the boot image, because since 0.3.26 there is a check that starts
# from the image rather than from anything the build says about it
test: checkfmt checkarch portable-check $(BINDIR)/$(KERNEL) $(BOOTIMG) \
      $(RAMDISK) $(T)/shell.tar $(TEST_BINS) $(TOOL_BINS) \
      fat32-image ext4-image part-images
	@fail=0; \
	for t in $(TEST_BINS); do \
		printf '  %-10s ' "$$(basename $$t)"; \
		case $$t in *fat32) arg=$(T)/fat32.img;; *) arg=;; esac; \
		if out=$$(./$$t $$arg 2>&1); then \
			echo 'ok'; \
		else \
			echo 'FAILED'; echo "$$out" | sed 's/^/    /'; fail=1; \
		fi; \
	done; \
	printf '  %-10s ' fatfsck; \
	if out=$$(python3 tools/readfat.py $(T)/fat32.img 2>&1); then \
		echo 'ok'; \
	else \
		echo 'FAILED'; echo "$$out" | sed 's/^/    /'; fail=1; \
	fi; \
	printf '  %-10s ' fsckcheck; \
	if out=$$(python3 tools/fsckcheck.py 2>&1); then \
		echo 'ok'; \
	else \
		echo 'FAILED'; echo "$$out" | sed 's/^/    /'; fail=1; \
	fi; \
	printf '  %-10s ' fsck; \
	if out=$$(python3 tools/readext4.py $(T)/ext4.img 2>&1); then \
		echo 'ok'; \
	else \
		echo 'FAILED'; echo "$$out" | sed 's/^/    /'; fail=1; \
	fi; \
	printf '  %-10s ' mkfsck; \
	if out=$$(python3 tools/readext4.py $(T)/mkfs.img 2>&1); then \
		echo 'ok'; \
	else \
		echo 'FAILED'; echo "$$out" | sed 's/^/    /'; fail=1; \
	fi; \
	printf '  %-10s ' growcheck; \
	if out=$$(python3 tools/readext4.py $(T)/resize.img 2>&1); then \
		echo 'ok'; \
	else \
		echo 'FAILED'; echo "$$out" | sed 's/^/    /'; fail=1; \
	fi; \
	printf '  %-10s ' toolcheck; \
	if out=$$(python3 tools/toolcheck.py 2>&1); then \
		echo 'ok'; \
	else \
		echo 'FAILED'; echo "$$out" | sed 's/^/    /'; fail=1; \
	fi; \
	printf '  %-10s ' zlibcheck; \
	if out=$$(python3 tools/zlibcheck.py 2>&1); then \
		echo 'ok'; \
	else \
		echo 'FAILED'; echo "$$out" | sed 's/^/    /'; fail=1; \
	fi; \
	printf '  %-10s ' hashcheck; \
	if out=$$(python3 tools/hashcheck.py 2>&1); then \
		echo 'ok'; \
	else \
		echo 'FAILED'; echo "$$out" | sed 's/^/    /'; fail=1; \
	fi; \
	printf '  %-10s ' srccheck; \
	if out=$$(python3 tools/srccheck.py $(BOOTIMG) 2>&1); then \
		echo 'ok'; \
	else \
		echo 'FAILED'; echo "$$out" | sed 's/^/    /'; fail=1; \
	fi; \
	if [ $$fail -eq 0 ]; then echo '  all suites passed'; else exit 1; fi

# the ext2 image is checked *after* the suite has finished writing to
# it, by a reader written from the on-disk layout rather than from the
# driver or the formatter. there is no e2fsck on this machine, so that
# separately written second opinion is the only thing standing between
# "the driver agrees with itself" and "the driver is right"

# gcc checks my format strings against real printf, which accepts far
# more than my kprintf implements. this catches the difference.
.PHONY: checkfmt
checkfmt:
	@printf '  %-10s ' checkfmt
	@python3 tools/checkfmt.py $(KERNEL_PORTABLE) arch && echo 'ok'

.PHONY: fuzz
fuzz: $(T)/fuzz_parsers
	@$(T)/fuzz_parsers

$(T)/fuzz_parsers: tools/fuzz_parsers.c net/dns.c net/http.c \
                   net/tcp.c net/net.c lib/string.c
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTFLAGS) -fsanitize=undefined -fsanitize-trap=undefined \
	    -fno-sanitize-recover=all $(filter %.c,$^) -o $@

# the style guide, as much of it as a machine can hold: the file header,
# the shape of a multi-line comment, the function brace, dashes and first
# person in prose, tabs, the `//` rule, and a marker with an owner. the
# whole tree by default, or the files named in FILES while working on one.
#
#   make lint                   the whole tree
#   make lint FILES=fs/ext4.c   just that file
#
# naming prefixes are warnings rather than errors until the tree has been
# converted to them, which is why a clean run still prints a count.
.PHONY: lint
lint:
	@python3 tools/lint.py $(FILES)

# the arch boundary, which is worth exactly as much as whatever checks
# it. a line drawn in 0.2.20 and not checked would decay the first time
# somebody needed a `hlt` in a hurry, silently, because the kernel goes
# on building and booting perfectly either way
.PHONY: checkarch
checkarch:
	@python3 tools/checkarch.py

# and the half checkarch cannot see. it greps: it catches an #include
# and a line of inline asm, and it cannot catch a portable file that
# quietly *depends* on something only x86 supplies. this compiles the
# portable kernel against an architecture that does nothing, which can.
#
#   make portable-check LIST=1    and what a port would have to supply
.PHONY: portable-check
portable-check: $(VERSION_H)
	@python3 tools/portable.py $(if $(LIST),--list,)

#
# compiles arch/$(ARCH)/ and nothing else, which is the only thing that
# can honestly be said about an architecture with no boot code yet.
# selftest.c calls every contract exactly once, because the headers are
# almost all `static inline`, and an inline nobody calls is one the
# assembler never reads, so a header full of mistyped system registers
# would compile clean and say nothing at all.
ARCH_DIR   := arch/$(ARCH)
ARCH_CC    := $(CROSS)gcc
ARCH_UPPER := $(shell echo $(ARCH) | tr a-z A-Z)
ARCH_FLAGS := -std=gnu11 -Wall -Wextra -g -ffreestanding \
              -fno-stack-protector -fno-stack-check -fno-omit-frame-pointer \
              -DVELVETOS_ARCH_$(ARCH_UPPER) -Iinclude -Iarch/$(ARCH)/include -c

.PHONY: arch-check
arch-check:
	@command -v $(ARCH_CC) >/dev/null 2>&1 || { \
	  echo "  arch-check  no $(ARCH_CC) on PATH."; \
	  echo "              set CROSS= to whatever prefix that architecture uses."; \
	  exit 1; }
	@mkdir -p $(OBJROOT)/archcheck
	@echo "  ARCH        $(ARCH), with $(ARCH_CC)"
	@for f in $(ARCH_DIR)/*.c $(ARCH_DIR)/*.S; do \
	   [ -e "$$f" ] || continue; \
	   printf '  CC          %s\n' "$$f"; \
	   $(ARCH_CC) $(ARCH_FLAGS) "$$f" -o $(OBJROOT)/archcheck/`basename $$f`.o || exit 1; \
	 done
	@echo "  arch-check  ok: $(ARCH) compiles. whether it is correct still needs a machine"

# boot the image and drive the shell over serial. needs qemu
.PHONY: boottest
boottest: $(BOOTIMG) $(T)/mkext4
	./tools/boottest.sh $(BOOTIMG)

# the selftests, run where a program is run: the image booted headless,
# logged in as guest, one `run` each, and every result read back as TAP.
# needs qemu.
#
#   make kselftest                  every test
#   make kselftest TARGETS=sched    the tests of one subsystem
TARGETS ?= $(sort $(patsubst %/,%,$(dir $(SELFTESTS))))

.PHONY: kselftest
kselftest: $(BOOTIMG)
	@python3 $(SELFTEST_DIR)/run_kselftest.py $(BOOTIMG) \
		$(filter $(addsuffix /%,$(TARGETS)),$(SELFTESTS))

clean:
	rm -rf $(BINDIR) $(OBJROOT)

distclean: clean
