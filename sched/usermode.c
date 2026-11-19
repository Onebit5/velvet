// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/usermode.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a user thread needs to know before it stops being a kernel one.
 */

#include <velvet/usermode.h>
#include <velvet/sched.h>
#include <velvet/thread.h>
#include <asm/context.h>
#include <velvet/tty.h>
#include <velvet/elf.h>
#include <velvet/vfs.h>
#include <velvet/kprintf.h>
#include <velvet/string.h>
#include <velvet/addrspace.h>
#include <velvet/kmalloc.h>
#include <velvet/pmm.h>
#include <velvet/vmm.h>
#include <velvet/process.h>
#include <velvet/pit.h>
#include <velvet/pipe.h>
#include <velvet/file.h>
#include <velvet/netif.h>
#include <velvet/signal.h>
#include <velvet/random.h>
#include <velvet/errno.h>
#include <velvet/futex.h>
#include <velvet/namei.h>
#include <uapi/velvet/fcntl.h>
#include <asm/irq.h>
#include <asm/uaccess.h>
#include <uapi/velvet/auxvec.h>

/* what a user thread needs to know before it stops being a kernel one. */
struct user_start {
    uint64_t entry;
    uint64_t stack_top;
};

static void user_thread_start(void *arg)
{
    struct user_start *u = arg;
    uint64_t entry = u->entry;
    uint64_t stack_top = u->stack_top;
    kfree(u);

    /*
     * the scheduler loaded its address space and pointed the tss rsp0
     * and the syscall stack at its kernel stack when it switched the kernel in,
     * so a trap from ring 3 lands somewhere the kernel owns. everything below
     * this line is one way
     */
    context_enter_user(entry, stack_top);
}

/*
 * one vector of strings out of ring 3, appended to what is in p_buf: how
 * many there were, or why not. the room counts a pointer for each as
 * well as its bytes, as both will lie on the new program's stack
 */
static int64_t copy_vector(uint64_t u_vec, char *p_buf, size_t *p_used,
                           size_t *p_count, size_t sz_counted)
{
    if (u_vec == 0) {
        return 0;
    }
    for (size_t sz_at = 0;; sz_at++) {
        uint64_t u_str = 0;
        if (get_user_u64(&u_str, u_vec + sz_at * 8) != 0) {
            return -EFAULT;
        }
        if (u_str == 0) {
            return 0;
        }
        size_t sz_pointers = (sz_counted + *p_count + 1) * 8;
        if (*p_used + sz_pointers >= EXEC_ARGS_MAX) {
            return -E2BIG;
        }
        size_t sz_room = EXEC_ARGS_MAX - sz_pointers - *p_used;
        int64_t i_len = strncpy_from_user(p_buf + *p_used, u_str, sz_room);
        if (i_len < 0) {
            return (i_len == -ENAMETOOLONG) ? -E2BIG : i_len;
        }
        *p_used += (size_t)i_len + 1;
        (*p_count)++;
    }
}

/* pointers to the count strings lying one after another from p_from */
static void point_at(const char **pp_out, char *p_from, size_t sz_count)
{
    for (size_t sz_at = 0; sz_at < sz_count; sz_at++) {
        pp_out[sz_at] = p_from;
        p_from += strlen(p_from) + 1;
    }
    pp_out[sz_count] = NULL;
}

int64_t exec_args_copy(uint64_t u_argv, uint64_t u_envp,
                       struct exec_args *p_out)
{
    memset(p_out, 0, sizeof *p_out);
    char *p_buf = kmalloc(EXEC_ARGS_MAX);
    if (p_buf == NULL) {
        return -ENOMEM;
    }
    size_t sz_used = 0;
    size_t sz_argc = 0;
    size_t sz_envc = 0;
    int64_t i_why = copy_vector(u_argv, p_buf, &sz_used, &sz_argc, 0);
    if (i_why == 0) {
        i_why = copy_vector(u_envp, p_buf, &sz_used, &sz_envc, sz_argc);
    }
    const char **pp_vec = (i_why == 0)
                          ? kmalloc((sz_argc + sz_envc + 2) * sizeof *pp_vec)
                          : NULL;
    if (i_why == 0 && pp_vec == NULL) {
        i_why = -ENOMEM;
    }
    if (i_why < 0) {
        kfree(p_buf);
        return i_why;
    }
    point_at(pp_vec, p_buf, sz_argc);
    char *p_env = p_buf;
    for (size_t sz_at = 0; sz_at < sz_argc; sz_at++) {
        p_env += strlen(p_env) + 1;
    }
    point_at(pp_vec + sz_argc + 1, p_env, sz_envc);

