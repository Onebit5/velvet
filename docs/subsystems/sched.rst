scheduling and processes
========================

``sched/`` is threads, the run queue, the context switch,
processes, signals, the lock discipline and init.

threads
-------

a ``struct thread`` is small on purpose: the saved stack pointer, and
everything else about a parked thread lives *on that stack*. one word is
the whole handle. (it was called ``rsp`` until 0.2.21. the x86 name for
it, in portable code. what it is, is a stack pointer.)

the state is one of ``READY``, ``RUNNING``, ``SLEEPING``, ``BLOCKED``,
``DEAD``. there is a fifth thing that is deliberately **not** a state:
``stopped``. a thread suspended by ctrl+z may *also* be blocked on a pipe,
or asleep, or ready. "what is it waiting for" and "may it run at all" are
different questions, and squeezing both into one enum means a
stopped-then-woken thread forgets it was stopped, which is a program that
resumes itself the moment anybody types at it.

``cpu_ticks`` counts the timer ticks a thread was running for. it is a
*sampling* measure rather than a real accounting. a thread that always
yields just before the tick looks free, but it is honest about being one,
and it is what turns the scheduler from a claim into something you can
watch (``top``).

a thread carries the console it belongs to and the pid of the program it is
running, if any. ``thread_create()`` builds one ready to go;
``thread_create_parked()`` builds one already blocked, so no other core can
grab it before its own core has claimed it.

the scheduler
-------------

round robin, preemptive, no priorities and no fairness accounting. it takes
turns. ``sched_init()`` adopts whatever is currently executing as thread 0
and spawns the idle thread, so after it ``kmain`` *is* a thread.

smp
~~~

``sched.h`` has the per-core entry points. ``sched_join()`` on a core
that has its own descriptor tables, ``sched_thread_cpu()``,
``sched_cores_scheduling()``, ``sched_cpu_running()`` for ``cpus``. a
thread marked ``RUNNING`` is on somebody's cpu and no other core may pick it
up: two cores running the same thread is two cores on one stack, which ends
exactly as badly as it sounds. the other cores are woken by
``arch/x86_64/trampoline.asm`` in real mode; see `the x86 parts
<arch.rst>`_.

the context switch
~~~~~~~~~~~~~~~~~~

``arch/x86_64/switch.asm`` saves six callee-saved registers and a return
address, and that is the entire parked state of a thread; everything else
the sysv abi already lets a function call clobber. rflags is deliberately
*not* saved, because every way of resuming a thread restores it some other
way: preempted threads come back through the ``iretq`` at the end of their
interrupt, threads that yielded come back through ``irq_restore()``, and
brand new threads ``sti`` for themselves in the bootstrap. that last one is
not optional. a new thread arrives via ``ret`` with interrupts still off,
and forgetting it silently kills preemption for the whole system.

the timer irq sends its EOI *before* running the handler, which looks
backwards. it is because the scheduler can switch threads inside the timer
handler and never return on that stack, and a freshly created thread has no
half-finished interrupt frame to return through, so the EOI would otherwise
never be sent and the pic would go quiet forever.

wait queues
-----------

a thread that needs to wait for something other than the clock parks on a
``struct waitq``. the keyboard has one, which is how the shell sits at a
prompt costing zero cpu until you press a key.

the subtle part is the handoff: ``waitq_block()`` **must be entered with
interrupts off and returns with them still off**, so that "look in the
buffer, find it empty, go to sleep" is one atomic move. get that wrong and a
key arriving in the gap between the check and the sleep is lost forever, and
the shell waits for something that already happened.

``waitq_enqueue()`` / ``waitq_sleep()`` are the same thing in two halves,
for a caller holding a lock of its own: go on the queue, then drop the lock,
then sleep. a **lock must never be held across the sleep**. turning
interrupts off is a property of a thread and rides through a context switch
harmlessly, but a lock is a property of the machine, and a sleeping thread
holding one is a machine where nobody else can ever have it.

locks
-----

