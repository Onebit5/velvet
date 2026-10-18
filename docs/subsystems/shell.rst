the shell
=========

``shell/shell.c`` is the velvet room terminal: one session, from
the greeting to whenever somebody logs out. one runs per virtual console,
started by init, when the medium carries no ``/init`` of its own; and
one, as the monitor, runs on the fifth console always.

it is a **kernel thread**, not a program in ring 3, which is why everything
inside it is a builtin and why pid 0 (not a process) is what the process
table sees as its owner. that has a consequence the process table cares
about: a process whose parent is 0 belongs to a shell that waits for its own
and reads their exit codes, so init must not collect it.

``shell_run()`` used to be marked ``noreturn``, because a shell was the last
thing a thread ever did. it returns on ``logout`` now, and that is the point
rather than a detail: a session that can *end* is one that leaves nothing
behind for the next person, and something has to be there to start the next
one. that something is init.

the monitor
-----------

the fifth console is the kernel's, and what runs on it is this shell as
a *monitor*, ``shell_monitor()``. it is there whatever the medium
carries, and it is entered on purpose, with alt+5, or ``ctrl+\`` then 5
on serial, rather than being where boot lands.

nobody logs into it. it has root's powers under the name ``monitor``,
because a monitor is for when the system above the kernel is what has
gone wrong, and a login checked against the system's accounts would be
asking the broken thing for permission.

**it runs no programs.** a name that is not one of its builtins is
refused rather than looked up on the path, and so are ``run``, ``jobs``,
``fg``, ``bg`` and ``logout``, a ``|`` and a redirection. ``help`` lists
only what it has, and ``help <program>`` does not run the program to ask
it. starting programs, and the sessions and jobs they belong to, is the
system's work. the monitor is for looking at the kernel underneath it,
``ps``, ``mem``, ``dmesg``, ``bt``, ``vmm``, ``hexdump``, and for taking
the machine down with ``reboot`` or ``poweroff`` when nothing else will.

builtins
--------

boot lands you at a ``velvet>`` prompt:

::

    help      list what thou may command
    clear     wipe the screen clean
    echo      say something back
    ls        what the ramdisk carries
    run       give a program the outer ring
    dmesg     everything boot said while you werent looking
    cat       read a file aloud
    arcana    the rank of this bond, and its making
    persona   the face this machine wears
    mem       frames and heap, honestly counted
    uptime    how long since the bond was formed
    ps        the threads that walk this realm
    bt        who called whom to get here
    date      what the battery-backed clock believes
    hexdump   look at memory, safely
    kill      end a thread by id
    history   what thou hast said before
    time      how long a command takes
    summon    call forth a persona thread
    vmm       what the page tables say about an address
    crash     tempt fate with a wild pointer
    smash     run off the end of the stack on purpose
    reboot    sever the bond and begin anew
    poweroff  let the velvet room fade

``vmm`` with no argument points at one thing of each kind. code, a string
constant, the heap, your stack, and an address nobody lives at, so the
permission column shows W^X actually holding. give it a hex address to look
that up instead.

``hexdump`` asks the page tables whether an address is mapped before reading
it, so a typo prints ``<not mapped>`` instead of panicking. ``kill`` works on
a thread in any state, including one blocked on a wait queue; it takes it
off that queue first, which is what the thread's back-pointer is for.

``summon pixie`` and ``summon jack-frost`` spawn real kernel threads that
count in the background while you keep typing; they speak eight times and
then depart, which also gives the reaper something to clean up (watch ``ps``
before and after). cancelling one with ctrl+c is cooperative rather than
forceful, and can take up to one sleep period.

``crash`` dereferences ``0xdeadbeef`` on purpose, which page faults inside
the shell thread and gets the full exception report. decoded fault reason,
cr2, every register, then the panic. the panic handler polls the 8042
directly, because interrupts are never coming back and the keyboard driver is
no help, and any keypress resets the box. it ignores key *releases*,
otherwise letting go of the enter key you used to type ``crash`` would reboot
instantly. ``smash`` runs off the end of the stack on purpose, to exercise
the guard page and the tss's interrupt stack table.

line editing
------------

as close to readline as a small kernel needs:

==============================  ================================================
left / right, ctrl+b / ctrl+f   move the cursor; you can type in the middle
ctrl+a / ctrl+e                 start and end of line
backspace, del / ctrl+d         delete behind and ahead
ctrl+w / ctrl+u / ctrl+k        kill a word, the line, or to the end
up / down                       the last 16 commands
tab                             complete a command name, or a filename after ``cat`` / ``run``
ctrl+l                          wipe the screen, keeping the line you were typing
ctrl+c                          abandon the line, and recall any running personas
==============================  ================================================

adjacent duplicates and empty lines do not make it into the history.

all of that rests on one small change: the console used to treat ``\b`` as
"move left and erase", which made ``"\b \b"`` work by accident. now it moves
only, the way every real terminal does, so ``"\b \b"`` still erases *and* a
bare ``\b`` is non-destructive cursor movement that behaves identically on
the framebuffer and down the serial line. the console grew a shadow buffer to
go with it, because a block cursor sitting *on* a character has to put that
character back when it moves away and a framebuffer cannot tell you what used
to be there. see `the console <drivers.rst>`_.

help
----

``help`` knows what it is describing by *asking*. what ``cat`` takes is
declared inside ``cat``; the shell could keep a copy and then there would be
two descriptions free to drift, so it does not keep one: **``help cat`` runs
cat with ``--help``** and lets it answer. the same declaration produces both,
so ``cat --help`` typed directly is the identical text by construction rather
than by discipline.

builtins are described out of the shell's own table, which is right for the
same reason, that is where they are declared, and ``help cd`` also says
*why* it is a builtin, because that is the question somebody asking has
probably got.

plain ``help`` prints names in columns sized to the terminal: what somebody
scanning it needs is the vocabulary, and what any one word means is ``help
<name>``. whether something runs inside the kernel or out in ring 3 is a fact
about how it was built, not about how it is used, so it is a dot in the
margin rather than a heading to look under. completion offers both.

commands and the path
---------------------

there is a search path. ``/bin``, then ``/boot/bin``. looked up through
the vfs like everything else, so a disk can supply a command and a name
earlier on the path hides one later. **the working directory is deliberately
not on it.** a name typed on its own should mean the same thing wherever you
happen to be standing, and a program left lying in a directory should not
quietly become a verb there. anything with a slash in it is a path, read from
where you are and taken exactly as written. say ``./thing`` if that is what
you mean.

environment and scripts
-----------------------

the environment is a *block* of ``NAME=value`` strings, inherited across
``spawn`` by one ``memcpy``. it belongs to the process rather than to the
program, which is why ``export`` is a builtin and not a command. a command
could only ever change its own. see `the environment <lib.rst>`_.

scripts are lines with ``if`` and ``while``, run by the shell; ``$PATH`` and
the other variables come out of the same block. ``hello.sh`` in ``base/diskroot/``
is an example.

shutting down
-------------

``logout`` ends the session and returns to init, which starts the next one
if the service is set to respawn. ``reboot`` and ``poweroff`` go through
init's ordered shutdown; everything stopped before the sync, in the reverse
of the order it came up, because the disk has a write-back cache and a
machine that resets while anything can still write loses whatever had not
reached the drive. see `init <sched.rst>`_.

the shell interface
---------------------

the velvet room terminal. the shell is a kernel thread rather than a
program, so the things that belong to a session, its working directory, its
history and its jobs, live in a struct that the console owns and every
function reaches through one accessor. that is why a second console has its
own shell rather than sharing one.

what follows is what each name means and what a caller may pass.
