# where this is going

velvet's plan, from 0.4.0 on, the release that made the kernel a project
of its own. a number below 0.4.0 anywhere in this document is a version
from before then, and [CHANGELOG.md](CHANGELOG.md) says where that history
is kept.

what follows is a lot of small steps rather than one heroic one. each
entry below should be a version on its own: buildable, bootable, and worth
a paragraph in the changelog. the numbers run without gaps.

this is the kernel's plan and only the kernel's. accounts, passwords,
logins, shells, libraries, installers and every program a person uses
belong to the system built on top of it, even where they are why the
kernel needs something: an entry here is the mechanism underneath, and
says what a program can do with it. a program can still be an entry's
yardstick, and the best one there is: a program written elsewhere and
run here unchanged is an outside answer about the kernel under it.

0.4.x is linux's interface to programs, answered here the way linux
answers it. 0.5.x is what the kernel holds between people who share a
machine, 0.6.x is hardware nobody emulated, and 0.7.x is a kernel that
can say what it is doing.

## non-goals

- **being useful in any practical sense.** this is a kernel built to make
  what happens between "power button" and "shell prompt" legible, every
  subsystem documented by why it is the way it is, and every promise to
  programs checked against another kernel's answer. every decision below
  is made with that ahead of speed or reach.
- **being linux.** the compatibility 0.4.x works towards is with linux's
  interface to programs: its system calls, its signals and the layout of
  what they hand back. linux's interfaces *inside* the kernel are not part
  of it (no modules, no driver api, nothing that would let linux's own
  code load here), and neither is the part of its userspace interface that
  exists for linux's own scale: namespaces, cgroups, `io_uring`, bpf.
- **an architecture nobody can test.** an aarch64 port was written and
  removed again in 0.2.20, and the rule it left is that a thing that cannot be checked here does not ship. a second
  architecture comes back when there is a machine to check it on.

## 0.4.x: the interface programs already speak

a compiler, a shell and an archiver are written against linux's
interface to programs by people who have never heard of this kernel, and
since 0.4.5 that interface is this kernel's: the system calls every x86_64
program and libc already makes, numbered and shaped the way they expect,
the signals and the frame a handler is entered on, errno, the stack exec
leaves, and floating point in ring 3, since 0.4.6 a root of its own to
mount things under and the drives and the network a client, an installer
and a checker need, since 0.4.7 terminals the way such a program asks
for them, a pty among them, and fifos, since 0.4.8 threads, made the way
a thread library makes them and waiting on futexes, and a child
posix_spawn makes in its parent's memory, since 0.4.9 a `/proc` laid
out the way such a program reads it, and since 0.4.10 a program linked
against a shared libc, started by the loader it names, scripts, and
memory shared between processes. what is left of the series is
somebody else's programs run unmodified. the reason is the one every
checker in this project rests on, an outside answer: the same static
binary run on both machines says, call by call, where this kernel
differs.

**0.4.11 somebody else's programs.** busybox, built static for linux and
not touched: a shell, `ls`, `sed`, `awk` and a hundred more, each written
against linux by people who have never heard of this machine. its own
test suite is the check, run the same way here as on linux, and the
difference between the two runs is the list of what is still not linux.
*ends when busybox's shell runs busybox's tests here, and every failure is
either fixed or written down as a decision.*

**what this series deliberately does not reach.** no 32-bit programs: the
ia32 interface is a second table and a second set of layouts, for programs
nobody builds any more. no vdso, so `clock_gettime` is a real call here,
which is slower and exactly as correct. and none of what the non-goals
rule out: this is linux's interface to programs, and only that.

## 0.5.x: what the kernel holds between people

0.4.x is about what a program can do. this series is about what one
person's programs cannot do to another's, which only the kernel can
hold, since nothing in ring 3 can stop the rest of ring 3. who the people
are, their names and passwords and how they prove who they are, is the
system's to keep: the kernel deals in numbers, and in what each number
may reach.

there have been users since 0.1.5, and since 0.4.5 a process carries a
gid and supplementary groups beside its uid, and a file's group bits are
read for them. what is left is what a machine with more than one person
on it stands on: bytes nobody can predict, a superuser that is a set of
permissions rather than a number, keys that never touch the disk, an
address space an exploit cannot count on, limits one person cannot spend
for everybody, and a log nobody can rewrite.

**0.5.0 something genuinely unpredictable.** there is a generator: a
chacha20 key behind `getrandom`, `/dev/random` and `/dev/urandom`, seeded
at boot from the wall clock, the cycle counter and `rdrand` where the cpu
has it, and stirred by every timer tick and every write to `/dev/random`.
what it cannot say is how good its seed was, and a vault key, an address
that moves and whatever ring 3 asks for need bytes that are
unpredictable, not merely different. so the seed comes from `rdseed`
where the cpu admits to having it and from timing jitter where it does
not, and the pool is reseeded from the arrival times of interrupts,
which is the one thing a machine on a wire has plenty of.

