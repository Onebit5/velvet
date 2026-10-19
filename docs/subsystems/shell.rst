the shell
=========

``shell/shell.c`` is the velvet room terminal, and it runs as one
thing: the kernel's monitor, on the fifth console, started by the kernel's
``services`` thread. the sessions somebody logs into on the other four are
the system's, run by its ``/init``.

it is a **kernel thread**, not a program in ring 3, which is why everything
inside it is a builtin.

``shell_monitor()`` does not return. the monitor is there for as long as
the machine is, and if it dies the ``services`` thread starts it again.

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
refused, and so are a ``|`` and a redirection: its builtins print straight
at the screen and have no output to hand anybody. starting programs, and
the sessions and jobs they belong to, is the system's work.
the monitor is for looking at the kernel underneath it, ``ps``, ``mem``,
``dmesg``, ``bt``, ``vmm``, ``hexdump``, and for taking the machine down
with ``reboot`` or ``poweroff`` when nothing else will.

builtins
--------

alt+5 lands you at the monitor's prompt, ``monitor@velvet[5]:/#``:

::

    help      list what thou may command
    clear     wipe the screen clean
    whoami    who thou art, and what that permits
    dmesg     everything boot said while you werent looking
    arcana    the rank of this bond, and its making
    persona   the face this machine wears
    mem       frames and heap, honestly counted
    ps        the threads that walk this realm
    top       the same, but watched rather than asked
    lspci     what is plugged into this machine
    slabs     the object caches, and what they hold
    disk      the drive, and the filesystem on it
    sync      put what is in memory onto the disk
    fsck      check the filesystem, and mend what has one answer
    resize    grow a filesystem into the partition it sits in
    mount     which filesystem is where; mount <n> moves it
    parts     what each drive says it holds
    chvt      show another console; alt+f1..f5 too
    mouse     the pointer, and whether it is well
    set       a variable, or all of them
    unset     forget one
    env       what is set
    test      answer a question in $?, for `if`
    source    run a file of commands in this shell
    cpus      the processors, and which are awake
    locks     what guards what, and what waits
    cd        go somewhere; no argument means the root
    pwd       where the kernel is standing
    ioapic    move external interrupts off the 8259 (risky)
    summon    call forth a persona thread (in the background)
    vmm       what the page tables say about an address
    bt        who called whom to get here
    date      what the battery-backed clock believes
    hexdump   look at memory, safely
    kill      end a thread by id
    signal    send a signal to a process by pid
    history   what thou hast said before
    time      how long a command takes
    crash     tempt fate with a wild pointer
    smash     run off the end of the stack on purpose
    init      what init looks after, and how it fares
    install   copy this system onto a drive and make it boot
    src       the source this was built from, still on the medium
    ifconfig  the card, and the address on it
    dhcp      the address the wire handed out
    tcp       connections, and what state they are in
    host      what a name resolves to
    fetch     pull a file down over http
    ping      the smallest round trip there is
    arp       who else is out there
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
tab                             complete a builtin's name, or a filename after one that takes one
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

builtins are described out of the shell's own table, which is where they
are declared, so the description cannot drift from the thing described:
``help cd`` prints the usage and what it is for.

plain ``help`` prints names in columns sized to the terminal: what somebody
scanning it needs is the vocabulary, and what any one word means is ``help
<name>``.

environment and scripts
-----------------------

the monitor's variables are kept as a program keeps its environment, a
*block* of ``NAME=value`` strings: ``set`` and ``unset`` change it, and
``$NAME`` and ``$?`` read it. see `the environment <lib.rst>`_.

``source`` runs a file of lines with ``if`` and ``while``, in the monitor
itself. ``hello.sh`` in ``base/diskroot/`` is an example.

shutting down
-------------

``reboot`` and ``poweroff`` go through init's ordered shutdown;
everything stopped before the sync, in the reverse of the order it came
up, because the disk has a write-back cache and a machine that resets
while anything can still write loses whatever had not reached the drive.
see `init <sched.rst>`_.

the shell interface
---------------------

the velvet room terminal, as the kernel's monitor. the shell is a kernel
thread rather than a program, so what a program would keep in its process,
its working directory, its history and its variables, lives in one struct
that every function reaches through one accessor.

what follows is what each name means and what a caller may pass.
