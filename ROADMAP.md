# where this is going

0.1.0 is the point where programs stopped being a demo and became a
thing the kernel actually hosts: their own memory, their own privilege
level, reclaimed when they die.

what follows is a lot of small steps rather than one heroic one. each
line below should be a version on its own -- buildable, bootable, and
worth a paragraph in the changelog.

0.1.x filled in what having processes implies. 0.2.x is about the
assumptions underneath *that* turning out to be too small. 0.3.x is
where the machine stops being the only one in the room, and then turns
into somewhere a person could actually work.

0.4.x is the one everything else is for, and everything after it is
written at the bottom too: 0.5.x is about who the machine belongs to,
0.6.x about software it did not write, 0.7.x about hardware nobody
emulated, and 0.8.x about a machine that can say what it is doing.

this is the kernel's half of that plan. until 0.4.0 the kernel and the
system on top of it were one tree with one roadmap, and the entries here
are the ones whose code is the kernel's, under the numbers they shipped
as or were planned as. the programs, the libc, the toolchain and git went
to velvetOS's roadmap along with their code, so the numbers have gaps,
and every gap is a version velvetOS's roadmap holds. an entry that was
about both is in whichever roadmap holds most of it, or split between
the two where its paragraphs split cleanly.

from 0.4.0 the kernel is released on its own, and its numbers are its
own. the planned entries keep the numbers they were given while the plan
was one plan, so a release here may skip a number velvetOS holds.

## 0.1.x: filling in what having processes implies

**0.1.1 processes, not just threads.** ~~exit codes carried back to
whoever waited. a real process table with a parent. `ps` splitting
kernel threads from programs properly. `kill` learning to end a
program rather than refusing anything blocked -- which needs threads to
carry a back-pointer to the waitq they are parked on, the same gap that
makes `kill` timid today.~~ **done in 0.1.1.**

**0.1.2 more to ask for.** ~~the syscall table is six calls wide. it
wants `open`/`read`/`close` against the ramdisk so a program can read a
file instead of being handed one; `getpid`; `spawn` and `wait` so a
program can start another. that last pair is what makes a userspace
shell possible at all.~~ **done in 0.1.2.**

**0.1.3 input that belongs to somebody.** ~~right now the kernel shell
peeks at keys while a program runs and hopes. a foreground process
should *own* the input queue, with ctrl+c delivered to it rather than
handled on its behalf. that is the beginning of a controlling
terminal.~~ **done in 0.1.3.**

**0.1.5 users.** ~~a read-only `passwd` in the ramdisk, a login prompt, a
uid on each process, and syscalls that check it.~~ **done in 0.1.5.** worth saying: this
only means something because ring 3 exists -- without a boundary the
hardware defends, a "user" is a variable that says you are an admin.
persistence is not required for this; only *changing* users needs a
writable disk.

**0.1.6 a kernel that measures itself.** ~~per-process cpu time, so `ps`
grows a cpu% column and the scheduler stops being theoretical. a `top`
that redraws. peak memory. how many syscalls of each kind.~~ **done in
0.1.6.**

**0.1.7 modern interrupt hardware.** ~~acpi tables (the loader hands over the
rsdp), then the lapic and ioapic in place of the 8259, and the lapic
timer in place of the pit. lateral on its own -- the same behaviour on
better hardware -- but it is the prerequisite for more than one cpu.~~
**done in 0.1.7**, with a fallback: if the firmware will not say where
the apics are, the 8259 keeps the job and nothing above notices.

**0.1.8 knowing what is plugged in.** ~~pci enumeration and an `lspci`.
small, satisfying, and the doorway to every real device driver.~~
**done in 0.1.8.**

**0.1.9 allocators worth the name.** ~~the pmm is a linear bitmap scan; a
buddy allocator would make it logarithmic. a slab allocator for the
fixed-size things I allocate constantly (threads, address spaces). a
`kmalloc` that is not first-fit.~~ **done in 0.1.9.**

**0.1.10 a real filesystem, on a real disk.** ~~an ahci driver and fat32, read and write.~~ **done in 0.1.10.**

**0.1.11 one namespace.** ~~the disk becomes the root and the ramdisk
moves to /boot, instead of the disk being bolted on at /disk. a bare
name is looked for on the disk first and the ramdisk second, so a disk
may supply its own copy of anything and a machine without one carries on
exactly as before.~~ **done in 0.1.11.** the ramdisk stays: it is what
makes the machine work when the disk does not, and it is the foundation
for booting one medium to install onto another.

**0.1.12 my own bootloader.** ~~stage 1 and stage 2, filling in the same
structures the kernel expects so the kernel itself need not change.~~
**done in 0.1.12.** philemon: one file, whose first 512 bytes are the
only part the bios will read, and a 64-bit half in C. the bootloader it
replaced is gone entirely, along with the iso, the uefi path and the boot
protocol that came with it. the kernel is handed one struct in rdi and
knows nothing about anybody's protocol including mine.

## 0.2.x: the kernel stops being one thing at a time

0.1.x was about a kernel that hosts programs. 0.2.x is about the
assumptions underneath that turning out to be too small: one cpu, one
directory, one architecture, one process at the front of everything.

**0.2.0 more than one cpu.** ~~the firmware has been telling me how many
there are since 0.1.7 and I have been ignoring all but the first. the
other cores wake in real mode at a page-aligned address below a
megabyte, so this needs a second small trampoline out into long mode --
philemon just taught me how to write one, and this time the budget is
four kilobytes rather than 512 bytes. per-cpu state to go with it:
`current`, the idle thread, the tss and the gdt stop being globals.
one enormous lock around the kernel to begin with, because a wrong
answer that is slow is still an answer.~~ **done in 0.2.0**, except for
the lock -- the woken cores halt instead. giving them kernel code to run
before 0.2.1 has replaced the thirty-nine places that call `cli` mutual
exclusion would not be a slow answer, it would be a corrupt one.

**0.2.1 locks worth the name.** ~~there are 39 `irq_save` pairs across
twelve files and every one of them is a lie on a second core: turning
interrupts off here says nothing about a thread running there. this is
the audit -- spinlocks, and then going through all 39 deciding what each
one was ever protecting. some become locks, some become per-cpu data,
and some turn out never to have needed anything. the fat32 driver has a
single 512-byte scratch buffer shared by every call into it, which is
not a race so much as a promise of one. the allocators get a host test
that hammers them from several threads at once, since that is the only
way I can see these bugs at all.~~ **done in 0.2.1.** the rank rule
complains rather than panics for now: with one core running kernel code
a wrong order cannot deadlock anything, so it should cost a line of text
and not a working machine. it becomes fatal in 0.2.2.

**0.2.2 a scheduler on every core.** ~~a run queue per cpu instead of one
behind a lock, work moved between them when they drift apart, and tlb
shootdown -- when one core unmaps a page the others still have it
cached, and nothing in the hardware tells them. that takes an
inter-processor interrupt and a handshake, and it is where real kernels
have real bugs. `ps` gains a column for which core, and `top` a row per
core.~~ **done in 0.2.2**, with one deliberate difference: **one shared
run queue that every core picks from**, rather than a queue each with
migration between them. at four cores the lock is not the bottleneck,
and an idle core taking whatever is ready is load balancing already --
without the machinery that per-core queues then need in order to undo
what they took apart. worth revisiting when there is evidence the lock
is the thing in the way.

**0.2.3 somewhere to stand.** ~~the shell has no idea where it is. `cd`,
`pwd`, a working directory per process, and paths resolved relative to
it -- `..` included, which the vfs currently throws away. `mkdir` and
`rmdir` to go with it.~~ **done in 0.2.3.**

**0.2.4 commands that are just commands.** ~~`run bin/cat` is an
embarrassment left over from when running a program was the
demonstration. a program should be typed by its name. the shell learns
where to look, `run` survives only for saying explicitly what to run,
and the two lists in `help` become one.~~ **done in 0.2.4.**

**0.2.7 making and unmaking.** ~~`rm`, `cp`, `mv`, `touch`, and files
that remember when they were written. fat has fields for all of that
and I have been writing zeroes into them.~~ **done in 0.2.7.** the two
things worth writing down: `mv` moves a name and not a file, so moving
a hundred megabytes costs the same as moving nothing -- and `rename`
writes the new entry before striking out the old one, deliberately, so
that a machine dying between the two leaves a file with two names
rather than none. `stat` came with it, because `ls -l` wanted a size
and a date for every name in a directory and opening each one to find
out would be a descriptor apiece for what the directory entry already
said.

**0.2.8 pipes.** ~~`cat x | head` has been an error message since
0.1.10. a pipe is a descriptor with a buffer behind it, a reader that
blocks until there is something or the writer is gone, and two
processes wired together at spawn. then `head`, `wc`, `grep` and
`sort`, which are only worth having once there is something to connect
them to.~~ **done in 0.2.8.** the buffer was the easy half. the two
rules hanging off the reference counts are the whole thing: a read
returns 0 when the last writer goes, which is the only reason a
pipeline ever finishes, and a write fails when the last reader goes,
which is the only reason `cat huge | head` stops instead of blocking
forever. unix raises SIGPIPE there and the default is to die; with no
signals I do the dying part directly. builtins cannot be in a pipeline
and it says so -- the shell is a kernel thread printing straight at the
screen, so it has no stdout to hand anybody.

**0.2.9 an editor, and redirection.** the editor, `margaret`, is
velvetOS's; the kernel's half of the version is below.

it needed four syscalls nothing had wanted before: one key rather than a
line, the size of the screen, where to put the cursor, and clear. asking
for a key *is* raw mode -- there is no flag anywhere saying a terminal
is raw, because asking for a line and asking for a key are different
questions and the answer to each is obvious.

`write` is gone with it. `echo hello > file.txt` says the same thing
with punctuation everybody already knows, and it is one program fewer.
that took the change 0.2.8 said it was deferring: **0, 1 and 2 became
real descriptors**. they had never been entries in the table at all --
the syscall layer answered them directly, because until pipes there was
exactly one place each could point -- and pointing stdout at a *file*
has nowhere to be written down until they are slots like any other.
`>`, `>>` and `<`, per command rather than per line, so `sort < a > b`
is one stage with both ends moved.

