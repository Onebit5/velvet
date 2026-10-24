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
``struct waitq``. the keyboard has one, which is how a program sits at a
prompt costing zero cpu until you press a key.

the subtle part is the handoff: ``waitq_block()`` **must be entered with
interrupts off and returns with them still off**, so that "look in the
buffer, find it empty, go to sleep" is one atomic move. get that wrong and a
key arriving in the gap between the check and the sleep is lost forever, and
the reader waits for something that already happened.

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

pid 0 is not a process at all; it is the parent of what the kernel starts
itself, which is ``/init``. pid 1 is init, handed out first so reparenting
has a number that is known before the process it names exists.

the process table (``process.c``) is where ``ps``, ``wait``, ``kill`` and
reparenting all bottom out. children are reparented to init as their parent
goes, inside ``process_exited()``, since the moment the parent ends is the
only moment anybody could notice, and a child born to a parent that has
already ended, or is not there at all, is init's from birth. so every
orphan has init for its parent, and init finds them all with ``wait``.

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

signals are numbered the way every x86_64 program numbers them: 1 to 31
with their usual meanings, SIGINT 2, SIGTERM 15, SIGCHLD 17 and the rest,
and 32 to 64 left for a program to give a meaning to. a set of them holds
signal n at bit n - 1 of a 64 bit word, which is a sigset_t's layout.

a signal in this kernel is **a bit in a word**, not a message: there is no
queue and no payload, and raising one that is already raised changes
nothing. two ctrl+c presses in the same instant are one interrupt, which is
exactly right and is the first thing people expect to be wrong. that also
means signals cannot be counted, and any design that wants to count them
wants something else.

a process ignores a signal, takes the default action, or runs a handler.
the default is nearly always "die", and the exceptions are the interesting
ones. SIGCHLD arrives constantly and a default of death would mean every
shell exiting the moment a command finished; SIGWINCH and SIGURG are news,
and are ignored too. SIGTSTP, SIGTTIN and SIGTTOU stop, and SIGCONT
continues. SIGKILL and SIGSTOP may not be
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

signals are delivered on every way back to ring 3, from a call, an
interrupt or a fault, since each leaves the same frame and the kernel has
the program's registers to hand in all three. a program busy in a loop
makes no calls, and the timer's next tick delivers to it all the same,
handler included. SIGKILL ending anything at all is the whole of its
promise.

a fault in ring 3 is raised as a signal against the program that made
it: SIGSEGV for memory, SIGILL for an instruction the cpu will not run,
SIGFPE for arithmetic, SIGTRAP for a breakpoint and SIGBUS for an
unaligned access the program asked to hear about. a handler may catch
it, and nothing may ignore or block it: one that is goes back to its
default first, since returning to the instruction that faulted would
fault again forever. the log says which program, which fault and where.

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

so there is a process 1, and it is the medium's: the program ``/init``. a
kernel is not a system. which consoles get a login, what a session is, what
happens when one ends and who collects an orphan are the system's to
decide, and every orphan is handed to ``/init``. a medium with no ``/init``
is refused at boot, with a panic that says why, rather than given a login
the kernel made up.

the kernel keeps its own half in a thread named ``services``. it brings the
kernel's services up in an order: the flusher, then the network. it restarts what dies, stops restarting what dies too
often, and takes the machine down **in the reverse of the order it came
up**. that last one is not ceremony: the disk has had a write-back cache
since 0.2.13, so a machine that resets while anything can still write loses
whatever had not reached the drive. everything has to be *stopped* before
the sync, and it can only be stopped in an order if it was started in one.

``/init`` is made first, parked, so that it is pid 1, and let go only once
the flusher is up, in front of console 0 so that what it says before it
takes a console of its own is seen. if it ever ends, the kernel panics:
nothing is left to start a session, take an orphan or bring the machine
down, and a machine that only looks alive is worse than one that says so.

a program asks for the road down with ``reboot``, root's alone, and it is
the only road down there is: every program but init first, then the
services in the reverse of their order, then the disk written out and its
journal closed. init is left waiting in the call, which is where an
init that asked is meant to be.

a *service* is a kernel thread the ``services`` thread is prepared to start
again: the flusher and the network. everything else is a program, and a
program is ``/init``'s to start again or not. the respawn
rule: ``INIT_RESPAWN_MAX`` (5) deaths inside ``INIT_RESPAWN_WINDOW_MS``
(10 s) and it gives up. the window matters as much as the count. five
deaths in a second is a loop, five across an afternoon is five separate
accidents.

the *policy* is split from the thread that acts on it (``init_table_*``,
``init_should_restart()``) for the same reason the mouse decoder is split
from the mouse: what is worth testing is the decisions, and a decision that
can only be reached by letting a real service die five times on a real
machine is a decision nobody ever checks.

