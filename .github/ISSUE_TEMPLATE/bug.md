---
name: bug report
about: something the machine gets wrong
title: ''
labels: bug
assignees: ''
---

<!-- one bug per issue, please. if you have found two, open two.

     this is the kernel's tracker. a bug in a program, the libc, the
     toolchain or git belongs to velvetOS; if you cannot tell which side it is
     on, open it here and say so. -->

## what happens

<!-- what the machine did, and what it should have done instead. -->

## how to reproduce

<!-- the smallest sequence that shows it: exact commands, in order. -->

```sh
make
make run
# and then, at the shell:
```

## where it lives

<!-- the subsystem, as best you can tell: arch, boot, mm, sched, fs, drivers,
     net, lib, shell, tools. a guess is fine and saves a round trip. -->

## the machine

- version: the banner prints `velvet v...`, and `make kernelrelease`
  prints the one a checkout would build
- commit: `git rev-parse --short HEAD`, or, if you saw it in velvetOS, the
  commit velvetOS has pinned (`git submodule status` there)
- host: `uname -a`, and the versions of gcc and nasm
- seen on this repository's own image (`make run` here), or on velvetOS?
- qemu settings, if they are not the defaults. `make run` uses `CPUS=4 MEM=2G`

## the output

<!-- the serial log, or what is on the screen. bin/*-boottest.log if the boot
     test produced it. if it panicked, the whole panic: the backtrace is the
     useful part, and the registers above it. -->

## does the suite see it?

- [ ] `make test` fails
- [ ] `make boottest` fails
- [ ] it only happens by hand
