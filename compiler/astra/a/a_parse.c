/* A -> AST parser. Recursive descent mirroring compiler/stages/parser.
 * Token stream comes from a_lex (AT_NL/AT_INDENT/AT_DEDENT significant). */
#include "a_internal.h"
#include <stdarg.h>
#include <ctype.h>

void a_perror(AModule *m, int line, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    TCCState *s1 = m->s1;
    tcc_error_noabort("%s:%d: %s", m->filename ? m->filename : "<a>", line, buf);
    m->had_error = 1; m->nerrors++;
}

static AToken *pk(AModule *m) { return &m->toks[m->itok < m->ntok ? m->itok : m->ntok - 1]; }
static AToken *pk2(AModule *m) { int i = m->itok + 1 < m->ntok ? m->itok + 1 : m->ntok - 1; return &m->toks[i]; }
static AToken *nx(AModule *m) { AToken *t = pk(m); if (m->itok < m->ntok) m->itok++; return t; }
static int eat(AModule *m, int k) { if (pk(m)->kind == k) { nx(m); return 1; } return 0; }
static void skipnl(AModule *m) { while (pk(m)->kind == AT_NL) nx(m); }

static ANode *parse_expr(AModule *m);
static ANode *parse_stmt(AModule *m);

/* ---------- string escapes ---------- */
static char *a_decode_escapes(const char *raw, int len) {
    char *out = (char *)tcc_malloc(len * 4 + 8);
    int oi = 0;
    for (int i = 0; i < len; i++) {
        char c = raw[i];
        if (c != '\\') { out[oi++] = c; continue; }
        if (i + 1 >= len) { out[oi++] = '\\'; break; }
        char e = raw[++i];
        switch (e) {
        case 'n': out[oi++] = '\n'; break;
        case 't': out[oi++] = '\t'; break;
        case 'r': out[oi++] = '\r'; break;
        case 'a': out[oi++] = '\a'; break;
        case 'b': out[oi++] = '\b'; break;
        case 'f': out[oi++] = '\f'; break;
        case 'v': out[oi++] = '\v'; break;
        case '0': out[oi++] = '\0'; break;
        case '\\': out[oi++] = '\\'; break;
        case '"': out[oi++] = '"'; break;
        case '\'': out[oi++] = '\''; break;
        case '{': out[oi++] = '{'; break;
        case '}': out[oi++] = '}'; break;
        case 'x': {
            unsigned v = 0; int d = 0;
            while (d < 2 && i + 1 < len && isxdigit((unsigned char)raw[i+1])) {
                char h = raw[++i];
                v = v * 16 + (isdigit((unsigned char)h) ? h - '0' : tolower((unsigned char)h) - 'a' + 10);
                d++;
            }
            out[oi++] = (char)v; break;
        }
        case 'u': {
            unsigned v = 0; int d = 0;
            if (i + 1 < len && raw[i+1] == '{') {
                i++;
                while (i + 1 < len && isxdigit((unsigned char)raw[i+1])) {
                    char h = raw[++i];
                    v = v * 16 + (isdigit((unsigned char)h) ? h - '0' : tolower((unsigned char)h) - 'a' + 10);
                    d++;
                }
                if (i + 1 < len && raw[i+1] == '}') i++;
            }
            if (d == 0 || v > 0x10FFFF) { out[oi++] = '?'; break; }
            if (v < 0x80) out[oi++] = (char)v;
            else if (v < 0x800) { out[oi++] = (char)(0xC0|(v>>6)); out[oi++] = (char)(0x80|(v&0x3F)); }
            else if (v < 0x10000) { out[oi++] = (char)(0xE0|(v>>12)); out[oi++] = (char)(0x80|((v>>6)&0x3F)); out[oi++] = (char)(0x80|(v&0x3F)); }
            else { out[oi++] = (char)(0xF0|(v>>18)); out[oi++] = (char)(0x80|((v>>12)&0x3F)); out[oi++] = (char)(0x80|((v>>6)&0x3F)); out[oi++] = (char)(0x80|(v&0x3F)); }
            break;
        }
        default: out[oi++] = e; break;
        }
    }
    out[oi] = 0;
    return out;
}

/* parse an expression held inside {..} interpolation; swaps token stream */
static ANode *a_parse_expr_text(AModule *m, const char *text, int len) {
    AModule tm; memset(&tm, 0, sizeof tm);
    tm.s1 = m->s1; tm.filename = m->filename; tm.src = text; tm.len = len;
    if (a_lex(&tm) < 0) { a_perror(m, 1, "bad interpolation expression"); return an_new(AN_INT, 1); }
    AToken *sv_toks = m->toks; int sv_ntok = m->ntok, sv_itok = m->itok;
    m->toks = tm.toks; m->ntok = tm.ntok; m->itok = 0;
    skipnl(m);
    ANode *e = parse_expr(m);
    skipnl(m);
    if (pk(m)->kind != AT_EOF) a_perror(m, pk(m)->line, "unexpected token in interpolation");
    m->toks = sv_toks; m->ntok = sv_ntok; m->itok = sv_itok;
    return e;
}

