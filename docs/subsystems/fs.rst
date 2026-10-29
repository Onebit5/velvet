filesystems
===========

``fs/`` is one namespace, several filesystems under it, and the
layers that let a program not care which one answered.

the namespace
-------------

``vfs.c`` is the router. the kernel hands /init a root and nothing else:

::

    /          the archive the loader handed over, unpacked into memory
    /dev       the devices, and the drives with their partitions

and which disk is the system, where it hangs and what else is mounted is
the system's to say. a system's /init mounts its disk, moves /dev onto it
and makes it the root with ``pivot_root``, the archive going somewhere
under it; the kernel's own image does that, and ends up with

::

    /          the system disk
    /boot      the archive, in memory
    /dev       the devices, moved here from the archive
    /proc      what the kernel says about itself, mounted by /init
    /work      a second filesystem, when the drive holds one

that list is a table of mounts, each a point and what hangs there. a name
belongs to the mount whose point is the longest one it starts with, at a
slash, so ``/bootleg`` is the root's and ``/boot/x`` is the archive's, and
what the filesystem under the mount is asked about is the rest of the
name, from that filesystem's own root. a disk mount with nothing behind it
is not in the namespace at all: the names under it are the next mount's
out, and a listing of the directory it stands in leaves it out.

``mount`` adds to the table and ``umount2`` takes from it, root's both
(``fs/namespace.c``). what can be mounted is ext2, ext3 and ext4, one
driver, and vfat, each off a drive or a partition named in /dev; ramfs,
also called tmpfs, with ``mode=`` for its root; and /proc and /dev in a
second place. it hangs over a directory that is there, which goes out of
sight until the mount comes down. a mount comes down only when nothing
stands on it, no file is open on it and no working directory is in it,
and a call in the middle of a name under one counts it, so it cannot go
out from under that call on another core. ``statfs`` says what kind of
filesystem a name is on, by the number every program knows that kind by,
and how full it is.

``MS_MOVE`` moves a mount, with every mount standing on it, and
``pivot_root`` makes one mount the root and puts the old root under it,
taking every mount that was not under the new root along. the table is a
list of paths, so both are a rewrite of the paths in it, and of every path
a process holds: its working directory, its root, and the name each open
file was opened by, each file once however many descriptors share it, so
whatever anybody named before names the same thing after. a process
standing in the old root is moved to the new one, which is what lets
/init carry on after pivoting with its working directory at the disk's
root. ``chroot`` gives a process a root of its own: a name it hands over
is resolved inside that root, where ``..`` stops, and ``getcwd`` answers
from inside it.

the root being the archive in memory is what makes the machine work when
the disk does not: a kernel whose only filesystem needs a sata controller
to bring up is one that a missing cable turns into a brick, and one whose
root is read-only memory is one a program cannot make a directory in to
mount anything over.

a ``struct vfs_file`` remembers **which filesystem it came from**. a
descriptor that remembered a file but not which of two disks it was on would
read the right offset out of the wrong disk, which is the sort of bug that
looks like a corrupt file. a rename from one mount to another is *refused*
rather than quietly turned into a copy and a delete wearing a rename's name,
because a rename that copies is not a rename.

every call that touches a file says which of the two disks it means
(``DISK_ROOT`` or ``DISK_WORK``). the alternative was a second copy of each
with ``work_`` in front of it, and two copies of anything drift. the two can
be on different drives: the block cache knows a block by its drive as well as
its number, so one cache holds both and neither answers for the other. at
boot the second is looked for on the drive the first was found on.

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

``/proc`` has ``version``, which kernel this is, the compiler that made it
and when, on one line, and ``uptime``, seconds since boot to the
hundredth. the rest are each subsystem's own.

the filesystem's own are written by ``diskinfo.c``: ``disk``, the drive
with every filesystem on it and what the block cache is holding;
``partitions``, what each drive's table says it has; and ``mounts``, which
filesystem is where.

/dev
----

``dev.c`` is a table of devices, opened by name: ``null``, which reads as
the end and swallows what is written; ``zero``, which reads as zeroes;
``full``, which reads as zeroes and has no room to write; ``random`` and
``urandom``, the same generator; and ``tty`` and ``console``, the
terminal. each is a file with operations of its own, and like /proc
nothing under /dev is stored anywhere.

after them come the drives and their partitions, as the disk layer found
them: ``sda`` for the first drive and ``sda1`` for the first partition
its table names, block devices numbered major 8 and minor drive * 16 +
partition. they are root's, mode 0600, because a drive is every file on
it whoever's those are. ``blockdev.c`` is what one is when open: read and
written by the byte from the position, the sector a run starts or ends
inside of read whole and put back whole. ``BLKGETSIZE64`` and
``BLKGETSIZE`` say how big it is, ``BLKSSZGET`` that a sector is 512
bytes, and ``BLKFLSBUF``, an ``fsync`` or the last close write out what
is held for it.