``spinlock.c`` exists because this kernel spent twelve versions using
``cli`` as mutual exclusion. that worked, and for a real reason: with one
core the only thing that could interrupt a critical section was an
interrupt. the question changed in 0.2.0. turning interrupts off on
*this* core says nothing about a thread on *that* one, and every one of
those thirty-nine places was quietly reclassified from correct to wrong by a
version that did not touch any of them.

so there is a real lock, and both halves of the problem at once:
``spin_lock_irq`` protects against this core's interrupts *and* the other
cores, and hands back the flags, so it drops into exactly the shape
``irq_save``/``irq_restore`` already had. that was deliberate. an audit of
thirty-nine call sites is worth doing where every change looks the same and
any that does not stands out.

ranks
~~~~~

two locks taken in opposite orders by two cores is a machine that stops,
with no fault and nothing printed. the only defence is a **rank** every
lock declares and never violates: a lock may only be taken while holding
locks of *lower* rank. it is checked, not hoped for.

::

    LOCK_RANK_PIPE      a pipe wakes threads and calls nothing below it
    LOCK_RANK_DEVICE    tty, input, pci, the disk
    LOCK_RANK_CLOCK     the cmos clock, a leaf: anything may ask it
    LOCK_RANK_SCHED     the run queue
    LOCK_RANK_PROCESS   the process table, which sched reaches into
    LOCK_RANK_HEAP      slab, and kmalloc above it
    LOCK_RANK_PMM       which the heap calls into, never the reverse
    LOCK_RANK_PRINT     anything may print; printing takes nothing

the clock got a rank of its own in 0.2.21, and the story is worth keeping:
the rule caught it the first time a filesystem stamped a file it had just
created. the disk lock is held for a whole create, and putting a timestamp
on the new inode means reading the clock, which was the same rank. a rank of
its own rather than reordering anything, because the clock is a leaf.

processes
---------

a process is a program someone started, and it **outlives the thread that
ran it**. that outliving is the entire point: a thread is reaped the moment
it dies, so if the exit code lived on the thread there would be nothing left
to read it from by the time a parent asked. so the code, the name and the
parentage live in a process slot that stays occupied until somebody collects
it. that is a zombie.

``struct process`` carries the pid, the parent, the **job group** (``pgid``)
and the uid, the environment block, the current working directory, the file
descriptors, and the signal state.

pid 0 is not a process at all; it is what the kernel shell uses, since a
shell here is a kernel thread. the difference matters to exactly one rule:
a process whose parent is 0 belongs to a shell that waits for its own, so
init must not collect it. pid 1 is init, handed out first so reparenting has
a number that is known before the process it names exists.

the process table (``process.c``) is where ``ps``, ``wait``, ``kill`` and
reparenting all bottom out. children are reparented to init as their parent
goes, inside ``process_exited()``, since the moment the parent ends is the
only moment anybody could notice. ``process_orphan()`` returns only what is
genuinely nobody's.

file descriptors
~~~~~~~~~~~~~~~~

0, 1 and 2 were not descriptors until 0.2.9. the syscall layer answered
them directly. that held until `echo hi > file.txt`, which is stdout
pointing at a *file*, and there was nowhere to write that down. so they are
real slots now, filled in when the process is made.

every read and every write is one lookup and a switch, and **redirection is
just a different thing in the slot before the program starts**: a program
that cannot tell where its output goes is a program that works in a
pipeline, in a file, and on a screen without knowing which. see `pipes and
redirection <fs.rst>`_.

jobs
~~~~

``cat x | grep y | wc -l`` is three processes and *one* thing the person
typing it is thinking about. ctrl+z has to stop all three or none, ctrl+c
has to reach all three, and ``fg`` has to bring all three back, so they
share a job group, and that number is the pid of the first of them. a
command on its own is a group of one.

signals
-------

until 0.3.7 there was one flag called ``interrupted``, and it was as close
to a signal as this kernel got. that is enough for ``cat`` and not enough
for anything that runs for a minute.

a signal in this kernel is **a bit in a word**, not a message: there is no
queue and no payload, and raising one that is already raised changes
nothing. two ctrl+c presses in the same instant are one interrupt, which is
exactly right and is the first thing people expect to be wrong. that also
means signals cannot be counted, and any design that wants to count them
wants something else.