/* split string token payload into AN_STR list of AN_STRRAW / expr parts */
static ANode *a_str_node(AModule *m, AToken *t) {
    ANode *s = an_new(AN_STR, t->line);
    const char *raw = t->sval ? t->sval : "";
    int len = t->len;
    if (t->raw) {
        if (len) { ANode *p = an_new(AN_STRRAW, t->line); p->sval = a_strndup(raw, len); a_node_list_add(s, p); }
        return s;
    }
    char *lit = (char *)tcc_malloc(len + 1);
    int li = 0;
    for (int i = 0; i < len;) {
        char c = raw[i];
        if (c == '\\') {
            if (i + 1 < len && raw[i+1] == '{') { lit[li++] = '{'; i += 2; continue; }
            if (i + 1 < len) { lit[li++] = c; lit[li++] = raw[i+1]; i += 2; continue; }
            lit[li++] = c; i++; continue;
        }
        if (c != '{') { lit[li++] = c; i++; continue; }
        int depth = 1, instr = 0, j = i + 1;
        while (j < len && depth > 0) {
            char d = raw[j];
            if (instr) { if (d == '\\') j++; else if (d == '"' || d == '\'') instr = 0; }
            else if (d == '"' || d == '\'') instr = 1;
            else if (d == '{') depth++;
            else if (d == '}') depth--;
            j++;
        }
        if (depth != 0) { a_perror(m, t->line, "unterminated '{' in string interpolation"); break; }
        if (li) {
            ANode *p = an_new(AN_STRRAW, t->line);
            p->sval = a_decode_escapes(lit, li);
            a_node_list_add(s, p); li = 0;
        }
        a_node_list_add(s, a_parse_expr_text(m, raw + i + 1, j - i - 2));
        i = j;
    }
    if (li || s->nlist == 0) {
        ANode *p = an_new(AN_STRRAW, t->line);
        p->sval = a_decode_escapes(lit, li);
        a_node_list_add(s, p);
    }
    tcc_free(lit);
    return s;
}

/* ---------- skipped type expressions ---------- */
static void skip_type(AModule *m, const int *terms, int nterms) {
    int depth = 0;
    while (pk(m)->kind != AT_EOF && pk(m)->kind != AT_NL) {
        int k = pk(m)->kind;
        if (depth == 0) {
            for (int i = 0; i < nterms; i++) if (k == terms[i]) return;
        }
        if (k == AT_LP || k == AT_LB || k == AT_LC || k == AT_LT) depth++;
        else if (k == AT_RP || k == AT_RB || k == AT_RC || k == AT_GT) { if (depth > 0) depth--; }
        nx(m);
    }
}

static ANode *parse_param(AModule *m) {
    AToken *t = pk(m);
    if (t->kind != AT_ID && t->kind != AT_KW_SELF) {
        a_perror(m, t->line, "expected parameter name"); nx(m);
        return an_new(AN_PARAM, t->line);
    }
    nx(m);
    ANode *p = an_new(AN_PARAM, t->line);
    p->name = a_strndup(t->p, t->len);
    if (eat(m, AT_COLON)) { static const int trm[] = { AT_COMMA, AT_RP }; skip_type(m, trm, 2); }
    return p;
}

static ANode *parse_fn_header(AModule *m, int is_lambda) {
    ANode *fn = an_new(is_lambda ? AN_LAMBDA : AN_FN, pk(m)->line);
    if (!is_lambda) {
        AToken *n = pk(m);
        if (n->kind == AT_ID) { nx(m); fn->name = a_strndup(n->p, n->len); }
        else a_perror(m, n->line, "expected function name");
        if (pk(m)->kind == AT_LT) { /* generics <T,..> */
            int d = 0;
            do { int k = nx(m)->kind; if (k == AT_LT) d++; else if (k == AT_GT) d--; }
            while (d > 0 && pk(m)->kind != AT_EOF);
        }
    }
    eat(m, AT_LP);
    if (!eat(m, AT_RP)) {
        for (;;) {
            a_node_list_add(fn, parse_param(m));
            if (!eat(m, AT_COMMA)) break;
            if (pk(m)->kind == AT_RP) break;
        }
        eat(m, AT_RP);
    }
    if (eat(m, AT_ARROW)) { static const int trm[] = { AT_COLON, AT_FATARROW, AT_NL, AT_KW_WHERE }; skip_type(m, trm, 4); }
    if (eat(m, AT_KW_WHERE)) { static const int trm[] = { AT_COLON, AT_FATARROW, AT_NL }; skip_type(m, trm, 3); }
    return fn;
}

