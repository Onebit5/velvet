#!/usr/bin/env python3
"""boot the image and run every selftest on it. each test prints TAP, and
this prints one TAP stream of the results, with each test's own output
under it as # lines.

a selftest is a program, so it is run where a program is run: at the
shell, logged in as guest, the account with the least the kernel will do
for it. a promise the kernel keeps to a guest it keeps to everybody.

a test that never comes back is cut off after --timeout seconds, and a
test that takes the kernel down ends the run, because there is no kernel
left to run the rest on. the whole serial log is kept in
bin/<image>-kselftest.log either way.

  usage: run_kselftest.py [--timeout SECONDS] IMAGE TEST...

a TEST is its path under tools/testing/selftests without the .c, which
is also where it is on the medium, under selftests/.
"""

import os
import re
import subprocess
import sys
import threading
import time

PROMPT = re.compile(rb"guest@velvet\[\d+\]:\S*\$ ")
PANIC = re.compile(rb"KERNEL PANIC")
TAP = re.compile(r"^(TAP version \d+|\d+\.\.\d+|(not )?ok \d+.*|#( .*)?)$")
# what is worth repeating from a panic: what happened and where
PANIC_LINES = re.compile(r"cpu exception|page fault at|double fault|"
                         r"KERNEL PANIC|^in thread")


class Machine:
    """qemu with the serial port on a pipe, and what it has said so far."""

    def __init__(self, image, log):
        cmd = ["qemu-system-x86_64", "-M", "q35", "-m", "1G",
               "-smp", os.environ.get("CPUS", "2"), "-boot", "order=c",
               "-drive", "id=boot,file=%s,format=raw,if=none" % image,
               "-device", "ide-hd,drive=boot,bus=ide.0,bootindex=0",
               "-display", "none", "-serial", "stdio", "-no-reboot"]
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
        self.qemu.stdin.write(text.encode() + b"\r")
        self.qemu.stdin.flush()

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
    if len(args) >= 2 and args[0] == "--timeout":
        timeout = int(args[1])
        args = args[2:]
    if len(args) < 1:
        sys.exit("usage: run_kselftest.py [--timeout SECONDS] IMAGE TEST...")
    image, tests = args[0], args[1:]

    os.makedirs("bin", exist_ok=True)
    log = "bin/%s-kselftest.log" % os.path.splitext(os.path.basename(image))[0]
    m = Machine(image, log)

    print("TAP version 13")
    print("1..%d" % len(tests))
    print("# timeout set to %d" % timeout)

    def login():
        at = m.expect(re.compile(rb"name the guest: "), 0, 120)
        if not isinstance(at, int):
            return at
        m.type("guest")
        at = m.expect(re.compile(rb"and the word: "), at, 10)
        if not isinstance(at, int):
            return at
        m.type("guest")
        return m.expect(PROMPT, at, 10)

    at = login()
    passed = failed = 0
    gone = None if isinstance(at, int) else "the machine never got to a prompt"
    for n, test in enumerate(tests, 1):
        name = "selftests: %s" % test.replace("/", ": ")
        print("# %s" % name)
        if gone:
            print("not ok %d %s # not run: %s" % (n, name, gone))
            failed += 1
            continue
        start = m.mark()
        m.type("run selftests/%s" % test)
        end = m.expect(PROMPT, start, timeout)
        output = lines(m.since(start))
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
        tap, why = judge(output)
        for l in tap:
            print("# " + l)
        if end is None:
            gone = "%s never came back" % test
            print("not ok %d %s # TIMEOUT %d seconds" % (n, name, timeout))
            failed += 1
        elif why:
            print("not ok %d %s # %s" % (n, name, why))
            failed += 1
        else:
            print("ok %d %s" % (n, name))
            passed += 1

    m.stop()
    print("# Totals: pass:%d fail:%d" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
