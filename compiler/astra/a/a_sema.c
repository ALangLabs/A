/* A -> resolved AST. Assigns loc_kind/loc_idx to AN_IDENT/AN_CALL callees and
 * allocates frame slots (locals/params), global slots, env capture slots and
 * builtin ids. Python-like rules:
 *  - top-level `x = e` / `x: T = e` declares a global slot (or assigns existing)
 *  - inside a function, `x = e` declares a LOCAL unless x is already a
 *    param/local/env-capture in this function; globals are read-only there
 *    (assignment to a global name inside a fn shadows it as a new local)
 *  - `x op= e` requires x to already resolve
 *  - lambdas capture enclosing-function locals by value into an env object
 *  - builtin names resolve to L_BUILTIN unless shadowed
 */
#include "a_internal.h"
#include <string.h>

/* builtin name -> id table (must match a_runtime.c) */
static const struct { const char *n; int id; } builtins[] = {
    {"print",A_BI_PRINT},{"eprint",A_BI_EPRINT},{"len",A_BI_LEN},{"str",A_BI_STR},
    {"int",A_BI_INT},{"float",A_BI_FLOAT},{"bool",A_BI_BOOL},{"abs",A_BI_ABS},
    {"min",A_BI_MIN},{"max",A_BI_MAX},{"sum",A_BI_SUM},{"pow",A_BI_POW},
    {"sqrt",A_BI_SQRT},{"sin",A_BI_SIN},{"cos",A_BI_COS},{"tan",A_BI_TAN},
    {"asin",A_BI_ASIN},{"acos",A_BI_ACOS},{"atan",A_BI_ATAN},{"exp",A_BI_EXP},
    {"log",A_BI_LOG},{"floor",A_BI_FLOOR},{"ceil",A_BI_CEIL},{"round",A_BI_ROUND},
    {"trunc",A_BI_TRUNC},{"random",A_BI_RANDOM},{"random_int",A_BI_RANDOM_INT},
    {"range",A_BI_RANGE},{"set",A_BI_SET},{"tuple",A_BI_TUPLE},{"list",A_BI_LIST},
    {"dict",A_BI_DICT},{"map",A_BI_MAP},{"filter",A_BI_FILTER},{"any",A_BI_ANY},
    {"all",A_BI_ALL},{"some",A_BI_SOME},{"ok",A_BI_OK},{"err",A_BI_ERR},
    {"panic",A_BI_PANIC},{"assert",A_BI_ASSERT},{"assert_eq",A_BI_ASSERT_EQ},
    {"input",A_BI_INPUT},{"read_file",A_BI_READ_FILE},{"write_file",A_BI_WRITE_FILE},
    {"append_file",A_BI_APPEND_FILE},{"file_exists",A_BI_FILE_EXISTS},
    {"list_dir",A_BI_LIST_DIR},{"remove_file",A_BI_REMOVE_FILE},
    {"create_dir",A_BI_CREATE_DIR},{"remove_dir",A_BI_REMOVE_DIR},
    {"rename",A_BI_RENAME},{"current_dir",A_BI_CURRENT_DIR},{"repr",A_BI_REPR},
    {"type",A_BI_TYPE},{"isinstance",A_BI_ISINSTANCE},{"join",A_BI_JOIN},
    {"sorted",A_BI_SORTED},{"reversed",A_BI_REVERSED},{"enumerate",A_BI_ENUMERATE},
    {"zip",A_BI_ZIP},{0,0}
};
static int builtin_id(const char *n) {
    for (int i = 0; builtins[i].n; i++) if (!strcmp(builtins[i].n, n)) return builtins[i].id;
    return -1;
}

/* ---- scope structures ---- */
typedef struct {
    char *name;
    int kind;      /* L_LOCAL/L_PARAM/L_GLOBAL/L_ENV/L_BUILTIN */
    int idx;       /* slot / param ord / global idx / env idx / builtin id */
} MSym;

typedef struct MScope {
    struct MScope *parent;
    int is_func;          /* function boundary (fn decl or lambda or top) */
    MSym *syms; int nsyms, capsym;
    /* for lambdas: capture list — env slot -> source loc in enclosing frame */
    int *cap_kind, *cap_idx; char **cap_src_name; int ncap, capcap;
    int nlocals;          /* next local slot */
    int env_size;         /* allocated env slots */
} MScope;