/* ---------- expressions ---------- */
static int binprec(int k) {
    switch (k) {
    case AT_PIPEOP: return 1;
    case AT_QUESTION: return 2;
    case AT_QUESTIONQUESTION: return 3;
    case AT_KW_OR: return 4;
    case AT_KW_AND: return 5;
    case AT_KW_IN: case AT_EQEQ: case AT_NE: case AT_LT: case AT_GT: case AT_LE: case AT_GE: return 7;
    case AT_RANGE: return 8;
    case AT_PIPE: return 9;
    case AT_CARET: return 10;
    case AT_AMP: return 11;
    case AT_SHL: case AT_SHR: return 12;
    case AT_PLUS: case AT_MINUS: return 13;
    case AT_STAR: case AT_SLASH: case AT_PERCENT: return 14;
    case AT_POW: return 16;
    default: return -1;
    }
}
static const char *opname(int k) {
    switch (k) {
    case AT_PLUS: return "+"; case AT_MINUS: return "-"; case AT_STAR: return "*";
    case AT_SLASH: return "/"; case AT_PERCENT: return "%"; case AT_POW: return "**";
    case AT_EQEQ: return "=="; case AT_NE: return "!="; case AT_LT: return "<";
    case AT_GT: return ">"; case AT_LE: return "<="; case AT_GE: return ">=";
    case AT_AMP: return "&"; case AT_PIPE: return "|"; case AT_CARET: return "^";
    case AT_SHL: return "<<"; case AT_SHR: return ">>";
    case AT_KW_AND: return "and"; case AT_KW_OR: return "or";
    case AT_KW_IN: return "in"; case AT_QUESTIONQUESTION: return "??";
    case AT_RANGE: return ".."; case AT_PIPEOP: return "|>";
    default: return "?";
    }
}

static int q_is_ternary(AModule *m) {
    int i = m->itok + 1, depth = 0;
    while (i < m->ntok) {
        int k = m->toks[i].kind;
        if (k == AT_LP || k == AT_LB || k == AT_LC) depth++;
        else if (k == AT_RP || k == AT_RB || k == AT_RC) { if (depth == 0) break; depth--; }
        else if (depth == 0 && k == AT_COLON) return 1;
        else if (depth == 0 && (k == AT_COMMA || k == AT_NL || k == AT_EOF || k == AT_RC || k == AT_DEDENT)) break;
        i++;
    }
    return 0;
}

static ANode *parse_postfix(AModule *m);
static ANode *parse_prefix(AModule *m);
static ANode *parse_primary(AModule *m);

static ANode *parse_expr_bp(AModule *m, int minprec) {
    ANode *left = parse_prefix(m);
    for (;;) {
        if (pk(m)->kind == AT_KW_NOT && pk2(m)->kind == AT_KW_IN) {
            nx(m); nx(m);
            ANode *r = parse_expr_bp(m, 8);
            ANode *n = an_new(AN_BIN, left->line); n->name = a_strdup("not in");
            n->a = left; n->b = r; left = n; continue;
        }
        int k = pk(m)->kind, prec = binprec(k);
        if (prec < 0 || prec < minprec) break;
        AToken *op = nx(m);
        if (k == AT_QUESTION) {
            ANode *t = an_new(AN_TERNARY, op->line);
            t->a = left; t->b = parse_expr_bp(m, 0);
            if (!eat(m, AT_COLON)) a_perror(m, op->line, "expected ':' in conditional expression");
            t->c = parse_expr_bp(m, prec);
            left = t; continue;
        }
        if (k == AT_RANGE) {
            if (left->kind == AN_RANGE && !left->next) {
                /* second '..' introduces the step: a..b..c */
                left->next = parse_expr_bp(m, prec + 1);
                continue;
            }
            ANode *r = an_new(AN_RANGE, op->line);
            r->a = left; r->b = parse_expr_bp(m, prec + 1);
            left = r;
            continue;
        }
        int nextmin = (k == AT_POW) ? prec : prec + 1;
        ANode *right = parse_expr_bp(m, nextmin);
        if (k == AT_PIPEOP) {
            if (right->kind == AN_CALL && right->a && right->a->kind == AN_IDENT) {
                ANode **nl = (ANode **)tcc_realloc(right->list, (right->nlist + 1) * sizeof(ANode *));
                memmove(nl + 1, nl, right->nlist * sizeof(ANode *));
                nl[0] = left; right->list = nl; right->nlist++;
                left = right; continue;
            }
            a_perror(m, op->line, "right side of '|>' must be a call");
            left = right; continue;
        }
        ANode *n = an_new(AN_BIN, op->line);
        n->name = a_strdup(opname(k)); n->a = left; n->b = right; left = n;
    }
    return left;
}

static ANode *parse_expr(AModule *m) { return parse_expr_bp(m, 0); }

static ANode *parse_prefix(AModule *m) {
    AToken *t = pk(m);
    if (t->kind == AT_KW_NOT) {
        nx(m);
        ANode *n = an_new(AN_UNARY, t->line); n->name = a_strdup("not");
        n->a = parse_expr_bp(m, 6); return n;
    }
    if (t->kind == AT_PLUSPLUS || t->kind == AT_MINUSMINUS) {
        nx(m);
        ANode *n = an_new(AN_UNARY, t->line);
        n->name = a_strdup(t->kind == AT_PLUSPLUS ? "++" : "--");
        n->a = parse_expr_bp(m, 15); return n;
    }
    if (t->kind == AT_MINUS || t->kind == AT_TILDE) {
        nx(m);
        ANode *n = an_new(AN_UNARY, t->line);
        n->name = a_strdup(t->kind == AT_MINUS ? "-" : "~");
        n->a = parse_expr_bp(m, 15); return n;
    }
    return parse_postfix(m);
}

