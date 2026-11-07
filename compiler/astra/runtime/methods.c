#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* methods                                                             */
/* ------------------------------------------------------------------ */

void want_args(const char *name, uint64_t n, uint64_t min, uint64_t max) {
    if (n < min || n > max) {
        if (min == max)
            panicf("'%s' expects %llu argument(s), got %llu", name,
                   (unsigned long long)min, (unsigned long long)n);
        panicf("'%s' expects between %llu and %llu arguments, got %llu", name,
               (unsigned long long)min, (unsigned long long)max,
               (unsigned long long)n);
    }
}

static aval method_no_method(aval obj, const char *name) {
    panicf("'%s' object has no method '%s'", type_name_cstr(obj), name);
    return A_NONE;
}

static aval str_case_map(aval v, int mode) {
    /* mode: 0 lower, 1 upper, 2 capitalize, 3 title, 4 swapcase */
    AStr *s = (AStr *)obj_of(v);
    AStr *r = (AStr *)xmalloc(sizeof(AStr) + (size_t)s->h.len + 1);
    r->h.type = T_STR;
    r->h.len = s->h.len;
    r->h.cap = s->h.len + 1;
    int prev_alpha = 0;
    for (uint32_t i = 0; i < s->h.len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        unsigned char o = c;
        if (mode == 0) o = (unsigned char)tolower(c);
        else if (mode == 1) o = (unsigned char)toupper(c);
        else if (mode == 2) o = (unsigned char)(i == 0 ? toupper(c) : tolower(c));
        else if (mode == 4) o = (unsigned char)(isupper(c) ? tolower(c) : islower(c) ? toupper(c) : c);
        else {
            if (isalpha(c)) o = (unsigned char)(prev_alpha ? tolower(c) : toupper(c));
            else o = c;
        }
        prev_alpha = isalpha(o) != 0;
        r->data[i] = (char)o;
    }
    r->data[s->h.len] = '\0';
    return mkobj(&r->h);
}

static int is_space_byte(unsigned char c) { return isspace(c) != 0; }

static aval str_strip_impl(aval v, int left, int right) {
    AStr *s = (AStr *)obj_of(v);
    uint32_t a = 0, b = s->h.len;
    if (left) while (a < b && is_space_byte((unsigned char)s->data[a])) ++a;
    if (right) while (b > a && is_space_byte((unsigned char)s->data[b - 1])) --b;
    return a_str_new(s->data + a, (uint64_t)(b - a));
}

static AVec str_split_impl(aval v, aval sep, int64_t maxsplit, int rsplit) {
    AStr *s = (AStr *)obj_of(v);
    uint64_t n = s->h.len;
    AVec out = { NULL, 0, 0 };
    if (a_is_none(sep)) {
        if (maxsplit == 0) {
            avec_push(&out, a_str_new(s->data, n));
            return out;
        }
        /* whitespace split; for rsplit with a limit merge leading words */
        uint64_t i = 0;
        int64_t splits = 0;
        while (i < n) {
            while (i < n && is_space_byte((unsigned char)s->data[i])) ++i;
            if (i >= n) break;
            uint64_t st = i;
            while (i < n && !is_space_byte((unsigned char)s->data[i])) ++i;
            avec_push(&out, a_str_new(s->data + st, i - st));
            if (maxsplit >= 0 && ++splits >= maxsplit && i < n) {
                /* keep remainder as one token */
                while (i < n && is_space_byte((unsigned char)s->data[i])) ++i;
                if (i < n) avec_push(&out, a_str_new(s->data + i, n - i));
                break;
            }
        }
        (void)rsplit;
        return out;
    }
    uint64_t sn;
    const char *sp = str_data(sep, &sn);
    if (sn == 0) panicf("empty separator");
    if (maxsplit < 0) maxsplit = INT64_MAX;
    if (!rsplit) {
        uint64_t pos = 0, last = 0;
        int64_t done = 0;
        while (pos + sn <= n && done < maxsplit) {
            size_t p;
            if (!find_bytes(s->data + pos, (size_t)(n - pos), sp, (size_t)sn, 0, &p)) break;
            uint64_t at = pos + p;
            avec_push(&out, a_str_new(s->data + last, at - last));
            pos = at + sn;
            last = pos;
            ++done;
        }
        avec_push(&out, a_str_new(s->data + last, n - last));
        return out;
    }
    /* rsplit: find all occurrences, keep the last maxsplit separators */
    AVec occ = { NULL, 0, 0 };
    uint64_t pos = 0;
    while (pos + sn <= n) {
        size_t p;
        if (!find_bytes(s->data + pos, (size_t)(n - pos), sp, (size_t)sn, 0, &p)) break;
        uint64_t at = pos + p;
        avec_push(&occ, (aval)at);
        pos = at + sn;
    }
    if ((int64_t)occ.len <= maxsplit) {
        /* same as split */
        free(occ.p);
        return str_split_impl(v, sep, -1, 0);
    }
    uint64_t keep = (uint64_t)maxsplit;
    uint64_t first = occ.len - keep; /* index of first kept occurrence */
    uint64_t start = 0;
    for (uint64_t k = first; k < occ.len; ++k) {
        uint64_t at = (uint64_t)occ.p[k];
        avec_push(&out, a_str_new(s->data + start, at - start));
        start = at + sn;
    }
    avec_push(&out, a_str_new(s->data + start, n - start));
    free(occ.p);
    return out;
}

