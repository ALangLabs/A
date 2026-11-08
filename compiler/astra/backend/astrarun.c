/*
 *  ASTRA - compiler for the A language - Support for -run switch
 *
 *  Copyright (c) 2001-2004 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "astra.h"

/* only native compiler supports -run */
#ifdef ASTRA_IS_NATIVE


typedef struct rt_frame {
    addr_t ip, fp, sp;
} rt_frame;

static ASTRAState *g_s1;
/* semaphore to protect it */
ASTRA_SEM(static rt_sem);
static void rt_wait_sem(void) { WAIT_SEM(&rt_sem); }
static void rt_post_sem(void) { POST_SEM(&rt_sem); }
static int rt_get_caller_pc(addr_t *paddr, rt_frame *f, int level);
static void rt_exit(rt_frame *f, int code);

/* ------------------------------------------------------------- */
/* defined when included from lib/bt-exe.c */

#ifndef _WIN32
# include <sys/mman.h>
#endif

static int protect_pages(void *ptr, unsigned long length, int mode);
static int astra_relocate_ex(ASTRAState *s1, void *ptr, unsigned ptr_diff);
static void st_link(ASTRAState *s1);
static void st_unlink(ASTRAState *s1);
#ifdef _WIN64
static void *win64_add_function_table(ASTRAState *s1);
static void win64_del_function_table(void *);
#endif

#if !defined PAGESIZE
# if defined _SC_PAGESIZE
#  define PAGESIZE sysconf(_SC_PAGESIZE)
# elif defined __APPLE__
#  include <libkern/OSCacheControl.h>
#  define PAGESIZE getpagesize()
# else
#  define PAGESIZE 4096
# endif
#endif

#define PAGEALIGN(n) ((addr_t)n + (-(addr_t)n & (PAGESIZE-1)))

#if !_WIN32 && !__APPLE__
//#define CONFIG_SELINUX 1
#endif

/* use VirtualAlloc() instead of astra_malloc() */
#if defined _WIN32 && !defined CONFIG_RUNMEM_VIRTUALALLOC
# define CONFIG_RUNMEM_VIRTUALALLOC 1
#endif

static int rt_mem(ASTRAState *s1, int size)
{
    void *ptr;
    int ptr_diff = 0;
#ifdef CONFIG_SELINUX
    /* Using mmap instead of malloc */
    void *prw;
    char tmpfname[] = "/tmp/.astrarunXXXXXX";
    int fd = mkstemp(tmpfname);
    unlink(tmpfname);
    ftruncate(fd, size);

    ptr = mmap(NULL, size * 2, PROT_READ|PROT_EXEC, MAP_SHARED, fd, 0);
    /* mmap RW memory at fixed distance */
    prw = mmap((char*)ptr + size, size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_FIXED, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED || prw == MAP_FAILED)
	return astra_error_noabort("astrarun: could not map memory");
    ptr_diff = (char*)prw - (char*)ptr; /* = size; */
    //printf("map %p %p %p\n", ptr, prw, (void*)ptr_diff);
    size *= 2;
#elif CONFIG_RUNMEM_VIRTUALALLOC
    /* always page-aligned */
    ptr = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!ptr)
        return astra_error_noabort("astrarun: could not allocate memory");
#else
    ptr = astra_malloc(size += PAGESIZE); /* one extra page to align malloc memory */
#endif
    s1->run_ptr = ptr;
    s1->run_size = size;
    return ptr_diff;
}

/* ------------------------------------------------------------- */
/* Do all relocations (needed before using astra_get_symbol())
   Returns -1 on error. */

LIBASTRAAPI int astra_relocate(ASTRAState *s1)
{
    int size, ret, ptr_diff;

    if (s1->run_ptr)
        exit(astra_error_noabort("'astra_relocate()' twice is no longer supported"));
    size = astra_relocate_ex(s1, NULL, 0);
    if (size < 0)
        return -1;
    ptr_diff = rt_mem(s1, size);
    if (ptr_diff < 0)
        return -1;
    ret = astra_relocate_ex(s1, s1->run_ptr, ptr_diff);
    if (ret == 0)
        st_link(s1);
    return ret;
}

