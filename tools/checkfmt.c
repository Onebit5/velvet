// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/checkfmt.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * every kprintf format string, against what kprintf can actually do.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>

/* what lib/kprintf.c implements, and nothing else */
static const char *supported_flags = "-0";
static const char *supported_conv = "csdiuxp%";
static const char *supported_length[] = { "", "l", "ll", "z", NULL };

static int problems;

static bool known_length(const char *length)
{
    for (int i = 0; supported_length[i] != NULL; i++) {
        if (strcmp(length, supported_length[i]) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * one `%...` at `p`. returns how many characters it took, or 0 if it is
 * not a specifier at all, inline asm is full of `%` and none of it is
 * a format string, which is why only lines that *call* something are
 * looked at in the first place
 */
static size_t one_spec(const char *p, const char *path, int lineno)
{
    const char *start = p;
    p++;                        /* past the % */

    char flags[8] = "";
    size_t nflags = 0;
    while (*p != '\0' && strchr("-+ #0", *p) != NULL) {
        if (nflags + 1 < sizeof flags) {
            flags[nflags++] = *p;
        }
        p++;
    }
    flags[nflags] = '\0';

    while (*p >= '0' && *p <= '9') {
        p++;                    /* a width, which the formatter does take */
    }

    char precision[8] = "";
    if (*p == '.') {
        size_t n = 0;
        while (*p != '\0' && (*p == '.' || (*p >= '0' && *p <= '9'))) {
            if (n + 1 < sizeof precision) {
                precision[n++] = *p;
            }
            p++;
        }
        precision[n] = '\0';
    }

    char length[4] = "";
    static const char *lengths[] = { "hh", "ll", "h", "l", "z", NULL };
    for (int i = 0; lengths[i] != NULL; i++) {
        size_t n = strlen(lengths[i]);
        if (strncmp(p, lengths[i], n) == 0) {
            snprintf(length, sizeof length, "%s", lengths[i]);
            p += n;
            break;
        }
    }

    char conv = *p;
    if (conv == '\0'
        || !((conv >= 'a' && conv <= 'z') || (conv >= 'A' && conv <= 'Z')
             || conv == '%')) {
        return 0;               /* not a specifier: a stray percent */
    }
    p++;

    if (conv == '%') {
        return (size_t)(p - start);
    }

    char why[192] = "";
    size_t at = 0;
    for (size_t i = 0; i < nflags; i++) {
        if (strchr(supported_flags, flags[i]) == NULL) {
            at += (size_t)snprintf(why + at, sizeof why - at, "%sflag '%s'",
                                   at ? ", " : "", flags);
            break;
        }
    }
    if (precision[0] != '\0') {
        at += (size_t)snprintf(why + at, sizeof why - at,
                               "%sprecision '%s'", at ? ", " : "", precision);
    }
    if (!known_length(length)) {
        at += (size_t)snprintf(why + at, sizeof why - at, "%slength '%s'",
                               at ? ", " : "", length);
    }
    if (strchr(supported_conv, conv) == NULL) {
        at += (size_t)snprintf(why + at, sizeof why - at,
                               "%sconversion '%c'", at ? ", " : "", conv);
    }

    if (at > 0) {
        fprintf(stderr, "%s:%d: kprintf cannot handle '%.*s': unsupported %s\n",
                path, lineno, (int)(p - start), start, why);
        problems++;
    }
    return (size_t)(p - start);
}

static void scan(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    char line[4096];
    int lineno = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        lineno++;
        if (strstr(line, "kprintf(") == NULL && strstr(line, "panic(") == NULL) {
            continue;
        }
        for (const char *p = line; *p != '\0'; ) {
            if (*p != '%') {
                p++;
                continue;
            }
            size_t took = one_spec(p, path, lineno);
            p += took > 0 ? took : 1;
        }
    }
    fclose(f);
}

/*
 * every .c and .h under here, in sorted order, so two runs on the same
 * tree print the same problems in the same order, which is what makes
 * the output diffable against the python's
 */
static void walk(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return;
    }
    char names[2048][256];
    size_t count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < 2048) {
        if (e->d_name[0] == '.') {
            continue;
        }
        snprintf(names[count++], 256, "%s", e->d_name);
    }
    closedir(d);

    for (size_t i = 1; i < count; i++) {
        char keep[256];
        memcpy(keep, names[i], sizeof keep);
        size_t j = i;
        while (j > 0 && strcmp(names[j - 1], keep) > 0) {
            memcpy(names[j], names[j - 1], sizeof keep);
            j--;
        }
        memcpy(names[j], keep, sizeof keep);
    }

    for (size_t i = 0; i < count; i++) {
        char path[1024];
        /* "." is left off the front, the way pathlib leaves it off */
        if (strcmp(dir, ".") == 0) {
            snprintf(path, sizeof path, "%s", names[i]);
        } else {
            snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        }
        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            walk(path);
            continue;
        }
        size_t n = strlen(path);
        if (n > 2 && path[n - 2] == '.' && (path[n - 1] == 'c'
                                            || path[n - 1] == 'h')) {
            scan(path);
        }
    }
}

/*
 * each argument in the order it was given: a directory is walked, a file
 * is read as it is. the python does the same, and the two have to agree
 * on the order as well as on the problems
 */
int main(int argc, char **argv)
{
    if (argc < 2) {
        walk(".");
    }
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            walk(argv[i]);
            continue;
        }
        size_t n = strlen(argv[i]);
        if (n > 2 && argv[i][n - 2] == '.' && (argv[i][n - 1] == 'c'
                                               || argv[i][n - 1] == 'h')) {
            scan(argv[i]);
        }
    }
    if (problems > 0) {
        fprintf(stderr, "\n%d unsupported format specifier(s). either avoid "
                "them or teach lib/kprintf.c the trick.\n", problems);
        return 1;
    }
    return 0;
}