**0.2.10 job control.** ~~ctrl+z, `bg`, `fg`, `jobs`, and process groups
underneath them -- which the terminal half-knows about already, since it
has had a foreground process since 0.1.3.~~ **done in 0.2.10.** stopping
a thread turned out to want a *flag* rather than a state: a suspended
thread may also be blocked on a pipe or asleep, and those answer
different questions -- "what is it waiting for" against "may it run at
all". squeezing both into one enum means a stopped thread forgetting it
was stopped the moment anybody wakes it.

the terminal talks to a group now rather than a pid, because `cat x |
wc -l` is two processes and one thing the person typing is thinking
about. and with no signals, a suspended job announces itself by leaving
a note where the shell will look: waiting has a second way to finish.

**0.2.11 fork, and copy on write.** ~~spawning is the only way to make a
process and it builds one from a file every time. `fork` copies an
address space instead -- or rather does not copy it, marks every page
read-only in both, and copies one page at a time as somebody writes.
the page fault handler stops being purely an error path.~~ **done in
0.2.11.** the awkward part was not the page tables, it was the two
returns: a forked child has to come back from a `syscall` it never
made, holding everything its parent held -- and half of that is in
callee-saved registers that the abi says are somebody else's problem,
sitting in the cpu at the moment of the call and buried under a C
prologue a moment later. so the entry stub writes the whole of ring 3
down on every call now, six extra pushes, for one caller.

the other thing worth writing down: the *parent's* pages have to lose
their write bit too. protecting only the child gives you a fork where
the parent quietly writes through the child's memory, and it looks
like it works.

**0.2.12 demand paging.** ~~if a fault can mean "copy this page" it can
mean "there was never a page here yet". stacks that grow, `mmap`, and
programs that start faster because nothing is loaded until it is
read.~~ **done in 0.2.12.** the whole of it hangs on one thing: a
*record* saying which addresses are legitimately empty. without one
there is no telling a stack that wants to grow from a program
dereferencing nonsense, and a kernel that guesses wrong either kills
good programs or conjures memory for bad ones.

so each space keeps a small table of ranges it has agreed to, and every
not-present fault is answered out of it or not at all. the stack is a
megabyte of range with two pages in it; the address below is nothing,
which is the guard page for free -- it costs no memory because there is
nothing there to cost anything.

lazy program loading is for images that will still be in memory when
the program runs, which is the ramdisk and therefore every program
there is. one read off the disk is a copy on the heap somebody has to
free, and making it outlive an unknown number of forks is a lifetime
scheme demand paging does not need in order to be worth having.

**0.2.13 a cache between the disk and everything else.** ~~every read
goes to the drive today, one sector at a time, through a single bounce
buffer. a cache of blocks, dirty ones written back later, and `sync` to
mean it.~~ **done in 0.2.13.** it slots exactly where fat32's two
function pointers already were, which is the whole reason it could be
added without the filesystem knowing: fat32 was handed a way to move
sectors and it is still handed a way to move sectors.

the write-back half is a promise broken on purpose. until a sync, what
is on the disk is not what the machine believes -- which is why unix
has had the command since 1971, why reboot and poweroff call it, and
why there is a flusher on a timer to turn "you might lose anything"
into "you might lose the last few seconds".

**0.2.14 a filesystem with opinions.** ~~fat records no ownership and no
permissions, which is why everything on the disk is 0644 by decree. a
filesystem that has them -- ext2, or one of my own -- plus symlinks and
proper timestamps. the vfs finally has two things to be a layer
over.~~ **done in 0.2.14.** ext2, read and written: superblock, block
groups, bitmaps, inodes, and the twelve-direct-then-indirect block map
that every unix filesystem of the era used.

the difference from fat is one sentence: **a name and a file are
different objects**. in fat a file *is* its directory entry, so it has
exactly one name and ownership has nowhere to live. here a directory
entry points at an inode, which is why permissions belong to the file
rather than to the name, why a rename moves nothing, and why `chmod`
finally has somewhere to write its answer.

there is no e2fsck on this machine, so the formatter and the driver
would otherwise be two programs by one author agreeing with each other.
tools/readext2.py is the answer: a reader written from the on-disk
layout, run by the test target *after* the driver has finished writing
to the image. it found two real bugs before the driver existed.

**0.2.15 partitions.** ~~a disk is not a filesystem; it is a table
saying where several of them are. mbr and gpt, and mounting by which
partition rather than by which drive answered first.~~ **done in
0.2.15.** both tables, because a machine has to read both: mbr's four
sixteen-byte entries from 1983, and gpt's checksummed header and array
-- with the protective mbr a gpt disk carries so an old tool sees a
full disk rather than an empty one.

the crc is the interesting part. a gpt table that does not add up is
*known* to be corrupt and is refused, where a corrupt mbr is simply
followed. so the parser is judged by what it will not do: mounting a
filesystem at an address nobody chose is worse than mounting nothing.

the filesystem is handed a view of one partition rather than of the
drive, so every address it uses is its own and it never finds out it is
not alone. a drive with no table gets one entry covering the whole of
itself -- an image written straight to sector zero is ordinary, and
should not be a special case anywhere above.

**0.2.16 more than one screen.** ~~alt+f1 through f4, several sessions
at once, each with its own foreground process and its own scrollback.
the terminal layer has been one machine pretending to be one seat.~~
**done in 0.2.16.** the console already kept a shadow of every cell --
added in 0.1.x so a block cursor could put back the character it was
sitting on -- and a console nobody is looking at turns out to be
exactly that shadow with nothing painting it. most of the driver is the
code it always was with one question in front of the parts that touch
pixels.

the harder half was the shell. its state was file-static, which was
correct while there was one of it and became a bug the moment there
were four: four shells sharing one working directory is one shell with
four windows onto it. it is a session per console now, reached through
the calling thread rather than passed in.

and one rule the whole thing turns on: **output belongs to its writer,
input belongs to the screen**. a shell on console 2 printing while
console 1 is displayed must not scribble over console 1; a process at
the front of console 3 is at the front of console 3 and is still not
being typed at.

**0.2.17 something to point with.** ~~the ps/2 mouse, a cursor, and
whatever it turns out to be good for. the first input that is not a
stream of characters.~~ **done in 0.2.17.** what it turned out to be
good for is what a pointer has been good for on a text console since
gpm in 1993: drag over a path in an `ls`, press the middle button at a
prompt, and it is typed for you. the characters go into the input queue
as though somebody had pressed the keys, so nothing above knows a mouse
exists -- the shell's line editor cannot tell and does not have to.

and it really is a different shape of input. a queue is right for
typing because typing *is* a sequence, and the order is the meaning. a
mouse reports a change since last time and the interesting thing is
never one report, it is where the pointer ended up -- so the driver
keeps a position and the events are edges.

the one hard part is that the 8042 has no framing. a packet is three
bytes and nothing marks where one starts except a bit that is always
set, so a single dropped byte puts every packet after it one out of
step and the pointer flies off in a straight line. that looks like a
hardware fault and is not, which is why the count of discarded bytes is
something `mouse` prints.

**0.2.18 variables, and scripts.** ~~an environment inherited across
spawn, `$PATH` meaning what it means everywhere else, and a shell that
can read a file of commands with `if` and `while` in it.~~ **done in
0.2.18.** the environment is one block of "NAME=value" strings on the
process, which is the shape it is for a reason: inheriting is one
memcpy, and inheriting is most of what an environment is *for*. a table
of pointers would need every one of them rewritten on the way into a
child.

`$PATH` means what it means everywhere else now, including for
completion -- which had been walking a fixed list that happened to be
the same one, and stopped being the same one the moment PATH became a
variable that did something. completion offering a program that cannot
be run is worse than no completion: it is completion that lies.

the script syntax ends blocks with `end` rather than `fi` and `done`,
deliberately. borrowing sh's spellings would claim a compatibility that
does not exist -- no functions, no arithmetic, no `&&`, no quoting to
speak of -- and a script that looks like sh and is not is worse than
one that plainly is not. a condition *is* sh's rule, because it is the
right one: a command, true when it exits zero, which makes every
program on the machine a usable condition without any of them knowing.

**0.2.19 an init worth the name.** ~~the shell is started by `kmain`
because there was nothing else to start it. a first process that owns
the others, brings things up in an order, restarts what dies, and shuts
the machine down tidily.~~ **done in 0.2.19.** the four clauses turn out
to be one job seen from four sides, and the one that pays for the rest
is *restarts what dies*: `logout` can end a session now instead of
calling `login` from inside it, so the next person does not inherit the
last one's directory, history, jobs and variables.

the rule with teeth is the one every init has had since sysvinit:
something that dies instantly and is restarted instantly is a machine
that does nothing else ever again. five deaths inside ten seconds and
init leaves it down and says so. the *window* matters as much as the
count -- five deaths across an afternoon is five people logging out, and
a machine that gave up on a console for having been used would be
worse than one with no rule at all.

shutting down is where owning things stops being theoretical. `reboot`
used to sync the disk and reset from whichever console typed it, while
three other sessions carried on writing -- so what reached the drive was
whatever was dirty at the instant somebody asked. stopping everything
first is not something a command can do to itself.

and pid 1 is not decoration: reparenting needs a number that is known
before the process it names exists. a child whose parent dies becomes
init's, and init collects it -- which replaced a sweep that ran on every
spawn and took *every* finished process with it, background jobs whose
exit codes nobody had read yet included.

**0.2.20 the x86 parts, in one place.** ~~everything that assumes this
architecture is scattered through the tree. an `arch/` boundary, drawn
from the outside in, so the rest of the kernel stops naming registers it
has no business knowing about. no new behaviour at all -- the test is
that nothing changes.~~ **done in 0.2.20.** drawn from the outside in
turned out to be the whole instruction: the headers are named for what
the kernel *wants* -- may interrupts happen, stop until something
occurs, this mapping is stale, land the kernel here when this thread
traps -- rather than for what x86 provides. drawn the other way round it
would have produced a `write_cr3()`, which is an x86 instruction wearing
a portable-looking name and worse than the inline asm it replaced,
because the inline asm at least admitted what it was.

the biggest single leak was not the assembly, it was `cpu/interrupts.h`:
nineteen files across mm, sched, fs, drivers and lib included an x86
header to get four lines of interrupt masking, and got a struct listing
rax through r15 and the whole 8259 along with them.