ST_FUNC void astra_run_free(ASTRAState *s1)
{
    unsigned size;
    void *ptr;
    int i;

    /* free any loaded DLLs */
    for ( i = 0; i < s1->nb_loaded_dlls; i++) {
        DLLReference *ref = s1->loaded_dlls[i];
        if ( ref->handle )
#ifdef _WIN32
            FreeLibrary((HMODULE)ref->handle);
#else
            dlclose(ref->handle);
#endif
    }
    /* unmap or unprotect and free memory */
    ptr = s1->run_ptr;
    if (NULL == ptr)
        return;
    st_unlink(s1);
#ifdef _WIN64
    win64_del_function_table(s1->run_function_table);
#endif
    size = s1->run_size;
#ifdef CONFIG_SELINUX
    munmap(ptr, size);
#elif CONFIG_RUNMEM_VIRTUALALLOC
    VirtualFree(ptr, size, MEM_RELEASE);
#else
    /* unprotect memory to make it usable for malloc again */
    protect_pages((void*)PAGEALIGN(ptr), size - PAGESIZE, 2 /*rw*/);
    astra_free(ptr);
#endif
}

#define RT_EXIT_ZERO 0xE0E00E0E /* passed from longjmp instead of '0' */

/* launch the compiled program with the given arguments */
LIBASTRAAPI int astra_run(ASTRAState *s1, int argc, char **argv)
{
    int (*prog_main)(int, char **, char **), ret;
    const char *top_sym;
    jmp_buf main_jb;

#if defined(__APPLE__)
    extern char ***_NSGetEnviron(void);
    char **envp = *_NSGetEnviron();
#elif defined(__OpenBSD__) || defined(__NetBSD__)  || defined(__FreeBSD__)
    extern char **environ;
    char **envp = environ;
#else
    char **envp = environ;
#endif

    /* astra -dt -run ... nothing to do if no main() */
    if ((s1->dflag & 16) && (addr_t)-1 == get_sym_addr(s1, "main", 0, 1))
        return 0;

    astra_add_symbol(s1, "__rt_exit", rt_exit);
    s1->run_main = "_runmain", top_sym = "main";
    if (s1->elf_entryname)
        s1->run_main = top_sym = s1->elf_entryname;
    astra_add_support(s1, "runmain.o");

    if (astra_relocate(s1) < 0)
        return -1;

    prog_main = (void*)get_sym_addr(s1, s1->run_main, 1, 1);
    if ((addr_t)-1 == (addr_t)prog_main)
        return -1;

    /* custom stdin for run_main, mainly if stdin is/was an input file.
     * fileno(stdin) should remain 0, as posix mandates to use the smallest
     * free fd, which is 0 after the initial fclose in freopen. windows too.
     * to set stdin to the tty, use /dev/tty (posix) or con (windows).
     */
    if (s1->run_stdin && !freopen(s1->run_stdin, "r", stdin)) {
        astra_error_noabort("failed to reopen stdin from '%s'", s1->run_stdin);
        return -1;
    }

    errno = 0; /* clean errno value */
    fflush(stdout);
    fflush(stderr);

    ret = astra_setjmp(s1, main_jb, astra_get_symbol(s1, top_sym));
    if (0 == ret) {
        ret = prog_main(argc, argv, envp);
    } else if (RT_EXIT_ZERO == ret) {
        ret = 0;
    }

    if (s1->dflag & 16 && ret) /* astra -dt -run ... */
        fprintf(s1->ppfp, "[returns %d]\n", ret), fflush(s1->ppfp);
    return ret;
}