typedef struct {
    AModule *m;
    MScope *top;          /* current scope */
    MScope *globals;      /* global scope (is_func=1) */
    int nglobals;
    int lam_count;
    int in_func;          /* >0 when inside any function/lambda */
} Sema;

static MSym *scope_find(MScope *s, const char *name) {
    for (; s; s = s->parent)
        for (int i = 0; i < s->nsyms; i++)
            if (!strcmp(s->syms[i].name, name)) return &s->syms[i];
    return NULL;
}
static MSym *scope_find_local(MScope *s, const char *name) {
    for (int i = 0; i < s->nsyms; i++)
        if (!strcmp(s->syms[i].name, name)) return &s->syms[i];
    return NULL;
}
static void scope_add(MScope *s, const char *name, int kind, int idx) {
    if (s->nsyms == s->capsym) {
        s->capsym = s->capsym ? s->capsym * 2 : 16;
        s->syms = tcc_realloc(s->syms, s->capsym * sizeof(MSym));
    }
    MSym *y = &s->syms[s->nsyms++];
    y->name = a_strdup(name); y->kind = kind; y->idx = idx;
}
static MScope *scope_new(MScope *parent, int is_func) {
    MScope *s = tcc_mallocz(sizeof(MScope));
    s->parent = parent; s->is_func = is_func;
    return s;
}

/* resolve name to loc in CURRENT function frame.
 * Crossing a function boundary inside a lambda => capture.
 * Returns 1 if resolved (fills kind/idx), 0 if unresolved. */
static int resolve_name(Sema *S, MScope *from, const char *name, int *kind, int *idx) {
    /* walk scopes; remember first function boundary crossed */
    MScope *s = from;
    int crossed = 0;
    for (; s; s = s->parent) {
        MSym *y = scope_find_local(s, name);
        if (y) {
            if (!crossed) { *kind = y->kind; *idx = y->idx; return 1; }
            /* found in an enclosing function while inside lambda:
             * only locals/params/env are capturable; globals/builtins pass through */
            if (y->kind == L_GLOBAL || y->kind == L_BUILTIN) { *kind = y->kind; *idx = y->idx; return 1; }
            /* capture: allocate env slot in the lambda scope (the scope
             * just inside the boundary — i.e. the deepest is_func scope
             * above 'from'... actually 'from' chain: find nearest func scope) */
            MScope *fnscope = from;
            while (fnscope && !fnscope->is_func) fnscope = fnscope->parent;
            /* fnscope is current function; captures recorded on IT? No:
             * env belongs to the lambda whose body we're resolving =
             * the innermost function scope containing 'from'. */
            MScope *lam = from;
            while (lam && !lam->is_func) lam = lam->parent;
            if (!lam) { *kind = y->kind; *idx = y->idx; return 1; }
            /* check existing capture */
            for (int i = 0; i < lam->ncap; i++)
                if (!strcmp(lam->cap_src_name[i], name)) { *kind = L_ENV; *idx = i; return 1; }
            if (lam->ncap == lam->capcap) {
                lam->capcap = lam->capcap ? lam->capcap * 2 : 8;
                lam->cap_kind = tcc_realloc(lam->cap_kind, lam->capcap * sizeof(int));
                lam->cap_idx = tcc_realloc(lam->cap_idx, lam->capcap * sizeof(int));
                lam->cap_src_name = tcc_realloc(lam->cap_src_name, lam->capcap * sizeof(char *));
            }
            int ci = lam->ncap++;
            lam->cap_src_name[ci] = a_strdup(name);
            lam->cap_kind[ci] = y->kind; lam->cap_idx[ci] = y->idx;
            *kind = L_ENV; *idx = ci;
            return 1;
        }
        if (s->is_func && s != from) crossed = 1;
    }
    /* builtins last */
    int b = builtin_id(name);
    if (b >= 0) { *kind = L_BUILTIN; *idx = b; return 1; }
    return 0;
}

/* declare-or-assign for `name = expr` statement.
 * mode: 0 = plain '=', requires existing for op-assign handled elsewhere */
