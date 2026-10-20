// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/string.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the string functions, written for size and clarity rather than speed.
 */

#include <velvet/string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
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
        while (n-- > 0) { *d++ = *s++; }
    } else {
        d += n; s += n;
        while (n-- > 0) { *--d = *--s; }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
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
