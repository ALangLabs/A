#include "methods_internal.h"

/* ---- local fast paths -------------------------------------------------
   The shared helpers in methods_helpers.c stay for cold paths; the hot
   string methods below use exact-size allocations and memcpy runs instead
   of Buf growth + buf_take's second copy. */

/* Append n copies of c to b with one reserve + memset (per-char buf_addc
   loops cost a call and a capacity check per byte). */
static void buf_pad(Buf *b, char c, uint64_t n) {
    if (!n) return;
    buf_reserve(b, (size_t)n);
    memset(b->p + b->len, c, (size_t)n);
    b->len += (size_t)n;
    b->p[b->len] = '\0';
}

/* Two-pass join over str/char avals: measure, then fill one exact-size
   AStr. Same shape as bi_join's array path — no Buf realloc chain, no
   buf_take copy, and small results still come from the bump arena. */
static aval join_items_aval(aval sepv, uint64_t n, aval *items) {
    uint64_t sn;
    const char *sp = str_data(sepv, &sn);
    uint64_t total = 0;
    if (n > 1) {
        if (sn && (uint64_t)(n - 1) > UINT32_MAX / sn)
            panicf("string too large");
        total = (uint64_t)(n - 1) * sn;
    }
    for (uint64_t i = 0; i < n; ++i) {
        aval v = items[i];
        if (kind_of(v) != K_STR && kind_of(v) != K_CHAR)
            panicf("sequence item %llu: expected str instance, %s found",
                   (unsigned long long)i, type_name_cstr(v));
        uint64_t l;
        str_data(v, &l);
        total += l;
        if (total > UINT32_MAX) panicf("string too large");
    }
    aval outv = a_str_new_uninit(total);
    AStr *o = (AStr *)obj_of(outv);
    char *d = o->data;
    for (uint64_t i = 0; i < n; ++i) {
        if (i) {
            memcpy(d, sp, (size_t)sn);
            d += sn;
        }
        uint64_t l;
        const char *p = str_data(items[i], &l);
        if (l) memcpy(d, p, (size_t)l);
        d += l;
    }
    return outv;
}

/* Whitespace split, forward direction. Single pass that emits each word
   directly — str_split_impl's span table costs two AVec pushes and two
   int_val_full reads per word before the same a_str_new calls. Semantics
   match the helper exactly: maxsplit==0 yields one item with only leading
   whitespace stripped; with a limit, the remainder is the raw text from
   the first unconsumed word to the end (internal/trailing whitespace
   preserved). */
static AVec ws_split_fwd(const char *d, uint64_t n, int64_t maxsplit) {
    AVec out = { NULL, 0, 0 };
    if (maxsplit == 0) {
        uint64_t lead = 0;
        while (lead < n && is_space_byte((unsigned char)d[lead])) ++lead;
        if (lead < n) avec_push(&out, a_str_new(d + lead, n - lead));
        return out;
    }
    if (maxsplit < 0) maxsplit = INT64_MAX;
    uint64_t i = 0;
    int64_t k = 0;
    while (i < n) {
        while (i < n && is_space_byte((unsigned char)d[i])) ++i;
        if (i >= n) break;
        uint64_t ws = i;
        while (i < n && !is_space_byte((unsigned char)d[i])) ++i;
        if (k >= maxsplit) {
            avec_push(&out, a_str_new(d + ws, n - ws));
            return out;
        }
        avec_push(&out, a_str_new(d + ws, i - ws));
        ++k;
    }
    return out;
}

/* Rightmost byte == c in p[0..n), or NULL — same word-at-a-time backward
   scan as core_str.c's memrchr8 (kept local: no new header prototypes). */
static const char *memrchr8_local(const char *p, size_t n, unsigned char c) {
    const char *base = p;
    p += n;
    uint64_t pat = UINT64_C(0x0101010101010101) * c;
    while ((size_t)(p - base) >= 8) {
        uint64_t w;
        memcpy(&w, p - 8, 8);
        uint64_t x = w ^ pat;
        uint64_t m = (x - UINT64_C(0x0101010101010101)) & ~x &
                     UINT64_C(0x8080808080808080);
        if (m) {
            for (int k = 7; k >= 0; --k)
                if (p[-8 + k] == (char)c) return p - 8 + k;
        }
        p -= 8;
    }
    while (p > base) {
        --p;
        if ((unsigned char)*p == c) return p;
    }
    return NULL;
}