static void declare_or_assign(Sema *S, MScope *fn, const char *name, int *kind, int *idx, int is_decl) {
    MSym *y = scope_find_local(fn, name);
    if (y) { *kind = y->kind; *idx = y->idx; return; }
    if (fn->is_func) {
        /* inside function: new local (params occupy first slots) */
        int slot = fn->nlocals++;
        scope_add(fn, name, L_LOCAL, slot);
        *kind = L_LOCAL; *idx = slot;
    } else {
        /* global scope */
        int g = S->nglobals++;
        scope_add(S->globals, name, L_GLOBAL, g);
        *kind = L_GLOBAL; *idx = g;
    }
    (void)is_decl;
}

static void sema_expr(Sema *S, ANode *n);
static void sema_stmt(Sema *S, ANode *n);

static void sema_block(Sema *S, ANode *b) {
    for (int i = 0; i < b->nlist; i++) sema_stmt(S, b->list[i]);
}

/* resolve ident node in place */
static void sema_ident(Sema *S, ANode *n) {
    int k = L_NONE, i = 0;
    if (resolve_name(S, S->top, n->name, &k, &i)) {
        n->loc_kind = k; n->loc_idx = i;
    } else {
        a_perror(S->m, n->line, "undefined name '%s'", n->name);
        n->loc_kind = L_GLOBAL; n->loc_idx = 0;
    }
}

static void sema_fn_node(Sema *S, ANode *fn, int is_lambda, ANode *env_source) {
    (void)env_source;
    MScope *outer = S->top;
    MScope *fs = scope_new(outer, 1);
    S->top = fs;
    int was_in = S->in_func; S->in_func++;
    /* params */
    for (int i = 0; i < fn->nlist; i++) {
        ANode *p = fn->list[i];
        scope_add(fs, p->name, L_PARAM, i);
    }
    fs->nlocals = 0;
    /* capture resolution happens lazily during body walk (resolve_name) */
    if (is_lambda) {
        if (fn->a) sema_expr(S, fn->a);
    } else {
        sema_block(S, fn->b);
    }
    AFuncInfo *fi = tcc_mallocz(sizeof(AFuncInfo));
    fi->nparams = fn->nlist;
    fi->nlocals = fs->nlocals;
    fi->ncap = fs->ncap;
    fi->cap_kind = fs->cap_kind;
    fi->cap_idx = fs->cap_idx;
    fi->is_lambda = is_lambda;
    fn->scope = (ANode *)fi;
    S->top = outer;
    S->in_func = was_in;
}