a drive's bytes go through the block cache, the same one a filesystem on
it goes through, rather than around it. a program reading a partition
under a mounted filesystem reads what that filesystem last wrote, and one
writing it changes what the filesystem reads next: there is no second
copy of any block to disagree with the first. what the filesystem holds
in memory and has not written yet it does not see, which is what
``sync`` is for.

the generator is ``drivers/random.c``: a chacha20 keystream whose key
changes after every request, to the first block the old one makes, which
is never handed out, so bytes already given away cannot be worked back to
from the key that follows them. it is seeded at boot from the core's own
generator where it has one, the clock, and the cycle counter many times
over, and every timer tick and anything written to ``random`` stirs in
more. ``lib/chacha20.c`` is the block function, held to the published test
vector by ``test_chacha20``.

the ramdisk
-----------

``ramdisk.c`` reads the ustar tar that the loader loaded at boot. a 512
byte header of ascii fields, the file's bytes rounded up to 512, repeat,
and two blocks of zeroes to finish. a header whose size would run off the
end of the archive stops the walk, so nothing past the end is ever read,
and a long name is the header's prefix, a slash and its name.

it is not a filesystem: it is unpacked, once, into a ramfs that is the
root, every file with its bytes, mode and owner, every directory a name
needs made on the way, links and device nodes made as what they are.

ramfs
-----

``ramfs.c`` is a filesystem kept in memory, written and read like any
other and forgotten when the machine stops: what the archive can be
unpacked into so a program may change it, and what a directory nobody
wants kept can be. it is a tree of nodes, each a file, a directory, a
link or a device's number, and a file's bytes are a page each, a page
never written reading as zeroes. there can be several at once, each
known by a number the way a disk mount is.

every node of all of them is in one table, and is known by a number
rather than by where it is in memory: the low bits are its place in the
table and the high bits how many nodes that place has held. an open file
holds its node's number, and so does a lookup on one core while another
unlinks the name, so a node that has gone is one whose number finds
nothing rather than memory already handed to something else. a file
unlinked while open keeps its bytes, without a name, until the last
close, and a filesystem with a file open on it cannot be given back.

one lock covers all of it, held for one walk of a path or one page's
copy at most.

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

open files
----------

a descriptor is a number for an open file, ``struct file`` in
``include/velvet/file.h``, and the file is what is shared. ``open`` makes
one, with a position and the mode it was opened for, which no later call
can widen: a file opened to be read is not written through, whoever owns
it. ``dup2`` and ``fork`` make another descriptor on the same file, so the
two share its position, which is what lets a shell's child write after
what its parent wrote. the file goes when the last descriptor on it, in
any process, closes, and not before.

what can be done to a file is in its ``struct file_operations``, filled in
by whoever opened it: ``vfs_open_file`` for a file on a disk, in memory
or in /proc, ``blockdev_open`` for a drive, ``pipe_open`` for a pipe's
two ends, and the console's own in
``tty.c``. each takes the kernel's memory, never ring 3's, and answers with
a count or minus an errno.

``struct file *file_alloc(const struct file_operations *p_ops, uint32_t u_mode, enum file_type e_type);``
    a new file with one reference. NULL when there is no memory

``void file_get(struct file *p_file);``
    one more reference.

``void file_put(struct file *p_file);``
    one fewer, released with the last. never with a lock held: releasing a
    pipe's end wakes whoever waits at the other one

``struct file *vfs_open_file(const struct vfs_file *p_node, uint32_t u_mode);``
    a file on a filesystem, at its start. reads carry on from where the last
    stopped and a regular file is read to its end; ``seek`` stays within it

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

a program builds the same thing with two calls. ``pipe`` hands it both ends
of a new pipe, each an open file, in the lowest free slots. ``dup2`` makes
one slot another descriptor on the file a second one holds, closing
whatever the first held, so a shell forks, puts a pipe end on 0 or 1 in the
child, and execs. a pipe counts the files open on each end, not the
descriptors, so the reader sees end of file only when every descriptor on
the writing end, in every process, has closed. ``lseek`` moves a file's
position, within the file and no further, and is how a shell puts ``>>`` on
the end of what is there rather than over its start.

growing a filesystem
--------------------

the kernel does not make filesystems; ``tools/mkfs.c`` does, on the host,
and a program would on the machine. what the kernel holds a formatter to is
that the descriptor table is reserved up front (``s_reserved_gdt_blocks``),
so that growing the filesystem never has to move every bitmap and inode
table on the disk. see `resize` in the testing page.