/* ------------------------------------------------------------- */
/* remove all STB_LOCAL symbols */
static void cleanup_symbols(ASTRAState *s1)
{
    Section *s = s1->symtab;
    int sym_index, end_sym = s->data_offset / sizeof (ElfSym);
    /* reset symtab */
    s->data_offset = s->link->data_offset = s->hash->data_offset = 0;
    init_symtab(s);
    /* add global symbols again */
    for (sym_index = 1; sym_index < end_sym; ++sym_index) {
        ElfW(Sym) *sym = &((ElfW(Sym) *)s->data)[sym_index];
        const char *name = (char *)s->link->data + sym->st_name;
        if (ELFW(ST_BIND)(sym->st_info) == STB_LOCAL)
            continue;
        //printf("sym %s\n", name);
        put_elf_sym(s, sym->st_value, sym->st_size, sym->st_info, sym->st_other, sym->st_shndx, name);
    }
}

/* free all sections except symbols */
static void cleanup_sections(ASTRAState *s1)
{
    struct { Section **secs; int nb_secs; } *p = (void*)&s1->sections;
    int i, f = 2;
    do {
        for (i = --f; i < p->nb_secs; i++) {
            Section *s = p->secs[i];
            if (s == s1->symtab || s == s1->symtab->link || s == s1->symtab->hash) {
                s->data = astra_realloc(s->data, s->data_allocated = s->data_offset);
            } else {
                free_section(s), astra_free(s), p->secs[i] = NULL;
            }
        }
    } while (++p, f);
}

/* ------------------------------------------------------------- */
/* 0 = .text rwx  other rwx (memory >= 2 pages a 4096 bytes) */
/* 1 = .text rx   other rw (memory >= 3 pages) */
/* 2 = .text rx  .rdata ro  .data/.bss rw (memory >= 4 pages) */

/* Some targets implement secutiry options that do not allow write in
   executable code. These targets need CONFIG_RUNMEM_RO=1.
   The disadvantage of this is that it requires a little bit more memory. */

#ifndef CONFIG_RUNMEM_RO
# ifdef __APPLE__
#   define CONFIG_RUNMEM_RO 1
# else
#   define CONFIG_RUNMEM_RO 0
#  endif
#endif

/* relocate code. Return -1 on error, required size if ptr is NULL,
   otherwise copy code into buffer passed by the caller */
