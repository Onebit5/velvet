# where this is going

velvet's plan, from 0.4.0 on, the release that made the kernel a project
of its own. a number below 0.4.0 anywhere in this document is a version
from before then, and [CHANGELOG.md](CHANGELOG.md) says where that history
is kept.

what follows is a lot of small steps rather than one heroic one. each
entry below should be a version on its own: buildable, bootable, and worth
a paragraph in the changelog. the numbers run without gaps.

this is the kernel's plan and only the kernel's. accounts, passwords,
logins, shells, libraries and installers belong to the system built on top
of it, even where they are why the kernel needs something; an entry here is
the mechanism underneath, and says what a program can do with it.

0.4.x is the kernel's half of a machine that rebuilds itself, and on the
way there velvet learns to answer the way linux does. 0.5.x is what the
kernel owes a machine with more than one person on it, 0.6.x is hardware
nobody emulated, and 0.7.x is a kernel that can say what it is doing.

## non-goals

- **being useful in any practical sense.** this is a kernel built to be
  understood, small enough to fit in one head, and every decision below
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

## 0.4.x: the machine builds itself

a machine that rebuilds itself from its own disk ends by building a
kernel, installing it and rebooting into it. the kernel's half of that is
a program that can reach a drive at all. a boot that can choose between
two kernels, and one that cannot be lost to a kernel that does not come
up, are the loader's, and philemon's roadmap has them.

the programs that do the building are not this machine's own. a
compiler, a shell and an archiver are written against linux's interface
to programs by people who have never heard of this kernel, and since
0.4.5 that interface is this kernel's: the system calls every x86_64
program and libc already makes, numbered and shaped the way they expect,
the signals and the frame a handler is entered on, errno, the stack exec
leaves, and floating point in ring 3. what is left of the series is the
rest of what such a program takes for granted: a root it can mount
things under, terminals, threads, a `/proc`, a loader, and at the end
somebody else's programs run unmodified. the reason is the one every
checker in this project rests on, an outside answer: the same static
binary run on both machines says, call by call, where this kernel
differs.

**0.4.6 a root of its own, and what a client, an installer and a checker
need.** the kernel mounts the disk at `/` itself and lays the ramdisk
under `/boot`, and that is a system's decision. the archive the loader
hands over becomes the root instead, a filesystem in memory a program may
write, with `/dev` on it filled in by the kernel, and `/init` mounts the
disk and moves into it with `pivot_root`. `mount`, `umount2`,
`pivot_root`, `chroot`, `statfs` and `mknod` arrive with it, and the
drives appear in `/dev` as block devices, the drive and each partition,
read and written by sector, root's. that is also what an installer
needs, a drive with no filesystem on it yet, and what a checker needs,
the partition under a mounted filesystem, with what the cache holds for
it written out first. since 0.4.3 the machine boots with no address, and
nothing can give it one: a dhcp client needs the interface ioctls that
set the address, the netmask and the route, root's, and a datagram to
255.255.255.255 that leaves as an ethernet broadcast whether or not there
is an address to send it from, since its first message goes out before
there is one. *ends when an `/init` in ring 3 mounts the disk and pivots
into it, a dhcp client leases an address from qemu's server and pings
the router with it, a program copies the medium onto a blank drive that
then boots on its own, and another checks the filesystem at `/` and
finds what the host's checker finds.*

**0.4.7 a terminal, the way linux asks for one.** 0.4.5's terminals
answer termios through `ioctl`, with sessions, a controlling terminal
and job control. what a program written elsewhere also takes for granted
is the rest of termios: output processing, `TOSTOP`, a session leader
that gets its terminal by opening it, and `SIGHUP` when the terminal
goes away. and then the layer this machine has always been missing: a
pty, a terminal with nothing behind it but another program. a pty is
what `script` and a multiplexer are, and it is the thing standing
between this machine and anything remote.

