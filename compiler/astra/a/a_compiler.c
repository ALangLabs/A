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
    if (tcc_compile_string(s1, a_runtime_src) < 0) return -1;
    a_runtime_done = 1;
    return 0;
}

ST_FUNC int tcc_compile_a(TCCState *s1, const char *filename, const char *buf, int len)
{
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