    p_out->pp_argv = pp_vec;
    p_out->sz_argc = sz_argc;
    p_out->pp_envp = pp_vec + sz_argc + 1;
    p_out->sz_envc = sz_envc;
    p_out->p_strings = p_buf;
    return 0;
}

void exec_args_free(struct exec_args *p_args)
{
    kfree(p_args->p_strings);
    kfree((void *)p_args->pp_argv);
    memset(p_args, 0, sizeof *p_args);
}

/* the auxiliary vector's entries, AT_NULL's included */
#define AUX_PAIRS 18

/* what the auxiliary vector says about the program and who runs it */
struct aux_facts {
    uint64_t u_entry;
    uint64_t u_phdr;
    uint64_t u_phnum;
    uint64_t u_base;            /* how far its loader was moved, or 0 */
    int i_uid;
    int i_gid;
};

/* the bytes a stack laid out for p_args needs, all of it */
static size_t stack_need(const struct exec_args *p_args, const char *str_path)
{
    size_t sz_bytes = strlen(str_path) + 1 + 16;
    for (size_t sz_at = 0; sz_at < p_args->sz_argc; sz_at++) {
        sz_bytes += strlen(p_args->pp_argv[sz_at]) + 1;
    }
    for (size_t sz_at = 0; sz_at < p_args->sz_envc; sz_at++) {
        sz_bytes += strlen(p_args->pp_envp[sz_at]) + 1;
    }
    sz_bytes += (1 + p_args->sz_argc + 1 + p_args->sz_envc + 1
                 + 2 * AUX_PAIRS) * 8;
    return sz_bytes + 32;       /* the two alignments */
}

/*
 * the stack a program starts on, as every x86_64 program expects to find
 * it. from rsp up: argc, the argv pointers and a NULL, the envp pointers
 * and a NULL, and the auxiliary vector ending in AT_NULL; above all of
 * that the strings they point at, the path the program was run by, and
 * sixteen bytes nobody could predict, for a libc's stack protector.
 *
 * written through the direct map into pages the new space does not have
 * loaded yet, while every pointer is the address the program will see,
 * which is what user_addr keeps straight. the stack pointer it starts
 * on, 16-aligned
 */
static uint64_t build_stack(uint8_t *p_top_k, const struct exec_args *p_args,
                            const char *str_path,
                            const struct aux_facts *p_facts,
                            struct addrspace *p_space)
{
    #define user_addr(va) (p_top_k - (USER_STACK_TOP - (va)))

    uint64_t u_sp = USER_STACK_TOP;

    size_t sz_len = strlen(str_path) + 1;
    u_sp -= sz_len;
    memcpy(user_addr(u_sp), str_path, sz_len);
    uint64_t u_execfn = u_sp;

    u_sp -= 16;
    random_bytes(user_addr(u_sp), 16);
    uint64_t u_random = u_sp;

    /*
     * the environment's strings and then the arguments', each set last
     * first, so that from argv[0] up they lie in the order they are
     * numbered, and the pointers can be worked out by walking them
     */
    p_space->u_env_end = u_sp;
    for (size_t sz_at = p_args->sz_envc; sz_at-- > 0; ) {
        sz_len = strlen(p_args->pp_envp[sz_at]) + 1;
        u_sp -= sz_len;
        memcpy(user_addr(u_sp), p_args->pp_envp[sz_at], sz_len);
    }
    p_space->u_env_start = u_sp;
    p_space->u_arg_end = u_sp;
    for (size_t sz_at = p_args->sz_argc; sz_at-- > 0; ) {
        sz_len = strlen(p_args->pp_argv[sz_at]) + 1;
        u_sp -= sz_len;
        memcpy(user_addr(u_sp), p_args->pp_argv[sz_at], sz_len);
    }
    p_space->u_arg_start = u_sp;
    uint64_t u_string = u_sp;