a process ignores a signal, takes the default action, or runs a handler.
the default is nearly always "die", and the exceptions are the interesting
ones. SIGCHLD arrives constantly and a default of death would mean every
shell exiting the moment a command finished. SIGKILL and SIGSTOP may not be
caught, blocked or ignored, which is the entire reason the system can still
be saved by somebody who can type.

the hard part is not delivery. every blocking call here was written
assuming it finishes for one of two reasons: what it waited for happened, or
the thing it waited on went away. a signal is a third, and a call that does
not know about it either sleeps through the interrupt or, worse, returns
as though it succeeded. so a blocked thread carrying a deliverable signal is
*woken*, and the call returns "interrupted" rather than a result. that
distinction has to reach the program, because a read that returns 0 on an
interrupt is indistinguishable from end of file.

the state lives in ``struct signal_state``: a pending set, a blocked set, a
handler table, and which signal is currently running. the last one matters:
a handler runs with its own signal blocked, so a SIGINT inside the SIGINT
handler waits rather than nesting, without it, ctrl+c held down is a stack
that grows until it hits the guard page. see `sigsuspend` and the rest in
`the system calls <../syscalls.rst>`_.

signals are delivered on the way back to ring 3 from a call, where the
kernel has the program's registers to hand for a handler. a program busy
in a loop makes no calls, so an interrupt that is about to return to
ring 3 also looks, for a pending signal whose action is to end the
process, and ends it there. SIGKILL ending anything at all is the whole
of its promise. a handler still waits for the next call: running one
needs the frame a call leaves, and an interrupt's is shaped differently.

stopping and continuing are acted on when the signal is sent rather than
delivered. a stopped thread never runs to come back from a call, so a
SIGCONT left pending for it would wait forever, and a loop makes no calls
to stop at. a stop that is caught, ignored or blocked is the exception,
and waits for delivery like any other. the process remembers which signal
stopped it, and ``wait`` with ``WUNTRACED`` tells a parent, once per
stop, which is how a shell learns that ctrl+z happened to its job.

a program sends one with ``sigsend``, and only where it has a say: to its
own processes, or to anybody's when it is root. the kernel raises signals
of its own, ctrl+c and a pipe with no reader among them, and those go
wherever they are due; the rule is about one program reaching another.
a guest able to stop root's programs, init among them, would be a guest
able to stop the machine.

a job is a process group, and a shell makes one with ``setpgid``: itself
or a child of its own, into a group named after it or one that already
has a member of the same user. minus a pgid, given to ``sigsend``, is the
whole group, and reaches every member the sender may signal. -1 is not a
group: elsewhere it means everyone, which is not something this kernel
offers.

init
----

until 0.2.19 ``kmain`` started the shells itself. that works exactly once,
and gives the machine no answer to what order things come up in, what
happens when one dies, who owns an orphan, or what "shut down" means.

so there is a process 1. it brings the machine up in an order, restarts what
dies, and stops restarting what dies too often. adopts processes whose
parent has gone, and takes the machine down **in the reverse of the order it
came up**. that last one is not ceremony: the disk has had a write-back
cache since 0.2.13, so a machine that resets while anything can still write
loses whatever had not reached the drive. everything has to be *stopped*
before the sync, and it can only be stopped in an order if it was started in
one.

a *service* is a kernel thread init is prepared to start again. the four
console sessions are services, and so is the disk flusher; everything else
is a program somebody typed, and a program somebody typed should stay dead
when it ends. the respawn rule: ``INIT_RESPAWN_MAX`` (5) deaths inside
``INIT_RESPAWN_WINDOW_MS`` (10 s) and init gives up. the window matters as
much as the count. five deaths in a second is a loop, five across an
afternoon is five separate accidents.

the *policy* is split from the thread that acts on it (``init_table_*``,
``init_should_restart()``) for the same reason the mouse decoder is split
from the mouse: what is worth testing is the decisions, and a decision that
can only be reached by letting a real service die five times on a real
machine is a decision nobody ever checks.

users
-----