static aval str_replace(aval v, aval oldv, aval newv, int64_t count) {
    AStr *s = (AStr *)obj_of(v);
    uint64_t on, nn, sn = s->h.len;
    const char *op = str_data(oldv, &on), *np = str_data(newv, &nn);
    const char *sp = s->data;
    Buf b;
    buf_init(&b);
    if (on == 0) panicf("The 'replace' target must not be empty.");
    if (count < 0) count = INT64_MAX;
    uint64_t pos = 0, last = 0;
    int64_t done = 0;
    while (done < count && pos + on <= sn) {
        size_t p;
        if (!find_bytes(sp + pos, (size_t)(sn - pos), op, (size_t)on, 0, &p)) break;
        uint64_t at = pos + p;
        buf_addn(&b, sp + last, at - last);
        buf_addn(&b, np, (size_t)nn);
        pos = at + on;
        last = pos;
        ++done;
    }
    buf_addn(&b, sp + last, sn - last);
    return buf_take(&b);
}

static int str_pred(aval v, int pred) {
    AStr *s = (AStr *)obj_of(v);
    if (s->h.len == 0) return 0;
    int has_lower = 0, has_upper = 0, prev_alpha = 0, title_ok = 1;
    for (uint32_t i = 0; i < s->h.len; ++i) {
        unsigned char c = (unsigned char)s->data[i];
        switch (pred) {
            case 0: if (!isdigit(c)) return 0; break;                 /* isdigit */
            case 1: if (!isalpha(c)) return 0; break;                 /* isalpha */
            case 2: if (!isspace(c)) return 0; break;                 /* isspace */
            case 3: if (!isalnum(c)) return 0; break;                 /* isalnum */
            case 4: /* islower */
                if (isupper(c)) return 0;
                if (islower(c)) has_lower = 1;
                break;
            case 5: /* isupper */
                if (islower(c)) return 0;
                if (isupper(c)) has_upper = 1;
                break;
            case 6: /* istitle */
                if (isalpha(c)) {
                    if (islower(c) && !prev_alpha) title_ok = 0;
                    if (isupper(c) && prev_alpha) title_ok = 0;
                    prev_alpha = 1;
                } else prev_alpha = 0;
                break;
        }
    }
    if (pred == 4) return has_lower;
    if (pred == 5) return has_upper;
    if (pred == 6) return title_ok;
    return 1;
}

static aval str_affix(aval v, int64_t width, aval fillv, int mode) {
    /* mode 0 ljust, 1 rjust, 2 center, 3 zfill */
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
        for (uint64_t k = i; k < pad + i; ++k) buf_addc(&b, '0');
        buf_addn(&b, s->data + i, n - i);
        return buf_take(&b);
    }
    for (uint64_t i = 0; i < left; ++i) buf_addc(&b, fill);
    buf_addn(&b, s->data, n);
    for (uint64_t i = 0; i < right; ++i) buf_addc(&b, fill);
    return buf_take(&b);
}

