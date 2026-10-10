# contributing

velvet is a 64-bit kernel for x86_64, written in C and booted by a bootloader
of its own, and the kernel of [velvetOS](https://github.com/Onebit5/velvetOS).
contributions are welcome, and they do not have to be code: a bug report that
reproduces something the machine gets wrong is worth as much as a patch, and so is
the test that catches it next time. the fastest way in is to build it, boot it, and
find the moment where what happens is not what should.

**first, the right repository.** this one is for what runs in ring 0 or before it:
the subsystems, the shell, philemon, the syscall layer, and the suites and tools
for those. the programs, the libc, the toolchain and git are velvetOS's. a change
that needs both, a new syscall being the usual one, lands here first and in velvetOS
once it pins a kernel that has it.

the full guide is [`docs/contributing.rst`](docs/contributing.rst). it covers
which repository a change goes to, the three ways to contribute, what a pull request
has to do, the commit message format, the sign-off, how review works, and one rule
that outranks the rest: **ci is the gate**. a change that does not pass it is not
merged, however good it is, because a tree that does not build is itself the bug.
read [`docs/coding-style.rst`](docs/coding-style.rst) before writing code, and run
`make lint` before you push.

in short: open an issue for a bug. for a feature, open an issue or send an
email to the maintainer of the subsystem it touches, after reading
[ROADMAP.md](ROADMAP.md) and the non-goals it lists. branch from the latest
`master`, keep one subsystem per commit, sign off with `git commit -s`, and make
`make test` and `make boottest` pass on your own machine before ci ever sees them.

velvet is GPL-2.0-only, and a contribution is accepted under the same terms:
the `Signed-off-by` trailer is that agreement in the log, and there is no
exception: even the console font, which is linux's, is GPL-2.0.

a patch may also arrive as mail, which is how kernel projects have always taken
them. it is not the recommended route: with no mailing list there is no public
archive, so the discussion would live in one inbox. it is there for the people
who already work that way, and [`docs/contributing.rst`](docs/contributing.rst)
has the `git send-email` recipe for anyone who wants it.
