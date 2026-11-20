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

0.4.x is linux's interface to programs, all of it, until any program
built for linux runs here unchanged, however many versions that takes.
0.5.x is what the kernel keeps from an attacker, 0.6.x is hardware
nobody emulated, and 0.7.x is a kernel that can say what it is doing.

## non-goals

- **being useful in any practical sense.** this is a kernel built to make
  what happens between "power button" and "shell prompt" legible, every
  subsystem documented by why it is the way it is, and every promise to
  programs checked against another kernel's answer. every decision below
  is made with that ahead of speed or reach.
- **being linux.** the compatibility 0.4.x works towards is with all of
  linux's interface to programs: its system calls, its signals, the
  layout of what they hand back and the files it says the machine through,
  so that any program built for linux runs here unchanged. linux's
  interfaces *inside* the kernel are not part of it: no modules, no
  driver api, nothing that would let linux's own code load here.
- **an architecture nobody can test.** an aarch64 port was written and
  removed again in 0.2.20, and the rule it left is that a thing that
  cannot be checked here does not ship. a second architecture comes back
  when there is a machine to check it on.

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
memory shared between processes.

that is fewer than half of the calls linux answers on x86_64, and the
series does not end until it is all of them, and the files beside them,
so that any program built for linux runs here unchanged. somebody
else's programs mark the way: busybox first, then the programs each
entry below names, copied off a linux machine and not touched, and at
the end the linux test project's own system call tests. the reason is
the one every checker in this project rests on, an outside answer: the
same binary run on both machines says, call by call, where this kernel
differs.

**0.4.11 somebody else's programs.** busybox, built static for linux and
not touched: a shell, `ls`, `sed`, `awk` and a hundred more, each written
against linux by people who have never heard of this machine. its own
test suite is the check, run the same way here as on linux, and the
difference between the two runs is the list of what is still not linux.
*ends when busybox's shell runs busybox's tests here, and every failure is
either fixed or written down as a decision.*

**0.4.12 waiting the way event loops wait.** poll and select look again
every 10 ms rather than being woken, which is correct and is a program
asleep for ten milliseconds it never asked for. a waiter on a descriptor
is woken by whatever changed it, and the calls every event loop is built
on are here: `epoll`, its create, its ctl and its three waits; `eventfd`,
a counter a descriptor can wait on; `timerfd`, a timer one can; and
`signalfd`, signals read as data. *ends when a web server copied off a
linux machine serves pages here to many clients at once, and no wait
outlasts what woke it.*

**0.4.13 programs talking on one machine.** sockets are `AF_INET` only,
and programs on one machine talk to each other through `AF_UNIX`:
stream, datagram and seqpacket, by path and by abstract name,
`socketpair`, and the two things nothing else carries, descriptors
handed over with `SCM_RIGHTS` and the sender's credentials. with it come
the calls that move data without bouncing it through a program,
`sendfile`, `splice`, `tee`, `vmsplice` and `copy_file_range`, and the
forms the rest of the reads and writes lack: `preadv`, `pwritev` and
their second versions, `sendmmsg` and `recvmmsg`. *ends when a message
bus copied off a linux machine carries messages between two programs
here.*

**0.4.14 the network everybody else has.** ipv6 beside ipv4, with the
neighbour discovery and the address a link configures for itself,
`AF_PACKET` and raw sockets for the programs that make their own
packets, and netlink: the socket the links, the addresses and the routes
are read and set through, which is how every program that configures a
network asks. the ioctls 0.4.6 answers stay. *ends when `ip` from a
linux machine shows and sets this machine's addresses and routes, and a
program reaches a host by its ipv6 address.*

