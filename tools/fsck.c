// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/fsck.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * fsck, over an image file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "fs/fsck.h"

static FILE *image;

static bool img_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    if (fseek(image, (long)(lba * FSCK_SECTOR), SEEK_SET) != 0) {
        return false;
    }
    size_t got = fread(buf, FSCK_SECTOR, count, image);
    if (got < count) {
        memset((char *)buf + got * FSCK_SECTOR, 0,
               (count - got) * FSCK_SECTOR);
    }
    return true;
}

static bool img_write(void *ctx, uint64_t lba, uint32_t count,
                      const void *buf)
{
    (void)ctx;
    return fseek(image, (long)(lba * FSCK_SECTOR), SEEK_SET) == 0
        && fwrite(buf, FSCK_SECTOR, count, image) == count;
}

int main(int argc, char **argv)
{
    bool dry = false;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            dry = true;
        } else {
            path = argv[i];
        }
    }
    if (path == NULL) {
        fprintf(stderr, "usage: fsck [-n] <image>\n");
        return 2;
    }

    image = fopen(path, dry ? "rb" : "r+b");
    if (image == NULL) {
        fprintf(stderr, "fsck: cannot open %s\n", path);
        return 2;
    }

    uint32_t blocks, inodes;
    if (!fsck_measure(img_read, NULL, &blocks, &inodes)) {
        fprintf(stderr, "fsck: %s holds no ext4 filesystem\n", path);
        return 2;
    }

    size_t need = fsck_workspace_bytes(blocks, inodes);
    void *work = malloc(need);
    if (work == NULL) {
        fprintf(stderr, "fsck: cannot find %zu bytes to check it with\n", need);
        return 2;
    }

    struct fsck_report r;
    bool ran = fsck_run(img_read, dry ? NULL : img_write, NULL, work, need, &r);
    free(work);

    if (!ran) {
        fprintf(stderr, "fsck: %s\n",
                r.stopped != NULL ? r.stopped : "cannot check it");
        return 2;
    }

    printf("%s: %u blocks of %u, %u inodes, %u group(s)\n",
           path, r.blocks, r.block_size, r.inodes, r.groups);

    if (r.total_found == 0) {
        printf("  nothing wrong with it\n");
        fclose(image);
        return 0;
    }

    for (int i = 0; i < FSCK_PROBLEMS; i++) {
        if (r.found[i] == 0) {
            continue;
        }
        printf("  %u x %s", r.found[i], fsck_problem_name((enum fsck_problem)i));
        if (r.mended[i] > 0) {
            printf("  (%u mended)", r.mended[i]);
        } else if (!dry) {
            printf("  (left alone)");
        }
        printf("\n");
    }
    printf("  %u problem(s), %u mended\n", r.total_found, r.total_mended);

    fclose(image);

    /*
     * the exit status says what happened, the way fsck has since v7:
     * nothing wrong, something mended, or something still wrong
     */
    if (r.total_mended == r.total_found) {
        return dry ? 1 : 0;
    }
    return 1;
}
