// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_ksyms.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the symbol lookup: given an address, which function is it in and how far
 * in.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "lib/ksyms.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

/* a fabricated kernel: four functions, one of them zero-length-ish */
static const char names[] = "alpha\0beta\0gamma\0delta";
static const struct ksym table[] = {
    { 0x1000, 0  },     /* alpha */
    { 0x1100, 6  },     /* beta  */
    { 0x1180, 11 },     /* gamma */
    { 0x2000, 17 },     /* delta */
};
#define COUNT (sizeof(table) / sizeof(table[0]))

static void check_at(uint64_t addr, const char *want_name, uint64_t want_off,
                     const char *what)
{
    uint64_t off = 0xdeadbeef;
    const char *got = ksym_lookup_in(table, COUNT, names, addr, &off);

    if (want_name == NULL) {
        if (got != NULL) {
            printf("FAIL %s: %#lx gave '%s', expected nothing\n", what, addr, got);
            failures++;
        }
        return;
    }
    if (got == NULL) {
        printf("FAIL %s: %#lx gave nothing, wanted %s\n", what, addr, want_name);
        failures++;
        return;
    }
    if (strcmp(got, want_name) != 0 || off != want_off) {
        printf("FAIL %s: %#lx gave %s+%#lx, wanted %s+%#lx\n",
               what, addr, got, off, want_name, want_off);
        failures++;
    }
}

int main(void)
{
    /* dead on a function's first instruction */
    check_at(0x1000, "alpha", 0, "the very start of the first function");
    check_at(0x1100, "beta",  0, "the start of a middle function");
    check_at(0x2000, "delta", 0, "the start of the last function");

    /* somewhere in the middle, which is the normal case */
    check_at(0x1042, "alpha", 0x42, "inside the first function");
    check_at(0x1101, "beta",  1,    "one byte into a function");
    check_at(0x117f, "beta",  0x7f, "the last byte before the next symbol");
    check_at(0x1181, "gamma", 1,    "just past a boundary");

    /* below everything the test knows about */
    check_at(0x0fff, NULL, 0, "one byte below the first function");
    check_at(0,      NULL, 0, "a null pointer");

    /* above the last function. */
    check_at(0x9000, "delta", 0x7000, "far past the end reads as a huge offset");

    /* an empty table must not walk off anything */
    uint64_t off = 0;
    CHECK(ksym_lookup_in(table, 0, names, 0x1000, &off) == NULL,
          "an empty table finds nothing");

    /* a single-entry table exercises the search with lo == hi */
    CHECK(strcmp(ksym_lookup_in(table, 1, names, 0x1234, &off), "alpha") == 0
          && off == 0x234, "a one-entry table still works");
    CHECK(ksym_lookup_in(table, 1, names, 0x100, &off) == NULL,
          "and still says no below its one symbol");

    /* the caller is allowed not to care about the offset */
    CHECK(strcmp(ksym_lookup_in(table, COUNT, names, 0x1042, NULL),
                 "alpha") == 0, "a NULL offset pointer is fine");

    /*
     * every address across a range must land in the right function,
     * this is the sweep that catches an off-by-one in the bias
     */
    for (uint64_t a = 0x1000; a < 0x1200; a++) {
        const char *want = a < 0x1100 ? "alpha" : (a < 0x1180 ? "beta" : "gamma");
        const char *got = ksym_lookup_in(table, COUNT, names, a, &off);
        if (got == NULL || strcmp(got, want) != 0) {
            printf("FAIL sweep: %#lx gave %s, wanted %s\n",
                   a, got ? got : "(none)", want);
            failures++;
            break;
        }
    }

    if (!failures) printf("all good\n");
    return failures;
}