the awkward part is in the middle of it: **this machine boots identically
every time**, which is what makes everything else here testable and is
exactly the property a seed must not have. so the boot log has to say
which source it got, because "seeded from the cpu" and "seeded from
nothing" look the same from the outside and are not the same machine at
all. and checking it needs a different shape than anything so far.
statistics over the output can only catch a generator that is obviously
broken, so what is checked is the *construction*, which chacha20's
published vectors already are, and then the plumbing separately, by
asking whether two boots differ. *ends when the pool is
seeded from at least three independent sources, refuses to hand out bytes
before it is, and writes no state to disk.*

**0.5.1 permissions rather than uid 0.** the kernel asks "is this uid 0"
by name wherever it decides whether something is allowed: to mount a
disk, to change a file's owner, to signal somebody else's program, to
read the kernel's caches. a `capable()` check takes its place, so each
is a permission a process holds, all of them uid 0's to begin with, and
one that can be given away without the rest. and `exec` honours the
setuid and setgid bits, which nothing here acts on today, so that a
program runs as the file's owner rather than as whoever started it: the
one way a person can act as another without being them.

**0.5.2 the primitives, and the first thing here that a right answer does
not prove.** the ones the kernel itself uses: aes-xts, the cipher a vault
(0.5.3) is written in, and sha-256 and hmac for the verifier in a vault's
header. they go in `lib/`, beside `hash.c`.
checked in isolation and against everybody: published vectors, and
python's `hashlib` and `cryptography` over the same inputs, which is
`hashcheck.py` doing again what it did for sha-1 in 0.3.17.

and then the part that is new to this project. every test written here so
far asks whether the answer is right, and for this code that is not
enough: **a comparison that returns on the first byte that differs is
correct, and tells whoever is asking how much of their guess was right.**
no test that checks answers can see that, and timing it on a machine
inside an emulator measures the emulator. so it is checked the way
`checkarch.py` checks the architecture boundary: a rule about the code,
enforced by a tool that fails the build. in these files, no early return
out of a comparison and no branch on a secret.

**0.5.3 vaults.** a driver between the disk and the vfs, which is where
0.2.13's block cache already sits, and it got there by being handed the
two function pointers fat32 was already using, so the seam exists and has
been load-bearing for a while. a vault is a large file that, given a key,
mounts as a directory: aes-xts, with the key handed in from ring 3. the
header is a salt and a verifier and nothing else. **the key never
touches the disk**: powered off, the whole of it is noise.

this is better than full disk encryption for a machine like this one, and
the reason is worth writing down rather than assuming: it protects
`/work` without making `/boot` impossible to debug, and a machine whose
early boot cannot be inspected is a machine this project could not have
been built on.

the danger is the cache rather than the cipher. a decrypted block sitting
in the block cache is plaintext in memory, and `sync` writing it home is
plaintext on the disk, so the boundary has to be exactly one layer and
provably so. and 0.3.23 and 0.3.24 both have a crash suite that stops the
machine at every write there is; both have to still pass with a cipher in
the path, which is the real test of whether the layer went in the right
place.

**0.5.4 an address space that moves.** the vmm can place things anywhere
and places them in the same place every time: `0x555555554000` for a
program built to be loaded anywhere, `MMAP_BASE` for its loader and for
the first of its mappings, one constant for the top of every stack. all
of them move on every exec. **it makes a fault a security feature rather
than only a bug**: an exploit that assumes a location crashes instead,
and the kernel says so. a program linked for one place stays in it,
since that is what it was built for.

and the kernel's own stack: it is built with `-fno-stack-protector`, and
turning that around is a flag rather than a version, with a canary out
of 0.5.0's pool.

what is already here and should be finished with it: NX is on. the vmm
sets `EFER.NXE` at boot, says so when the cpu has no such bit, and maps
user stacks non-executable. what is missing is the pair the kernel does
not set at all, **SMEP and SMAP**: two bits in cr4 that make the hardware
refuse to run or read user memory while in kernel mode, which turns a
whole family of kernel bugs into an immediate fault.

**0.5.5 resource limits that hold.** `setrlimit` and `prlimit64` keep
every limit a program sets, and only the one on descriptors is held to: a
program can still take every page of memory and every process there is.
0.3.13 drew this line once already and drew it in the other direction: a
reserve only the kernel may draw on, so that running out costs the
program rather than the machine. this is the same line between two
*people*: memory, processes, the size of a file and time on a cpu held
to, per process and per user, so that one person cannot fork-bomb a
machine into a state where another cannot log in to kill it.

