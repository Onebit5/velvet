#!/usr/bin/env python3
"""boot the image and run every selftest on it. each test prints TAP, and
this prints one TAP stream of the results, with each test's own output
under it as # lines.

the image's /init asks which tests to run, and is told once, with all of
them on one line. it runs each as a program, as guest, the account with
the least the kernel will do for it, in front of the console, and says
on the serial port when each starts and how it ended. a promise the
kernel keeps to a guest it keeps to everybody. after the last it powers
the machine off.

a test that never comes back is killed by /init after --timeout seconds
and the rest still run. a test that takes the kernel down ends the run,
because there is no kernel left to run the rest on. the whole serial log
is kept in bin/<image>-kselftest.log either way.

a --disk is a second drive. the machine gets a copy of it, made fresh
for each run, so what a test writes is gone afterwards and every run
starts from the same disk.

  usage: run_kselftest.py [--timeout SECONDS] [--disk DISK] IMAGE TEST...

a TEST is its path under tools/testing/selftests without the .c, which
is also where it is on the medium, under selftests/.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

QUESTION = re.compile(rb"run which selftests\? ")
NOT_THERE = re.compile(rb"init: there is no selftests/(\S+)")
PANIC = re.compile(rb"KERNEL PANIC")
TAP = re.compile(r"^(TAP version \d+|\d+\.\.\d+|(not )?ok \d+.*|#( .*)?)$")
# what is worth repeating from a panic: what happened and where
PANIC_LINES = re.compile(r"cpu exception|page fault at|double fault|"
                         r"KERNEL PANIC|^in thread")


class Machine:
    """qemu with the serial port on a pipe, and what it has said so far."""

    def __init__(self, image, log, disk=None):
        cmd = ["qemu-system-x86_64", "-M", "q35", "-m", "1G",
               "-smp", os.environ.get("CPUS", "2"), "-boot", "order=c",
               "-drive", "id=boot,file=%s,format=raw,if=none" % image,
               "-device", "ide-hd,drive=boot,bus=ide.0,bootindex=0",
               "-display", "none", "-serial", "stdio", "-no-reboot"]
        if disk:
            cmd += ["-drive", "id=data,file=%s,format=raw,if=none" % disk,
                    "-device", "ide-hd,drive=data,bus=ide.1"]
        self.out = bytearray()
        self.lock = threading.Lock()
        self.log = open(log, "wb")
        self.qemu = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        threading.Thread(target=self.listen, daemon=True).start()

    def listen(self):
        while True:
            chunk = os.read(self.qemu.stdout.fileno(), 4096)
            if not chunk:
                return
            with self.lock:
                self.out += chunk
            self.log.write(chunk)
            self.log.flush()

    def mark(self):
        with self.lock:
            return len(self.out)

    def since(self, at):
        with self.lock:
            return bytes(self.out[at:])

    def type(self, text):
        """a line, a few keys at a time: the kernel keeps 256 keys for a
        reader, and a long line sent at once can outrun one"""
        keys = text.encode() + b"\r"
        for at in range(0, len(keys), 32):
            self.qemu.stdin.write(keys[at:at + 32])
            self.qemu.stdin.flush()
            time.sleep(0.05)

    def expect(self, pattern, at, seconds):
        """wait for pattern after `at`. returns where it ended, or None on
        a timeout, or "panic" when the kernel went down first."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            seen = self.since(at)
            m = pattern.search(seen)
            if m:
                return at + m.end()
            if PANIC.search(seen):
                return "panic"
            if self.qemu.poll() is not None:
                return None
            time.sleep(0.1)
        return None

    def stop(self):
        self.qemu.kill()
        self.qemu.wait()
        self.log.close()


def lines(raw):
    text = raw.decode("utf-8", "replace").replace("\r", "")
    return [l.rstrip() for l in text.split("\n")]


def judge(output):
    """pass, or why not, from the TAP the test printed."""
    tap = [l for l in output if TAP.match(l)]
    plan = [l for l in tap if re.match(r"^\d+\.\.\d+$", l)]
    if not plan:
        return tap, "no plan: the test did not get far enough to make one"
    planned = int(plan[0].split("..")[1])
    passed = len([l for l in tap if re.match(r"^ok \d+", l)])
    failed = len([l for l in tap if re.match(r"^not ok \d+", l)])
    if not any(l.startswith("# Totals:") for l in tap):
        return tap, "no totals: the test did not finish"
    if failed or passed != planned:
        return tap, "%d of %d passed" % (passed, planned)
    return tap, ""


