#ifndef LIBASTRA_H
#define LIBASTRA_H

#ifndef LIBASTRAAPI
# define LIBASTRAAPI
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*****************************/
/* set custom allocator for all allocations (optional), NULL for default. */
typedef void *ASTRAReallocFunc(void *ptr, unsigned long size);
LIBASTRAAPI void astra_set_realloc(ASTRAReallocFunc *my_realloc);

/*****************************/
typedef struct ASTRAState ASTRAState;

/* create a new ASTRA compilation context */
LIBASTRAAPI ASTRAState *astra_new(void);

/* free a ASTRA compilation context */
LIBASTRAAPI void astra_delete(ASTRAState *s);

/* set CONFIG_ASTRADIR at runtime */
LIBASTRAAPI void astra_set_lib_path(ASTRAState *s, const char *path);

/* set error/warning callback (optional) */
typedef void ASTRAErrorFunc(void *opaque, const char *msg);
LIBASTRAAPI void astra_set_error_func(ASTRAState *s, void *error_opaque, ASTRAErrorFunc *error_func);

/* set options as from command line (multiple supported) */
LIBASTRAAPI int astra_set_options(ASTRAState *s, const char *str);

/*****************************/
/* preprocessor */

/* add include path */
LIBASTRAAPI int astra_add_include_path(ASTRAState *s, const char *pathname);

/* add in system include path */
LIBASTRAAPI int astra_add_sysinclude_path(ASTRAState *s, const char *pathname);

/* define preprocessor symbol 'sym'. value can be NULL, sym can be "sym=val" */
LIBASTRAAPI void astra_define_symbol(ASTRAState *s, const char *sym, const char *value);

/* undefine preprocess symbol 'sym' */
LIBASTRAAPI void astra_undefine_symbol(ASTRAState *s, const char *sym);

/*****************************/
/* compiling */

/* add a file (C file, dll, object, library, ld script). Return -1 if error. */
LIBASTRAAPI int astra_add_file(ASTRAState *s, const char *filename);

/* compile a string containing a C source. Return -1 if error. */
LIBASTRAAPI int astra_compile_string(ASTRAState *s, const char *buf);

/* Tip: to have more specific errors/warnings from astra_compile_string(),
   you can prefix the string with "#line <num> \"<filename>\"\n" */

/*****************************/
/* linking commands */

/* set output type. MUST BE CALLED before any compilation */
LIBASTRAAPI int astra_set_output_type(ASTRAState *s, int output_type);
#define ASTRA_OUTPUT_MEMORY   1 /* output will be run in memory */
#define ASTRA_OUTPUT_EXE      2 /* executable file */
#define ASTRA_OUTPUT_DLL      4 /* dynamic library */
#define ASTRA_OUTPUT_OBJ      3 /* object file */
#define ASTRA_OUTPUT_PREPROCESS 5 /* only preprocess */

/* equivalent to -Lpath option */
LIBASTRAAPI int astra_add_library_path(ASTRAState *s, const char *pathname);

/* the library name is the same as the argument of the '-l' option */
LIBASTRAAPI int astra_add_library(ASTRAState *s, const char *libraryname);

/* add a symbol to the compiled program */
LIBASTRAAPI int astra_add_symbol(ASTRAState *s, const char *name, const void *val);

/* output an executable, library or object file. DO NOT call
   astra_relocate() before. */
LIBASTRAAPI int astra_output_file(ASTRAState *s, const char *filename);

/* link and run main() function and return its value. DO NOT call
   astra_relocate() before. */
LIBASTRAAPI int astra_run(ASTRAState *s, int argc, char **argv);

/* do all relocations (needed before using astra_get_symbol()) */
LIBASTRAAPI int astra_relocate(ASTRAState *s1);

/* return symbol value or NULL if not found */
LIBASTRAAPI void *astra_get_symbol(ASTRAState *s, const char *name);

/* list all (global) symbols and their values via 'symbol_cb()' */
LIBASTRAAPI void astra_list_symbols(ASTRAState *s, void *ctx,
    void (*symbol_cb)(void *ctx, const char *name, const void *val));

/* experimental/advanced section (see libastra_test_mt.c for an example) */

/* catch runtime exceptions (optionally limit backtraces at top_func),
   when using astra_set_options("-bt") and when not using astra_run() */
LIBASTRAAPI void *_astra_setjmp(ASTRAState *s1, void *jmp_buf, void *top_func, void *longjmp);
#define astra_setjmp(s1,jb,f) setjmp(_astra_setjmp(s1, jb, f, longjmp))

/* custom error printer for runtime exceptions. Returning 0 stops backtrace */
typedef int ASTRABtFunc(void *udata, void *pc, const char *file, int line, const char* func, const char *msg);
LIBASTRAAPI void astra_set_backtrace_func(ASTRAState *s1, void* userdata, ASTRABtFunc*);

#ifdef __cplusplus
}
#endif

#endif
