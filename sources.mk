# what the kernel is made of, written down once.
#
# the portable half is these files and directories, and the build adds
# exactly one directory under arch/ to them. it is a list rather than a
# `find .` because the root of this tree also holds the bootloader, the
# host suites and the tools, and none of those are the kernel: a build
# that globbed the root would compile tests/test_switch.c, inline asm
# and all, into a machine.
#
# read by the GNUmakefile, by tools/checkarch.py, tools/portable.py and
# tools/toolcheck.py, and by anything that builds this kernel from outside
# it. velvetOS's self-hosting build includes this file rather than keeping
# a copy of the list, because two copies of it would drift.
KERNEL_PORTABLE := main.c boot.c boot.h version.h drivers fs lib mm net sched shell
