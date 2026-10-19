drivers
=======

``drivers/`` is the things that talk to hardware: the console and
its font, the serial port, the terminal discipline, the keyboard and mouse,
the timers, the pci bus, the sata controller, and the partition tables.

the console
-----------

``console.c`` is framebuffer text consoles: an 8x16 font, scrolling, a block
cursor. there are **four** of them and only one is on the screen. up to
0.2.16 there was one, which meant the terminal layer was one machine
pretending to be one seat. every real system has had several since the
eighties for the same reason. something is running and you want to do
something else, and suspending it is not the same as putting it somewhere.

what makes it possible is that the console already kept a *shadow* of what
was in every cell. that was added so a block cursor could put back the
character it was sitting on, and it turns out to be the whole of what an
off-screen console is: a screen nobody is looking at is a shadow buffer
nobody is painting.

there are five. four are for whatever the system runs on them, and the
fifth, ``VCONSOLE_MONITOR``, is the kernel's own, kept for its monitor:
no program may take it as its terminal. alt+f1..f5, or alt and the
number row for a host that eats the function keys, shows each.

the rule that makes five consoles five: **output goes to the
console its writer belongs to, not to whichever is on the screen.** a shell
on console 2 printing while console 1 is displayed must not scribble over
console 1. so ``console.c`` asks a hook, the hook is set once at boot to
something that looks up the current thread, and from an interrupt, where
there is no current thread; everything goes to whichever is on the screen,
which is the only answer that could be right.

the font is the vga 8x16 that linux's console starts with,
``lib/fonts/font_8x16.c`` in linux, converted to a C array by
``tools/font2c.py``. it is GPL-2.0, as this kernel is, so it carries no
license of its own. its 256 glyphs are code page 437, the order the vga's
rom keeps them in: ascii in the lower half, box drawing, accented letters
and a little greek above it, and the vga's symbols in the control range,
which the console draws like any other glyph when it is sent a control
byte it does not act on. the file it was converted from is blob
``523e95c75569eee09d133adf1e52c6b08a18fab3`` in linux's history.

the serial port
---------------

``serial.c`` is com1. this is the debugging lifeline; everything gets
logged here, and it is also a *second input path*: ``serial_input_init()``
turns on the receive interrupt so typing into the serial console drives the
shell. it speaks a slightly different dialect than a ps/2 keyboard (cr for
enter, del for backspace, escape sequences for arrows), so ``serial_feed()``
translates. it is split out from the irq handler so the host suite can feed
it bytes.

the vocabulary of escape sequences lives in ``ansi.c``; the terminal
discipline that decides what a key means is ``termios.c``.

where typing meets
------------------

``input.c`` is one ring buffer, one wait queue, one consumer (the shell).
the ps/2 keyboard pushes here, and so does the serial port, so the shell
neither knows nor cares whether you are sitting at the machine or on the
other end of a wire.

keys that are not characters get values above 0xff so they cannot be
confused with one. ctrl+letter comes through as the usual control codes
(ctrl+c is 3, same as every terminal since 1963). ``input_getchar_blocking()``
sleeps on the wait queue, which is how the shell sits at a prompt all day
burning zero cycles. it wakes for an interrupt too: a reader whose
process is interrupted with no key waiting gets -1 and takes nothing, so
the next key typed is still there for whoever reads it, and a key that
is there is handed over, interrupt or not. ``input_peek()`` looks without consuming, so a waiter
can watch for one particular key while leaving the rest of the queue for
whoever it was meant for.

the keyboard and the mouse
--------------------------

``keyboard.c`` is a ps/2 keyboard, scancode set 1, us layout, interrupt
driven. the scancode state machine is split from the irq handler
(``keyboard_feed()``) so it can be fed synthetic bytes in tests. the irq
handler is just ``keyboard_feed(inb(0x60))``.

``mouse.c`` is the first input that is not a stream of characters. a queue
is the right shape for typing because typing *is* a sequence. the order is
the meaning. a mouse is not that: it reports a change since the last time it
was asked, and the interesting thing is never one report, it is where the
pointer has ended up. so the driver keeps a position and the events are
edges.

the packets are three bytes, or four on a wheel mouse, and the fourth is
asked for by a handshake so odd it can only be historical: set the sample
rate to 200, then 100, then 80, and a mouse that understands starts
answering with a wheel byte. the decoder takes bytes and gives events and
touches no hardware, which is what lets the fiddly parts. the sign bits
living in the first byte, resynchronisation when a byte goes missing. be
tested against sequences written by hand.

the terminal discipline
-----------------------

``tty.c`` answers who the keyboard belongs to. up to 0.1.2 nobody owned it:
the shell peeked at keys while a program ran, watching for ctrl+c on the
program's behalf, which meant a program could never really read the
keyboard. now there is a foreground process, and ctrl+c is delivered *to* it
rather than acted on for it.

the front of the terminal is a **group**, not a process. ``cat x | wc`` is
three processes and one thing the person typing is thinking about, and every
question the terminal asks has to be asked of the whole job. ``TTY_SHELL``
(0) means no program is in front: the monitor's console, and one no
program has been put in front of. there ctrl+c is a key like any other,
the monitor's to read, and ctrl+z means nothing.