a program asks for a resize with ``ioctl(fd, EXT4_IOC_RESIZE_FS,
&blocks)``, root's alone, on any file or directory of the mounted
filesystem, with the size in its own blocks, 0 for as far as the
partition and that reserved table both allow. how far the partition
allows is ``disk_resize``'s to enforce rather than its caller's: the
filesystem is handed a view that ends where its partition does and cannot
see past it, and a filesystem longer than its partition writes over
whatever is next. a resize that cannot be made is ``-EINVAL``, and why,
what was in the way of a shrink or what the partition would not hold, is
in the kernel's log.

elf
---

``elf.c`` loads a program, from whatever filesystem it is on. the programs themselves are not in this tree.

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

``bool ext4_truncate(struct ext4 *fs, struct ext4_file *f, uint64_t u_size);``
    a regular file made any length. longer reads as zeroes past the old
    end, since a block that is not there does; shorter gives back every
    block past the new end and zeroes the rest of the last one kept. an
    extent tree deeper than unlink walks is refused with nothing changed

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
on it, so nothing above has to know which is which. a filesystem is
mounted in a slot of its own, eight of them, and every call that touches a
file says which slot it means. the kernel mounts none of them: ``mount``
does, and a system that puts its work on a second filesystem beside the
system keeps a full work filesystem from taking the machine with it.
where each hangs in the namespace is the namespace's to say.

``uint64_t entry_sector;``
    where the 32-byte record describing this file sits on the disk.

``struct fat32_time written;``
    when it was last written.

``enum disk_kind { DISK_NONE = 0, DISK_FAT32, DISK_EXT4, };``
    which filesystem answered.

``int disk_mounted_part(size_t which);``
    which partition is in this slot, or -1

``int disk_mount_block(const struct disk_block *p_block, enum disk_kind e_want);``
    the filesystem on a drive or a partition, of the kind wanted or of either kind, mounted in a free slot: which one, or -EBUSY when its sectors are mounted already, since two mounts of one filesystem are two copies of its state disagreeing about the same disk

``bool disk_unmount_slot(size_t sz_which);``
    a slot's filesystem written out, its log closed, and the slot free

``bool disk_scan(void);``
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

``const struct disk_block *disk_block_at(size_t sz_index);``
    everything else here goes through a mounted filesystem, which is the right shape for every question except one: a drive by its bytes, where there may be no filesystem yet. the drives and the partitions their tables name are listed once, when the drives are found, every drive first and then every partition, each with the number /dev gives it

``int64_t disk_block_read(const struct disk_block *p_block, uint64_t u_offset, void *p_buf, uint64_t u_len);``
    and its bytes, through the block cache, so a program and a filesystem on the same drive see one copy of every block between them. every drive is ready for commands from the moment it is found, so nothing is switched to reach one and nothing is switched back

``int disk_mounted_drive(void);``
    which drive the mounted filesystem is on, or -1 if nothing is mounted.

``int disk_system_drive(void);``
    which drive carries the boot table, or -1 if none does.

``bool disk_resize(size_t which, uint64_t blocks, struct ext4_resize *out, const char **error);``
    resize the filesystem mounted at `which` to `blocks` of its own, and
    refuse a size longer than the partition under it

``uint64_t disk_room(size_t which);``
    how many of this filesystem's blocks the partition under it holds, and the largest it could ever be told to become

``const char *disk_label(size_t which);``
    what to tell the user about it


the diskinfo interface
------------------------

the disk and the namespace, described. each call writes with kprintf, and
``/proc`` catches what it writes in the reader's buffer.

``void disk_show(void);``
    the drive, every filesystem on it, and the block cache:
    ``/proc/disk``

``void disk_show_partitions(void);``
    what each drive's partition table says it holds, and where each one
    mounted is: ``/proc/partitions``

``void vfs_show_mounts(void);``
    which filesystem is where: ``/proc/mounts``

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

``bool fat32_truncate(struct fat32 *fs, struct fat32_file *f, uint64_t u_size);``
    a file made any length: zeroes written past the old end, since fat has no holes and a free cluster still holds what it held, and the chain ended and the rest freed short of it

``bool fat32_usage(struct fat32 *fs, uint32_t *used, uint32_t *total);``
    how much of the disk is spoken for, in clusters


the vfs interface
-------------------

one namespace, several filesystems underneath it, mounted where a table
of mounts says. a name with no leading slash, from inside the kernel,
which has no working directory, is the root's.

``#define VFS_NAME_MAX 128``
    one namespace, several filesystems underneath it.

``size_t mount;``
    which filesystem it came from, when it came from one.

``uint32_t mode;``
    the node's mode in memory, and the inode's for a disk that has one.

