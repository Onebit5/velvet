// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_switch.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the context switch, exercised for real in userspace.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

extern void switch_context(uint64_t *save_rsp, uint64_t *load_rsp);

static uint64_t main_rsp, thread_rsp;
static volatile int reached;
static int failures;

#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

/*
 * results land in globals, not locals: gcc addresses locals off rbp at
 * -O0 and the register test below deliberately stomps rbp, which would
 * send the stores somewhere exciting (learned the hard way)
 */
uint64_t rbx_out, r12_out, r13_out, r14_out, r15_out, rbp_out;

/* stands in for thread_bootstrap */
static void fake_thread(void)
{
    reached = 1;
    switch_context(&thread_rsp, &main_rsp);   /* hand the cpu back */
    reached = 2;                              /* the test got resumed */
    switch_context(&thread_rsp, &main_rsp);
    reached = 99;                             /* must never happen */
    for (;;) { }
}

static void build_stack(void *stack_top, void (*entry)(void))
{
    uint64_t *sp = (uint64_t *)stack_top;
    *--sp = 0;                    /* fake return address */
    *--sp = (uint64_t)entry;      /* what switch_context's ret jumps to */
    *--sp = 0;                    /* rbp */
    *--sp = 0;                    /* rbx */
    *--sp = 0;                    /* r12 */
    *--sp = 0;                    /* r13 */
    *--sp = 0;                    /* r14 */
    *--sp = 0;                    /* r15 */
    thread_rsp = (uint64_t)sp;
}

int main(void)
{
    void *stack = aligned_alloc(4096, 64 * 1024);
    void *top = (uint8_t *)stack + 64 * 1024;


    build_stack(top, fake_thread);
    CHECK(reached == 0, "the thread has not run yet");
    switch_context(&main_rsp, &thread_rsp);
    CHECK(reached == 1, "a fabricated stack lands in the entry function");


    switch_context(&main_rsp, &thread_rsp);
    CHECK(reached == 2, "a parked thread resumes where it left off");

    /* --- do the callee-saved registers actually survive? */
    build_stack(top, fake_thread);
    reached = 0;

    asm volatile (
        "movq $0xbbbb, %%rbx\n\t"
        "movq $0x1212, %%r12\n\t"
        "movq $0x1313, %%r13\n\t"
        "movq $0x1414, %%r14\n\t"
        "movq $0x1515, %%r15\n\t"
        "pushq %%rbp\n\t"
        "movq $0xdddd, %%rbp\n\t"
        "callq switch_context\n\t"
        "movq %%rbx, rbx_out(%%rip)\n\t"
        "movq %%r12, r12_out(%%rip)\n\t"
        "movq %%r13, r13_out(%%rip)\n\t"
        "movq %%r14, r14_out(%%rip)\n\t"
        "movq %%r15, r15_out(%%rip)\n\t"
        "movq %%rbp, rbp_out(%%rip)\n\t"
        "popq %%rbp\n\t"
        :
        : "D"(&main_rsp), "S"(&thread_rsp)
        : "rbx", "r12", "r13", "r14", "r15", "rax", "rcx", "rdx",
          "r8", "r9", "r10", "r11", "memory");

    CHECK(reached == 1, "the second thread ran");
    CHECK(rbx_out == 0xbbbb, "rbx survived the switch");
    CHECK(r12_out == 0x1212, "r12 survived the switch");
    CHECK(r13_out == 0x1313, "r13 survived the switch");
    CHECK(r14_out == 0x1414, "r14 survived the switch");
    CHECK(r15_out == 0x1515, "r15 survived the switch");
    CHECK(rbp_out == 0xdddd, "rbp survived the switch");

    if (!failures) printf("all good\n");
    return failures;
}
