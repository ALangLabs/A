#include "a_internal.h"
#include <ctype.h>

/* Lexer for A source. Single pass over m->src[0..m->len); fills m->toks.
 * Mirrors compiler/astra/frontend/lexer (Python-style significant newlines/indent,
 * spaces-only indentation, # comments, longest-match operators). */

static const struct { const char *s; int k; } akw[] = {
    {"fn",AT_KW_FN},{"class",AT_KW_CLASS},{"Class",AT_KW_CLASS},
    {"metaclass",AT_KW_METACLASS},{"Metaclass",AT_KW_METACLASS},
    {"enum",AT_KW_ENUM},{"trait",AT_KW_TRAIT},{"impl",AT_KW_IMPL},
    {"type",AT_KW_TYPE},{"comptime",AT_KW_COMPTIME},{"import",AT_KW_IMPORT},
    {"from",AT_KW_FROM},{"as",AT_KW_AS},{"match",AT_KW_MATCH},{"None",AT_KW_NONE},
    {"do",AT_KW_DO},{"where",AT_KW_WHERE},{"if",AT_KW_IF},{"elif",AT_KW_ELIF},
    {"else",AT_KW_ELSE},{"while",AT_KW_WHILE},{"for",AT_KW_FOR},{"in",AT_KW_IN},
    {"loop",AT_KW_LOOP},{"break",AT_KW_BREAK},{"continue",AT_KW_CONTINUE},
    {"return",AT_KW_RETURN},{"defer",AT_KW_DEFER},{"pass",AT_KW_PASS},
    {"self",AT_KW_SELF},
    {"true",AT_KW_TRUE},{"false",AT_KW_FALSE},{"and",AT_KW_AND},
    {"or",AT_KW_OR},{"not",AT_KW_NOT},{0,0}};

static int aisid0(int c){ return isalpha(c) || c=='_'; }
static int aisid(int c){ return isalnum(c) || c=='_'; }

static void a_push_tok(AModule *m, AToken t){
    if(m->ntok >= m->tokcap){
        m->tokcap = m->tokcap ? m->tokcap*2 : 256;
        m->toks = (AToken*)tcc_realloc(m->toks, m->tokcap*sizeof(AToken));
    }
    m->toks[m->ntok++] = t;
}

/* Count logical characters in a raw single-quoted literal body: a backslash
 * escape (\n, \xNN, \u{...}, ...) counts as ONE unit. */
static int a_char_units(const char *p, int len){
    int u = 0, j = 0;
    while(j < len){
        if(p[j]=='\\'){
            u++; j++;
            if(j>=len) break;
            if(p[j]=='x'){ j++; int k=0; while(k<2 && j<len && isxdigit((unsigned char)p[j])){ j++; k++; } }
            else if(p[j]=='u' && j+1<len && p[j+1]=='{'){ j+=2; while(j<len && p[j]!='}') j++; if(j<len) j++; }
            else j++;
        } else { u++; j++; }
    }
    return u;
}

int a_lex(AModule *m){
    const char *s = m->src; int n = m->len;
    if(!s) n = 0;
    m->ntok = 0; m->itok = 0;   /* lexer owns the token stream; safe to re-lex */
    int i = 0, line = 1, col = 1, depth = 0;
    int *ind = (int*)tcc_malloc(64*sizeof(int)); int nind = 1, ic = 64; ind[0] = 0;
    int at_bol = 1;   /* next chars are leading whitespace of a logical line */
    int first  = 1;   /* next real token is first on its logical line (bol pending) */
    int has_tok = 0;  /* current logical line produced at least one real token */
    int nerr = 0;

#define CUR()   ((i<n) ? (unsigned char)s[i]   : 0)
#define PEEK(k) ((i+(k)<n) ? (unsigned char)s[i+(k)] : 0)
#define ADV()   do{ unsigned char _c=(unsigned char)s[i++]; if(_c=='\n'){line++;col=1;} else col++; }while(0)
#define TOK(K,LN,CL,P,LE,B) do{ AToken t; memset(&t,0,sizeof t); \
    t.kind=(K); t.line=(LN); t.col=(CL); t.p=(P); t.len=(LE); t.bol=(B); \
    a_push_tok(m,t); }while(0)
