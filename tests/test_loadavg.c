// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_loadavg.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the load average's arithmetic, against what the averages must do.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <velvet/loadavg.h>

static int s_i_failures;

#define CHECK(cond, what) do {                                          \
        if (!(cond)) {                                                  \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);     \
            s_i_failures++;                                             \
        }                                                               \
    } while (0)

/* an average, as it is shown, into a number of hundredths */
static uint64_t shown(uint64_t u_load)
{
    uint64_t u_whole;
    uint64_t u_hundredths;
    load_digits(u_load, &u_whole, &u_hundredths);
    return u_whole * 100 + u_hundredths;
}

/* u_folds of one count into an average that starts at u_from */
static uint64_t folded(uint64_t u_from, size_t sz_span, uint64_t u_active,
                       int i_folds)
{
    uint64_t u_load = u_from;
    for (int i_at = 0; i_at < i_folds; i_at++) {
        u_load = load_fold(u_load, load_decay(sz_span), u_active);
    }
    return u_load;
}

int main(void)
{
    CHECK(load_decay(0) == 1884 && load_decay(1) == 2014
          && load_decay(2) == 2037 && load_decay(3) == 0,
          "the weights an average keeps, and no fourth average");

    CHECK(folded(0, 0, LOAD_ONE, 1) == 164,
          "one busy thread for five seconds is the minute's 164/2048");
    CHECK(shown(folded(0, 0, LOAD_ONE, 1)) == 8,
          "which is shown as 0.08");
    /* a minute of it is 1 - 1/e of a thread, 0.63 */
    CHECK(shown(folded(0, 0, LOAD_ONE, 12)) == 63,
          "a minute of one busy thread is 0.63 on the minute's average");
    CHECK(shown(folded(0, 1, LOAD_ONE, 12)) == 18
          && shown(folded(0, 2, LOAD_ONE, 12)) == 6,
          "and less on the five minutes' and the fifteen's");
    CHECK(folded(0, 0, LOAD_ONE, 200) == LOAD_ONE
          && folded(0, 2, 3 * LOAD_ONE, 2000) == 3 * LOAD_ONE,
          "a load that stays put is reached, not only approached");
    CHECK(folded(LOAD_ONE, 0, LOAD_ONE, 1) == LOAD_ONE,
          "and kept");
    CHECK(folded(LOAD_ONE, 0, 0, 12) < LOAD_ONE / 2
          && folded(LOAD_ONE, 0, 0, 300) == 0,
          "and nothing to do lets it fall away to nothing");

    CHECK(shown(0) == 0 && shown(LOAD_ONE) == 100
          && shown(5 * LOAD_ONE / 2) == 250,
          "an average is shown as whole threads and hundredths");
    CHECK(shown(1023) == 50 && shown(20) == 1,
          "rounded to the nearest hundredth, not cut");

    if (s_i_failures == 0) {
        printf("all good\n");
    }
    return s_i_failures;
}
