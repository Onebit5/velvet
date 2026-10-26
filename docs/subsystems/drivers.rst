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

there are five, all of them for whatever the system runs on them. the
fifth was the kernel's own until 0.4.4, kept for a shell that ran in the
kernel, and is the system's now that there is none. alt+f1..f5, or alt and the number row
for a host that eats the function keys, shows each.

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

``input.c`` is a ring of bytes per console, and a wait queue on each. the
ps/2 keyboard pushes here, and so does the serial port, so the reader
neither knows nor cares whether you are sitting at the machine or on the
other end of a wire. a key lands in the ring of the console being looked
at.

a character is its byte, and ctrl+letter the usual control code (ctrl+c
is 3, same as every terminal since 1963). the keys that are not
characters are pushed as the escape sequences a console terminal sends
for them: the arrows ``ESC [ A`` to ``ESC [ D``, home ``ESC [ 1 ~``,
delete ``ESC [ 3 ~``, end ``ESC [ 4 ~`` and the page keys ``ESC [ 5 ~`` and
``ESC [ 6 ~``, so a program reads them as bytes like everything else.
the terminal gets first refusal at every key, for the ones that switch
or scroll consoles.

``input_wait()`` sleeps on a console's queue, which is how a shell sits
at a prompt all day burning zero cycles. it wakes for an interrupt too:
a reader whose process is interrupted with nothing waiting is told so
and takes nothing, so the next key typed is still there for whoever
reads it.

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

``tty.c`` is the terminals: one on each console, ``/dev/tty1`` to
``/dev/tty5``, with ``/dev/console`` the first of them and ``/dev/tty``
whichever is the caller's own. a terminal holds what a program sets with
``tcsetattr``, a ``struct termios``, the line being typed, the group in
front and the session it belongs to.

a terminal belongs to a **session**. a session leader with none takes
one with ``TIOCSCTTY``, and has it until it lets go with ``TIOCNOTTY`` or
ends; root may take one from another session. every process of the
session reaches it as ``/dev/tty``, and a process whose session has none
is told ``-ENXIO`` there. the kernel starts ``/init`` on the first
console, and an ``/init`` that wants it for its terminal takes it.

the front of the terminal is a **group**, not a process. ``cat x | wc`` is
two processes and one thing the person typing is thinking about, and every
question the terminal asks has to be asked of the whole job. a group of
the terminal's session is put in front with ``tcsetpgrp``, which is
``TIOCSPGRP``.

a group behind the one in front is stopped when it reads from the
terminal, with ``SIGTTIN``, and when it changes it, ``tcsetattr`` or
``tcsetpgrp``, with ``SIGTTOU``; the call is made again once the group is
continued. a group that ignores or blocks the signal is told ``-EIO`` for
a read and is let make the change, which is how a shell, that ignores
``SIGTTOU``, hands the terminal from job to job. a group behind may write
to it.

with ``ISIG``, the ``VINTR``, ``VQUIT`` and ``VSUSP`` characters, ctrl+c,
ctrl+\\ and ctrl+z unless a program changed them, are ``SIGINT``,
``SIGQUIT`` and ``SIGTSTP`` to the group in front, and that is all: no
second ctrl+c turns into a kill, and no terminal is handed back on a
stop. what becomes of a job is the business of whoever started it, and a
shell learns of the stop from ``wait`` and takes the terminal back
itself.

with ``ICANON`` a read takes one whole line, edited as it is typed with
``VERASE``, ``VWERASE`` and ``VKILL``, and ``VEOF`` at the start of a line
is the end of input. without it a read is what ``VMIN`` and ``VTIME`` say.
the keys that are not characters, the arrows, home, end, delete and the
page keys, arrive as the escape sequences a console terminal sends for
them.

a key lands in the terminal of the console being looked at, and a reader
only takes from its own, so a read on a hidden console waits until
somebody switches to it, and can never take what was typed at another.

``termios.c`` is how a terminal treats what is typed at it, and it is only
the *policy*: given a ``struct termios`` and a byte, what does the
terminal do with it, a character for the line, an erase, a signal, the
end of input. the reading, echoing and waking need a machine and live in
``tty.c``; this half does not, so it can be checked without one, and it
is the half where a wrong answer means ctrl+c reaching a program as the
byte 0x03 instead of as a signal.

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
