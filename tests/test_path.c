// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_path.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for path resolution.
 */

#include <stdio.h>
#include <string.h>

#include "fs/path.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

static const char *resolved(const char *cwd, const char *path)
{
    static char out[PATH_MAX];
    if (!path_resolve(cwd, path, out, sizeof out)) {
        return "<refused>";
    }
    return out;
}

#define SAME(cwd, path, want, msg) \
    CHECK(strcmp(resolved(cwd, path), want) == 0, msg)

int main(void)
{


    SAME("/", "x.txt", "/x.txt", "a bare name is read from where the test is");
    SAME("/notes", "x.txt", "/notes/x.txt", "and from wherever that is");
    SAME("/notes", "/x.txt", "/x.txt",
         "a leading slash says where it is from, so the cwd is not consulted");
    SAME("/notes", "", "/notes", "an empty path is where the test already am");
    SAME("/", "", "/", "including at the root");



    SAME("/", "//x//y//", "/x/y", "repeated slashes collapse");
    SAME("/", "/x/y/", "/x/y", "and a trailing one is dropped");
    SAME("//notes//", "x", "/notes/x", "even in the working directory");
    SAME("/", "/", "/", "the root is the root");



    SAME("/a/b", ".", "/a/b", "a dot is where the test is");
    SAME("/a/b", "./x", "/a/b/x", "and goes away in the middle");
    SAME("/a/b", "..", "/a", "two dots is back one");
    SAME("/a/b/c", "../..", "/a", "and they stack");
    SAME("/a/b", "../c", "/a/c", "back one, then forward");
    SAME("/a/b", "c/../d", "/a/b/d", "and undone in the middle of a path");
    SAME("/a/b", "./../.././a/./b", "/a/b", "a mess of them still lands right");

    /* a path able to climb above the root is a path that can name anything at all. */
    SAME("/", "..", "/", "back one from the root stays at the root");
    SAME("/", "../../..", "/", "however many times it is asked");
    SAME("/a", "../../../../x", "/x",
         "and climbing past it does not escape, it just arrives at the top");
    SAME("/", "/../etc/passwd", "/etc/passwd",
         "a path that tries to climb out is flattened, not honoured");



    char small[8];
    CHECK(!path_resolve("/", "/a-name-far-longer-than-this", small, sizeof small),
          "a path that will not fit is refused");
    CHECK(!path_resolve("/", "/x", small, 1), "and so is one with no room at all");

    char big[PATH_MAX];
    char deep[PATH_MAX] = "/";
    for (int i = 0; i < 40; i++) {
        strcat(deep, "d/");
    }
    CHECK(!path_resolve("/", deep, big, sizeof big),
          "a path with more components than the test keeps is refused, not folded");



    char dir[PATH_MAX], name[PATH_MAX];

    CHECK(path_split("/a/b/c.txt", dir, sizeof dir, name, sizeof name),
          "an absolute path splits");
    CHECK(strcmp(dir, "/a/b") == 0, "into where it lives");
    CHECK(strcmp(name, "c.txt") == 0, "and what it is called");

    CHECK(path_split("/x.txt", dir, sizeof dir, name, sizeof name),
          "a path at the root splits");
    CHECK(strcmp(dir, "/") == 0, "with the root spelled as a slash");
    CHECK(strcmp(name, "x.txt") == 0, "and the name after it");

    CHECK(path_split("/", dir, sizeof dir, name, sizeof name),
          "even the root splits");
    CHECK(strcmp(name, "") == 0, "into nothing much");
    CHECK(!path_split("relative", dir, sizeof dir, name, sizeof name),
          "but a relative path does not, since it has no place yet");



    CHECK(path_is_root("/"), "a slash is the root");
    CHECK(path_is_root("///"), "and so are three of them");
    CHECK(!path_is_root("/a"), "but a name is not");
    CHECK(!path_is_root(""), "and neither is nothing");

    if (failures == 0) printf("all good\n");
    return failures;
}
