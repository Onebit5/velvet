VERSION = 0
PATCHLEVEL = 4
SUBLEVEL = 5
KERNELRELEASE = $(VERSION).$(PATCHLEVEL).$(SUBLEVEL)

# velvet build. needs gcc, nasm, make and python3. `make run` and
# `make boottest` additionally need qemu.
#
# targets:
#   make            -> the bootable image, velvet.img
#   make kernel     -> just the kernel elf, bin/velvet
#   make philemon   -> fetch and build the loader, into bin/boot/
#   make tools      -> the C build tools, bin/tests/
#   make run        -> boot it in qemu (CPUS=4, MEM=2G, QEMU_EXTRA=...)
#   make test       -> the host suites and every checker
#   make boottest   -> boot it headless, power it off, and check what it left
#   make kernelrelease -> print the version
#   make clean
#
# building from somewhere else. a tree that ships this kernel builds it
# out of its own tree, and says where things go rather than letting them
# land in this checkout:
#
#   make -C velvet BINDIR=... OBJROOT=... kernel tools
#
# BINDIR and OBJROOT are where the outputs go.

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

# the loader, and the release of him the image is written with. see the
# rule that fetches him, further down
include philemon-release.mk

PHILEMON_RELEASE := $(OBJROOT)/philemon
PHILEMON_SRC ?= $(PHILEMON_RELEASE)
LOADER := $(BINDIR)/boot/philemon.bin $(BINDIR)/boot/philemon64.bin

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
# an aarch64 port was written and removed again, in 0.2.20. what it
# found while it existed is still here and is most of why 0.2.20's
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
	-DVELVET_ARCH_$(shell echo $(ARCH) | tr a-z A-Z)

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

philemon: $(LOADER)
	@:

kernelrelease:
	@echo $(KERNELRELEASE)

# the version, as C. VERSION, PATCHLEVEL and SUBLEVEL at the top of this
# file are the only place it is written, and the banner and /proc/version
# both print this one macro, so they cannot disagree.
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
# linker and compare the two.
$(BINDIR)/$(KERNEL): $(OBJ) arch/$(ARCH)/linker.ld tools/gensyms.py
	@mkdir -p $(@D) $(OBJDIR)
	@python3 tools/gensyms.py --stub > $(OBJDIR)/ksyms.c
	@$(CC) $(CFLAGS) -c $(OBJDIR)/ksyms.c -o $(OBJDIR)/ksyms.o
	@echo '  LD      pass 1 (to find out where everything landed)'
	@$(LD) $(LDFLAGS) $(OBJ) $(OBJDIR)/ksyms.o -o $@.pass1
	@echo '  GENSYMS $(OBJDIR)/ksyms.c'
	@python3 tools/gensyms.py $@.pass1 > $(OBJDIR)/ksyms.c
	@$(CC) $(CFLAGS) -c $(OBJDIR)/ksyms.c -o $(OBJDIR)/ksyms.o
	@echo '  LD      pass 2 (with the symbols folded in)'
	@$(LD) $(LDFLAGS) $(OBJ) $(OBJDIR)/ksyms.o -o $@
	@python3 tools/gensyms.py --check $@ $(OBJDIR)/ksyms.c
	@echo $(OBJ) $(OBJDIR)/ksyms.o > $(OBJDIR)/kernel.objects
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
# this one carries the kernel's own selftests, below, and an /init that
# runs them, and nothing else: no accounts, no greetings, no profile. it
# is the smallest medium the kernel can be booted and checked on, and a
# system built on this kernel packs its own
RAMDISK := $(BINDIR)/ramdisk.tar

