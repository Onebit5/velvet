// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_elf.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the elf header checks. exec maps what these describe, so everything it
 * refuses to load is decided here, and a loader that maps nonsense is a
 * loader that hands ring 3 the kernel.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <velvet/elf.h>
#include <velvet/vmm.h>    /* for the PTE flags a segment turns into */

/*
 * whether this cpu can honour the no-execute bit is a question about
 * the machine rather than about the file, so describing a segment asks
 *, and on a host there is nobody to ask
 */
uint64_t vmm_nx(void)
{
    return 0;
}

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

/* a minimal but valid elf64 header, which each test then breaks in one specific way. */
static uint8_t img[16384];

static void reset(void)
{
    memset(img, 0, sizeof img);
    img[0] = 0x7f; img[1] = 'E'; img[2] = 'L'; img[3] = 'F';
    img[4] = 2;                     /* 64-bit */
    img[5] = 1;                     /* little endian */
    *(uint16_t *)(img + 16) = 2;    /* ET_EXEC */
    *(uint16_t *)(img + 18) = 0x3e; /* x86-64 */
    *(uint64_t *)(img + 24) = 0x400000;  /* entry */
    *(uint64_t *)(img + 32) = 64;        /* phoff */
    *(uint16_t *)(img + 54) = 56;        /* phentsize */
    *(uint16_t *)(img + 56) = 1;         /* phnum */
}

/* the header checked as exec checks it, against the whole of img */
static bool loadable(const uint8_t *p_img, uint64_t u_size, const char **why)
{
    uint64_t u_phoff;
    size_t sz_phlen;
    return elf_check(p_img, u_size, &u_phoff, &sz_phlen, why);
}

static void refuses(const char *what)
{
    const char *why = NULL;
    if (loadable(img, sizeof img, &why)) {
        printf("FAIL: accepted an image that %s\n", what);
        failures++;
    }
}

/* write a PT_LOAD program header into the image at slot `n` */
static void segment(int n, uint64_t offset, uint64_t vaddr, uint64_t filesz,
                    uint64_t memsz, uint32_t flags)
{
    uint8_t *p = img + 64 + n * 56;
    *(uint32_t *)(p + 0) = 1;           /* PT_LOAD */
    *(uint32_t *)(p + 4) = flags;
    *(uint64_t *)(p + 8) = offset;
    *(uint64_t *)(p + 16) = vaddr;
    *(uint64_t *)(p + 32) = filesz;
    *(uint64_t *)(p + 40) = memsz;
}