and the version's real deliverable is `tools/checkarch.py`, because a
boundary is worth exactly what checks it. it fails the build on inline
assembly outside `arch/` and on any portable file naming x86, and it
prints the ten files still allowed to -- with the reason for each. the
list is meant to shrink; three of the entries I first wrote turned out
to be wrong, which the checker said so on its first run.

"nothing changes" is checkable and was checked: 1057 of the 1073
functions in both builds have byte-identical instruction sequences, and
every one of the sixteen that differ is accounted for. two of them are
not changes at all -- two files each have a static function called
`mine`, and comparing by name compares the pair.

the thing that nearly slipped through: this kernel builds at `-O0`,
where a plain `static inline` is a call like any other. for `cpu_relax`
that is merely slower; for `cpu_frame_pointer` it is *wrong*, because a
called function reads its own frame and the backtrace would have started
one line low, politely reporting itself.

**cancelled: a second architecture.** it holds no version number,
because it shipped nothing: 0.2.21 below is the next thing to build.
aarch64 on qemu's virt board: a different uart, a different interrupt controller, a different
timer, a different mmu, and the same kernel above all four. the point was
never that anybody needs velvetOS on arm -- it was that the boundary drawn
in 0.2.20 is either real or it is decoration.

**it was written and it is gone.** the whole of it: the five contracts,
boot code that drops EL2 to EL1 and builds page tables before the first
print statement, a pl011, a gicv2, the generic timer, exception vectors,
a fabricated handoff with the ramdisk linked into the image. it compiled
-- every C file first try -- and it linked. it never ran, and it was
never going to run here: **I cannot test it.** the machine I develop on
has no way to boot an arm kernel, and a second architecture that only
one of us can build is worse than none, because it rots silently while
looking finished.

so it is out of the tree rather than sitting there as a promise. it is
not abandoned as an idea -- when there is hardware, or a setup I can
actually run it on, it comes back and this entry comes back with it.

**what it found is still here, and that was the point.** three things,
none of which needed the port to survive in order to stay fixed:

*the scheduler was building an x86 stack frame by hand.* `thread.c`
fabricated the frame a new thread resumes from -- six zeroes with a
comment naming each register, and a return address for `ret` to pop --
in portable code. neither checker could see it: no inline assembly, no
x86 header, and it compiles perfectly against an architecture that does
nothing, because it is integers written into memory. only writing the
second architecture found it. it is `context_make_stack()` now.

*`kmain` was a machine description with a kernel wrapped round it.* the
gdt, the 8259, the task state segment, the apics and the pci bus, in the
order they have to happen. three hooks now -- early, late, and the clock
-- and `main.c` came off the allow list.

*the serial driver was a terminal with a chip stuck to it.* an 8250 is
not an x86 chip; *reaching* it through a port space is. the escape
sequence machine stayed in `drivers/`, the `outb`s went to
`arch/x86_64/uart.c`, and `drivers/serial.c` came off the allow list
too. that is the question 0.2.20 deferred, answered for one driver.

and the half of the version that never needed arm at all: `arch/none/`,
an architecture where every function is empty and every constant is a
plausible lie, and `make portable-check`, which compiles the portable
kernel against it. thirty-five files build with no machine underneath
them, and the undefined symbols left over are the porting checklist,
derived mechanically rather than remembered. that runs anywhere and it
stays.

**0.2.21 a live mode, and an installer.** ~~the ramdisk has been kept
since 0.1.11 on the argument that it is what makes the machine work
when the disk does not. this is the other half of that argument: boot
from one medium, partition and format another, copy the system onto it,
and leave a machine that boots on its own. everything from 0.2.14 and
0.2.15 exists to make this the small step it ought to be.~~ **done in
0.2.21**, and it was nearly the small step it ought to be. two things
were missing and neither was small: this kernel could read a partition
table and not write one, and it could mount a filesystem and not make
one. every filesystem it had ever seen was formatted by a python script
on a development machine.

so 0.2.21 is mostly `mkfs_ext2` -- mke2fs, in the kernel, in the
smallest form that produces something real. it makes an *empty*
filesystem and nothing else: a root directory, no lost+found, no
reserved blocks. copying files into it afterwards goes through the
ordinary vfs_create and vfs_write that 0.2.14 already built and tested,
because a formatter that also laid out files would be a second
implementation of the half that works.

the verification is the part worth keeping. a formatter and a driver by
one author can agree on something wrong and both be happy -- that is
precisely what happened to mkfat.py in 0.1.10 and took an independently
written reader to find. so what it produces is checked three ways: the
arithmetic directly, then fs/ext2.c which is a separate implementation
of the same layout written a version earlier, and then
tools/readext2.py, which was written from the on-disk format and shares
code with neither. `make test` runs that last one over an image the
kernel formatted.

and then the sequence, which is three steps that each already had a
test and one property that none of them can see: **the order**. an mbr
partition table lives at offset 446 of the first sector, and the first
sector is also the one philemon lives in and the first thing the copy
overwrites. write the table first and the copy erases it. philemon's
first stage ends at byte 365, which is not luck -- it is the constraint
every bootloader sharing a disk with a table has worked under since
1983, and it is the only reason any of this fits.

live or installed finally means something, too. a drive carrying
philemon's table at sector 32 is a boot medium; the machine is
*installed* when that is also the drive the root filesystem came from.
one disk that both starts the machine and holds it. the boot log says
which, because it is the difference between a machine whose changes
survive a reboot and one whose changes do not.

## 0.3.x: the machine stops being alone, and becomes somewhere to work

**0.3.0 local networking.** ~~no internet and nothing routed -- just this
machine and whatever else is on the wire. a network card found the same
way the disk was, then arp, ipv4 and icmp, so it can be pinged and can
ping back. then udp, and enough of a socket layer for a program to use
it. `ifconfig`, `ping`, and something that lists what else is out
there.~~ **done in 0.3.0**, and the project is called velvetOS as of it.

the shape of the whole thing follows from one sentence that took a while
to become obvious: **a network protocol is almost entirely arithmetic
over a byte buffer.** a header is a set of offsets, a checksum is a sum,
an address is four bytes. so the card is one file and everything else in
`net/` is pure -- no card, no kernel -- which means every protocol here
is tested against packets laid out by hand from the rfcs, and not one
file in the directory names x86.

the checksum got the most attention, because three protocols use it and
it is wrong in ways that look right. three gaps my own tests missed:
folding the carries needs a *loop* rather than one pass (right for every
small packet, wrong near an mtu), udp's pseudo-header has to be summed
*with* the datagram (skip it and the stack accepts datagrams delivered
to the wrong host, which is the one thing it exists to prevent), and a
checksum that computes to zero goes out as all ones because zero means
"not computed".

and I nearly told a lie in a comment. the fixtures were described as
captured packets; they are not, and the bytes I wrote from memory
carried a checksum that was simply wrong. they are laid out from the
rfcs now and the comment says so.

**four things only a real machine found**, which is the honest measure
of what a host suite cannot reach:

the card answered every register read, reported its hardware address,
and moved not one frame in either direction. **pci bus mastering** was
never enabled, and could not be -- `pci.h` had no config *write* at all.
ahci never needed one because firmware enables it on whatever it boots
from; a network card gets no such favour.

a program faulted on an instruction fetch inside its own argv. `hermes`
was written with a `main`, and there is no crt here -- so the linker
could not resolve `ENTRY(_start)` and defaulted the entry to the start
of `.text`, which ran `write_num` with argc in the register it wanted a
number in. `ld` warned exactly that, in a line I had filtered out of the
build output.

sending to this machine's own address could never work, because there
was no **loopback**: arp asks who has an address nobody will claim,
since a card does not hand back what it just sent.

and `ping` tripped the lock rank rule -- the disk lock is held for a
whole create, and stamping the new file reads the cmos clock. the clock
has a rank of its own now, because it is a leaf: it takes no other lock
and never will.

what is deliberately not here: no tcp, no dhcp, no routing, and no
reassembly of fragments -- a fragment is dropped rather than read as a
whole datagram, which is how a stack ends up acting on half a packet.
`recvfrom` does not block either; it says "nothing yet" and the program
asks again, which is honest for a kernel with no way to park a thread on
a socket.

### the wire, finished

**0.3.1 an interrupt instead of a poll.** ~~the card is watched by a thread
twenty times a second, which is late by up to fifty milliseconds and
burns a wakeup whether or not anything arrived. the e1000 can raise an
interrupt; taking it means deciding what an interrupt handler is allowed
to do, and the answer is "put the frame somewhere and wake the thread"
rather than "parse ip" -- the poll thread stays, it just stops guessing
when to run.~~ **done in 0.3.1**, and the answer above held: the handler
reads one register and wakes the thread. two things I had not predicted.
`make checkarch` failed the moment the handler existed, because taking an
interrupt meant naming x86 -- and the fix was that not one of the five
handlers in this kernel had ever read the register frame it was passed,
so `irq_register` + `pic_unmask` became one portable `irq_install`. and
the fallback timer needs a *diagnostic*: a machine that never gets its
interrupt still works, half a second late, and looks identical to one
that does. the version's real cost was learning that this is three
states and not two -- "never wired" and "wired, nothing has arrived yet"
are not the same answer, and on a wire where nothing arrives unasked the
second one is the normal one.

**0.3.2 an address the network hands out.** ~~`ifconfig 10.0.2.15` is a
number somebody has to know. dhcp is a four-packet conversation --
discover, offer, request, acknowledge -- and it is the first thing here
that is a *protocol with states* rather than a packet with fields. it
also means the machine has to cope with not having an address yet, which
is a state it currently only passes through.~~ **done in 0.3.2.** the
state machine is pure -- told the time, handed packets, and *returns*
what to send -- which is what makes an eight-second backoff and a
twelve-hour lease testable in microseconds. the part the entry did not
predict: "cope with not having an address yet" is not a state to pass
through but a *send path of its own*, because every check in net_send_ip
is correct and every one of them is fatal to a machine with no address.
and two of the three bugs this version had were in what it *said* rather
than what it did -- a lease message that landed in the login prompt and
was typed into the name field, and a `dhcp` command that printed a
given-up lease exactly like a live one.