static aval seq_index(aval v, aval item, int64_t start, int64_t stop) {
    AArr *a = (AArr *)obj_of(v);
    uint64_t n = a->h.len;
    int64_t lo = start < 0 ? start + (int64_t)n : start;
    int64_t hi = stop < 0 ? stop + (int64_t)n : stop;
    if (lo < 0) lo = 0;
    if (hi > (int64_t)n) hi = (int64_t)n;
    for (int64_t i = lo; i < hi; ++i)
        if (values_equal(a->items[i], item)) return mkint_full(i);
    panicf("Value is not in the list.");
    return A_NONE;
}

static int seq_contains(aval v, aval item) {
    AArr *a = (AArr *)obj_of(v);
    for (uint64_t i = 0; i < a->h.len; ++i)
        if (values_equal(a->items[i], item)) return 1;
    return 0;
}

void sort_arr(aval *a, uint64_t n) {
    for (uint64_t i = 1; i < n; ++i) {
        aval key = a[i];
        int64_t j = (int64_t)i - 1;
        while (j >= 0 && truthy(a_lt(key, a[j]))) {
            a[j + 1] = a[j];
            --j;
        }
        a[j + 1] = key;
    }
}

aval collect_iterable(aval v) {
    return a_list_from_iter(v);
}

aval a_getattr(aval obj, const char *name) {
    if (kind_of(obj) == K_DICT) {
        ADict *d = (ADict *)obj_of(obj);
        aval key = a_str_new(name, strlen(name));
        int64_t i = dict_find(d, key);
        if (i >= 0) return d->pairs[i].v;
    }
    panicf("'%s' object has no attribute '%s'", type_name_cstr(obj), name);
    return A_NONE;
}

static aval list_pop(aval v, int64_t idx, int has_idx) {
    AArr *a = (AArr *)obj_of(v);
    if (a->h.len == 0) panicf("Cannot pop from an empty array.");
    int64_t i = has_idx ? normalize_index(idx, a->h.len) : (int64_t)a->h.len - 1;
    aval r = a->items[i];
    memmove(a->items + i, a->items + i + 1,
            (size_t)(a->h.len - (uint64_t)i - 1) * sizeof(aval));
    a->h.len--;
    return r;
}

static aval dict_method_get(aval v, uint64_t n, aval *argv) {
    want_args("get", n, 1, 2);
    return a_dict_get(v, argv[0], n == 2 ? argv[1] : A_NONE);
}

static aval make_keys_values_items(aval v, int mode) {
    ADict *d = (ADict *)obj_of(v);
    AArr *out = (AArr *)xmalloc(sizeof(AArr));
    out->h.type = T_LIST;
    out->h.len = 0;
    out->h.cap = d->h.len;
    out->items = (aval *)xmalloc((size_t)(d->h.len ? d->h.len : 1) * sizeof(aval));
    for (uint64_t i = 0; i < d->h.len; ++i) {
        aval x;
        if (mode == 0) x = d->pairs[i].k;
        else if (mode == 1) x = d->pairs[i].v;
        else {
            aval pair[2] = { d->pairs[i].k, d->pairs[i].v };
            x = a_tuple_new(2, pair);
        }
        out->items[out->h.len++] = x;
    }
    return mkobj(&out->h);
}

static aval method_set_op(aval v, aval other, int mode) {
    /* mode: 0 union, 1 intersection, 2 difference, 3 symmetric_difference */
    aval tmp = collect_iterable(other);
    AArr *o = (AArr *)obj_of(tmp);
    AArr *s = (AArr *)obj_of(v);
    AArr *out = (AArr *)xmalloc(sizeof(AArr));
    out->h.type = T_SET;
    out->h.len = 0;
    out->h.cap = s->h.len + o->h.len;
    out->items = (aval *)xmalloc((size_t)(out->h.cap ? out->h.cap : 1) * sizeof(aval));
    for (uint64_t i = 0; i < s->h.len; ++i) {
        int in_o = 0;
        for (uint64_t j = 0; j < o->h.len; ++j)
            if (values_equal(s->items[i], o->items[j])) { in_o = 1; break; }
        int keep = mode == 0 ? 1 : mode == 1 ? in_o : mode == 2 ? !in_o : (in_o ? 0 : 1);
        /* symmetric_difference keeps elements in exactly one side; first pass adds self-only */
        if (mode == 3) keep = !in_o;
        if (keep && !set_contains(out, s->items[i])) arr_push(out, s->items[i]);
    }
    if (mode == 0 || mode == 3) {
        for (uint64_t j = 0; j < o->h.len; ++j) {
            int in_s = 0;
            for (uint64_t i = 0; i < s->h.len; ++i)
                if (values_equal(o->items[j], s->items[i])) { in_s = 1; break; }
            if ((mode == 0 || !in_s) && !set_contains(out, o->items[j]))
                arr_push(out, o->items[j]);
        }
    }
    return mkobj(&out->h);
}