/* real token: bol = its column iff it is the first token of the logical line */
#define EMITR(K,LN,CL,P,LE) do{ TOK(K,LN,CL,P,LE, first?(CL):0); first=0; has_tok=1; }while(0)
#define LERR(LN, ...) do{ a_perror(m, (LN), __VA_ARGS__); nerr++; }while(0)

    while(i<n){
        /* ---- start of a logical line: measure indentation (spaces only) ---- */
        if(at_bol && depth==0){
            int indcnt = 0;
            while(i<n && (s[i]==' ' || s[i]=='\t')){
                if(s[i]=='\t') LERR(line, "tabs are not allowed for indentation");
                ADV(); indcnt++;
            }
            if(i>=n) break;
            unsigned char c = CUR();
            if(c=='\n'){ ADV(); continue; }                 /* blank line: no tokens, no indent change */
            if(c=='\r'){ ADV(); continue; }                 /* stray CR: stay at bol */
            if(c=='#'){                                      /* comment-only line */
                while(i<n && s[i]!='\n') ADV();
                if(i<n) ADV();                              /* consume the newline too */
                continue;
            }
            if(indcnt > ind[nind-1]){
                if(nind>=ic){ ic*=2; ind=(int*)tcc_realloc(ind, ic*sizeof(int)); }
                ind[nind++] = indcnt;
                TOK(AT_INDENT, line, col, 0, 0, 0);
            } else if(indcnt < ind[nind-1]){
                /* lenient: pop until top <= indcnt; no "inconsistent indent" error */
                while(nind>1 && indcnt < ind[nind-1]){ nind--; TOK(AT_DEDENT, line, col, 0, 0, 0); }
            }
            at_bol = 0;
        }

        unsigned char c = CUR();
        if(c==' ' || c=='\t' || c=='\r'){ ADV(); continue; }

        if(c=='\n'){
            int el = line, ec = col;
            ADV();
            if(depth==0){
                /* a line continuing with '|>' belongs to the previous statement:
                 * suppress the newline and the indentation measurement */
                int j = i;
                while(j<n && (s[j]==' ' || s[j]=='\t' || s[j]=='\r')) j++;
                int cont = (j+1<n && s[j]=='|' && s[j+1]=='>');
                if(!cont){
                    if(has_tok) TOK(AT_NL, el, ec, 0, 0, 0);
                    has_tok = 0; at_bol = 1; first = 1;
                }
            }
            continue;
        }
        if(c=='#'){ while(i<n && s[i]!='\n') ADV(); continue; }

        /* ---- identifiers / keywords ---- */
        if(aisid0(c)){
            int sl=line, sc=col, st=i;
            ADV();
            while(i<n && aisid((unsigned char)s[i])) ADV();
            int l=i-st, k=AT_ID;
            for(int t=0; akw[t].s; t++)
                if((int)strlen(akw[t].s)==l && !memcmp(akw[t].s, s+st, l)){ k=akw[t].k; break; }
            EMITR(k, sl, sc, s+st, l);
            continue;
        }

        /* ---- numbers ---- */
        if(isdigit(c)){
            int sl=line, sc=col, st=i;
            if(c=='0' && (PEEK(1)=='x' || PEEK(1)=='X')){
                ADV(); ADV();
                unsigned long long v=0; int nd=0;
                while(i<n && (isxdigit((unsigned char)s[i]) || s[i]=='_')){
                    if(s[i]!='_'){ unsigned char d=s[i];
                        v = v*16 + (isdigit(d) ? d-'0' : tolower(d)-'a'+10); nd++; }
                    ADV();
                }
                if(!nd) LERR(sl, "hexadecimal literal has no digits");
                EMITR(AT_INT, sl, sc, s+st, i-st);
                m->toks[m->ntok-1].ival = (long long)v;
                continue;
            }
            if(c=='0' && (PEEK(1)=='b' || PEEK(1)=='B')){
                ADV(); ADV();
                unsigned long long v=0; int nd=0;
                while(i<n && (s[i]=='0' || s[i]=='1' || s[i]=='_')){
                    if(s[i]!='_'){ v = v*2 + (unsigned)(s[i]-'0'); nd++; }
                    ADV();
                }
                if(!nd) LERR(sl, "binary literal has no digits");
                EMITR(AT_INT, sl, sc, s+st, i-st);
                m->toks[m->ntok-1].ival = (long long)v;
                continue;
            }
            if(c=='0' && (PEEK(1)=='o' || PEEK(1)=='O')){
                ADV(); ADV();
                unsigned long long v=0; int nd=0;
                while(i<n && ((s[i]>='0' && s[i]<='7') || s[i]=='_')){
                    if(s[i]!='_'){ v = v*8 + (unsigned)(s[i]-'0'); nd++; }
                    ADV();
                }
                if(!nd) LERR(sl, "octal literal has no digits");
                EMITR(AT_INT, sl, sc, s+st, i-st);
                m->toks[m->ntok-1].ival = (long long)v;
                continue;
            }
            /* decimal int / float */
            while(i<n && (isdigit((unsigned char)s[i]) || s[i]=='_')) ADV();
            int isf = 0;
            /* '.' begins a fraction when followed by a digit, or when it cannot
             * start a '..' range or a field name (so `1.` is a float like in
             * the reference lexer, while `1..5` -> `1`,`..`,`5` and `1.x` -> `1`,`.`,`x`) */
            if(i<n && s[i]=='.'){
                unsigned char p1 = PEEK(1);
                if(isdigit(p1) || (p1!='.' && !aisid0(p1))){
                    isf = 1; ADV();
                    while(i<n && (isdigit((unsigned char)s[i]) || s[i]=='_')) ADV();
                }
            }
            /* exponent: only when at least one digit follows e[+-]? */
            if(i<n && (s[i]=='e' || s[i]=='E')){
                int sv=i, sl2=line, sc2=col;
                ADV();
                if(i<n && (s[i]=='+' || s[i]=='-')) ADV();
                if(i<n && isdigit((unsigned char)s[i])){
                    isf = 1;
                    while(i<n && (isdigit((unsigned char)s[i]) || s[i]=='_')) ADV();
                } else { i=sv; line=sl2; col=sc2; }
            }
            EMITR(isf?AT_FLOAT:AT_INT, sl, sc, s+st, i-st);
            {
                AToken *t = &m->toks[m->ntok-1];
                int l=i-st, j=0; char *b=(char*)tcc_malloc(l+1);
                for(int k=st;k<i;k++) if(s[k]!='_') b[j++]=s[k];
                b[j]=0;
                if(isf){ t->dval=strtod(b,0); memcpy(&t->uval,&t->dval,8); t->ival=(long long)t->uval; }
                else    { t->ival=strtoll(b,0,10); }
            }
            continue;
        }

        /* ---- strings / char literals ---- */
        if(c=='"' || c=='\''){
            int q=c, sl=line, sc=col;
            int triple = (i+2<n && s[i+1]==q && s[i+2]==q);
            if(triple){
                /* raw string: no escapes, no interpolation handling here */
                ADV(); ADV(); ADV();
                if(i<n && s[i]=='\r' && PEEK(1)=='\n'){ ADV(); ADV(); }
                else if(i<n && s[i]=='\n'){ ADV(); }
                int content=i, closed=0;
                while(i<n){
                    if(s[i]==q && i+2<n && s[i+1]==q && s[i+2]==q){ closed=1; break; }
                    ADV();
                }
                int inner_end=i;
                if(closed){ ADV(); ADV(); ADV(); }
                else LERR(sl, "unterminated triple-quoted string literal");
                int il=inner_end-content;
                EMITR(AT_STR, sl, sc, s+content, il);
                AToken *t=&m->toks[m->ntok-1];
                t->sval=a_strndup(s+content, il); t->raw=1;
                continue;
            }
            ADV(); /* opening quote */
            int content=i, closed=0;
            while(i<n){
                unsigned char ch=CUR();
                if(ch=='\\'){ ADV(); if(i>=n) break; if(CUR()=='\n') break; ADV(); continue; }
                if(ch==q){ ADV(); closed=1; break; }
                if(ch=='\n') break;      /* single-line literal: newline ends it (error) */
                ADV();
            }
            int inner_end = closed ? i-1 : i;
            int il = inner_end-content;
            if(!closed) LERR(sl, q=='\'' ? "unterminated character literal" : "unterminated string literal");
            int kind = AT_STR;
            if(q=='\''){
                /* 'x' or '\n'-style single unit -> AT_CHAR; anything longer -> AT_STR */
                if(a_char_units(s+content, il)==1) kind=AT_CHAR;
            }
            EMITR(kind, sl, sc, s+content, il);
            AToken *t=&m->toks[m->ntok-1];
            t->sval=a_strndup(s+content, il);
            t->is_char = (kind==AT_CHAR);
            continue;
        }

        /* ---- operators / punctuation: longest match ---- */
        {
            int sl=line, sc=col, st=i;
            unsigned char c2=PEEK(1), c3=PEEK(2);
            int k=-1, w=0;
            if(c=='<' && c2=='<' && c3=='='){ k=AT_SHLEQ; w=3; }
            else if(c=='>' && c2=='>' && c3=='='){ k=AT_SHREQ; w=3; }
            else if(c=='<' && c2=='<'){ k=AT_SHL; w=2; }
            else if(c=='>' && c2=='>'){ k=AT_SHR; w=2; }
            else if(c=='?' && c2=='.'){ k=AT_QUESTIONDOT; w=2; }
            else if(c=='?' && c2=='?'){ k=AT_QUESTIONQUESTION; w=2; }
            else if(c=='.' && c2=='.'){ k=AT_RANGE; w=2; }
            else if(c=='-' && c2=='>'){ k=AT_ARROW; w=2; }
            else if(c=='=' && c2=='>'){ k=AT_FATARROW; w=2; }
            else if(c=='|' && c2=='>'){ k=AT_PIPEOP; w=2; }
            else if(c=='*' && c2=='*'){ k=AT_POW; w=2; }
            else if(c=='<' && c2=='='){ k=AT_LE; w=2; }
            else if(c=='>' && c2=='='){ k=AT_GE; w=2; }
            else if(c=='=' && c2=='='){ k=AT_EQEQ; w=2; }
            else if(c=='!' && c2=='='){ k=AT_NE; w=2; }
            else if(c=='+' && c2=='='){ k=AT_PLUSEQ; w=2; }
            else if(c=='-' && c2=='='){ k=AT_MINUSEQ; w=2; }
            else if(c=='*' && c2=='='){ k=AT_STAREQ; w=2; }
            else if(c=='/' && c2=='='){ k=AT_SLASHEQ; w=2; }
            else if(c=='%' && c2=='='){ k=AT_PERCEQ; w=2; }
            else if(c=='&' && c2=='='){ k=AT_AMPEQ; w=2; }
            else if(c=='|' && c2=='='){ k=AT_BAREQ; w=2; }
            else if(c=='^' && c2=='='){ k=AT_CARETEQ; w=2; }
            else if(c=='+' && c2=='+'){ k=AT_PLUSPLUS; w=2; }
            else if(c=='-' && c2=='-'){ k=AT_MINUSMINUS; w=2; }
            else switch(c){
                case '(': k=AT_LP;  w=1; break;
                case ')': k=AT_RP;  w=1; break;
                case '[': k=AT_LB;  w=1; break;
                case ']': k=AT_RB;  w=1; break;
                case '{': k=AT_LC;  w=1; break;
                case '}': k=AT_RC;  w=1; break;
                case ',': k=AT_COMMA; w=1; break;
                case ':': k=AT_COLON; w=1; break;
                case '.': k=AT_DOT;  w=1; break;
                case ';': k=AT_SEMI; w=1; break;
                case '?': k=AT_QUESTION; w=1; break;
                case '!': k=AT_BANG; w=1; break;
                case '~': k=AT_TILDE; w=1; break;
                case '&': k=AT_AMP; w=1; break;
                case '|': k=AT_PIPE; w=1; break;
                case '^': k=AT_CARET; w=1; break;
                case '+': k=AT_PLUS; w=1; break;
                case '-': k=AT_MINUS; w=1; break;
                case '*': k=AT_STAR; w=1; break;
                case '/': k=AT_SLASH; w=1; break;
                case '%': k=AT_PERCENT; w=1; break;
                case '=': k=AT_EQ;   w=1; break;
                case '<': k=AT_LT;   w=1; break;
                case '>': k=AT_GT;   w=1; break;
                default: break;
            }
            if(k<0){
                LERR(sl, "unexpected character");
                ADV();
                continue;
            }
            if(c=='(' || c=='[' || c=='{') depth++;
            else if((c==')' || c==']' || c=='}') && depth>0) depth--;
            for(int t=0;t<w;t++) ADV();
            EMITR(k, sl, sc, s+st, w);
            continue;
        }
    }

    /* EOF: terminate a pending logical line, close all indent levels, EOF */
    if(has_tok) TOK(AT_NL, line, col, s+i, 0, 0);
    while(nind>1){ nind--; TOK(AT_DEDENT, line, col, 0, 0, 0); }
    TOK(AT_EOF, line, col, s+i, 0, 0);

#undef CUR
#undef PEEK
#undef ADV
#undef TOK
#undef EMITR
#undef LERR
    return nerr ? -1 : 0;
}
