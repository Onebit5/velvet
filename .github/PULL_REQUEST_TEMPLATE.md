<!-- thanks for sending a patch. the guide is docs/contributing.rst; this is
     the checklist it asks for, so a reviewer does not have to ask. -->

## what this changes

<!-- a paragraph on what it does and why. the diff already shows how. -->

## subsystem

<!-- arch, boot, mm, sched, fs, drivers, net, lib, shell, tools, docs -->

## checklist

- [ ] branched from the latest `master`
- [ ] one change, and one subsystem, per commit
- [ ] `make lint` passes
- [ ] `make test` passes
- [ ] `make boottest` passes, if it changes what the machine does
- [ ] `make kselftest` passes, and a bug a program can see comes with a
      selftest that fails before the fix
- [ ] if it changes the syscalls or anything else a program sees, what a
      program does about it is described above
- [ ] a new feature brings its documentation in `docs/`
- [ ] signed off with `git commit -s`
- [ ] any model assistance disclosed with an `Assisted-by:` trailer

## notes for review

<!-- what a reviewer should look at first, or a question you want answered.
     if you are fixing an issue, quote the number: `fixes #12`. -->
