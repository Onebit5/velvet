# changelog

every version of velvet, newest first.

until 0.4.0 the kernel was part of velvetOS: one tree, one roadmap and one
changelog, and that changelog is where the kernel's history before the split
lives. every version from 0.1.0 to 0.4.0 is there, each of them a machine
that booted, kernel and programs together. it was not divided up on the
way out. the entries are a record of what happened, written when it
happened, and a record sorted into two piles afterwards is a record
rewritten.

the versions are the ones in [ROADMAP.md](ROADMAP.md), and each of them is
a kernel that booted.

---

- **0.4.3**: a kernel and nothing more. **the kernel is nobody's client.**
  it leased its own address with dhcp at boot, looked names up with a
  resolver of its own and fetched files over http, each of them a
  program's work done in ring 0. `dhcp.c`, `dns.c` and `http.c` are gone,
  with their suites and their halves of the fuzzer, and the stack under
  them stays: ethernet, arp, ip, icmp, udp, tcp and the sockets. the
  machine boots with no address and says nothing on the wire until it is
  given one. the monitor's `ifconfig` gives it one by hand, and takes a
  router as well, since the lease was the only thing that ever set one;
  `dhcp`, `host` and `fetch` are gone, and `ping` and `tcp` take an
  address rather than a name. a program cannot yet hand the kernel what a
  client of its own found out, which is 0.4.4.

  **nor its installer.** `install` copied the medium onto another drive
  and made it boot, and putting a system on a disk is a system's work:
  `install.c` is gone. the formatter it called left the kernel image for
  `tools/mkfs.c`, where `mkext4` and the suites still format with it, and
  the tree copy `src unpack` shared with it is `source_unpack`, in
  `source.c`. the calls that read and write any drive by sector stay in
  `disk.c`, for 0.4.4 to give a program.

  **and it names no system.** deflate, inflate and adler32 left `lib/`,
  since only git ever called them, and `strcasecmp` went with dns. the
  hosted and arch macros are `VELVET_HOSTED` and `VELVET_ARCH_*`; what the
  kernel and its tools format is labelled `velvet`, or `VELVET` on fat32,
  unless the caller says otherwise, and `mkfat` takes a label now as
  `mkext4` did; the monitor's persona is `monitor@velvet`, and a reboot
  grants the ability to create thine own system. the documentation, the
  roadmap and the templates describe what is built on this kernel without
  naming anything built on it: what belongs here, a selftest with whatever
  a program can see, and a release named by its version and never moved.