**0.4.15 the rest of what a file is.** a file here has one name, and
`link` is `-EPERM`. hard links; extended attributes in every namespace a
filesystem keeps, `user.`, `trusted.`, `security.`, and the posix acls in
`system.`, which the permission check reads; locks, `flock` and `fcntl`'s
record locks, the process's and the open file's; and the flags `chattr`
sets, append-only and immutable among them, which the kernel holds to,
because a log the logged-in can edit is not a log. and the calls a file
is asked about and changed with that are not here yet: `statx`,
`openat2`, `utimensat` and the older times calls, `fallocate`,
`fadvise64`, `readahead`, `sync_file_range`, `close_range`, `fchmodat2`,
file handles and `cachestat`. *ends when git and tar from a linux machine
keep a tree here with its links, its attributes and its times, as they
keep it there.*

**0.4.16 watching files.** `inotify`, a descriptor that says what changed
in the directories a program watches, and `fanotify`, the same for a
whole filesystem, and for a program that has to allow an open before it
happens. *ends when a program that waits on inotify, `tail -f` or a build
that runs again on a save, notices a change here the moment it is
written.*

**0.4.17 the rest of memory.** mremap only makes a mapping smaller, and
nothing can be kept from being paged. a mapping that grows and moves,
`mlock` and the rest of its family, `mincore`, every `madvise`,
`memfd_create` and its seals, `membarrier`, `userfaultfd`, the numa calls
answered for the one node there is, protection keys answered as on a cpu
that has none, `mseal`, and a shadow stack where the cpu keeps one. and
memory past memory: swap, pages written to a disk when the machine runs
short and read back when they are touched, `swapon` and `swapoff`. *ends
when a program with a garbage collector and a jit, a javascript runtime
copied off a linux machine, passes its own tests here.*

**0.4.18 time the rest of the way.** the interval timers count real time
only, and a process has no timer of its own. the posix timers,
`timer_create` and the rest, on every clock, the cpu time clocks among
them; the virtual and profiling interval timers; and a clock slewed
rather than only set, with `adjtimex` and `clock_adjtime`, because a
clock that is only ever set jumps, and every timestamp taken across the
jump is wrong by it. and the vdso: the page every program finds through
`AT_SYSINFO_EHDR`, which answers `clock_gettime`, `gettimeofday`, `time`
and `getcpu` without entering the kernel. *ends when a time daemon copied
off a linux machine keeps this clock to a server's by slewing it.*

**0.4.19 scheduling the way programs ask.** there are no priorities
anywhere in the scheduler, only one shared run queue that any idle core
takes from (0.2.2), so this is a change to how the pick is made rather
than a second queue: niceness, `getpriority` and `setpriority`; the
policies, `SCHED_OTHER`, `SCHED_BATCH`, `SCHED_IDLE`, `SCHED_FIFO` and
`SCHED_RR`, and their parameters, `sched_setattr` and the rest;
`sched_setaffinity` beside the get that is here; i/o priorities; and
`rseq`. and the futex calls that need a priority to mean anything, the
priority inheriting ones, with `FUTEX_WAKE_OP`, `futex_waitv` and the
newer futex calls. the thing it buys is specific: a compile in the
background should not make an editor lag. *ends when `nice`, `chrt` and
`taskset` from a linux machine do here what they do there, and a thread
holding a priority inheriting mutex runs at the priority of the one
waiting for it.*

**0.4.20 permissions rather than uid 0.** the kernel asks "is this uid 0"
by name wherever it decides whether something is allowed: to mount a
disk, to change a file's owner, to signal somebody else's program, to
read the kernel's caches. capabilities take its place, `capget` and
`capset` and the ones a file carries, so each is a permission a process
holds, all of them uid 0's to begin with, and one that can be given away
without the rest, or given up for good with `no_new_privs`. `exec`
honours the setuid and setgid bits, which nothing here acts on today, so
that a program runs as the file's owner rather than as whoever started
it: the one way a person can act as another without being them. and
`setfsuid`, `setfsgid`, and the kernel's keyrings, `add_key`,
`request_key` and `keyctl`, which programs keep a person's secrets in.
*ends when `ping` from a linux machine sends its echo with a capability
and no setuid bit, and `setpriv` from a linux machine gives one up here.*