static void sema_expr(Sema *S, ANode *n) {
    if (!n) return;
    switch (n->kind) {
    case AN_IDENT: sema_ident(S, n); break;
    case AN_INT: case AN_FLOAT: case AN_BOOL: case AN_NONE: case AN_STRRAW: break;
    case AN_STR:
        for (int i = 0; i < n->nlist; i++) {
            ANode *p = n->list[i];
            if (p->kind == AN_STRRAW) continue;
            sema_expr(S, p);
        }
        break;
    case AN_CALL:
        sema_expr(S, n->a);
        /* builtin call: callee is plain ident resolving to builtin -> fine */
        for (int i = 0; i < n->nlist; i++) {
            ANode *a = n->list[i];
            if (a->kind == AN_PAIR) sema_expr(S, a->a); else sema_expr(S, a);
        }
        break;
    case AN_FIELD: case AN_UNWRAP: case AN_TRY:
        sema_expr(S, n->a); break;
    case AN_INDEX: sema_expr(S, n->a); sema_expr(S, n->b); break;
    case AN_SLICE:
        sema_expr(S, n->a);
        if (n->b) sema_expr(S, n->b);
        if (n->c) sema_expr(S, n->c);
        if (n->next) sema_expr(S, n->next);
        break;
    case AN_BIN: sema_expr(S, n->a); sema_expr(S, n->b); break;
    case AN_UNARY: sema_expr(S, n->a); break;
    case AN_TERNARY: sema_expr(S, n->a); sema_expr(S, n->b); sema_expr(S, n->c); break;
    case AN_RANGE:
        if (n->a) sema_expr(S, n->a);
        if (n->b) sema_expr(S, n->b);
        if (n->next) sema_expr(S, n->next);
        break;
    case AN_TUPLE: case AN_LIST: case AN_SET:
        if (n->a && n->a->kind == AN_COMP) { sema_expr(S, n->a); break; }
        for (int i = 0; i < n->nlist; i++) sema_expr(S, n->list[i]);
        break;
    case AN_DICT:
        if (n->a && n->a->kind == AN_COMP) { sema_expr(S, n->a); break; }
        for (int i = 0; i < n->nlist; i++) { sema_expr(S, n->list[i]->a); sema_expr(S, n->list[i]->b); }
        break;
    case AN_PAIR: sema_expr(S, n->a); break;
    case AN_COMP: {
        /* each clause: declare loop var as local in current func, resolve iter/cond/elt */
        for (int i = 0; i < n->nlist; i++) {
            ANode *cl = n->list[i];
            int slot = -1;
            MScope *fn = S->top;
            while (fn && !fn->is_func) fn = fn->parent;
            if (fn) {
                MSym *y = scope_find_local(fn, cl->name);
                if (y) { slot = y->idx; }
                else { slot = fn->nlocals++; scope_add(fn, cl->name, L_LOCAL, slot); }
            }
            cl->aux = slot;
            sema_expr(S, cl->a);
            if (cl->b) sema_expr(S, cl->b);
        }
        sema_expr(S, n->a);
        if (n->b) sema_expr(S, n->b);
        break;
    }
    case AN_LAMBDA: sema_fn_node(S, n, 1, NULL); break;
    case AN_MATCH: {
        sema_expr(S, n->a);
        for (int i = 0; i < n->nlist; i++) {
            ANode *arm = n->list[i];
            /* pattern: bind names / resolve literal exprs */
            ANode *stack[64]; int sp = 0;
            if (arm->a) stack[sp++] = arm->a;
            while (sp) {
                ANode *p = stack[--sp];
                if (p->aux == P_BIND) {
                    /* declare binding in current function scope */
                    MScope *fn = S->top;
                    while (fn && !fn->is_func) fn = fn->parent;
                    MSym *y = fn ? scope_find_local(fn, p->name) : NULL;
                    if (y) { p->loc_kind = y->kind; p->loc_idx = y->idx; }
                    else if (fn) { int s = fn->nlocals++; scope_add(fn, p->name, L_LOCAL, s); p->loc_kind = L_LOCAL; p->loc_idx = s; }
                } else if (p->aux == P_LIT && p->a) sema_expr(S, p->a);
                else if (p->aux == P_RANGE) { if (p->a) sema_expr(S, p->a); if (p->b) sema_expr(S, p->b); }
                else if (p->aux == P_OR || p->aux == P_TUPLE) {
                    for (int j = 0; j < p->nlist; j++) if (sp < 64) stack[sp++] = p->list[j];
                }
            }
            if (arm->b) sema_expr(S, arm->b);
            sema_expr(S, arm->c);
        }
        break;
    }
    default: break;
    }
}

static void sema_assign_target(Sema *S, ANode *t, int *kind, int *idx) {
    if (t->kind == AN_IDENT) {
        /* op-assign (= handled by caller via is_decl) */
        int k = L_NONE, i = 0;
        if (resolve_name(S, S->top, t->name, &k, &i)) { *kind = k; *idx = i; t->loc_kind = k; t->loc_idx = i; return; }
        /* plain '=' on unresolved name -> declare */
        MScope *fn = S->top;
        while (fn && !fn->is_func) fn = fn->parent;
        if (fn) {
            int s = fn->nlocals++;
            scope_add(fn, t->name, L_LOCAL, s);
            *kind = L_LOCAL; *idx = s;
        } else {
            int g = S->nglobals++;
            scope_add(S->globals, t->name, L_GLOBAL, g);
            *kind = L_GLOBAL; *idx = g;
        }
        t->loc_kind = *kind; t->loc_idx = *idx;
        return;
    }
    /* index/field targets: just resolve subexprs; loc unused */
    sema_expr(S, t);
}