static int astra_relocate_ex(ASTRAState *s1, void *ptr, unsigned ptr_diff)
{
    Section *s;
    unsigned offset, length, align, i, k, f;
    unsigned n, copy;
    addr_t mem, addr;

    if (NULL == ptr) {
        s1->nb_errors = 0;
#ifdef ASTRA_TARGET_PE
        pe_output_file(s1, NULL);
#else
        astra_add_runtime(s1);
	resolve_common_syms(s1);
        build_got_entries(s1, 0);
#endif
    }

    offset = copy = 0;
    mem = (addr_t)ptr;
redo:
    if (s1->verbose == 2 && copy)
        printf(&"-----------------------------------------------------\n"[PTR_SIZE*2 - 8]);
    if (s1->nb_errors)
        return -1;
    if (copy == 3)
        return 0;

    for (k = 0; k < 3; ++k) { /* 0:rx, 1:ro, 2:rw sections */
        n = 0; addr = 0;
        for(i = 1; i < s1->nb_sections; i++) {
            static const char shf[] = {
                SHF_ALLOC|SHF_EXECINSTR, SHF_ALLOC, SHF_ALLOC|SHF_WRITE
                };
            s = s1->sections[i];
            if (shf[k] != (s->sh_flags & (SHF_ALLOC|SHF_WRITE|SHF_EXECINSTR)))
                continue;
            length = s->data_offset;
            if (copy == 2) {
                if (addr == 0)
                    addr = s->sh_addr;
                n = (s->sh_addr - addr) + length;
                continue;
            }
            if (copy) { /* final step: copy section data to memory */
                if (s1->verbose == 2)
                    printf("%d: %-16s %p  len %05x  align %04x\n",
                        k, s->name, (void*)s->sh_addr, length, s->sh_addralign);
                ptr = (void*)s->sh_addr;
                if (k == 0)
                    ptr = (void*)(s->sh_addr + ptr_diff);
                if (NULL == s->data || s->sh_type == SHT_NOBITS)
                    memset(ptr, 0, length);
                else
                    memcpy(ptr, s->data, length);
                continue;
            }

            if ((s->sh_flags & SHF_TLS) && length)
                return astra_error_noabort("thread-local storage not supported with -run");

            align = s->sh_addralign;
            if (++n == 1) {
#if defined ASTRA_TARGET_I386 || defined ASTRA_TARGET_X86_64
                /* To avoid that x86 processors would reload cached instructions
                   each time when data is written in the near, we need to make
                   sure that code and data do not share the same 64 byte unit */
                if (align < 64)
                    align = 64;
#endif
                /* start new page for different permissions */
                if (k <= CONFIG_RUNMEM_RO)
                    align = PAGESIZE;
            }
            s->sh_addralign = align;
            addr = k ? mem + ptr_diff : mem;
            offset += -(addr + offset) & (align - 1);
            s->sh_addr = mem ? addr + offset : 0;
            offset += length;
        }
        if (copy == 2) { /* set permissions */
            if (n == 0) /* no data  */
                continue;
#ifdef CONFIG_SELINUX
            if (k == 0) /* SHF_EXECINSTR has its own mapping */
                continue;
#endif
            f = k;
            if (f >= CONFIG_RUNMEM_RO) {
                if (f != 0)
                    continue;
                f = 3; /* change only SHF_EXECINSTR to rwx */
            }
            n = PAGEALIGN(n);
            if (s1->verbose == 2) {
                printf("protect         %3s %p  len %05x\n",
                    &"rx\0ro\0rw\0rwx"[f*3], (void*)addr, (unsigned)n);
            }
            if (protect_pages((void*)addr, n, f) < 0)
                return astra_error_noabort(
#ifdef _WIN32
                    "VirtualProtect failed");
#else
                    "mprotect failed (did you mean to configure --with-selinux?)");
#endif
        }
    }

    if (0 == mem)
        return PAGEALIGN(offset);

    if (++copy == 2) {
        goto redo;
    }
    if (copy == 3) {
#ifdef _WIN64
        s1->run_function_table = win64_add_function_table(s1);
#endif
        /* remove local symbols and free sections except symtab */
        cleanup_symbols(s1);
        cleanup_sections(s1);
        goto redo;
    }

    /* relocate symbols */
    relocate_syms(s1, s1->symtab, 1);
    if (s1->nb_errors)
        goto redo;
    /* relocate sections */
#ifdef ASTRA_TARGET_PE
    s1->pe_imagebase = mem;
#else
    relocate_plt(s1);
#endif
    relocate_sections(s1);
    goto redo;
}

/* ------------------------------------------------------------- */
/* allow to run code in memory */

static int protect_pages(void *ptr, unsigned long length, int mode)
{
#ifdef _WIN32
    static const unsigned char protect[] = {
        PAGE_EXECUTE_READ,
        PAGE_READONLY,
        PAGE_READWRITE,
        PAGE_EXECUTE_READWRITE
        };
    DWORD old;
    if (!VirtualProtect(ptr, length, protect[mode], &old))
        return -1;
#else
    static const unsigned char protect[] = {
        PROT_READ | PROT_EXEC,
        PROT_READ,
        PROT_READ | PROT_WRITE,
        PROT_READ | PROT_WRITE | PROT_EXEC
        };
    if (mprotect(ptr, length, protect[mode]))
        return -1;
#endif
/* XXX: BSD sometimes dump core with bad system call */
#if (defined ASTRA_TARGET_ARM && !TARGETOS_BSD) \
    || defined ASTRA_TARGET_ARM64 || defined ASTRA_TARGET_RISCV64
    if (mode == 0 || mode == 3) {
#ifdef _WIN32
        FlushInstructionCache(GetCurrentProcess(), ptr, length);
#else
        void __clear_cache(void *beginning, void *end);
        __clear_cache(ptr, (char *)ptr + length);
#endif
    }
#endif
    return 0;
}

#ifdef _WIN64
static void *win64_add_function_table(ASTRAState *s1)
{
    void *p = NULL;
    if (s1->uw_pdata) {
        p = (void*)s1->uw_pdata->sh_addr;
        RtlAddFunctionTable(
            (RUNTIME_FUNCTION*)p,
            s1->uw_pdata->data_offset / sizeof (RUNTIME_FUNCTION),
            s1->pe_imagebase
            );
        s1->uw_pdata = NULL;
    }
    return p;
}