and the scheduler learns about niceness, which it currently has no notion
of. there are no priorities anywhere in it, only one shared run queue
that any idle core takes from (0.2.2), so this is a change to how the
pick is made rather than a second queue. the thing it buys is specific: a
compile in the background, which after 0.4.x is a real and long-running
thing, should not make the editor lag.

**0.5.6 a log nobody can rewrite.** `kprintf` keeps a ring of everything
it prints, and since 0.4.1 a program can read it with `syslog`. what is
left is the other direction: a program writing to the kernel's log, which
is the orderly version of the serial-port logging this project has leaned
on since 0.1.x. and a file can be made append-only to everybody but the
kernel, with the kernel holding to it, because a log the logged-in can
edit is not a log.

**what this series deliberately does not reach.** no acls and no
mandatory access control, because owner-group-other with a `capable()`
check underneath is the model this machine's filesystem already stores,
and the next model up is a research project. no disk quotas, which is the
one genuine omission from 0.5.5: ext4 has a feature bit for them and this
would be the place, but a quota that is enforced in some paths and not
others is worse than none, and finding every path is a version rather
than a paragraph.

## 0.6.x: the machine leaves the emulator

everything above has been checked on one machine, and that machine is not
a machine. qemu is a second implementation of the hardware and a
forgiving one: it answers registers that real silicon leaves floating, it
never takes longer than expected, and it gives every machine a ps/2
keyboard whether or not anything sold this decade has one.

so this series has a problem the others did not, and it is worth saying
before the entries rather than inside them. **most of it is checkable and
the claim it makes is not.** OVMF, `-device qemu-xhci`, `-device nvme` and
a virtio disk are all right there, and every entry below is written to be
finished in an emulator first. what none of them can settle is whether
the thing boots on hardware somebody else owns, and that is the whole
point of the series. the cancelled arm port is the precedent and the
warning: a thing that cannot be tested here does not ship, so each of
these has to be testable here *and* confirmed there. starting the kernel
on firmware with no bios is the loader's: uefi is in philemon's roadmap,
and what this series owes it is a kernel that boots from it unchanged.

**0.6.0 usb.** xhci, and a **keyboard first**, because a machine you cannot
type on is one you cannot even read the panic off. ps/2 is present on
almost nothing built in the last ten years and present in every emulator,
which is the most dangerous combination a driver can have: it works
everywhere it is tested and nowhere it is used. then mass storage, since
a usb stick is how this actually reaches another machine.

**0.6.1 nvme.** ahci was 0.1.10 and there has been exactly one disk driver
since. the seam it would slot into is already load-bearing (the block
cache took its place in 0.2.13 by being handed the two function pointers
fat32 was using), so this is a driver rather than a rewrite. and a second
disk driver is the only way to find out which of the first one's habits
were assumptions.

**0.6.2 a framebuffer the size of a real screen.** vbe hands over something
modest; gop on a laptop, once philemon can boot one, hands over 1920x1080
or worse, and at that size
every scroll is a memmove of eight megabytes and every redraw is a repaint
of two million pixels. 0.3.10 already found this shape of problem at
1024x768 (a redraw per keystroke is what made the editor feel like
wading), and it arrives again with a bigger number. the cell grid is
256x128 and copes; the 8x16 font at 4k is not a font, it is a rumour, so
a scale factor is part of the version.

**0.6.3 virtio.** net and block. it is what the machine gets when it runs
on somebody's hypervisor instead of somebody's desk, and it is the
cheapest possible second implementation of two drivers that have never had
one, which is what will find the e1000-shaped assumptions in `net/` and
the ahci-shaped ones underneath the cache.

**0.6.4 a clock that is right rather than merely increasing.** the lapic
timer is calibrated against the pit already; the tsc's frequency has to
be measured rather than assumed too, and the rtc read with its drift
admitted. ring 3 can set the clock, and it has to be able to slew it as
well, with `adjtimex`, because a clock that is only ever set jumps, and
every timestamp taken across the jump is wrong by it.

**0.6.5 power.** `machine_poweroff` writes 0x2000 to port 0x604 and then to
0xb004, which is qemu and bochs, and on real hardware neither does
anything at all, so the machine says goodbye and keeps running. doing it
properly is the fadt, `PM1a_CNT`, and a sleep type that lives in the dsdt,
which means parsing the first bytes of aml this project has ever looked
at. and the power button becomes an event rather than a way to lose a
filesystem, which is what makes this worth a version: 0.3.24 spent an
entire version making a crash cheap, and a machine that can only be
turned off by holding the button in is a machine that crashes every time
somebody leaves the room.

**0.6.6 a machine that says what it found.** on a desk there is no serial
console: the log has nowhere to go, so a machine that fails on somebody
else's hardware fails silently. the boot log gets written to the medium
it booted from before anything else can go wrong, and there is a report
to go with it: what the firmware claimed, which drivers bound to what,
what was refused and why. that is the difference between "it did not
work" and a bug report, and it is the bridge into 0.7.x.

