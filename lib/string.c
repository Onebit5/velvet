// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/string.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the string functions: the three that move memory a word at a time, the
 * rest written for size and clarity rather than speed.
 */

#include <velvet/string.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * a word at a time wherever the two ends are aligned alike, which the
 * big copies are: a screen moving up a line is megabytes, and a byte
 * at a time that is eight times the loop. whatever is left over, and
 * anything misaligned against the other end, goes a byte at a time
 */
typedef uint64_t __attribute__((__may_alias__)) word_t;
#define WORD_MASK (sizeof(word_t) - 1)

static bool in_step(const void *a, const void *b)
{
    return (((uintptr_t)a ^ (uintptr_t)b) & WORD_MASK) == 0;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (in_step(d, s)) {
        for (; n > 0 && ((uintptr_t)d & WORD_MASK) != 0; n--) {
            *d++ = *s++;
        }
        for (; n >= sizeof(word_t); n -= sizeof(word_t)) {
            *(word_t *)d = *(const word_t *)s;
            d += sizeof(word_t);
            s += sizeof(word_t);
        }
    }
    while (n-- > 0) { *d++ = *s++; }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || n == 0) {
        return dst;
    }
    /*
     * backwards when they overlap the wrong way, which is the entire
     * difference from memcpy and the reason both exist
     */
    if (d < s) {
        return memcpy(dst, src, n);
    }
    d += n; s += n;
    if (in_step(d, s)) {
        for (; n > 0 && ((uintptr_t)d & WORD_MASK) != 0; n--) {
            *--d = *--s;
        }
        for (; n >= sizeof(word_t); n -= sizeof(word_t)) {
            d -= sizeof(word_t);
            s -= sizeof(word_t);
            *(word_t *)d = *(const word_t *)s;
        }
    }
    while (n-- > 0) { *--d = *--s; }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    word_t u_fill = (unsigned char)c * 0x0101010101010101ull;
    for (; n > 0 && ((uintptr_t)d & WORD_MASK) != 0; n--) {
        *d++ = (unsigned char)c;
    }
    for (; n >= sizeof(word_t); n -= sizeof(word_t)) {
        *(word_t *)d = u_fill;
        d += sizeof(word_t);
    }
    while (n-- > 0) { *d++ = (unsigned char)c; }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}

void *memchr(const void *p, int c, size_t n)
{
    const unsigned char *s = p;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == (unsigned char)c) {
            return (void *)(s + i);
        }
    }
    return NULL;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') { n++; }
    return n;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n] != '\0') { n++; }
    return n;
}

/*
 * TODO: nothing calls this or strcat. either give them a caller or delete
 * them, since a string function with no caller is one nobody has tested.
 */
char *strcpy(char *dst, const char *src)
{
    char *at = dst;
    while ((*at++ = *src++) != '\0') { }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    while (i < n && src[i] != '\0') { dst[i] = src[i]; i++; }
    /* pads with zeroes to `n`, and does *not* terminate when the source filled it exactly. */
    while (i < n) { dst[i++] = '\0'; }
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *at = dst + strlen(dst);
    while ((*at++ = *src++) != '\0') { }
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x != y) { return (int)x - (int)y; }
        if (x == '\0') { return 0; }
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) { return (char *)s; }
        if (*s == '\0') { return NULL; }
    }
}

char *strrchr(const char *s, int c)
{
    const char *found = NULL;
    for (;; s++) {
        if (*s == (char)c) { found = s; }
        if (*s == '\0') { return (char *)found; }
    }
}

char *strstr(const char *hay, const char *needle)
{
    if (*needle == '\0') {
        return (char *)hay;     /* everybody contains the empty string */
    }
    for (; *hay != '\0'; hay++) {
        const char *h = hay, *n = needle;
        while (*n != '\0' && *h == *n) { h++; n++; }
        if (*n == '\0') { return (char *)hay; }
    }
    return NULL;
}