static void win64_del_function_table(void *p)
{
    if (p) {
        RtlDeleteFunctionTable((RUNTIME_FUNCTION*)p);
    }
}
#endif

static void st_link(ASTRAState *s1)
{
    rt_wait_sem();
    s1->next = g_s1, g_s1 = s1;
    rt_post_sem();
}

/* remove 'el' from 'list' */
static void ptr_unlink(void *list, void *e, unsigned next)
{
    void **pp, **nn, *p;
    for (pp = list; !!(p = *pp); pp = nn) {
        nn = (void*)((char*)p + next); /* nn = &p->next; */
        if (p == e) {
            *pp = *nn;
            break;
        }
    }
}

static void st_unlink(ASTRAState *s1)
{
    rt_wait_sem();
    ptr_unlink(&g_s1, s1, offsetof(ASTRAState, next));
    rt_post_sem();
}

LIBASTRAAPI void *_astra_setjmp(ASTRAState *s1, void *p_jmp_buf, void *func, void *p_longjmp)
{
    s1->run_lj = p_longjmp;
    s1->run_jb = p_jmp_buf;
    return p_jmp_buf;
}

LIBASTRAAPI void astra_set_backtrace_func(ASTRAState *s1, void *data, ASTRABtFunc *func)
{
    s1->bt_func = func;
    s1->bt_data = data;
}

static ASTRAState *rt_find_state(rt_frame *f)
{
    ASTRAState *s;
    int level;
    addr_t pc;

    s = g_s1;
    if (NULL == s || NULL == s->next) {
        /* play it safe in the simple case when there is only one state */
        return s;
    }
    for (level = 0; level < 8; ++level) {
        if (rt_get_caller_pc(&pc, f, level) < 0)
            break;
        for (s = g_s1; s; s = s->next) {
            if (pc >= (addr_t)s->run_ptr
             && pc  < (addr_t)s->run_ptr + s->run_size)
                return s;
        }
    }
    return NULL;
}

static void rt_exit(rt_frame *f, int code)
{
    ASTRAState *s;
    rt_wait_sem();
    s = rt_find_state(f);
    rt_post_sem();
    if (s && s->run_lj) {
        if (code == 0)
            code = RT_EXIT_ZERO;
        ((void(*)(void*,int))s->run_lj)(s->run_jb, code);
    }
    exit(code);
}

/* ------------------------------------------------------------- */
static int rt_get_caller_pc(addr_t *paddr, rt_frame *f, int level)
{
    if (level)
        return -1;
    *paddr = f->ip;
    return 0;
}
/* ------------------------------------------------------------- */
#ifdef CONFIG_ASTRA_STATIC

/* dummy function for profiling */
ST_FUNC void *dlopen(const char *filename, int flag)
{
    return NULL;
}

ST_FUNC void dlclose(void *p)
{
}

ST_FUNC const char *dlerror(void)
{
    return "error";
}

typedef struct ASTRASyms {
    char *str;
    void *ptr;
} ASTRASyms;


/* add the symbol you want here if no dynamic linking is done */
static ASTRASyms astra_syms[] = {
#if !defined(CONFIG_ASTRABOOT)
#define ASTRASYM(a) { #a, &a, },
    ASTRASYM(printf)
    ASTRASYM(fprintf)
    ASTRASYM(fopen)
    ASTRASYM(fclose)
#undef ASTRASYM
#endif
    { NULL, NULL },
};

ST_FUNC void *dlsym(void *handle, const char *symbol)
{
    ASTRASyms *p;
    p = astra_syms;
    while (p->str != NULL) {
        if (!strcmp(p->str, symbol))
            return p->ptr;
        p++;
    }
    return NULL;
}

#endif /* CONFIG_ASTRA_STATIC */
#endif /* ASTRA_IS_NATIVE */
/* ------------------------------------------------------------- */
