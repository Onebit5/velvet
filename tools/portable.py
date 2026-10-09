#!/usr/bin/env python3
"""compile the portable kernel against an architecture that does nothing.

tools/checkarch.py greps. it can see an #include of an x86 header and a
line of inline assembly, and it cannot see a portable file that quietly
*depends* on something only x86 supplies, a constant, a struct layout,
a function it happens to link against. that is the difference between a
boundary nobody has crossed lately and a boundary that holds.

so this compiles the portable half of the kernel against
arch/none/, where every arch function is empty and every arch
constant is a plausible-looking lie. if it compiles, nothing above the
line needs a machine. if it does not, the compiler says which file and
which line, which is a better answer than any list I could keep by hand.

then it links what it compiled and collects the *undefined* symbols. that
list is the porting checklist, worked out mechanically rather than by
somebody trying to remember what an architecture owes the kernel, and
it is why `make portable-check` is worth having beyond this one version:
an architecture is finished when it defines everything on the list, which
is a question with an answer.

usage:  tools/portable.py [--list]
"""

import os
import re
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
KERNEL = ROOT
OUT = os.path.join(ROOT, "obj", "portable")

CC = os.environ.get("HOSTCC", "gcc")

# the same freestanding flags the kernel is built with, minus the ones
# that name x86. that subtraction is itself part of the experiment: what
# is left has to be enough
CFLAGS = [
    "-std=gnu11", "-Wall", "-Wextra", "-g",
    "-ffreestanding", "-fno-stack-protector", "-fno-stack-check",
    "-fno-PIC", "-fno-omit-frame-pointer",
    "-DVELVETOS_ARCH_NONE",
    # the contract in place of an architecture, by include path the same
    # way the real build picks x86_64, and the version the build writes
    "-I" + os.path.join(KERNEL, "include"),
    "-I" + os.path.join(KERNEL, "arch", "none", "include"),
    "-I" + os.path.join(ROOT, "obj", "include"),
    # -O0 to match the real build, since that is where a `static inline`
    # is a call and the difference has bitten once already
    "-O0",
]


def allowed_to_name_x86():
    """the list checkarch.py keeps, read rather than duplicated.

    two lists of the same files, maintained by hand in two places, would
    disagree within a version, and the disagreement would be silent,
    which is the failure mode this whole file exists to avoid."""
    src = open(os.path.join(ROOT, "tools", "checkarch.py")).read()
    body = src[src.index("ALLOWED = {"):src.index("}", src.index("ALLOWED = {"))]
    return set(re.findall(r'"([^"]+\.c)":', body))


def kernel_paths():
    """the portable half, as sources.mk lists it: the same list the
    build compiles and checkarch.py walks, so the three cannot disagree
    about what the kernel is."""
    text = open(os.path.join(ROOT, "sources.mk")).read()
    m = re.search(r"^KERNEL_PORTABLE\s*:?=\s*(.*)$", text, re.M)
    return m.group(1).split()


def portable_sources():
    skip = allowed_to_name_x86()
    for entry in kernel_paths():
        top = os.path.join(KERNEL, entry)
        if os.path.isfile(top):
            found = [top]
        else:
            found = []
            for base, dirs, files in os.walk(top):
                dirs.sort()
                found += [os.path.join(base, n) for n in sorted(files)]
        for full in found:
            if not full.endswith(".c"):
                continue
            rel = os.path.relpath(full, KERNEL).replace(os.sep, "/")
            if rel not in skip:
                yield full, rel


def main():
    listing = "--list" in sys.argv

    os.makedirs(OUT, exist_ok=True)
    objects, failures = [], []

    for full, rel in portable_sources():
        obj = os.path.join(OUT, rel.replace("/", "_") + ".o")
        r = subprocess.run(CC.split() + CFLAGS + ["-c", full, "-o", obj],
                           capture_output=True, text=True)
        if r.returncode != 0:
            failures.append((rel, r.stderr.strip()))
        else:
            objects.append((rel, obj))

    if failures:
        print("portable-check: %d file%s could not be built without a machine"
              % (len(failures), "" if len(failures) == 1 else "s"))
        for rel, err in failures:
            print("\n  --- %s" % rel)
            for line in err.splitlines()[:12]:
                print("      " + line)
        print("\n  -> either it wants something that belongs behind arch/,")
        print("     or arch/none/ is missing part of the contract.")
        return 1

    # what the portable half defines for itself, and what it reaches for
    defined, wanted = set(), {}
    for rel, obj in objects:
        out = subprocess.run(["nm", obj], capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == "U":
                wanted.setdefault(parts[1], []).append(rel)
            elif len(parts) == 3 and parts[1] not in "Uw":
                defined.add(parts[2])

    missing = {s: f for s, f in wanted.items()
               if s not in defined and not s.startswith("__")}

    # two very different kinds of missing, and reporting one number for
    # both would be the least useful thing this could say.
    #
    # the contract is what an *architecture* owes the kernel: the names
    # arch/none declares and does not define. everything else is a
    # symbol that lives in one of the ten files still allowed to name
    # x86, not a port's problem so much as the boundary's remaining
    # debt, and a number worth watching go down.
    contract = set()
    asm = os.path.join(KERNEL, "arch", "none", "include", "asm")
    for name in sorted(os.listdir(asm)):
        if not name.endswith(".h"):
            continue
        text = open(os.path.join(asm, name)).read()
        for decl in re.finditer(
                r'^(?:extern\s+)?[a-z_0-9]+\s+\**([a-z_0-9]+)\s*\([^)]*\)\s*(?:__attribute__[^;]*)?;',
                text, re.M):
            contract.add(decl.group(1))

    owed = sorted(s for s in missing if s in contract)
    debt = sorted(s for s in missing if s not in contract)

    print("  portable   ok        %d files build with no architecture at all"
          % len(objects))
    print("               %d symbols owed by an architecture, "
          "%d by the files that still name x86" % (len(owed), len(debt)))

    if listing:
        print("\n    what a port must supply:")
        for sym in owed:
            users = sorted(set(missing[sym]))
            shown = ", ".join(users[:3]) + ("..." if len(users) > 3 else "")
            print("      %-24s %s" % (sym, shown))
        print("\n    and what it drags along, from the ten files that still")
        print("    name x86, this list is the boundary's remaining debt:")
        for sym in debt:
            users = sorted(set(missing[sym]))
            shown = ", ".join(users[:2]) + ("..." if len(users) > 2 else "")
            print("      %-24s %s" % (sym, shown))
    return 0


if __name__ == "__main__":
    sys.exit(main())