static void parse_args(AModule *m, ANode *call) {
    eat(m, AT_LP);
    if (eat(m, AT_RP)) return;
    for (;;) {
        if (pk(m)->kind == AT_ID && pk2(m)->kind == AT_EQ) {
            AToken *id = nx(m); nx(m);
            ANode *p = an_new(AN_PAIR, id->line);
            p->name = a_strndup(id->p, id->len);
            p->a = parse_expr(m);
            a_node_list_add(call, p);
        } else {
            a_node_list_add(call, parse_expr(m));
        }
        if (!eat(m, AT_COMMA)) break;
        if (pk(m)->kind == AT_RP) break;
    }
    eat(m, AT_RP);
}

static ANode *parse_postfix(AModule *m) {
    ANode *e = parse_primary(m);
    for (;;) {
        AToken *t = pk(m);
        if (t->kind == AT_PLUSPLUS || t->kind == AT_MINUSMINUS) {
            nx(m);
            ANode *n = an_new(AN_UNARY, t->line);
            n->name = a_strdup(t->kind == AT_PLUSPLUS ? "++" : "--");
            n->aux = 1; n->a = e; e = n; continue;
        }
        if (t->kind == AT_DOT || t->kind == AT_QUESTIONDOT) {
            int safe = t->kind == AT_QUESTIONDOT;
            nx(m);
            AToken *f = pk(m);
            if (f->kind != AT_ID) { a_perror(m, f->line, "expected field or method name"); break; }
            nx(m);
            ANode *n = an_new(AN_FIELD, f->line);
            n->a = e; n->name = a_strndup(f->p, f->len); n->aux = safe;
            e = n; continue;
        }
        if (t->kind == AT_BANG) {
            nx(m);
            ANode *n = an_new(AN_UNWRAP, t->line); n->a = e; e = n; continue;
        }
        if (t->kind == AT_QUESTION) {
            if (q_is_ternary(m)) break;
            nx(m);
            ANode *n = an_new(AN_TRY, t->line); n->a = e; e = n; continue;
        }
        if (t->kind == AT_LP) {
            ANode *c = an_new(AN_CALL, t->line);
            c->a = e; c->list = NULL; c->nlist = 0;
            parse_args(m, c);
            e = c; continue;
        }
        if (t->kind == AT_LB) {
            nx(m);
            if (eat(m, AT_RANGE)) {
                ANode *s = an_new(AN_SLICE, t->line);
                s->a = e; s->b = NULL;
                s->c = pk(m)->kind == AT_RB ? NULL : parse_expr_bp(m, 13);
                if (eat(m, AT_RANGE)) s->next = parse_expr_bp(m, 13);
                eat(m, AT_RB);
                e = s; continue;
            }
            ANode *first = parse_expr_bp(m, 13);
            if (pk(m)->kind == AT_RANGE) {
                nx(m);
                ANode *s = an_new(AN_SLICE, t->line);
                s->a = e; s->b = first;
                s->c = pk(m)->kind == AT_RB ? NULL : parse_expr_bp(m, 13);
                if (pk(m)->kind == AT_RANGE) { nx(m); s->next = parse_expr_bp(m, 13); }
                eat(m, AT_RB);
                e = s; continue;
            }
            eat(m, AT_RB);
            ANode *n = an_new(AN_INDEX, t->line); n->a = e; n->b = first; e = n; continue;
        }
        break;
    }
    return e;
}

static void parse_comp_clauses(AModule *m, ANode *comp) {
    for (;;) {
        if (pk(m)->kind == AT_KW_FOR) {
            nx(m);
            AToken *v = pk(m);
            if (v->kind == AT_ID) nx(m); else a_perror(m, v->line, "expected loop variable name");
            if (!eat(m, AT_KW_IN)) a_perror(m, v->line, "expected 'in'");
            ANode *cl = an_new(AN_CLAUSE, v->line);
            cl->name = a_strdup(v->kind == AT_ID ? v->p : "_");
            cl->a = parse_expr(m);
            a_node_list_add(comp, cl);
        } else if (pk(m)->kind == AT_KW_IF) {
            nx(m);
            if (comp->nlist == 0) a_perror(m, pk(m)->line, "'if' without 'for' in comprehension");
            else {
                ANode *last = comp->list[comp->nlist - 1];
                if (last->b) {
                    ANode *cc = an_new(AN_BIN, pk(m)->line); cc->name = a_strdup("and");
                    cc->a = last->b; cc->b = parse_expr(m); last->b = cc;
                } else last->b = parse_expr(m);
            }
        } else break;
    }
}

static ANode *parse_lambda(AModule *m) {
    AToken *ft = nx(m);
    ANode *fn = parse_fn_header(m, 1);
    fn->line = ft->line;
    if (!eat(m, AT_FATARROW)) a_perror(m, pk(m)->line, "expected '=>' in lambda");
    fn->a = parse_expr(m);
    return fn;
}

/* ---------- match patterns ---------- */
static ANode *parse_pattern(AModule *m);