/* str.find within a byte prefix [0, lim) without copying the prefix into
   a temporary AStr (the old code allocated one per call). Mirrors
   str_find's index/clamp rules on the bounded view. */
static int64_t str_find_bounded(const char *h, uint64_t lim, aval needle,
                                int64_t start, int reverse) {
    uint64_t nn;
    const char *nd = str_data(needle, &nn);
    int64_t st = start;
    if (st < 0) {
        st += (int64_t)u8_count(h, lim);
        if (st < 0) st = 0;
    }
    if (nn == 0) {
        if (st > (int64_t)u8_count(h, lim)) return -1;
        return st;
    }
    uint64_t st_byte = st == 0 ? 0 : cp_to_byte(h, lim, st);
    if (st_byte >= lim) return -1;
    if (!reverse) {
        size_t pos = 0;
        if (!find_bytes(h, (size_t)lim, nd, (size_t)nn, st_byte, &pos))
            return -1;
        return (int64_t)u8_count(h, pos);
    }
    if (nn > lim) return -1;
    int64_t i = (int64_t)(lim - nn);
    while (i >= (int64_t)st_byte) {
        const char *hit = memrchr8_local(h + st_byte,
                                         (size_t)(i - (int64_t)st_byte + 1),
                                         (unsigned char)nd[0]);
        if (!hit) return -1;
        size_t cand = (size_t)(hit - h);
        if (memcmp(h + cand, nd, (size_t)nn) == 0)
            return (int64_t)u8_count(h, cand);
        i = (int64_t)cand - 1;
    }
    return -1;
}

/* Two-pass replace: count matches with find_bytes, size the result
   exactly, then fill with memcpy runs. The Buf version paid realloc
   growth plus buf_take's full extra copy on every call. */
static aval str_replace_fast(aval v, aval oldv, aval newv, int64_t count) {
    AStr *s = (AStr *)obj_of(v);
    uint64_t on, nn, sn = s->h.len;
    const char *op = str_data(oldv, &on), *np = str_data(newv, &nn);
    const char *sp = s->data;
    if (on == 0) panicf("The 'replace' target must not be empty.");
    if (count < 0) count = INT64_MAX;
    /* pass 1: count non-overlapping matches (same scan order as before) */
    uint64_t pos = 0, matches = 0;
    while ((int64_t)matches < count && pos + on <= sn) {
        size_t p;
        if (!find_bytes(sp + pos, (size_t)(sn - pos), op, (size_t)on, 0, &p))
            break;
        ++matches;
        pos += p + on;
    }
    if (matches == 0) return v;  /* immutable: aliasing is safe */
    /* out_len = sn + matches*(nn-on) when nn>on; guard before the
       multiply so the uint64 math cannot wrap. */
    if (nn > on && matches > (uint64_t)(UINT32_MAX - sn) / (nn - on))
        panicf("string too large");
    uint64_t out_len = sn - matches * on + matches * nn;
    if (out_len > UINT32_MAX) panicf("string too large");
    /* pass 2: fill */
    aval outv = a_str_new_uninit(out_len);
    AStr *o = (AStr *)obj_of(outv);
    char *d = o->data;
    pos = 0;
    uint64_t last = 0, done = 0;
    while (done < matches) {
        size_t p;
        find_bytes(sp + pos, (size_t)(sn - pos), op, (size_t)on, 0, &p);
        uint64_t at = pos + p;
        if (at > last) {
            memcpy(d, sp + last, (size_t)(at - last));
            d += at - last;
        }
        if (nn) {
            memcpy(d, np, (size_t)nn);
            d += nn;
        }
        pos = at + on;
        last = pos;
        ++done;
    }
    if (last < sn) {
        memcpy(d, sp + last, (size_t)(sn - last));
        d += sn - last;
    }
    return outv;
}

/* ASCII case maps. In the C locale (the runtime never calls setlocale)
   tolower/toupper/isalpha/isupper/islower only ever touch A-Z/a-z, so the
   range checks below are exact for c < 128; bytes >= 128 fall through to
   the same ctype calls the generic helper used. */