    uint64_t arr_aux[AUX_PAIRS * 2] = {
        AT_PHDR,   p_facts->u_phdr,
        AT_PHENT,  ELF_PHDR_SIZE,
        AT_PHNUM,  p_facts->u_phnum,
        AT_PAGESZ, PAGE_SIZE,
        AT_BASE,   p_facts->u_base,
        AT_FLAGS,  0,
        AT_ENTRY,  p_facts->u_entry,
        AT_UID,    (uint64_t)p_facts->i_uid,
        AT_EUID,   (uint64_t)p_facts->i_uid,
        AT_GID,    (uint64_t)p_facts->i_gid,
        AT_EGID,   (uint64_t)p_facts->i_gid,
        AT_SECURE, 0,
        AT_CLKTCK, 100,
        AT_HWCAP,  0,
        AT_RANDOM, u_random,
        AT_EXECFN, u_execfn,
        AT_IGNORE, 0,
        AT_NULL,   0,
    };

    /*
     * everything below the strings is words, and argc has to land on a
     * sixteen byte boundary, so the gap goes above the words
     */
    size_t sz_words = 1 + p_args->sz_argc + 1 + p_args->sz_envc + 1
                      + AUX_PAIRS * 2;
    u_sp &= ~15ull;
    if (sz_words % 2 != 0) {
        u_sp -= 8;
    }
    u_sp -= sz_words * 8;

    uint64_t *p_word = (uint64_t *)user_addr(u_sp);
    *p_word++ = p_args->sz_argc;
    for (size_t sz_at = 0; sz_at < p_args->sz_argc; sz_at++) {
        *p_word++ = u_string;
        u_string += strlen(p_args->pp_argv[sz_at]) + 1;
    }
    *p_word++ = 0;
    for (size_t sz_at = 0; sz_at < p_args->sz_envc; sz_at++) {
        *p_word++ = u_string;
        u_string += strlen(p_args->pp_envp[sz_at]) + 1;
    }
    *p_word++ = 0;
    memcpy(p_word, arr_aux, sizeof arr_aux);

    #undef user_addr
    return u_sp;
}

/*
 * a program made ready to run, short of anything to run it: its own
 * address space with the image and a stack in it, where it starts, and
 * its arguments already on that stack. spawn puts a new process around
 * one, and exec swaps one in for the caller's
 */
struct user_image {
    struct addrspace *p_space;
    uint64_t u_entry;
    uint64_t u_stack_top;
    const char *str_exe;        /* the program's path, held by the space */
};

/*
 * the program a path names, opened, and what its headers say it is: the
 * file comes back held, for the regions it is read into to hold in turn
 */
static int64_t program_open(const char *str_path, struct file **pp_file,
                            struct elf_image *p_image,
                            const char **pp_error)
{
    struct vfs_file node;
    if (!vfs_open(str_path, &node) || node.is_dir) {
        *pp_error = "no such file";
        return -ENOENT;
    }
    struct file *p_file = vfs_open_file(&node, FILE_READ);
    if (p_file == NULL || !vfs_mappable(p_file)) {
        file_put(p_file);
        *pp_error = "not a file a program can be read out of";
        return -EACCES;
    }
    strncpy(p_file->arr_path, str_path, sizeof p_file->arr_path - 1);

    uint8_t arr_header[ELF_HEADER_SIZE];
    uint64_t u_phoff = 0;
    size_t sz_phlen = 0;
    *pp_error = "too small to be an elf at all";
    if (vfs_read_file(p_file, 0, arr_header, sizeof arr_header)
            != (int64_t)sizeof arr_header
        || !elf_check(arr_header, node.size, &u_phoff, &sz_phlen,
                      pp_error)) {
        file_put(p_file);
        return -ENOEXEC;
    }
    void *p_phdrs = kmalloc(sz_phlen);
    if (p_phdrs == NULL) {
        file_put(p_file);
        *pp_error = "no memory for its headers";
        return -ENOMEM;
    }
    *pp_error = "its program headers cannot be read";
    bool b_ok = vfs_read_file(p_file, u_phoff, p_phdrs, sz_phlen)
                    == (int64_t)sz_phlen
                && elf_describe(arr_header, p_phdrs, node.size, p_image,
                                pp_error);
    kfree(p_phdrs);
    if (!b_ok) {
        file_put(p_file);
        return -ENOEXEC;
    }
    *pp_file = p_file;
    return 0;
}

/*
 * a program another names, the loader a program names or the program a
 * script names, found the way the first was: from the caller's root and
 * working directory, the last link followed, into PATH_MAX of str_path.
 * 0, or ENOENT when nothing is there, EACCES when the caller may not run
 * it, or what else the lookup says
 */
