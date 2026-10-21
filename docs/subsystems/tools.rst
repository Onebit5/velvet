the tools
=========

``tools/`` is the python that checks the project and builds the images it
runs. it is deliberately ordinary python 3 with no third-party imports: a
tool here has to run on a machine that has nothing but an interpreter, which
is the same constraint the machine itself is built under. see `python
<../coding-style.rst>`_ for the style.

there are two jobs in here, and the split matters: tools that **build**
something the machine boots from or reads, and tools that **check** the
project against somebody else's implementation of the same thing.

the second opinion
------------------

most of the checkers exist for one reason, stated in `contributing
<../contributing.rst>`_ as a rule: a check written by the code it checks
proves nothing. where another implementation of a format exists, the project
tests against that instead.

what is here checks the kernel and the formats the kernel reads.

``hashcheck.py``
   crc32 against ``zlib``, whole and in pieces.

``readext4.py``
   read an ext4 or ext2 image and complain about it. written from the
   on-disk layout, sharing no line with the driver or the formatter.

``readfat.py``
   the same, for fat32.

``fsckcheck.py``
   break a filesystem on purpose, one deliberate break per check, and demand
   the project's fsck finds each. the repaired image is then handed to
   ``readext4.py``, which shares no line with the checker, because an fsck
   saying it fixed something is an fsck's opinion of itself.

``toolcheck.py``
   the C build tools against the python ones they replace: byte for byte
   where there is one right answer, and through ``readext4.py`` and
   ``readfat.py`` for the two formatters, since two formatters that
   allocate in a different order write two good filesystems.

the static checks
-----------------

``checkarch.py``
   does the arch boundary hold? grep for inline assembly and x86 headers
   outside ``arch/``. a line drawn and not checked decays the first time
   somebody needs a ``hlt`` in a hurry. silently, because the kernel goes
   on building and booting perfectly either way.

``portable.py``
   the half ``checkarch.py`` cannot see: it greps, and cannot catch a
   portable file that quietly *depends* on something only x86 supplies. this
   compiles the portable kernel against ``arch/none/``. ``--list`` prints the
   porting checklist. every name a port would have to define.

``checkfmt.py``
   every ``kprintf``/``panic`` format string against what ``lib/kprintf.c``
   actually implements. this exists because of a bug that cost an afternoon:
   gcc checks a format string against *real* printf, which accepts far more
   than this formatter does, so a ``%-7s`` slipped through, printed itself
   literally, and read every argument after it from the wrong slot.

the image builders
------------------

``mkboot.py``
   build the bootable disk image for philemon: the bootloader, the table at
   sector 32 naming where everything is, the kernel and the ramdisk. there is
   no filesystem in the first 512 bytes, because there is barely room for
   anything.

``mkfat.py``
   build a fat32 image and fill it from a directory. there is no
   ``mkfs.vfat`` on this machine, so this writes the filesystem by hand.

``mkext4.py``
   the same for ext2/ext4. ``mkext4.c`` formats with ``mkfs.c`` instead,
   and the two are checked against each other.

``mkdisk.py``
   wrap filesystem images in a partition table, because a disk is not a
   filesystem.

``gensyms.py``
   turn the kernel's function symbols into a C table, so a panic can say
   ``kmain+0x42`` rather than making you run ``addr2line`` by hand. reads the
   ELF's symbol table itself rather than asking ``nm``, because a
   self-hosting machine has no more binutils on it than it has python.

``bin2c.py``
   turn a flat binary into a C array. used for the code a second cpu wakes
   up in.

``font2c.py``
   bdf to C array, for the console font. run once; the output is committed,
   since regenerating it needs the original bdf.

the C tools
-----------

six of the python builders have a C version beside them, because a
self-hosting machine has no python: ``bin2c.c``, ``checkfmt.c``,
``gensyms.c``, ``mkboot.c``, ``mkext4.c`` and ``mkfat.c``. they are built
by ``make tools`` into ``bin/tests/``, and both halves stay until the python
can go, because ``toolcheck.py`` needs something to hold each C tool
against.

``mkext4.c`` and ``mkfat.c`` are drivers rather than rewrites: they point
``mkfs.c``, ``fs/ext4.c`` and ``fs/fat32.c`` at a host file and a host
directory, so the image a disk is built from is written by the same code the
machine runs. ``fsckext4.c`` is the same shape for ``fsck.c``, the checker.

``mkfs.c`` is mke2fs in about the smallest form that produces something
``fs/ext4.c`` will mount and ``readext4.py``, written from the layout rather
than from either of them, agrees is a filesystem. it makes an *empty* one: a
root directory and nothing in it, no lost+found, no reserved blocks, no
sparse superblocks, no directory index. the files go in afterwards through
the ordinary ext4 write path, because a formatter that also laid out files
would be a second implementation of the half that already works. it writes
through a function pointer, the same shape ``drivers/part.c`` uses, so it is
tested against a buffer as easily as pointed at a file. it is here and not
in ``fs/`` because the kernel never makes a filesystem: a disk arrives
formatted, by this or by a program.

``fsck.c`` is an ext2/ext4 checker, and it was the kernel's until 0.4.4,
when checking a filesystem went the way of making one: it is a program's
work, done over the drive. it stays here because it is how the suites
judge the driver. ``test_crash`` pulls the plug at every write and
``test_resize`` grows and shrinks, and both hand what is left to it.

it **reads the layout by hand rather than through the driver**, for exactly
the reason ``readext4.py`` imports nothing: a checker that shares the
driver's idea of what the disk says is unable to notice the driver being
wrong.

it mends only what has exactly one *computable* answer. free counts, link
counts, a block marked used that nothing claims. a block two files both claim
is reported and **left alone**, because one of them is wrong and nothing on
the disk says which. an inode in use that no name points at is left alone
too: the answer is lost+found and there is not one, so deleting it would be
choosing to lose it. it reaches the disk through two function pointers, and
is handed its workspace, so it does not know or care whose memory and whose
drive it is working on.

``fuzz_parsers.c`` is the one that is not a tool for building anything: it
feeds random and mutated input to the parsers that read from the wire or
from a disk. see `testing <../testing.rst>`_.

the boot test
-------------

``boottest.sh``
   build the image, boot it headless in qemu, and **type commands at the
   shell over the serial port**, checking the answers. the medium carries
   no programs, so what it checks is what the kernel can show by itself;
   ring 3 is checked by the selftests, and a system's programs by that
   system. it is a shell script
   rather than python because it is mostly qemu's own flags. the serial log
   is left in ``bin/*-boottest.log``, and CI uploads it with the boot image
   when a run fails. see `testing <../testing.rst>`_.