**0.4.8 threads the way linux makes them.** `clone`, with the flags a
pthread library passes (one address space, one descriptor table, one set
of signal dispositions, a new stack), and `futex`, which is the whole of
every lock a libc builds: a mutex is a word in memory and a call for when
the word says wait. 0.4.5's `clone` makes processes and nothing else,
which is enough for `fork` and `vfork` and not for `posix_spawn`, which
musl builds on a child sharing its parent's memory until it execs. 0.2.x
gave the kernel threads, and this hands them to ring 3; 0.2.1's locks are
the warning about what sharing costs, and the locks suite is the way to
find out. *ends when a program built against musl's pthreads on linux
runs its threads here and agrees with itself on both machines.*

**0.4.9 `/proc`, laid out the way linux lays it out.** 0.4.1's `/proc` is
the kernel's own reports, in the words its shell printed them, which is
enough for a `ps` written against them and for nothing written elsewhere.
software written elsewhere reads a directory per process, `/proc/self`,
and files such as `stat` and `meminfo` in formats a great deal of code
parses, and reading them is also how this version finds out how much of
the kernel's state is still only reachable from inside the kernel.

**0.4.10 a program that names its loader.** a dynamically linked program
does not start at its own entry point: it names a loader in `PT_INTERP`,
and the kernel maps that too and starts there, with `AT_BASE`, `AT_PHDR`
and `AT_ENTRY` in the auxiliary vector saying where everything went. a
libc's dynamic linking needs exactly this from the kernel, and so does
every program on a linux machine that was not built static. with it
comes what a loader does to the files it maps: a program built position
independent, loaded wherever the kernel puts it, and a file mapped
shared, its writes carried back to the file. and a script names its
interpreter on its first line, `#!`, which is the same idea one level
up. *ends when a dynamically linked program and its loader, both copied
off a linux machine, start here.*

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

## 0.5.x: a machine for more than one person

0.4.x is about what the machine can do. this is the first series about who
it belongs to, and most of what that means to a person is programs: the
account files, the shadow file and the login that reads it, `who`, a way
to act as somebody else, tls and the log of who did what. what is here is
what those stand on, and what the kernel has to enforce because nothing in
ring 3 can.

there have been users since 0.1.5 and a session per console since 0.2.16,
and both entries were careful about what they did not mean. the
interesting part of a user is not how the password is stored but what its
uid can and cannot reach, and that part is enforced by ring 3 and by a
kernel that checks before it hands anything over. what has changed is
that there is now something here worth reaching: a filesystem that
survives a reboot, the source the machine was built from, and after 0.4.x
the compiler that rebuilds it.

and one comment in `fs/vfs.c` is the kernel's half of this series in
miniature: **"the group bits are deliberately not consulted. there is no
notion of belonging to a group anywhere in this system, so checking them
would be reading a number nobody ever sets and calling it a
permission."** every entry below deletes a clause of that sentence.

**0.5.0 something genuinely unpredictable.** this machine has no source of
randomness at all, and this series cannot be finished without one: a salt
for a shadow file, a vault key, an address that moves, and every
nonce in a tls handshake. a csprng in `lib/`, seeded from `rdseed` where
the cpu admits to having it, from timing jitter where it does not, and
reseeded from the arrival times of interrupts, which is the one thing a
machine on a wire has plenty of. ring 3 gets it through a `getrandom` and
a `/dev/urandom` that are the same pool.

the awkward part is in the middle of it: **this machine boots identically
every time**, which is what makes everything else here testable and is
exactly the property a seed must not have. so the boot log has to say
which source it got, because "seeded from the cpu" and "seeded from
nothing" look the same from the outside and are not the same machine at
all. and checking it needs a different shape than anything so far.
statistics over the output can only catch a generator that is obviously
broken, so what gets checked is the *construction*, against published
vectors the way `hashcheck.py` once did sha-1, and then the plumbing
separately by asking whether two boots differ. *ends when the pool is
seeded from at least three independent sources, refuses to hand out bytes
before it is, and writes no state to disk.*

