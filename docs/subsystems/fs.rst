filesystems
===========

``fs/`` is one namespace, several filesystems under it, and the
layers that let a program not care which one answered.

the namespace
-------------

``vfs.c`` is the router. until 0.2.15 the ramdisk was the whole world and
the disk was bolted on at ``/disk``, which had it backwards. now:

::

    /          the system disk, when there is one
    /work      a second filesystem, when the drive holds one
    /boot      the ramdisk, always
    /boot/src  the source this was built from, out on the medium
    /proc      what the kernel says about itself, written as it is read

the ramdisk keeps its own mount point rather than being deleted, because it
is what makes the machine work when the disk does not. every program, and
the ``passwd`` file, live on it. a kernel whose only filesystem needs a sata
controller to bring up is one that a missing cable turns into a brick.

a name with no leading slash is looked for on the disk first and the ramdisk
second, which is what lets a disk supply a newer ``bin/ls`` while a machine
with no disk carries on with the one it booted with.

a ``struct vfs_file`` remembers **which filesystem it came from**. a
descriptor that remembered a file but not which of two disks it was on would
read the right offset out of the wrong disk, which is the sort of bug that
looks like a corrupt file. a rename from one mount to another is *refused*
rather than quietly turned into a copy and a delete wearing a rename's name,
because a rename that copies is not a rename.

every call that touches a file says which of the two disks it means
(``DISK_ROOT`` or ``DISK_WORK``). the alternative was a second copy of each
with ``work_`` in front of it, and two copies of anything drift. both are on
the same drive, and that is the block cache's doing rather than a
simplification: the cache holds absolute sector numbers and is bound to
whichever drive is selected.

/proc
-----

``proc.c`` holds files nothing stores. a file there is a name and a
function that writes the file's text with ``kprintf``, the same way the
kernel writes anything for a person, and a capture (``struct kcapture`` in
``<velvet/kprintf.h>``) turns that text into the bytes of a read. so a
subsystem that can describe itself on a screen describes itself to a
program in the same words, by registering one name:

.. code-block:: c

   proc_register("version", PROC_PUBLIC, show_version);

nothing is kept between reads. every read writes the file again, which is
why a file here says it is 0 bytes long and is read to its end like any
other, and why two reads a second apart may disagree: they are two answers
to the same question. a descriptor on one remembers only which file and how
far along, so a reader taking it in pieces sees one file as long as nothing
in it changed in between, and a fork or a close has nothing to free. a
file is registered as ``PROC_PUBLIC``, for anyone to read, or
``PROC_PRIVATE``, for root alone, which is what a file that says where the
kernel keeps things is: an address of the kernel's is no business of a
guest's.

``/proc`` has ``version``, which kernel this is, and ``uptime``, seconds
since boot to the hundredth. the rest are each subsystem's own.

the filesystem's own are written by ``diskinfo.c``: ``disk``, the drive
with every filesystem on it and what the block cache is holding;
``partitions``, what each drive's table says it has; and ``mounts``, which
filesystem is where. ``disk``, ``parts`` and ``mount`` in the shell call
the same three functions, so the screen and the file cannot drift apart.

the ramdisk
-----------

``ramdisk.c`` is a read-only ustar tar that philemon loaded at boot. a 512
byte header of ascii fields, the file's bytes rounded up to 512, repeat, and
two blocks of zeroes to finish. there is no writing, no directories to speak
of, and no allocation. ``ramdisk_open()`` hands back a pointer straight
into the archive.

it is a filesystem in the sense that a filing cabinet is furniture. (see
``base/ramdisk/README``, which says so with fewer words.)

fat32
-----

``fat32.c`` is the filesystem on a disk that survives a reboot. a boot
sector describes the layout, then a table with one entry per cluster where
entry N holds the number of the cluster that follows N. a linked list with
all the pointers gathered in one place, which is why it is called a file
allocation table. directories are not a special kind of object: a directory
is a file whose contents are 32-byte records.

a file in fat *is* its directory entry, which is why a name is unique and
there is nowhere to record an owner or a permission. ``tools/mkfat.py``
builds the images the suite runs against.

the driver never touches hardware. it is handed two functions that move
sectors, exactly like the pci scan is handed a way to read config space,
which is what lets the tests run it against a real image on a machine with
no disk. the block cache slots into those same two functions.

ext4
----

``ext4.c`` is fat's counterpart with opinions, and the better answer about
where a file is.

the reason it exists: fat records no ownership and no permissions, so
everything on the disk was 0644 owned by root by decree, and ``chmod`` had
nowhere to write an answer down. while there is a filesystem that *has*
them, there may as well be symlinks and timestamps that are not rounded to
two seconds. that was 0.2.14 and it was ext2.

**it is the same driver grown up, not a second one beside it.** ext4 *is*
ext2 with features bolted on, and an ext4 disk holds files of both kinds at
once. a file made before the feature was turned on keeps the map it was
born with. so which map a file uses is a bit in its **inode**, not a
property of the filesystem. two drivers would have been the same code twice
with the bug fixed in one of them, which this project has already done once
with two libcs and once with two ext2 layouts.

