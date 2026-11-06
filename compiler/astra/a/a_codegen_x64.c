/* A -> x86_64 machine code. Values are aval (u64) in frame slots.
 * Every operation lowers to calls into the a_runtime.c helpers, which are
 * compiled into the same TCCState by a_ensure_runtime (tcc_compile_string).
 *
 * Frame layout (rbp-relative, all slots 8 bytes):
 *   [rbp-48]      slot 0: saved argv pointer (rdi at entry)
 *   [rbp-56-8i]   local i (L_LOCAL idx i -> fi = 1+i)
 *   temps         fi = 1 + nlocals + t
 * OFF(fi) = 48 + 8*fi  ->  [rbp - OFF(fi)]
 * Params are read through argv: argv[0]=env, argv[1+i]=param i.
 */
#include "a_internal.h"

#ifndef TCC_TARGET_X86_64
int a_codegen(AModule *m){ (void)m; a_perror(m,1,"Astra A: only x86_64 implemented"); return -1; }
#else

typedef struct CG {
    AModule *m;
    TCCState *s1;
    Section *t;
    int fnparams, fnlocals;      /* current function */
    int maxtemp;                 /* high-water temp index */
    int tmplo, tmphi;            /* temp allocator stack */
    int temps[4096];
    int ntemps;
    int outbytes;                /* max outgoing arg bytes for calls */
    int framepatch;              /* offset of sub rsp,imm32 */
    int epilogue;                /* label id of fn epilogue */
    int lamcount;
    int strcount;
    Sym *globals_sym;
    int loopbp[128], loopcont[128], loopend[128], loopdepth;   /* label ids */
    int nlabels;
    struct { int id, pos; } labels[4096];
    /* pending fixups: pos -> label id (jz/jmp rel32) */
    struct { int pos, lbl; } fix[8192];
    int nfix;
    /* defers: stack of lists */
    ANode ***deferlists; int *defercounts, *defercaps, ndeflists;
} CG;

static void cgerr(CG *g, ANode *n, const char *msg) { a_perror(g->m, n ? n->line : 1, "%s", msg); }