**0.4.21 resource limits that hold.** `setrlimit` and `prlimit64` keep
every limit a program sets, and only the one on descriptors is held to: a
program can still take every page of memory and every process there is.
0.3.13 drew this line once already and drew it in the other direction: a
reserve only the kernel may draw on, so that running out costs the
program rather than the machine. this is the same line between two
*people*: memory, processes, the size of a file, time on a cpu and the
size of a core, held to per process and per user, so that one person
cannot fork-bomb a machine into a state where another cannot log in to
kill it. and process accounting, `acct`, a record of every process that
ended. *ends when a fork bomb under one user's limit leaves the machine
answering another's login, and `prlimit` from a linux machine sets and
reads the limits here.*

**0.4.22 one process reaching into another.** what a debugger needs from
a kernel: stopping a process, reading and writing its registers and its
memory, `int3` for breakpoints and the trap flag for stepping, each
reported to whoever is watching, which is `ptrace`, with
`process_vm_readv`, `process_vm_writev` and `kcmp` beside it. it is the
one thing this kernel has always refused, one process reaching into
another's address space, which is why it is a version rather than a
program, and why it wants 0.4.20's capabilities underneath it, so that
reaching in is a permission somebody holds rather than a thing that is
possible. with it, a process known by a descriptor rather than a number
that can be reused: `pidfd_open`, `pidfd_send_signal`, `pidfd_getfd`,
`waitid`, `clone3`, `execveat`, and signals queued with a value. *ends
when strace and gdb copied off a linux machine trace and debug a program
here.*

**0.4.23 core dumps.** a program that dies leaves nothing behind but a
message. an ELF core (the registers, the mappings, the memory that
mattered) written where the person can find it, in the format everybody
else's tools already read, as big as 0.4.21's limit lets it be. that last
clause is the whole verification and it costs nothing extra: gdb on the
development machine opening a core written by velvet is the test.

**0.4.24 the older ways processes share.** system v's shared memory,
semaphores and message queues, posix's message queues, `mq_open` and the
rest, and `/dev/shm`, the memory filesystem posix shared memory lives
in. *ends when a database that keeps its buffers in system v shared
memory, postgres copied off a linux machine, starts here and answers a
query.*

**0.4.25 asynchronous i/o.** the old interface, `io_setup`, `io_submit`
and `io_getevents`, and io_uring: two rings in memory shared with the
kernel, requests in one and what became of them in the other, filled
and emptied without a call for every operation. *ends when liburing's
tests, built on a linux machine, run here, and every failure is fixed or
written down as a decision.*

**0.4.26 namespaces, and mounts made the new way.** a process sees the
machine through namespaces, and a container is a process given new
ones: the mounts, the process ids, the hostname, ipc, the network, the
users, the cgroups and the time, made with `unshare` and `clone`'s flags
and entered with `setns`. and the mount interface that goes with them:
`open_tree`, `move_mount`, `fsopen`, `fsconfig`, `fsmount`, `fspick`,
`mount_setattr`, `statmount` and `listmount`. *ends when a sandbox copied
off a linux machine, bubblewrap, runs a shell in namespaces of its own
here.*

**0.4.27 cgroups.** the cgroup2 filesystem, a tree of groups with every
process in one, and the controllers that hold a group to its share: cpu,
memory, pids and io. *ends when a group held to a memory limit is held to
it here, and a service started the way a service manager starts one,
in a group of its own, stays in it.*

**0.4.28 programs the kernel runs, and its events.** bpf: programs ring 3
hands the kernel, checked before they run and attached where the kernel
offers, seccomp's filters among them, which hold a process to the calls
it may make; landlock, which holds it to the files it may reach; and
perf events: the cpu's counters, samples taken on the lapic timer, which
has been per-cpu since 0.2.2, and unwound from wherever the interrupt
landed, and the tracepoints, which is how the kernel says what it is
doing to a program that asks: which call, from whom, its arguments as
the registers held them and what came back. the unwinding works only
because `-fno-omit-frame-pointer` has been in the build since the
beginning for the backtrace's sake, which is the second thing that flag
pays for. *ends when perf copied off a linux machine records where a
build spends its minutes here, and a program under a seccomp filter is
stopped at the call it may not make.*