static int asc_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int asc_upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int asc_isalpha(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
static int asc_isupper(int c) { return c >= 'A' && c <= 'Z'; }
static int asc_islower(int c) { return c >= 'a' && c <= 'z'; }

/* Same modes as str_case_map (0 lower, 1 upper, 2 capitalize, 3 title,
   4 swapcase) but allocated through a_str_new_uninit so small results
   come from the bump arena instead of a fresh xmalloc each call. */
static aval str_case_fast(aval v, int mode) {
    AStr *s = (AStr *)obj_of(v);
    uint64_t n = s->h.len;
    aval outv = a_str_new_uninit(n);
    AStr *r = (AStr *)obj_of(outv);
    const unsigned char *d = (const unsigned char *)s->data;
    unsigned char *o = (unsigned char *)r->data;
    int prev_alpha = 0;
    for (uint64_t i = 0; i < n; ++i) {
        unsigned char c = d[i];
        unsigned char oc;
        if (mode == 0) {
            oc = c < 128 ? (unsigned char)asc_lower(c)
                         : (unsigned char)tolower(c);
        } else if (mode == 1) {
            oc = c < 128 ? (unsigned char)asc_upper(c)
                         : (unsigned char)toupper(c);
        } else if (mode == 2) {
            oc = c < 128 ? (unsigned char)(i == 0 ? asc_upper(c)
                                                  : asc_lower(c))
                         : (unsigned char)(i == 0 ? toupper(c)
                                                  : tolower(c));
        } else if (mode == 4) {
            if (c < 128) {
                oc = asc_isupper(c) ? (unsigned char)asc_lower(c)
                     : asc_islower(c) ? (unsigned char)asc_upper(c) : c;
            } else {
                oc = isupper(c) ? (unsigned char)tolower(c)
                     : islower(c) ? (unsigned char)toupper(c) : c;
            }
        } else {
            int alpha = c < 128 ? asc_isalpha(c) : (isalpha(c) != 0);
            if (alpha) {
                int up = c < 128 ? !prev_alpha
                                 : (prev_alpha ? 0 : 1);
                oc = c < 128 ? (unsigned char)(up ? asc_upper(c)
                                                  : asc_lower(c))
                             : (unsigned char)(up ? toupper(c)
                                                  : tolower(c));
            } else {
                oc = c;
            }
        }
        prev_alpha = c < 128 ? asc_isalpha(oc) != 0 : (isalpha(oc) != 0);
        o[i] = oc;
    }
    return outv;
}

/* "{}".format / "{0}".format / "{name}".format — positional and named
   fields with {{ }} escapes, like Python's str.format (no format specs). */
static aval str_format_obj(aval self, uint64_t n, aval *argv) {
    uint64_t slen;
    const char *s = str_data(self, &slen);
    Buf b;
    buf_init(&b);
    uint64_t autoIdx = 0;
    for (uint64_t i = 0; i < slen; ++i) {
        char c = s[i];
        if (c == '}') {
            if (i + 1 < slen && s[i + 1] == '}') { buf_addc(&b, '}'); ++i; continue; }
            panicf("single '}' in format string");
        }
        if (c != '{') { buf_addc(&b, c); continue; }
        if (i + 1 < slen && s[i + 1] == '{') { buf_addc(&b, '{'); ++i; continue; }
        // Parse field: {name} or {index}; no format specs supported.
        uint64_t j = i + 1;
        uint64_t fieldStart = j;
        while (j < slen && s[j] != '}' && s[j] != ':' && s[j] != '!') ++j;
        if (j >= slen || s[j] != '}') {
            panicf("unsupported or unterminated '{}' field in format string");
        }
        uint64_t flen = j - fieldStart;
        aval arg;
        if (flen == 0) {
            if (autoIdx >= n) panicf("not enough arguments for format string");
            arg = argv[autoIdx++];
        } else {
            // Field must be all digits (positional index).
            uint64_t idx = 0;
            int digits = 1;
            for (uint64_t k = fieldStart; k < j; ++k) {
                if (s[k] < '0' || s[k] > '9') { digits = 0; break; }
                idx = idx * 10 + (uint64_t)(s[k] - '0');
            }
            if (!digits) panicf("named format fields are not supported");
            if (idx >= n) panicf("argument index out of range in format string");
            arg = argv[idx];
        }
        aval sv = a_str(arg);
        uint64_t vlen;
        const char *vs = str_data(sv, &vlen);
        buf_addn(&b, vs, vlen);
        i = j;
    }
    (void)autoIdx;
    return buf_take(&b);
}

/* str_affix with memset padding instead of per-byte buf_addc. Same modes
   (0 ljust, 1 rjust, 2 center, 3 zfill) and byte-count semantics. */
static aval str_affix_fast(aval v, int64_t width, aval fillv, int mode) {
    AStr *s = (AStr *)obj_of(v);
    uint64_t n = s->h.len;
    char fill = ' ';
    if (!a_is_none(fillv)) {
        uint64_t fn;
        const char *fp = str_data(fillv, &fn);
        if (fn) fill = fp[0];
    }
    if (width < 0) width = 0;
    if ((uint64_t)width <= n) return a_str_new(s->data, n);
    uint64_t pad = (uint64_t)width - n;
    Buf b;
    buf_init(&b);
    uint64_t left = 0, right = pad;
    if (mode == 1) { left = pad; right = 0; }
    else if (mode == 2) { left = (pad + 1) / 2; right = pad - left; }
    else if (mode == 3) {
        /* zfill: sign first, then zeros */
        uint64_t i = 0;
        if (n && (s->data[0] == '-' || s->data[0] == '+')) {
            buf_addc(&b, s->data[0]);
            ++i;
        }
        buf_pad(&b, '0', pad);
        buf_addn(&b, s->data + i, n - i);
        return buf_take(&b);
    }
    buf_pad(&b, fill, left);
    buf_addn(&b, s->data, n);
    buf_pad(&b, fill, right);
    return buf_take(&b);
}

aval method_str(aval obj, const char *name, uint64_t n, aval *argv) {
        AStr *s = (AStr *)obj_of(obj);
        if (!strcmp(name, "upper")) { want_args(name, n, 0, 0); return str_case_fast(obj, 1); }
        if (!strcmp(name, "lower")) { want_args(name, n, 0, 0); return str_case_fast(obj, 0); }
        if (!strcmp(name, "capitalize")) { want_args(name, n, 0, 0); return str_case_fast(obj, 2); }
        if (!strcmp(name, "title")) { want_args(name, n, 0, 0); return str_case_fast(obj, 3); }
        if (!strcmp(name, "casefold")) { want_args(name, n, 0, 0); return str_case_fast(obj, 0); }
        if (!strcmp(name, "swapcase")) { want_args(name, n, 0, 0); return str_case_fast(obj, 4); }
        if (!strcmp(name, "strip") || !strcmp(name, "lstrip") || !strcmp(name, "rstrip")) {
            int left = name[0] != 'r';
            int right = strcmp(name, "lstrip") != 0;
            if (n == 0) return str_strip_impl(obj, left, right);
            want_args(name, n, 1, 1);
            /* Strip chars arg: trim those bytes (ASCII set). 256-bit
               membership bitmap instead of an inner scan of the cutset
               for every edge byte. */
            uint64_t cn;
            const char *cp = str_data(argv[0], &cn);
            uint32_t set[8];
            memset(set, 0, sizeof set);
            for (uint64_t q = 0; q < cn; ++q) {
                unsigned char ch = (unsigned char)cp[q];
                set[ch >> 5] |= 1u << (ch & 31);
            }
            uint32_t a = 0, b = s->h.len;
            if (left) {
                while (a < b &&
                       (set[(unsigned char)s->data[a] >> 5] >>
                        ((unsigned char)s->data[a] & 31)) & 1u)
                    ++a;
            }
            if (right) {
                while (b > a &&
                       (set[(unsigned char)s->data[b - 1] >> 5] >>
                        ((unsigned char)s->data[b - 1] & 31)) & 1u)
                    --b;
            }
            return a_str_new(s->data + a, (uint64_t)(b - a));
        }
        if (!strcmp(name, "split") || !strcmp(name, "rsplit")) {
            want_args(name, n, 0, 2);
            aval sep = n >= 1 ? argv[0] : A_NONE;
            int64_t ms = -1;
            if (n >= 2) ms = expect_int64(argv[1], "maxsplit");
            /* Whitespace split, forward (and unlimited rsplit, which the
               helper defines as identical): single-pass direct emit. */
            AVec parts;
            if (a_is_none(sep) && (name[0] != 'r' || ms < 0))
                parts = ws_split_fwd(s->data, s->h.len, ms);
            else
                parts = str_split_impl(obj, sep, ms, name[0] == 'r');
            aval r = list_from_vec(parts.p, parts.len, 0);
            free(parts.p);
            return r;
        }
        if (!strcmp(name, "join")) {
            want_args(name, n, 1, 1);
            AObj *io = obj_of(argv[0]);
            /* Fast path: a concrete list/tuple/set of strings joins in two
               passes straight into one exact-size AStr — no iterator
               protocol per element, no Buf growth, no second copy.
               Packed (all-int) arrays skip this and take the generic
               path, which raises the proper type error on item 0. */
            if (io && (io->type == T_LIST || io->type == T_TUPLE ||
                       io->type == T_SET) &&
                !arr_packed((AArr *)io))
                return join_items_aval(obj, ((AArr *)io)->h.len,
                                       ((AArr *)io)->items);
            /* Generic iterable: stream into an AVec (an iterator cannot
               be rewound for the measure pass), then the same fill. */
            aval it = (io && io->type == T_ITER) ? argv[0] : a_iter_new(argv[0]);
            AVec sv = { NULL, 0, 0 };
            aval out;
            while (a_iter_next(it, &out)) {
                if (kind_of(out) != K_STR && kind_of(out) != K_CHAR)
                    panicf("sequence item %llu: expected str instance, %s found",
                           (unsigned long long)sv.len, type_name_cstr(out));
                avec_push(&sv, out);
            }
            aval r = join_items_aval(obj, sv.len, sv.p);
            free(sv.p);
            return r;
        }
        if (!strcmp(name, "replace")) {
            want_args(name, n, 2, 3);
            int64_t count = n == 3 ? expect_int64(argv[2], "count") : -1;
            return str_replace_fast(obj, argv[0], argv[1], count);
        }
        if (!strcmp(name, "format")) {
            return str_format_obj(obj, n, argv);
        }
        if (!strcmp(name, "startswith") || !strcmp(name, "endswith")) {
            want_args(name, n, 1, 3);
            int64_t st = 0, en = (int64_t)s->h.len;
            if (n >= 2) st = expect_int64(argv[1], "start");
            if (n >= 3) en = expect_int64(argv[2], "end");
            if (st < 0) st += (int64_t)s->h.len;
            if (en < 0) en += (int64_t)s->h.len;
            if (st < 0) st = 0;
            if (en > (int64_t)s->h.len) en = (int64_t)s->h.len;
            uint64_t pn;
            const char *pp = str_data(argv[0], &pn);
            if ((uint64_t)(en - st) < pn) return A_FALSE;
            int c = !memcmp(s->data + st, pp, (size_t)pn);
            if (!strcmp(name, "endswith")) {
                int64_t pos = en - (int64_t)pn;
                c = pos >= st && !memcmp(s->data + pos, pp, (size_t)pn);
            }
            return bool_aval(c);
        }
        if (!strcmp(name, "find") || !strcmp(name, "index") || !strcmp(name, "rindex") || !strcmp(name, "rfind")) {
            want_args(name, n, 1, 3);
            int64_t st = 0;
            if (n >= 2) st = expect_int64(argv[1], "start");
            int64_t en = (int64_t)s->h.len;
            if (n >= 3) {
                /* end is a code-point index like Python; normalize
                   negatives and clamp, then map to a byte offset. */
                int64_t total_cp = (int64_t)u8_count(s->data, s->h.len);
                int64_t en_cp = expect_int64(argv[2], "end");
                if (en_cp < 0) en_cp += total_cp;
                if (en_cp < 0) en_cp = 0;
                if (en_cp > total_cp) en_cp = total_cp;
                en = (int64_t)cp_to_byte(s->data, s->h.len, en_cp);
                /* approximate by clamping search to prefix — searched
                   in place, no temporary prefix string */
                if (en < (int64_t)s->h.len) {
                    int64_t r = str_find_bounded(s->data, (uint64_t)en,
                                                 argv[0], st,
                                                 !strcmp(name, "rindex") || !strcmp(name, "rfind"));
                    if ((r < 0 && (!strcmp(name, "index") || !strcmp(name, "rindex"))) ||
                        (r >= 0 && en < st)) r = -1;
                    if (r < 0 && (!strcmp(name, "index") || !strcmp(name, "rindex")))
                        panicf("Substring not found.");
                    return mkint_full(r);
                }
            }
            int64_t r = str_find(obj, argv[0], st, !strcmp(name, "rindex") || !strcmp(name, "rfind"));
            if (r < 0 && (!strcmp(name, "index") || !strcmp(name, "rindex")))
                panicf("Substring not found.");
            return mkint_full(r);
        }
        if (!strcmp(name, "count")) {
            want_args(name, n, 1, 3);
            int64_t st = n >= 2 ? expect_int64(argv[1], "start") : 0;
            int64_t en = n >= 3 ? expect_int64(argv[2], "end") : (int64_t)s->h.len;
            if (en < 0) en += (int64_t)s->h.len;
            if (en < 0) en = 0;
            if (en > (int64_t)s->h.len) en = (int64_t)s->h.len;
            aval hay = obj;
            if (en < (int64_t)s->h.len) hay = a_str_new(s->data, (uint64_t)en);
            uint64_t hn, nn;
            const char *h = str_data(hay, &hn), *nd = str_data(argv[0], &nn);
            int64_t count = 0, pos = st < 0 ? st + (int64_t)hn : st;
            if (pos < 0) pos = 0;
            if (nn == 0) return mkint_full((int64_t)hn + 1 > pos ? (int64_t)hn - pos + 1 : 0);
            while ((uint64_t)pos + nn <= hn) {
                size_t p;
                if (!find_bytes(h + pos, (size_t)(hn - (uint64_t)pos), nd, (size_t)nn, 0, &p)) break;
                ++count;
                pos += (int64_t)p + (int64_t)nn;
            }
            return mkint_full(count);
        }
        if (!strcmp(name, "isdigit")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 0)); }
        if (!strcmp(name, "isalpha")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 1)); }
        if (!strcmp(name, "isspace")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 2)); }
        if (!strcmp(name, "isalnum")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 3)); }
        if (!strcmp(name, "islower")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 4)); }
        if (!strcmp(name, "isupper")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 5)); }
        if (!strcmp(name, "istitle")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 6)); }
        if (!strcmp(name, "isascii")) {
            want_args(name, n, 0, 0);
            for (uint64_t q = 0; q < s->h.len; ++q) if ((unsigned char)s->data[q] >= 128) return A_FALSE;
            return A_TRUE;
        }
        if (!strcmp(name, "isdecimal") || !strcmp(name, "isnumeric")) {
            want_args(name, n, 0, 0);
            if (!s->h.len) return A_FALSE;
            for (uint64_t q = 0; q < s->h.len; ++q) if (!isdigit((unsigned char)s->data[q])) return A_FALSE;
            return A_TRUE;
        }
        if (!strcmp(name, "isidentifier")) {
            want_args(name, n, 0, 0);
            if (!s->h.len) return A_FALSE;
            if (!isalpha((unsigned char)s->data[0]) && s->data[0] != '_') return A_FALSE;
            for (uint64_t q = 1; q < s->h.len; ++q)
                if (!isalnum((unsigned char)s->data[q]) && s->data[q] != '_') return A_FALSE;
            return A_TRUE;
        }
        if (!strcmp(name, "isprintable")) {
            want_args(name, n, 0, 0);
            if (!s->h.len) return A_TRUE;
            for (uint64_t q = 0; q < s->h.len; ++q) if (!isprint((unsigned char)s->data[q])) return A_FALSE;
            return A_TRUE;
        }
        if (!strcmp(name, "expandtabs")) {
            want_args(name, n, 0, 1);
            int64_t tab = n == 1 ? expect_int64(argv[0], "tabsize") : 8;
            if (tab <= 0) tab = 8;
            Buf b;
            buf_init(&b);
            int64_t col = 0;
            for (uint64_t q = 0; q < s->h.len; ++q) {
                char c = s->data[q];
                if (c == '\t') {
                    int64_t spaces = tab - (col % tab);
                    buf_pad(&b, ' ', (uint64_t)spaces);
                    col += spaces;
                } else if (c == '\n' || c == '\r') {
                    buf_addc(&b, c);
                    col = 0;
                } else {
                    buf_addc(&b, c);
                    ++col;
                }
            }
            return buf_take(&b);
        }
        if (!strcmp(name, "removeprefix") || !strcmp(name, "removesuffix")) {
            want_args(name, n, 1, 1);
            uint64_t pn;
            const char *pp = str_data(argv[0], &pn);
            if (!strcmp(name, "removeprefix")) {
                if (s->h.len >= pn && !memcmp(s->data, pp, (size_t)pn))
                    return a_str_new(s->data + pn, s->h.len - pn);
                return obj;
            }
            if (s->h.len >= pn && !memcmp(s->data + s->h.len - pn, pp, (size_t)pn))
                return a_str_new(s->data, s->h.len - pn);
            return obj;
        }
        if (!strcmp(name, "partition") || !strcmp(name, "rpartition")) {
            want_args(name, n, 1, 1);
            int rev = !strcmp(name, "rpartition");
            int64_t r = str_find(obj, argv[0], 0, rev);
            aval head, sep, tail;
            if (r < 0) {
                if (rev) head = obj, tail = a_str_new("", 0);
                else head = obj, tail = a_str_new("", 0);
                sep = a_str_new("", 0);
                /* Python: not found -> (original, '', '') */
                head = obj;
            } else {
                uint64_t pn;
                str_data(argv[0], &pn);
                head = a_str_new(s->data, (uint64_t)r);
                sep = a_str_new(s->data + r, pn);
                tail = a_str_new(s->data + r + pn, s->h.len - (uint64_t)r - pn);
            }
            aval items[3] = { head, sep, tail };
            return a_tuple_new(3, items);
        }
        if (!strcmp(name, "splitlines")) {
            want_args(name, n, 0, 1);
            int keep = n == 1 && truthy(argv[0]);
            aval lst = a_list_new(0, NULL);
            uint64_t start = 0;
            uint64_t i = 0;
            while (i < s->h.len) {
                char c = s->data[i];
                if (c == '\n' || c == '\r') {
                    uint64_t br_end = i + 1;
                    if (c == '\r' && br_end < s->h.len && s->data[br_end] == '\n') br_end++;
                    uint64_t end = keep ? br_end : i;
                    a_list_push(lst, a_str_new(s->data + start, end - start));
                    i = br_end;
                    start = br_end;
                } else {
                    ++i;
                }
            }
            if (start < s->h.len)
                a_list_push(lst, a_str_new(s->data + start, s->h.len - start));
            else if (start == s->h.len && s->h.len == 0) {
                /* empty string -> [] */
            }
            return lst;
        }
        if (!strcmp(name, "zfill") || !strcmp(name, "center") || !strcmp(name, "ljust") || !strcmp(name, "rjust")) {
            want_args(name, n, 1, 2);
            int64_t w = expect_int64(argv[0], "width");
            aval fill = n == 2 ? argv[1] : A_NONE;
            int mode = !strcmp(name, "ljust") ? 0 : !strcmp(name, "rjust") ? 1 : !strcmp(name, "center") ? 2 : 3;
            return str_affix_fast(obj, w, fill, mode);
        }
        if (!strcmp(name, "chars")) {
            want_args(name, n, 0, 0);
            aval lst = a_list_new(0, NULL);
            uint64_t i = 0;
            while (i < s->h.len) {
                uint64_t start = i;
                unsigned char c = (unsigned char)s->data[i];
                if (c < 0x80) i += 1;
                else if ((c & 0xE0) == 0xC0) i += 2;
                else if ((c & 0xF0) == 0xE0) i += 3;
                else if ((c & 0xF8) == 0xF0) i += 4;
                else i += 1;
                if (i > s->h.len) i = s->h.len;
                a_list_push(lst, a_char_new(s->data + start, i - start));
            }
            return lst;
        }
        if (!strcmp(name, "repeat")) {
            want_args(name, n, 1, 1);
            int64_t cnt = expect_int64(argv[0], "count");
            /* str_repeat fills with an exponential self-copy — far fewer
               memcpy calls than the old per-repeatition Buf loop, and no
               Buf growth + buf_take double copy. */
            return str_repeat(obj, cnt);
        }
        if (!strcmp(name, "contains")) {
            want_args(name, n, 1, 1);
            if (kind_of(argv[0]) != K_STR && kind_of(argv[0]) != K_CHAR) return a_bool(0);
            AStr *hay = (AStr *)obj_of(obj);
            AStr *needle = (AStr *)obj_of(argv[0]);
            if (needle->h.len == 0) return a_bool(1);
            if (needle->h.len > hay->h.len) return a_bool(0);
            /* memchr-based search: the memcmp-at-every-offset loop cost 1.7 ms
               per miss on a 200 KB haystack (~120 MB/s vs memmem's ~10 GB/s). */
            size_t pos = 0;
            return a_bool(find_bytes(hay->data, hay->h.len, needle->data,
                                     needle->h.len, 0, &pos) != NULL);
        }
        return method_no_method(obj, name);
}