**0.3.3 tcp.** ~~the big one on the wire. a handshake, sequence numbers,
acknowledgements, retransmission, and a window -- which together are the
first thing in this kernel that has to remember what it said and be
prepared to say it again. udp needed a queue; this needs a state machine
per connection, and getting the close sequence wrong is how sockets pile
up in a state nobody can clear.~~ **done in 0.3.3.** the close sequence
was indeed where the bugs were, and all three were invisible to anybody
watching a connection work: TIME_WAIT answered nothing at all, which is
the whole reason the state exists; a *retransmitted* fin was ignored
because it carries a sequence number one behind what this end now
expects; and a bare ack provoked another ack, which two machines will
keep up between them forever. the last one is the only bug here that
gets worse the better the network is.

the version also found something bigger than itself. breaking a function
in tcp.h on purpose and watching the suite pass revealed that **test
binaries had no header prerequisites** -- the kernel rebuilds correctly
from -MMD dependency files and the tests never did, so for the whole
life of this project, editing a header and running `make test` ran the
previous binaries and reported them passing.

**0.3.4 sockets worth the name.** ~~`recvfrom` does not block: it says
"nothing yet" and the program asks again. that is a poll dressed as an
api, and it is only honest because there is no way to park a thread on a
socket. this is that way -- a waitq per socket, a blocking read, and
something like `select` so one program can wait on several things at
once, which is what every server is.~~ **done in 0.3.4**, and tcp got
its ring-3 interface at the same time because a stream read that could
not block would have been the same mistake twice.

the entry said "a waitq per socket" and that is right for a blocking
read and wrong for `select`: a thread can only be parked on *one* queue,
since the wait list is threaded through the thread itself. so select
waits on a shared queue and looks again when anything at all arrives --
more wakeups than it needs, and nothing worth measuring at eight
sockets.

two shapes here are honest about table sizes rather than general, and
both are written down where somebody will hit them: tcp handles are
offset by 64 so one number space serves both kinds, and `accept` returns
the *same* handle because a listening entry becomes the connection and
there is no backlog.

**0.3.5 names.** ~~dns, so an address can be a word. small on its own, and
the first time this machine has to *ask a question and wait for an
answer* on a protocol it does not control the other end of.~~ **done in
0.3.5**, and it was not small, because "wait for an answer" turned out
to be the hard half. the first version parked the asking thread on a
waitq with a deadline checked only *on waking* -- and nothing woke it
when no reply came, so one `host` against a silent server hung the shell
for good and left the resolver jammed for every later name. a timeout
that is only checked when something else happens is not a timeout.

**and this entry quietly needed a route.** 0.3.6 cannot fetch anything
without one, and there was no entry for it anywhere: everything off this
wire had been refused since 0.2.x, and the gateway dhcp offers had gone
unused since 0.3.2. so `ip_next_hop` went in here -- on this wire, arp
asks for the destination; anywhere else, arp asks for the *gateway*
while the ip header still names where it is really going. one route, no
table.

**0.3.6 fetching something.** ~~enough http to pull a file down. the point
is not the web -- it is that a machine which can fetch a tarball is a
machine that can be given things, which is how everything after this
gets easier.~~ **done in 0.3.6.** http itself was the small half: the
interesting one is that a response arrives as whatever pieces tcp felt
like delivering, so the reader holds its position between calls and the
test drives every fixture through it split at every byte offset. a
reader that only works when the status line arrives whole works until
the network is slightly busier.

**and it found a real bug in 0.3.3.** four failed fetches filled the
connection table with entries that could never finish: nobody read the
responses, a full receive buffer advertises a window of zero, a peer
with a zero window cannot send its fin, and a connection waiting for a
fin that cannot be sent holds its slot until the machine is rebooted.
data arriving after the program has closed is now acknowledged and
discarded rather than buffered, and closing with data still unread sends
a *reset* -- those bytes were acknowledged and then thrown away, and a
fin would claim the conversation ended properly.

no https, and that is a real limit rather than a deferral: there is no
tls here, most of the internet will answer with a redirect to a scheme
this cannot speak, and the redirect is reported rather than followed.

### a machine somebody could work on

**0.3.7 signals.** ~~there is one flag called `interrupted` and it is as
close to a signal as this kernel gets. a compiler that runs for a minute
needs ctrl+c to mean something; a shell needs to know why a child died
rather than only that it did. handlers, a mask, and delivery on the way
out of a syscall -- and the hard part is not the delivery, it is every
place in the kernel that currently assumes a blocking call finishes for
one of two reasons.~~ **done in 0.3.7**, and the entry was right about
which half was hard. delivery is thirty lines; the rest is that a
blocked thread now has to be *woken* by a signal, or one sent to a
program parked on a read arrives whenever that read happens to finish --
which for a program waiting on the keyboard is never.

the mistake worth recording: making `process_take_interrupt` stop
consuming its flag, on the grounds that the signal is consumed
elsewhere. several callers loop until it clears, so the syscall suite
hung on a `write` that had nothing to do with signals. the flag and the
signal are two different facts and both have to be consumed, in
different places.

one divergence from unix is deliberate and documented: an *ignored*
signal still cuts a blocking call short, because `interrupted` predates
signals here and means "stop waiting" rather than "what becomes of this
process".

**0.3.9 a terminal that behaves.** ~~raw and cooked as a *mode* rather than
as two different syscalls, a window size somebody can ask for, and
control sequences that a program written for a real terminal would
recognise. margaret works today because it was written against exactly
what this kernel does; nothing else would.~~ **done in 0.3.9.** the
console understood *no* sequences at all, so a program that cleared the
screen printed `[2J` -- there is a parser in front of every byte now,
fed one byte at a time by its suite because that is what a program
writing `\033` and `[2J` separately produces.

the mode is three flags rather than one word, because they are
independent: a program can want raw keys and still want to be killable.
and it belongs to the *console* rather than the process, since a program
that dies in raw mode has to leave something the shell can put right.

what is still owed: sgr colour is parsed and ignored, so the sequence no
longer lands on the screen as text but the colour does not change
either. and nothing in the ramdisk asks for raw mode or the window size
yet -- **0.3.10** is where margaret should stop assuming both.

**0.3.10 margaret, grown up.** the editor is velvetOS's.

**the version's real finding was in the kernel.** rewriting the editor
showed that wiring the escape parser in for 0.3.9 made every cursor move
repaint the entire screen -- two thousand glyphs -- so an editor drawing
twenty lines paid for twenty full redraws. and `sys_write_console` had
been parsing a format string per character since long before that. both
were slowing every program down, not only this one.

**0.3.11 the small tools.** the tools are velvetOS's.

**and the first of them found a kernel bug older than any of them.**
`diff` is the first program here ever to hand malloc'd memory to a
syscall, and every read was refused: memory from `mmap` is *reserved*
rather than mapped, so a page the program has not yet touched has no
entry -- and `user_range_ok` refused it for not existing. nothing had
reached it because nothing had done it; `wordcount` reads a byte at a
time into a stack variable.

**0.3.12 time that means something.** ~~a monotonic clock that is not the
pit tick count, timers a program can set, and file timestamps that
survive a reboot with the right values in them. `make` is the reason:
it decides what to rebuild by comparing two numbers, and a clock that
resets to zero at boot makes every one of those comparisons a lie.~~
**done in 0.3.12.** the cmos chip is read *once*, at boot, and the timer
says how long ago that was -- the filesystem had been reading the chip
per file stamped, which is a device access each time and, worse, a clock
that can give the same second twice or skip one.

the conversion is checked against the host's own `timegm`, and the
property that matters is the round trip: every day of sixty years turned
into a number and back must be the same day. `alarm` is a deadline
rather than a countdown, compared where something is already looking.

still resting on its suite: nothing in the ramdisk calls `alarm`, so
that path has no running program behind it.

**0.3.13 memory under pressure.** ~~compiling is the first thing this
machine will do that can genuinely run out. what happens then is
currently "the allocator returns null and something gives up" -- which
is survivable but not *diagnosable*. a way to see where memory went, and
to fail one program rather than the machine.~~ **done in 0.3.13.** a
reserve only the kernel may draw on, checked in `populate` -- the one
place a program's memory arrives -- so that running out costs the
program while the kernel still has enough to write its output and say
why. and a memory column in `ps`, counted as pages arrive rather than by
walking four levels of page table to answer a number that could have
been kept.

the rule is a policy rather than a data structure and is tested apart
from the allocator. the mistake it is easiest to make: the reserve is
what would be *left*, not what is there now.

**0.3.17 hashes.** ~~sha-1 and crc32, because a repository is a store
addressed by the hash of what is in it, and because "did this arrive
intact" is a question that will keep coming up.~~ **done in 0.3.17**, and
adler32 as well, because a zlib stream ends with one and 0.3.18 would
have had to stop and write it. all three with a streaming form, in
`kernel/lib/hash.c`, which the kernel and `libc.a` both compile --
so a ring 3 program gets them out of the archive rather than out of a
second copy of the source.

checked against python, zlib and **`git hash-object`** by
`tools/hashcheck.py`: three hundred lengths, and every one of them again
in pieces of 1, 7, 63, 64 and 65 bytes, since 64 is the block size and a
published vector only ever hashes one buffer in one go. the git check is
the one that matters -- an object's name is the sha-1 of its type, its
length, a zero byte and then the content, which is the contract 0.3.19
rests on and is true now, before there is anything resting on it.

and the finding worth keeping: **adler32 taken modulo 65536 rather than
65521 passed every published vector**, because all three are short
enough that the sums never reach either, and passed the
whole-against-parts check too, because a wrong constant is wrong
consistently. only an outside answer caught it.

the gpt driver's private crc32 is gone, and so is the copy of the table
it had inlined a second time to checksum an entry array sector by
sector. there is a `sha1sum` to run, with `-c`, `-a` and `-g`.

**0.3.18 deflate and inflate.** ~~every object in a git repository is
compressed, so reading one means implementing this whether or not
anything else wants it. inflate is much the easier half and is enough to
*read* a repository; deflate can be a stored-block cheat at first, which
produces a valid repository that is merely large.~~ **done in 0.3.18**,
and the cheat was not needed: the compressor is fixed huffman with a
greedy match search, which is the middle of the three answers. storing
everything is four lines; a dynamic table is a second huffman *encoder*
for a saving that matters to a network and not to this machine. zlib
beats it about threefold and both files are equally valid.

