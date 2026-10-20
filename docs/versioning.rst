releases
========

velvet does not put version numbers in commits. the log is a record of
changes, not a sequence of numbered drops: a commit says what it changed and
why, and nothing about which release it will end up in. the numbers live in
**github releases**, and a release is cut from ``master`` when a milestone is
done.

how a release happens
---------------------

#. the work for a roadmap entry is merged, with its tests, and CI is green
   on ``master``.
#. ``VERSION``, ``PATCHLEVEL`` and ``SUBLEVEL`` at the top of the
   ``GNUmakefile`` are bumped to the new number.
#. the ``CHANGELOG.md`` entry is added, in the roadmap's words, with the
   parts that turned out to matter written in.
#. the release is published on github, tagged ``vX.Y.Z``, with that changelog
   entry as its notes.

the tag and the release are the version. no commit in the history names it.

a system built on this kernel names the release it builds on by its
version alone, and clones whatever the tag is. so **a tag here is never
moved once it is pushed**: a tag that moved would hand every build after
it a different kernel under the same name. a fix to a release is the next
release.

the number is this kernel's own. a system built on it has a number of its
own, and moves to a new release of this one when it is ready.

the 0.x milestones
------------------

until 1.0.0 the rule that makes a number worth anything is simple:

    every version is a machine that boots.

a version that only builds, or only passes its tests, is not a version. the
number is a claim about a disk, and the claim is checked the same way
everything else is, by the boot test in CI. that is why there are eighty-odd
of them instead of five, and why a release is cut only when the machine it
names has been watched to start.

it has a second edge worth saying plainly: because each version boots, a
regression can be bisected to the release that owns it. the `testing`_ page
describes the checks; the changelog says which version introduced the thing
that broke.

after 1.0.0
-----------

once the project reaches 1.0.0 the milestone rule stops. from there the
numbers are ordinary versions, bumped when they grow unwieldy rather than
because a machine learned something, and a number no longer carries the
sentence a 0.x number carried. the releases keep happening; the ceremony
does not.

where a version lives
---------------------

``ROADMAP.md <../ROADMAP.md>``
   the plan. one entry per version, each entry the sentence that version is
   about. everything after 0.4.x is written there too, so the shape of the
   project is public before the work is done. the roadmap also names the
   non-goals, and those are as much a part of the plan as the goals.

``CHANGELOG.md <../CHANGELOG.md>``
   the record, newest first, one entry per release, saying what changed and
   why, not how, which is what the commit log is for. the release notes are
   this entry, copied.

the top of the ``GNUmakefile``
   the one place the version is written down, as ``VERSION``,
   ``PATCHLEVEL`` and ``SUBLEVEL``. the build turns them into
   ``<generated/version.h>``, and the boot banner,
   ``top``, ``arcana`` and ``persona`` all print that one macro, so they
   can never disagree about which kernel they are running. ``make
   kernelrelease`` prints it. it is the kernel's number and nobody else's:
   a system built on it has its own.

all three have to agree at the moment a release is cut. a version in one and
not the others is the kind of inconsistency that looks fine until a machine
is asked what it is.

a release that turns out to be wrong is not edited and re-tagged. the next
one fixes it, and the changelog says so, because a changelog that gets
quietly corrected is a changelog nobody can trust about the past.

.. _testing: testing.rst
