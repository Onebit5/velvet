contributing
============

velvet is a 64-bit kernel for x86_64, written in C and booted by a
bootloader of its own, and a machine built to be understood rather than
deployed. contributions are welcome, and they go through the same door as
everything else: an issue, an email, or a pull request on github that
builds, boots, and passes CI. this page is the door.

read `code style <coding-style.rst>`_ before you write anything. it is short
and it is not optional, for the same reason the compiler flags are not
optional: a change that works but does not fit is a change somebody else has
to fix.

what belongs here
-----------------

everything that runs in ring 0 or before it: the subsystems, the monitor,
philemon, the syscall layer, the host suites for any of those, and the
tools that build and check them. programs, a libc, a toolchain and what a
system puts on its medium are not here. they belong to the system built on
this kernel, and a change to them goes there.

a new syscall, or anything else a program can see, lands here with a
selftest that holds the kernel to it, and the pull request says what a
program does with it. a program can use it from the first release that
has it.

ways to contribute
------------------

there are three, and the first two are worth as much as the third.

**report a bug.** open an issue. a bug report is a contribution on its own,
and for a project this size it is often the most useful one. say:

- the version, from the boot banner or ``make kernelrelease``;
- what you did, what you expected, what happened;
- the serial log, if the machine got far enough to write one (``make
  boottest`` leaves one in ``bin/*-boottest.log``);
- the exact command that failed, if it was the build.

**propose a feature.** a new feature needs a conversation before it is a
patch. open an issue, or send an email to the maintainer of the subsystem it
belongs to, or to the project owner (``azpijr@gmail.com``), and describe what
you want to build and why. the plan is `ROADMAP.md <../ROADMAP.md>`_: every
version in it is a machine that boots, and each entry is the sentence that
version is about. work that maps to a roadmap entry is the work that is
wanted; work beside it needs the conversation, so nobody spends a weekend on
a thing that was already decided against. what is deliberately *not* here is
in the roadmap too. a patch that adds something the roadmap has named as a
non-goal gets a polite no, and it gets it faster if the issue comes first.

a feature that needs a name takes one from the Persona series, when one fits:
the velvet room, igor, margaret, theodore, lavenza, the arcana. it is a
flavour rather than a rule, and it is not needed after 1.0.0. the details are
in `code style <coding-style.rst>`_.

**send a patch.** a pull request against ``master``, on the terms below.

before you send anything
------------------------

get the tree building and testing on your machine, by hand, before CI ever
sees it:

.. code-block:: sh

   make            # the boot image, velvet.img
   make test       # the host suites; no qemu, about a second
   make boottest   # boot in qemu and drive the monitor over serial
   make kselftest  # run the selftests in ring 3, as guest

`building`_ has the toolchain and every target. if ``make test`` does not
pass on your machine before your change, fix that first; you cannot tell your
change's failures from the ones that were already there.

the workflow
------------

#. **fork the repository** (or branch, if you have write access) and branch
   from the **latest** ``master``. a pull request cut from an old
   ``master`` is a pull request against a tree that no longer exists, and
   it will be asked to rebase before it is read. name the branch for the
   subsystem and the change: ``mm/slab-merge-fix``, ``net/tcp-retransmit``.

#. **keep every commit buildable.** each commit in the series builds and
   boots on its own, so a reviewer can read them in order and a bisect lands
   on something real.

#. **a pull request that touches more than one subsystem splits its commits
   by subsystem.** one commit for the ``mm`` change, another for the ``fs``
   change, and so on. a reviewer reads by subsystem, and a change that crosses
   several of them in one commit is a change nobody can review.

#. **write the commit messages** as described below.

#. **run the checks locally**: ``make test``, ``make``, ``make boottest``,
   ``make kselftest``. the same commands CI runs.

#. **push and open the pull request** against ``master``. in the description,
   say what the change is, why it is needed, which subsystem it touches, and
   how you tested it. if it maps to a roadmap entry, name the entry. if it
   closes an issue, say so. a new feature must also say where its
   documentation is (see below).

#. **expect review.** comments are about the code, not the person, and
   "this is wrong because X" is a complete sentence. push follow-up commits
   for a while; a maintainer will say when to squash.

commit messages
---------------

there is one hard rule: the message must be correct English. beyond that,
write it the way it reads best to you; the subject and the body are both
free-form.

the only required lower case part is the subsystem token at the front of the
subject:

::

    subsystem: Summary of the change

    The body says what changed and why, which is the part the diff cannot
    show. Say what was wrong, what the fix assumes, and what breaks if that
    assumption stops holding. Reference the issue where there is one. Wrap
    at about 72 columns.

    Signed-off-by: Your Name <you@example.com>

the subsystem
   one word, matching the directory the change lives in and the vocabulary
   the project already uses: ``mm``, ``sched``, ``fs``, ``net``,
   ``drivers``, ``lib``, ``shell``, ``arch``, ``boot``, ``tools``,
   ``tests``, ``build``. a change that genuinely spans two gets
   the one it is *about*, or ``tree`` for the rare cross-cutting one. this
   token is the only part of the message that must be lower case.

the subject
   a summary, in normal English, of what the change does:
   ``mm: Fix slab merging across page boundaries``. no trailing period, and
   keep it under about 72 characters so it does not wrap in ``git log
   --oneline``.

the body
   say what changed and why, not how; the diff is the how. a typo fix can
   skip it. a one-line change to the allocator that took an afternoon to
   find the *reason* for cannot: the reason is the whole value of the
   commit. `CHANGELOG.md <../CHANGELOG.md>`_ is a hundred of these telling
   one story, and it is the model for how much context is worth writing
   down.

no versions
   a commit never names a version. the log records changes, and the numbers
   live in github releases rather than in the history. see `versioning
   <versioning.rst>`_.

the sign-off line
   every commit carries ``Signed-off-by:`` with your real name and a real
   address, as the `developer certificate of origin
   <https://developercertificate.org/>`_ describes it. it is your statement
   that you wrote the change or have the right to pass it on, and that you
   are fine with it living under this project's license. a pull request whose
   commits are missing it will be asked to amend. see below for how.

   .. code-block:: sh

      git commit -s        # adds the trailer from your git identity

   if the commits are already pushed, ``git rebase --signoff master`` and a
   force-push fixes the whole series at once.

the assisted-by trailer
   if a model wrote it, helped write it, or suggested it, the commit says so,
   in an ``Assisted-by:`` trailer naming the tool. this covers code, a test,
   a comment, a commit message, an argument made in review, and an idea that
   turned into a change. it is required rather than requested, and a patch
   without it that reads like nobody read it back is a patch that gets sent
   back. the rule, and the reason for it, are in `code style
   <coding-style.rst>`_.

replying to review
------------------

**answer the comment where it was written.** keep the conversation in the
pull request, so the reasoning stays searchable, and quote the line you are
answering when it is not obvious which one it is. there is no rule about where
the new words go: write the reply so it reads on its own.

everything else about review holds too:

- small, focused pull requests are reviewed quickly; a pull request that
  touches six subsystems is asked to split it.
- do not force-push over a reviewer's comments. push follow-ups until the
  conversation is done.
- once it is approved, the maintainer decides squash versus merge.

patches by email
----------------

a patch may also arrive as mail, sent to the project owner
(``azpijr@gmail.com``), which is how kernel projects have always taken them.
it is not the recommended route, and the reason is worth stating plainly:
with no mailing list there is no public archive, so a patch sent that way
lands in one inbox and nowhere else, and the discussion around it is a
discussion nobody can read later. it is here for the people who already work
this way, and it will be read.

**this path is for hardcore enthusiasts.** ``git send-email`` wants an smtp
account that will let you send, the plain-text and threading rules are real,
and the first setup costs an hour and usually fails once before it works. the
tools are written down below for anyone who wants them.

the rules that come with it:

- **top-post the reply.** the newest words go above the words being answered,
  because that is where they are read. mail is the one place in this project
  where that is the rule, and it is the rule because mail is read from the
  top.
- **a patch is a patch.** arriving by mail does not lower the standard it is
  held to: `code style <coding-style.rst>`_ applies, and so do ``make lint``,
  ``make test``, ``make boottest`` and ``make kselftest``.

if enough people want it, this becomes a real mailing list. `patchwork`_ would
track the patches, and `public-inbox`_ would keep an archive that is
searchable and cloneable the way a kernel list is. that needs an audience
first, so it is a maybe rather than a plan, and nothing in this tree needs it
today.

the tools
~~~~~~~~~

``git send-email`` ships with git, though some distributions package it
separately. point it at an smtp account once:

.. code-block:: sh

   git config --global sendemail.smtpserver smtp.example.com
   git config --global sendemail.smtpserverport 587
   git config --global sendemail.smtpencryption tls
   git config --global sendemail.smtpuser you@example.com
   git config --global sendemail.from "Your Name <you@example.com>"

then write the patches and send them:

.. code-block:: sh

   git format-patch --cover-letter -o /tmp/patches origin/master
   git send-email --to azpijr@gmail.com /tmp/patches

if you use a mail client other than ``git send-email``: plain text only, no
html, and the patch in the body rather than attached, because that is what
``git am`` reads. on the receiving side it arrives with ``git am``, and a
long thread can be fetched and applied by message id with `b4`_:

.. code-block:: sh

   git am < 0001-sched-hold-the-outgoing-thread.patch
   b4 am <message-id>

.. _patchwork: https://patchwork.readthedocs.io/
.. _public-inbox: https://public-inbox.org/
.. _b4: https://b4.docs.kernel.org/

CI is the gate
--------------

**a pull request that does not pass CI is not merged, no matter how good the
change is.** this is not a formality and it is not negotiable. a change that
breaks the build or the boot is a defect in its own right, exactly like an
off-by-one would be. "but it fixes something great" does not make the tree
boot again, and a tree that does not boot cannot measure how great the fix
was.

concretely: a red build is not "fixed later". there is no merge-then-fix, no
maintainer override, and no branch that lands because the diff is obviously
correct. if CI is failing, the pull request is not ready to be reviewed as
finished, because the first thing a reviewer would have to do is the thing CI
already did and could not get through.

if a check is failing for a reason that is not your change's fault (a flaky
test, a runner that ran out of disk, a dependency that moved), say so in the
pull request with the failing log, and fix the flake in your branch or in a
pull request of its own. do not work around it, and do not ask for the check
to be ignored. a project whose CI can be waived has no CI.

what CI runs
------------

the workflow is `.github/workflows/ci.yml <../.github/workflows/ci.yml>`_ and
it runs on every push and every pull request. there are two jobs. the first
is ``lint`` on its own, because it needs no toolchain and no build: it is the
fastest answer a pull request can get, and it should be the one that comes
back first.

the second job, ``build-and-test``, installs the toolchain and then, in order:

``make test``
   the host suites, the real kernel sources compiled as ordinary linux
   programs, plus ``checkfmt``, ``checkarch``, the portable build and the
   checks against other implementations. this is fast, it needs no qemu,
   and it catches most of what the rest would.

``make``
   the kernel and the boot image, with the machine's gcc, nasm and ld.

``make boottest``
   boot the image headless in qemu, tell its ``/init`` to run nothing, and
   type commands at the kernel's monitor over the serial port, checking the
   answers.

``make kselftest``
   boot it again and have its ``/init`` run the selftests in ring 3, as guest.
   a selftest that fails is a promise to programs broken, and the change
   that broke it is what gets fixed: the kernel does not break userspace.
   `kernel selftests <dev-tools/kselftest.rst>`_ has the rest.

what this CI cannot see is the programs a system ships on top of the
kernel. a change here that breaks one of them where no selftest looked is
still a bug here, even when the failure is reported somewhere else: the
selftests are the promise written down, not the whole of it.

`testing`_ is the reference for all of these, including how to run a single
suite and what a failure looks like.

tests come with the change
--------------------------

a behaviour change arrives with the suite that proves it. this is not a
policy about coverage percentages; it is that a fix without a test is a fix
that will be undone by the next person who does not know why it was there,
and this project has already paid for that lesson more than once.

- a new subsystem gets ``tests/test_<subsystem>.c`` and a line in the
  makefile's suite list.
- a bug fix gets the case that reproduces the bug, added to the suite that
  should have caught it. if there is no such suite, that is the finding.
- the test asserts on observed behaviour, not on the implementation. where
  somebody else's implementation exists, test against that:
  ``tests/test_epoch.c`` against the host's ``timegm``, and
  ``tests/test_hash.c`` against python, zlib and git through
  ``tools/hashcheck.py``.

documentation
-------------

a new feature must come with its own documentation, in the right place. if it
extends a subsystem, extend that subsystem's page under
`docs/subsystems/ <subsystems/index.rst>`_; if it is a new thing, give it a
page of its own and link it from `docs/index.rst <index.rst>`_. `code style
<coding-style.rst>`_ has the rules for the writing itself. a feature without
documentation is an unfinished feature, and a change to a subsystem's public
interface that leaves its page describing the old one is incomplete.

license and the DCO
-------------------

velvet is GPL-2.0-only. see `LICENSE <../LICENSE>`_. by contributing you
agree your work is licensed under the same terms, and the ``Signed-off-by``
trailer is that agreement in the log. there is no exception: the console
font is linux's, which is GPL-2.0 as well.

that license is copyleft, which means a contribution stays under it: the
copyright stays yours, but the grant does not come back out of the tree. if
that is a problem for you or for whoever you work for, say so before writing
the patch rather than after it is merged.

third-party code is not taken into this tree. a new dependency is a
conversation before it is a commit, with a one-line answer to *why not
hand-rolled, what it buys, and under what license*.

.. _building: building.rst
.. _testing: testing.rst
