#!/usr/bin/env python3
"""the C build tools against the python ones they replace.

0.3.21 is the entry that is easy to miss and blocks everything: six of
this project's build tools are python, and a self-hosting machine has no
python and is not getting one. so each becomes a C program, and each
swap is a chance to change the build's output by accident.

so while both exist, both run, and the output is compared. that is the
same argument as every other check here: the old implementation is the
oracle, and it is the only thing that knows the details nobody wrote
down.

when a python tool goes away, its case here goes with it, and what is
left is a C tool checked against nothing, which is why the C ones are
introduced *beside* the python ones rather than instead of them.

usage:  tools/toolcheck.py
"""
import re, subprocess, sys, os, tempfile, random, shutil

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def run(args, **kw):
    return subprocess.run(args, capture_output=True, cwd=ROOT, **kw)


def main():
    """the checks, in a directory that is thrown away afterwards.

    that last part is not tidiness. two of these build whole filesystem
    images, two fat32 images of 64 MiB apiece, and two ext4 ones,
    and `make test` runs on every commit, so a run that leaves its
    working directory behind leaves a tenth of a gigabyte behind with
    it. a hundred runs later /tmp is full and what fails is something
    else entirely, a long way from here."""
    work = tempfile.mkdtemp()
    try:
        return check(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def check(work):
    ours = os.path.join(ROOT, "bin", "tests", "bin2c")
    if not os.path.exists(ours):
        print("  bin/tests/bin2c is not built")
        return 1

    bad = 0
    checked = 0
    rng = random.Random(20250821)

    cases = {
        "empty": b"",
        "one byte": b"\x00",
        # eleven, twelve and thirteen: twelve to a line, so the row
        # either side of the boundary is where a rewrite gets it wrong
        "eleven": bytes(range(11)),
        "twelve": bytes(range(12)),
        "thirteen": bytes(range(13)),
        "high bytes": bytes([0x80, 0xff, 0x7f, 0x00] * 5),
        "random": rng.randbytes(4000),
    }
    # and the real one, which is the only input that actually matters
    real = os.path.join(ROOT, "arch", "x86_64", "trampoline.asm")
    if os.path.exists(real):
        out = os.path.join(work, "trampoline.bin")
        if run(["nasm", "-f", "bin", real, "-o", out]).returncode == 0:
            cases["the trampoline"] = open(out, "rb").read()

    for name, data in cases.items():
        path = os.path.join(work, "case.bin")
        open(path, "wb").write(data)
        theirs = run(["python3", "tools/bin2c.py", "smp_trampoline", path])
        mine = run([ours, "smp_trampoline", path])
        if theirs.stdout != mine.stdout:
            print("  bin2c differs on %s" % name)
            t = theirs.stdout.decode().splitlines()
            m = mine.stdout.decode().splitlines()
            for i in range(max(len(t), len(m))):
                a = t[i] if i < len(t) else "(nothing)"
                b = m[i] if i < len(m) else "(nothing)"
                if a != b:
                    print("      python: %s" % a)
                    print("      ours:   %s" % b)
                    break
            bad += 1
        checked += 1

    ours = os.path.join(ROOT, "bin", "tests", "checkfmt")
    if os.path.exists(ours):
        # the kernel, named the way the makefile names it: the portable
        # list out of sources.mk, and arch/ beside it
        listed = re.search(r"^KERNEL_PORTABLE\s*:?=\s*(.*)$",
                           open(os.path.join(ROOT, "sources.mk")).read(),
                           re.M).group(1).split()
        trees = [("the kernel", listed + ["arch"])]
        # and a tree written to be wrong in every way the formatter can
        # be wrong, since a checker that agrees about a clean tree has
        # agreed about nothing
        bad_tree = os.path.join(work, "badfmt")
        os.makedirs(bad_tree, exist_ok=True)
        open(os.path.join(bad_tree, "a.c"), "w").write(
            'void f(void) {\n'
            '    kprintf("%5.2f\\n", x);\n'
            '    kprintf("%+d and %hhu\\n", a, b);\n'
            '    kprintf("%-8s %08lx %zu %% %p\\n", s, v, n, p);\n'
            '    panic("%e and %S", q, r);\n'
            '    asm volatile("mov %%rax, %%rbx");\n'
            '}\n')
        trees.append(("badfmt", [bad_tree]))

        for label, tree in trees:
            theirs = run(["python3", "tools/checkfmt.py"] + tree)
            mine = run([ours] + tree)
            if theirs.stderr != mine.stderr \
                    or theirs.returncode != mine.returncode:
                print("  checkfmt differs on %s" % label)
                t = theirs.stderr.decode().splitlines()
                m = mine.stderr.decode().splitlines()
                for i in range(max(len(t), len(m))):
                    a = t[i] if i < len(t) else "(nothing)"
                    b = m[i] if i < len(m) else "(nothing)"
                    if a != b:
                        print("      python: %s" % a)
                        print("      ours:   %s" % b)
                        break
                bad += 1
            checked += 1

    ours = os.path.join(ROOT, "bin", "tests", "gensyms")
    elf = os.path.join(ROOT, "bin", "velvet")
    if os.path.exists(ours) and os.path.exists(elf):
        for args in (["--stub"], [elf]):
            theirs = run(["python3", "tools/gensyms.py"] + args)
            mine = run([ours] + args)
            if theirs.stdout != mine.stdout:
                print("  gensyms differs on `%s`" % " ".join(args))
                t = theirs.stdout.decode().splitlines()
                m = mine.stdout.decode().splitlines()
                for i in range(max(len(t), len(m))):
                    a = t[i] if i < len(t) else "(nothing)"
                    b = m[i] if i < len(m) else "(nothing)"
                    if a != b:
                        print("      python: %s" % a)
                        print("      ours:   %s" % b)
                        break
                bad += 1
            checked += 1

        # and --check, both when the table is good and when it is stale.
        # the stale answer is the one that matters: a checker that only
        # agrees about success has agreed about nothing
        table = os.path.join(work, "ksyms.c")
        open(table, "wb").write(run([ours, elf]).stdout)
        for label, mangle in (("current", False), ("stale", True)):
            if mangle:
                text = open(table).read()
                at = text.index("{ 0x")
                text = text[:at + 6] + ("0" if text[at + 6] != "0" else "1") \
                    + text[at + 7:]
                open(table, "w").write(text)
            theirs = run(["python3", "tools/gensyms.py", "--check", elf, table])
            mine = run([ours, "--check", elf, table])
            if theirs.returncode != mine.returncode \
                    or theirs.stderr != mine.stderr:
                print("  gensyms --check differs on a %s table: "
                      "python rc=%d, ours rc=%d"
                      % (label, theirs.returncode, mine.returncode))
                bad += 1
            checked += 1

    ours = os.path.join(ROOT, "bin", "tests", "mkboot")
    # the source archive is one of the pieces and it is in
    # this list rather than left out of it because the fields that
    # describe it are the newest ones in the table, exactly the place
    # two implementations of the same layout drift apart
    pieces = [os.path.join(ROOT, p) for p in
              ("bin/boot/philemon.bin", "bin/boot/philemon64.bin",
               "bin/velvet", "bin/ramdisk.tar", "bin/source.tar")]
    if os.path.exists(ours) and all(os.path.exists(p) for p in pieces):
        theirs_img = os.path.join(work, "theirs.img")
        mine_img = os.path.join(work, "mine.img")
        run(["python3", "tools/mkboot.py", theirs_img] + pieces)
        run([ours, mine_img] + pieces)
        a = open(theirs_img, "rb").read() if os.path.exists(theirs_img) else b""
        b = open(mine_img, "rb").read() if os.path.exists(mine_img) else b""
        if a != b or not a:
            print("  mkboot: the images differ (%d bytes vs %d)"
                  % (len(a), len(b)))
            for i in range(min(len(a), len(b))):
                if a[i] != b[i]:
                    print("      first at 0x%x: python %02x, ours %02x"
                          % (i, a[i], b[i]))
                    break
            bad += 1
        checked += 1

        # and the refusal. the assembly and the C describe the same
        # memory in two files that cannot include each other, and a
        # number that drifts in one of them is a machine that hangs with
        # nothing on the screen, so the image must not get written
        tree = os.path.join(work, "drifted")
        os.makedirs(os.path.join(tree, "boot"), exist_ok=True)
        import shutil as sh
        for name in ("philemon.inc", "philemon.h", "philemon.asm"):
            sh.copy(os.path.join(ROOT, "boot", name),
                    os.path.join(tree, "boot", name))
        path = os.path.join(tree, "boot", "philemon.inc")
        text = open(path).read().replace("PT_BASE", "PT_BASE_UNUSED", 1)
        open(path, "w").write(text + "\nPT_BASE equ 0x9999\n")
        r = subprocess.run([ours, os.path.join(work, "no.img")] + pieces,
                           capture_output=True, cwd=ROOT,
                           env=dict(os.environ, MKBOOT_ROOT=tree))
        if r.returncode == 0 or b"in the assembly but" not in r.stderr:
            print("  mkboot wrote an image with the constants drifted apart")
            bad += 1
        checked += 1

    #
    # two formatters that allocate blocks in a different order produce
    # two perfectly good filesystems. demanding identical bytes would be
    # demanding identical decisions, which is the mistake this project
    # has avoided since readext4.py was written.
    #
    # so both images are handed to that reader, written from the
    # on-disk layout, separately from either formatter, and both have
    # to come back clean.
    ours = os.path.join(ROOT, "bin", "tests", "mkext4")
    tree = os.path.join(ROOT, "bin", "tests", "ext4root")
    if os.path.exists(ours) and os.path.isdir(tree):
        for who, cmd in (("python", ["python3", "tools/mkext4.py"]),
                         ("ours", [ours])):
            img = os.path.join(work, "%s.ext4" % who)
            r = run(cmd + [img, tree, "16"])
            if r.returncode != 0:
                print("  mkext4 (%s) would not build an image: %s"
                      % (who, r.stderr.decode(errors="replace").strip()[:160]))
                bad += 1
                continue
            r = run(["python3", "tools/readext4.py", img])
            if r.returncode != 0 or b"nothing wrong with it" not in r.stdout:
                print("  readext4 refuses the %s image: %s"
                      % (who, (r.stdout + r.stderr).decode(
                          errors="replace").strip()[:200]))
                bad += 1
            checked += 1

    #
    # the same argument as mkext4, with a second question on top of it.
    # this is the tool whose image the fat32 suite runs against, so it
    # is not enough that both images are good filesystems: they have to
    # be the same filesystem. two formatters that lay the clusters out
    # differently are both right; two that disagree about what a file is
    # *called* are not, and that is exactly what this swap risks, the
    # python wrote its own long-name entries and the C one goes through
    # fat32.c, which learned to write them for this version.
    #
    # so both are handed to tools/readfat.py, written apart from either,
    # and the trees it reads back out have to match line for line.
    ours = os.path.join(ROOT, "bin", "tests", "mkfat")
    tree = os.path.join(ROOT, "base/diskroot")
    if os.path.exists(ours) and os.path.isdir(tree):
        trees = {}
        for who, cmd in (("python", ["python3", "tools/mkfat.py"]),
                         ("ours", [ours])):
            img = os.path.join(work, "%s.fat32" % who)
            r = run(cmd + [img, tree, "64"])
            if r.returncode != 0:
                print("  mkfat (%s) would not build an image: %s"
                      % (who, r.stderr.decode(errors="replace").strip()[:160]))
                bad += 1
                continue
            # --fresh: a formatter is the one thing with no excuse for
            # leaving the free count wrong, since nothing has written to
            # the disk since it made it
            r = run(["python3", "tools/readfat.py", img, "--fresh", "--tree"])
            if r.returncode != 0 or b"nothing wrong with it" not in r.stdout:
                print("  readfat refuses the %s image: %s"
                      % (who, (r.stdout + r.stderr).decode(
                          errors="replace").strip()[:300]))
                bad += 1
                continue
            trees[who] = [line for line in r.stdout.decode().splitlines()
                          if line.startswith("  ")]
            checked += 1

        if len(trees) == 2:
            if trees["python"] != trees["ours"]:
                print("  mkfat: the two images do not hold the same tree")
                t, m = trees["python"], trees["ours"]
                for i in range(max(len(t), len(m))):
                    a = t[i] if i < len(t) else "(nothing)"
                    b = m[i] if i < len(m) else "(nothing)"
                    if a != b:
                        print("      python: %s" % a.strip())
                        print("      ours:   %s" % b.strip())
                        break
                bad += 1
            elif not trees["ours"]:
                print("  mkfat: both images are empty, which checks nothing")
                bad += 1
            checked += 1

    if not bad:
        print("  %d outputs identical to the python tools" % checked)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
