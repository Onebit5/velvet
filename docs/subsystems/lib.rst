the library
===========

``lib/`` is the things the rest of the kernel would otherwise
each write badly: printing, panicking, strings, the two checksums, dates,
and the symbols a panic names itself with.

printing
--------

``kprintf.c`` is the kernel's printf. output goes to serial always, and to
the framebuffer console once it is up. supported: ``%c %s %d %i %u %x %p
%%``, the ``l``/``ll``/``z`` length modifiers (all 64-bit here anyway), and
zero padding and width like ``%08x``. that is it. no floats, no fanciness.

``kwrite()`` is a run of bytes with no format string to parse. ``kprintf
("%c", c)`` in a loop was how a program's output reached the screen, and it
parses a format string, takes a lock and walks a variadic list *per
character*. for a program redrawing a screen that is two thousand of each to
write what one call could.

``kprintf_to_console()`` decides whether the screen sees it. serial and the
log always get everything. boot runs with it off, so the screen shows a
greeting instead of a wall of driver chatter, and ``dmesg`` can still show
the chatter. ``kprintf_serial_filter()`` decides whether what is printed
also goes down the serial line: the serial line is a second window onto
whichever console is being looked at rather than a console of its own, since
four shells writing down one wire is four conversations in one column.

``klog_printf()`` never goes to the screen, only the log and serial, for
something worth recording that arrives when the screen belongs to somebody
else.

a program reads the log with the ``syslog`` call, which ``klog_copy()``
answers: the newest bytes, as many as the caller has room for, copied
under the print lock so a line another core is halfway through is either
all there or not yet. the copy goes into a buffer of the kernel's first
and to the program's after the lock is let go, since a write to a user
page can fault and nobody should take a fault with every core's printing
held up.

the format strings are checked against what this file actually implements,
not against real printf: gcc checks a format string against *real* printf,
which accepts far more than this does. a ``%-7s`` slipped through once,
printed itself literally, and read every later argument from the wrong slot.
``tools/checkfmt.py`` is the check, and it fails the build.

``panic.c`` prints to serial and console if up, then parks the cpu with
interrupts off. a panic is for an invariant that has already been broken and
cannot be repaired. never control flow. see `error handling
<../coding-style.rst>`_.

strings
-------

``string.c`` is the usual suspects. there is no libc in the kernel, so they
are all here, and they *have* to exist, because gcc can emit calls to
``memcpy``/``memset`` behind your back even in freestanding mode (struct
copies, and so on).

the three that move memory, ``memcpy``, ``memmove`` and ``memset``, go a
word at a time once both ends are aligned alike, and a byte at a time
for the rest. the rest of the file is written for clarity, but these are
the ones the console scrolls a screen with, megabytes per line, and a
byte at a time that took long enough under qemu for a selftest that
printed a dozen lines to run out of time.

checksums
---------

``hash.c`` is crc32, which answers "did this arrive intact". arithmetic in
a polynomial field, chosen so that the errors a wire or a disk actually
makes, a flipped bit or a short burst, are certain to change it rather than
merely likely to. it is *not* a hash in the security sense: making a second
file with the same crc32 is easy on purpose, and gpt uses it because a disk
does not lie deliberately.

sha-1 was here beside it until 0.4.4, for the stamp the kernel carried of
its own source archive, and it left with the archive reader.

it is streaming, because the whole point of a checksum over a stream is to
compute it while the bytes go by.

dates
-----

``epoch.c`` turns the cmos clock's "the 15th of august, 2026, at 10:19" into
a number. seconds since 1970-01-01, because `make` decides what to
rebuild by comparing two times and a filesystem stores one per file, and a
comparison needs a number.

it is its own file because it is arithmetic with traps in it, and every one
of them can be checked without a machine: months have different lengths and
february has two; a leap year is every four years except every hundred
except every four hundred (1900 was not one and 2000 was); the epoch is not
the start of a year the caller is thinking about. the property worth having
is that the conversion goes both ways and agrees with itself. turning a
date into a number and back gives the same date, for every date, checkable
by loop rather than by argument.

``clock.c`` is the time now: when boot was, which the cmos chip said once,
and how long the timer says it has been since. it is apart from
``epoch.c`` because it needs the timer and the arithmetic does not.

symbols and backtraces
----------------------