static ANode *parse_pattern(AModule *m) {
    ANode *pat = an_new(AN_PAT, pk(m)->line);
    AToken *pt = pk(m);
    if (pt->kind == AT_ID && pt->len == 1 && pt->p[0] == '_') { nx(m); pat->aux = P_WILD; return pat; }
    if (pt->kind == AT_LP) {
        nx(m); pat->aux = P_TUPLE;
        if (!eat(m, AT_RP)) {
            for (;;) {
                a_node_list_add(pat, parse_pattern(m));
                if (!eat(m, AT_COMMA)) break;
                if (pk(m)->kind == AT_RP) break;
            }
            eat(m, AT_RP);
        }
        return pat;
    }
    if (pt->kind == AT_ID) {
        AToken *id = nx(m);
        if (pk(m)->kind == AT_LP) {
            /* variant ctor call — keep as literal call expr (runtime will reject) */
            pat->aux = P_LIT;
            ANode *c = an_new(AN_CALL, id->line);
            c->a = an_new(AN_IDENT, id->line); c->a->name = a_strndup(id->p, id->len);
            parse_args(m, c);
            pat->a = c;
            return pat;
        }
        /* dotted chain? treat as literal field expr */
        if (pk(m)->kind == AT_DOT) {
            ANode *f = an_new(AN_FIELD, id->line);
            f->a = an_new(AN_IDENT, id->line); f->a->name = a_strndup(id->p, id->len);
            while (eat(m, AT_DOT)) {
                AToken *d = pk(m); if (d->kind != AT_ID) break; nx(m);
                ANode *nf = an_new(AN_FIELD, d->line); nf->a = f; nf->name = a_strndup(d->p, d->len); f = nf;
            }
            if (pk(m)->kind == AT_RANGE) { nx(m); pat->aux = P_RANGE; pat->a = f; pat->b = parse_expr_bp(m, 10); }
            else { pat->aux = P_LIT; pat->a = f; }
            return pat;
        }
        if (pk(m)->kind == AT_RANGE) {
            nx(m); pat->aux = P_RANGE;
            pat->a = an_new(AN_IDENT, id->line); pat->a->name = a_strndup(id->p, id->len);
            pat->b = parse_expr_bp(m, 10);
            return pat;
        }
        pat->aux = P_BIND; pat->name = a_strndup(id->p, id->len);
        return pat;
    }
    /* literal expression (int/float/str/bool/None/-num), maybe range */
    ANode *e = parse_expr_bp(m, 10);
    if (pk(m)->kind == AT_RANGE) { nx(m); pat->aux = P_RANGE; pat->a = e; pat->b = parse_expr_bp(m, 10); }
    else { pat->aux = P_LIT; pat->a = e; }
    return pat;
}

static ANode *parse_match_expr(AModule *m) {
    AToken *mt = nx(m);
    ANode *me = an_new(AN_MATCH, mt->line);
    me->a = parse_expr(m);
    if (!eat(m, AT_COLON)) a_perror(m, mt->line, "expected ':' after match subject");
    skipnl(m);
    if (pk(m)->kind == AT_INDENT) nx(m);
    skipnl(m);
    while (pk(m)->kind != AT_EOF && pk(m)->kind != AT_DEDENT && pk(m)->kind != AT_RC) {
        if (pk(m)->kind == AT_NL) { nx(m); continue; }
        int nk = pk(m)->kind;
        if (nk != AT_ID && nk != AT_INT && nk != AT_FLOAT && nk != AT_STR && nk != AT_CHAR &&
            nk != AT_KW_TRUE && nk != AT_KW_FALSE && nk != AT_KW_NONE && nk != AT_LP &&
            nk != AT_MINUS && nk != AT_TILDE) break;
        ANode *arm = an_new(AN_ARM, pk(m)->line);
        arm->a = parse_pattern(m);
        if (eat(m, AT_KW_IF)) arm->b = parse_expr(m);
        if (!eat(m, AT_FATARROW)) a_perror(m, arm->line, "expected '=>' after match pattern");
        arm->c = parse_expr(m);
        a_node_list_add(me, arm);
        skipnl(m);
        if (pk(m)->kind == AT_DEDENT) { nx(m); break; }
    }
    if (pk(m)->kind == AT_DEDENT) nx(m);
    if (me->nlist == 0) a_perror(m, mt->line, "match needs at least one arm");
    return me;
}