``auth.c`` reads accounts out of a file in the ramdisk. the passwords are
stored in the clear, and that is the honest shape of what a user means here:
storing one properly needs somewhere to write, which this kernel does not
have. the interesting half of an account is not how its password is kept but
what its uid can and cannot reach, and that half is enforced by hardware:
ring 3, an address space of its own, and a kernel that checks a uid before
handing anything over.

``auth_login()`` returns the uid, or -1 if the name *or* the password is
wrong, and does not say which, because saying which tells an attacker half
of it. ``auth_load()`` takes the text directly so the awkward cases.
comments, blank lines, a line with a field missing. can be fed in
deliberately rather than hoped about.

ring 3
------

``usermode.c`` and ``arch/x86_64/{syscall,usermode}.asm`` are the way in and
out of ring 3. ``syscall``/``sysret`` is the fast path; the tss's ``rsp0`` is
the stack a trap lands on. a program gets its own address space (see
`memory <mm.rst>`_), its own process slot, and a set of descriptors. the
syscall interface itself is documented in `the system calls
<../syscalls.rst>`_.

the process interface
---------------------

a process is a program someone started, and it outlives the thread that ran
it. it owns the descriptors, the working directory, the environment and the
signal state, which is what makes ``&``, ``cd`` and ``export`` mean anything:
each of those belongs to the process rather than to the thread or to the
program.

what follows is what each call takes and gives back, and what each field of
the table means.

``#define PROC_ENV_MAX    1024``
    how much environment one process may carry.

``#define MAX_FDS         12``
    how many files one process may hold open.

``#define PROCESS_KILLED  (-1)``
    what the kernel records when a process was killed rather than choosing to go