static int64_t program_find(const char *str_name, char *str_path,
                            struct vfs_file *p_node)
{
    /*
     * an empty name, which a script's line or a program's headers can
     * give and no path from ring 3 can, names where the caller stands: a
     * directory, so nothing to run
     */
    if (str_name[0] == '\0') {
        return -EACCES;
    }
    int64_t i_why = path_from(AT_FDCWD, str_name, str_path);
    if (i_why == 0) {
        i_why = path_lookup(str_path, true, p_node);
    }
    if (i_why == 0 && (p_node->is_dir || !may_access(p_node, X_OK))) {
        i_why = -EACCES;
    }
    return i_why;
}

/*
 * the loader the program in p_file names, found as program_find finds
 * one and opened the way the program was. a name with no end is no
 * program's, ENOEXEC; nothing at it is ENOENT, though the program is
 * there; one too short to hold a header is EIO, since its header cannot
 * be read; one that is no program, or none this kernel loads, is
 * ELIBBAD, a library that is not one
 */
static int64_t loader_open(struct file *p_file,
                           const struct elf_image *p_prog,
                           struct file **pp_loader,
                           struct elf_image *p_loader,
                           const char **pp_error)
{
    char *str_name = kmalloc(p_prog->sz_interp);
    if (str_name == NULL) {
        *pp_error = "no memory for the name of its loader";
        return -ENOMEM;
    }
    char arr_path[PATH_MAX];
    struct vfs_file node;
    int64_t i_why = -ENOEXEC;
    *pp_error = "the name of its loader does not end";
    if (vfs_read_file(p_file, p_prog->u_interp_offset, str_name,
                      p_prog->sz_interp) == (int64_t)p_prog->sz_interp
        && str_name[p_prog->sz_interp - 1] == '\0') {
        *pp_error = "the loader it names is not there to run";
        i_why = program_find(str_name, arr_path, &node);
    }
    kfree(str_name);
    if (i_why == 0 && node.size < ELF_HEADER_SIZE) {
        *pp_error = "the loader it names is too short to be one";
        i_why = -EIO;
    }
    if (i_why < 0) {
        return i_why;
    }
    i_why = program_open(arr_path, pp_loader, p_loader, pp_error);
    return (i_why == -ENOEXEC) ? -ELIBBAD : i_why;
}

/* how much of a file exec reads to know whether it is a script */
#define SCRIPT_HEAD 256

/* how many scripts may name the next before the last is refused */
#define SCRIPT_DEPTH 5

static bool blank(char c_at)
{
    return c_at == ' ' || c_at == '\t';
}

/*
 * the first line of the file at str_path into p_line, SCRIPT_HEAD + 1
 * bytes, when it starts with "#!": the program it names, and the one
 * argument it may give after a blank, everything to the end of the line
 * with the blanks around it dropped. 1 for a script, 0 for a file that
 * is not one, which is left for the program reader to say what it is,
 * and ENOEXEC for a line naming nothing, or one whose name runs past the
 * bytes read, which would name something else cut short. an argument
 * running past them is cut there, since its program can read the line
 * again for itself. a line that does not end before the file does names
 * what is up to the file's end, which for "#!" alone is the empty name
 */
static int64_t script_line(const char *str_path, char *p_line,
                           const char **pp_prog, const char **pp_arg)
{
    struct vfs_file node;
    struct file *p_file = (vfs_open(str_path, &node) && !node.is_dir)
                          ? vfs_open_file(&node, FILE_READ) : NULL;
    int64_t i_len = (p_file != NULL)
                    ? vfs_read_file(p_file, 0, p_line, SCRIPT_HEAD) : 0;
    file_put(p_file);
    if (i_len < 2 || p_line[0] != '#' || p_line[1] != '!') {
        return 0;
    }
    memset(p_line + i_len, 0, SCRIPT_HEAD + 1 - (size_t)i_len);

    char *p_last = p_line + SCRIPT_HEAD - 1;
    char *p_end = memchr(p_line, '\n', SCRIPT_HEAD);
    if (p_end == NULL) {
        char *p_at = p_line + 2;
        while (p_at < p_last && blank(*p_at)) {
            p_at++;
        }
        while (p_at < p_last && !blank(*p_at) && *p_at != '\0') {
            p_at++;
        }
        if (p_at == p_last) {
            return -ENOEXEC;
        }
        p_end = p_last;
    }
    while (blank(p_end[-1])) {
        p_end--;
    }
    *p_end = '\0';

    char *p_prog = p_line + 2;
    while (blank(*p_prog)) {
        p_prog++;
    }
    if (p_prog == p_end) {
        return -ENOEXEC;
    }
    char *p_sep = p_prog;
    while (*p_sep != '\0' && !blank(*p_sep)) {
        p_sep++;
    }
    *pp_prog = p_prog;
    *pp_arg = NULL;
    if (*p_sep != '\0') {
        *p_sep++ = '\0';
        while (blank(*p_sep)) {
            p_sep++;
        }
        *pp_arg = (*p_sep != '\0') ? p_sep : NULL;
    }
    return 1;
}

