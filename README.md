# velvet

![ci](https://github.com/Onebit5/velvet/actions/workflows/ci.yml/badge.svg)

a tiny 64-bit kernel for x86_64, written in C, booted by a bootloader of its own.

built to make what happens between "power button" and "shell prompt" legible, rather
than to be the next linux. small enough to fit in one head.

it is heading towards linux's interface to programs, all the same: its system calls,
its signals and the layouts it hands back, so that a program built for linux on x86_64
starts here and runs unmodified. not linux's insides: no modules, no driver api.
that plan is the 0.6.x series of [ROADMAP.md](ROADMAP.md), and the non-goals are at the
top of it.

it is the kernel of [velvetOS](https://github.com/Onebit5/velvetOS), which builds on a
release of it and puts the programs, the libc and the toolchain on top. until 0.4.0 the
two were one tree; this is the half that runs in ring 0, and before it.

what is in here: philemon, the bootloader, and the handoff it makes; physical and
virtual memory, with demand paging and copy on write; threads, processes, signals and
an init; smp with a scheduler on every core; fat32, and ext4 with extents and a
journal; ahci, e1000, ps/2, the console and the serial line; a tcp/ip stack with
dhcp, dns and http; the source this kernel was built from, on the medium it booted
from; and the velvet room, the shell, which runs in the kernel.

still a non-goal: being useful in any practical sense.

## the tree

laid out by subsystem, with no source at the root:

```
init/                  kmain, and the handoff philemon leaves behind
mm/ sched/ fs/ net/    the subsystems, each beside the headers only it reads
drivers/ lib/ shell/
include/velvet/        the headers more than one subsystem reads: <velvet/vfs.h>
include/uapi/velvet/   what programs compile against: the syscall numbers,
                       signals, keys and terminal flags
arch/x86_64/           the one architecture: its headers in include/asm/
                       (<asm/cpu.h>), and linker.ld
arch/none/             an architecture that does nothing, for portable-check
boot/                  philemon, the bootloader
tests/ tools/          the host suites, the selftests, the checkers and the image builders
base/                  what the boot medium and the data disk are made of
```

the version is `VERSION`, `PATCHLEVEL` and `SUBLEVEL` at the top of the GNUmakefile,
and the build writes it into `<generated/version.h>` for the banner to print.

## building

you need `gcc`, `nasm`, `make`, `python3` and `qemu` (any recentish versions). on fedora:

```
dnf install gcc nasm make python3 qemu-system-x86
```

then:

```
make lint            # the style guide, checked (whole tree, or FILES=fs/ext4.c)
make                 # the bootable disk image, velvet.img
make run             # boot it in qemu (CPUS=4, MEM=2G)
make test            # the host test suites and the checkers (no qemu needed)
make boottest        # boot it and drive the shell over serial
make kselftest       # run the selftests in ring 3, as guest (TARGETS=sched for one subsystem)
make kernelrelease   # print the version
```

`make run` gives you a qemu window *and* a serial console in your terminal, and since com1 is wired into the input queue, you can type at either one. the shell cannot tell the difference.

the image this tree builds carries no programs but its selftests, so what you get is the
kernel and its shell: the builtins all work, and `ls` answers that it is not something you
can type. for the machine with programs on it, build velvetOS.

another tree builds the kernel with `make -C path/to/velvet BINDIR=... OBJROOT=...
SOURCE=... kernel philemon tools`; [docs/building.rst](docs/building.rst) says what each
of those is for.

## releases

a release is a tag, `vX.Y.Z`, cut from `master` when a roadmap entry is done, and
never moved once it is pushed: velvetOS names the release it builds on by its version
alone, in its `kernel-release.mk`, so a tag that moved would hand it a different
kernel under the same name.
[docs/versioning.rst](docs/versioning.rst) has the rest.

## documentation

the full documentation is in [`docs/`](docs/index.rst), split into pages
about the project and pages about the code:

- [**code style**](docs/coding-style.rst): how to write the code, name things, comment, and handle errors; where a header lives; the TODO/FIXME convention, and `make lint`
- [**contributing**](docs/contributing.rst): which repository a change goes to, the workflow, commit messages, sign-off, review, patches by email, and the rule that **ci is the gate**
- [**building**](docs/building.rst): the toolchain, every make target, running it, and building it from another tree
- [**testing**](docs/testing.rst): the host suites, the checks against other implementations, and the boot test
- [**layout**](docs/layout.rst): the source tree, the headers, and the subsystem map
- [**releases**](docs/versioning.rst): how versions are cut, and how a release reaches velvetOS
- [**the system calls**](docs/syscalls.rst): the abi between ring 3 and the kernel

and one page per subsystem under [`docs/subsystems/`](docs/subsystems/index.rst):
[arch](docs/subsystems/arch.rst), [boot](docs/subsystems/boot.rst),
[mm](docs/subsystems/mm.rst), [sched](docs/subsystems/sched.rst),
[fs](docs/subsystems/fs.rst), [drivers](docs/subsystems/drivers.rst),
[net](docs/subsystems/net.rst), [lib](docs/subsystems/lib.rst),
[shell](docs/subsystems/shell.rst), [tools](docs/subsystems/tools.rst).

the kernel's history from 0.4.0 on is [CHANGELOG.md](CHANGELOG.md). before that it is
velvetOS's changelog, which is the history of the two of them together.

## naming

things that need a name take one from the Persona series, when one fits:
the velvet room, igor, margaret, theodore, lavenza, and the arcana. it is a
flavour rather than a rule, and it is expected to fade after 1.0.0.

## license

GPL-2.0-only, see [LICENSE](LICENSE). Copyright (c) 2026 Jose A. Perez de Azpillaga.
that includes the console font, which is the vga 8x16 from linux's `lib/fonts/`,
GPL-2.0 like the rest.