def main(argv):
    args = argv[1:]
    timeout = 30
    disk = None
    while len(args) >= 2 and args[0] in ("--timeout", "--disk"):
        if args[0] == "--timeout":
            timeout = int(args[1])
        else:
            disk = args[1]
        args = args[2:]
    if len(args) < 1:
        sys.exit("usage: run_kselftest.py [--timeout SECONDS] [--disk DISK] "
                 "IMAGE TEST...")
    image, tests = args[0], args[1:]

    os.makedirs("bin", exist_ok=True)
    log = "bin/%s-kselftest.log" % os.path.splitext(os.path.basename(image))[0]
    scratch = tempfile.mkdtemp(prefix="velvet-kselftest-")
    copy = None
    if disk:
        copy = os.path.join(scratch, "disk.img")
        shutil.copyfile(disk, copy)
    try:
        return run(image, tests, timeout, log, copy)
    finally:
        shutil.rmtree(scratch)


def run(image, tests, timeout, log, disk):
    print("TAP version 13")
    print("1..%d" % len(tests))
    print("# timeout set to %d" % timeout)
    if not tests:
        print("# Totals: pass:0 fail:0")
        return 0

    m = Machine(image, log, disk)
    at = m.expect(QUESTION, 0, 120)
    gone = None
    if isinstance(at, int):
        m.type("timeout=%d %s" % (timeout, " ".join(tests)))
    elif at == "panic":
        gone = "the kernel panicked before /init asked"
    else:
        gone = "the machine never got to /init's question"

    passed = failed = 0
    for n, test in enumerate(tests, 1):
        name = "selftests: %s" % test.replace("/", ": ")
        print("# %s" % name)
        if gone:
            print("not ok %d %s # not run: %s" % (n, name, gone))
            failed += 1
            continue

        mark = re.escape(test.encode())
        start = m.expect(re.compile(rb"init: selftests/" + mark +
                                    rb" starts\r?\n"), at, 30)
        if not isinstance(start, int):
            missing = NOT_THERE.search(m.since(at))
            gone = ("the image has no selftests/%s" % missing.group(1).decode()
                    if missing else "/init never started %s" % test)
            print("not ok %d %s # not run: %s" % (n, name, gone))
            failed += 1
            continue

        ended = re.compile(rb"init: selftests/" + mark +
                           rb" (?:ended with (-?\d+)|ran out of)")
        # /init kills a test at the timeout; this is for an /init that
        # never says so
        end = m.expect(ended, start, timeout + 15)
        if end == "panic":
            time.sleep(1)       # the rest of the panic, for the record
            output = lines(m.since(start))
            tap, why = judge(output)
            for l in tap:
                print("# " + l)
            for l in output:
                if PANIC_LINES.search(l):
                    print("# " + l)
            gone = "the kernel panicked during %s" % test
            print("not ok %d %s # kernel panic" % (n, name))
            failed += 1
            continue
        if end is None:
            for l in judge(lines(m.since(start)))[0]:
                print("# " + l)
            gone = "%s never came back" % test
            print("not ok %d %s # not run: /init never said how %s ended"
                  % (n, name, test))
            failed += 1
            continue

        told = ended.search(m.since(start))
        output = lines(m.since(start)[:told.start()])
        tap, why = judge(output)
        for l in tap:
            print("# " + l)
        at = end
        if told.group(1) is None:
            print("not ok %d %s # TIMEOUT %d seconds" % (n, name, timeout))
            failed += 1
        elif why or told.group(1) != b"0":
            print("not ok %d %s # %s" % (n, name, why or "exited with %s"
                                         % told.group(1).decode()))
            failed += 1
        else:
            print("ok %d %s" % (n, name))
            passed += 1

    # /init powers the machine off after the last, which ends qemu
    try:
        m.qemu.wait(timeout=60)
    except subprocess.TimeoutExpired:
        pass
    m.stop()
    print("# Totals: pass:%d fail:%d" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