int main(int i_argc, char **pp_argv)
{
    const char *why = NULL;

    reset();
    CHECK(loadable(img, sizeof img, &why),
          "a well formed header is accepted");
    {
        uint64_t u_phoff = 0;
        size_t sz_phlen = 0;
        CHECK(elf_check(img, sizeof img, &u_phoff, &sz_phlen, &why)
              && u_phoff == 64 && sz_phlen == 56,
              "and says where its program headers are, and how long");
    }

    CHECK(!loadable(NULL, 0, &why), "NULL is not an elf");
    CHECK(!loadable(img, 8, &why), "eight bytes is not an elf");
    CHECK(loadable(img, sizeof img, NULL),
          "the reason pointer may be NULL");

    reset(); img[1] = 'X';                      refuses("has no elf magic");
    reset(); img[4] = 1;                        refuses("is 32-bit");
    reset(); img[5] = 2;                        refuses("is big endian");
    reset(); *(uint16_t *)(img + 18) = 0x28;    refuses("is arm");
    reset(); *(uint16_t *)(img + 16) = 4;       refuses("is a core");
    reset(); *(uint16_t *)(img + 16) = 1;       refuses("is a relocatable object");
    reset(); *(uint16_t *)(img + 56) = 0;       refuses("has no program headers");
    reset(); *(uint16_t *)(img + 54) = 32;      refuses("has odd-sized phdrs");
    reset(); *(uint64_t *)(img + 24) = 0;       refuses("has no entry point");

    /* the one that matters most: headers pointing past the file. */
    /*
     * expressed against the image rather than as a number, so growing
     * the image cannot quietly turn this into a valid file
     */
    reset(); *(uint64_t *)(img + 32) = sizeof img - 8;
    refuses("puts its program headers past the end of the file");

    reset();
    *(uint64_t *)(img + 32) = 64;
    *(uint16_t *)(img + 56) = 1000;   /* 1000 * 56 bytes runs off the end */
    refuses("claims more program headers than fit");

    /*
     * and a file claiming more than the kernel reads, even where they
     * fit: they are read into the heap, all of them, before any is looked
     * at
     */
    reset();
    *(uint16_t *)(img + 56) = ELF_PHDRS_MAX + 1;
    refuses("claims more program headers than the kernel reads");
    reset();
    *(uint64_t *)(img + 32) = ~0ull - 8;
    refuses("puts its program headers where the sum wraps");

    /* and a real program the build makes, the image's /init */
    if (i_argc > 1) {
        FILE *fp = fopen(pp_argv[1], "rb");
        static uint8_t real[1024 * 1024];
        size_t n = (fp != NULL) ? fread(real, 1, sizeof real, fp) : 0;
        if (fp != NULL) {
            fclose(fp);
        }
        struct elf_image image;
        why = NULL;
        CHECK(n < sizeof real && loadable(real, n, &why)
              && elf_describe(real, real + *(uint64_t *)(real + 32), n,
                              &image, &why)
              && image.sz_segs >= 2 && image.u_phdr != 0,
              "the program the build makes is loadable, and says where "
              "its headers are for its libc");
        if (why) printf("    (it said: %s)\n", why);
    }

    /*
     * the loader has nothing to do at load time: a page of a segment is
     * read out of the file the first time it is touched. this is what it
     * works from, and the arithmetic in it is what decides which bytes a
     * page gets when the fault arrives
     */
    {
        struct elf_image image;
        struct elf_segment *segs = image.arr_segs;

        reset();
        *(uint16_t *)(img + 56) = 2;    /* two segments */
        /* text: read + execute, exactly a page */
        segment(0, 0x1000, 0x400000, 0x1000, 0x1000, 5);
        /* data: read + write, with 200 bytes of bss past the file */
        segment(1, 0x2000, 0x402000, 0x400, 0x4c8, 6);

        CHECK(elf_describe(img, img + 64, sizeof img, &image, &why),
              "a two-segment image can be described");
        CHECK(image.sz_segs == 2, "as two segments");
        CHECK(image.u_entry == 0x400000, "with the entry point");
        CHECK(image.u_phnum == 2 && image.u_phdr == 0,
              "and its headers in no segment, so nowhere in memory");

        CHECK(segs[0].vaddr == 0x400000 && segs[0].end == 0x401000,
              "the first covers exactly its page");
        CHECK(segs[0].file_end == 0x401000,
              "with a file byte behind every address in it");
        CHECK(!(segs[0].flags & PTE_WRITE),
              "and is not writable, because the file said so");

        CHECK(segs[1].vaddr == 0x402000, "the second starts where it says");
        CHECK(segs[1].end == 0x403000,
              "and is rounded up to whole pages, bss included");
        CHECK(segs[1].file_end == 0x402400,
              "with the file stopping partway through, everything past "
              "here is bss, and the page that straddles it is both");
        CHECK(segs[1].flags & PTE_WRITE, "and it is writable");
        CHECK(segs[1].offset == 0x2000, "reading from the right place");

        CHECK(image.u_high == 0x403000, "and the break is past all of it");

        /*
         * one built to be loaded anywhere is linked at 0 and put
         * somewhere else, and everything its headers say moves with it
         */
        *(uint16_t *)(img + 16) = 3;
        *(uint64_t *)(img + 24) = 0x1234;
        segment(0, 0x1000, 0x1000, 0x1000, 0x1000, 5);
        segment(1, 0x2000, 0x2000, 0x400, 0x4c8, 6);
        CHECK(elf_describe(img, img + 64, sizeof img, &image, &why)
              && image.b_anywhere && image.u_low == 0x1000
              && image.u_high == 0x3000,
              "a program built to be loaded anywhere is described where "
              "it was linked");
        elf_move(&image, 0x555555554000ull);
        CHECK(image.u_entry == 0x555555555234ull
              && image.u_low == 0x555555555000ull
              && image.u_high == 0x555555557000ull
              && segs[1].vaddr == 0x555555556000ull
              && segs[1].file_end == 0x555555556400ull
              && segs[1].offset == 0x2000 && image.u_phdr == 0,
              "and moved, its entry and its segments move with it, and "
              "where in the file each is does not");
        *(uint16_t *)(img + 16) = 2;
        *(uint64_t *)(img + 24) = 0x400000;
        segment(0, 0x1000, 0x400000, 0x1000, 0x1000, 5);
        segment(1, 0x2000, 0x402000, 0x400, 0x4c8, 6);
        CHECK(elf_describe(img, img + 64, sizeof img, &image, &why)
              && !image.b_anywhere, "one built for its place is not");

        /*
         * a program naming a loader is not started without it: at its own
         * entry it would run with nothing it was linked against there
         */
        uint8_t *p_interp = img + 64 + 2 * 56;
        *(uint16_t *)(img + 56) = 3;
        *(uint32_t *)p_interp = 3;      /* PT_INTERP */
        CHECK(!elf_describe(img, img + 64, sizeof img, &image, &why),
              "a program naming a loader is refused");
        memset(p_interp, 0, 56);
        *(uint16_t *)(img + 56) = 2;

        /* the headers in the first segment are where it puts them */
        segment(0, 0, 0x400000, 0x1000, 0x1000, 5);
        CHECK(elf_describe(img, img + 64, sizeof img, &image, &why)
              && image.u_phdr == 0x400040,
              "headers inside a segment are wherever it puts them");

        /* and a segment that starts further into its page than the file */
        reset();
        segment(0, 0x10, 0x400100, 0x10, 0x10, 5);
        CHECK(!elf_describe(img, img + 64, sizeof img, &image, &why),
              "a segment starting before its file does is refused");

        reset();
        segment(0, 0x1000, 0x400000, 0x2000, 0x1000, 6);
        CHECK(!elf_describe(img, img + 64, sizeof img, &image, &why),
              "and so is one holding more of the file than it has room "
              "for");

        /*
         * the refusals. each of these is a program that would otherwise
         * be described wrongly and fault forever, or worse
         */
        reset();
        *(uint16_t *)(img + 56) = 1;
        segment(0, 0x1000, 0xffff800000000000ull, 0x1000, 0x1000, 6);
        CHECK(!elf_describe(img, img + 64, sizeof img, &image, &why),
              "a segment that wants to live in kernel space is refused");

        reset();
        *(uint16_t *)(img + 56) = 1;
        segment(0, 0x1000, 0x400000, 0x100000, 0x100000, 6);
        CHECK(!elf_describe(img, img + 64, sizeof img, &image, &why),
              "and so is one reaching past the end of the file");

        /*
         * two segments sharing a page cannot both be described: one
         * record would have to fetch the other's bytes, and they may
         * not even agree about whether the page is writable
         */
        reset();
        *(uint16_t *)(img + 56) = 2;
        segment(0, 0x1000, 0x400000, 0x100, 0x100, 5);
        segment(1, 0x1100, 0x400100, 0x100, 0x100, 6);
        CHECK(!elf_describe(img, img + 64, sizeof img, &image, &why),
              "two segments sharing a page are refused");

        reset();
        *(uint16_t *)(img + 56) = 0;
        CHECK(!elf_describe(img, img + 64, sizeof img, &image, &why),
              "an image with nothing to load is refused");

        /* more segments than there is room to remember. */
        reset();
        *(uint16_t *)(img + 56) = ELF_SEGMENTS_MAX + 1;
        for (int i = 0; i <= ELF_SEGMENTS_MAX; i++) {
            segment(i, 0x1000, 0x400000 + (uint64_t)i * 0x1000, 0x10, 0x10, 6);
        }
        CHECK(!elf_describe(img, img + 64, sizeof img, &image, &why),
              "and so is one with more segments than the test keeps room for");
    }

    if (!failures) printf("all good\n");
    return failures;
}
