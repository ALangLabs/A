#include "methods_internal.h"

aval method_no_method(aval obj, const char *name) {
    panicf("'%s' object has no method '%s'", type_name_cstr(obj), name);
    return A_NONE;
}

aval str_case_map(aval v, int mode) {
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

int is_space_byte(unsigned char c) { return isspace(c) != 0; }

aval str_strip_impl(aval v, int left, int right) {
    AStr *s = (AStr *)obj_of(v);
    uint32_t a = 0, b = s->h.len;
    if (left) while (a < b && is_space_byte((unsigned char)s->data[a])) ++a;
    if (right) while (b > a && is_space_byte((unsigned char)s->data[b - 1])) --b;
    return a_str_new(s->data + a, (uint64_t)(b - a));
}

AVec str_split_impl(aval v, aval sep, int64_t maxsplit, int rsplit) {
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
        avec_push(&occ, mkint_full((int64_t)at));
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
        uint64_t at = (uint64_t)int_val_full(occ.p[k]);
        avec_push(&out, a_str_new(s->data + start, at - start));
        start = at + sn;
    }
    avec_push(&out, a_str_new(s->data + start, n - start));
    free(occ.p);
    return out;
}

aval str_replace(aval v, aval oldv, aval newv, int64_t count) {
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

int str_pred(aval v, int pred) {
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

aval str_affix(aval v, int64_t width, aval fillv, int mode) {
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

aval seq_index(aval v, aval item, int64_t start, int64_t stop) {
    AArr *a = (AArr *)obj_of(v);
    arr_ensure_aval(a);
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

int seq_contains(aval v, aval item) {
    AArr *a = (AArr *)obj_of(v);
    arr_ensure_aval(a);
    for (uint64_t i = 0; i < a->h.len; ++i)
        if (values_equal(a->items[i], item)) return 1;
    return 0;
}
aval list_pop(aval v, int64_t idx, int has_idx) {
    AArr *a = (AArr *)obj_of(v);
    arr_ensure_aval(a);
    if (a->h.len == 0) panicf("Cannot pop from an empty array.");
    int64_t i = has_idx ? normalize_index(idx, a->h.len) : (int64_t)a->h.len - 1;
    aval r = a->items[i];
    memmove(a->items + i, a->items + i + 1,
            (size_t)(a->h.len - (uint64_t)i - 1) * sizeof(aval));
    a->h.len--;
    return r;
}

aval dict_method_get(aval v, uint64_t n, aval *argv) {
    want_args("get", n, 1, 2);
    return a_dict_get(v, argv[0], n == 2 ? argv[1] : A_NONE);
}

aval make_keys_values_items(aval v, int mode) {
    ADict *d = (ADict *)obj_of(v);
    AArr *out = (AArr *)xmalloc(sizeof(AArr));
    out->h.type = T_LIST;
    out->h.len = 0;
    out->h.cap = d->h.len;
    out->items = (aval *)xmalloc((size_t)(d->h.len ? d->h.len : 1) * sizeof(aval));
    out->iitems = NULL;
    out->flags = 0;
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

aval method_set_op(aval v, aval other, int mode) {
    /* mode: 0 union, 1 intersection, 2 difference, 3 symmetric_difference */
    aval tmp = collect_iterable(other);
    AArr *o = (AArr *)obj_of(tmp);
    AArr *s = (AArr *)obj_of(v);
    AArr *out = (AArr *)xmalloc(sizeof(AArr));
    out->h.type = T_SET;
    out->h.len = 0;
    out->h.cap = s->h.len + o->h.len;
    out->items = (aval *)xmalloc((size_t)(out->h.cap ? out->h.cap : 1) * sizeof(aval));
    out->iitems = NULL;
    out->flags = 0;
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

aval method_subset(aval v, aval other, int sup) {
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