/*
 * the scripts an exec goes through on the way to a program, and what
 * that program is started with: each script's line, and the arguments
 */
struct script_chain {
    char arr_lines[SCRIPT_DEPTH + 1][SCRIPT_HEAD + 1];
    const char *arr_progs[SCRIPT_DEPTH + 1];
    const char *arr_args[SCRIPT_DEPTH + 1];
    size_t sz_scripts;
    char arr_path[PATH_MAX];        /* the program, at last */
    struct exec_args args;
};

/*
 * the program str_path comes to, once every script on the way has named
 * the next, into p_chain: its path in arr_path, and its arguments in
 * args. a script is run by the program its line names, which is handed
 * the line's name and argument, the name the script was asked for by,
 * str_name for the first and the line before's name after that, and the
 * rest of the arguments, so each line goes before the one that named
 * it. 0, or why not: what finding a program says, ELOOP for a script
 * one deeper than SCRIPT_DEPTH, ENOEXEC for a line naming nothing
 */
static int64_t script_follow(const char *str_path, const char *str_name,
                             const struct exec_args *p_args,
                             struct script_chain *p_chain,
                             const char **pp_error)
{
    strncpy(p_chain->arr_path, str_path, PATH_MAX - 1);
    p_chain->arr_path[PATH_MAX - 1] = '\0';
    p_chain->sz_scripts = 0;
    p_chain->args = *p_args;
    for (;;) {
        size_t sz_at = p_chain->sz_scripts;
        int64_t i_is = script_line(p_chain->arr_path,
                                   p_chain->arr_lines[sz_at],
                                   &p_chain->arr_progs[sz_at],
                                   &p_chain->arr_args[sz_at]);
        if (i_is < 0) {
            *pp_error = "a script that names no program";
            return i_is;
        }
        if (i_is == 0) {
            break;
        }
        struct vfs_file node;
        *pp_error = "the program a script names is not there to run";
        int64_t i_why = program_find(p_chain->arr_progs[sz_at],
                                     p_chain->arr_path, &node);
        if (i_why < 0) {
            return i_why;
        }
        if (++p_chain->sz_scripts > SCRIPT_DEPTH) {
            *pp_error = "scripts naming scripts, too deep";
            return -ELOOP;
        }
    }
    if (p_chain->sz_scripts == 0) {
        return 0;
    }

    /* the first argument, the script's own name, gives way to the line */
    size_t sz_skip = (p_args->sz_argc > 0) ? 1 : 0;
    size_t sz_argc = p_args->sz_argc - sz_skip + 1;
    for (size_t sz_at = 0; sz_at < p_chain->sz_scripts; sz_at++) {
        sz_argc += (p_chain->arr_args[sz_at] != NULL) ? 2 : 1;
    }
    const char **pp_argv = kmalloc((sz_argc + 1) * sizeof *pp_argv);
    if (pp_argv == NULL) {
        *pp_error = "no memory for a script's arguments";
        return -ENOMEM;
    }
    size_t sz_out = 0;
    for (size_t sz_at = p_chain->sz_scripts; sz_at-- > 0; ) {
        pp_argv[sz_out++] = p_chain->arr_progs[sz_at];
        if (p_chain->arr_args[sz_at] != NULL) {
            pp_argv[sz_out++] = p_chain->arr_args[sz_at];
        }
    }
    pp_argv[sz_out++] = str_name;
    for (size_t sz_at = sz_skip; sz_at < p_args->sz_argc; sz_at++) {
        pp_argv[sz_out++] = p_args->pp_argv[sz_at];
    }
    pp_argv[sz_out] = NULL;
    p_chain->args.pp_argv = pp_argv;
    p_chain->args.sz_argc = sz_argc;
    p_chain->args.p_strings = NULL;
    return 0;
}

/* every segment of p_image a region of p_file in p_space, or false */
static bool segments_add(struct addrspace *p_space,
                         const struct elf_image *p_image,
                         struct file *p_file)
{
    bool b_placed = true;
    for (size_t sz_at = 0; sz_at < p_image->sz_segs && b_placed; sz_at++) {
        const struct elf_segment *p_seg = &p_image->arr_segs[sz_at];
        b_placed = addrspace_add_region(p_space, p_seg->vaddr, p_seg->end,
                                        p_seg->flags, VMA_FILE, p_file,
                                        p_seg->offset, p_seg->file_end);
    }
    return b_placed;
}