inflate reads all three block types, which is the half that matters, and
`tools/zlibcheck.py` is built around the reason: deflate is three
formats in a trenchcoat and the *compression level* decides which comes
out -- 0 stores, low levels emit fixed codes, high ones build a table
per block. a decompressor checked only against our own compressor has
seen one third of the format, so every input goes through zlib at all
ten levels. **two bugs passed the suite and were caught only by the high
levels**: the code-length alphabet's transmission order, and the
repeat-the-last-length code appearing first in a table.

and the loose objects of a repository made by real git are inflated and
compared, header included -- so the thing 0.3.19 has to read is already
being read.

both files are in `libc.a` as well as in the kernel, which is how a ring
3 git will get at them, and is also the proof they compile freestanding.

### and the things that make it survivable

**0.3.21 the build tools, in C.** ~~*(and while the build is being rewritten:
`kernel/lib/string.c` and `userland/libc/string.c` are the same code
twice, since 0.3.8. one file compiled twice -- freestanding for the
kernel, hosted for ring 3 -- is a build-system change and this is the
version that touches the build system. the two printf formatters are
**not** the same and should stay apart: the kernel's streams into a
console, a serial line and a ring buffer under a lock, so a panic
half-way through still prints what it got.)* this is the entry that is easy to miss
and blocks everything. `gensyms.py`, `mkboot.py`, `bin2c.py`, `mkfat.py`,
`mkext2.py` and `checkfmt.py` are python, and a self-hosting machine has
no python and is not going to get one. every one of them has to be a C
program that lives in the image.~~ **done in 0.3.21**, all six, and the
string libraries with them -- one file compiled twice, and the
formatters left apart for the reason the entry gives.

the risk in a version like this is that **nothing looks different when
it goes wrong**. so each C tool is built beside the python it replaces
and `tools/toolcheck.py` runs both: byte for byte for the four that have
one right answer, and through a reader for the two formatters, since two
formatters that allocate in a different order produce two perfectly good
filesystems. when a python tool goes away its case there goes with it,
which is why they both still exist.

two of them stopped reaching for what the machine will not have.
`gensyms` asked `nm`; it reads the symbol table out of the ELF now, the
same structures 0.3.15's linker already walks. and `mkext2` was four
hundred and sixty lines of python laying out ext2 a *second* time, next
to `mkfs.c` and `ext2.c`, which have done it since 0.2.14 -- the
duplication `mkfs.c` says in its own comment it exists to avoid, sitting
in a different language where nobody looked.

**mkfat is where this stopped being a rewrite.** it populates through
`fs/fat32.c` for the same reason -- and the driver could read a long
name and refused to write one, so going through it would have renamed
half of `base/diskroot/`. that fix belonged in the driver, not the tool, and
now every program on the machine has it. a long name is several records
that go in together or not at all; the short name behind it is minted
with `~1`, `~2` until nothing in that directory shares those eleven
bytes; and the group is kept inside one cluster, with the slots it
cannot fit into **struck out** rather than skipped -- a zero byte left
in the middle of a directory is the end of that directory as far as
every reader is concerned, and everything after it would have been
invisible to everyone except the code that wrote it.

which is the whole argument for **`tools/readfat.py`**: a reader sharing
no line with the formatter or the driver, since the alternative is my
code checking my code. it goes over what the formatter wrote and over
what the suite wrote -- different claims -- and it found mkfat leaving
fsinfo saying the whole disk was free on a disk that was not.

**0.3.22 ext4.** ~~a newer filesystem than the one from 1993.~~ **done in
0.3.22**, and as the same driver grown up rather than a second one
beside it -- which is the only honest way, since ext4 *is* ext2 with
features bolted on and an ext4 disk holds files of both kinds at once.
two drivers would have been the same code twice with the bug fixed in
one of them. `fs/ext2.c` is `fs/ext4.c` now and which map a file uses is
a flag in its inode, not a property of the disk.

**extents** are what the version is actually about. ext2 answers "where
is block N" with a list of every block the file owns -- a gigabyte in
1 KiB blocks is a million numbers, and reading the file means reading
them. an extent says the other sentence: *the next N blocks are at P*.
a file written front to back is twelve bytes of map whatever its size.

reading walks a tree of any depth up to five; writing builds depth 0
and depth 1, which is three hundred and thirty-six runs of up to
thirty-two thousand blocks apiece, and a file this machine cannot
describe that way is one it has no room for. **the tree grows a level
exactly once per file** -- the inode's four records spilling into a
leaf -- and that is the only place its shape ever changes, so it is the
one thing the suite forces on purpose by growing two files a block at a
time so that neither gets a contiguous run.

what is read and deliberately *not written* is everything with a
checksum in it. modern mke2fs sets `metadata_csum`, every write then
owes a crc32c this driver does not compute, and a filesystem whose
checksums disagree with its contents is one fsck calls corrupt. so such
a disk **mounts read-only** rather than being refused or, worse,
written to -- and the same goes for one with a journal, since writing
beside a journal is how the older version of a block wins.

**0.3.23 a filesystem that survives the plug.** ~~the block cache means
what is on the disk lags what the machine believes, and 0.2.13 was
honest that this turns "you might lose anything" into "you might lose
the last few seconds". a machine holding the only copy of its own source
deserves better: an fsck of its own, and then either ordered writes or a
journal.~~ **done in 0.3.23**, by the first of those two -- ordered
writes, with the journal split out below and the reasons given there.

**an fsck of its own** is `fs/fsck.c`, and it reads the layout by hand
rather than through the driver, for the reason `readext4.py` refuses to
import anything: a checker that shares the driver's idea of what the
disk says cannot notice the driver being wrong. it mends only what has
exactly one right answer *and* can be computed rather than chosen --
free counts, link counts, blocks marked used that nothing claims, an
inode allocated whose name never landed. a block two files both claim
is reported and left alone, because one of them is wrong and nothing on
the disk says which.

**and then the ordering, which is the actual answer to the entry.** what
a crash leaves behind is decided entirely by the order the writes went
out in, and there are two possible outcomes: a *leak*, which has one
answer, or a *dangling reference*, which has none. three orders were
backwards and each left the unanswerable kind -- a link count written
before the name it counts, a directory reachable before it had its own
`.` and `..`, and a name removed before the count came down. all three
are the obvious order, and all three are wrong.

what makes this a claim rather than a hope is `tests/test_crash.c`: it
stops the machine at **every write there is**, seventy-nine of them, and
demands that what is left is mendable and that mending it leaves a
filesystem the checker then calls clean. not usually -- every one.

**0.3.24 the journal.** ~~what 0.3.23 bought is that a crash costs
nothing worse than a run of fsck. a journal is what makes the fsck
unnecessary: metadata goes to a log first and to its home second, so
recovery is replaying a list rather than deducing what happened. jbd2 is
a named format with a specification, since 0.3.22 made this ext4 -- and
it is the thing that would lift the read-only rule on any disk with a
journal already on it, which today is every ext4 anybody else made.

it is its own version because it is not a feature bolted to the side:
every metadata write becomes part of a transaction, and the block cache
underneath has to stop being free to write a block home whenever it
likes. doing that badly makes data loss *more* likely rather than less,
which is the one outcome worse than not having it.~~ **done in 0.3.24**,
and that last sentence turned out to be a description of the first
working version of it rather than a hypothetical: the crash suite went
from 0 stopping points leaving unanswerable damage to 4.

**the log is `fs/jbd2.c` and a filesystem of its own**: inode 8, a ring
of blocks, and a transaction in it is a descriptor saying where the
blocks after it belong, the blocks, and a commit block saying they all
arrived. every field big endian on a machine that is not, because a
journal is the one structure that may have to be read by a machine other
than the one that wrote it. what makes it a journal rather than a second
copy of the data is **three waits**: the blocks are on the disk before
the commit block claims they are, the commit block is on the disk before
anything goes to its home, and the homes are on the disk before the log
says it no longer holds them. the third is the one that is easy to leave
out and it is the one that protects the disk from the log -- a tail moved
past blocks the cache then lost is a change nothing will ever replay
again.

**doing it badly made things worse three times, and each was instructive
in a different way.** a data block was zeroed *through* the log and
filled straight to the disk, so the commit copied the log's zeroes over
the contents -- a symlink whose target did not fit in its inode lost the
target, which is what "ordered mode" means when you get the two paths
out of step. a freshly formatted log says `s_start = 0`, and treating
that as "nothing to replay" meant the whole of the first transaction sat
outside recovery's reach; an empty log is replayed too now, which costs
one read and closes the window. and a transaction that outgrew the log
was *abandoned* -- dropping its earliest writes and letting the rest go
to the disk, applying the end of an operation without its beginning,
which is damage invented by the thing meant to prevent it. it restarts
instead: what is staged is committed and the rest continues in a fresh
transaction, so the worst an operation too big for the log can leave is
what 0.3.23 left.

**`tests/test_crash.c` asks the harder question now.** it stopped the
machine at every write there is and demanded the wreckage be *mendable*;
it stops at all 145 of them, boots the machine again, and demands there
be nothing to mend. and because a journal checked only by the code that
wrote it is two hundred lines agreeing with themselves, `test_mkfs.c`
hand-writes a transaction from the format -- header, tag, block, commit
-- and requires the driver to replay it, and requires the same log
without its commit block to be replayed by nothing.

**what is not here is revoke.** every mke2fs sets it, so a log written
by linux is still refused and its disk still mounts read-only -- which
costs less than it sounds, because such a disk has `metadata_csum` on it
and is read-only for that reason anyway. none are written either, and
that one is a design rather than an omission: revoke exists because
linux checkpoints lazily, and this checkpoints inside the commit and
waits for it, so no transaction outlives the operation that made it.

**0.3.25 a disk that can grow.** ~~ext4 was written to be read and
written, not to be *managed*. resizing a filesystem, and a second
partition for work that is not the system, so that filling one does not
stop the other.~~ **done in 0.3.25**, and the two halves turned out to
be the same sentence twice: a filesystem is not the size of its disk,
and a filesystem is not *the* filesystem.

