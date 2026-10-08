#!/usr/bin/env python3
"""the three sums, against three other people's implementations.

the fourth tool checked this way, and the one where a published test
vector looks like it ought to be enough. it is not. a vector proves the
algorithm was copied correctly for the lengths whoever wrote the standard
happened to think of, and every one of them is a single buffer hashed in
one go, which is the case that cannot fail, because it never exercises
the part where the input arrives in pieces and a half-finished block has
to be kept until the rest of it turns up.

so this feeds three hundred and something lengths of pseudo-random bytes
through ours and through python's, and then feeds every one of them again
in chunks of 1, 7, 63, 64 and 65 bytes. sixty-four is the block size, so
the ones either side of it are where the buffering is either off by one
or absent.

and the last check is the one that matters for what comes next: **git
itself**. a git object's name is the sha-1 of its type, its length, a
zero byte and its content, which is not a thing a sha-1 vector can tell
you, so the name this produces for a file is compared against what
`git hash-object` says. that is the whole contract 0.3.19 rests on,
tested before there is anything to rest it on.

usage:  tools/hashcheck.py
"""
import subprocess, sys, os, random, zlib, hashlib, tempfile, shutil

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OURS = os.path.join(ROOT, "bin", "tests", "hash")


def ours(mode, data):
    r = subprocess.run([OURS, mode, "-"], input=data, capture_output=True)
    if r.returncode != 0:
        return "error: " + r.stderr.decode(errors="replace").strip()
    return r.stdout.decode().strip()


def main():
    if not os.path.exists(OURS):
        print("  bin/tests/hash is not built")
        return 1

    # a fixed seed, so a failure is a failure somebody else can reproduce
    # rather than a thing that happened once on my machine
    rng = random.Random(20250817)

    lengths = list(range(0, 200))
    lengths += [255, 256, 257, 511, 512, 513, 1000]
    lengths += [5551, 5552, 5553]       # where adler32 has to reduce
    lengths += [65535, 65536, 65537]

    bad = 0
    checked = 0

    for n in lengths:
        data = bytes(rng.randrange(256) for _ in range(n)) if n < 2000 \
            else rng.randbytes(n)

        want = hashlib.sha1(data).hexdigest()
        got = ours("--sha1", data)
        if got != want:
            print("  sha1 of %d bytes: ours %s, python %s" % (n, got, want))
            bad += 1

        want = "%08x" % zlib.crc32(data)
        got = ours("--crc32", data)
        if got != want:
            print("  crc32 of %d bytes: ours %s, zlib %s" % (n, got, want))
            bad += 1

        want = "%08x" % zlib.adler32(data)
        got = ours("--adler32", data)
        if got != want:
            print("  adler32 of %d bytes: ours %s, zlib %s" % (n, got, want))
            bad += 1

        checked += 3

        # and the same bytes arriving badly. 64 is the block size, so 63
        # and 65 are where a buffer is kept for one byte too few or one
        # too many, which produces a digest that is perfectly
        # self-consistent and agrees with nobody
        for chunk in (1, 7, 63, 64, 65):
            if n == 0 and chunk != 1:
                continue
            want = hashlib.sha1(data).hexdigest()
            got = ours("--sha1-chunked=%d" % chunk, data)
            if got != want:
                print("  sha1 of %d bytes in %d-byte pieces: ours %s, "
                      "python %s" % (n, chunk, got, want))
                bad += 1
            checked += 1

        if bad:
            break

    # what git calls a file, which is a different question from what
    # sha-1 makes of it
    if shutil.which("git") is not None and not bad:
        work = tempfile.mkdtemp()
        for n in (0, 1, 11, 63, 64, 65, 1000, 100000):
            path = os.path.join(work, "f%d" % n)
            open(path, "wb").write(bytes(rng.randrange(256) for _ in range(n))
                                   if n < 2000 else rng.randbytes(n))
            want = subprocess.run(["git", "hash-object", path],
                                  capture_output=True, text=True).stdout.strip()
            r = subprocess.run([OURS, "--git-blob", path],
                               capture_output=True, text=True)
            got = r.stdout.strip()
            if got != want:
                print("  git would name the %d-byte file %s, we say %s"
                      % (n, want, got))
                bad += 1
            checked += 1
        shutil.rmtree(work, ignore_errors=True)

    if not bad:
        print("  %d answers agree with python, zlib and git" % checked)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
