#ifndef ASTRA_A_INTERNAL_H
#define ASTRA_A_INTERNAL_H
#include "../tcc.h"
#include <stdint.h>
/* host TU already has real stdint types via tcc.h; skip a_runtime.h's
   freestanding typedefs (used only when tcc compiles the embedded runtime) */
#define ASTRA_RUNTIME_TYPES
#include "a_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================= tokens ================= */
typedef struct AToken {
    int kind, line, col, len;
    int bol;
    long long ival;
    double dval;
    unsigned long long uval;
    const char *p;
    char *sval;
    int raw;
    int is_char;
} AToken;

enum ATok {
    AT_EOF = 0,
    AT_ID, AT_INT, AT_FLOAT, AT_STR, AT_CHAR,
    AT_NL, AT_INDENT, AT_DEDENT,
    AT_LP = '(', AT_RP = ')', AT_LB = '[', AT_RB = ']',
    AT_LC = '{', AT_RC = '}', AT_COMMA = ',', AT_COLON = ':',
    AT_DOT = '.', AT_SEMI = ';', AT_QUESTION = '?', AT_BANG = '!',
    AT_TILDE = '~', AT_AMP = '&', AT_PIPE = '|', AT_CARET = '^',
    AT_PLUS = '+', AT_MINUS = '-', AT_STAR = '*', AT_SLASH = '/',
    AT_PERCENT = '%', AT_EQ = '=', AT_LT = '<', AT_GT = '>',
    AT_EQEQ = 200, AT_NE, AT_LE, AT_GE, /* 200: auto-increment from AT_GT('>'=62) would collide with AT_QUESTION('?')=63 */
    AT_PLUSEQ, AT_MINUSEQ, AT_STAREQ, AT_SLASHEQ, AT_PERCEQ,
    AT_AMPEQ, AT_BAREQ, AT_CARETEQ, AT_SHLEQ, AT_SHREQ,
    AT_SHL, AT_SHR, AT_POW,
    AT_ARROW, AT_FATARROW, AT_RANGE,
    AT_QUESTIONDOT, AT_QUESTIONQUESTION, AT_PIPEOP,
    AT_PLUSPLUS, AT_MINUSMINUS,
    AT_KW_FN = 256, AT_KW_CLASS, AT_KW_METACLASS, AT_KW_ENUM,
    AT_KW_TRAIT, AT_KW_IMPL, AT_KW_TYPE, AT_KW_COMPTIME,
    AT_KW_IMPORT, AT_KW_FROM, AT_KW_AS, AT_KW_MATCH, AT_KW_NONE,
    AT_KW_DO, AT_KW_WHERE, AT_KW_IF, AT_KW_ELIF, AT_KW_ELSE,
    AT_KW_WHILE, AT_KW_FOR, AT_KW_IN, AT_KW_LOOP, AT_KW_BREAK,
    AT_KW_CONTINUE, AT_KW_RETURN, AT_KW_DEFER, AT_KW_PASS,
    AT_KW_SELF, AT_KW_TRUE, AT_KW_FALSE, AT_KW_AND, AT_KW_OR,
    AT_KW_NOT,
    AT_KW_LAST
};

/* ================= AST ================= */
typedef struct ANode ANode;
struct ANode {
    int kind, line, col;
    ANode *a, *b, *c;
    ANode *next;
    ANode **list; int nlist;
    char *name;
    char *sval;
    long long ival;
    double dval;
    int aux;
    int loc_kind, loc_idx;
    ANode *scope;
};

enum {
    L_NONE = 0, L_LOCAL, L_PARAM, L_ENV, L_GLOBAL, L_BUILTIN, L_FUN
};

enum ANodeKind {
    AN_PROG, AN_FN, AN_LAMBDA, AN_PARAM, AN_SCOPE, AN_SYM, AN_BLOCK,
    AN_DECL, AN_ASSIGN, AN_RETURN, AN_IF, AN_IFBR, AN_WHILE, AN_DOWHILE,
    AN_FOR, AN_LOOP, AN_BREAK, AN_CONTINUE, AN_PASS, AN_DEFER, AN_EXPR,
    AN_MATCH, AN_ARM, AN_PAT, AN_IMPORT, AN_UNSUPPORTED,
    AN_INT, AN_FLOAT, AN_STR, AN_BOOL, AN_NONE, AN_STRRAW,
    AN_IDENT, AN_CALL, AN_FIELD, AN_INDEX, AN_SLICE,
    AN_BIN, AN_UNARY, AN_TERNARY, AN_RANGE,
    AN_TUPLE, AN_LIST, AN_DICT, AN_SET, AN_PAIR,
    AN_COMP, AN_CLAUSE, AN_TRY, AN_UNWRAP
};

enum { P_WILD = 0, P_LIT = 1, P_RANGE = 2, P_BIND = 3, P_OR = 4, P_TUPLE = 5 };
enum { C_LIST = 0, C_SET = 1, C_DICT = 2 };

/* per-function info produced by sema (AN_FN/AN_LAMBDA->scope) */
typedef struct {
    int nparams, nlocals, ncap, is_lambda;
    int *cap_kind, *cap_idx;
} AFuncInfo;

typedef struct AModule {
    TCCState *s1;
    const char *filename, *src;
    int len;
    AToken *toks;
    int ntok, itok, tokcap;
    ANode *prog;
    int had_error, nerrors;
    int nlocals_peak;
    int nmain_locals;   /* top-level "locals" (globals-scope nlocals) for main's frame */
} AModule;

static ANode *an_new(int k, int line) {
    ANode *n = (ANode *)tcc_mallocz(sizeof(ANode));
    n->kind = k; n->line = line;
    return n;
}
static char *a_strndup(const char *s, int n) {
    char *r = (char *)tcc_malloc(n + 1); memcpy(r, s, n); r[n] = 0; return r;
}
static char *a_strdup(const char *s) { return a_strndup(s, (int)strlen(s)); }
static void a_node_list_add(ANode *parent, ANode *child) {
    parent->list = (ANode **)tcc_realloc(parent->list, (parent->nlist + 1) * sizeof(ANode *));
    parent->list[parent->nlist++] = child;
}
static void a_module_free(AModule *m) { (void)m; }

void a_perror(AModule *m, int line, const char *fmt, ...);

int a_lex(AModule *m);
int a_parse(AModule *m);
int a_sema(AModule *m);
int a_codegen(AModule *m);
int a_ensure_runtime(TCCState *s1);
void tccpp_set_keep_tokens(int keep);

#endif