static aval method_subset(aval v, aval other, int sup) {
    aval tmp = collect_iterable(other);
    AArr *o = (AArr *)obj_of(tmp);
    AArr *s = (AArr *)obj_of(v);
    AArr *base = sup ? s : o;
    AArr *test = sup ? o : s;
    for (uint64_t i = 0; i < test->h.len; ++i) {
        int found = 0;
        for (uint64_t j = 0; j < base->h.len; ++j)
            if (values_equal(test->items[i], base->items[j])) { found = 1; break; }
        if (!found) return A_FALSE;
    }
    return A_TRUE;
}

aval a_method(aval obj, const char *name, uint64_t n, aval *argv) {
    int k = kind_of(obj);
    if (k == K_STR) {
        AStr *s = (AStr *)obj_of(obj);
        if (!strcmp(name, "upper")) { want_args(name, n, 0, 0); return str_case_map(obj, 1); }
        if (!strcmp(name, "lower")) { want_args(name, n, 0, 0); return str_case_map(obj, 0); }
        if (!strcmp(name, "capitalize")) { want_args(name, n, 0, 0); return str_case_map(obj, 2); }
        if (!strcmp(name, "title")) { want_args(name, n, 0, 0); return str_case_map(obj, 3); }
        if (!strcmp(name, "casefold")) { want_args(name, n, 0, 0); return str_case_map(obj, 0); }
        if (!strcmp(name, "swapcase")) { want_args(name, n, 0, 0); return str_case_map(obj, 4); }
        if (!strcmp(name, "strip") || !strcmp(name, "lstrip") || !strcmp(name, "rstrip")) {
            int left = name[0] != 'r';
            int right = strcmp(name, "lstrip") != 0;
            if (n == 0) return str_strip_impl(obj, left, right);
            want_args(name, n, 1, 1);
            /* Strip chars arg: trim those bytes (ASCII set). */
            uint64_t cn;
            const char *cp = str_data(argv[0], &cn);
            uint32_t a = 0, b = s->h.len;
            int inSet = 0;
            if (left) {
                while (a < b) {
                    inSet = 0;
                    for (uint64_t q = 0; q < cn; ++q) if (s->data[a] == cp[q]) { inSet = 1; break; }
                    if (!inSet) break;
                    ++a;
                }
            }
            if (right) {
                while (b > a) {
                    inSet = 0;
                    for (uint64_t q = 0; q < cn; ++q) if (s->data[b - 1] == cp[q]) { inSet = 1; break; }
                    if (!inSet) break;
                    --b;
                }
            }
            return a_str_new(s->data + a, (uint64_t)(b - a));
        }
        if (!strcmp(name, "split") || !strcmp(name, "rsplit")) {
            want_args(name, n, 0, 2);
            aval sep = n >= 1 ? argv[0] : A_NONE;
            int64_t ms = -1;
            if (n >= 2) ms = expect_int64(argv[1], "maxsplit");
            AVec parts = str_split_impl(obj, sep, ms, name[0] == 'r');
            aval r = list_from_vec(parts.p, parts.len, 0);
            free(parts.p);
            return r;
        }
        if (!strcmp(name, "join")) {
            want_args(name, n, 1, 1);
            aval tmp = collect_iterable(argv[0]);
            AArr *a = (AArr *)obj_of(tmp);
            for (uint64_t i = 0; i < a->h.len; ++i)
                if (kind_of(a->items[i]) != K_STR && kind_of(a->items[i]) != K_CHAR)
                    panicf("sequence item %llu: expected str instance, %s found",
                           (unsigned long long)i, type_name_cstr(a->items[i]));
            aval r = a_str_joinsep(obj, a->h.len, a->items);
            return r;
        }
        if (!strcmp(name, "replace")) {
            want_args(name, n, 2, 3);
            int64_t count = n == 3 ? expect_int64(argv[2], "count") : -1;
            return str_replace(obj, argv[0], argv[1], count);
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
            int c = !strncmp(s->data + st, pp, (size_t)pn);
            if (!strcmp(name, "endswith")) {
                int64_t pos = en - (int64_t)pn;
                c = pos >= st && !strncmp(s->data + pos, pp, (size_t)pn);
            }
            return bool_aval(c);
        }
        if (!strcmp(name, "find") || !strcmp(name, "index") || !strcmp(name, "rindex") || !strcmp(name, "rfind")) {
            want_args(name, n, 1, 3);
            int64_t st = 0;
            if (n >= 2) st = expect_int64(argv[1], "start");
            int64_t en = (int64_t)s->h.len;
            if (n >= 3) {
                en = expect_int64(argv[2], "end");
                /* approximate by clamping search to prefix */
                if (en < (int64_t)s->h.len) {
                    aval tmp = a_str_new(s->data, (uint64_t)en);
                    int64_t r = str_find(tmp, argv[0], st, !strcmp(name, "rindex") || !strcmp(name, "rfind"));
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
                    for (int64_t k = 0; k < spaces; ++k) buf_addc(&b, ' ');
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
            return str_affix(obj, w, fill, mode);
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
            if (cnt < 0) cnt = 0;
            Buf b; buf_init(&b);
            for (int64_t i = 0; i < cnt; ++i) buf_addn(&b, s->data, (size_t)s->h.len);
            return buf_take(&b);
        }
        if (!strcmp(name, "contains")) {
            want_args(name, n, 1, 1);
            if (kind_of(argv[0]) != K_STR && kind_of(argv[0]) != K_CHAR) return a_bool(0);
            AStr *hay = (AStr *)obj_of(obj);
            AStr *needle = (AStr *)obj_of(argv[0]);
            if (needle->h.len == 0) return a_bool(1);
            if (needle->h.len > hay->h.len) return a_bool(0);
            for (uint64_t i = 0; i + needle->h.len <= hay->h.len; ++i) {
                if (memcmp(hay->data + i, needle->data, (size_t)needle->h.len) == 0)
                    return a_bool(1);
            }
            return a_bool(0);
        }
        return method_no_method(obj, name);
    }

    if (k == K_LIST || k == K_TUPLE) {
        int is_list = k == K_LIST;
        AArr *a = (AArr *)obj_of(obj);
        if (!strcmp(name, "index")) {
            want_args(name, n, 1, 3);
            int64_t st = n >= 2 ? expect_int64(argv[1], "start") : 0;
            int64_t en = n >= 3 ? expect_int64(argv[2], "end") : (int64_t)a->h.len;
            return seq_index(obj, argv[0], st, en);
        }
        if (!strcmp(name, "count")) {
            want_args(name, n, 1, 3);
            int64_t st = n >= 2 ? expect_int64(argv[1], "start") : 0;
            int64_t en = n >= 3 ? expect_int64(argv[2], "end") : (int64_t)a->h.len;
            int64_t lo = st < 0 ? st + (int64_t)a->h.len : st;
            int64_t hi = en < 0 ? en + (int64_t)a->h.len : en;
            if (lo < 0) lo = 0;
            if (hi > (int64_t)a->h.len) hi = (int64_t)a->h.len;
            int64_t c = 0;
            for (int64_t i = lo; i < hi; ++i)
                if (values_equal(a->items[i], argv[0])) ++c;
            return mkint_full(c);
        }
        if (!strcmp(name, "contains")) { want_args(name, n, 1, 1); return bool_aval(seq_contains(obj, argv[0])); }
        if (!strcmp(name, "first")) { want_args(name, n, 0, 0); if (!a->h.len) return A_NONE; return a_some_new(a->items[0]); }
        if (!strcmp(name, "last")) { want_args(name, n, 0, 0); if (!a->h.len) return A_NONE; return a_some_new(a->items[a->h.len - 1]); }
        if (!strcmp(name, "copy")) {
            want_args(name, n, 0, 0);
            return list_from_vec(a->items, a->h.len, !is_list);
        }
        if (!is_list) return method_no_method(obj, name);
        if (!strcmp(name, "append")) {
            want_args(name, n, 1, 1);
            arr_push(a, argv[0]);
            return A_NONE;
        }
        if (!strcmp(name, "extend")) {
            want_args(name, n, 1, 1);
            aval tmp = collect_iterable(argv[0]);
            AArr *e = (AArr *)obj_of(tmp);
            for (uint64_t i = 0; i < e->h.len; ++i) arr_push(a, e->items[i]);
            return A_NONE;
        }
        if (!strcmp(name, "insert")) {
            want_args(name, n, 2, 2);
            int64_t i = expect_int64(argv[0], "index");
            if (i < 0) i += (int64_t)a->h.len;
            if (i < 0) i = 0;
            if (i > (int64_t)a->h.len) i = (int64_t)a->h.len;
            arr_reserve(a, (uint64_t)a->h.len + 1);
            memmove(a->items + i + 1, a->items + i,
                    (size_t)(a->h.len - (uint64_t)i) * sizeof(aval));
            a->items[i] = argv[1];
            a->h.len++;
            return A_NONE;
        }
        if (!strcmp(name, "remove_at")) {
            want_args(name, n, 1, 1);
            int64_t i = expect_int64(argv[0], "index");
            if (i < 0) i += (int64_t)a->h.len;
            if (i < 0 || i >= (int64_t)a->h.len) panicf("remove_at index out of range");
            aval r = a->items[i];
            memmove(a->items + i, a->items + i + 1, (size_t)(a->h.len - (uint64_t)i - 1) * sizeof(aval));
            a->h.len--;
            return r;
        }
        if (!strcmp(name, "join")) {
            want_args(name, n, 1, 1);
            aval tmp = collect_iterable(obj);
            AArr *ja = (AArr *)obj_of(tmp);
            for (uint64_t i = 0; i < ja->h.len; ++i) ja->items[i] = a_str(ja->items[i]);
            return a_str_joinsep(argv[0], ja->h.len, ja->items);
        }
        if (!strcmp(name, "remove")) {
            want_args(name, n, 1, 1);
            for (uint64_t i = 0; i < a->h.len; ++i) {
                if (values_equal(a->items[i], argv[0])) {
                    memmove(a->items + i, a->items + i + 1,
                            (size_t)(a->h.len - i - 1) * sizeof(aval));
                    a->h.len--;
                    return A_NONE;
                }
            }
            panicf("Value is not in the list.");
            return A_NONE;
        }
        if (!strcmp(name, "pop")) {
            want_args(name, n, 0, 1);
            return list_pop(obj, n ? expect_int64(argv[0], "index") : 0, n != 0);
        }
        if (!strcmp(name, "clear")) { want_args(name, n, 0, 0); a->h.len = 0; return A_NONE; }
        if (!strcmp(name, "sort")) {
            want_args(name, n, 0, 0);
            sort_arr(a->items, a->h.len);
            return A_NONE;
        }
        if (!strcmp(name, "reverse")) {
            want_args(name, n, 0, 0);
            for (uint64_t i = 0; i < a->h.len / 2; ++i) {
                uint64_t j = a->h.len - 1 - i;
                aval t = a->items[i]; a->items[i] = a->items[j]; a->items[j] = t;
            }
            return A_NONE;
        }
        return method_no_method(obj, name);
    }

    if (k == K_VIEW) {
        AView *vw = (AView *)obj_of(obj);
        int64_t base;
        AArr *root = view_root(obj, &base);
        if (!strcmp(name, "append")) {
            panicf("Cannot 'append' a view.");
        }
        if (!strcmp(name, "pop")) {
            panicf("Cannot 'pop' a view.");
        }
        if (!strcmp(name, "reverse")) {
            want_args(name, n, 0, 0);
            for (int64_t i = 0; i < vw->len / 2; ++i) {
                int64_t j = vw->len - 1 - i;
                aval t = root->items[(size_t)(base + i)];
                root->items[(size_t)(base + i)] = root->items[(size_t)(base + j)];
                root->items[(size_t)(base + j)] = t;
            }
            return A_NONE;
        }
        if (!strcmp(name, "sort")) {
            want_args(name, n, 0, 0);
            aval *ptr = root->items + (size_t)base;
            sort_arr(ptr, (uint64_t)vw->len);
            return A_NONE;
        }
        if (!strcmp(name, "count")) {
            want_args(name, n, 1, 1);
            int64_t c = 0;
            for (int64_t i = 0; i < vw->len; ++i)
                if (values_equal(root->items[(size_t)(base + i)], argv[0])) ++c;
            return mkint_full(c);
        }
        if (!strcmp(name, "index")) {
            want_args(name, n, 1, 1);
            for (int64_t i = 0; i < vw->len; ++i)
                if (values_equal(root->items[(size_t)(base + i)], argv[0]))
                    return mkint_full(i);
            panicf("Value is not in the list.");
            return A_NONE;
        }
        if (!strcmp(name, "contains")) {
            want_args(name, n, 1, 1);
            for (int64_t i = 0; i < vw->len; ++i)
                if (values_equal(root->items[(size_t)(base + i)], argv[0]))
                    return A_TRUE;
            return A_FALSE;
        }
        if (!strcmp(name, "first")) {
            want_args(name, n, 0, 0);
            if (!vw->len) return A_NONE;
            return a_some_new(root->items[(size_t)base]);
        }
        if (!strcmp(name, "last")) {
            want_args(name, n, 0, 0);
            if (!vw->len) return A_NONE;
            return a_some_new(root->items[(size_t)(base + vw->len - 1)]);
        }
        if (!strcmp(name, "copy")) {
            want_args(name, n, 0, 0);
            return list_from_vec(root->items + (size_t)base, (uint64_t)vw->len, vw->is_tuple);
        }
        if (!strcmp(name, "clear")) {
            panicf("Cannot 'clear' a view.");
        }
        return method_no_method(obj, name);
    }

    if (k == K_DICT) {
        ADict *d = (ADict *)obj_of(obj);
        if (!strcmp(name, "get")) return dict_method_get(obj, n, argv);
        if (!strcmp(name, "keys")) { want_args(name, n, 0, 0); return make_keys_values_items(obj, 0); }
        if (!strcmp(name, "values")) { want_args(name, n, 0, 0); return make_keys_values_items(obj, 1); }
        if (!strcmp(name, "items")) { want_args(name, n, 0, 0); return make_keys_values_items(obj, 2); }
        if (!strcmp(name, "update")) {
            want_args(name, n, 1, 1);
            if (kind_of(argv[0]) == K_DICT) {
                ADict *o = (ADict *)obj_of(argv[0]);
                for (uint64_t i = 0; i < o->h.len; ++i) dict_put(d, o->pairs[i].k, o->pairs[i].v);
            } else {
                aval tmp = collect_iterable(argv[0]);
                AArr *a = (AArr *)obj_of(tmp);
                for (uint64_t i = 0; i < a->h.len; ++i) {
                    aval p = a->items[i];
                    if (kind_of(p) != K_LIST && kind_of(p) != K_TUPLE)
                        panicf("cannot convert dictionary update sequence element #%llu", (unsigned long long)i);
                    AArr *pa = (AArr *)obj_of(p);
                    if (pa->h.len != 2) panicf("dictionary update sequence element has length %u; 2 is required", pa->h.len);
                    dict_put(d, pa->items[0], pa->items[1]);
                }
            }
            return A_NONE;
        }
        if (!strcmp(name, "pop")) {
            want_args(name, n, 1, 2);
            int64_t i = dict_find(d, argv[0]);
            if (i < 0) {
                if (n == 2) return argv[1];
                panicf("key not found");
            }
            aval r = d->pairs[i].v;
            memmove(d->pairs + i, d->pairs + i + 1,
                    (size_t)(d->h.len - (uint64_t)i - 1) * sizeof(APair));
            d->h.len--;
            return r;
        }
        if (!strcmp(name, "clear")) { want_args(name, n, 0, 0); d->h.len = 0; return A_NONE; }
        if (!strcmp(name, "copy")) {
            want_args(name, n, 0, 0);
            aval nd = a_dict_new();
            ADict *m = (ADict *)obj_of(nd);
            for (uint64_t i = 0; i < d->h.len; ++i) dict_put(m, d->pairs[i].k, d->pairs[i].v);
            return nd;
        }
        if (!strcmp(name, "setdefault")) {
            want_args(name, n, 1, 2);
            int64_t i = dict_find(d, argv[0]);
            if (i >= 0) return d->pairs[i].v;
            aval dv = n == 2 ? argv[1] : A_NONE;
            dict_put(d, argv[0], dv);
            return dv;
        }
        if (!strcmp(name, "contains")) {
            want_args(name, n, 1, 1);
            return a_bool(dict_find(d, argv[0]) >= 0);
        }
        return method_no_method(obj, name);
    }

    if (k == K_SET) {
        AArr *s = (AArr *)obj_of(obj);
        if (!strcmp(name, "add")) { want_args(name, n, 1, 1); if (!set_contains(s, argv[0])) arr_push(s, argv[0]); return A_NONE; }
        if (!strcmp(name, "discard")) {
            want_args(name, n, 1, 1);
            for (uint64_t i = 0; i < s->h.len; ++i) {
                if (values_equal(s->items[i], argv[0])) {
                    memmove(s->items + i, s->items + i + 1, (size_t)(s->h.len - i - 1) * sizeof(aval));
                    s->h.len--;
                    return A_NONE;
                }
            }
            return A_NONE;
        }
        if (!strcmp(name, "remove")) {
            want_args(name, n, 1, 1);
            for (uint64_t i = 0; i < s->h.len; ++i) {
                if (values_equal(s->items[i], argv[0])) {
                    memmove(s->items + i, s->items + i + 1, (size_t)(s->h.len - i - 1) * sizeof(aval));
                    s->h.len--;
                    return A_NONE;
                }
            }
            panicf("value not in set");
            return A_NONE;
        }
        if (!strcmp(name, "clear")) { want_args(name, n, 0, 0); s->h.len = 0; return A_NONE; }
        if (!strcmp(name, "copy")) {
            want_args(name, n, 0, 0);
            AArr *out = (AArr *)xmalloc(sizeof(AArr));
            out->h.type = T_SET;
            out->h.len = s->h.len;
            out->h.cap = s->h.len;
            out->items = (aval *)xmalloc((size_t)(s->h.len ? s->h.len : 1) * sizeof(aval));
            if (s->h.len) memcpy(out->items, s->items, (size_t)s->h.len * sizeof(aval));
            return mkobj(&out->h);
        }
        if (!strcmp(name, "union")) { want_args(name, n, 1, 1); return method_set_op(obj, argv[0], 0); }
        if (!strcmp(name, "intersection")) { want_args(name, n, 1, 1); return method_set_op(obj, argv[0], 1); }
        if (!strcmp(name, "difference")) { want_args(name, n, 1, 1); return method_set_op(obj, argv[0], 2); }
        if (!strcmp(name, "symmetric_difference")) { want_args(name, n, 1, 1); return method_set_op(obj, argv[0], 3); }
        if (!strcmp(name, "issubset")) { want_args(name, n, 1, 1); return method_subset(obj, argv[0], 0); }
        if (!strcmp(name, "issuperset")) { want_args(name, n, 1, 1); return method_subset(obj, argv[0], 1); }
        if (!strcmp(name, "pop")) {
            want_args(name, n, 0, 0);
            if (!s->h.len) panicf("pop from an empty set");
            aval r = s->items[0];
            memmove(s->items, s->items + 1, (size_t)(s->h.len - 1) * sizeof(aval));
            s->h.len--;
            return r;
        }
        if (!strcmp(name, "contains")) {
            want_args(name, n, 1, 1);
            return a_bool(set_contains(s, argv[0]));
        }
        return method_no_method(obj, name);
    }

    return method_no_method(obj, name);
}