users
-----

a user is a uid, and the kernel knows nothing else about one. names,
passwords and who may log in are the system's, kept wherever its login
looks for them. what the kernel holds to is what a uid can and cannot
reach, and that half is enforced by hardware: ring 3, an address space of
its own, and a kernel that checks a uid before handing anything over.

a login in ring 3 checks the password itself and then becomes the user
with ``setuid``. root may become anybody and anyone else only who they
already are, so the one way to a uid of 0 is to have it already, and a
login that has become a guest cannot come back.

ring 3
------

``usermode.c`` and ``arch/x86_64/{syscall,usermode}.asm`` are the way in and
out of ring 3. a call comes in by ``syscall``, an interrupt or a fault through
the idt, onto the stack the tss's ``rsp0`` names, and each leaves the same
frame, ``struct pt_regs``. every way back out is ``iretq`` from that frame, and
on the way, a pending signal is delivered, from a call, an interrupt or a
fault alike. the x87 and sse registers and the fs base are the program's
too, saved into the thread on every switch, copied by ``fork``, set to
what a new program expects by ``exec``, and set aside for a signal
handler, which starts with a fresh set. a fault in ring 3 is the
program's: it becomes its signal,
SIGSEGV, SIGILL, SIGFPE, SIGTRAP or SIGBUS, which a handler may catch and
which nothing may ignore or block, and by default the program dies of it. a program gets its own address space (see
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

``#define MAX_FDS         64``
    how many descriptors one process has.

``#define PROCESS_KILLED  (-1)``
    what the kernel records when a process was killed rather than choosing to go

``struct file *fds[MAX_FDS];``
    what each descriptor is open on, NULL for one that is not. the file is
    shared with whatever descriptors were duplicated or forked from this one.

``uint64_t u_cloexec;``
    bit n: descriptor n closes when the process runs another program.

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

``void process_set_thread(int pid, int thread_id);``
    note which thread is running it, once there is one

``int  process_thread(int pid);``
    which thread is running it, or 0.

``void process_exited(int pid, int code, uint64_t now_ms);``
    it finished. the slot stays occupied, holding the code, until somebody collects it

``bool process_collect(int pid, int *code);``
    has it finished? if so, take the code and free the slot.

``size_t process_interrupt_all(void);``
    raise the interrupt flag on everything still running, and say how many that was.

``const char *process_name(int pid);``
    what it is called. a forked child takes its parent's name, since it is the same program

``void process_inherit(int i_child, int i_parent);``
    a forked child made its parent's copy: the group, the working
    directory, the environment, the signal dispositions and mask, and the
    trampoline its handlers return through. not what is pending: a child
    is not born owing its parent's interrupts

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

``int process_fd_add(int i_pid, struct file *p_file, int i_from, bool b_cloexec);``
    a file at the lowest descriptor free from i_from up, keeping the
    caller's reference: the descriptor, or -EMFILE.

``struct file *process_fd_file(int i_pid, int i_fd);``
    the file a descriptor is open on, with a reference to put, or NULL.

``bool process_fd_replace(int i_pid, int i_fd, struct file *p_file, bool b_cloexec, struct file **pp_old);``
    a file at a given descriptor, whatever was there handed back to be put.

``struct file *process_fd_take(int i_pid, int i_fd);``
    a descriptor closed: the file it was open on, to be put.

``bool process_fds_inherit(int pid, int from);``
    the child holds the parent's files, not copies of them.

``void process_fds_release(int i_pid);``
    every descriptor closed, as a process ends; every way a process ends
    calls it, with no lock held, since a pipe's end closing wakes threads.

``void process_fds_exec(int i_pid);``
    the descriptors marked close-on-exec closed.

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
    the run queue and any pipe still open, which is what ``/proc/threads``
    is

``void sched_proc_init(void);``
    registers ``/proc/threads`` and ``/proc/environ``, for anyone, and
    ``/proc/stack``, for root. ``environ`` is the reader's own
    environment, each ``NAME=value`` ended by a NUL, which is how a
    program learns which variables there are when it can only ask for
    one by name. ``stack`` is the kernel's call trace of whoever reads
    it, which is the walker a panic uses with nothing on fire, and which
    names functions
    by where the kernel keeps them

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
    whether this one is in the registry of every lock.

``void spin_abandon_all(void);``
    once the machine is dying, locks are in the way rather than any use: a panic while holding one would take the print lock, find it already held by this very core, and panic about that instead. called first thing by panic()

``size_t spin_count(void);``
    how many locks are registered, for deciding what to split up later


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

``int user_spawn(const char *path, int argc, const char *const argv[], const char *cwd, int parent, int uid, bool announce, const char **error);``
    start a program, parked until ``user_start``, holding its parent's
    environment, and return its pid, or 0 with ``*error`` set.

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