``enum fd_kind { FD_FREE = 0, FD_CONSOLE,     /* the screen. writes print; reads are meaningless */``
    what a descriptor can be pointing at.

``const uint8_t  *data;       /* in memory */``
    a file in the ramdisk is already in memory, so the descriptor is a bookmark.

``uint32_t        cluster;``
    on disk: where the file starts, and where the record describing it lives, so a write can correct the size afterwards

``size_t          mount;``
    which of the disks it is on.

``uint64_t        medium_at;``
    on the boot medium: where in the source archive its bytes start

``bool            writing;``
    which end of a pipe this is, and for a file whether it was opened to be written to.

``struct fd_disk { size_t   mount;``
    everything a disk-backed descriptor knows about its file

``struct fd_source { uint64_t medium_at;``
    everything a source-backed descriptor knows about its file.

``int      pgid;``
    which job this belongs to.

``bool     announce;``
    whether to narrate this one's comings and goings.

``bool     interrupted;``
    an interrupt has been delivered and not yet looked at.

``struct signal_state sig;``
    the real thing. handlers, a mask, and a pending set

``uint64_t sig_trampoline;``
    where a handler returns to, in the program's own text.

``uint64_t alarm_at;``
    when SIGALRM is due, in milliseconds since boot.

``int      killed_by;``
    how it ended, when it ended by signal rather than by exiting.

``char cwd[PATH_MAX];``
    where this process is standing.

``/* where a process is standing, and moving it. */ const char *process_cwd(int pid);``
    claim a slot. returns the new pid, or 0 if the table is full

``const char *process_cwd(int pid);``
    where a process is standing, and moving it.

``bool process_announces(int pid);``
    should this one's arrival and departure be narrated?

``int  process_uid(int pid);``
    who a process runs as.

``int  process_pgid(int pid);``
    a job is a group, and everything the terminal does it does to a whole one: the keys belong to a group, ctrl+c reaches a group, ctrl+z stops a group

``size_t process_group_threads(int pgid, int *ids, size_t max);``
    the thread ids of everything still running in a group.

``void process_interrupt_group(int pgid);``
    deliver an interrupt to every member. one ctrl+c, the whole job

``bool process_group_alive(int pgid);``
    is anything in the group still going?

``void process_set_thread(int pid, int thread_id);``
    note which thread is running it, once there is one

``int  process_thread(int pid);``
    which thread is running it, or 0.

``void process_exited(int pid, int code, uint64_t now_ms);``
    it finished. the slot stays occupied, holding the code, until somebody collects it

``bool process_collect(int pid, int *code);``
    has it finished? if so, take the code and free the slot.

``int process_orphan(void);``
    the pid of something init should collect, or 0 when there is nothing.

``size_t process_interrupt_all(void);``
    raise the interrupt flag on everything still running, and say how many that was.

``const char *process_name(int pid);``
    what it is called. a forked child takes its parent's name, since it is the same program

``/* replace the whole environment. `len` counts the trailing empty string */ bool process_set_env(int pid, const char *block, size_t len);``
    these work on the block rather than on one variable at a time, because the block is what gets inherited and inheriting is most of what an environment is for

``bool process_set_env(int pid, const char *block, size_t len);``
    replace the whole environment. `len` counts the trailing empty string

``size_t process_get_env(int pid, char *out, size_t max);``
    a copy of it. returns how many bytes, including the terminator

``bool process_env_get(int pid, const char *name, char *out, size_t max);``
    look one variable up.

``bool process_env_set(int pid, const char *name, const char *value);``
    set or replace one. a NULL value removes it

``void process_interrupt(int pid);``
    deliver an interrupt.

``bool process_signal(int pid, int sig);``
    raising one is all any of this does from outside: what happens next is decided when the process next returns to ring 3, by the rules in signal.h. false if there is no such process or no such signal

``struct signal_state *process_signal_state(int pid);``
    the state itself, for the syscalls that install handlers and set masks.

``enum signal_action process_take_signal(int pid, int *sig_out, uint64_t *handler_out);``
    what to do about the signal now due, if any.

``/* set or clear the alarm. */ uint64_t process_set_alarm(int pid, uint64_t seconds, uint64_t now_ms);``
    the process died because of a signal rather than by exiting

``uint64_t process_set_alarm(int pid, uint64_t seconds, uint64_t now_ms);``
    set or clear the alarm.

``void process_check_alarms(uint64_t now_ms);``
    raise SIGALRM on anything whose deadline has passed.

``bool process_interrupt_pending(int pid);``
    is one waiting, unlooked-at? asking does not consume it

``bool process_take_interrupt(int pid);``
    take it, clearing the flag. true if there was one

``/* take a descriptor onto a stretch of ramdisk. */ int  process_fd_open(int pid, const void *data, uint64_t size);``
    the process owns these, so they close themselves when it ends.

``int  process_fd_open(int pid, const void *data, uint64_t size);``
    take a descriptor onto a stretch of ramdisk.

``int  process_fd_open_disk(int pid, uint32_t cluster, uint64_t size, uint64_t entry_sector, uint32_t entry_offset, size_t mount);``
    the same, for a file whose bytes are still on the disk

``bool process_fd_disk(int pid, int fd, struct fd_disk *out);``
    where a disk-backed descriptor has got to. false for a memory one

``int  process_fd_open_source(int pid, uint64_t medium_at, uint64_t size);``
    the same, for a file whose bytes are on the boot medium

``bool process_fd_source(int pid, int fd, struct fd_source *out);``
    where a source-backed descriptor has got to. false for anything else

``void process_fd_grew(int pid, int fd, uint32_t cluster, uint64_t size);``
    a write may have grown the file, or given an empty one its first cluster.

``bool process_fd_peek(int pid, int fd, const void **data, uint64_t *remaining);``
    where the descriptor has got to, and how much is left.

``void process_fd_advance(int pid, int fd, uint64_t n);``
    note that n bytes were taken

``bool process_fd_get(int pid, int fd, struct fd *out);``
    a copy of what a descriptor points at.

``bool process_fd_install(int pid, int fd, const struct fd *src);``
    put something in a numbered slot.

``bool process_fds_inherit(int pid, int from);``
    take every one of `from`'s descriptors, pointing at the same things.

``size_t process_fd_count(int pid);``
    how many a process is holding, for `ps` and tests

``const struct process *process_at(size_t index);``
    walk the table. index from 0; slots that are free are skipped


the signal interface
----------------------

what a program can be told without asking. a signal is a bit rather than a
message: there is no queue and no payload, so raising one that is already
raised changes nothing, and two ctrl+c presses in the same instant are one
interrupt. a process ignores it, takes the default action, or runs a handler,
and the two that may not be caught are what keeps a machine recoverable from
outside.

``#define SIG_DEFAULT 0``
    what a handler value of 0 or 1 means, matching every unix since the seventh edition.

``uint64_t handler[SIGNAL_MAX];``
    SIG_DEFAULT, SIG_IGNORE, or an address in the program

``int      running;``
    what is being run right now, and what the mask was before it.

``int signal_next(const struct signal_state *s);``
    which signal should be delivered now, or 0.

``enum signal_action signal_action_for(const struct signal_state *s, int sig);``
    what to do about `sig`, given this process's handlers

``enum signal_action signal_take(struct signal_state *s, int sig, uint64_t *handler_out);``
    take it off the pending set and, for a handler, block it for the duration.

``void signal_handler_returned(struct signal_state *s);``
    a handler has returned: put the mask back

``bool signal_deliverable(const struct signal_state *s);``
    is there anything to deliver: the question the syscall return path asks, and the one a blocking call asks to decide whether to give up


the auth interface
--------------------

who may log in. the accounts are read out of a file in the ramdisk, and a
name and a password are turned into a uid or a refusal. the shell asks at
login and nothing else does.

``#define AUTH_MAX_ACCOUNTS 8``
    who is allowed in, read out of a file in the ramdisk.

``void auth_init(void);``
    read the accounts out of the ramdisk. safe to call with no ramdisk

``void auth_load(const char *text, size_t len);``
    the parser, given the text directly.

``int auth_login(const char *name, const char *password);``
    returns the uid, or -1 if the name or the password is wrong.


the init interface
--------------------

process 1 and what it supervises. init brings the machine up in an order,
restarts the services that die, stops restarting the ones dying in a loop,
adopts what has been left behind, and takes the machine down again.

the table and the decisions about it are kept apart from the thread that acts
on them, so the decisions can be tested without a machine: a decision that
can only be reached by letting a real service die five times on real hardware
is a decision nobody ever checks.

``#define INIT_SERVICES_MAX  8``
    a service is a kernel thread init is prepared to start again.

``unsigned console;``
    which screen it belongs to.

``bool respawn;``
    should it be started again when it ends?

``enum init_action { INIT_LEAVE,         /* it was never meant to come back */ INIT_RESTART, INIT_GIVE_UP,       /* it is dying in a loop. stop feeding it */ };``
    what to do about a service that has just ended

``void init_table_reset(struct init_table *t);``
    split out from the thread that acts on it for the same reason the mouse decoder is split from the mouse: what is worth testing here is the *decisions*, and a decision that can only be reached by letting a real service die five times on a real machine is a decision nobody ever checks. these three take a table and a clock reading and touch nothing else.

``bool init_add(struct init_table *t, const char *name, void (*entry)(void *),``
    add one. false if the table is full

``void init_started(struct init_table *t, size_t i, int tid, uint64_t now_ms);``
    note that service `i` is now running as thread `tid`

``int init_find(const struct init_table *t, const char *name);``
    find one by name. -1 if there is no such service

``bool init_boot(void);``
    make process 1 and the thread that runs it.

``enum init_stop { INIT_REBOOT, INIT_POWEROFF, };``
    how the machine stops

``void init_stop_machine(enum init_stop how) __attribute__((noreturn));``
    ask init to take the machine down.

``bool init_stopping(void);``
    is the machine on its way down?

``void init_snapshot(struct init_table *out);``
    a copy of the table, for `init` to print.

``bool init_restart(const char *name);``
    restart a service by name, for `init start <name>`.


the sched interface
---------------------

what runs next. the scheduler is portable code right up to the three moments
where a processor has to be involved, and everything here is a ring of
threads, a quantum, and the rules for who gets the cpu when somebody becomes
ready, goes to sleep, or stops existing.

``void sched_init(void);``
    adopts whatever is currently executing as thread 0 and spawns the idle thread.

``void sched_yield(void);``
    give up the rest of the timeslice

``void sleep_ms(uint64_t ms);``
    block for a while. the cpu goes to somebody who can use it

``uint64_t sched_quantum_ms(void);``
    how long a thread gets before it is interrupted, in milliseconds.

``void sched_tick(void);``
    called from the timer irq. counts down the quantum and preempts

``void sched_add(struct thread *t);``
    drop a freshly built thread into the run queue.

``struct waitq { struct thread *head;``
    a place for threads to wait for something that isnt a clock.

``void waitq_block(struct waitq *q);``
    park the running thread until somebody wakes this queue.

``void waitq_enqueue(struct waitq *q);``
    the same thing in two halves, for a caller holding a lock of its own.

``bool sched_join(unsigned cpu, const char *idle_name);``
    a core other than the first, joining the scheduler.

``size_t sched_cores_scheduling(void);``
    how many cores are actually taking work

``const char *sched_cpu_running(unsigned cpu);``
    the name of whatever a given core is running this instant, for `cpus`

``void waitq_wake_all(struct waitq *q);``
    wake everyone parked on the queue. safe to call from an irq

``void sched_dump(void);``
    walk the run queue (for the `ps` command in m6)

``void sched_show_threads(void);``
    the run queue and any pipe still open: what ``ps`` prints, and what
    ``/proc/threads`` is, so a program reading the file and a person at
    the shell see the same table

``void sched_proc_init(void);``
    registers ``/proc/threads``, for anyone, and ``/proc/stack``, for
    root: the kernel's call trace of whoever reads it, which is the
    walker a panic uses with nothing on fire, and which names functions
    by where the kernel keeps them

``size_t sched_thread_count(void);``
    how many threads are in the ring, dead ones included

``bool sched_thread_alive(int id);``
    is there still a thread with this id that has not finished?

``enum sched_kill_result sched_kill(int id);``
    mark a thread dead so the reaper collects it.

``void sched_wake_thread(int id);``
    make a thread runnable wherever it is: asleep, or parked on a queue.

``void sched_set_stopped(int id, bool stopped);``
    stop a thread, or let it go again.

``void waitq_remove(struct waitq *q, struct thread *t);``
    take one thread off a queue without waking it.


the spinlock interface
------------------------

locks. turning interrupts off on this core says nothing about a thread on
*that* one, so a critical section that has to be safe from both takes a lock
and stops interrupts, which is what ``spin_lock_irq`` does in one call.

every lock declares a rank and may only be taken while holding locks of a
lower one. two locks taken in opposite orders by two cores is a machine that
stops with no fault and nothing printed, so the order is checked rather than
hoped for.

``LOCK_RANK_CLOCK, LOCK_RANK_SCHED,            /* the run queue */ LOCK_RANK_PROCESS,          /* the process table, which sched reaches into */ LOCK_RANK_HEAP,             /* slab, and kmalloc above it */ LOCK_RANK_PMM,              /* which the heap calls into, never the reverse */ LOCK_RANK_PRINT,            /* anything may print; printing takes nothing */``
    the clock, which is a device that other devices ask.

``volatile uint32_t owner;``
    which core has it, for saying something useful when it goes wrong.

``volatile uint64_t contended;``
    how often somebody had to wait.

``volatile uint32_t listed;``
    whether this one is in the list the shell shows.

``void spin_abandon_all(void);``
    once the machine is dying, locks are in the way rather than any use: a panic while holding one would take the print lock, find it already held by this very core, and panic about that instead. called first thing by panic()

``size_t spin_count(void);``
    for the shell, and for deciding what to split up later


the thread interface
----------------------

a thread: where the cpu left it, how much it has run, who it belongs to, and
whether it is runnable, waiting or gone. everything about a parked thread
lives on its own stack, so a saved thread is one word, and that is what the
scheduler is built on rather than an implementation detail of it.

``uint64_t sp;``
    the saved stack pointer.

``bool     stopped;``
    suspended by ctrl+z, and not to be picked until somebody says otherwise.

``uint64_t cpu_ticks;``
    how many timer ticks this thread was the one running when the timer went off.

``unsigned console;``
    which screen this thread's output goes to, and which keyboard it may read.

``int  on_cpu;``
    which core this is on, or -1 for none.

``bool from_heap;``
    the boot thread is a static, everything else came from kmalloc.

``struct waitq  *waiting_on;``
    the queue this thread is parked on, and the next one along it.

``int pid;``
    which program this thread is running, or 0 for a kernel thread.

``void thread_set_name(struct thread *t, const char *name);``
    rename a thread in place.

``struct thread *thread_create(const char *name, void (*entry)(void *), void *arg);``
    build a thread that will start life inside entry(arg).

``struct thread *thread_create_parked(const char *name, void (*entry)(void *),``
    the same, but parked: it goes into the ring already blocked, so no other core can pick it up before its own has claimed it. a core building its idle thread needs exactly this, between creating one and saying "this is the kernel's", a ready thread is fair game to anybody

``void thread_free(struct thread *t);``
    give a dead thread's struct back.

``void thread_free_stack(struct thread *t);``
    hand a dead thread's stack back to the pmm, guard page and all


the usermode interface
------------------------

entering ring 3, and everything that has to be true before a program runs:
the redirect that gives it its descriptors and its environment, the stack and
the trampoline it starts on, and the path back in through a trap.

it is also where the ABI lives, since this is the code that decides what a
program is handed when it starts and what it finds when it asks for
something.

``#define USER_STACK_PAGES 256            /* a megabyte of room */``
    how big a stack ring 3 may grow to, and how much of it exists before the program starts.

``#define USER_STACK_TOP 0x0000700000000000ull``
    where user stacks go.

``#define MAX_ARGS 8``
    how many arguments a program may be handed.

``extern const char *const USER_RUN_NO_SUCH_FILE;``
    the shell wants to say something more useful than this particular message, so it is a value it can compare against rather than prose

``struct spawn_env { const char *block;``
    the environment a program is born holding. NULL means an empty one

``int user_spawn(const char *path, int argc, const char *const argv[], const char *cwd, int parent, int uid, bool announce, const struct spawn_io *io, const char **error);``
    start a program and return its pid, or 0 with ``*error`` set.

``void user_spawn_env(const struct spawn_env *env);``
    what the next spawn should hand its child.

``bool user_exec(const char *str_path, int i_argc, const char *const pp_argv[], struct user_entry *p_out, const char **pp_error);``
    the calling process becomes the program at ``str_path``, keeping its
    pid, descriptors, working directory, uid and environment, with its
    caught signals back to their defaults. true with ``p_out`` saying
    where the new program starts; false with nothing changed at all

spawn and exec share everything up to the program being ready to run:
the image read and described or loaded, a fresh address space, a stack
and the arguments on it. what differs is what goes around it. spawn
makes a new process and a thread for it. exec swaps the new address
space in for the caller's and destroys the old one, and only after
everything that can fail has already succeeded, so a failed exec leaves
the caller running exactly what it was. the ``execve`` call then
rewrites the frame the syscall exit path restores, so the return from
the call is the new program's first instruction.

``#define PIPELINE_MAX 4``
    one command in a pipeline: already resolved to a path, with the arguments it was typed with and whatever redirection was written beside it

``struct job { int  pgid;``
    a job: everything one typed line started, held together by a group number so the terminal can talk to all of it at once

``int  status;``
    what the last of them exited with.

``bool user_job_wait(struct job *j);``
    wait for a job to end, or to be stopped, which is the other way waiting can finish.

``void user_job_continue(struct job *j, bool foreground);``
    let a stopped job go again, with or without the terminal.

``bool user_job_alive(const struct job *j);``
    is anything in it still going?

``bool user_pipeline(const struct stage *stages, int count, const char *cwd, int uid, bool background, struct job *out, const char **error);``
    run `count` commands with a pipe between each neighbouring pair, and wait for all of them.

``bool user_wait(int pid, int *code);``
    block until a pid has ended, then collect it.

``bool user_run(const char *path, int argc, const char *const argv[], const char *cwd, int uid, bool background, bool announce, struct job *out, const char **error);``
    the shell's way in: spawn, and unless told otherwise wait for it and report how it went
