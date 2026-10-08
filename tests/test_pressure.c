// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_pressure.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for who gets refused when memory runs out.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "mm/pressure.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    const uint64_t R = PRESSURE_RESERVE;



    CHECK(pressure_allow(10000, 100, PRESSURE_USER), "a program may take");
    CHECK(pressure_allow(10000, 100, PRESSURE_KERNEL), "and so may the kernel");
    CHECK(pressure_allow(10000, 0, PRESSURE_USER), "asking for nothing is fine");



    CHECK(pressure_allow(R + 100, 100, PRESSURE_USER),
          "a request that leaves exactly the reserve is allowed");
    CHECK(!pressure_allow(R + 100, 101, PRESSURE_USER),
          "and one that leaves a single page less is not, the reserve "
          "is what would be *left*, not what is there now, and checking "
          "the wrong one still lets the machine below the line");

    CHECK(!pressure_allow(R, 1, PRESSURE_USER),
          "at the reserve, a program may take nothing at all");
    CHECK(pressure_allow(R, 1, PRESSURE_KERNEL),
          "while the kernel may, which is the point of holding it back: "
          "the machine still has enough to write the file, print the "
          "message and reap the process");
    CHECK(pressure_allow(R, R, PRESSURE_KERNEL),
          "and may take all of it");



    CHECK(!pressure_allow(100, 200, PRESSURE_USER),
          "more than there is, is refused");
    CHECK(!pressure_allow(100, 200, PRESSURE_KERNEL),
          "including for the kernel, a reserve does not conjure pages");
    CHECK(!pressure_allow(0, 1, PRESSURE_KERNEL), "and nothing left is nothing");

    /* a request so large it would wrap a subtraction done the naive way */
    CHECK(!pressure_allow(1000, (size_t)-1, PRESSURE_USER),
          "an absurd request is refused rather than wrapping into a small "
          "one that fits");



    CHECK(pressure_available(10000) == 10000 - R,
          "what is available to a program is the free count less the "
          "reserve, which is not the same number a `free` command would "
          "print");
    CHECK(pressure_available(R) == 0, "at the reserve, nothing is available");
    CHECK(pressure_available(R - 1) == 0,
          "and below it, still nothing rather than a number that wrapped");
    CHECK(pressure_available(0) == 0, "with none free, none available");

    if (failures == 0) printf("all good\n");
    return failures;
}