the shape, which almost every unix filesystem since has been a variation on:

::

    a superblock says how big everything is.
    the disk is cut into block groups, each with a bitmap of its blocks,
    a bitmap of its inodes, and a table of the inodes themselves.
    an inode is a file. its mode, its owner, its size, its times, and
    fifteen block numbers. a name is not in there at all.
    a directory is a file whose contents are (inode, name) pairs.

that last pair of facts is the whole difference from fat, and the reason
unix has hard links, why permissions belong to the file rather than to the
name, and why renaming across directories moves nothing.

extents
~~~~~~~

twelve of the fifteen block numbers are the first twelve blocks; the rest
point at blocks of block numbers, one and two levels deep. that is the ext2
answer, and ext2 says *where is block N of this file* with a list of every
block. an **extent** says the other thing. *the next N blocks are at P*,
so a file written front to back is twelve bytes of map whatever its size.
that is the difference ext4 is actually about, and everything else it added
is trimming.

the tree grows a level exactly once per file, and that is the only
interesting moment in the feature: the inode's sixty bytes hold a header and
four records, so a file in four pieces or fewer costs nothing to describe,
and the fifth piece is the one that has to move those four into a leaf block
and leave an index behind.

read, deliberately not written
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

everything with a checksum in it is read and not written. a modern
``mke2fs`` turns on ``metadata_csum``, and then every write owes a crc32c
this driver does not compute; an image whose checksums disagree with its
contents is one fsck calls corrupt. so a disk claiming that feature
**mounts read-only** rather than being refused or, worse, written to. the
formatter claims neither feature, which is the same decision seen from the
other side.

the journal
-----------

``jbd2.c`` is the journal, and it is jbd2. the format ext4 already
names, so this is a log a linux could read rather than one of its own
invention.

0.3.23 ordered the writes so that a crash could only leave a *leak*.
something allocated that nothing points at, and built an fsck that gives
leaks back. that works and costs a walk of the whole filesystem after every
unclean stop. a journal replaces the deduction with a record: before a
change touches the filesystem it is written down somewhere else, in full,
with a mark at the end saying all of it arrived.

the journal is a file like any other. inode 8, and its blocks are a
ring. a transaction is a descriptor block saying where the blocks after it
belong, the blocks themselves, and a commit block saying the ones before it
are all there. **the commit block is the whole of the guarantee**: a
transaction without one never happened, however much of it reached the
disk.

every field is big endian, on a machine that is not. that is not the
format's age. jbd2 is younger than that; it is that a journal is the one
structure that may have to be read by a machine other than the one that
wrote it.

the whole of the correctness is **three waits**:

.. code-block:: text

    write the descriptor and the blocks, then wait
    write the commit block, then wait
    write the blocks to where they actually live, then wait
    only then say the log no longer holds them

the waits are why ``struct ext4`` grew a ``sync``. a write-back cache is
free to put blocks on the disk in any order it likes, which is exactly the
freedom a journal cannot allow. the first two waits protect the log from the
disk; the third is the one easy to leave out and it protects the disk *from
the log*. moving the tail past blocks a cache then loses is a change
nothing will ever replay again, sitting in a log that has been told it is
empty.

a journalled disk mounts read-only until ``ext4_start_journal()`` is called,
because the promise about ordering is not the filesystem's to make. ``init``
closes the journal on the way down, and ``mkext4`` closes the one in every
image it builds, so what ships is a filesystem that was unmounted rather
than one that merely stopped being written to. ``needs_recovery`` is set the
moment the log goes live and cleared only by a clean unmount.

what is not here is **revoke**: none are written (a design; this
checkpoints inside the commit and waits, so no transaction outlives the
operation that made it), and none are *understood* (an omission. every
mke2fs sets the feature, so a log written by linux is refused and its disk
stays read-only).

the block cache
---------------

``bcache.c`` sits between the filesystem and the drive. every read used to
go to the drive one sector at a time through a single bounce buffer, and the
drive was being asked the same question hundreds of times: walking a cluster
chain reads the same handful of table sectors over and over, and reading a
directory reads the same entry sector once per name in it.

it slots into exactly where fat32's two function pointers already are, which
is the whole reason it can exist without the filesystem knowing.

writes are **write-back, not write-through**: a written block is marked
dirty and stays in memory, and the drive finds out later, on eviction, when
the flusher comes round, or when somebody says ``sync``. that is faster and
it is a promise broken on purpose: until one of those happens, what is on
the disk is not what the machine believes. power going out at the wrong
moment loses work, which is why unix has had ``sync`` since 1971.

a program says it with the ``sync`` call, which asks the drive only when
something is waiting: 0 once the disk holds what the kernel does, and -1
when the drive refused part of it.

partitions
----------

``part.c`` and ``drivers/part.c`` handle mbr and gpt. a disk is not a
filesystem, so "which disk" was never the right question: every drive is
scanned at boot and what is found is kept, including drives with no table
at all, which get one entry covering the whole of themselves, because an
image written straight to sector zero is a perfectly ordinary thing and
should not need a special case anywhere above.