**growing is additive, and that is the whole of why it is safe.** a new
block group is a bitmap, an inode bitmap and an inode table written past
the end of the filesystem as the superblock currently describes it --
nothing can reach any of it, so a machine that stops half way through
has changed nothing at all. then one transaction moves one number and
the whole of it becomes real. the crash suite for it stops at every one
of the 217 writes a grow takes and demands a filesystem at every one.

**shrinking moves nothing either, and that is a limit rather than a
design.** emptying a tail means finding everything that points at every
block out there and correcting all of it, which is a different program.
so a shrink whose tail is already empty is arithmetic, and one whose
tail is not says how many blocks are in the way and stops. the third
outcome -- a shrink that succeeds and loses a file -- is the one that
must never happen.

**the ceiling is the descriptor table**, and the fix for it went in the
formatter rather than the resizer. the table describes every group and
sits at the front of every group with the bitmaps directly behind it, so
a table needing one more block would push every bitmap on the disk along
by one. `mkfs.c` now reserves room for it to grow into -- enough to
describe a filesystem sixty-four times the one being made -- and a
resize fills that room and refuses to go further. an 8 MiB filesystem
can become 512 MiB; the limit is arithmetic and is reported rather than
discovered.

**and a filesystem is not the filesystem.** one was mounted at a time
and `mount <n>` *moved* it, which is fine for looking at another disk
and no use for the thing this version is about. there are two now, `/`
and `/work`, and every call that touches a file says which -- the
alternative being a second copy of each with `work_` in front of it,
and two copies of anything drift. the installer lays down two
partitions, the system taking a floor or an eighth of the disk and the
work taking the rest, because what the system needs is bounded and known
and what the work needs is not.

**and the other filesystems.** XFS, Btrfs, NTFS and APFS, read-only,
one per version and each when there is a way to check it. that last
part is the whole difficulty: there is no `mkfs.xfs` on this machine
and no `newfs_apfs` anywhere near it, so a driver would be checked
against images I wrote myself -- my code checking my code, which is the
one thing this project has refused since 0.2.14. btrfs needs its chunk
tree parsed before a single byte is readable and APFS's specification
is only partly published, so neither is a version's worth of work; they
are projects. ext4 was different, and that is why it went first: it is
the one that could be grown out of something already here and checked
against something already written.

**0.3.26 the source, on the disk.** ~~the last step before 0.4.0 and
entirely unglamorous: the kernel's own source tree shipped in the image
and installed alongside it, so that a machine booted from an installed
disk has the thing it would need to rebuild itself sitting there. until
this exists the rest is a toolchain with nothing to build.~~ **done in
0.3.26**, and the unglamorous part turned out to hide one decision worth
arguing about: not whether to ship the source but *where it lives while
the machine is running*.

**the obvious place is the ramdisk, and the obvious place is wrong.**
the ramdisk is already a tar, already read, already understood -- and it
is loaded whole into memory at boot and never freed, because programs
are run out of it. putting the source there spends three and a half
megabytes of every boot, forever, on files that are read on the day the
machine is installed and then not again. the source has to be
*reachable*, not resident, and those are very different prices. so it is
written into the boot image past the ramdisk, at sectors philemon's
table names, and left there. philemon does not load it. the kernel does
not load it. it is read a sector at a time by whoever opens a file in
it, and the rest of the time it costs nothing at all.

**it appears at `/boot/src`, which is where it actually is.** `/boot` is
what the boot medium handed over; the source came on the same medium and
is a mount of its own standing in the ramdisk's directory, the way
`/work` stands in the root's. that is a third kind of thing under one
namespace -- not a disk, not memory, but a tar on raw sectors -- and the
whole of it took one branch in `vfs_open`, one in `vfs_readdir` and one
case in `vfs_read`, because 0.2.15 and 0.3.25 had already made "which
mount does this name belong to" a question with a place to be answered.
`ls /boot/src`, `cat`, `grep` and `diff` work on it with nothing added
to any of them.

**it did not make the image bigger.** the image has been padded to a
cylinder past eight megabytes since philemon existed, because firmware
that computes geometry the 1981 way reads a smaller disk as no disk at
all -- so there were already four megabytes of zeroes in there, and the
source fits in them. `velvetos.img` is the same 8,773,632 bytes it was
the version before.

**a stamp, because "there is a source tree here" is the weaker claim.**
an archive and a kernel sitting in the same image are two files that
happen to be adjacent; anybody can put two files next to each other.
what is wanted is "this is the source *this* kernel was built from", and
the only moment that can be established is the moment both halves are in
the same room, which is the build. `tools/srcstamp.py` hashes the
archive and writes the number into a C file that is linked into the
kernel, so `src verify` reads every sector off the medium, hashes it,
and holds the two against each other. the cost is that editing anything
at all -- a line of this file included -- relinks the kernel, which is
not overhead so much as the claim being true.

**the installer copies it twice, and they are deliberately different
copies.** the archive is inside the system area, so the verbatim
sector-for-sector copy carries it without being told; and then the
installed machine gets it *expanded* into `/src` as ordinary files. one
is what this kernel was built from and cannot be written; the other is
yours. `diff` between them is then a sentence about what you have
changed, which is the thing 0.4.0 will want on the morning somebody
edits the kernel from inside it.

**and it found a race that had been there since the installer.**
reading the source means selecting a drive that is usually not the
mounted one, and `disk_raw_read` did that without the lock every
filesystem operation holds -- so a thread switching to drive 0 while
another sat between selecting drive 1 and reading from it would hand the
second thread the wrong disk's sectors. that was survivable while the
only raw reads in the machine were one install command; it stopped being
survivable when `ls` could do one.

**what is checked, and by whom.** `tests/test_source.c` runs the reader
against a medium made of bytes -- half-written archives, a table
claiming more than was set aside, names split across ustar's prefix
field, and every awkward offset a read can start at. `tools/srccheck.py`
goes the other way and starts from the *image*: it finds the archive
through the boot table, reads it with python's tarfile -- somebody
else's tar reader, which is the whole point -- compares every file byte
for byte with the working tree, requires that everything git tracks is
in there, and requires the archive's sha-1 to appear inside the kernel
that shipped beside it. the boot test then does the same thing the slow
way, on a real machine, off a real drive that is not the one it is
running from.

**what is not here is a way to write one.** the machine can read both
archives and can write neither: `tar` builds them, on the development
machine, and `tar` is not on this one. so an installed velvetOS can
rebuild the kernel's *contents* and not yet the image that carries them
-- which is a smaller gap than it sounds, since `mkboot` is already C
and knows about the new region, and a ustar writer is a hundred lines.
it is named here rather than glossed, because 0.4.0's sentence is about
what a surviving disk can do and this is one of the things it cannot do
yet.

## 0.4.x: the machine builds itself

**0.4.3 a boot it can choose between.** philemon starts the kernel at one
fixed place, which is right while there is only one kernel and wrong the
moment an install can write a second. it learns to read a small file naming
which kernel to start, and to say so when the answer is missing. *ends when
two kernels are on one disk, either can be chosen, and neither has to know
the other exists.*

**0.4.5 an update that goes wrong costs one reboot.** 0.4.4 without this is
a machine that can brick itself with one bad build. a new kernel runs on
probation: it has to reach a prompt and say so, and if it does not, the
boot after it starts the kernel that was there before. *ends when a
deliberately broken kernel is installed, fails, and the machine comes back
up on the old one without anyone touching it.*

## 0.5.x: a machine for more than one person

0.4.x is about what the machine can do. this is the first series about
who it belongs to.

there have been users since 0.1.5 and a session per console since
0.2.16, and both entries were careful about what they did not mean.
`passwd` keeps its passwords in plain text and says why in a comment of
its own: the interesting part of a user is not how the password is
stored but what its uid can and cannot reach, and that part is enforced
by ring 3 and by a kernel that checks before it hands anything over.
that was true when it was written. what has changed is that there is now
something here worth reaching -- a filesystem that survives a reboot,
the source the machine was built from, and after 0.4.x the compiler that
rebuilds it.

and one comment in `fs/vfs.c` is this whole series in miniature: **"the
group bits are deliberately not consulted. there is no notion of
belonging to a group anywhere in this system, so checking them would be
reading a number nobody ever sets and calling it a permission."** every
entry below deletes a clause of that sentence.

**0.5.0 something genuinely unpredictable.** this machine has no source
of randomness at all, and four entries below cannot be written without
one: a salt, a vault key, an address that moves, and every nonce in a
handshake. a csprng in `kernel/lib/`, where `hash.c` already sits
for the same reason -- the kernel and `libc.a` both compile it, so there
is one generator and not two. seeded from `rdseed` where the cpu admits
to having it, from timing jitter where it does not, and reseeded from
the arrival times of interrupts, which is the one thing a machine on a
wire has plenty of.

the awkward part is in the middle of it: **this machine boots
identically every time**, which is what makes everything else here
testable and is exactly the property a seed must not have. so the boot
log has to say which source it got, because "seeded from the cpu" and
"seeded from nothing" look the same from the outside and are not the
same machine at all. and checking it needs a different shape than
anything so far -- statistics over the output can only catch a generator
that is obviously broken, so what gets checked is the *construction*,
against published vectors the way `hashcheck.py` does sha-1, and then
the plumbing separately by asking whether two boots differ.

**0.5.1 groups, and real permissions.** uid *and gid* on every process,
and `/etc/passwd` and `/etc/group` to back them -- which moves the file
into `/etc` on the way, since it has been sitting at the root of the
ramdisk since 0.1.5. ext4 has recorded a gid on every inode since
0.2.14 and nothing has ever set one to anything. `chmod` and `chown`
finally have a filesystem that respects them, and the vfs gains a
`capable()` check so the kernel stops asking "is this uid 0" -- which it
asks by name in the vfs, in the ramdisk driver and twice in the shell --
and starts asking "is this allowed to mount a disk".

the passwd file has been making the argument for this since 0.1.5
without meaning to: `elizabeth` is uid 0 because there was no other way
to say she is staff rather than a visitor. with a group there is one.