/*
 * the program at str_path made ready to run, started with p_args by
 * whoever asked for it by str_name, which is what AT_EXECFN says
 */
static int64_t image_load(const char *str_path, const char *str_name,
                          const struct exec_args *p_args, int i_uid,
                          int i_gid, struct user_image *p_out,
                          const char **pp_error)
{
    /*
     * the stack has to hold all of it before the program has run an
     * instruction, so what does not fit is refused before anything is
     * read
     */
    size_t sz_need = stack_need(p_args, str_name);
    size_t sz_eager = (sz_need + PAGE_SIZE - 1) / PAGE_SIZE + 1;
    if (sz_eager < USER_STACK_EAGER) {
        sz_eager = USER_STACK_EAGER;
    }
    if (sz_eager > USER_STACK_PAGES / 4) {
        *pp_error = "those arguments do not fit on a stack";
        return -E2BIG;
    }

    struct file *p_file = NULL;
    struct elf_image prog;
    int64_t i_why = program_open(str_path, &p_file, &prog, pp_error);
    if (i_why < 0) {
        return i_why;
    }
    /*
     * one built to be loaded anywhere goes where every such program goes,
     * and all its headers said moves with it
     */
    if (prog.b_anywhere) {
        elf_move(&prog, USER_PROGRAM_BASE - prog.u_low);
    }

    /*
     * one that names a loader is started by it, and the loader is told
     * by the auxiliary vector where the program is and where it was put
     * itself: where mmap puts the first thing it is asked for, out of
     * the way of the program and of its break
     */
    struct file *p_loader = NULL;
    struct elf_image loader = { 0 };
    uint64_t u_base = 0;
    if (prog.sz_interp != 0) {
        i_why = loader_open(p_file, &prog, &p_loader, &loader, pp_error);
        if (i_why < 0) {
            file_put(p_file);
            return i_why;
        }
        if (loader.b_anywhere) {
            u_base = MMAP_BASE - loader.u_low;
            elf_move(&loader, u_base);
        }
    }
    struct aux_facts facts = {
        .u_entry = prog.u_entry, .u_phdr = prog.u_phdr,
        .u_phnum = prog.u_phnum, .u_base = u_base, .i_uid = i_uid,
        .i_gid = i_gid,
    };

    /*
     * its own memory. two programs can now link to the same addresses
     * and never meet, which is the whole point of this milestone
     */
    struct addrspace *space = addrspace_create(vmm_kernel_pml4());
    if (space == NULL) {
        file_put(p_file);
        file_put(p_loader);
        *pp_error = "no memory for an address space";
        return -ENOMEM;
    }

    /*
     * every segment is a region of the file, a page of which is read out
     * of it the first time the page is touched, so a program starts
     * without a byte of it having been read and what is never touched
     * never is. the regions hold the file open for as long as they are
     * there, on whatever filesystem it is on, and the space holds it as
     * the program it is. the loader's are the same, held by its regions
     * alone
     */
    bool b_placed = segments_add(space, &prog, p_file)
                    && segments_add(space, &loader, p_loader);
    space->p_exe = p_file;
    file_put(p_loader);
    if (!b_placed) {
        addrspace_put(space);
        *pp_error = "no room to describe the program";
        return -ENOMEM;
    }

    /* the break starts on the page after the last segment, and grows up */
    space->u_brk_start = prog.u_high;
    space->u_brk = space->u_brk_start;

    /* a stack for ring 3: writable, never executable, mapped user. */
    uint64_t stack_flags = PTE_USER | PTE_WRITE | vmm_nx();
    uint64_t stack_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;

    if (!addrspace_add_region(space, stack_base, USER_STACK_TOP,
                              stack_flags, VMA_ANON, NULL, 0, 0)) {
        addrspace_put(space);
        *pp_error = "no room to describe a stack";
        return -ENOMEM;
    }

    uint64_t eager_base = USER_STACK_TOP - sz_eager * PAGE_SIZE;
    uint64_t stack_phys = pmm_alloc_pages(sz_eager);
    if (stack_phys == 0) {
        addrspace_put(space);
        *pp_error = "no memory for a user stack";
        return -ENOMEM;
    }
    memset(pmm_phys_to_virt(stack_phys), 0, sz_eager * PAGE_SIZE);

    if (!vmm_map_range(space->pml4, eager_base, stack_phys,
                       sz_eager * PAGE_SIZE, stack_flags)) {
        pmm_free_pages(stack_phys, sz_eager);
        addrspace_put(space);
        *pp_error = "could not map a user stack";
        return -ENOMEM;
    }

    uint8_t *p_top_k = (uint8_t *)pmm_phys_to_virt(stack_phys)
                       + sz_eager * PAGE_SIZE;
    p_out->u_stack_top = build_stack(p_top_k, p_args, str_name, &facts,
                                     space);
    space->u_stack_start = p_out->u_stack_top;
    p_out->p_space = space;
    p_out->u_entry = (prog.sz_interp != 0) ? loader.u_entry : prog.u_entry;
    p_out->str_exe = p_file->arr_path;
    return 0;
}