``const void *data;``
    where the bytes are: already in memory, or out on the disk

``struct fat32_time written;``
    when it was last written.

``bool vfs_open(const char *path, struct vfs_file *out);``
    resolve a path, through the mount it is under, to what the filesystem there has

``bool vfs_readdir(const char *path, size_t index, struct vfs_file *out);``
    the nth thing in a directory.

``bool vfs_mkdir(const char *path);``
    and directories. /proc and /dev keep no names of their own, so they refuse both, there is nowhere for a new name to go

``bool vfs_unlink(const char *path);``
    remove a file. directories go through vfs_rmdir, which will not remove one that still has anything in it

``bool vfs_truncate(struct vfs_file *f, uint64_t size);``
    a file on a disk or in memory made any length. /proc refuses, and so does a directory

``bool vfs_chmod(const char *path, uint32_t mode);``
    every one of these answers false in /proc and /dev, and on a fat disk.

``bool vfs_readlink(const char *path, char *out, size_t size);``
    where a symlink points, and finding one without following it, which is what `ls -l` and `rm` want

``bool vfs_may_read(const struct vfs_file *f, int uid);``
    may a process running as `uid` read this?

``bool vfs_writable(const struct vfs_file *f);``
    may anyone write here at all? /proc and /dev are the kernel's

``struct vfs_mount { char arr_at[PATH_MAX];``
    one mount, copied out of the table, for /proc/mounts to print

``int64_t vfs_mount_add(const char *str_at, enum vfs_kind e_kind, size_t sz_slot, const char *str_source);``
    a filesystem hung at a point: -EBUSY when something hangs there already. that there is a directory there is the caller's to see to

``int64_t vfs_mount_remove(const char *str_at, enum vfs_kind *p_kind, size_t *p_slot);``
    a mount taken down, saying what was there so the caller can let it go: -EBUSY for the root, or for one something else stands on, a file is open on, a working directory is in, or a call is in the middle of a name under


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

``void bcache_init(bcache_in read, bcache_out write);``
    say how to reach the drives. a block is known by the ``ctx`` it was read or written with as well as its number, and that ``ctx`` is what it goes back to the drive with, so one cache serves every drive. forgets everything it was holding, which is the wrong thing to do without syncing first, so calling it over a dirty cache is a caller's mistake, not something to paper over here

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
    how many files are open on each end.

``uint32_t pipe_put(struct pipe *p, const void *buf, uint32_t len);``
    move what fits / what is there.

``void pipe_reset(struct pipe *p);``
    set one up by hand, for tests and for pipe_create

``struct pipe *pipe_create(void);``
    a new pipe, held open at both ends. NULL if there is no memory

``void pipe_close_read(struct pipe *p);``
    let go of one end. the pipe frees itself once nobody holds either, and whoever is left at the other end is woken to find out

``int64_t pipe_read(struct pipe *p, int pid, void *buf, uint64_t len);``
    read, blocking until there is something or every writer has gone: 0
    then, or -EINTR for a signal while waiting.

``int64_t pipe_write(struct pipe *p, int pid, const void *buf, uint64_t len);``
    write, blocking until it fits: -EPIPE when no reader is left.

``bool pipe_open(struct file **pp_read, struct file **pp_write);``
    a new pipe, as two files: one to read it and one to write it.

``size_t pipe_count(void);``
    how many exist right now, for ``/proc/threads`` and for finding a leak


the ramdisk interface
-----------------------

the tar the loader put in memory at boot, read where it lies and
unpacked into the root.

``struct ramdisk_file { char arr_name[RAMDISK_NAME_MAX];``
    one entry: its whole name, prefix and all, its bytes where they lie, its type, mode, owner, and a link's target or a device's number

``void ramdisk_init(void);``
    take the archive the loader loaded.

``void ramdisk_mount(const void *base, uint64_t size);``
    point at an archive directly. the guts, so tests can hand it bytes

``bool ramdisk_stat(size_t index, struct ramdisk_file *out);``
    walk the archive. index from 0 until it returns false, which it does at the end, at a header that is not ustar's, and at one whose size runs off the end

``size_t ramdisk_unpack(size_t sz_fs);``
    every entry made in the ramfs numbered sz_fs, with the directories each needs on the way


the ustar interface
---------------------

the tar format, which is what the ramdisk is: a fixed-width header of text
fields per file, then the bytes, padded out to a block. it is read rather
than written.

``#define USTAR_BLOCK 512``
    ustar, which the ramdisk is.

``static inline uint64_t ustar_octal(const char *field, size_t len)``
    ascii octal, possibly padded with spaces or terminated early.

``static inline uint64_t ustar_next(uint64_t offset, const struct tar_header *h)``
    how far to the next header: this one, plus its contents rounded up