#
# the selftests: programs, in tools/testing/selftests/, that check from
# ring 3 what the kernel promises ring 3. a directory is a subsystem and
# a .c file is a test, so tools/testing/selftests/sched/wait.c is
# selftests/sched/wait on the medium.
#
# a test is a static program built with musl, a libc that has never heard
# of this kernel, so it cannot lean on how the kernel is built inside,
# only on what it promises every x86_64 program, and a test that starts
# failing is a promise broken. the uapi headers come after the libc's,
# for what is this kernel's own, never in place of them
SELFTEST_DIR  := tools/testing/selftests
SELFTESTS     := $(sort $(patsubst $(SELFTEST_DIR)/%.c,%, \
                   $(shell find $(SELFTEST_DIR) -mindepth 2 -name '*.c' \
                           2>/dev/null)))
SELFTEST_BINS := $(addprefix $(BINDIR)/selftests/,$(SELFTESTS))

# the tests of what only root may do. each goes on the medium so that only
# root may run it, which is how /init knows to run it as root rather than
# as its guest; every other test anyone may run
SELFTEST_ROOT := fs/blockdev

# the image's /init: asks which tests to run, runs each as a guest and
# powers the machine off. built the way a test is, since it is a program
# that sees the kernel only from where they do
SELFTEST_INIT := $(BINDIR)/init

