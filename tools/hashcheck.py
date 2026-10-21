#!/usr/bin/env python3
"""crc32, against somebody else's.

a published check value looks like it ought to be enough. it is not. it
proves the algorithm was copied correctly for one buffer summed in one
go, which is the case that cannot fail, because it never exercises the
part where the input arrives in pieces and the running value has to be
carried from one to the next.

so this feeds three hundred and something lengths of pseudo-random bytes
through ours and through zlib's, and then feeds every one of them again
in chunks of 1, 7, 63, 64 and 65 bytes, through crc32_more, which is
how a gpt's header and its entries are summed.

usage:  tools/hashcheck.py
"""
import subprocess, sys, os, random, zlib

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
    lengths += [65535, 65536, 65537]

    bad = 0
    checked = 0

    for n in lengths:
        data = bytes(rng.randrange(256) for _ in range(n)) if n < 2000 \
            else rng.randbytes(n)

        want = "%08x" % zlib.crc32(data)
        got = ours("--crc32", data)
        if got != want:
            print("  crc32 of %d bytes: ours %s, zlib %s" % (n, got, want))
            bad += 1

        checked += 1

        # and the same bytes arriving in pieces, which is where a running
        # value that is not carried right agrees with itself and nobody
        # else
        for chunk in (1, 7, 63, 64, 65):
            if n == 0 and chunk != 1:
                continue
            want = "%08x" % zlib.crc32(data)
            got = ours("--crc32-chunked=%d" % chunk, data)
            if got != want:
                print("  crc32 of %d bytes in %d-byte pieces: ours %s, "
                      "zlib %s" % (n, chunk, got, want))
                bad += 1
            checked += 1

        if bad:
            break

    if not bad:
        print("  %d answers agree with zlib" % checked)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
