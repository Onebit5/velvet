// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_auth.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * reading the accounts.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "sched/auth.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

static void load(const char *text)
{
    auth_load(text, strlen(text));
}

int main(void)
{

    load("# who may enter\n"
         "#\n"
         "# name:password:uid:description\n"
         "\n"
         "igor:velvet:0:master of the velvet room\n"
         "guest:guest:1000:a visitor, and treated as one\n");

    CHECK(auth_count() == 2, "two accounts, and the comments are not accounts");
    CHECK(auth_login("igor", "velvet") == 0, "the master gets uid 0");
    CHECK(auth_login("guest", "guest") == 1000, "and a guest gets its own");


    CHECK(auth_login("igor", "margaret") == -1, "a wrong password is refused");
    CHECK(auth_login("igor", "") == -1, "and an empty one");
    CHECK(auth_login("igor", "velve") == -1, "and a prefix of the right one");
    CHECK(auth_login("igor", "velvett") == -1, "and one with something extra");
    CHECK(auth_login("nobody", "velvet") == -1, "an unknown name is refused");
    CHECK(auth_login("", "") == -1, "and so is nothing at all");
    CHECK(auth_login("IGOR", "velvet") == -1, "names are not case-insensitive");

    CHECK(auth_find("igor") != NULL, "an account can be looked up");
    CHECK(auth_find("nobody") == NULL, "and one that is not there cannot");
    CHECK(strcmp(auth_name_for(0), "igor") == 0, "a uid maps back to a name");
    CHECK(strcmp(auth_name_for(4242), "somebody") == 0,
          "and an unknown uid gets something harmless rather than a crash");


    load("igor:velvet:0:master\n"
         "broken-no-uid:pw\n"                    /* missing fields */
         ":nameless:5:has no name\n"             /* no name */
         "bad:pw:notanumber:uid is not a number\n"
         "guest:guest:1000:fine\n");
    CHECK(auth_count() == 2,
          "a line missing a field is skipped rather than half-read, a "
          "passwd file letting somebody in on a guess is the worst thing "
          "this code could do");
    CHECK(auth_login("broken-no-uid", "pw") == -1, "the broken line is nobody");
    CHECK(auth_login("bad", "pw") == -1, "and neither is the one with a bad uid");
    CHECK(auth_login("guest", "guest") == 1000, "while the good ones still work");


    load("");
    CHECK(auth_count() == 0, "an empty file has no accounts");
    CHECK(auth_login("igor", "velvet") == -1, "and lets nobody in");

    load("# nothing but comments\n# and more of them\n");
    CHECK(auth_count() == 0, "a file of comments has no accounts either");

    auth_load(NULL, 0);
    CHECK(auth_count() == 0, "no file at all is survivable");
    CHECK(auth_login("igor", "velvet") == -1, "and still lets nobody in");

    /* a last line with no newline is the normal way files end */
    load("igor:velvet:0:master");
    CHECK(auth_count() == 1 && auth_login("igor", "velvet") == 0,
          "a final line without a newline still counts");

    /* a description is optional */
    load("plain:word:7:\n");
    CHECK(auth_login("plain", "word") == 7, "a missing description is fine");

    /* more accounts than the table holds must stop, not overflow */
    {
        char big[1024] = "";
        for (int i = 0; i < AUTH_MAX_ACCOUNTS + 5; i++) {
            char line[64];
            snprintf(line, sizeof line, "user%d:pw:%d:x\n", i, i);
            strcat(big, line);
        }
        load(big);
        CHECK(auth_count() == AUTH_MAX_ACCOUNTS,
              "the table fills to its size and stops");
        CHECK(auth_login("user0", "pw") == 0, "the ones that fitted work");
    }

    /*
     * a name or password longer than the field is truncated, and the
     * truncated form must not then match something shorter
     */
    {
        char line[256];
        snprintf(line, sizeof line,
                 "%.*s:pw:3:x\n", AUTH_NAME_MAX + 10,
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        load(line);
        CHECK(auth_count() == 1, "an overlong name still parses");
        CHECK(auth_login("a", "pw") == -1, "and does not match a short one");
    }

    if (!failures) printf("all good\n");
    return failures;
}
