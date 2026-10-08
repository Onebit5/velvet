#!/usr/bin/env python3
"""deflate and inflate, against python's zlib and against a real git repo.

the two halves need different checks, and the reason is worth stating.

**inflate** has to read what *other* people wrote. deflate is three
formats in a trenchcoat, stored, fixed huffman, dynamic huffman, and
which one comes out is decided by the compressor's level: 0 stores, the
low levels emit fixed codes, the high ones build a table per block. so a
decompressor tested only against its own compressor has been tested
against one third of the format, and the third it wrote itself. every
input here goes through zlib at all ten levels.

**deflate** only has to be *understood*. "it is small" is not a claim
worth making and not a claim anything checks; "zlib reads it back and
gets the original" is the whole contract, and it is the one that decides
whether a repository this machine writes can be cloned by git.

and then the thing this version exists for: the loose objects in an
actual repository, created by actual git, inflated by ours, and compared
against the bytes git says are in them.

usage:  tools/zlibcheck.py
"""
import subprocess, sys, os, zlib, random, tempfile, shutil

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OURS = os.path.join(ROOT, "bin", "tests", "inflate")


def run(mode, data):
    r = subprocess.run([OURS, mode], input=data, capture_output=True)
    return r.stdout, r.returncode, r.stderr.decode(errors="replace").strip()


def cases():
    rng = random.Random(20250818)
    yield "nothing", b""
    yield "one byte", b"x"
    yield "a short line", b"hello, velvet\n"
    yield "a long run", b"a" * 70000
    yield "alternating", b"ab" * 30000
    yield "random", rng.randbytes(50000)
    yield "text", (b"the quick brown fox jumps over the lazy dog\n" * 4000)
    yield "this file", open(__file__, "rb").read()
    # something with structure at a distance: a table repeated far apart,
    # which is what makes the 32k window matter rather than the last page
    filler = rng.randbytes(20000)
    yield "far apart", b"HEADER" * 100 + filler + b"HEADER" * 100
    # every length either side of a byte boundary, because a bit writer
    # that loses the last partial byte fails only on some lengths
    for n in (1, 2, 3, 7, 8, 9, 255, 256, 257):
        yield "%d bytes" % n, bytes(rng.randrange(256) for _ in range(n))


def main():
    if not os.path.exists(OURS):
        print("  bin/tests/inflate is not built")
        return 1

    bad = 0
    checked = 0

    for name, data in cases():
        # ours must read what zlib writes, at every level, which is
        # the only way the stored and dynamic paths are reached at all
        for level in range(0, 10):
            packed = zlib.compress(data, level)
            got, rc, err = run("--inflate", packed)
            if rc != 0 or got != data:
                print("  %s at level %d: ours %s"
                      % (name, level, err or "gave %d bytes, wanted %d"
                         % (len(got), len(data))))
                bad += 1
                break
            checked += 1

        # and zlib must read what ours writes
        packed, rc, err = run("--deflate", data)
        if rc != 0:
            print("  %s: ours would not compress it: %s" % (name, err))
            bad += 1
            continue
        try:
            if zlib.decompress(packed) != data:
                print("  %s: zlib read ours and got something else" % name)
                bad += 1
        except zlib.error as e:
            print("  %s: zlib will not read what we wrote: %s" % (name, e))
            bad += 1
        checked += 1

        if bad:
            break

    # a real repository, written by real git. the objects in .git/objects
    # are zlib streams of "<type> <length>\0<content>", which is the
    # thing 0.3.19 has to be able to read, and this is it being read
    if shutil.which("git") is not None and not bad:
        work = tempfile.mkdtemp()
        env = dict(os.environ, GIT_AUTHOR_NAME="check", GIT_AUTHOR_EMAIL="c@x",
                   GIT_COMMITTER_NAME="check", GIT_COMMITTER_EMAIL="c@x")
        def git(*args):
            return subprocess.run(["git"] + list(args), cwd=work, env=env,
                                  capture_output=True, text=True)
        git("init", "-q")
        rng = random.Random(7)
        open(os.path.join(work, "small.txt"), "w").write("hello velvet\n")
        open(os.path.join(work, "big.txt"), "w").write("line\n" * 20000)
        with open(os.path.join(work, "noise.bin"), "wb") as f:
            f.write(rng.randbytes(30000))
        git("add", "-A")
        git("-c", "user.name=check", "-c", "user.email=c@x",
            "commit", "-q", "-m", "one")

        objects = 0
        for dirpath, _, names in os.walk(os.path.join(work, ".git", "objects")):
            for n in names:
                if len(os.path.basename(dirpath)) != 2:
                    continue
                path = os.path.join(dirpath, n)
                name = os.path.basename(dirpath) + n
                raw = open(path, "rb").read()
                got, rc, err = run("--inflate", raw)
                if rc != 0:
                    print("  git object %s: ours %s" % (name[:12], err))
                    bad += 1
                    continue
                if got != zlib.decompress(raw):
                    print("  git object %s: not what zlib makes of it"
                          % name[:12])
                    bad += 1
                    continue
                # and the header is the one 0.3.19 will have to parse
                if b"\0" not in got or got.split(b" ", 1)[0] not in \
                        (b"blob", b"tree", b"commit"):
                    print("  git object %s: unpacked to something with no "
                          "type in front of it" % name[:12])
                    bad += 1
                objects += 1
                checked += 1
        shutil.rmtree(work, ignore_errors=True)
        if objects == 0 and not bad:
            print("  the test repository had no loose objects in it")
            bad += 1

    if not bad:
        print("  %d streams agree with zlib, git objects included" % checked)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