the checksum is the whole difference between the two schemes in one respect,
and the reason a table can be looked at and trusted or thrown away.

paths
-----

``path.c`` is working out what a name means. a process has a working
directory; a name is read relative to it unless it starts with a slash; the
whole thing is flattened into one absolute path before anybody goes looking
on a disk. ``.`` means here, ``..`` means back one, repeated slashes mean
nothing, and **``..`` from the root stays at the root**. the one rule that
has to be got right, because a path that can climb above ``/`` can name
anything at all. none of this touches a filesystem, which is what makes it
entirely testable.

pipes and redirection
---------------------

``pipe.c`` is a buffer with a process at each end. the two rules hanging off
the reference counts are the whole of why pipelines work:

- a read with nothing in the buffer blocks, **unless every writer has
  gone**, in which case it returns 0. that zero is end of file, and it is
  the only reason ``head`` ever stops.
- a write with no reader left is pointless by definition; nobody will ever
  collect it, so it fails rather than filling a buffer that will never
  drain. unix raises SIGPIPE; this kernel ends the process, which is the
  same outcome by a shorter road, and is what makes ``cat huge | head``
  stop.

the buffer is one page (``PIPE_BUF``), and that number is also the promise:
a write of ``PIPE_BUF`` or less either fits or waits until it fits, so it
never arrives interleaved with somebody else's. bigger writes are split, and
always were, on every system that has ever had pipes.

redirection is the process table's business rather than the pipe's: 0, 1 and
2 are real descriptor slots, and putting a pipe or a file where the console
would have been before the program starts is the whole mechanism. see
`file descriptors <sched.rst>`_.

making a filesystem
-------------------

``mkfs.c`` is mke2fs, in the kernel, in about the smallest form that
produces something ``ext4.c`` will mount and ``tools/readext4.py``. written
from the layout rather than from either of them. agrees is a filesystem.

it makes an *empty* filesystem: a root directory and nothing in it. no
lost+found, no reserved blocks, no sparse superblocks, no directory index.
that is the split that keeps it small: copying a system onto the new
filesystem is done afterwards through the ordinary ext4 write path, because
a formatter that also knew how to lay out files would be a second
implementation of the half that already works, and the two would drift.

the descriptor table is reserved up front (``s_reserved_gdt_blocks``) so
that growing the filesystem never has to move every bitmap and inode table
on the disk. see `resize` in the testing page.

checking a filesystem
---------------------

``fsck.c`` is an ext2/ext4 checker in C, on the machine. ``tools/readext4.py``
has been the second opinion since 0.2.14, but it runs on the machine this was
*developed* on, and a disk that can be damaged and cannot be examined is a
disk you throw away.

it **reads the layout by hand rather than through the driver**, for exactly
the reason ``readext4.py`` imports nothing: a checker that shares the
driver's idea of what the disk says is unable to notice the driver being
wrong.

it mends only what has exactly one *computable* answer. free counts, link
counts, a block marked used that nothing claims. a block two files both claim
is reported and **left alone**, because one of them is wrong and nothing on
the disk says which. an inode in use that no name points at is left alone
too: the answer is lost+found and there is not one, so deleting it would be
choosing to lose it.

the source on the medium
------------------------

``source.c`` reads the project's own source tree back out of the boot image,
at sectors philemon's table names. it is a third kind of thing under one
namespace: neither a disk (ext4) nor memory (the ramdisk), but a tar on raw
sectors.

the design point is that it is **reachable, not resident**. the obvious way
to ship a source tree is to put it in the ramdisk, which is already a tar and
already read, but the ramdisk is loaded whole into memory at boot and never
freed, so that spends three and a half megabytes of *every* boot on files
read once at install time. three megabytes of nothing in ram for the life of
the machine is what the other arrangement costs.

``/boot/src`` is where it actually is. the installer expands it to ``/src``
on the disk it writes, and those two are deliberately different places: one
is the tree this kernel was built from and cannot be edited, the other is
yours, and ``diff`` between them is a sentence about what you have changed.

there is a **stamp**: ``tools/srcstamp.py`` hashes the archive and writes the
number into a C file linked into the kernel, and ``src verify`` reads every
sector back off the medium and holds the two against each other. an archive
and a kernel sitting in one image are two files that happen to be adjacent;
"this is the source *this* kernel was built from" can only be established at
the one moment both halves are in the same room, which is the build.

the installer
-------------

``install.c`` copies the machine onto a disk it can then boot from on its
own. the boot medium is not a filesystem; it is philemon in the first
sectors, a table at sector 32, and then the kernel and ramdisk as raw runs
of sectors, so installing is: copy that system area verbatim, put a
partition table after it, and format the partition. the copy is verbatim on
purpose; reading the table and rebuilding the image would be a second
implementation of ``tools/mkboot.py``.

the one thing that makes it possible: an mbr partition table lives at offset
446 of the first sector, which is the same sector the bios loads and jumps
to, and philemon's first stage ends at byte 365. that is not luck, it is the
constraint every bootloader that has ever shared a disk with a partition
table works under.

elf
---