static ANode *parse_primary(AModule *m) {
    AToken *t = pk(m);
    switch (t->kind) {
    case AT_INT: nx(m); { ANode *n = an_new(AN_INT, t->line); n->ival = t->ival; return n; }
    case AT_FLOAT: nx(m); { ANode *n = an_new(AN_FLOAT, t->line); n->dval = t->dval; return n; }
    case AT_STR: nx(m); return a_str_node(m, t);
    case AT_CHAR: nx(m); {
        ANode *n = an_new(AN_STR, t->line);
        ANode *p = an_new(AN_STRRAW, t->line);
        p->sval = a_decode_escapes(t->sval ? t->sval : "", t->len);
        a_node_list_add(n, p); return n;
    }
    case AT_KW_TRUE: case AT_KW_FALSE: nx(m); { ANode *n = an_new(AN_BOOL, t->line); n->ival = (t->kind == AT_KW_TRUE); return n; }
    case AT_KW_NONE: nx(m); return an_new(AN_NONE, t->line);
    case AT_KW_SELF: nx(m); { ANode *n = an_new(AN_IDENT, t->line); n->name = a_strdup("self"); return n; }
    case AT_ID: nx(m); { ANode *n = an_new(AN_IDENT, t->line); n->name = a_strndup(t->p, t->len); return n; }
    case AT_KW_FN: return parse_lambda(m);
    case AT_KW_MATCH: return parse_match_expr(m);
    case AT_LP: {
        nx(m);
        if (eat(m, AT_RP)) { a_perror(m, t->line, "empty tuples not supported"); return an_new(AN_TUPLE, t->line); }
        ANode *first = parse_expr(m);
        if (!eat(m, AT_COMMA)) { eat(m, AT_RP); return first; }
        ANode *tp = an_new(AN_TUPLE, t->line);
        a_node_list_add(tp, first);
        while (!eat(m, AT_RP)) {
            a_node_list_add(tp, parse_expr(m));
            if (!eat(m, AT_COMMA)) { eat(m, AT_RP); break; }
        }
        return tp;
    }
    case AT_LB: {
        nx(m);
        if (eat(m, AT_RB)) return an_new(AN_LIST, t->line);
        ANode *first = parse_expr(m);
        if (pk(m)->kind == AT_KW_FOR) {
            ANode *c = an_new(AN_COMP, t->line); c->aux = C_LIST; c->a = first;
            parse_comp_clauses(m, c);
            eat(m, AT_RB);
            ANode *l = an_new(AN_LIST, t->line); l->a = c; return l;
        }
        ANode *l = an_new(AN_LIST, t->line);
        a_node_list_add(l, first);
        while (eat(m, AT_COMMA)) { if (pk(m)->kind == AT_RB) break; a_node_list_add(l, parse_expr(m)); }
        eat(m, AT_RB);
        return l;
    }
    case AT_LC: {
        nx(m);
        if (eat(m, AT_RC)) return an_new(AN_DICT, t->line);
        ANode *firstKey = parse_expr(m);
        if (pk(m)->kind == AT_KW_FOR) {           /* set comprehension {e for ...} */
            ANode *c = an_new(AN_COMP, t->line); c->aux = C_SET; c->a = firstKey;
            parse_comp_clauses(m, c);
            eat(m, AT_RC);
            ANode *s = an_new(AN_SET, t->line); s->a = c; return s;
        }
        if (pk(m)->kind == AT_COLON) {           /* dict literal or dict comprehension */
            nx(m);
            ANode *firstVal = parse_expr(m);
            if (pk(m)->kind == AT_KW_FOR) {       /* {k: v for ...} */
                ANode *c = an_new(AN_COMP, t->line); c->aux = C_DICT;
                c->a = firstKey; c->b = firstVal;
                parse_comp_clauses(m, c);
                eat(m, AT_RC);
                ANode *d = an_new(AN_DICT, t->line); d->a = c; return d;
            }
            ANode *d = an_new(AN_DICT, t->line);
            ANode *pr = an_new(AN_PAIR, t->line); pr->a = firstKey; pr->b = firstVal;
            a_node_list_add(d, pr);
            while (eat(m, AT_COMMA)) {
                if (pk(m)->kind == AT_RC) break;
                ANode *k = parse_expr(m);
                if (!eat(m, AT_COLON)) { a_perror(m, t->line, "expected ':' after map key"); break; }
                ANode *v = parse_expr(m);
                ANode *p2 = an_new(AN_PAIR, t->line); p2->a = k; p2->b = v;
                a_node_list_add(d, p2);
            }
            eat(m, AT_RC);
            return d;
        }
        /* set literal {a, b, c} */
        ANode *s = an_new(AN_SET, t->line);
        a_node_list_add(s, firstKey);
        while (eat(m, AT_COMMA)) { if (pk(m)->kind == AT_RC) break; a_node_list_add(s, parse_expr(m)); }
        eat(m, AT_RC);
        return s;
    }
    default:
        a_perror(m, t->line, "expected expression");
        nx(m);
        return an_new(AN_INT, t->line);
    }
}

/* ---------- statements ---------- */
static ANode *parse_block(AModule *m, int header_col) {
    ANode *b = an_new(AN_BLOCK, pk(m)->line);
    if (eat(m, AT_LC)) {           /* { ... } brace suite */
        skipnl(m);
        while (!eat(m, AT_RC) && pk(m)->kind != AT_EOF) {
            int before = m->itok;
            ANode *s = parse_stmt(m);
            if (s) a_node_list_add(b, s);
            skipnl(m);
            if (m->itok == before) break;
        }
        return b;
    }
    if (eat(m, AT_COLON)) {
        if (pk(m)->kind != AT_NL && pk(m)->kind != AT_EOF) {
            ANode *s = parse_stmt(m);           /* inline suite: if x: stmt */
            if (s) a_node_list_add(b, s);
            return b;
        }
        skipnl(m);
        if (pk(m)->kind == AT_INDENT) nx(m);
        skipnl(m);
        while (pk(m)->kind != AT_EOF && pk(m)->kind != AT_KW_ELIF && pk(m)->kind != AT_KW_ELSE &&
               pk(m)->kind != AT_RC && pk(m)->kind != AT_DEDENT) {
            AToken *t = pk(m);
            if (!t->bol || t->col <= header_col) break;
            int before = m->itok;
            ANode *s = parse_stmt(m);
            if (s) a_node_list_add(b, s);
            skipnl(m);
            if (m->itok == before) break;
        }
        if (pk(m)->kind == AT_DEDENT) nx(m);   /* consume this block's dedent */
        return b;
    }
    /* no colon: single statement */
    { ANode *s = parse_stmt(m); if (s) a_node_list_add(b, s); }
    return b;
}