**0.5.1 credentials, groups, and real permissions.** a gid beside the uid
on every process, the supplementary groups beside that, and the calls that
set them, as 0.4.1's `setuid` sets the uid a login becomes. ext4 has recorded a gid on every inode since 0.2.14 and nothing has
ever set one to anything; the vfs starts consulting the group bits, and
gains a `capable()` check so the kernel stops asking "is this uid 0",
which it asks by name in the vfs and in the ramdisk driver, and starts
asking "is this allowed to mount a disk". and `exec`
honours the setuid bit, which nothing in this tree knows how to set
today, for a program that acts as somebody else to stand on. which uid
is called what, and which groups exist, is the system's to say.
the kernel's own login went in 0.4.2, and the kernel deals in numbers.

**0.5.2 the primitives, and the first thing here that a right answer does
not prove.** the ones the kernel itself uses: aes-xts, the cipher a vault
(0.5.3) is written in, and sha-256 and hmac for the verifier in a vault's
header. they go in `lib/`, beside `hash.c`. tls's are not here: nothing
in the kernel speaks tls, and arithmetic the kernel does not use is a
library's to keep.
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
mounts as a directory: aes-xts, with the key handed in from ring 3, which
derives it from a password however it likes. the header is a salt and a verifier and nothing else. **the key
never touches the disk**: powered off, the whole of it is noise.

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
and places them in the same place every time: `0x400000` for every
program, one constant for the top of every stack, one `MMAP_BASE` for
every heap. stacks, heaps and mappings move on every spawn. **it makes a
fault a security feature rather than only a bug**: an exploit that
assumes a location crashes instead, and the kernel says so.

the program's own base is the one that cannot move yet, and the
dependency is worth naming rather than discovering: a program linked at a
fixed address stays there, so a program that can be loaded anywhere is
position-independent code, which is the compiler's problem and not the
kernel's. same for stack
canaries: every build in this tree has `-fno-stack-protector` on it, and
turning that around is a flag rather than a version.

what is already here and should be finished with it: NX is on. the vmm
sets `EFER.NXE` at boot, says so when the cpu has no such bit, and maps
user stacks non-executable. what is missing is the pair the kernel does
not set at all, **SMEP and SMAP**: two bits in cr4 that make the hardware
refuse to run or read user memory while in kernel mode, which turns a
whole family of kernel bugs into an immediate fault.

**0.5.5 resource limits.** a program can take every page of memory or
every descriptor there is. `setrlimit`, and defaults set by init. 0.3.13
drew this line once already and drew it in the other direction: a reserve
only the kernel may draw on, so that running out costs the program rather
than the machine. this is the same line between two *people*: a user
should not be able to fork-bomb a machine into a state where the admin
cannot log in to kill it.

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
edit is not a log. a system's record of who logged in, and who acted as
whom, is what such a file is for.

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
these has to be testable here *and* confirmed there. what reaching that
hardware means for the system, an installer that writes a disk the
firmware will boot and the time from somewhere that knows it, is the
system's, and starting the kernel on firmware with no bios is the
loader's: uefi is in philemon's roadmap, and what this series owes it is a
kernel that boots from it unchanged.

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

**0.6.4 a clock that is right rather than merely increasing.** the tsc's
frequency has to be measured rather than assumed, the lapic timer
calibrated against something that is not itself, and the rtc read with its
drift admitted. and the clock becomes something ring 3 can set and slew,
because a laptop whose coin cell died is a machine that rejects the entire
internet until somebody tells it what year it is, and the somebody is an
ntp client.

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
what a program was doing at the moment it stopped. a debugger is a
program, and what it needs from the kernel is in this series.

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
holds rather than a thing that is possible. the debugger that uses it is
a program.

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
have written over it. that is the kernel's half, and it is all numbers.
turning them into words, a path, a set of flag names, the file a
descriptor is, is a program's work: a tracer's.

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
optimised code, which is a real limit here: gcc optimises the userland,
and nothing in this plan reads the result.
