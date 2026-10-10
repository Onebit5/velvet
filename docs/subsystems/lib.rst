the library
===========

``lib/`` is the things the rest of the kernel would otherwise
each write badly: printing, panicking, strings, the three checksums, deflate,
dates, and the symbols a panic names itself with.

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

this is the **only** copy of these functions: velvetOS's libc compiles this
same file hosted, out of the release it clones into ``obj/velvet/``,
rather than keeping a second one, which is the mistake this project
already made once with two libcs and does not need twice. so a change
here is a change to that libc too, the day velvetOS moves to a release
that has it.
``strcasecmp`` is here because dns needs it. a name is case-insensitive
and a server echoes the question back with whatever case it chose, some
varying it deliberately to make a reply harder to forge.

checksums
---------

``hash.c`` is crc32, adler32 and sha-1, which get lumped together as
"hashes" and answer different questions:

- **crc32** answers "did this arrive intact". arithmetic in a polynomial
  field, chosen so that the errors a wire or a disk actually makes. a
  flipped bit, a short burst. are certain to change it rather than merely
  likely to. it is *not* a hash in the security sense: making a second file
  with the same crc32 is easy on purpose, and gpt uses it because a disk
  does not lie deliberately.
- **adler32** answers the same question and answers it worse, and is here
  for one reason: a zlib stream ends with one, so reading a compressed git
  object means checking one.
- **sha-1** answers "is this the same thing". a git repository is a store
  addressed by the hash of what is in it. the name of an object *is* its
  sha-1, so this is the format's idea of identity, not a check bolted on.

sha-1 is broken, and the header says so: two different files with the same
sha-1 have been published. git uses it anyway, for the same reason it always
did; this is a content address rather than a signature, and a repository
is not a place where somebody else chooses both files. the machine has to
speak git, and git speaks sha-1.

the routines are streaming, because a git object can be larger than anything
worth holding at once, and because the whole point of a checksum over a
stream is to compute it while the bytes go by.

deflate
-------

``deflate.c`` / ``inflate.c`` are here because **every object in a git
repository is compressed**. not optionally, not as a setting. the file on
disk *is* a zlib stream, so reading a repository means implementing this
whether or not anything else wants compression. it arrived as a prerequisite
rather than as a feature.

deflate is two independent ideas stacked:

- **lz77** writes anything that appeared before as "go back this far, copy
  this many" rather than writing it again. that is why text compresses and
  random bytes do not.
- **huffman** gives the symbols that come up often short codes and the rare
  ones long codes, so the common case costs fewer bits. the codes are
  *canonical*, which means the table is not transmitted, only the length
  of each code is, and both sides work out the same codes from the lengths
  by the same rule. that is the trick that keeps the header small.

a stream is a series of blocks, and each block picks its own answer: stored,
fixed huffman, or dynamic huffman. a zlib stream wraps that in two bytes of
header and an adler32 of the *uncompressed* bytes, which is why adler32
was written before anything wanted it.

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

the environment
---------------

``env.c`` implements the environment *block*, shared between the process
table and the shell's session (a shell is a kernel thread with no process
entry of its own). one block of ``NAME=value`` strings, each ended by a NUL,
with an empty string for the end of the lot, that shape is what makes the
whole thing one ``memcpy`` to inherit, and inheriting is most of what an
environment is *for*. what a program does with it is velvetOS's, in its
``docs/subsystems/userland.rst``.


the deflate interface
---------------------

deflate is here for one reason: every object in a git repository is a zlib
stream, not optionally, so reading a repository means implementing this
whether or not anything else ever wants compression. the two ideas inside it
are lz77, which writes a repeat as "go back this far, copy this many", and
huffman, whose codes are canonical, so only each code's length is sent.

``long inflate(const void *in, size_t in_len, void *out, size_t out_cap)``
    the compressed data alone, as RFC 1951 describes it and as a member
    inside a zip contains.

``long zlib_inflate(const void *in, size_t in_len, void *out, size_t out_cap)``
    a zlib stream: two bytes of header, the same data, and the adler32 of the
    uncompressed bytes checked on the way out. this is the one a git object
    needs.

both return the bytes produced or a negative error, and the caller says how
much room there is. a decompressor that cannot be told when to stop writes
past the end of a buffer the day somebody hands it a hostile file.


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
    than from the chip that was read once at boot.


the hash interface
------------------

three sums that get lumped together as hashes and answer different
questions, which is why all three are here rather than whichever one came
first. each of them can be fed a piece at a time, because a git object can be
larger than anything worth holding at once.

``void sha1_of(const void *data, size_t len, uint8_t out[20])``
    the whole thing at once, when it is all here. the streaming form is
    ``struct sha1``, whose state the caller carries between calls.

``void sha1_hex(const uint8_t digest[20], char out[41])``
    twenty bytes as the forty characters everybody writes down. this is the
    one that names a git object, since the name of an object *is* its sha-1.

``crc32``
    did this arrive intact: the reflected crc32 everybody means by the name,
    the one gpt, png, zip and gzip use. ``crc32_start()`` is the seed and
    ``crc32_more`` is handed back what the last call returned.

``adler32``
    the same question answered worse, here because a zlib stream ends with
    one, so reading a compressed git object means checking one.
    ``adler32_start()`` is the seed, which is 1 and not 0.


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

``void klog_dump(void)``
    everything kprintf has printed, oldest first. this is what ``dmesg`` is.


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

``int    strcasecmp(const char *a, const char *b);``
    the same, ignoring case.

``size_t strnlen(const char *s, size_t max);``
    the rest of the set, which came in when the two implementations became one.
