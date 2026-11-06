#define USING_GLOBALS
#include "a_compiler.h"
#include "a_internal.h"

/* runtime C source, embedded as a string (generated from a_runtime.c) */
#include "a_runtime_src.h"

/* unity build: frontend stages compiled into libtcc via this TU */
#include "a_lex.c"
#include "a_parse.c"
#include "a_sema.c"
#include "a_codegen_x64.c"

static int a_runtime_done = 0;

int a_ensure_runtime(TCCState *s1)
{
    if (a_runtime_done) return 0;
    /* Compile embedded runtime as C (support library; A programs themselves
     * are compiled A->machine code directly). Use host headers for libc. */
    {
        int of = s1->filetype, nb = s1->nb_sysinclude_paths, ni = s1->nb_include_paths;
        int onostd = s1->nostdinc;
        s1->filetype = AFF_TYPE_C;
        if (s1->tcc_lib_path) {
            char p[1024]; snprintf(p, sizeof p, "%s/include", s1->tcc_lib_path);
            tcc_add_sysinclude_path(s1, p);
        }
        tcc_add_sysinclude_path(s1, "build/tcclib/include");
        tcc_add_sysinclude_path(s1, "/usr/lib/gcc/x86_64-linux-gnu/13/include");
        tcc_add_sysinclude_path(s1, "/usr/local/include");
        tcc_add_sysinclude_path(s1, "/usr/include/x86_64-linux-gnu");
        tcc_add_sysinclude_path(s1, "/usr/include");
        /* Compile the runtime in-place, replicating tcc_compile()'s steps but
         * SKIPPING tccgen_finish()/preprocess_end(): those tear down the
         * global symbol table (sym_pop + sym_pools reset + tccpp_delete),
         * which codegen still needs for runtime symbol resolution.
         * The outer tcc_compile() performs the real finish/cleanup. */
        {
            int len = (int)strlen(a_runtime_src);
            tcc_open_bf(s1, "<a-runtime>", len);
            memcpy(file->buffer, a_runtime_src, len);
            preprocess_start(s1, AFF_TYPE_C);
            tccgen_init(s1);
            tccelf_begin_file(s1);
            tccgen_compile(s1);
            tccelf_end_file(s1);
            /* leave buffered files open: outer preprocess_end() closes them */
        }
        tcc_state = s1;
        s1->filetype = of; s1->nb_sysinclude_paths = nb; s1->nb_include_paths = ni; s1->nostdinc = onostd;
    }
    a_runtime_done = 1;
    return 0;
}

#include <execinfo.h>
#include <signal.h>
static void a_segv(int s){ void *b[64]; int n = backtrace(b, 64); backtrace_symbols_fd(b, n, 2); _exit(139); }
ST_FUNC int tcc_compile_a(TCCState *s1, const char *filename, const char *buf, int len)
{
    static int segv_armed = 0;
    if (!segv_armed) { signal(SIGSEGV, a_segv); signal(SIGBUS, a_segv); segv_armed = 1; }
    AModule am;
    memset(&am, 0, sizeof am);
    am.s1 = s1;
    am.filename = filename ? filename : "<a>";
    am.src = buf;
    am.len = len;
    if (a_lex(&am) < 0) return -1;
    if (a_parse(&am) < 0) return -1;
    if (a_sema(&am) < 0) return -1;
    if (a_ensure_runtime(s1) < 0) return -1;
    if (a_codegen(&am) < 0) return -1;
    a_module_free(&am);
    return 0;
}