/*
 * the program at str_path, or the one it comes to through the scripts on
 * the way, made ready to run, with the arguments the scripts make of
 * p_args. str_name is what it was asked for by
 */
static int64_t image_build(const char *str_path, const char *str_name,
                           const struct exec_args *p_args, int i_uid,
                           int i_gid, struct user_image *p_out,
                           const char **pp_error)
{
    struct script_chain *p_chain = kmalloc(sizeof *p_chain);
    if (p_chain == NULL) {
        *pp_error = "no memory to read the program's first line";
        return -ENOMEM;
    }
    int64_t i_why = script_follow(str_path, str_name, p_args, p_chain,
                                  pp_error);
    if (i_why == 0) {
        i_why = image_load(p_chain->arr_path, str_name, &p_chain->args,
                           i_uid, i_gid, p_out, pp_error);
    }
    if (p_chain->args.pp_argv != p_args->pp_argv) {
        kfree((void *)p_chain->args.pp_argv);
    }
    kfree(p_chain);
    return i_why;
}

/*
 * what a program is known by: the last part of the path it was started
 * by, the way a program is everywhere, and not where on the medium it
 * happened to be, or every program under one long directory would have
 * that directory's name
 */
static const char *last_part(const char *str_path)
{
    const char *str_name = strrchr(str_path, '/');
    return (str_name != NULL && str_name[1] != '\0') ? str_name + 1
                                                     : str_path;
}

int user_spawn(const char *path, const struct exec_args *p_args,
               const char *cwd, int parent, int uid, bool announce,
               const char **error)
{
    struct user_image img;
    if (image_build(path, path, p_args, uid, uid, &img, error) < 0) {
        return 0;
    }
    struct addrspace *space = img.p_space;
    struct user_start *start = kmalloc(sizeof *start);
    if (start == NULL) {
        addrspace_put(space);
        *error = "no memory";
        return 0;
    }

    start->entry     = img.u_entry;
    start->stack_top = img.u_stack_top;

    /*
     * the process comes first, because it is what outlives the thread
     * and holds the exit code somebody will want to read
     */
    /* a program cannot ask to be somebody else: it runs as who started it */
    int pid = process_create(last_part(path), parent, uid, announce,
                             pit_uptime_ms());

    /* wherever whoever started it was standing, running what it was told */
    if (pid != 0 && cwd != NULL) {
        process_set_cwd(pid, cwd);
    }
    if (pid != 0) {
        process_set_exe(pid, img.str_exe);
    }
    struct file *p_console = (pid != 0) ? tty_open(0, FILE_READ | FILE_WRITE)
                                        : NULL;
    if (p_console == NULL) {
        if (pid != 0) {
            process_exited(pid, PROCESS_KILLED, pit_uptime_ms());
            process_collect(pid, NULL);
        }
        kfree(start);
        addrspace_put(space);
        *error = (pid == 0) ? "the process table is full"
                            : "no memory for a terminal";
        return 0;
    }

    /*
     * a program the kernel starts reads and writes the first console,
     * one open of it on all three, as though it had opened it once and
     * duplicated that twice
     */
    for (int i_fd = FD_STDIN; i_fd <= FD_STDERR; i_fd++) {
        struct file *p_old = NULL;
        file_get(p_console);
        process_fd_replace(pid, i_fd, p_console, false, &p_old);
    }
    file_put(p_console);

    /*
     * parked, not running.
     *
     * a thread that starts the instant it is created can print before
     * anybody has said which job it belongs to or who holds the
     * terminal, so its first line of output goes to whichever answer
     * those questions happened to have a moment ago. that is a race
     * with a very quiet failure: some output, sometimes, missing.
     *
     * so it goes into the ring asleep and whoever spawned it says when.
     * user_start is that.
     */
    struct thread *t = thread_create_parked(last_part(path),
                                            user_thread_start, start);
    if (t == NULL) {
        process_exited(pid, PROCESS_KILLED, pit_uptime_ms());
        process_fds_release(pid);
        process_collect(pid, NULL);
        kfree(start);
        addrspace_put(space);
        *error = "no memory for a thread";
        return 0;
    }
    t->space = space;
    t->pid   = pid;
    t->i_tid = pid;
    process_set_thread(pid, t->id);

    if (announce) {
        kprintf("[kernel] %s is pid %d, ring 3 at %p\n",
                path, pid, (void *)img.u_entry);
    }
    return pid;
}

