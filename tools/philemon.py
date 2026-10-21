#!/usr/bin/env python3
"""the loader the kernel's image boots with: fetch it.

velvet does not carry a bootloader. philemon-release.mk names one release
of philemon by its version, and this clones the tag for that version,
vX.Y.Z, into the directory it is given, which the makefile keeps in
obj/philemon/. a released tag is never moved, so the version is all a
build needs to know which loader it boots with. a clone that is already
at that tag is used as it is, and the network is asked only when the
version changes.

  philemon.py fetch DIR [URL VERSION]   make DIR the release named
  philemon.py local DIR                 check DIR is a philemon to build

missing values are philemon-release.mk's. `make PHILEMON_URL=../philemon`
clones the same release out of a local copy, which has to have the tag,
and `make PHILEMON_SRC=../philemon` builds that checkout as it is, which
is how the loader and the kernel are worked on at once.
"""

import os
import re
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PIN = os.path.join(ROOT, "philemon-release.mk")
STABLE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def say(msg):
    print("  " + msg, file=sys.stderr)


def die(msg):
    print("philemon.py: " + msg, file=sys.stderr)
    sys.exit(1)


def git(*args):
    r = subprocess.run(("git",) + args, capture_output=True, text=True)
    if r.returncode != 0:
        die("git %s: %s" % (" ".join(args), (r.stderr or r.stdout).strip()))
    return r.stdout.strip()


def pinned():
    values = {}
    for line in open(PIN):
        m = re.match(r"^(PHILEMON_[A-Z]+)\s*[?:]?=\s*(\S+)", line)
        if m:
            values[m.group(1)] = m.group(2)
    return values


def resolve(d, rev):
    """the commit rev names in the checkout d, or None."""
    if not os.path.exists(os.path.join(d, ".git")):
        return None
    r = subprocess.run(["git", "-C", d, "rev-parse", "-q", "--verify",
                        rev + "^{commit}"], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def fetch(d, url, version):
    if not STABLE.match(version):
        die("%s is not a release: philemon-release.mk names one as X.Y.Z"
            % version)
    tag = "v" + version
    at = resolve(d, "HEAD")
    if at is not None and at == resolve(d, tag):
        return
    if at is not None:
        if git("-C", d, "status", "--porcelain"):
            die("%s is at %s and has changes in it, and philemon-release.mk "
                "says %s. it is not replaced while it holds work: move the "
                "work to a checkout of philemon and build that with "
                "PHILEMON_SRC" % (d, at[:12], tag))
    elif os.path.isdir(d) and os.listdir(d):
        die("%s has files in it and is not a checkout. move it out of "
            "the way and the release is fetched in its place" % d)
    else:
        git("init", "-q", d)
    say("LOADER  philemon %s, from %s" % (tag, url))
    # the fetch runs inside the clone, so a local path given relative to
    # this tree has to be made absolute first, or ../philemon means the
    # clone itself
    if os.path.exists(url):
        url = os.path.abspath(url)
    git("-C", d, "fetch", "-q", "--depth", "1", url,
        "+refs/tags/%s:refs/tags/%s" % (tag, tag))
    git("-C", d, "-c", "advice.detachedHead=false", "checkout", "-q",
        "--detach", tag)


def local(d):
    """a checkout somebody is working in, used as it is. the one check is
    that it is philemon at all, since a typo here would otherwise surface
    as a missing file a long way into the build."""
    if not os.path.isfile(os.path.join(d, "src", "philemon.asm")):
        die("%s is not a checkout of philemon: there is no "
            "src/philemon.asm in it" % d)
    at = resolve(d, "HEAD")
    dirty = at is not None and git("-C", d, "status", "--porcelain")
    say("LOADER  %s as it is%s%s" % (d, ", at " + at[:12] if at else "",
                                      " with changes" if dirty else ""))


def main():
    args = sys.argv[1:]
    if len(args) < 2 or args[0] not in ("fetch", "local"):
        sys.exit(__doc__)
    if args[0] == "local":
        local(args[1])
        return
    pin = pinned()
    url = args[2] if len(args) > 2 and args[2] else pin["PHILEMON_URL"]
    version = args[3] if len(args) > 3 and args[3] else \
        pin["PHILEMON_VERSION"]
    fetch(args[1], url, version)


if __name__ == "__main__":
    main()
