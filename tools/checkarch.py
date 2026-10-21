#!/usr/bin/env python3
"""does the arch boundary actually hold?

0.2.20 drew a line: everything under arch/x86_64/ assumes this
machine, and everything above it is not supposed to. a line like that is
worth exactly as much as whatever checks it, it decays the first time
somebody needs a `hlt` in a hurry, and it decays silently, because the
kernel goes on building and booting perfectly.

so this is the thing that notices. it fails the build on:

  * inline assembly outside arch/
  * an #include of an <asm/...> header that is not part of the
    contract, from outside arch/, unless the file is on the list
    below. the contract is what arch/none provides: a header every
    architecture has is portable to include, and one only x86 has is
    the machine by another name
  * an #include that names a directory under arch/ at all, which
    is reaching past the include path to one architecture

and it prints the list every time, because the list is the interesting
output. it is a record of what has not been done yet, and it is meant to
shrink, a version that adds a name to it should have said why.

the same idea as tools/checkfmt.py, and for the same reason: the bug it
prevents is one that hides.
"""

import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
KERNEL = ROOT
ARCH = os.path.join(KERNEL, "arch")

# files that may still name the architecture, and why.
#
# every one of these is a driver for a chip that is only ever found on a
# pc, or a file that reports on this hardware by name.
#
# the question of which are "an x86 driver" and which merely "happen to
# use port io" was answered for exactly one of them, and the answer came
# from writing a second architecture: drivers/serial.c was a terminal
# with a chip stuck to it, and the terminal half is off this list now.
#
# the rest are still here because that port was removed before it could
# say anything about them. the list is the record of what has not been
# asked yet, and it is meant to shrink.
#
# what it buys in the meantime is that the leak is *counted* rather than
# assumed, and see tools/portable.py, which is the half of this that
# does not grep, and which catches what grep cannot.
ALLOWED = {
    "drivers/pit.c":      "the 8254 timer, at ports 0x40-0x43 since 1981",
    "drivers/keyboard.c": "ps/2, which is an 8042 at port 0x60",
    "drivers/mouse.c":    "the same 8042, sharing the same port",
    "drivers/rtc.c":      "the cmos clock, at ports 0x70/0x71",
    "drivers/pci.c":      "configuration space through ports 0xcf8/0xcfc",
    "mm/vmm.c":           "the nx bit lives in an msr",
    "mm/addrspace.c":     "tlb shootdown is an inter-processor interrupt",
}

ASM = re.compile(r'\basm\s+volatile\b|\b__asm__\b|\basm\s*\(')
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)


def contract():
    """the headers every architecture provides, as the one that does
    nothing provides them. read from the tree, so a header added to the
    contract is portable the moment arch/none has it, and not before."""
    asm = os.path.join(ARCH, "none", "include", "asm")
    return {"asm/" + n for n in os.listdir(asm) if n.endswith(".h")}


def kernel_paths():
    """the portable half of the kernel, as sources.mk lists it.

    the root of this tree holds the bootloader, the host suites and the
    tools beside the kernel, and none of them are held to this rule: a
    suite that calls switch_context through inline asm is doing exactly
    what a suite should. so the walk is the build's list, read rather
    than copied."""
    text = open(os.path.join(ROOT, "sources.mk")).read()
    m = re.search(r"^KERNEL_PORTABLE\s*:?=\s*(.*)$", text, re.M)
    return m.group(1).split()


def sources():
    for entry in kernel_paths():
        top = os.path.join(KERNEL, entry)
        if os.path.isfile(top):
            if entry.endswith((".c", ".h")):
                yield top, entry
            continue
        for base, dirs, files in os.walk(top):
            dirs.sort()
            for name in sorted(files):
                if name.endswith((".c", ".h")):
                    full = os.path.join(base, name)
                    rel = os.path.relpath(full, KERNEL)
                    yield full, rel.replace(os.sep, "/")


def main():
    problems = []
    naming = {}
    portable = contract()

    for full, rel in sources():
        with open(full, encoding="utf-8", errors="replace") as f:
            text = f.read()

        # inline asm, anywhere outside arch/. no exceptions and no list:
        # this is the whole of what "stops naming registers" means, and a
        # single allowed one is a precedent rather than a line
        for n, line in enumerate(text.splitlines(), 1):
            code = line.split("/*")[0].split("//")[0]
            if ASM.search(code):
                problems.append(
                    "%s:%d: inline assembly outside arch/\n"
                    "    %s\n"
                    "    -> it belongs behind %s" % (
                        rel, n, line.strip(), ", ".join(
                            "<%s>" % h for h in sorted(portable))))

        named = sorted({h for h in INCLUDE.findall(text)
                        if (h.startswith("asm/") and h not in portable)
                        or h.startswith("arch/")})
        if named:
            naming[rel] = named

    for rel, headers in sorted(naming.items()):
        if rel not in ALLOWED:
            problems.append(
                "%s: includes %s from outside arch/\n"
                "    -> use a header the contract has, or add it to ALLOWED in "
                "tools/checkarch.py with a reason" % (rel, ", ".join(headers)))

    stale = sorted(set(ALLOWED) - set(naming))
    for rel in stale:
        problems.append(
            "%s: on the ALLOWED list in tools/checkarch.py and does not "
            "name the architecture any more\n"
            "    -> take it off the list. a list nobody prunes stops "
            "meaning anything" % rel)

    if problems:
        print("arch boundary: %d problem%s"
              % (len(problems), "" if len(problems) == 1 else "s"))
        for p in problems:
            print("  " + p)
        return 1

    clean = sum(1 for _ in sources()) - len(naming)
    print("  checkarch  ok        %d files portable, %d still name x86:"
          % (clean, len(naming)))
    for rel in sorted(naming):
        print("               %-20s %s" % (rel, ALLOWED[rel]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