**0.5.2 the shadow.** `passwd` is readable by everyone, which is what
makes it useful and is why the hashes cannot live in it. a `/etc/shadow`
reachable only by the superuser, and a password hasher -- argon2id --
beside the other primitives. **0.1.5's passwords were a variable; these
are a secret.** the comment in the file says storing them properly needs
somewhere to write, which is what a real disk is for; the disk arrived in
0.2.21 and this is that comment coming due.

two things follow that the entry does not look like it contains. the
hasher is *deliberately slow and deliberately hungry* -- argon2 asks for
tens of megabytes because that is what makes it worth having -- so this
is the first program on the machine whose whole point is to allocate
more than it needs, and 0.3.13's reserve has to hold while it does.
and slow means `login` cannot stay where it is: it is a function inside
the kernel shell today, and a password check that takes half a second
must not be somewhere that can stop another console from typing. the
hasher goes in `lib/` next to `hash.c`, and login becomes a program.

**0.5.4 who else is here.** there have been four consoles since 0.2.16
and, once 0.5.1 exists, they can be four different people. `who` and
`w`; a console that belongs to whoever logged into it, so that 0.2.16's
rule -- output belongs to its writer, input belongs to the screen --
gains the clause it has not needed until now, which is that neither
belongs to anybody else.

and the smallest, most embarrassing hole in the series, which is open
today: **logout does not clear the screen.** the session is thrown away
properly -- not the directory, not the history, not the variables -- and
`login` then prints a prompt onto a console still holding 128 lines of
scrollback from the person who just left. the state was cleaned and the
*picture* of it was not.

**0.5.6 the primitives, and the first thing here that a right answer
does not prove.** sha-256, hmac, hkdf, aes with the two modes this
series needs -- xts for 0.5.7 and gcm for 0.5.10 -- and x25519. it is
split out of tls because it is the half that can be checked in
isolation and against everybody: published vectors, and python's
`hashlib` and `cryptography` over the same inputs, which is
`hashcheck.py` doing again in 0.5.x what it did for sha-1 in 0.3.17.

and then the part that is new to this project. every test written here
so far asks whether the answer is right, and for this code that is not
enough: **a comparison that returns on the first byte that differs is
correct, and tells whoever is asking how much of their guess was
right.** no test that checks answers can see that, and timing it on a
machine inside an emulator measures the emulator. so it is checked the
way `checkarch.py` checks the architecture boundary -- a rule about the
code, enforced by a tool that fails the build: in these files, no early
return out of a comparison and no branch on a secret.

**0.5.7 vaults.** a driver between the disk and the vfs -- which is
where 0.2.13's block cache already sits, and it got there by being handed
the two function pointers fat32 was already using, so the seam exists and
has been load-bearing for a while. a vault is a large file that, given a
password, mounts as a directory: aes-xts, and a key derived from the
password with 0.5.2's hasher. the header is a salt and a verifier and
nothing else. **the key never touches the disk** -- powered off, the
whole of it is noise.

this is better than full disk encryption for a machine like this one,
and the reason is worth writing down rather than assuming: it protects
`/work` without making `/boot` impossible to debug, and a machine whose
early boot cannot be inspected is a machine this project could not have
been built on.

the danger is the cache rather than the cipher. a decrypted block sitting
in the block cache is plaintext in memory, and `sync` writing it home is
plaintext on the disk -- so the boundary has to be exactly one layer and
provably so. and 0.3.23 and 0.3.24 both have a crash suite that stops the
machine at every write there is; both have to still pass with a cipher in
the path, which is the real test of whether the layer went in the right
place.

**0.5.8 an address space that moves.** the vmm can place things anywhere
and places them in the same place every time: `0x400000` for every
program, one constant for the top of every stack, one `MMAP_BASE` for
every heap. stacks, heaps and mappings move on every spawn. **it makes a
fault a security feature rather than only a bug** -- an exploit that
assumes a location crashes instead, and the kernel says so.

the program's own base is the one that cannot move yet, and the
dependency is worth naming rather than discovering: `toolchain/linker.ld`
links every program at a fixed address, so a program that can be loaded
anywhere is position-independent code, which is the compiler's problem
and therefore 0.4.x's. same for stack canaries -- every build in this
tree has `-fno-stack-protector` on it, and turning that around is
a flag rather than a version.

what is already here and should be finished with it: NX is on -- the vmm
sets `EFER.NXE` at boot, says so when the cpu has no such bit, and maps
user stacks non-executable. what is missing is the pair the kernel does
not set at all, **SMEP and SMAP**: two bits in cr4 that make the
hardware refuse to run or read user memory while in kernel mode, which
turns a whole family of kernel bugs into an immediate fault.

**0.5.9 resource limits.** a program can take every page of memory or
every descriptor there is. `setrlimit`, and defaults set by init.
0.3.13 drew this line once already and drew it in the other direction --
a reserve only the kernel may draw on, so that running out costs the
program rather than the machine. this is the same line between two
*people*: a user should not be able to fork-bomb a machine into a state
where the admin cannot log in to kill it.

and the scheduler learns about niceness, which it currently has no
notion of -- there are no priorities anywhere in it, only one shared run
queue that any idle core takes from (0.2.2), so this is a change to how
the pick is made rather than a second queue. the thing it buys is
specific: a compile in the background, which after 0.4.x is a real and
long-running thing, should not make the editor lag.

**0.5.10 tls.** 0.3.6 could fetch a file and could not talk to most of
the web, and said so: there is no tls here, and a redirect to https is
reported rather than followed. this is that limit lifted -- tls 1.3, one
cipher suite, no legacy anything, on top of 0.5.6's arithmetic. **the
most arithmetic-heavy version since the compiler.**

the handshake is not the hard half. the hard half is that a certificate
is a claim about *dates*, and this machine reads the cmos clock once at
boot and believes it (0.3.12): a clock that is wrong by a year rejects
the entire internet, and one set forward accepts a certificate that
expired. and the trust store has to come from somewhere -- shipping
somebody else's list of certificate authorities inside the image is a
decision to make out loud rather than a file to copy in, because it is
the one thing on this machine that is trusted without being checkable
here.

**0.5.11 a shell that remembers.** up-arrow history that survives a
reboot, and completion that knows about users and groups as well as
files. it stores history in `~/.history`, which means 0.5.1's home
directories have to exist and be writable -- nothing in this tree names
`/home` today. the shell stops being a command runner and starts being a
workspace.

the detail that will bite: history has been per-console since 0.2.16,
deliberately, because four shells sharing one of anything is one shell
with four windows onto it. one person logged into two consoles is now
two sessions and one file, so it appends rather than rewrites -- a shell
that writes its whole history at logout is a shell that loses an
afternoon of it whenever somebody logs out of the other console second.

**0.5.12 auditing.** a log in `/var/log/auth` recording every login,
every failed password and every use of `grant`. **security is not only
about stopping people; it is about knowing what happened when you were
not looking.** the kernel gains a `klog` that programs can write to,
which is the orderly version of the serial-port logging this project has
leaned on since 0.1.x.

the machine already gets one part of this right by accident: a failed
login says "that is not a name and a word i know" for both halves,
because saying which was wrong hands over half of it. what it does not
do is *remember* that somebody asked. two rules come with the file. a
log the logged-in can edit is not a log, so it is append-only to
everybody but the kernel. and a log with no limit is a disk that fills,
which is 0.5.9's problem arriving from a direction 0.5.9 does not cover
-- so it rotates, and the rotation is part of the version rather than a
thing to add after the first full disk.

**what this series deliberately does not reach.** no remote login: a
machine with users on a wire wants ssh, and ssh needs 0.5.6's primitives
*and* a pseudo-terminal layer that does not exist here -- there are four
consoles and nothing that can be a terminal without a screen behind it.
that is a version of its own and probably a series of its own. no acls
and no mandatory access control, because owner-group-other with a
`capable()` check underneath is the model this machine's filesystem
already stores and the next model up is a research project. no disk
quotas, which is the one genuine omission from 0.5.9 -- ext4 has a
feature bit for them and this would be the place, but a quota that is
enforced in some paths and not others is worse than none, and finding
every path is a version rather than a paragraph.

## 0.6.x: the machine runs software it did not write

and what is missing is not a list of features, it is one sentence: **a
program on this machine cannot start another program.** the ring 3
`spawn` takes a path and nothing else, argv is assembled by the shell,
the shell is a kernel thread, and the pipes in a pipeline are made in
`sched/usermode.c`. everything below follows from taking that back out
of the kernel.

**0.6.0 the process interface, from ring 3.** `exec`, a spawn that
carries argv and envp, `pipe()`, `dup2`, and close-on-exec. the hard
half of this has been done since 0.2.11: `fork` copies an address space
and gets two returns out of one syscall. what it has never had is the
other half -- a fork that cannot exec is a process that can only ever be
itself. the check is a ring 3 program that builds and runs a pipeline
the kernel never saw, and the kernel keeps its own pipeline builder,
because the rescue shell in 0.6.3 still needs one.

**0.6.1 errno.** every syscall in this machine returns -1 and says
nothing further. the kernel almost always knows more than that -- the
vfs knows the difference between a name that is not there, a name that
is a directory and a name this uid may not have -- and throws it away at
the boundary. this is small in code, reaches every error path in the
tree, and is checked the flattest way there is: `perror` here and
`perror` on linux printing the same word for the same mistake.

**0.6.2 seek.** there is no `lseek` in fifty-three syscalls. `less` and
`diff` read forward because forward is all there is, and everything that
revisits a file -- an archive reader, a database, a linker reading back
what it wrote -- cannot be written at all. `pread` and `pwrite` come
with it rather than after it, since a descriptor with a position that
two threads share is a race, and 0.2.11 made sharing descriptors the
normal case.

**0.6.5 the libc the port asked for.** the libc is velvetOS's, and one
item on its list is a kernel change wearing a libc hat: `strtod` and
`%f` mean floating point in ring 3, and this kernel is built
`-mno-80387 -mno-sse` because there is **no fpu state in the context
switch**. a program that uses an `xmm` register today is a program
whose arithmetic another thread can quietly corrupt. so the port's
most innocent-looking requirement is the one that reaches furthest in.

**0.6.8 ptys.** the layer 0.5.x closed by naming as missing: a terminal
with nothing behind it but another program. it makes job control real
for a ring 3 shell, it is what `script` and a multiplexer are, and it is
the thing standing between this machine and anything remote.