each of the consoles has its own answer to all of this, and which
console a caller means is not passed in: it is whichever the calling thread
belongs to, because that is always the right answer and passing it would
only be an opportunity to pass the wrong one.

the group in front of a console may read the keyboard whether or not that
console is on the screen. a key lands in the ring of the console being
looked at, and a reader only takes from its own, so a read on a hidden
console waits until somebody switches to it, and can never take what was
typed at another.

with a program in front, ctrl+c and ctrl+z are SIGINT and SIGTSTP to
its group, and that is all: no second ctrl+c turns into a kill, and no
terminal is handed back on a stop. what becomes of a job is the business
of whoever started it, and a shell learns of the stop from ``wait`` and
takes the terminal back itself.

a console can also belong to a program. ``ttyattach``, which only root
may call, moves the caller onto a console and makes it the program that
console belongs to, with its group in front; its children are born on the
same console. from then on the jobs are the program's to run: it puts a
group in front with ``tcsetpgrp``. the console's program may always put a
group in front; otherwise only a member of the group already there may,
which is how a job gives the terminal back.

``termios.c`` is how a terminal treats what is typed at it, and it is only
the *policy*: given a mode and a key, what does the terminal do. the reading,
echoing and waking need a machine and live in ``tty.c``; this half does not,
so it can be checked without one, and it is the half where a wrong answer
means ctrl+c reaching a program as the byte 0x03 instead of as a signal.

the mode matters because a shell wants *cooked* mode (a whole line, echoed,
handed over on enter) and an editor cannot use it (it needs each key as it
arrives, the screen left alone, and to decide for itself what ctrl+c means).
the roadmap asked for this as a **mode** rather than as two different
syscalls, and that is the part worth getting right: two calls would mean
every program choosing at every read, and a program that got it wrong once
would hang. a mode is set once, asked about, and *restored*, which an
editor must do before it exits, or it leaves the terminal unusable for
whatever runs next. see ``TERM_COOKED`` and ``TERM_RAW``.

the timers
----------

``pit.c`` is the 8253/8254 programmable interval timer, at ``PIT_HZ`` = 100,
so one tick is 10 ms. it is one of the oldest chips still wired into a modern
pc and the easiest way to make time pass.

``pit_busy_wait()`` counts ticks the interrupt delivers, which is what makes
it useless for calibrating a replacement for that timer; ``pit_poll_wait()``
is the same wait with no interrupt involved, using channel 2 and a bit on the
keyboard controller's port, capped at 50 ms. that is how one clock is
measured against another before either is delivering anything.

``pit_stop()`` stops the pit interrupting but keeps its tick counter.
uptime and every sleep are counted in those ticks, and the lapic timer
calls ``pit_tick()`` once the pit has stepped aside, so nothing above notices
the change. see `the x86 parts <arch.rst>`_ for the apic.

``rtc.c`` is the cmos real time clock, on the same two io ports since 1984
and the only thing in this machine that knows what year it is. the fiddly
parts. bcd, the pm bit hiding in the top of the hour, midnight being 12am,
are split from the io in ``rtc_decode()`` so they can be tested. this is
``LOCK_RANK_CLOCK``, a leaf: anything may ask it and it can ask nothing back.

the buses and the disk
----------------------

``pci.c`` enumerates what is plugged in. every device answers to 256 bytes of
configuration space whose first few fields are the same for all of them: who
made it, what it is, and roughly what sort of thing that makes it. that is
the doorway to every real driver; you cannot write to a disk you have not
found. the config-space reader is a function pointer so a test can supply its
own machine. what the scan found is ``pciinfo.c``'s to say: ``pci_show``
writes the list ``lspci`` prints, and ``/proc/pci`` is the same list for a
program.

``ahci.c`` is the sata controller, and it is nothing like the old ide
interface it replaced: you do not write a command to a register and wait, you
build the command in ram. a command header, a table holding the frame the
drive receives, a scatter list saying where the data goes. then set one bit
to say slot N is ready, and the controller does the rest by itself. one slot,
polled, because a disk read that blocks the kernel for a millisecond is not
worth an interrupt handler yet. every wait is bounded, so a controller that
never answers cannot hang the boot.

``part.c`` reads partition tables. up to 0.2.14 the kernel believed a disk
was a filesystem; it asked each drive whether byte 1024 looked like an ext2
superblock. a real disk begins with a *table* saying where several
filesystems are:

- **mbr**, from 1983: four entries of sixteen bytes at offset 446 of the
  first sector, each with a start and a length in 32-bit sectors, which is
  where the two-terabyte limit comes from.
- **gpt**: a header at sector 1, an array of entries after it, 64-bit
  addresses, names, and a crc32 over both so a corrupt table can be *known*
  to be corrupt rather than followed. a gpt disk still carries an mbr holding
  one entry of type 0xEE spanning the whole disk, so an old tool sees a full
  disk and declines to helpfully repartition it. finding that entry means the
  real table is the gpt.

none of it touches hardware. it is handed a function that reads sectors,
which is what lets the whole of it be tested against tables built by hand.