**0.6.7 the boot performed rather than described.** three machines that
are not this one, three firmwares, and the log from each of them kept in
the tree. no suite can run this and that is exactly the point: it is the
one claim in this document that only somebody else's hardware can settle.
the honest form of the entry is a list of what did not work, because the
first three real machines will each say something nobody predicted.

## 0.7.x: the machine explains itself

this kernel has been able to say *what* went wrong since 0.1.x: a panic
with a symbolised backtrace, `bt`, `ps`, `top`, a memory column, a count
of syscalls by kind. what it cannot say is why, or where the time went, or
what a program was doing at the moment it stopped. what a debugger, a
tracer and a profiler need from the kernel is in this series.

the outside answers here are unusually good, and one of them has been
sitting in the build the whole time: **the kernel is compiled with `-g`
and always has been**, so there is dwarf in `bin/velvet` today that
nothing in this project has ever read, and `addr2line` will say what any
address in it means. every reader below has somebody else's answer to be
held against, and one of them hands the whole kernel to somebody else's
debugger.

**0.7.0 dwarf, and a backtrace that names a line.** a backtrace today is a
symbol and an offset, out of the table `gensyms` builds from the symbol
table. the line number is already in the binary and has been since 0.1.0.
reading it is a version of its own because dwarf's line table is not a
table: it is a bytecode for a little state machine that you run to
produce one. checked against `addr2line` on the same kernel, address for
address, which is the cheapest outside answer in this document.

**0.7.1 core dumps.** a program that dies leaves nothing behind but a
message. an ELF core (the registers, the mappings, the memory that
mattered) written where the person can find it, in the format everybody
else's tools already read. that last clause is the whole verification and
it costs nothing extra: gdb on the development machine opening a core
written by velvet is the test.

**0.7.2 one process reaching into another.** what a debugger needs from a
kernel: stopping a process, reading and writing its registers and its
memory, `int3` for breakpoints and the trap flag for stepping, each
reported to whoever is watching. it is the one thing this kernel has
always refused, one process reaching into another's address space, which
is why it is a version rather than a program, and why it wants 0.5.1's
`capable()` underneath it, so that reaching in is a permission somebody
holds rather than a thing that is possible.

**0.7.3 a gdb stub.** the remote serial protocol, over the wire or the
serial line, so that *real gdb* attaches to this kernel. it is the
strongest outside answer a debugger can have: a protocol we did not
design, driven by a program we did not write, against a kernel that has to
be right about every register it claims to report. and it is what makes
debugging the kernel itself possible, which 0.7.2 deliberately does not
attempt.

**0.7.4 tracing.** 0.1.6 counted syscalls by kind; this makes them a
stream: which call, from whom, its arguments as the registers held them,
and what came back, with every string a call was handed copied out as the
call read it, since by the time anybody reads the stream the program may
have written over it. the stream is numbers, as the call had them.

**0.7.5 a profiler.** sampling on the lapic timer, which has been per-cpu
since 0.2.2, and unwinding from wherever the interrupt happened to land,
which works only because `-fno-omit-frame-pointer` has been in the build
since the beginning for the backtrace's sake. this is the second thing
that flag pays for. the question it exists to answer is where a build
spends its minutes, measured rather than guessed.

**0.7.6 the log, and a panic that outlives the machine.** with 0.5.6 the
kernel's log can be read from ring 3; this is the part that matters on
hardware that is not here. the panic is written to the medium before the
machine gives up, so the evidence survives the reboot. 0.6.6 writes the
first log of a boot for the same reason; this writes the last.

**0.7.7 numbers that are kept.** a suite that measures instead of
checking: boot time, the cost of a syscall, a page fault, a context
switch, a megabyte through the disk and a megabyte through the wire.
recorded in the tree, per version, because the only outside answer
available for performance is *yesterday*, and a regression nobody wrote
down is a regression nobody finds. this project has twice discovered a
slowdown by feel (0.3.10 twice over), and feeling it is not a method.

**0.7.8 breaking it on purpose.** 0.3.23 and 0.3.24 stop the machine at
every write there is and demand the wreckage be mendable. this is the same
idea pointed everywhere else: an allocation that fails, a read that
returns short, a packet that never arrives, a disk that reports an error
it has never reported. every error path in this kernel is written and
most of them have never once run, which is also the honest test of how
much of 0.4.5's errno was aspiration.

**what the machine still cannot say.** no reverse debugging and no record
and replay, which is the thing that would make an intermittent bug
reproducible and is a project rather than a version. no way to see inside
optimised code: the kernel is built without the optimiser, and the line
table read here says nothing about what one did.