``elf.c`` loads a program, with the ramdisk and the disk as its image
sources. the programs themselves are velvetOS's, and its
``docs/subsystems/userland.rst`` is the program side.

the ext4 interface
--------------------

ext4, read and written by hand. it is the driver that was ext2, grown
rather than replaced: an ext4 disk holds files of both kinds at once, so what
a file does is decided per file by a flag in the inode, and what the driver
does not understand it refuses to write rather than guessing at.

``#define EXT4_INCOMPAT_KNOWN     (EXT4_INCOMPAT_FILETYPE \``
    what may be mounted at all: flex_bg only moves metadata about and costs nothing to accept, and recover is accepted but downgraded to read-only, since refusing it would refuse every disk that was unplugged carelessly and replaying a journal is the job of fs/jbd2.c

``#define EXT4_RO_COMPAT_WRITABLE (EXT4_RO_SPARSE_SUPER \``
    the ro_compat bits that change nothing about writing: they are either about how big a number may get or about a count the kernel already maintain. everything else in that field, checksums above all, means mounted, readable, and not written to

``#define EXT4_EXTENTS_FL         0x00080000``
    in the inode's flags: this file is mapped by extents rather than by a list of block numbers

``#define EXT4_EXTENT_MAGIC       0xf30a``
    at the top of every node of an extent tree, in the inode and in the blocks below it alike

``#define EXT4_S_IFMT   0xf000``
    the mode bits, which are unix's and are the point of the exercise

``typedef uint32_t (*ext4_clock)(void);``
    what time it is, for the three ext2 keeps.

``typedef bool (*ext4_sync)(void *ctx);``
    empty whatever sits between this driver and the disk.

``uint32_t block_size;``
    out of the superblock

``uint32_t reserved_gdt;``
    how many blocks after it belong to it but hold nothing yet.

``struct jbd journal;``
    the log, if there is one.