/*
 * the caller the last thread of its process: every other doomed, and
 * waited for until it has ended, and the caller its process's first
 * thread from then on, with the pid for its tid. false when it was
 * doomed itself first, by a thread ending the process or running a
 * program of its own, and is on its way out
 */
static bool alone(struct thread *p_me)
{
    if (!process_doom_others(p_me->i_tid, 0, true)) {
        return false;
    }
    while (process_thread_count(p_me->pid) > 1) {
        sleep_ms(1);
    }
    process_lead(p_me->i_tid);
    p_me->i_tid = p_me->pid;
    return true;
}

/*
 * the word set_tid_address named, which belongs to the program going
 * away, cleared, with whoever waits on it woken, as an end would: a
 * space shared with another process, which a vfork's child has, is
 * where somebody could be waiting
 */
static void forget_clear_tid(int i_tid)
{
    uint64_t u_flags;
    struct task *t = process_lock_task(i_tid, &u_flags);
    uint64_t u_tid = 0;
    if (t != NULL) {
        u_tid = t->u_clear_tid;
        t->u_clear_tid = 0;
    }
    process_unlock_slot(u_flags);
    if (u_tid != 0 && put_user_i32(u_tid, 0) == 0) {
        futex_wake(u_tid, 1);
    }
}

int64_t user_exec(const char *str_path, const char *str_name,
                  const struct exec_args *p_args, struct user_entry *p_out)
{
    struct thread *p_me = sched_current();
    if (p_me == NULL || p_me->pid == 0 || p_me->space == NULL) {
        return -EPERM;
    }
    const struct process *p_proc = process_find(p_me->pid);
    int i_uid = (p_proc != NULL) ? p_proc->uid : 0;
    int i_gid = (p_proc != NULL) ? p_proc->i_gid : 0;

    /* everything that can fail, done while the old program is still whole */
    struct user_image img;
    const char *str_why = NULL;
    int64_t i_why = image_build(str_path, str_name, p_args, i_uid, i_gid,
                                &img, &str_why);
    if (i_why < 0) {
        return i_why;
    }

    /*
     * the program is the process's, and every other thread of it goes
     * with the old one, before anything of the old one is let go of. one
     * doomed while it was being built tells nobody anything: it is ended
     * on its way back to ring 3, which is the only answer it gets
     */
    if (!alone(p_me)) {
        addrspace_put(img.p_space);
        return -EINTR;
    }
    futex_robust_release();
    forget_clear_tid(p_me->i_tid);

    struct addrspace *p_old = sched_swap_space(p_me, img.p_space);
    addrspace_put(p_old);

    signal_exec(process_signal_state(p_me->pid),
                process_thread_signals(p_me->i_tid));
    process_vfork_done(p_me->pid);

    process_set_name(p_me->pid, last_part(str_name));
    thread_set_name(p_me, last_part(str_name));
    process_set_exe(p_me->pid, img.str_exe);

    p_out->u_entry = img.u_entry;
    p_out->u_stack_top = img.u_stack_top;
    return 0;
}

/*
 * collect a process that has ended, letting go of its pipes first. a
 * killed one leaves them for the reaper, and a parent that collects it
 * before the reaper comes round would otherwise take the slot, and the
 * only record of those ends, with it
 */
bool user_collect(int i_pid, int *p_code)
{
    process_fds_release(i_pid);
    return process_collect(i_pid, p_code);
}

/* let a spawned process actually begin: its first thread is its pid */
void user_start(int pid)
{
    int i_thread = process_thread(pid);
    if (i_thread != 0) {
        sched_wake_thread(i_thread);
    }
}