static void sema_stmt(Sema *S, ANode *n) {
    if (!n) return;
    switch (n->kind) {
    case AN_EXPR: sema_expr(S, n->a); break;
    case AN_DECL: {
        int k, i;
        declare_or_assign(S, S->top, n->name, &k, &i, 1);
        n->loc_kind = k; n->loc_idx = i;
        sema_expr(S, n->b);
        break;
    }
    case AN_ASSIGN: {
        if (n->aux == 1 && n->a->kind == AN_TUPLE) {
            /* destructure: declare each name, eval value once */
            sema_expr(S, n->b);
            for (int j = 0; j < n->a->nlist; j++) {
                ANode *el = n->a->list[j];
                if (el->kind == AN_IDENT && !(el->name[0] == '_' && !el->name[1])) {
                    int k, i;
                    declare_or_assign(S, S->top, el->name, &k, &i, 1);
                    el->loc_kind = k; el->loc_idx = i;
                }
            }
            break;
        }
        if (n->name[0] == '=' && !n->name[1]) {
            /* plain assignment: declare if new */
            if (n->a->kind == AN_IDENT) {
                int k = L_NONE, i = 0;
                if (resolve_name(S, S->top, n->a->name, &k, &i)) {
                    n->a->loc_kind = k; n->a->loc_idx = i;
                } else {
                    MScope *fn = S->top;
                    while (fn && !fn->is_func) fn = fn->parent;
                    if (fn) { int s = fn->nlocals++; scope_add(fn, n->a->name, L_LOCAL, s); n->a->loc_kind = L_LOCAL; n->a->loc_idx = s; }
                    else { int g = S->nglobals++; scope_add(S->globals, n->a->name, L_GLOBAL, g); n->a->loc_kind = L_GLOBAL; n->a->loc_idx = g; }
                }
            } else sema_expr(S, n->a);
        } else {
            /* op-assign: target must resolve */
            if (n->a->kind == AN_IDENT) {
                int k = L_NONE, i = 0;
                if (resolve_name(S, S->top, n->a->name, &k, &i)) { n->a->loc_kind = k; n->a->loc_idx = i; }
                else a_perror(S->m, n->line, "cannot assign to undefined name '%s'", n->a->name);
            } else sema_expr(S, n->a);
        }
        sema_expr(S, n->b);
        break;
    }
    case AN_RETURN: sema_expr(S, n->a); break;
    case AN_IF:
        for (int i = 0; i < n->nlist; i++) { sema_expr(S, n->list[i]->a); sema_block(S, n->list[i]->b); }
        if (n->c) sema_block(S, n->c);
        break;
    case AN_WHILE: sema_expr(S, n->a); sema_block(S, n->b); break;
    case AN_DOWHILE: sema_block(S, n->b); sema_expr(S, n->a); break;
    case AN_FOR: {
        sema_expr(S, n->a);
        MScope *fn = S->top;
        while (fn && !fn->is_func) fn = fn->parent;
        if (fn) {
            MSym *y = scope_find_local(fn, n->name);
            if (y) n->aux = y->idx;
            else { int s = fn->nlocals++; scope_add(fn, n->name, L_LOCAL, s); n->aux = s; }
        }
        sema_block(S, n->b);
        break;
    }
    case AN_LOOP: sema_block(S, n->b); break;
    case AN_BREAK: case AN_CONTINUE: case AN_PASS: break;
    case AN_DEFER: sema_stmt(S, n->a); break;
    case AN_BLOCK: sema_block(S, n); break;
    case AN_FN: sema_fn_node(S, n, 0, NULL); break;
    case AN_IMPORT: case AN_UNSUPPORTED: break;
    default: sema_expr(S, n); break;
    }
}

int a_sema(AModule *m) {
    Sema S; memset(&S, 0, sizeof S);
    S.m = m;
    S.globals = scope_new(NULL, 1);
    S.top = S.globals;
    /* pass 1: hoist top-level fn names into globals so calls resolve */
    for (int i = 0; i < m->prog->nlist; i++) {
        ANode *s = m->prog->list[i];
        if (s->kind == AN_FN && s->name) {
            if (!scope_find_local(S.globals, s->name)) {
                int g = S.nglobals++;
                scope_add(S.globals, s->name, L_GLOBAL, g);
            }
        }
    }
    sema_block(&S, m->prog);
    m->nlocals_peak = S.nglobals; /* reuse: global count for codegen */
    /* stash globals count in prog aux? codegen reads via scope — store on prog */
    m->prog->aux = S.nglobals;
    return m->had_error ? -1 : 0;
}