- **0.4.2**: the kernel steps back. **a medium with no `/init` is a
  panic**, saying why, rather than a set of logins the kernel made up:
  which consoles get a login and what a session is are the system's to
  decide, and every orphan names `/init` as its parent. the kernel's
  login, its accounts, its greeting and profile, `logout`, `run`, `jobs`,
  `fg`, `bg`, pipelines and the path search are gone, and so is what ran
  jobs for them, `user_run`, `user_pipeline` and the `user_job` calls.

  **the kernel's shell is a monitor**, on a fifth console of its own:
  alt+5, or `ctrl+\` then 5 on serial. nobody logs into it, it has root's
  powers under the name `monitor`, and it runs no programs, since that is
  the system's work. it is for looking at the kernel when the system above
  it has gone wrong: `ps`, `mem`, `dmesg`, `bt`, `vmm`, `hexdump`, and
  `reboot` when nothing else will. with a program in front of a console,
  ctrl+c and ctrl+z are SIGINT and SIGTSTP to its group, wherever it is.

  **this kernel's image carries an `/init` of its own** and nothing else
  but the selftests: no account and no greeting. it asks on the first
  console which selftests to run, runs each in a group of its own, in
  front of the console and as guest, kills one that outlives its timeout
  and goes on, and powers the machine off after the last. `none` leaves
  the machine up for the monitor. `make kselftest` answers it, and a test
  that times out no longer ends the run; the boot test answers `none` and
  types its commands at the monitor. the selftests run as guest still,
  since each of them checks a promise made to anybody; a selftest of what
  only root may do is not here yet.

  and what moving them turned up: **completing a word in the middle of a
  line** overwrote the rest of it whenever the word grew by less than what
  followed; **a child made after its parent had ended** kept naming that
  parent, and nobody would have collected it, so it is init's from birth.
  `sched/threads` had looked for its own name as the kernel's `run` gave
  it, and `fs/lseek` had read a motd that was only there by accident.

- **0.4.1**: a first program. everything velvetOS's boot test types
  at the kernel's shell is now something a program can ask for, and a
  medium can carry the program that asks: **when there is an `/init`, it
  is pid 1.** the kernel's thread beside it, `services`, keeps the
  flusher, the network and the road down, starts no consoles and collects
  no orphans, which are `/init`'s, and if `/init` ever ends the kernel
  panics rather than run on with nobody to start a session. without one,
  nothing changes, and this kernel's own image is booted that way.

  **`/proc`**, files the kernel writes when they are read. a show function
  prints with kprintf, the way the shell's commands always printed, and a
  capture held by the reading thread turns that text into the bytes of
  the read, so the shell and a program are told the same words by the
  same function. `version` (with the compiler and the build time),
  `uptime`, `meminfo`, `slabinfo`, `threads`, `pci`, `cpuinfo`, `disk`,
  `partitions` and `mounts` are anyone's, and so is `environ`, the
  reader's own environment; `vmm` and `stack` say where the kernel keeps
  things and are root's.

  **fifteen calls, 53 to 67**: `syslog` for the kernel's log, `sync`,
  `fsck` and `resize` naming a filesystem by where it is mounted and
  answering in structs a program puts into words, `source` for the tree
  on the medium and whether it hashes to the build's stamp, `execve`,
  `pipe` and `dup2`, `setpgid`, `ttyattach`, `tcsetpgrp` and `tcgetpgrp`
  for a program that owns a console and runs its jobs, `setuid` for a
  login to become the person it let in, `reboot`, and `lseek`, without
  which a shell could not put `>>` on the end of a file. `wait` takes
  `WNOHANG` and `WUNTRACED` and -1 for any child, and `sigsend` takes
  minus a pgid for a whole group.

  and what asking from ring 3 turned up, each fixed in a commit of its
  own: **a guest could signal anybody's programs, init among them**, and
  now reaches only its own. **no pipe end was ever let go of when its
  holder exited**, so a reader waited forever on a writer that was gone.
  **SIGKILL could not end a program busy in a loop**, since signals were
  only acted on as a call returned; one that ends the process is now
  acted on as an interrupt returns to ring 3. **stop and continue waited
  for a return that a stopped thread never makes**, and now act when
  sent, and the process remembers it stopped, for `wait` to say. **a
  resize longer than its partition** was refused only by the shell, and
  is refused by `disk_resize` now that something else can call it.
  **a forked child lost its parent's environment and signal
  dispositions**, so a shell's child had no PATH; it is a copy now.
  **a read on a console nobody was looking at failed at once** rather
  than waiting, and **a wait for a key could not be interrupted**, and
  dropped the key that finally woke it. a new thread and a reused
  process slot both started out holding whatever their last occupant
  left in any field nobody set; both are cleared first. and the date
  arithmetic is apart from the clock, in `lib/clock.c`, so a program can
  take the one without answering for the other.

  and **`make kselftest`** is new: programs in `tools/testing/selftests/`,
  built against the uapi headers alone, that check from ring 3 what the
  kernel promises ring 3. they run on the booted image as guest, with a
  fresh copy of `disk.img` as a second drive, and print TAP. there are 26,
  and a change that makes one fail is the wrong change.

- **0.4.0**: the kernel, as a project of its own. velvetOS 0.4.0's kernel,
  moved into a repository of its own, with nothing about what it does
  changed except what it calls itself: the boot banner, `top` and `arcana`
  say **velvet 0.4.0**, and the rank `persona` prints is the kernel's,
  because the version is the kernel's number now and the system carries its
  own. it is written once, at the top of the `GNUmakefile`, and the
  build turns it into `<generated/version.h>`;
  `make kernelrelease` prints it. velvetOS builds its machine on a release
  of this repository: it names the release by its version in its
  `kernel-release.mk`, and its build clones that release's tag into its
  `obj/velvet/`. **the kernel's directories
  are the root of the tree**, the way other kernels are laid out, so every
  source file keeps the path it had under velvetOS's `kernel/`: `lib/hash.c`
  and the rest are where they were. philemon came with it, because the
  struct the kernel is handed is philemon's and six files include it; so
  did the host suites that compile kernel sources, and every tool that
  builds the kernel, writes its images or checks what it reads. the programs, the libc, the assembler, linker,
  make and `ar`, git, and the checkers for those stayed in velvetOS with
  them.

  two things had to become explicit that being one tree had let stay
  implicit. **what the kernel is made of**: a `find kernel` was the
  definition while the kernel had a directory to itself, and at the root of
  a tree that also holds `boot/`, `tests/` and `tools/` a glob would have
  compiled a suite's inline asm into a machine. so `sources.mk` lists the
  portable half once, and the makefile, `checkarch`, `portable-check`,
  `toolcheck` and velvetOS's self-hosting build all read it, the last of
  those because the comparison it ends in is byte for byte, and the order
  objects are linked in decides where every function lands. and **who the
  build is for**: `BINDIR`, `OBJROOT` and `SOURCE` let another tree build
  the kernel into its own `bin/`, stamped with the hash of the source that
  tree ships rather than this one's, and the link writes down what it linked,
  in order, so velvetOS's `kernelcheck` can hand exactly those objects to its
  own linker.

  **where a header lives says who may read it.** the ones more than one
  subsystem uses are in `include/velvet/`, included as `<velvet/vfs.h>`,
  and a header only its own subsystem reads stays beside it. each
  architecture's are in `arch/<arch>/include/asm/`, and the build puts
  exactly one of those on the include path, so `<asm/cpu.h>` is x86_64, or
  the `none` that `portable-check` compiles against. the five headers that
  picked an implementation with `#if defined(VELVETOS_ARCH_...)` are gone,
  and the list of architectures each of them carried with them. `checkarch`
  reads the contract from the tree: an `<asm/...>` header `arch/none`
  provides is portable to include, and one only x86 has is the machine by
  another name. what ring 3 sees is in `include/uapi/velvet/`: the syscall
  numbers, the two structs the calls hand back, the signal numbers, the
  keys and the terminal flags, of which velvetOS's libc had kept a copy of
  its own, under an XXX saying nothing held the two lists together. it
  includes these now, and checks its `struct stat` against the kernel's at
  compile time. there is no `-I.`, so a header left where it used to be
  fails to build rather than working.

  **there is no source at the root**: `main.c` and `boot.c` are in
  `init/`, and the linker script is the architecture's, in
  `arch/x86_64/`. and
  **the console font is linux's**: the vga 8x16 from linux's
  `lib/fonts/font_8x16.c`, GPL-2.0 like everything else here, in place of
  spleen and the second license it brought with it. its upper half is code
  page 437, the vga's own order, rather than latin-1, and a control byte
  the console does not act on shows the vga's symbol for it now rather
  than nothing.

  the roadmap is the kernel's own: its numbers run on from 0.4.1 without
  gaps, the entries that were already done are in velvetOS's changelog,
  where they happened, and it plans the kernel's work and nothing else.
  accounts, passwords, logins, shells and installers went to velvetOS's
  roadmap, and an entry here names the velvetOS entry it is the mechanism
  for. 0.6.x is where velvet learns to answer the way linux does, so that
  a program built for linux runs here unmodified.

  the medium this tree boots carries **no programs**. it is the accounts,
  the profile, and the files the boot test reads back, so `make boottest`
  checks what the kernel shows with nothing in ring 3: the boot, both cores,
  the login and a guest, every builtin it types, the journal, a resize, fsck
  twice, and the source verified and unpacked. ring 3 is checked by
  velvetOS's boot test, on every kernel it pins. the shell suite resolves
  and completes names against a `/bin`, so it gets one of its own: the
  medium with empty files named after what velvetOS ships, since resolving
  a name opens a file and nothing reads it.

  and moving it found four things that had been quietly wrong. **toolcheck
  had been checking less than it said**: the trampoline, which its own
  comment calls the only input that matters, and the `checkfmt` comparison
  over the kernel both looked in `kernel/src/`, a directory that has not
  existed for a long time, and both were skipped by an `exists()` that never
  said so. they run now, and pass. the boot test's `history` check looked
  for a command it had just typed, which the serial line echoes back whether
  or not `history` prints anything; it looks for the numbered entry now.
  and `make run` named the disk as a prerequisite before the line that
  defined it, so on a fresh checkout it asked for nothing and handed qemu a
  drive that did not exist, which is the exact trap the comment on
  `BOOTIMG` two lines above it describes. and the boot test's cache check,
  carried over from velvetOS, looked for a cache that only exists once a
  program has run, and passed anyway on a file name in the source listing;
  it looks for the thread cache's row in the slabs table now.