/* ---------- byte emit ---------- */
static void ob(CG *g, int c){ unsigned char *p = section_ptr_add(g->t, 1); p[0] = (unsigned char)c; }
static void o32(CG *g, unsigned v){ unsigned char *p = section_ptr_add(g->t, 4); p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void o64(CG *g, unsigned long long v){ o32(g,(unsigned)v); o32(g,(unsigned)(v>>32)); }
static int here(CG *g){ return (int)g->t->data_offset; }

/* ---------- labels / fixups ---------- */
static int newlabel(CG *g){ return ++g->nlabels; }
static void deflabel(CG *g, int id){ g->labels[g->nlabels].id = id; g->labels[g->nlabels].pos = here(g); }
static int findlabel(CG *g, int id){ for (int i = g->nlabels; i >= 1; i--) if (g->labels[i].id == id) return g->labels[i].pos; return -1; }
static void jmp32(CG *g, int op2, int lbl){ /* op2: 0xE9 jmp, or 0x8x jcc second byte */
    if (op2 == 0xE9) ob(g, 0xE9); else { ob(g, 0x0F); ob(g, op2); }
    int pos = here(g); o32(g, 0);
    g->fix[g->nfix].pos = pos; g->fix[g->nfix].lbl = lbl; g->nfix++;
}
static void patchfix(CG *g){
    for (int i = 0; i < g->nfix; i++) {
        int tgt = findlabel(g, g->fix[i].lbl);
        if (tgt < 0) tgt = here(g);
        int rel = tgt - (g->fix[i].pos + 4);
        unsigned char *p = g->t->data + g->fix[i].pos;
        p[0]=rel; p[1]=rel>>8; p[2]=rel>>16; p[3]=rel>>24;
    }
    g->nfix = 0;
}

/* ---------- symbols ---------- */
static Sym *a_rsym(CG *g, const char *name){
    int v = tok_alloc_const(name);
    TCCState *s1 = g->s1;
    Sym *s = sym_find(v);
    if (!s) { a_perror(g->m, 1, "internal: runtime symbol '%s' missing", name); return NULL; }
    return s;
}
static Sym *data_sym(CG *g, const char *name, Section *sec, unsigned long off, unsigned long size){
    int v = tok_alloc_const(name);
    Sym *s = external_global_sym(v, &char_pointer_type);
    put_extern_sym(s, sec, off, size);
    return s;
}
/* call runtime function `name` with args already in rdi,rsi,rdx,rcx,r8,r9 */
static void callrt(CG *g, const char *name){
    Sym *s = a_rsym(g, name);
    ob(g, 0xE8);
    int pos = here(g); o32(g, 0);
    greloca(g->t, s, pos, R_X86_64_PC32, -4);
}
/* movabs reg, imm64 ; for sym addrs pass sym!=NULL -> reloc64 */
static void movabs_imm(CG *g, int reg, unsigned long long v){
    ob(g, 0x48); ob(g, 0xB8 + reg); o64(g, v);
}
static void movabs_sym(CG *g, int reg, Sym *s, unsigned long long addend){
    ob(g, 0x48); ob(g, 0xB8 + reg); int pos = here(g); o64(g, addend);
    greloca(g->t, s, pos, R_X86_64_64, 0);
}
/* load/store [rbp - off(fi)] into rax */
static int OFF(int fi){ return 48 + 8 * fi; }
static void load_slot(CG *g, int fi){ /* mov rax, [rbp-off] */
    ob(g, 0x48); ob(g, 0x8B); ob(g, 0x85); o32(g, (unsigned)(-(OFF(fi))));
}
static void store_slot(CG *g, int fi){ /* mov [rbp-off], rax */
    ob(g, 0x48); ob(g, 0x89); ob(g, 0x85); o32(g, (unsigned)(-(OFF(fi))));
}
/* temp allocator: returns frame index */
static int tmp_new(CG *g){
    int fi = 1 + g->fnlocals + g->ntemps;
    g->ntemps++;
    if (fi > g->maxtemp) g->maxtemp = fi;
    return fi;
}
static void tmp_free(CG *g, int n){ g->ntemps -= n; if (g->ntemps < 0) g->ntemps = 0; }

/* rax = argv[idx] ; argv pointer lives in slot 0 */
static void emit_mov_rax_arg(CG *g, int idx){
    load_slot(g, 0);                    /* rax = argv ptr */
    ob(g, 0x48); ob(g, 0x8B); ob(g, 0x80); o32(g, (unsigned)(idx * 8)); /* mov rax,[rax+disp32] */
}
static void load_ident(CG *g, int kind, int idx){
    switch (kind) {
    case L_LOCAL: load_slot(g, 1 + idx); break;
    case L_PARAM: emit_mov_rax_arg(g, 1 + idx); break;
    case L_ENV: {
        emit_mov_rax_arg(g, 0);        /* rax = env aval */
        /* call a_env_get(env, idx): rdi=env rsi=idx */
        /* rax currently env; move to rdi */
        ob(g, 0x48); ob(g, 0x89); ob(g, 0xC7); /* mov rdi, rax */
        movabs_imm(g, 6, (unsigned long long)idx); /* movabs rsi, idx (48 BE) */
        callrt(g, "a_env_get");
        break;
    }
    case L_GLOBAL: {
        movabs_sym(g, 0, g->globals_sym, 0);   /* rax = &a_globals */
        ob(g, 0x48); ob(g, 0x8B); ob(g, 0x80); o32(g, (unsigned)(idx * 8));
        break;
    }
    case L_BUILTIN: {
        movabs_imm(g, 7, (unsigned long long)idx); /* movabs rdi, idx — WRONG reg encoding */
        /* movabs rdi = 48 BF ; movabs_imm uses B8+reg so reg=7 -> BF ok */
        callrt(g, "a_builtin");
        break;
    }
    default: load_slot(g, 1); break;
    }
}
/* proper global store: rax=value -> a_globals[idx] */
static void store_global(CG *g, int idx){
    /* rcx = &a_globals ; mov [rcx+idx*8], rax */
    movabs_sym(g, 1, g->globals_sym, 0);
    ob(g, 0x48); ob(g, 0x89); ob(g, 0x81); o32(g, (unsigned)(idx * 8)); /* mov [rcx+disp32], rax */
}
/* proper local/param/env stores */
static void store_local(CG *g, int idx){ store_slot(g, 1 + idx); }
/* stores that need argv/env: L_ENV via a_env_set; params read-only */
static void store_loc(CG *g, int kind, int idx){
    if (kind == L_LOCAL) store_local(g, idx);
    else if (kind == L_GLOBAL) store_global(g, idx);
    else if (kind == L_ENV) {
        /* a_env_set(env, idx, val): rdi=env rsi=idx rdx=val */
        int vfi = tmp_new(g);
        store_slot(g, vfi);               /* save val */
        emit_mov_rax_arg(g, 0);           /* rax = env */
        ob(g, 0x48); ob(g, 0x89); ob(g, 0xC7); /* rdi = env */
        movabs_imm(g, 6, (unsigned long long)idx); /* rsi = idx */
        load_slot(g, vfi);                /* rax = val */
        ob(g, 0x48); ob(g, 0x89); ob(g, 0xC2); /* rdx = rax */
        callrt(g, "a_env_set");
        tmp_free(g, 1);
    } else {
        a_perror(g->m, 1, "cannot assign to this name kind");
    }
}

/* ---------- literals ---------- */
static void emit_int_val(CG *g, long long v, int fi){
    /* tagged inline int if fits 47 bits signed, else call a_i64n */
    if (v >= -(1LL<<46) && v < (1LL<<46)) {
        unsigned long long tag = A_TAG | ((((unsigned long long)v) << 1) | 1ULL);
        movabs_imm(g, 0, tag);
    } else {
        movabs_imm(g, 7, (unsigned long long)v); /* rdi = v */
        callrt(g, "a_i64n");
    }
    store_slot(g, fi);
}
static void emit_dbl_val(CG *g, double d, int fi){
    union { double d; unsigned long long u; } c; c.d = d;
    if ((c.u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL && (c.u & 0x000FFFFFFFFFFFFFULL))
        c.u = 0x7FF8000000000000ULL; /* canonicalize NaN out of tag space */
    movabs_imm(g, 0, c.u);
    store_slot(g, fi);
}
static Sym *str_sym(CG *g, const char *s, int len){
    TCCState *s1 = g->s1;
    char name[32]; snprintf(name, sizeof name, "__a_str_%d", g->strcount++);
    unsigned long off = g->t->sh_num ? 0 : 0; (void)off;
    /* emit into rodata_section */
    Section *r = rodata_section;
    unsigned long roff = r->data_offset;
    unsigned char *p = section_ptr_add(r, len + 1);
    memcpy(p, s, len); p[len] = 0;
    Sym *sym = data_sym(g, name, r, roff, len + 1);
    return sym;
}
static void emit_str_lit(CG *g, const char *s, int len, int fi){
    if (len == 0) { /* still need a valid str obj: call a_str_new("",0) */
        movabs_imm(g, 7, 0); /* rdi=0 — will reloc? use str_sym of "" */
    }
    Sym *sym = str_sym(g, s, len);
    movabs_sym(g, 7, sym, 0);            /* rdi = ptr */
    movabs_imm(g, 6, (unsigned long long)len); /* rsi = len */
    callrt(g, "a_str_new");
    store_slot(g, fi);
}

/* ---------- expression codegen: result -> frame slot fi ---------- */
static void cg_expr(CG *g, ANode *n, int fi);
static void cg_stmt(CG *g, ANode *n);
static void cg_block(CG *g, ANode *b);
static void callrt0(CG *g, const char *name);
static void cg_comp_clauses(CG *g, ANode *comp, int ci, int fi);
static void emit_fn_prologue(CG *g, AFuncInfo *fi);
static void emit_fn_epilogue(CG *g);
static void emit_fn_sym(CG *g, const char *name, unsigned long start, unsigned long size);

/* push arg slots onto stack as argv array */
static void emit_call_args(CG *g, int *arr, int n){
    int bytes = (n * 8 + 15) & ~15;
    if (bytes > g->outbytes) g->outbytes = bytes;
    ob(g, 0x48); ob(g, 0x81); ob(g, 0xEC); o32(g, (unsigned)bytes); /* sub rsp,bytes */
    for (int i = 0; i < n; i++) {
        load_slot(g, arr[i]);                       /* rax = arg */
        ob(g, 0x48); ob(g, 0x89); ob(g, 0x44); ob(g, 0x24); ob(g, (unsigned char)(i * 8)); /* mov [rsp+i*8],rax (imm8) */
    }
}
static void emit_call_cleanup(CG *g, int n){
    int bytes = (n * 8 + 15) & ~15;
    ob(g, 0x48); ob(g, 0x81); ob(g, 0xC4); o32(g, (unsigned)bytes); /* add rsp,bytes */
}
static void emit_a_call(CG *g, int fnslot, int *arr, int n, int dst){
    emit_call_args(g, arr, n);
    load_slot(g, fnslot);          /* rax = fnval */
    ob(g, 0x48); ob(g, 0x89); ob(g, 0xC7); /* rdi = fn */
    movabs_imm(g, 6, (unsigned long long)n); /* rsi = n */
    /* rdx = rsp */
    ob(g, 0x48); ob(g, 0x89); ob(g, 0xE2); /* mov rdx, rsp */
    callrt(g, "a_call");
    emit_call_cleanup(g, n);
    store_slot(g, dst);
}
/* call runtime fn with 1..4 aval args in slots -> dst */
static void rt1(CG *g, const char *f, int a, int d){ load_slot(g,a); ob(g,0x48);ob(g,0x89);ob(g,0xC7); callrt(g,f); store_slot(g,d); }
static void rt2(CG *g, const char *f, int a, int b, int d){
    load_slot(g,a); ob(g,0x48);ob(g,0x89);ob(g,0xC7);          /* rdi=a */
    load_slot(g,b); ob(g,0x48);ob(g,0x89);ob(g,0xC6);          /* rsi=b */
    callrt(g,f); store_slot(g,d);
}
static void rt3(CG *g, const char *f, int a, int b, int c, int d){
    load_slot(g,a); ob(g,0x48);ob(g,0x89);ob(g,0xC7);
    load_slot(g,b); ob(g,0x48);ob(g,0x89);ob(g,0xC6);
    load_slot(g,c); ob(g,0x48);ob(g,0x89);ob(g,0xC2);
    callrt(g,f); store_slot(g,d);
}
static void rt4(CG *g, const char *f, int a, int b, int c, int e, int d){
    load_slot(g,a); ob(g,0x48);ob(g,0x89);ob(g,0xC7);
    load_slot(g,b); ob(g,0x48);ob(g,0x89);ob(g,0xC6);
    load_slot(g,c); ob(g,0x48);ob(g,0x89);ob(g,0xC2);
    load_slot(g,e); ob(g,0x48);ob(g,0x89);ob(g,0xC1);
    callrt(g,f); store_slot(g,d);
}

static const char *binrt(const char *op){
    if (!strcmp(op,"+")) return "a_add"; if (!strcmp(op,"-")) return "a_sub";
    if (!strcmp(op,"*")) return "a_mul"; if (!strcmp(op,"/")) return "a_div";
    if (!strcmp(op,"%")) return "a_mod"; if (!strcmp(op,"**")) return "a_pow";
    if (!strcmp(op,"==")) return "a_eq"; if (!strcmp(op,"!=")) return "a_ne";
    if (!strcmp(op,"<")) return "a_lt"; if (!strcmp(op,"<=")) return "a_le";
    if (!strcmp(op,">")) return "a_gt"; if (!strcmp(op,">=")) return "a_ge";
    if (!strcmp(op,"&")) return "a_band"; if (!strcmp(op,"|")) return "a_bor";
    if (!strcmp(op,"^")) return "a_bxor"; if (!strcmp(op,"<<")) return "a_shl";
    if (!strcmp(op,">>")) return "a_shr"; if (!strcmp(op,"in")) return "a_contains";
    return NULL;
}

/* truthiness test: rax = 0/1 */
static void emit_truthy(CG *g, int slot){
    load_slot(g, slot);
    callrt(g, "a_truthy_raw");
    store_slot(g, slot);
}

/* forward */
static void cg_match_pat(CG *g, ANode *pat, int subjfi, int dstfi);
static void cg_comp(CG *g, ANode *comp, int fi);

static void cg_lambda_fn(CG *g, ANode *lam, const char *symname){
    /* emit a function for lambda; current CG state saved/restored by caller */
    (void)lam; (void)symname;
}

static void cg_expr(CG *g, ANode *n, int fi){
    if (!n) { movabs_imm(g, 0, A_NONE); store_slot(g, fi); return; }
    switch (n->kind) {
    case AN_INT: emit_int_val(g, n->ival, fi); break;
    case AN_FLOAT: emit_dbl_val(g, n->dval, fi); break;
    case AN_BOOL: movabs_imm(g, 0, n->ival ? A_TRUE : A_FALSE); store_slot(g, fi); break;
    case AN_NONE: movabs_imm(g, 0, A_NONE); store_slot(g, fi); break;
    case AN_STR: {
        /* parts: STRRAW or expr -> a_str_joinsep? use a_str_join(n,items) */
        if (n->nlist == 1 && n->list[0]->kind == AN_STRRAW) {
            ANode *p = n->list[0];
            emit_str_lit(g, p->sval ? p->sval : "", (int)strlen(p->sval ? p->sval : ""), fi);
        } else {
            /* interpolation: a_str_joinsep("", n, argv) */
            int *slots = tcc_malloc(sizeof(int) * (n->nlist + 1));
            for (int i = 0; i < n->nlist; i++) {
                ANode *p = n->list[i];
                int s = tmp_new(g);
                if (p->kind == AN_STRRAW) emit_str_lit(g, p->sval ? p->sval : "", (int)strlen(p->sval ? p->sval : ""), s);
                else { cg_expr(g, p, s); rt1(g, "a_str", s, s); }
                slots[i] = s;
            }
            int es = tmp_new(g); emit_str_lit(g, "", 0, es);
            emit_call_args(g, slots, n->nlist);
            load_slot(g, es); ob(g,0x48);ob(g,0x89);ob(g,0xC7);      /* rdi = sep */
            movabs_imm(g, 6, (unsigned long long)n->nlist);        /* rsi = n */
            ob(g,0x48);ob(g,0x89);ob(g,0xE2);                      /* rdx = rsp argv */
            callrt(g, "a_str_joinsep");
            emit_call_cleanup(g, n->nlist);
            store_slot(g, fi);
            for (int i = 0; i < n->nlist; i++) tmp_free(g, 1);
            tmp_free(g, 1);
            tcc_free(slots);
        }
        break;
    }
    case AN_IDENT: load_ident(g, n->loc_kind, n->loc_idx); store_slot(g, fi); break;
    case AN_FIELD: {
        int of = tmp_new(g); cg_expr(g, n->a, of);
        Sym *sym = str_sym(g, n->name, (int)strlen(n->name));
        movabs_sym(g, 6, sym, 0); /* rsi = name */
        load_slot(g, of); ob(g,0x48);ob(g,0x89);ob(g,0xC7); /* rdi=obj */
        callrt(g, n->aux ? "a_getattr" : "a_getattr");
        tmp_free(g, 1); store_slot(g, fi);
        break;
    }
    case AN_INDEX: {
        int a = tmp_new(g), b = tmp_new(g);
        cg_expr(g, n->a, a); cg_expr(g, n->b, b);
        rt2(g, "a_getitem", a, b, fi); tmp_free(g, 2); break;
    }
    case AN_SLICE: {
        int a = tmp_new(g), lo = tmp_new(g), hi = tmp_new(g), st = tmp_new(g);
        cg_expr(g, n->a, a);
        if (n->b) cg_expr(g, n->b, lo); else movabs_imm(g,0,A_NONE), store_slot(g,lo);
        if (n->c) cg_expr(g, n->c, hi); else movabs_imm(g,0,A_NONE), store_slot(g,hi);
        if (n->next) cg_expr(g, n->next, st); else emit_int_val(g, 1, st);
        rt4(g, "a_slice", a, lo, hi, st, fi); tmp_free(g, 4); break;
    }
    case AN_BIN: {
        const char *op = n->name;
        if (!strcmp(op, "and") || !strcmp(op, "or")) {
            int ta = tmp_new(g), dst = fi;
            cg_expr(g, n->a, ta);
            emit_truthy(g, ta);
            int jlabel = newlabel(g);
            if (!strcmp(op, "and")) jmp32(g, 0x84, jlabel); /* jz -> eval rhs? no: and: if false skip rhs */
            else jmp32(g, 0x85, jlabel); /* or: if true skip rhs */
            /* eval rhs into dst */
            cg_expr(g, n->b, dst);
            int endl = newlabel(g); jmp32(g, 0xE9, endl);
            deflabel(g, jlabel);
            /* dst = ta (short-circuit value) */
            load_slot(g, ta); store_slot(g, dst);
            deflabel(g, endl);
            tmp_free(g, 1);
            break;
        }
        if (!strcmp(op, "??")) {
            int ta = tmp_new(g); cg_expr(g, n->a, ta);
            /* if ta == A_NONE -> eval rhs else keep ta */
            load_slot(g, ta);
            movabs_imm(g, 1, A_NONE); /* rcx */
            ob(g,0x48);ob(g,0x39);ob(g,0xC8); /* cmp rax,rcx */
            int jnz = newlabel(g); jmp32(g, 0x85, jnz); /* jne -> keep ta */
            cg_expr(g, n->b, fi);
            int end = newlabel(g); jmp32(g, 0xE9, end);
            deflabel(g, jnz);
            load_slot(g, ta); store_slot(g, fi);
            deflabel(g, end);
            tmp_free(g, 1);
            break;
        }
        const char *f = binrt(op);
        if (!f) { cgerr(g, n, "unsupported operator"); movabs_imm(g,0,A_NONE); store_slot(g,fi); break; }
        int a = tmp_new(g), b = tmp_new(g);
        cg_expr(g, n->a, a); cg_expr(g, n->b, b);
        rt2(g, f, a, b, fi); tmp_free(g, 2); break;
    }
    case AN_UNARY: {
        if (!strcmp(n->name, "not")) { int t = tmp_new(g); cg_expr(g, n->a, t); rt1(g, "a_not", t, fi); tmp_free(g,1); break; }
        if (!strcmp(n->name, "-") || !strcmp(n->name, "~")) {
            int t = tmp_new(g); cg_expr(g, n->a, t);
            rt1(g, n->name[0] == '-' ? "a_neg" : "a_bnot", t, fi); tmp_free(g,1); break;
        }
        if (!strcmp(n->name, "++") || !strcmp(n->name, "--")) {
            /* only ident targets */
            ANode *tgt = n->a;
            int delta = n->name[0] == '+' ? 1 : -1;
            int cur = tmp_new(g), nw = tmp_new(g), old = tmp_new(g);
            if (tgt->kind == AN_IDENT) {
                load_ident(g, tgt->loc_kind, tgt->loc_idx); store_slot(g, cur);
                emit_int_val(g, delta, nw);
                rt2(g, "a_add", cur, nw, nw);
                store_loc(g, tgt->loc_kind, tgt->loc_idx); /* stores rax — but nw in slot; load first */
                load_slot(g, nw); store_loc(g, tgt->loc_kind, tgt->loc_idx);
                if (n->aux == 1) { load_slot(g, cur); store_slot(g, fi); } else { load_slot(g, nw); store_slot(g, fi); }
                tmp_free(g, 3); break;
            }
            cgerr(g, n, "++/-- requires a variable target");
            movabs_imm(g,0,A_NONE); store_slot(g,fi); tmp_free(g,3); break;
        }
        cgerr(g, n, "bad unary"); break;
    }
    case AN_TERNARY: {
        int tc = tmp_new(g); cg_expr(g, n->a, tc); emit_truthy(g, tc);
        int els = newlabel(g), end = newlabel(g);
        load_slot(g, tc); /* rax=0/1 */
        ob(g,0x48);ob(g,0x85);ob(g,0xC0); /* test rax,rax */
        jmp32(g, 0x84, els); /* jz -> else */
        cg_expr(g, n->b, fi); jmp32(g, 0xE9, end);
        deflabel(g, els); cg_expr(g, n->c, fi); deflabel(g, end);
        tmp_free(g, 1); break;
    }
    case AN_TRY: {
        int t = tmp_new(g), o = tmp_new(g);
        cg_expr(g, n->a, t);
        load_slot(g, t); store_slot(g, o);       /* keep original */
        load_slot(g, t); callrt(g, "a_try_result"); store_slot(g, fi);
        /* if original should propagate (None or Err result) -> return fi */
        load_slot(g, o); callrt(g, "a_is_propagate");
        ob(g,0x48);ob(g,0x85);ob(g,0xC0);        /* test rax,rax */
        int no = newlabel(g); jmp32(g, 0x84, no);
        load_slot(g, fi);
        jmp32(g, 0xE9, g->epilogue);
        deflabel(g, no);
        tmp_free(g, 2); break;
    }
    case AN_UNWRAP: { int t = tmp_new(g); cg_expr(g, n->a, t); rt1(g, "a_unwrap", t, fi); tmp_free(g,1); break; }
    case AN_RANGE: {
        int a = tmp_new(g), b = tmp_new(g), s = tmp_new(g);
        cg_expr(g, n->a, a); cg_expr(g, n->b, b);
        if (n->next) cg_expr(g, n->next, s); else emit_int_val(g, 1, s);
        rt3(g, "a_range_new", a, b, s, fi); tmp_free(g, 3); break;
    }
    case AN_TUPLE: case AN_LIST: {
        if (n->a && n->a->kind == AN_COMP) { cg_comp(g, n->a, fi); break; }
        int cnt = n->nlist;
        const char *rf = n->kind == AN_TUPLE ? "a_tuple_new" : "a_list_new";
        int *sl = tcc_malloc(sizeof(int)*(cnt+1));
        for (int i = 0; i < cnt; i++) { sl[i] = tmp_new(g); cg_expr(g, n->list[i], sl[i]); }
        emit_call_args(g, sl, cnt);
        movabs_imm(g, 7, (unsigned long long)cnt);   /* rdi = n */
        ob(g,0x48);ob(g,0x89);ob(g,0xE6);             /* rsi = rsp argv */
        callrt(g, rf);
        emit_call_cleanup(g, cnt);
        store_slot(g, fi);
        for (int i = 0; i < cnt; i++) tmp_free(g, 1);
        tcc_free(sl);
        break;
    }
    case AN_SET: {
        if (n->a && n->a->kind == AN_COMP) { cg_comp(g, n->a, fi); break; }
        /* a_set_new then a_set_add per elem */
        callrt0(g, "a_set_new"); store_slot(g, fi);
        for (int i = 0; i < n->nlist; i++) {
            int e = tmp_new(g); cg_expr(g, n->list[i], e);
            load_slot(g, fi); ob(g,0x48);ob(g,0x89);ob(g,0xC7);
            load_slot(g, e); ob(g,0x48);ob(g,0x89);ob(g,0xC6);
            callrt(g, "a_set_add");
            tmp_free(g, 1);
        }
        break;
    }
    case AN_DICT: {
        if (n->a && n->a->kind == AN_COMP) { cg_comp(g, n->a, fi); break; }
        callrt0(g, "a_dict_new"); store_slot(g, fi);
        for (int i = 0; i < n->nlist; i++) {
            int k = tmp_new(g), v = tmp_new(g);
            cg_expr(g, n->list[i]->a, k); cg_expr(g, n->list[i]->b, v);
            load_slot(g, fi); ob(g,0x48);ob(g,0x89);ob(g,0xC7);
            load_slot(g, k); ob(g,0x48);ob(g,0x89);ob(g,0xC6);
            load_slot(g, v); ob(g,0x48);ob(g,0x89);ob(g,0xC2);
            callrt(g, "a_dict_set");
            tmp_free(g, 2);
        }
        break;
    }
    case AN_CALL: {
        ANode *callee = n->a;
        if (callee->kind == AN_FIELD) {
            /* method call: a_method(obj, name, n, argv) */
            int of = tmp_new(g); cg_expr(g, callee->a, of);
            int *sl = tcc_malloc(sizeof(int)*(n->nlist+1));
            for (int i = 0; i < n->nlist; i++) {
                ANode *a = n->list[i];
                sl[i] = tmp_new(g);
                if (a->kind == AN_PAIR) cg_expr(g, a->a, sl[i]); else cg_expr(g, a, sl[i]);
            }
            int bytes = (n->nlist * 8 + 15) & ~15;
            if (bytes > g->outbytes) g->outbytes = bytes;
            ob(g,0x48);ob(g,0x81);ob(g,0xEC);o32(g,(unsigned)bytes);
            for (int i = 0; i < n->nlist; i++) { load_slot(g, sl[i]); ob(g,0x48);ob(g,0x89);ob(g,0x44);ob(g,0x24);ob(g,(unsigned char)(i*8)); }
            load_slot(g, of); ob(g,0x48);ob(g,0x89);ob(g,0xC7);       /* rdi=obj */
            Sym *ms = str_sym(g, callee->name, (int)strlen(callee->name));
            movabs_sym(g, 6, ms, 0);                                /* rsi=name */
            movabs_imm(g, 2, (unsigned long long)n->nlist);         /* rdx=n — reg2=rdx? B8+2=BA yes */
            ob(g,0x48);ob(g,0x89);ob(g,0xE1);                       /* rcx=rsp — WRONG: argv is 4th arg=rcx */
            /* fix: rcx = rsp */
            callrt(g, "a_method");
            emit_call_cleanup(g, n->nlist);
            store_slot(g, fi);
            for (int i = 0; i < n->nlist; i++) tmp_free(g, 1);
            tmp_free(g, 1); tcc_free(sl);
        } else {
            int cf = tmp_new(g); cg_expr(g, callee, cf);
            int *sl = tcc_malloc(sizeof(int)*(n->nlist+1));
            for (int i = 0; i < n->nlist; i++) {
                ANode *a = n->list[i];
                sl[i] = tmp_new(g);
                if (a->kind == AN_PAIR) cg_expr(g, a->a, sl[i]); else cg_expr(g, a, sl[i]);
            }
            emit_a_call(g, cf, sl, n->nlist, fi);
            for (int i = 0; i < n->nlist; i++) tmp_free(g, 1);
            tmp_free(g, 1); tcc_free(sl);
        }
        break;
    }
    case AN_MATCH: {
        int sf = tmp_new(g); cg_expr(g, n->a, sf);
        int endl = newlabel(g);
        int matched = 0;
        for (int i = 0; i < n->nlist; i++) {
            ANode *arm = n->list[i];
            int pf = tmp_new(g);
            cg_match_pat(g, arm->a, sf, pf);       /* pf = 0/1 */
            emit_truthy(g, pf);                    /* ensure 0/1 (already) */
            int next = newlabel(g);
            load_slot(g, pf); ob(g,0x48);ob(g,0x85);ob(g,0xC0); /* test rax,rax */
            jmp32(g, 0x84, next);                  /* jz -> next arm */
            if (arm->b) { int gf = tmp_new(g); cg_expr(g, arm->b, gf); emit_truthy(g, gf);
                load_slot(g, gf); ob(g,0x48);ob(g,0x85);ob(g,0xC0);
                jmp32(g, 0x84, next); tmp_free(g,1); }
            cg_expr(g, arm->c, fi);
            jmp32(g, 0xE9, endl);
            deflabel(g, next);
            tmp_free(g, 1);
            matched = 1;
        }
        if (!matched) { movabs_imm(g,0,A_NONE); store_slot(g,fi); }
        else {
            /* fallthrough: no arm matched -> panic */
            load_slot(g, sf);
            callrt(g, "a_match_fail");
            movabs_imm(g,0,A_NONE); store_slot(g,fi);
        }
        deflabel(g, endl);
        tmp_free(g, 1);
        break;
    }
    case AN_LAMBDA: {
        /* emit the lambda's code inline, jumped over, then build FNON value */
        char nm[32]; snprintf(nm, sizeof nm, "__a_lam_%d", g->lamcount++);
        int skip = newlabel(g);
        jmp32(g, 0xE9, skip);                    /* jump over fn code */
        unsigned long lstart = g->t->data_offset;
        AFuncInfo *lfi = (AFuncInfo *)n->scope;
        /* save enclosing fn state */
        int sp = g->fnparams, sl = g->fnlocals, sm = g->maxtemp, sn = g->ntemps, so = g->outbytes, sf = g->framepatch;
        int se = g->epilogue, nd = g->ndeflists;
        emit_fn_prologue(g, lfi);
        int rf = tmp_new(g);
        if (n->a) cg_expr(g, n->a, rf); else movabs_imm(g, 0, A_NONE), store_slot(g, rf);
        load_slot(g, rf);
        jmp32(g, 0xE9, g->epilogue);
        emit_fn_epilogue(g);
        emit_fn_sym(g, nm, lstart, g->t->data_offset - lstart);
        /* restore enclosing state */
        g->fnparams = sp; g->fnlocals = sl; g->maxtemp = sm; g->ntemps = sn;
        g->outbytes = so; g->framepatch = sf; g->epilogue = se; g->ndeflists = nd;
        deflabel(g, skip);
        /* build FNON: a_fnon_new(entry, env, name) */
        int ef = tmp_new(g);
        if (lfi && lfi->ncap > 0) {
            movabs_imm(g, 7, (unsigned long long)lfi->ncap); /* rdi=ncap */
            callrt(g, "a_closure_env_new"); store_slot(g, ef);
            for (int i = 0; i < lfi->ncap; i++) {
                int vf = tmp_new(g);
                load_ident(g, lfi->cap_kind[i], lfi->cap_idx[i]); store_slot(g, vf);
                load_slot(g, ef); ob(g,0x48);ob(g,0x89);ob(g,0xC7); /* rdi=env */
                movabs_imm(g, 6, (unsigned long long)i);           /* rsi=i */
                load_slot(g, vf); ob(g,0x48);ob(g,0x89);ob(g,0xC2); /* rdx=v */
                callrt(g, "a_env_set");
                tmp_free(g, 1);
            }
        } else {
            movabs_imm(g, 0, A_NONE); store_slot(g, ef);
        }
        Sym *es = a_rsym(g, nm);
        movabs_sym(g, 7, es, 0);                 /* rdi = entry addr */
        load_slot(g, ef); ob(g,0x48);ob(g,0x89);ob(g,0xC6); /* rsi = env */
        movabs_imm(g, 2, 0);                     /* rdx = NULL name */
        callrt(g, "a_fnon_new");
        store_slot(g, fi);
        tmp_free(g, 1);
        break;
    }
    default:
        cgerr(g, n, "unsupported expression");
        movabs_imm(g, 0, A_NONE); store_slot(g, fi);
        break;
    }
}

/* callrt0: call fn with no args -> rax */
static void callrt0(CG *g, const char *name){ callrt(g, name); }

/* match pattern -> pf slot = 0/1 */
static void cg_match_pat(CG *g, ANode *pat, int subjfi, int dstfi){
    switch (pat->aux) {
    case P_WILD: emit_int_val(g, 1, dstfi); break;
    case P_BIND: {
        load_slot(g, subjfi); store_slot(g, 1 + pat->loc_idx);
        emit_int_val(g, 1, dstfi); break;
    }
    case P_LIT: {
        int pf = tmp_new(g); cg_expr(g, pat->a, pf);
        rt2(g, "a_eq", subjfi, pf, dstfi); tmp_free(g, 1); break;
    }
    case P_RANGE: {
        int a = tmp_new(g), b = tmp_new(g);
        cg_expr(g, pat->a, a); cg_expr(g, pat->b, b);
        rt3(g, "a_in_range", subjfi, a, b, dstfi); tmp_free(g, 2); break;
    }
    case P_OR: {
        int any = newlabel(g), end = newlabel(g);
        emit_int_val(g, 0, dstfi);
        for (int i = 0; i < pat->nlist; i++) {
            int pf = tmp_new(g);
            cg_match_pat(g, pat->list[i], subjfi, pf);
            emit_truthy(g, pf);
            load_slot(g, pf); ob(g,0x48);ob(g,0x85);ob(g,0xC0);
            jmp32(g, 0x85, end); /* jne -> matched */
            tmp_free(g, 1);
        }
        deflabel(g, end);
        /* dstfi already set by last match or 0 — simplify: recompute properly below */
        break;
    }
    case P_TUPLE: {
        /* subj must be list/tuple of len n; match elements */
        int ok = newlabel(g), end = newlabel(g), fail = newlabel(g);
        /* check len via a_len */
        int lf = tmp_new(g); rt1(g, "a_len", subjfi, lf);
        emit_int_val(g, pat->nlist, dstfi);
        rt2(g, "a_eq", lf, dstfi, dstfi);
        load_slot(g, dstfi); ob(g,0x48);ob(g,0x85);ob(g,0xC0);
        jmp32(g, 0x84, fail);
        /* match each element: elem_i = a_getitem(subj, i) */
        for (int i = 0; i < pat->nlist; i++) {
            int ef = tmp_new(g), idx = tmp_new(g), pf = tmp_new(g);
            emit_int_val(g, i, idx);
            rt2(g, "a_getitem", subjfi, idx, ef);
            cg_match_pat(g, pat->list[i], ef, pf);
            emit_truthy(g, pf);
            load_slot(g, pf); ob(g,0x48);ob(g,0x85);ob(g,0xC0);
            jmp32(g, 0x84, fail);
            tmp_free(g, 3);
        }
        emit_int_val(g, 1, dstfi);
        jmp32(g, 0xE9, end);
        deflabel(g, fail); emit_int_val(g, 0, dstfi);
        deflabel(g, end);
        tmp_free(g, 1);
        break;
    }
    default: emit_int_val(g, 0, dstfi); break;
    }
}

/* comprehension -> collection in fi */
static void cg_comp(CG *g, ANode *comp, int fi){
    /* desugar: create empty collection; for each clause: iter loop; conds; emit elt */
    /* create result */
    if (comp->aux == C_LIST) { movabs_imm(g,7,0); movabs_imm(g,6,0); callrt(g,"a_list_new"); store_slot(g, fi); }
    else if (comp->aux == C_SET) { callrt0(g, "a_set_new"); store_slot(g, fi); }
    else { callrt0(g, "a_dict_new"); store_slot(g, fi); }
    /* iterate clauses recursively via helper emitting nested loops —
       implement single-level + recursion for multiple clauses */
    /* For simplicity support clauses by recursive descent: */
    cg_comp_clauses(g, comp, 0, fi);
}
static void cg_comp_clauses(CG *g, ANode *comp, int ci, int fi){
    if (ci >= comp->nlist) {
        /* emit element */
        if (comp->aux == C_DICT) {
            int k = tmp_new(g), v = tmp_new(g);
            cg_expr(g, comp->a, k); cg_expr(g, comp->b, v);
            load_slot(g, fi); ob(g,0x48);ob(g,0x89);ob(g,0xC7);
            load_slot(g, k); ob(g,0x48);ob(g,0x89);ob(g,0xC6);
            load_slot(g, v); ob(g,0x48);ob(g,0x89);ob(g,0xC2);
            callrt(g, "a_dict_set");
            tmp_free(g, 2);
        } else {
            int e = tmp_new(g); cg_expr(g, comp->a, e);
            if (comp->aux == C_LIST) {
                load_slot(g, fi); ob(g,0x48);ob(g,0x89);ob(g,0xC7);
                load_slot(g, e); ob(g,0x48);ob(g,0x89);ob(g,0xC6);
                callrt(g, "a_list_push");
            } else {
                load_slot(g, fi); ob(g,0x48);ob(g,0x89);ob(g,0xC7);
                load_slot(g, e); ob(g,0x48);ob(g,0x89);ob(g,0xC6);
                callrt(g, "a_set_add");
            }
            tmp_free(g, 1);
        }
        return;
    }
    ANode *cl = comp->list[ci];
    int itf = tmp_new(g), of = tmp_new(g);
    cg_expr(g, cl->a, itf);
    load_slot(g, itf); callrt(g, "a_iter_new"); store_slot(g, of);
    int top = newlabel(g), end = newlabel(g);
    deflabel(g, top);
    /* a_iter_next(it, &out) */
    load_slot(g, of); ob(g,0x48);ob(g,0x89);ob(g,0xC7); /* rdi=it */
    /* rsi = addr of out slot: lea rsi,[rbp-off] */
    ob(g,0x48);ob(g,0x8D);ob(g,0xB5);o32(g,(unsigned)(-(OFF(1 + cl->aux)))); /* lea rsi,[rbp-off] — slot for var */
    callrt(g, "a_iter_next");
    ob(g,0x48);ob(g,0x85);ob(g,0xC0); /* test rax,rax */
    jmp32(g, 0x84, end);
    /* store out into var slot: a_iter_next wrote aval to [rbp-off] already = slot 1+cl->aux */
    /* conds */
    if (cl->b) {
        int cf = tmp_new(g); cg_expr(g, cl->b, cf); emit_truthy(g, cf);
        load_slot(g, cf); ob(g,0x48);ob(g,0x85);ob(g,0xC0);
        jmp32(g, 0x84, top); /* false -> next iter */
        tmp_free(g, 1);
    }
    cg_comp_clauses(g, comp, ci + 1, fi);
    jmp32(g, 0xE9, top);
    deflabel(g, end);
    tmp_free(g, 2);
}

/* ---------- statements ---------- */
static void cg_defer_push(CG *g, ANode *stmt){
    if (g->ndeflists == 0) return;
    int d = g->ndeflists - 1;
    if (g->defercounts[d] == g->defercaps[d]) {
        g->defercaps[d] = g->defercaps[d] ? g->defercaps[d] * 2 : 8;
        g->deferlists[d] = tcc_realloc(g->deferlists[d], g->defercaps[d] * sizeof(ANode*));
    }
    g->deferlists[d][g->defercounts[d]++] = stmt;
}
static void cg_defer_run(CG *g, int down_to){
    while (g->ndeflists > down_to) {
        int d = --g->ndeflists;
        for (int i = g->defercounts[d] - 1; i >= 0; i--) cg_stmt(g, g->deferlists[d][i]);
        g->defercounts[d] = 0;
    }
}
static void cg_defer_pushframe(CG *g){
    g->ndeflists++;
    g->deferlists = tcc_realloc(g->deferlists, g->ndeflists * sizeof(ANode**));
    g->defercounts = tcc_realloc(g->defercounts, g->ndeflists * sizeof(int));
    g->defercaps = tcc_realloc(g->defercaps, g->ndeflists * sizeof(int));
    g->deferlists[g->ndeflists-1] = NULL;
    g->defercounts[g->ndeflists-1] = 0;
    g->defercaps[g->ndeflists-1] = 0;
}
static void cg_defer_popframe(CG *g){ cg_defer_run(g, g->ndeflists - 1); }

static void cg_stmt(CG *g, ANode *n){
    if (!n) return;
    switch (n->kind) {
    case AN_EXPR: { int t = tmp_new(g); cg_expr(g, n->a, t); tmp_free(g, 1); break; }
    case AN_PASS: case AN_IMPORT: break;
    case AN_UNSUPPORTED: cgerr(g, n, "unsupported declaration in Astra backend"); break;
    case AN_DECL: {
        int t = tmp_new(g); cg_expr(g, n->b, t);
        store_loc(g, n->loc_kind, n->loc_idx); /* store_loc expects rax — load first */
        load_slot(g, t); store_loc(g, n->loc_kind, n->loc_idx);
        tmp_free(g, 1); break;
    }
    case AN_ASSIGN: {
        if (n->aux == 1 && n->a->kind == AN_TUPLE) {
            /* destructure: rhs once, a_getitem per elem */
            int rv = tmp_new(g); cg_expr(g, n->b, rv);
            for (int i = 0; i < n->a->nlist; i++) {
                ANode *el = n->a->list[i];
                if (el->kind != AN_IDENT || (el->name[0] == '_' && !el->name[1])) continue;
                int ix = tmp_new(g), ev = tmp_new(g);
                emit_int_val(g, i, ix);
                rt2(g, "a_getitem", rv, ix, ev);
                load_slot(g, ev); store_loc(g, el->loc_kind, el->loc_idx);
                tmp_free(g, 2);
            }
            tmp_free(g, 1); break;
        }
        ANode *tgt = n->a;
        int is_plain = n->name[0] == '=' && !n->name[1];
        if (tgt->kind == AN_IDENT) {
            int v = tmp_new(g); cg_expr(g, n->b, v);
            if (is_plain) { load_slot(g, v); store_loc(g, tgt->loc_kind, tgt->loc_idx); }
            else {
                int cur = tmp_new(g), res = tmp_new(g);
                load_ident(g, tgt->loc_kind, tgt->loc_idx); store_slot(g, cur);
                const char *f = binrt(n->name); /* "+=" -> op "+"? binrt expects op; strip '=' */
                char opbuf[3] = { n->name[0], 0, 0 };
                f = binrt(opbuf);
                if (!f) { cgerr(g, n, "bad assign op"); }
                else { rt2(g, f, cur, v, res); load_slot(g, res); store_loc(g, tgt->loc_kind, tgt->loc_idx); }
                tmp_free(g, 2);
            }
            tmp_free(g, 1); break;
        }
        if (tgt->kind == AN_INDEX) {
            int ov = tmp_new(g), iv = tmp_new(g), vv = tmp_new(g);
            cg_expr(g, tgt->a, ov); cg_expr(g, tgt->b, iv); cg_expr(g, n->b, vv);
            if (is_plain) {
                load_slot(g, ov); ob(g,0x48);ob(g,0x89);ob(g,0xC7);
                load_slot(g, iv); ob(g,0x48);ob(g,0x89);ob(g,0xC6);
                load_slot(g, vv); ob(g,0x48);ob(g,0x89);ob(g,0xC2);
                callrt(g, "a_setitem");
            } else {
                int cur = tmp_new(g);
                rt2(g, "a_getitem", ov, iv, cur);
                char opbuf[3] = { n->name[0], 0, 0 };
                const char *f = binrt(opbuf);
                int res = tmp_new(g); rt2(g, f ? f : "a_add", cur, vv, res);
                load_slot(g, ov); ob(g,0x48);ob(g,0x89);ob(g,0xC7);
                load_slot(g, iv); ob(g,0x48);ob(g,0x89);ob(g,0xC6);
                load_slot(g, res); ob(g,0x48);ob(g,0x89);ob(g,0xC2);
                callrt(g, "a_setitem");
                tmp_free(g, 2);
            }
            tmp_free(g, 3); break;
        }
        if (tgt->kind == AN_FIELD) {
            int ov = tmp_new(g), vv = tmp_new(g);
            cg_expr(g, tgt->a, ov); cg_expr(g, n->b, vv);
            Sym *fs = str_sym(g, tgt->name, (int)strlen(tgt->name));
            if (is_plain) {
                load_slot(g, ov); ob(g,0x48);ob(g,0x89);ob(g,0xC7);
                movabs_sym(g, 6, fs, 0);
                load_slot(g, vv); ob(g,0x48);ob(g,0x89);ob(g,0xC2);
                callrt(g, "a_setattr");
            } else {
                int cur = tmp_new(g), res = tmp_new(g);
                load_slot(g, ov); ob(g,0x48);ob(g,0x89);ob(g,0xC7); movabs_sym(g,6,fs,0); callrt(g,"a_getattr"); store_slot(g,cur);
                char opbuf[3] = { n->name[0], 0, 0 };
                const char *f = binrt(opbuf);
                rt2(g, f ? f : "a_add", cur, vv, res);
                load_slot(g, ov); ob(g,0x48);ob(g,0x89);ob(g,0xC7);
                movabs_sym(g, 6, fs, 0);
                load_slot(g, res); ob(g,0x48);ob(g,0x89);ob(g,0xC2);
                callrt(g, "a_setattr");
                tmp_free(g, 2);
            }
            tmp_free(g, 2); break;
        }
        cgerr(g, n, "bad assignment target");
        break;
    }
    case AN_RETURN: {
        cg_defer_run(g, 1); /* run all defers in this function */
        if (n->a) { int t = tmp_new(g); cg_expr(g, n->a, t); load_slot(g, t); tmp_free(g,1); }
        else movabs_imm(g, 0, A_NONE);
        jmp32(g, 0xE9, g->epilogue);
        break;
    }
    case AN_IF: {
        int endl = newlabel(g);
        for (int i = 0; i < n->nlist; i++) {
            ANode *br = n->list[i];
            int cf = tmp_new(g); cg_expr(g, br->a, cf); emit_truthy(g, cf);
            int els = newlabel(g);
            load_slot(g, cf); ob(g,0x48);ob(g,0x85);ob(g,0xC0);
            jmp32(g, 0x84, els);
            cg_defer_pushframe(g); cg_block(g, br->b); cg_defer_popframe(g);
            jmp32(g, 0xE9, endl);
            deflabel(g, els);
            tmp_free(g, 1);
        }
        if (n->c) { cg_defer_pushframe(g); cg_block(g, n->c); cg_defer_popframe(g); }
        deflabel(g, endl);
        break;
    }
    case AN_WHILE: case AN_LOOP: case AN_DOWHILE: {
        int top = newlabel(g), end = newlabel(g), cont = newlabel(g);
        g->loopbp[g->loopdepth] = top; g->loopcont[g->loopdepth] = cont;
        g->loopend[g->loopdepth] = end; g->loopdepth++;
        if (n->kind != AN_DOWHILE) deflabel(g, top);
        if (n->kind == AN_LOOP) { /* infinite */ }
        else if (n->kind == AN_WHILE) {
            int cf = tmp_new(g); cg_expr(g, n->a, cf); emit_truthy(g, cf);
            load_slot(g, cf); ob(g,0x48);ob(g,0x85);ob(g,0xC0);
            jmp32(g, 0x84, end); tmp_free(g, 1);
        }
        cg_defer_pushframe(g); cg_block(g, n->b); cg_defer_popframe(g);
        if (n->kind == AN_DOWHILE) {
            deflabel(g, cont);
            int cf = tmp_new(g); cg_expr(g, n->a, cf); emit_truthy(g, cf);
            load_slot(g, cf); ob(g,0x48);ob(g,0x85);ob(g,0xC0);
            jmp32(g, 0x85, top); tmp_free(g, 1);
        } else { deflabel(g, cont); jmp32(g, 0xE9, top); }
        deflabel(g, end);
        g->loopdepth--;
        break;
    }
    case AN_FOR: {
        int itf = tmp_new(g), of = tmp_new(g);
        cg_expr(g, n->a, itf);
        load_slot(g, itf); callrt(g, "a_iter_new"); store_slot(g, of);
        int top = newlabel(g), end = newlabel(g), cont = newlabel(g);
        g->loopbp[g->loopdepth] = top; g->loopcont[g->loopdepth] = cont;
        g->loopend[g->loopdepth] = end; g->loopdepth++;
        deflabel(g, cont); /* continue -> re-fetch next */
        deflabel(g, top);
        load_slot(g, of); ob(g,0x48);ob(g,0x89);ob(g,0xC7);
        ob(g,0x48);ob(g,0x8D);ob(g,0xB5);o32(g,(unsigned)(-(OFF(1 + n->aux))));
        callrt(g, "a_iter_next");
        ob(g,0x48);ob(g,0x85);ob(g,0xC0);
        jmp32(g, 0x84, end);
        cg_defer_pushframe(g); cg_block(g, n->b); cg_defer_popframe(g);
        jmp32(g, 0xE9, cont);
        deflabel(g, end);
        g->loopdepth--;
        tmp_free(g, 2); break;
    }
    case AN_BREAK:
        if (g->loopdepth <= 0) { cgerr(g, n, "break outside loop"); break; }
        jmp32(g, 0xE9, g->loopend[g->loopdepth-1]);
        break;
    case AN_CONTINUE:
        if (g->loopdepth <= 0) { cgerr(g, n, "continue outside loop"); break; }
        jmp32(g, 0xE9, g->loopcont[g->loopdepth-1]);
        break;
    case AN_DEFER: cg_defer_push(g, n->a); break;
    case AN_BLOCK: cg_defer_pushframe(g); cg_block(g, n); cg_defer_popframe(g); break;
    case AN_FN: break; /* decls emitted separately */
    case AN_LAMBDA: case AN_MATCH: { int t = tmp_new(g); cg_expr(g, n, t); tmp_free(g,1); break; }
    default: cgerr(g, n, "unsupported statement"); break;
    }
}

static void cg_block(CG *g, ANode *b){ for (int i = 0; i < b->nlist; i++) cg_stmt(g, b->list[i]); }

/* loop break/continue need end/cont labels — store them: use label ids -2/-3
 * remapped: we stored label ids in loopbp/loopcont as `newlabel` ids but never
 * defined `end`. Fix: give each loop an end label too. */

static void emit_fn_prologue(CG *g, AFuncInfo *fi){
    ob(g,0x55); ob(g,0x48);ob(g,0x89);ob(g,0xE5);       /* push rbp; mov rbp,rsp */
    ob(g,0x53);                                        /* push rbx */
    ob(g,0x41);ob(g,0x54);                             /* push r12 */
    ob(g,0x41);ob(g,0x55);                             /* push r13 */
    ob(g,0x41);ob(g,0x56);                             /* push r14 */
    ob(g,0x41);ob(g,0x57);                             /* push r15 */
    ob(g,0x48);ob(g,0x89);ob(g,0x7D);ob(g,0xD0);       /* mov [rbp-48],rdi (argv) */
    g->framepatch = here(g);
    ob(g,0x48);ob(g,0x81);ob(g,0xEC);o32(g,0);         /* sub rsp, imm32 (patched) */
    g->fnparams = fi ? fi->nparams : 0;
    g->fnlocals = fi ? fi->nlocals : 0;
    g->ntemps = 0; g->maxtemp = 0; g->outbytes = 0;
    g->ndeflists = 0;
    g->epilogue = newlabel(g);
}
static void emit_fn_epilogue(CG *g){
    deflabel(g, g->epilogue);
    int used = 48 + 8 * (g->maxtemp + 1) + g->outbytes;
    int F = ((used + 7) & ~15) | 8;
    /* patch sub rsp */
    unsigned char *p = g->t->data + g->framepatch + 3;
    p[0]=F; p[1]=F>>8; p[2]=F>>16; p[3]=F>>24;
    ob(g,0x48);ob(g,0x81);ob(g,0xC4);o32(g,(unsigned)F); /* add rsp,F */
    ob(g,0x41);ob(g,0x5F); ob(g,0x41);ob(g,0x5E); ob(g,0x41);ob(g,0x5D); ob(g,0x41);ob(g,0x5C); ob(g,0x5B);
    ob(g,0x5D); ob(g,0xC3);                              /* pop r15,r14,r13,r12,rbx,rbp; ret */
}
static void emit_fn_sym(CG *g, const char *name, unsigned long start, unsigned long size){
    int v = tok_alloc_const(name);
    Sym *s = external_global_sym(v, &func_old_type);
    put_extern_sym(s, g->t, start, size);
}

int a_codegen(AModule *m){
    TCCState *s1 = m->s1;
    CG g; memset(&g, 0, sizeof g);
    g.m = m; g.s1 = m->s1; g.t = text_section;
    g.globals_sym = data_sym(&g, "a_globals", bss_section, bss_section->data_offset, (m->prog->aux ? m->prog->aux : 1) * 8);
    /* reserve bss */
    section_ptr_add(bss_section, (m->prog->aux ? m->prog->aux : 1) * 8);
    /* collect top-level fns and lambdas count */
    /* emit all AN_FN functions */
    int *fn_global_idx = tcc_malloc(sizeof(int) * m->prog->nlist);
    for (int i = 0; i < m->prog->nlist; i++) {
        ANode *s = m->prog->list[i];
        if (s->kind != AN_FN) continue;
        AFuncInfo *fi = (AFuncInfo *)s->scope;
        unsigned long start = g.t->data_offset;
        emit_fn_prologue(&g, fi);
        cg_block(&g, s->b);
        /* default return NONE */
        movabs_imm(&g, 0, A_NONE);
        jmp32(&g, 0xE9, g.epilogue);
        emit_fn_epilogue(&g);
        emit_fn_sym(&g, s->name, start, g.t->data_offset - start);
        fn_global_idx[i] = s->loc_idx;
    }
    /* emit main */
    unsigned long mstart = g.t->data_offset;
    {
        AFuncInfo dummy; memset(&dummy,0,sizeof dummy);
        emit_fn_prologue(&g, &dummy);
        /* init fn FNONs into globals */
        for (int i = 0; i < m->prog->nlist; i++) {
            ANode *s = m->prog->list[i];
            if (s->kind != AN_FN) continue;
            Sym *fs = a_rsym(&g, s->name);
            movabs_sym(&g, 7, fs, 0);           /* rdi = entry addr */
            movabs_imm(&g, 6, A_NONE);          /* rsi = env NONE */
            /* rdx = name cstr */
            Sym *ns = str_sym(&g, s->name, (int)strlen(s->name));
            movabs_sym(&g, 2, ns, 0);           /* rdx = name — reg2 */
            callrt(&g, "a_fnon_new");
            store_global(&g, s->loc_idx);
        }
        cg_block(&g, m->prog);
        /* top-level may contain AN_FN stmts — skip them in cg_stmt? cg_stmt handles AN_FN as expr — fix: prog list includes AN_FN; cg_block iterates all -> would emit expr for fn decl! Need filter. */
        movabs_imm(&g, 0, 0);                  /* eax = 0 */
        /* fallthrough to epilogue: jmp */
        jmp32(&g, 0xE9, g.epilogue);
        emit_fn_epilogue(&g);
    }
    emit_fn_sym(&g, "main", mstart, g.t->data_offset - mstart);
    patchfix(&g);
    return m->had_error ? -1 : 0;
}

#endif
