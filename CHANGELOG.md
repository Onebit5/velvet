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

- **0.4.0**: the kernel, as a project of its own. velvetOS 0.4.0's kernel,
  moved into a repository of its own, with nothing about what it does
  changed except what it calls itself: the boot banner, `top` and `arcana`
  say **velvet 0.4.0**, and the rank `persona` prints is the kernel's,
  because the version is the kernel's number now and the system carries its
  own. it is written once, at the top of the `GNUmakefile`, and the
  build turns it into `<generated/version.h>`;
  `make kernelrelease` prints it. velvetOS keeps this
  repository as its `kernel/` submodule, pinned to a commit, and builds its
  machine on top of whatever that commit is. **the kernel's directories
  are the root of the tree**, the way other kernels are laid out, so every
  source file velvetOS named, `kernel/lib/hash.c` and the rest, is still
  where it was. philemon came with it, because the struct the kernel is handed is
  philemon's and six files include it; so did the host suites that compile
  kernel sources, and every tool that builds the kernel, writes its images
  or checks what it reads. the programs, the libc, the assembler, linker,
  make and `ar`, git, and the checkers for those stayed in velvetOS with
  them.

  two things had to become explicit that being one tree had let stay
  implicit. **what the kernel is made of**: a `find kernel` was the
  definition while the kernel had a directory to itself, and at the root of
  a tree that also holds `boot/`, `tests/` and `tools/` a glob would have
  compiled a suite's inline asm into a machine. so `sources.mk` lists the
  portable half once, and the makefile, `checkarch`, `portable-check`,
  `toolcheck` and velvetOS's self-hosting build all read it -- the last of
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
  `arch/x86_64/`.

  the medium this tree boots carries **no programs**. it is the accounts,
  the profile, and the files the boot test reads back, so `make boottest`
  checks what the kernel shows with nothing in ring 3: the boot, both cores,
  the login and a guest, every builtin it types, the journal, a resize, fsck
  twice, and the source verified and unpacked. ring 3 is checked by
  velvetOS's boot test, on every kernel it pins. the shell suite resolves
  and completes names against a `/bin`, so it gets one of its own: the
  medium with empty files named after what velvetOS ships, since resolving
  a name opens a file and nothing reads it.

  and moving it found three things that had been quietly wrong. **toolcheck
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
  `BOOTIMG` two lines above it describes.