static void expect_nl(AModule *m) {
    if (pk(m)->kind == AT_EOF || pk(m)->kind == AT_DEDENT || pk(m)->kind == AT_RC) return;
    if (pk(m)->kind == AT_NL) { nx(m); return; }
    a_perror(m, pk(m)->line, "expected end of line");
}

static ANode *parse_if_chain(AModule *m, AToken *kw) {
    ANode *i = an_new(AN_IF, kw->line);
    ANode *br = an_new(AN_IFBR, kw->line);
    br->a = parse_expr(m); br->b = parse_block(m, kw->col);
    a_node_list_add(i, br);
    skipnl(m);
    while (pk(m)->kind == AT_KW_ELIF) {
        AToken *e = nx(m);
        ANode *br2 = an_new(AN_IFBR, e->line);
        br2->a = parse_expr(m); br2->b = parse_block(m, e->col);
        a_node_list_add(i, br2);
        skipnl(m);
    }
    if (eat(m, AT_KW_ELSE)) {
        i->c = parse_block(m, kw->col);
    }
    return i;
}

static ANode *parse_fn_decl(AModule *m, AToken *kw) {
    ANode *fn = parse_fn_header(m, 0);
    fn->line = kw->line;
    if (pk(m)->kind != AT_COLON) a_perror(m, kw->line, "expected ':' after function header");
    fn->b = parse_block(m, kw->col); /* parse_block consumes the ':' */
    return fn;
}

/* skip a decl keyword's indented/raw body: used for class/enum/trait/etc. */
static ANode *parse_unsupported(AModule *m, AToken *kw) {
    ANode *u = an_new(AN_UNSUPPORTED, kw->line);
    u->name = a_strdup(kw->p ? kw->p : "?");
    /* consume optional header exprs until ':' then skip block */
    int depth = 0;
    while (pk(m)->kind != AT_EOF && pk(m)->kind != AT_NL) {
        int k = pk(m)->kind;
        if (k == AT_COLON && depth == 0) { nx(m); break; }
        if (k == AT_LP || k == AT_LB || k == AT_LC) depth++;
        else if (k == AT_RP || k == AT_RB || k == AT_RC) depth--;
        nx(m);
    }
    skipnl(m);
    if (pk(m)->kind == AT_INDENT) {
        int d = 1; nx(m);
        while (d > 0 && pk(m)->kind != AT_EOF) {
            int k = nx(m)->kind;
            if (k == AT_INDENT) d++;
            else if (k == AT_DEDENT) d--;
        }
    } else {
        /* single-line decl: consume to EOL */
        while (pk(m)->kind != AT_EOF && pk(m)->kind != AT_NL && pk(m)->kind != AT_DEDENT) nx(m);
    }
    return u;
}

static ANode *parse_import_stmt(AModule *m, int from) {
    AToken *kw = nx(m); /* import or from */
    ANode *im = an_new(AN_IMPORT, kw->line);
    /* consume raw module spec: string | ident(.ident)* | std:x | python:x */
    char buf[256]; int bi = 0;
    if (pk(m)->kind == AT_STR) {
        AToken *s = nx(m);
        int l = s->len < 200 ? s->len : 200;
        memcpy(buf, s->sval ? s->sval : "", l); buf[l] = 0; bi = l;
    } else {
        for (;;) {
            AToken *t = pk(m);
            if (t->kind == AT_ID || t->kind == AT_COLON) {
                int l = t->len < 200 - bi ? t->len : 200 - bi;
                if (t->kind == AT_COLON) buf[bi++] = ':';
                else { memcpy(buf + bi, t->p, l); bi += l; }
                nx(m);
            } else break;
            if (pk(m)->kind == AT_DOT) { buf[bi++] = '.'; nx(m); continue; }
            break;
        }
    }
    buf[bi] = 0;
    im->name = a_strdup(buf);
    if (from) {
        /* from X import a, b as c, ... */
        if (!eat(m, AT_KW_IMPORT)) a_perror(m, kw->line, "expected 'import' after module");
        while (pk(m)->kind == AT_ID) { nx(m); if (eat(m, AT_KW_AS)) { if (pk(m)->kind == AT_ID) nx(m); } if (!eat(m, AT_COMMA)) break; }
    } else if (eat(m, AT_KW_AS)) {
        if (pk(m)->kind == AT_ID) nx(m);
    }
    expect_nl(m);
    return im;
}