**0.4.29 the machine as `/sys` says it.** a program learns what devices
the machine has from `/sys`: what is attached and to what, each device's
class, the block devices and their sizes, the network interfaces. it
tunes the kernel through `/proc/sys`, which `sysctl` reads and writes,
`core_pattern` among it. and `/dev/kmsg`, the kernel's log read and
written as a file, which is the orderly version of the serial-port
logging this project has leaned on since 0.1.x. *ends when `lsblk`,
`lspci` and `sysctl` copied off a linux machine say here what is here.*

**0.4.30 32-bit programs.** the ia32 interface: `int 0x80` and the 32-bit
system call table, the layout every structure has there, a 32-bit
program's stack and auxiliary vector, and the descriptors it sets with
`set_thread_area` and `modify_ldt`. *ends when a 32-bit busybox, built
static and copied off a linux machine, runs its tests here as the 64-bit
one did in 0.4.11.*

**0.4.31 the rest, and linux's own tests.** whatever is still left:
`personality`, the i/o ports `iopl` and `ioperm` give root, disk quotas
and `quotactl`, `kexec`, `vhangup`, the security module calls answered as
by a kernel with none, the module calls answered as by a kernel built
without modules, and the calls linux itself no longer answers, answered
as it answers them. and the check that says whether anything is still
left: the linux test project's system call tests, built on a linux
machine and run here unchanged. *ends when they run here and every
failure is either fixed or written down as a decision, which is what
full support means here: any program built for linux runs here
unchanged.*

## 0.5.x: what the kernel keeps from an attacker

0.4.x gives a program everything linux would. this series is about what
it cannot take: bytes nobody can predict, keys that never touch the
disk, and an address space an exploit cannot count on. none of it is an
interface a program calls differently; all of it is the kernel being
harder to fool.

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

**0.5.1 the primitives, and the first thing here that a right answer does
not prove.** the ones the kernel itself uses: aes-xts, the cipher a vault
(0.5.2) is written in, and sha-256 and hmac for the verifier in a vault's
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

**0.5.2 vaults.** a driver between the disk and the vfs, which is where
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

**0.5.3 an address space that moves.** the vmm can place things anywhere
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

**what this series deliberately does not reach.** no mandatory access
control: owner, group and other with capabilities underneath is the
model this machine's filesystems store and 0.4.20 holds to, and the next
model up, a security module with a policy of its own, is a research
project.

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
admitted, so that what 0.4.18's slewing starts from is the right time
and not merely a steady one.

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
of syscalls by kind. what a debugger, a tracer and a profiler need to
ask that of a program, ptrace, core dumps and perf events, are linux's
interface and came with 0.4.x. this series is the kernel saying it of
itself: why it went wrong, and where its own time went.

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

**0.7.1 a gdb stub.** the remote serial protocol, over the wire or the
serial line, so that *real gdb* attaches to this kernel. it is the
strongest outside answer a debugger can have: a protocol we did not
design, driven by a program we did not write, against a kernel that has to
be right about every register it claims to report. and it is what makes
debugging the kernel itself possible, which 0.4.22 deliberately does not
attempt.

**0.7.2 the log, and a panic that outlives the machine.** the kernel's
log can be read from ring 3 since 0.4.1, and as `/dev/kmsg` since 0.4.29;
this is the part that matters on hardware that is not here. the panic is
written to the medium before the machine gives up, so the evidence
survives the reboot. 0.6.6 writes the first log of a boot for the same
reason; this writes the last.

**0.7.3 numbers that are kept.** a suite that measures instead of
checking: boot time, the cost of a syscall, a page fault, a context
switch, a megabyte through the disk and a megabyte through the wire.
recorded in the tree, per version, because the only outside answer
available for performance is *yesterday*, and a regression nobody wrote
down is a regression nobody finds. this project has twice discovered a
slowdown by feel (0.3.10 twice over), and feeling it is not a method.

**0.7.4 breaking it on purpose.** 0.3.23 and 0.3.24 stop the machine at
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