**0.6.9 `/proc`.** software written elsewhere expects to *read* the
kernel rather than ask it. `ps` and `top` stop being kernel commands
with private access and become programs reading files -- which is also
how this version finds out how much of the kernel's state was only ever
reachable from inside the kernel.

**0.6.12 entropy.** every cryptographic thing this machine will ever run
needs bytes nobody could have guessed, and there is no such thing here yet.
a pool, seeded from the cpu's own random instruction where it has one, the
lapic timer, the jitter between interrupts, and the timing of disks and
keystrokes, behind a `getrandom`-shaped call and a `/dev/urandom` that is
the same pool. it is here because ssh needs it, and because nothing that
speaks tls will run without it. *ends when the pool is seeded from at least
three independent sources, refuses to hand out bytes before it is, and
writes no state to disk.*

## 0.7.x: the machine leaves the emulator

everything above has been checked on one machine, and that machine is
not a machine. qemu is a second implementation of the hardware and a
forgiving one: it answers registers that real silicon leaves floating,
it never takes longer than expected, and it gives every machine a ps/2
keyboard whether or not anything sold this decade has one.

so this series has a problem the others did not, and it is worth saying
before the entries rather than inside them. **most of it is checkable
and the claim it makes is not.** OVMF, `-device qemu-xhci`, `-device
nvme` and a virtio disk are all right there, and every entry below is
written to be finished in an emulator first. what none of them can
settle is whether the thing boots on hardware somebody else owns, and
that is the whole point of the series. the cancelled arm port is the
precedent and the warning: a thing that cannot be tested here does not
ship, so each of these has to be testable here *and* confirmed there.

**0.7.0 uefi.** philemon reads the disk through the bios and asks vbe
for a framebuffer, which is `int 10h`, which does not exist on firmware
made after about 2012. so it gets a second front half: a PE application
the firmware loads, GOP for the framebuffer, `GetMemoryMap` where e820
was, and `ExitBootServices` before anything of ours runs. what it must
not get is a second handoff -- 0.1.12's decision was that the kernel is
handed one struct and knows nothing about anybody's protocol, and this
is the version that finds out whether that was true. the bios path
stays: a machine that only boots on 2012 firmware is exactly as narrow
as one that only boots on 1981's.

**0.7.1 a table the firmware will look at.** uefi wants gpt and a FAT32
EFI system partition, and this machine has read gpt since 0.2.15, read
and written fat32 since 0.1.10, and been able to format one since
0.3.21. so this is a small version: the installer writes a gpt with its
protective mbr, lays down an ESP beside the two partitions 0.3.25
already arranges, and puts the loader where firmware looks rather than
where philemon put it.

**0.7.2 usb.** xhci, and a **keyboard first**, because a machine you
cannot type on is one you cannot even read the panic off. ps/2 is
present on almost nothing built in the last ten years and present in
every emulator, which is the most dangerous combination a driver can
have: it works everywhere it is tested and nowhere it is used. then
mass storage, since a usb stick is how this actually reaches another
machine.

**0.7.3 nvme.** ahci was 0.1.10 and there has been exactly one disk
driver since. the seam it would slot into is already load-bearing -- the
block cache took its place in 0.2.13 by being handed the two function
pointers fat32 was using -- so this is a driver rather than a rewrite.
and a second disk driver is the only way to find out which of the first
one's habits were assumptions.

**0.7.4 a framebuffer the size of a real screen.** vbe hands over
something modest; gop on a laptop hands over 1920x1080 or worse, and at
that size every scroll is a memmove of eight megabytes and every redraw
is a repaint of two million pixels. 0.3.10 already found this shape of
problem at 1024x768 -- a redraw per keystroke is what made the editor
feel like wading -- and it arrives again with a bigger number. the cell
grid is 256x128 and copes; the 8x16 font at 4k is not a font, it is a
rumour, so a scale factor is part of the version.

**0.7.5 virtio.** net and block. it is what the machine gets when it
runs on somebody's hypervisor instead of somebody's desk, and it is the
cheapest possible second implementation of two drivers that have never
had one -- which is what will find the e1000-shaped assumptions in
`net/` and the ahci-shaped ones underneath the cache.

**0.7.6 a clock that is right rather than merely increasing.** the tsc's
frequency has to be measured rather than assumed, the lapic timer
calibrated against something that is not itself, and the rtc read with
its drift admitted. then ntp, because 0.5.10 turned certificates into
claims about dates and this machine reads the cmos once at boot and
believes it: a laptop whose coin cell died is a machine that rejects the
entire internet until somebody tells it what year it is.

**0.7.7 power.** `machine_poweroff` writes 0x2000 to port 0x604 and then
to 0xb004 -- qemu, and bochs -- and on real hardware neither does
anything at all, so the machine says goodbye and keeps running. doing it
properly is the fadt, `PM1a_CNT`, and a sleep type that lives in the
dsdt, which means parsing the first bytes of aml this project has ever
looked at. and the power button becomes an event rather than a way to
lose a filesystem, which is what makes this worth a version: 0.3.24
spent an entire version making a crash cheap, and a machine that can
only be turned off by holding the button in is a machine that crashes
every time somebody leaves the room.

**0.7.9 a machine that says what it found.** on a desk there is no
serial console: the log has nowhere to go, so a machine that fails on
somebody else's hardware fails silently. the boot log gets written to
the medium it booted from before anything else can go wrong, and there
is a report to go with it -- what the firmware claimed, which drivers
bound to what, what was refused and why. that is the difference between
"it did not work" and a bug report, and it is the bridge into 0.8.x.

**0.7.10 the boot performed rather than described.** three machines that
are not this one, three firmwares, and the log from each of them kept in
the tree. no suite can run this and that is exactly the point: it is the
one claim in this document that only somebody else's hardware can
settle. the honest form of the entry is a list of what did not work,
because the first three real machines will each say something nobody
predicted.

## 0.8.x: the machine explains itself

this machine has been able to say *what* went wrong since 0.1.x: a panic
with a symbolised backtrace, `bt`, `ps`, `top`, a memory column, a count
of syscalls by kind. what it cannot say is why, or where the time went,
or what a program was doing at the moment it stopped. 0.4.x named the
gap in its own closing paragraph -- no debugger -- and left it there.

the outside answers here are unusually good, and one of them has been
sitting in the build the whole time: **the kernel is compiled with `-g`
and always has been**, so there is dwarf in `bin/velvetos` today that
nothing in this project has ever read, and `addr2line` will say what any
address in it means. every reader below has somebody else's answer to be
held against, and the last of them hands the whole machine to somebody
else's debugger.

**0.8.0 dwarf, and a backtrace that names a line.** a backtrace today is
a symbol and an offset, out of the table `gensyms` builds from the
symbol table. the line number is already in the binary and has been
since 0.1.0. reading it is a version of its own because dwarf's line
table is not a table -- it is a bytecode for a little state machine that
you run to produce one. checked against `addr2line` on the same kernel,
address for address, which is the cheapest outside answer in this
document.

**0.8.1 core dumps.** a program that dies leaves nothing behind but a
message. an ELF core -- the registers, the mappings, the memory that
mattered -- written where the person can find it, in the format
everybody else's tools already read. that last clause is the whole
verification and it costs nothing extra: gdb on the development machine
opening a core written by velvetOS is the test.

**0.8.2 a debugger.** `int3` for breakpoints, the trap flag for
stepping, registers, memory read and write, and the symbols and lines
0.8.0 made readable. it needs the one thing this kernel has always
refused -- one process reaching into another's address space -- which is
why it is a version rather than a program, and why it wants 0.5.1's
`capable()` underneath it, so that reaching in is a permission somebody
holds rather than a thing that is possible.

**0.8.3 a gdb stub.** the remote serial protocol, over the wire or the
serial line, so that *real gdb* attaches to this machine. it is the
strongest outside answer a debugger can have: a protocol we did not
design, driven by a program we did not write, against a kernel that has
to be right about every register it claims to report. and it is what
makes debugging the kernel itself possible, which 0.8.2 deliberately
does not attempt.

**0.8.4 tracing.** 0.1.6 counted syscalls by kind; this makes them a
stream -- which call, with which arguments, from whom, and what came
back. the counting was the easy part and is already done; the decoding
is the version. a path is a string, a flags word is a set of names, a
descriptor is a file somebody opened, and a number left undecoded is a
number nobody reads twice.

**0.8.5 a profiler.** sampling on the lapic timer, which has been
per-cpu since 0.2.2, and unwinding from wherever the interrupt happened
to land -- which works only because `-fno-omit-frame-pointer` has been
in the build since the beginning for the backtrace's sake. this is the
second thing that flag pays for. the question it exists to answer is
0.4.8's: where a build spends its minutes, measured rather than guessed.

**0.8.6 the log, and a panic that outlives the machine.** `kprintf` has
kept a ring of everything printed since long before this series --
`klog`, in `kprintf.c` -- and nothing but the shell has ever been able
to read it. a `dmesg`, levels worth filtering by, and then the part that
matters on hardware that is not here: the panic is written to the medium
before the machine gives up, so the evidence survives the reboot. 0.7.9
writes the first log of a boot for the same reason; this writes the
last.

**0.8.7 numbers that are kept.** a suite that measures instead of
checking -- boot time, the cost of a syscall, a page fault, a context
switch, a megabyte through the disk and a megabyte through the wire, and
the build itself. recorded in the tree, per version, because the only
outside answer available for performance is *yesterday*, and a
regression nobody wrote down is a regression nobody finds. this project
has twice discovered a slowdown by feel (0.3.10 twice over); feeling it
is not a method.

**0.8.8 breaking it on purpose.** 0.3.23 and 0.3.24 stop the machine at
every write there is and demand the wreckage be mendable. this is the
same idea pointed everywhere else: an allocation that fails, a read that
returns short, a packet that never arrives, a disk that reports an error
it has never reported. every error path in this kernel is written and
most of them have never once run -- which is also the honest test of how
much of 0.6.1's errno was aspiration.

**what the machine still cannot say.** no reverse debugging and no
record and replay, which is the thing that would make an intermittent
bug reproducible and is a project rather than a version. no way to see
inside optimised code, which is a real limit here: gcc optimises the
userland, and there is no debugger on this machine to read the result.