# what goes on the medium: init, those tests and the directories they are
# in, named one by one rather than the whole of bin/selftests/, so a
# binary left behind by a test that has since gone does not ship
SELFTEST_TAR := $(sort ./init ./selftests \
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

# musl-gcc, from the distribution's musl-tools or a musl built anywhere
MUSL_GCC ?= musl-gcc

# every program built for the machine is built for any x86_64, as the
# kernel is. what gcc assumes otherwise is its distribution's choice,
# and the one in ubuntu's amd64v3 archive assumes bmi2 and avx2, which
# qemu's default cpu does not have
MACHINE_CPU := -march=x86-64

SELFTEST_CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -fno-pie $(MACHINE_CPU) \
                   -fno-omit-frame-pointer -idirafter include/uapi
SELFTEST_LDFLAGS := -static -no-pie -Wl,--build-id=none

$(BINDIR)/selftests/%: $(SELFTEST_DIR)/%.c $(SELFTEST_DIR)/kselftest.h \
                       $(wildcard include/uapi/velvet/*.h)
	@mkdir -p $(@D)
	$(MUSL_GCC) $(SELFTEST_CFLAGS) $(SELFTEST_LDFLAGS) $< -o $@

$(SELFTEST_INIT): $(SELFTEST_DIR)/init.c $(SELFTEST_DIR)/kselftest.h \
                  $(wildcard include/uapi/velvet/*.h)
	@mkdir -p $(@D)
	$(MUSL_GCC) $(SELFTEST_CFLAGS) $(SELFTEST_LDFLAGS) $< -o $@

# the makefile too, since what the archive is made from is written here,
# and a change to that is a change make cannot otherwise see
$(RAMDISK): $(SELFTEST_INIT) $(SELFTEST_BINS) $(SELFTEST_LIST) GNUmakefile
	@mkdir -p $(@D)
	@chmod 0755 $(SELFTEST_BINS)
	@chmod 0700 $(addprefix $(BINDIR)/selftests/,$(SELFTEST_ROOT))
	tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner \
		--mtime=@0 -cf $@ \
		-C $(abspath $(BINDIR)) --no-recursion $(SELFTEST_TAR)

#
# the loader is not this tree's. philemon is a project of his own, and
# the image the tests boot is written with a release of him:
# philemon-release.mk names it by version, tools/philemon.py clones its
# tag into obj/philemon/, and his own makefile builds him into
# bin/boot/. the image's format is his too, so his tools/mkboot.py
# writes it.
#
#   make PHILEMON_URL=../philemon   the same release, out of a local copy
#   make PHILEMON_SRC=../philemon   that checkout as it is
#
# only the image asks for him, so building the kernel alone never
# fetches anything. the kernel is handed one struct, in rdi, and the
# protocol is its own, in include/velvet/boot.h; tests/test_boot.c
# holds his declaration of it against the kernel's. which release, and
# where it is, are named at the top with the other names, for the reason
# BOOTIMG is
#
# always asked, the way a tree that builds this kernel asks for it: his
# makefile answers in a moment when nothing changed, and leaves the
# files alone, so the image is not rewritten for the asking
$(LOADER) &: philemon-release.mk FORCE
ifeq ($(PHILEMON_SRC),$(PHILEMON_RELEASE))
	@python3 tools/philemon.py fetch '$(PHILEMON_SRC)' '$(PHILEMON_URL)' \
		'$(PHILEMON_VERSION)'
else
	@python3 tools/philemon.py local '$(PHILEMON_SRC)'
endif
	@$(MAKE) --no-print-directory -C $(PHILEMON_SRC) \
		BINDIR=$(abspath $(BINDIR)/boot) OBJROOT=$(abspath $(OBJROOT)/loader)

$(BOOTIMG): $(LOADER) $(BINDIR)/$(KERNEL) $(RAMDISK)
	@python3 $(PHILEMON_SRC)/tools/mkboot.py $@ $(LOADER) \
		$(BINDIR)/$(KERNEL) $(RAMDISK)

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
# system, and a fat32 one, so the partition scan has something of another
# kind to find. the system partition is built by the C formatter, so the
# disk the machine boots is a real ext4, extents and all. the work image
# is made smaller than the partition it goes in, and that is deliberate:
# 8 MiB of filesystem inside 24 MiB of partition is exactly the situation
# the resize call exists for, so `make run` boots a machine with
# something to grow
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
# VELVET_HOSTED turns irq_save/irq_restore into no-ops, since userspace
# gets shot for saying cli.

HOSTCC    := gcc
HOSTFLAGS := -std=gnu11 -Wall -Wextra -Wno-stringop-truncation -Wno-stringop-overread -g -DVELVET_HOSTED -I. -Iinclude -Iarch/x86_64/include -I$(OBJROOT)/include -DVELVET_ARCH_X86_64

TEST_BINS := $(T)/kprintf $(T)/mm $(T)/buddy $(T)/slab \
             $(T)/vmm $(T)/gdt \
             $(T)/ksyms $(T)/rtc $(T)/ramdisk $(T)/elf \
             $(T)/addrspace $(T)/process \
             $(T)/tty $(T)/acpi $(T)/pci \
             $(T)/keyboard $(T)/serial \
             $(T)/fat32 $(T)/vfs $(T)/boot \
             $(T)/locks $(T)/path \
             $(T)/pipe $(T)/bcache $(T)/ramfs \
             $(T)/ext4 $(T)/part $(T)/console \
             $(T)/mouse $(T)/init $(T)/mkfs \
             $(T)/net \
             $(T)/tcp \
             $(T)/signal $(T)/ansi \
             $(T)/termios $(T)/string \
             $(T)/epoch $(T)/pressure $(T)/hash $(T)/chacha20 \
             $(T)/random \
             $(T)/crash \
             $(T)/resize $(T)/switch \
             $(T)/proc

$(T)/kprintf:  tests/test_kprintf.c  lib/kprintf.c \
               sched/spinlock.c
$(T)/kprintf:  LDLIBS = -pthread
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
               lib/string.c
$(T)/pci:      tests/test_pci.c      drivers/pci.c \
               lib/string.c
$(T)/acpi:     tests/test_acpi.c     arch/x86_64/acpi.c \
               lib/string.c
$(T)/tty:      tests/test_tty.c      sched/spinlock.c \
               drivers/tty.c drivers/termios.c arch/x86_64/uaccess.c \
               sched/process.c sched/signal.c lib/string.c
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
$(T)/pipe:     tests/test_pipe.c     fs/pipe.c fs/file.c \
               sched/spinlock.c lib/string.c
$(T)/ramfs:    tests/test_ramfs.c    fs/ramfs.c \
               sched/spinlock.c lib/string.c
$(T)/bcache:   tests/test_bcache.c   fs/bcache.c \
               lib/string.c
$(T)/ext4:     tests/test_ext4.c     fs/ext4.c fs/jbd2.c \
               tools/fsck.c lib/string.c
# the formatter, checked against the *driver* rather than against itself.
# a formatter and a reader by one author can agree on something wrong and
# both be happy, which is exactly what happened to mkfat.py in 0.1.10
$(T)/mkfs:     tests/test_mkfs.c     tools/mkfs.c \
               fs/ext4.c fs/jbd2.c \
               lib/string.c
# almost all of a network stack is arithmetic over a byte buffer, so
# almost all of it can be checked with no card and nothing on the wire.
# the fixtures that matter are real captured packets: a header I
# invent and a parser I invent agree with each other and with nothing
# else on earth
# the largest state machine in the kernel, and the one whose bugs are
# least visible from outside: a stack that gets the close sequence
# wrong works perfectly for every connection anybody watches
$(T)/tcp:      tests/test_tcp.c      net/tcp.c \
               net/net.c lib/string.c
$(T)/net:      tests/test_net.c      net/net.c \
               net/ether.c net/arp.c \
               net/ip.c net/icmp.c \
               net/udp.c net/socket.c \
               lib/string.c
$(T)/part:     tests/test_part.c     drivers/part.c \
               lib/string.c lib/hash.c
# crc32. a suite when run with no arguments and a checksum tool when
# run with some, which is how tools/hashcheck.py drives it against zlib
$(T)/hash:     tests/test_hash.c     lib/hash.c \
               lib/string.c
$(T)/chacha20: tests/test_chacha20.c lib/chacha20.c
$(T)/random:   tests/test_random.c   drivers/random.c lib/chacha20.c \
               sched/spinlock.c lib/string.c
# the six python build tools, rewritten in C. not suites: they are
# programs, and nothing they produce can judge itself, so they build
# outside TEST_BINS and tools/toolcheck.py is what runs them.
#
# and built *beside* the python rather than instead of it, so that
# toolcheck has something to compare against: a swap that changes the
# build's output by accident is the failure this version is most likely
# to have, and it would look exactly like a build that worked
TOOL_BINS := $(T)/bin2c $(T)/checkfmt $(T)/gensyms \
             $(T)/mkext4 $(T)/mkfat $(T)/fsckext4

tools: $(TOOL_BINS)
	@:

$(T)/bin2c:    tools/bin2c.c
$(T)/checkfmt: tools/checkfmt.c
# gensyms reads the symbol table out of the ELF itself rather than
# asking nm, which is the point: a self-hosting machine has no more
# binutils on it than it has python
$(T)/gensyms:  tools/gensyms.c
# not a rewrite so much as a driver: mkfs.c already formats the
# filesystem and ext4.c already writes files, and both are tested. what
# was missing was a way to point them at a host file and a host
# directory
$(T)/mkext4:   tools/mkext4.c        tools/mkfs.c \
               fs/ext4.c fs/jbd2.c \
               lib/string.c
# the one with code of its own in it: fat32.c mounts, creates and
# writes, but nothing anywhere formatted a fat32 volume. so the boot
# sector, the two tables and the root cluster are laid out here, and the
# files go in through the same driver the kernel uses
$(T)/mkfat:    tools/mkfat.c         fs/fat32.c \
               lib/string.c
# and the checker, the same driver-over-a-host-file shape: fsck.c
# checks a filesystem through two function pointers, and this points it
# at an image
$(T)/fsckext4: tools/fsckext4.c      tools/fsck.c \
               lib/string.c
# pulling the plug at every write there is. the driver, the formatter
# and the checker all at once, because what is under test is not any one
# of them: it is whether the *order* they write in leaves damage that
# has an answer
$(T)/crash:    tests/test_crash.c    fs/ext4.c \
               fs/jbd2.c \
               tools/mkfs.c tools/fsck.c \
               lib/string.c
# growing a filesystem onto a disk bigger than the one it was made for.
# the formatter, the driver and the checker again, and for the same
# reason: what is under test is whether a resize leaves a filesystem,
# which is not a question the thing doing the resizing may answer
$(T)/resize:   tests/test_resize.c   fs/ext4.c \
               fs/jbd2.c \
               tools/mkfs.c tools/fsck.c \
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
$(T)/termios:  tests/test_termios.c  drivers/termios.c lib/string.c
# the copies every subsystem makes, a word at a time where they can be,
# held against a copy made a byte at a time at every alignment
$(T)/string:   tests/test_string.c   lib/string.c
# a date into a number. checked against the host's own timegm, which
# is somebody else's implementation and the only opinion worth having
$(T)/epoch:    tests/test_epoch.c    lib/epoch.c lib/clock.c
# who gets refused when memory runs out. a policy rather than a data
# structure, and its interesting cases are all at the edges
$(T)/pressure: tests/test_pressure.c mm/pressure.c
$(T)/vfs:      tests/test_vfs.c      fs/vfs.c fs/file.c fs/dev.c \
               fs/ramdisk.c \
               lib/string.c
# the protocol as the kernel declares it against the loader's own header,
# field by field, since the two describe the same bytes and neither can
# include the other's. his header is his release's, so the suite is
# built after he is fetched
$(T)/boot:     tests/test_boot.c     include/velvet/boot.h $(LOADER)
$(T)/boot:     HOSTFLAGS += -I$(PHILEMON_SRC)/include
$(T)/gdt:      tests/test_gdt.c      arch/x86_64/gdt.c
$(T)/gdt:      SRCS = tests/test_gdt.c
$(T)/keyboard: tests/test_keyboard.c drivers/keyboard.c \
               drivers/input.c \
               sched/spinlock.c
$(T)/serial:   tests/test_serial.c   drivers/serial.c \
               drivers/input.c \
               sched/spinlock.c
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
TEST_HEADERS := $(shell find $(KERNEL_PORTABLE) arch tools tests -name '*.h' 2>/dev/null)
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
toolcheck: $(TOOL_BINS) $(BINDIR)/$(KERNEL) $(RAMDISK) ext4-image
	@python3 tools/toolcheck.py

# the two sums against python, zlib and git. a published vector only
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

# the kernel itself is a prerequisite, and that is not decoration. twice
# now every host suite has passed while the kernel did not link: disk.c
# is compiled into no host test, so nothing but building the real thing
# catches a missing function there. and the boot image, because the boot
# test and the selftests boot it, and a run of the suites that never
# wrote one would not notice when one could not be written
test: checkfmt checkarch portable-check $(BINDIR)/$(KERNEL) $(BOOTIMG) \
      $(RAMDISK) $(TEST_BINS) $(TOOL_BINS) \
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
	printf '  %-10s ' hashcheck; \
	if out=$$(python3 tools/hashcheck.py 2>&1); then \
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

$(T)/fuzz_parsers: tools/fuzz_parsers.c \
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
              -DVELVET_ARCH_$(ARCH_UPPER) -Iinclude -Iarch/$(ARCH)/include -c

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

# boot the image, power it off, and check the disk it ran on. needs qemu
.PHONY: boottest
boottest: $(BOOTIMG) $(T)/mkext4 $(T)/fsckext4
	./tools/boottest.sh $(BOOTIMG)

# the selftests, run where a program is run: the image booted headless
# with a fresh copy of the disk `make run` boots, its /init told which to
# run, each run as guest, and every result read back as TAP. needs qemu.
#
#   make kselftest                  every test
#   make kselftest TARGETS=sched    the tests of one subsystem
TARGETS ?= $(sort $(patsubst %/,%,$(dir $(SELFTESTS))))

.PHONY: kselftest
kselftest: $(BOOTIMG) $(DISK)
	@python3 $(SELFTEST_DIR)/run_kselftest.py --disk $(DISK) $(BOOTIMG) \
		$(filter $(addsuffix /%,$(TARGETS)),$(SELFTESTS))

clean:
	rm -rf $(BINDIR) $(OBJROOT)

distclean: clean
