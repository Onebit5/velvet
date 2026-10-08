#!/usr/bin/env python3
"""read the source tree back out of the boot image, and hold it against
the tree it was supposed to be.

0.3.26 writes the project's own source into the boot image, past the
ramdisk, at sectors philemon's table names. everything that puts it
there is mine, a line of the makefile, a python tool, a C reader,
and a source tree that is checked only by the code that wrote it is a
source tree nobody has actually read.

so this starts from the image and works outwards, the way readext4.py
does with a filesystem:

    the boot table says where the archive is
    python's tarfile reads it, which is somebody else's tar reader
    every file in it is compared, byte for byte, with the working tree
    every file git tracks has to be in it
    and the sha-1 of the whole archive has to appear inside the kernel,
      which is how the machine knows its own source at runtime

the last one is the one worth having. an archive and a kernel sitting in
the same image are two files that happen to be adjacent; the stamp is
what makes them a claim about each other, and a stamp nothing checks is
a claim nobody has tested.

usage: srccheck.py [image]
"""

import hashlib
import io
import os
import struct
import subprocess
import sys
import tarfile

SECTOR = 512
TABLE_LBA = 32
MAGIC = 0x50484c4d4e30       # "PHLMN0"

# what the build leaves out of the archive on purpose: things it made
BUILT = ('base/ramdisk/bin/',)


def read_table(image):
    at = TABLE_LBA * SECTOR
    fields = struct.unpack('<12Q', image[at:at + 96])
    if fields[0] != MAGIC:
        sys.exit('srccheck: no philemon table at lba %d' % TABLE_LBA)
    return {
        'kernel_lba': fields[3], 'kernel_size': fields[5],
        'ramdisk_lba': fields[6], 'ramdisk_sectors': fields[7],
        'source_lba': fields[9], 'source_sectors': fields[10],
        'source_size': fields[11],
    }


def main(path):
    image = open(path, 'rb').read()
    t = read_table(image)
    problems = []

    if t['source_size'] == 0:
        sys.exit('srccheck: %s carries no source tree at all' % path)

    # the region has to be where it says, entirely inside the image, and
    # after everything philemon loads. an archive overlapping the ramdisk
    # would read perfectly here and boot into nonsense
    start = t['source_lba'] * SECTOR
    end = start + t['source_size']
    if t['source_lba'] < t['ramdisk_lba'] + t['ramdisk_sectors']:
        problems.append('the archive starts inside the ramdisk')
    if end > len(image):
        problems.append('the archive runs off the end of the image')
    if t['source_size'] > t['source_sectors'] * SECTOR:
        problems.append('the table claims more bytes than sectors')
    if problems:
        for p in problems:
            print('  %s' % p)
        sys.exit('srccheck: the table does not describe this image')

    archive = image[start:end]

    # somebody else's tar reader. there is no second implementation of
    # ext4 or fat on this machine, which is why those checkers are
    # written by hand, but there is one of tar, in the standard
    # library, and the whole point of a second opinion is that it is not
    # mine
    tar = tarfile.open(fileobj=io.BytesIO(archive), mode='r:')
    members = [m for m in tar.getmembers() if m.isfile()]
    names = sorted(m.name for m in members)

    if len(names) != len(set(names)):
        problems.append('the archive holds the same name twice')

    # every byte of every file, against the tree this was built from
    for m in members:
        if not os.path.exists(m.name):
            problems.append('%s is in the archive and not in the tree' % m.name)
            continue
        want = open(m.name, 'rb').read()
        got = tar.extractfile(m).read()
        if got != want:
            problems.append('%s differs from the working tree (%d bytes in '
                            'the archive, %d on disk)'
                            % (m.name, len(got), len(want)))
        if m.mode & 0o022:
            problems.append('%s is group- or world-writable in the archive'
                            % m.name)
        if m.name.startswith('/') or '..' in m.name.split('/'):
            problems.append('%s would unpack outside where it was put'
                            % m.name)

    # and the other direction, which is the one that matters: a source
    # tree missing the file you need is worse than no source tree, since
    # you find out at the end. git is the second opinion about what the
    # project consists of, the makefile builds the list with `find`,
    # and a checker running the same `find` would agree with itself.
    # --recurse-submodules because a tree that carries this kernel as a
    # submodule ships the kernel's files too, and without it git names
    # the submodule as one entry and none of what is in it
    try:
        tracked = subprocess.run(['git', 'ls-files', '--recurse-submodules'],
                                 capture_output=True,
                                 text=True, check=True).stdout.split()
    except (OSError, subprocess.CalledProcessError):
        tracked = None
        print('  (no git here, so what the project consists of is taken '
              'on trust)')

    if tracked:
        have = set(names)
        for f in tracked:
            if f.startswith(BUILT) or f in have:
                continue
            problems.append('%s is tracked and not in the archive' % f)

    # the stamp: the sha-1 of the archive, which tools/srcstamp.py wrote
    # into the kernel before it was linked. it has to be *in* the kernel
    # that is in this image, not merely in some kernel
    digest = hashlib.sha1(archive).hexdigest()
    kernel = image[t['kernel_lba'] * SECTOR:
                   t['kernel_lba'] * SECTOR + t['kernel_size']]
    if digest.encode() not in kernel:
        problems.append('the kernel in this image does not carry the '
                        'archive\'s sha-1 (%s)' % digest)

    if problems:
        for p in problems:
            print('  %s' % p)
        sys.exit('srccheck: %d problem(s)' % len(problems))

    print('%s: %d files, %d KiB at lba %d, sha1 %s, every one of them '
          'matches the tree, and the kernel knows it'
          % (path, len(members), len(archive) // 1024, t['source_lba'],
             digest))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'velvet.img')