``struct ext4_file { uint32_t ino;``
    what an inode says about itself.

``bool ext4_mount(struct ext4 *fs, ext4_io read, ext4_out write, void *ctx);``
    read the superblock and check the kernel understands it.

``void ext4_set_clock(struct ext4 *fs, ext4_clock clock);``
    where the time comes from.

``bool ext4_lookup(struct ext4 *fs, const char *path, struct ext4_file *out);``
    resolve a path like "notes/deep.txt".

``bool ext4_lookup_nofollow(struct ext4 *fs, const char *path, struct ext4_file *out);``
    the same, without following a symlink at the end of it.

``bool ext4_readdir(struct ext4 *fs, uint32_t dir_ino, size_t index, struct ext4_file *out);``
    walk a directory. index from 0 until it returns false.

``int64_t ext4_read(struct ext4 *fs, const struct ext4_file *f, uint64_t offset, void *buf, uint64_t len);``
    read from anywhere in a file. returns bytes read, or -1

``int64_t ext4_write(struct ext4 *fs, struct ext4_file *f, uint64_t offset, const void *buf, uint64_t len);``
    write, growing the file and allocating blocks as needed.

``bool ext4_create(struct ext4 *fs, const char *path, uint32_t mode, uint32_t uid, uint32_t gid, struct ext4_file *out);``
    make things. the parent has to exist; the name has no 8.3 rule to obey, which is most of why this exists

``bool ext4_unlink(struct ext4 *fs, const char *path);``
    and unmake them

``bool ext4_chmod(struct ext4 *fs, const char *path, uint32_t mode);``
    the things fat had nowhere to write down

``uint64_t blocks_in_way;``
    set when a shrink was refused: what would have been lost

``uint64_t ext4_resize_ceiling(const struct ext4 *fs);``
    the largest this filesystem could ever be told to become, in blocks.

``bool ext4_raw_read(struct ext4 *fs, uint32_t block, void *into);``
    what fs/jbd2.c needs of this one: a block moved without the log in the way, where a file's nth block lives, and a way to make the disk actually hold what has been written

``void ext4_set_sync(struct ext4 *fs, ext4_sync sync);``
    where the sync comes from. optional, and the journal needs it

``bool ext4_start_journal(struct ext4 *fs);``
    replay the journal and start using it.

``void ext4_unix_to_time(uint32_t seconds, struct fat32_time *out);``
    seconds since 1970 into a date somebody can read.


the disk interface
------------------------

the disk, mounted. one place owns the sata controller and the filesystems
on it, so nothing above has to know which is which, and one disk can hold
two: the system on one and the work on another, which is what keeps a full
work filesystem from taking the machine with it. every call that touches a
file says which of them it means.

``const char *disk_mount_point(size_t which);``
    where a mount hangs in the namespace above: "/" or "/work"

``uint64_t entry_sector;``
    where the 32-byte record describing this file sits on the disk.

``struct fat32_time written;``
    when it was last written.

``enum disk_kind { DISK_NONE = 0, DISK_FAT32, DISK_EXT4, };``
    which filesystem answered.

``int disk_mounted_part(size_t which);``
    which partition is at this mount point, or -1

``bool disk_mount_part(size_t which, size_t index);``
    mount a particular partition at one of the two places.

``bool disk_mount(void);``
    find a controller, mount what is on it.

``bool disk_lookup(size_t which, const char *path, struct disk_entry *out);``
    look a file up. `path` is relative to the mount

``bool disk_readdir(size_t which, const char *path, size_t index, struct disk_entry *out);``
    the nth thing in a directory

``int64_t disk_read(size_t which, uint32_t cluster, uint64_t size, uint64_t offset, void *buf, uint64_t len);``
    read from a file already found.

``int64_t disk_write_at(size_t which, struct disk_entry *e, uint64_t offset, const void *buf, uint64_t len);``
    write to a file found by disk_create or disk_lookup.

``bool disk_mkdir(size_t which, const char *path);``
    directories, made and unmade. the same 8.3 rule as disk_create

``bool disk_unlink(size_t which, const char *path);``
    remove a file, and give its clusters back.

``bool disk_rename(size_t which, const char *from, const char *to);``
    give a file another name, possibly in another directory.

``bool disk_chmod(size_t which, const char *path, uint32_t mode);``
    all of these answer false on a fat disk, and say so rather than pretending to have worked.

``bool disk_readlink(size_t which, const char *path, char *out, size_t size);``
    where a symlink points, and looking one up *without* following it, which is what `ls -l` and `rm` want

``bool disk_sync(void);``
    writes do not reach the drive when they are made.

``bool disk_unmount(void);``
    and the last one: everything out, then the journal closed behind it.

``size_t   disk_drive_count(void);``
    everything else here goes through the mounted filesystem, which is the right shape for every question except one: installing this system onto a *different* drive, where there is no filesystem yet and the bytes have to land at particular sector numbers. these bypass the block cache entirely, and that is deliberate rather than lazy. the cache holds absolute sector numbers and is therefore bound to whichever drive is selected, a cached block written back after the drive changed would land on the wrong disk, which is the worst failure this kernel could have. the drive is selected, the transfer happens, and the mounted drive is selected again before returning, so nothing above notices

``int disk_mounted_drive(void);``
    which drive the mounted filesystem is on, or -1 if nothing is mounted.

``bool disk_system_read(void *ctx, uint64_t lba, uint32_t count, void *buf);``
    reading the boot medium, which is what fs/source.c does to find the source tree lying past everything philemon loaded. a plain shim over disk_raw_read on whichever drive that turned out to be

``int disk_system_drive(void);``
    which drive carries philemon's table, or -1 if none does.

``bool disk_fsck(size_t which, bool mend, struct fsck_report *out, const char **error);``
    check the mounted filesystem and mend what has exactly one right answer.

``uint64_t disk_room(size_t which);``
    how many of this filesystem's blocks the partition under it holds, and the largest it could ever be told to become

``const char *disk_label(size_t which);``
    what to tell the user about it


the diskinfo interface
------------------------

the disk and the namespace, described. each call writes with kprintf, so
it lands on the screen for the shell and in the reader's buffer for
``/proc``.

``void disk_show(void);``
    the drive, every filesystem on it, and the block cache: what ``disk``
    prints, and ``/proc/disk``

``void disk_show_partitions(void);``
    what each drive's partition table says it holds: ``parts``, and
    ``/proc/partitions``

``void vfs_show_mounts(void);``
    which filesystem is where: ``mount`` with nothing after it, and
    ``/proc/mounts``

``void fs_proc_init(void);``
    registers those three, for anyone to read


the fat32 interface
---------------------

fat32: the filesystem on a disk that survives a reboot. a boot sector, a
table with one entry per cluster where entry N holds the number of the
cluster that follows N, and then the data. a directory is not a special kind
of object, it is a file whose contents are 32-byte records.

the driver touches no hardware: it is handed two functions that move sectors,
which is what lets a test run it against a real image on a machine with no
disk at all.

``#define FAT32_SECTOR   512``
    fat32: a filesystem on a disk that survives a reboot.

``typedef bool (*fat32_io)(void *ctx, uint64_t lba, uint32_t count, void *buf);``
    how to reach the disk. returns false if the transfer failed

``uint32_t sectors_per_cluster;``
    straight out of the boot sector

``uint8_t  scratch[FAT32_SECTOR];``
    somewhere to put a sector while the kernel looks at it.

``struct fat32_file { char     name[FAT32_NAME_MAX];``
    what a directory entry says about a file.

``struct fat32_time written;``
    when it was last written, out of the directory entry

``bool fat32_mount(struct fat32 *fs, fat32_io read, fat32_out write, void *ctx);``
    read the boot sector and check it says what it should.

``void fat32_set_clock(struct fat32 *fs, fat32_clock clock);``
    where the time comes from.

``bool fat32_lookup(struct fat32 *fs, const char *path, struct fat32_file *out);``
    resolve a path like "notes/deep.txt". an empty path is the root

``bool fat32_readdir(struct fat32 *fs, uint32_t dir_cluster, size_t index, struct fat32_file *out);``
    walk a directory. index from 0 until it returns false.

``int64_t fat32_read(struct fat32 *fs, const struct fat32_file *f, uint64_t offset, void *buf, uint64_t len);``
    read from anywhere in a file.

``int64_t fat32_write(struct fat32 *fs, struct fat32_file *f, uint64_t offset, const void *buf, uint64_t len);``
    write, growing the file and its cluster chain as needed, and correcting the size in the directory afterwards. `f` is updated to match what is now on disk

``bool fat32_create(struct fat32 *fs, const char *path, struct fat32_file *out);``
    make a file in an existing directory.

``bool fat32_mkdir(struct fat32 *fs, const char *path);``
    make a directory. the same naming as fat32_create, and the new directory is born with the two entries every directory has

``bool fat32_rmdir(struct fat32 *fs, const char *path);``
    remove an empty one. a directory with anything in it is refused, unlinking a tree is a different operation and should look like one

``bool fat32_unlink(struct fat32 *fs, const char *path);``
    remove a file. the clusters go back, the entry is struck out, and the long-name entries in front of it go too

``bool fat32_usage(struct fat32 *fs, uint32_t *used, uint32_t *total);``
    how much of the disk is spoken for, in clusters


the vfs interface
-------------------

one namespace, several filesystems underneath it. the disk gets the root
because it is the bigger, writable, persistent thing; ``/work`` is a second
filesystem when the drive holds one; ``/boot`` is the ramdisk, which is
always there; and ``/boot/src`` is the source this kernel was built from, out
on the medium. a name with no leading slash is looked for on the disk first
and the ramdisk second, which is what lets a disk supply a newer ``bin/ls``
while a machine with no disk carries on with the one it booted with.

``#define VFS_NAME_MAX 128``
    one namespace, several filesystems underneath it.

``size_t mount;``
    which filesystem it came from, when it came from one.

``uint32_t mode;``
    the tar header's mode for the ramdisk, and the inode's for a disk that has one.

``const void *data;``
    where the bytes are: already in memory, or out on the disk

``struct fat32_time written;``
    when it was last written.

``bool vfs_open(const char *path, struct vfs_file *out);``
    resolve a path. absolute names go where they point; relative ones are tried on the disk and then the ramdisk

``bool vfs_readdir(const char *path, size_t index, struct vfs_file *out);``
    the nth thing in a directory.

``bool vfs_mkdir(const char *path);``
    and directories. the ramdisk is a tar in read-only memory, so `/boot` refuses both, there is nowhere for a new name to go

``bool vfs_unlink(const char *path);``
    remove a file. directories go through vfs_rmdir, which will not remove one that still has anything in it

``bool vfs_chmod(const char *path, uint32_t mode);``
    every one of these answers false on the ramdisk, and on a fat disk.

``bool vfs_readlink(const char *path, char *out, size_t size);``
    where a symlink points, and finding one without following it, which is what `ls -l` and `rm` want

``bool vfs_may_read(const struct vfs_file *f, int uid);``
    may a process running as `uid` read this?

``bool vfs_writable(const struct vfs_file *f);``
    may anyone write here at all? the ramdisk is read-only memory

``struct vfs_mount { const char *at;``
    the mount table, for `mount` to print


the bcache interface
----------------------

the block cache, between the filesystem and the drive. it slots in exactly
where fat32's two function pointers already are, which is why it can exist
without the filesystem knowing: the filesystem was handed a way to move
sectors, and still is.

writes are write-back rather than write-through, so until a block is evicted,
flushed or synced, what is on the disk is not what the machine believes. that
is the price of the speed, and it is why ``sync`` exists.

``#define BCACHE_SECTOR         512``
    a cache of disk blocks between the filesystem and the drive.

``typedef bool (*bcache_in)(void *ctx, uint64_t lba, uint32_t count, void *buf);``
    how to reach the actual drive.

``void bcache_init(bcache_in read, bcache_out write, void *ctx);``
    point it at a drive. forgets everything it was holding, which is the right thing when the drive underneath has changed and the wrong thing to do without syncing first, so mounting a second disk over a dirty cache is a caller's mistake, not something to paper over here

``bool bcache_read(void *ctx, uint64_t lba, uint32_t count, void *buf);``
    the two calls the filesystem makes.

``bool bcache_sync(void);``
    write every dirty block out.

``bool bcache_dirty(void);``
    is there anything to lose? cheap enough to ask on a timer

``struct bcache_stats { uint64_t hits;          /* answered without touching the drive */``
    what it has been doing, for `disk` to print.

``static inline uint64_t bcache_block_of(uint64_t lba)``
    which block an address belongs to, and where in it.


the elf interface
-------------------

loading a program. the file is checked before anything is mapped: an image
this machine cannot run is refused with a reason rather than half-loaded, and
then its segments are described so the loader knows where to put each one and
where to start executing.

``struct elf_load_result { uint64_t entry;         /* where to start executing */``
    just enough elf64 to load a static executable.

``bool elf_is_loadable(const void *image, uint64_t size, const char **why);``
    check the header without loading anything.

``bool elf_describe(const void *image, uint64_t size, struct elf_segment *out, size_t max, size_t *count, uint64_t *entry, uint64_t *brk, const char **why);``
    describe the segments without mapping anything.


the fsck interface
--------------------

checking a filesystem, and mending it, on the machine rather than on the
one it was developed on. a disk that can be damaged and cannot be examined is
a disk you throw away.

it reads the layout itself rather than walking the filesystem through the
driver, because a checker that shares the driver's idea of the disk cannot
notice the driver being wrong, which is the one thing it is for.

``#define FSCK_SECTOR     512``
    checking a filesystem, and mending it.

``typedef bool (*fsck_io)(void *ctx, uint64_t lba, uint32_t count, void *buf);``
    how to reach the disk.

``enum fsck_problem { FSCK_SUPERBLOCK,        /* it does not describe itself consistently */ FSCK_BLOCK_UNMARKED,    /* a file claims a block the bitmap calls free */ FSCK_BLOCK_CROSSED,     /* two files claim the same block */ FSCK_BLOCK_LEAKED,      /* the bitmap says used and nobody claims it */ FSCK_INODE_UNMARKED,    /* a name points at an inode the bitmap calls free */ FSCK_INODE_ORPHANED,    /* in use, and no name points at it */ FSCK_LINKS_WRONG,       /* the count is not the number of names */ FSCK_DIR_NO_DOTS,       /* a directory without . or .. */ FSCK_DIR_BAD_PARENT,    /* .. that is not the parent */ FSCK_DIR_BAD_RECORD,    /* a record whose length walks off the block */ FSCK_EXTENT_BAD,        /* a tree that does not parse */ FSCK_COUNTS_WRONG,      /* a free count that is not the true one */ FSCK_PROBLEMS };``
    what kind of thing was wrong.

``uint32_t blocks, inodes, groups, block_size;``
    what the disk turned out to be, for whoever is printing

``const char *stopped;``
    the first thing that went wrong, in words, or NULL

``bool fsck_measure(fsck_io read, void *ctx, uint32_t *blocks_out, uint32_t *inodes_out);``
    read the superblock far enough to size the workspace.

``const char *fsck_problem_name(enum fsck_problem which);``
    one line per kind of problem, for whoever is printing.


the install interface
-----------------------

copying this machine onto a disk it can then boot from on its own: the
system area verbatim, a partition table after it, and a filesystem on the
partition. what it needs from outside is function pointers for reading,
writing and saying what it is doing, which is what lets the same code be run
against a disk, a file or a fixture.

``bool (*src_read)(void *ctx, uint64_t lba, uint32_t count, void *buf);``
    the medium being installed *from*

``bool (*dst_read)(void *ctx, uint64_t lba, uint32_t count, void *buf);``
    and the one being installed *to*.

``void (*say)(void *ctx, const char *what);``
    said out loud as it goes.

``struct install_result { uint64_t system_sectors;    /* philemon, the table, the kernel, the ramdisk */``
    what it did, for the caller to report and the test to check

``uint64_t work_first_lba;``
    and the second partition, when the disk was big enough for one to be worth having.

``bool install_system(const struct install_io *io, uint32_t now, bool split, struct install_result *out, const char **error);``
    copy the system, partition what is left, and format it.

``uint64_t install_system_end(bool (*read)(void *, uint64_t, uint32_t, void *),``
    where the system area ends on a medium, by reading its table.


the jbd2 interface
--------------------

the journal: a record instead of a deduction. before a change touches the
filesystem it is written down in full elsewhere, with a mark at the end
saying all of it arrived, so recovery is read the log, apply what is marked
complete, and ignore the rest.

correctness here is ordering and nothing else, which is why every stage waits
before the next one begins.

``#define JBD_MAGIC        0xc03b3998u``
    the journal, and the crash question it answers.

``#define JBD_FEATURE_INCOMPAT_REVOKE  0x00000001u``
    the journal's own feature flags, which are not the filesystem's.

``#define JBD_FLAG_ESCAPE    0x0001   /* the block began with the magic */``
    in a descriptor's tags

``#define JBD_MAX_BLOCKS   16``
    the most blocks one transaction may hold.

``#define JBD_MAP_MAX      1024``
    the longest log this will use.

``uint32_t map[JBD_MAP_MAX];``
    where each block of the log actually is, resolved once

``uint32_t depth;``
    the transaction being built.

``void jbd_begin(struct jbd *j);``
    the transaction, around one thing a caller asked for

``bool jbd_open(const struct jbd *j);``
    is a transaction open?

``bool jbd_stage(struct jbd *j, uint32_t block, const void *data);``
    put one block in the log rather than at home.

``bool jbd_peek(struct jbd *j, uint32_t block, void *into);``
    has this block been written in the transaction that is open?


the mkfs interface
--------------------

making a filesystem rather than reading one, in the kernel, in the smallest
form that fs/ext4.c will mount and tools/readext4.py agrees is a filesystem.

it makes an *empty* one. copying a system onto it afterwards goes through the
ordinary write path, and a formatter that also laid out files would be a
second implementation of the half that already works.

``typedef bool (*mkfs_io)(void *ctx, uint64_t lba, uint32_t count,``
    the same shape part.c uses, for the same reason: it makes this testable against a buffer instead of a disk

``struct mkfs_result { uint64_t blocks;            /* 1 KiB each */``
    what the geometry came out as.

``bool mkfs_ext4(mkfs_io write, void *ctx, uint64_t sectors, const char *label, uint32_t now, struct mkfs_result *out, const char **error);``
    format `sectors` 512-byte sectors starting at lba 0 of whatever `write` writes to.

``#define MKFS_MIN_SECTORS 512``
    the smallest disk this can format, in sectors.


the path interface
--------------------

names, and where they point. a path is resolved against a working
directory, and split into the directory it names and the last component of
it. no filesystem is consulted: this is arithmetic over strings, which is why
it can be tested without one.

``#define PATH_MAX 256``
    working out what a name means.

``bool path_resolve(const char *cwd, const char *path, char *out, size_t size);``
    flatten `path`, read relative to `cwd`, into `out`.

``bool path_split(const char *path, char *dir, size_t dir_size, char *name, size_t name_size);``
    the directory containing `path`, and the last component of it.

``bool path_is_root(const char *path);``
    is `path` the root, however it happens to be spelled


the pipe interface
--------------------

a pipe: a ring buffer with a process at each end, and two rules that
between them are the whole of why a pipeline works.

a read with nothing in the buffer blocks, unless every writer has gone, and
then it returns 0, which is end of file. a write with no reader left fails
rather than filling a buffer that will never drain.

``#define PIPE_BUF 4096``
    a pipe: a buffer with a process at each end.

``int      readers;``
    how many descriptors still hold each end.

``uint32_t pipe_put(struct pipe *p, const void *buf, uint32_t len);``
    move what fits / what is there.

``void pipe_reset(struct pipe *p);``
    set one up by hand, for tests and for pipe_create

``struct pipe *pipe_create(void);``
    a new pipe, held open at both ends. NULL if there is no memory

``void pipe_close_read(struct pipe *p);``
    let go of one end. the pipe frees itself once nobody holds either, and whoever is left at the other end is woken to find out

``void pipe_share(struct pipe *p, bool writing);``
    one more holder of an end.

``int64_t pipe_read(struct pipe *p, int pid, void *buf, uint64_t len);``
    read, blocking until there is something or every writer has gone.

``int64_t pipe_write(struct pipe *p, int pid, const void *buf, uint64_t len);``
    write, blocking until it fits.

``void pipe_release_for(int pid);``
    let go of whichever ends a process was holding.

``size_t pipe_count(void);``
    how many exist right now, for `ps` and for finding a leak


the ramdisk interface
-----------------------

the tar philemon loaded into memory at boot: read-only, always there, and
gone when the power goes. it is what makes the machine work when the disk
does not, and every program, and the passwd file, live on it.

``struct ramdisk_file { const char *name;``
    a read-only filesystem that is just a tar file philemon handed the kernel at boot.

``uint32_t    mode;``
    the unix mode tar recorded.

``bool ramdisk_may_read(const struct ramdisk_file *f, int uid);``
    may a process running as `uid` read this file?

``void ramdisk_init(void);``
    take the archive philemon loaded.

``void ramdisk_mount(const void *base, uint64_t size);``
    point at an archive directly. the guts, so tests can hand it bytes

``bool ramdisk_stat(size_t index, struct ramdisk_file *out);``
    walk the archive. index from 0 until it returns false

``bool ramdisk_open(const char *name, struct ramdisk_file *out);``
    find one by name. returns false if it isnt there


the source interface
----------------------

the source tree this kernel was built from, out on the boot medium and read
a sector at a time by whoever asks. it is *reachable* rather than resident:
the ramdisk is loaded whole and never freed, and the source is read on the
day you install the machine and rarely again.

``#define SOURCE_NAME_MAX 128``
    the source this machine was built from, still on the medium it booted from.

``uint64_t at;``
    where the bytes are, as a byte offset from the start of the medium.

``void source_init(void);``
    find the archive on the drive this machine booted from, and say so.

``bool source_open(const char *name, struct source_file *out);``
    by name, exactly as the archive spells it: "main.c"

``int64_t source_read_at(uint64_t at, uint64_t size, uint64_t offset, void *buf, uint64_t len);``
    bytes out of a file. `at` and `size` are the file's, and everything in between is arithmetic on sectors

``bool source_digest(uint8_t out[20]);``
    and the whole archive, hashed off the medium.

``extern const char source_stamp[41];``
    what tools/srcstamp.py wrote down at build time: the sha-1 of the archive, hex, as a fact about the kernel rather than about the medium. the two agreeing is the claim; the two disagreeing is a medium that was half written, or an image assembled out of parts from different builds


the ustar interface
---------------------

the tar format, which is both of the archives here: a fixed-width header of
text fields per file, then the bytes, padded out to a block. it is read
rather than written, and the two readers of it are the ramdisk and the source
tree on the medium.

``#define USTAR_BLOCK 512``
    ustar, which two things in this kernel now read.

``static inline uint64_t ustar_octal(const char *field, size_t len)``
    ascii octal, possibly padded with spaces or terminated early.

``static inline uint64_t ustar_next(uint64_t offset, const struct tar_header *h)``
    how far to the next header: this one, plus its contents rounded up