``ksyms.c`` reads the kernel's own symbol table, baked in at build time by
``tools/gensyms.py``, so a panic can say *where* it happened in words rather
than as an address. names are offsets into one big blob rather than
pointers, because pointers would need relocating and the whole table has to
survive being generated from one link and used in the next. the build is two
passes for the same reason. the symbol table describes addresses, and
linking it in changes them. ``gensyms.py --check`` proves the second pass did
not move a single function.

``backtrace.c`` walks the saved frame pointers and prints who called whom.
pass 0 for ``rbp`` to start from wherever the caller is; pass a saved one
(from an interrupt frame) to walk somebody else's stack. ``kbacktrace()``
and not ``backtrace()``, because glibc has a ``backtrace()`` and the host
tests link against it. the kernel's was quietly being shadowed.

the epoch interface
-------------------

a date is a number here, and everything above this converts once and then
compares numbers: ``make`` decides what to rebuild by comparing two of them,
a filesystem stores one per file, and the clock is a number that only ever
goes up.

``<velvet/epoch.h>``

``uint64_t epoch_from_date(const struct rtc_time *t)``
    the seconds between 1970-01-01T00:00:00Z and a date read off the cmos
    clock. the arithmetic has the usual traps in it: months of different
    lengths, february in a leap year, and 1900 not being one.

``void epoch_to_date(uint64_t seconds, struct rtc_time *out)``
    the same arithmetic read the other way. a date turned into seconds and
    back has to be the date it started as, for every date, which is the
    property the suite spends its time on.

``uint8_t epoch_days_in_month(uint16_t year, uint8_t month)``
    the length of one month in one year. both conversions need it, and it is
    the one place the leap rule is written down.

``uint64_t epoch_now(void)``
    the time as the kernel believes it, which comes from the clock rather
    than from the chip that was read once at boot. in ``clock.c``, with
    ``epoch_start`` and ``epoch_known``.


the hash interface
------------------

one sum, which can be fed a piece at a time.

``crc32``
    did this arrive intact: the reflected crc32 everybody means by the name,
    the one gpt, png, zip and gzip use. ``crc32_start()`` is the seed and
    ``crc32_more`` is handed back what the last call returned.

the kprintf interface
---------------------

printing, and where it goes. every kprintf reaches the serial line, and
reaches the framebuffer console once that exists, which is why a boot that
dies before the console is up still says something to whoever is watching the
port.

``void kprintf(const char *fmt, ...)``
    kernel printf. the compiler is told it is one, so a format string that
    does not match its arguments is a warning at build time rather than a
    surprise at three in the morning.

``void kwrite(const char *s, size_t n)``
    a run of bytes with no format string to parse, which is what a syscall
    handing over a buffer wants.

``void kprintf_to_console(bool on)``
    whether kprintf reaches the screen, for the moment something is painting
    the screen itself.

``void kprintf_serial_filter(bool (*fn)(void))``
    whether what is being printed should also go down the serial line, so a
    program's output can be kept out of the kernel's log.

``size_t klog_copy(char *str_buf, size_t sz_size)``
    the newest ``sz_size`` bytes of the log, oldest first, into
    ``str_buf``: how many that was. the log is left as it was, so every
    reader sees all of it.


the ksyms interface
-------------------

turning an address back into a name, which is what makes a panic readable and
a backtrace worth printing at all. the table is generated from the build; it
is absent from host builds, which supply their own.

``const char *ksym_lookup(uint64_t addr, uint64_t *offset)``
    which function contains the address, and how far into it, or false.

``const char *ksym_lookup_in(table, count, names, addr, offset)``
    the same search against a table the caller supplies. this is the part
    with the logic in it, which is why the tests exercise this one and not
    the wrapper.


the panic interface
-------------------

the last thing the kernel says when it cannot go on. a panic prints, and then
stops the machine with interrupts off, because carrying on with the state
that caused it is how a bug becomes a corrupted disk.

``void panic(const char *fmt, ...)``
    prints to serial and to the console if it is up, then parks the cpu. it
    does not return, and callers know it: the compiler is told, so no code
    after a call is treated as reachable.


``void  *memcpy(void *dst, const void *src, size_t n);``
    the usual suspects. no libc here so the kernel rolls its own.

``size_t strnlen(const char *s, size_t max);``
    the rest of the set, which came in when the two implementations became one.