static ANode *parse_stmt(AModule *m) {
    skipnl(m);
    AToken *t = pk(m);
    switch (t->kind) {
    case AT_KW_FN: nx(m); return parse_fn_decl(m, t);
    case AT_KW_CLASS: case AT_KW_METACLASS: case AT_KW_ENUM:
    case AT_KW_TRAIT: case AT_KW_IMPL: case AT_KW_TYPE: case AT_KW_COMPTIME:
        nx(m); return parse_unsupported(m, t);
    case AT_KW_PASS: nx(m); expect_nl(m); return an_new(AN_PASS, t->line);
    case AT_KW_IF: nx(m); return parse_if_chain(m, t);
    case AT_KW_WHILE: {
        nx(m);
        ANode *w = an_new(AN_WHILE, t->line);
        w->a = parse_expr(m);
        w->b = parse_block(m, t->col);
        return w;
    }
    case AT_KW_DO: {
        nx(m);
        ANode *d = an_new(AN_DOWHILE, t->line);
        d->b = parse_block(m, t->col);
        skipnl(m);
        if (!eat(m, AT_KW_WHILE)) a_perror(m, t->line, "expected 'while' after do block");
        d->a = parse_expr(m);
        expect_nl(m);
        return d;
    }
    case AT_KW_FOR: {
        nx(m);
        ANode *f = an_new(AN_FOR, t->line);
        AToken *v = pk(m);
        if (v->kind == AT_ID) { nx(m); f->name = a_strndup(v->p, v->len); }
        else a_perror(m, v->line, "expected loop variable name");
        if (!eat(m, AT_KW_IN)) a_perror(m, v->line, "expected 'in'");
        f->a = parse_expr(m);
        f->b = parse_block(m, t->col);
        return f;
    }
    case AT_KW_LOOP: {
        nx(m);
        ANode *l = an_new(AN_LOOP, t->line);
        l->b = parse_block(m, t->col);
        return l;
    }
    case AT_KW_RETURN: {
        nx(m);
        ANode *r = an_new(AN_RETURN, t->line);
        if (pk(m)->kind != AT_NL && pk(m)->kind != AT_EOF && pk(m)->kind != AT_DEDENT && pk(m)->kind != AT_RC)
            r->a = parse_expr(m);
        expect_nl(m);
        return r;
    }
    case AT_KW_BREAK: nx(m); expect_nl(m); return an_new(AN_BREAK, t->line);
    case AT_KW_CONTINUE: nx(m); expect_nl(m); return an_new(AN_CONTINUE, t->line);
    case AT_KW_DEFER: {
        nx(m);
        ANode *d = an_new(AN_DEFER, t->line);
        d->a = parse_stmt(m);
        return d;
    }
    case AT_KW_IMPORT: return parse_import_stmt(m, 0);
    case AT_KW_FROM: return parse_import_stmt(m, 1);
    case AT_KW_ELIF: case AT_KW_ELSE:
        a_perror(m, t->line, "branch without matching 'if'");
        nx(m); expect_nl(m); return an_new(AN_PASS, t->line);
    default: break;
    }
    /* typed declaration: Ident ':' Type '=' expr */
    if (t->kind == AT_ID && pk2(m)->kind == AT_COLON) {
        AToken *id = nx(m); nx(m); /* id : */
        static const int trm[] = { AT_EQ };
        skip_type(m, trm, 1);
        if (!eat(m, AT_EQ)) a_perror(m, id->line, "expected '=' in declaration");
        ANode *d = an_new(AN_DECL, id->line);
        d->name = a_strndup(id->p, id->len);
        d->b = parse_expr(m);
        expect_nl(m);
        return d;
    }
    /* simple statement: expr [assignop expr] */
    {
        ANode *target = parse_expr(m);
        int k = pk(m)->kind;
        if (k == AT_EQ || k == AT_PLUSEQ || k == AT_MINUSEQ || k == AT_STAREQ ||
            k == AT_SLASHEQ || k == AT_PERCEQ || k == AT_AMPEQ || k == AT_BAREQ ||
            k == AT_CARETEQ || k == AT_SHLEQ || k == AT_SHREQ) {
            AToken *op = nx(m);
            ANode *val = parse_expr(m);
            expect_nl(m);
            if (target->kind == AN_TUPLE && k == AT_EQ) {
                /* destructuring (a, b) = expr */
                ANode *asg = an_new(AN_ASSIGN, op->line);
                asg->name = a_strdup("="); asg->a = target; asg->b = val;
                asg->aux = 1; /* destructure flag */
                return asg;
            }
            ANode *asg = an_new(AN_ASSIGN, op->line);
            const char *os = (k == AT_EQ) ? "=" : opname(k - (AT_PLUSEQ - AT_PLUS));
            /* opname expects single-char kinds; build op strings explicitly */
            switch (k) {
            case AT_EQ: os = "="; break;
            case AT_PLUSEQ: os = "+="; break;
            case AT_MINUSEQ: os = "-="; break;
            case AT_STAREQ: os = "*="; break;
            case AT_SLASHEQ: os = "/="; break;
            case AT_PERCEQ: os = "%="; break;
            case AT_AMPEQ: os = "&="; break;
            case AT_BAREQ: os = "|="; break;
            case AT_CARETEQ: os = "^="; break;
            case AT_SHLEQ: os = "<<="; break;
            case AT_SHREQ: os = ">>="; break;
            }
            asg->name = a_strdup(os);
            asg->a = target; asg->b = val;
            return asg;
        }
        expect_nl(m);
        ANode *s = an_new(AN_EXPR, t->line);
        s->a = target;
        return s;
    }
}

int a_parse(AModule *m) {
    if (a_lex(m) < 0) return -1;
    m->itok = 0;
    m->prog = an_new(AN_PROG, 1);
    skipnl(m);
    while (pk(m)->kind != AT_EOF) {
        int before = m->itok;
        ANode *s = parse_stmt(m);
        if (s) a_node_list_add(m->prog, s);
        skipnl(m);
        if (m->itok == before) { nx(m); skipnl(m); }
    }
    return m->had_error ? -1 : 0;
}
