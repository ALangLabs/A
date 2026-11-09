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
        /* whitespace split; words recorded as (start,end) byte spans so
           rsplit can rebuild the unstripped remainder verbatim. */
        AVec spans = { NULL, 0, 0 };
        uint64_t i = 0;
        while (i < n) {
            while (i < n && is_space_byte((unsigned char)s->data[i])) ++i;
            if (i >= n) break;
            uint64_t st = i;
            while (i < n && !is_space_byte((unsigned char)s->data[i])) ++i;
            avec_push(&spans, mkint_full((int64_t)st));
            avec_push(&spans, mkint_full((int64_t)i));
        }
        uint64_t nw = spans.len / 2;
        if (maxsplit == 0) {
            /* No splits: one item = the string with leading whitespace
               stripped (trailing/internal whitespace preserved). */
            uint64_t lead = 0;
            while (lead < n && is_space_byte((unsigned char)s->data[lead])) ++lead;
            if (lead < n) avec_push(&out, a_str_new(s->data + lead, n - lead));
            free(spans.p);
            return out;
        }
        if (rsplit && maxsplit >= 0 && nw > (uint64_t)maxsplit) {
            /* The last maxsplit separators split off as individual words;
               everything before them stays as one raw remainder (leading
               whitespace kept, separator runs consumed). */
            uint64_t m = (uint64_t)maxsplit;
            uint64_t first = nw - m;
            int64_t rend = int_val_full(spans.p[2 * (first - 1) + 1]);
            avec_push(&out, a_str_new(s->data, (uint64_t)rend));
            for (uint64_t k = first; k < nw; ++k) {
                int64_t ws = int_val_full(spans.p[2 * k]);
                int64_t we = int_val_full(spans.p[2 * k + 1]);
                avec_push(&out, a_str_new(s->data + ws, (uint64_t)(we - ws)));
            }
            free(spans.p);
            return out;
        }
        if (maxsplit < 0) maxsplit = INT64_MAX;
        /* forward split (rsplit with no effective limit is identical) */
        for (uint64_t k = 0; k < nw; ++k) {
            int64_t ws = int_val_full(spans.p[2 * k]);
            int64_t we = int_val_full(spans.p[2 * k + 1]);
            if ((int64_t)k >= maxsplit) {
                /* keep remainder as one token (raw text incl. internal
                   and trailing whitespace) */
                avec_push(&out, a_str_new(s->data + ws, n - (uint64_t)ws));
                break;
            }
            avec_push(&out, a_str_new(s->data + ws, (uint64_t)(we - ws)));
        }
        free(spans.p);
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
    uint64_t n = a->h.len;
    int64_t lo = start < 0 ? start + (int64_t)n : start;
    int64_t hi = stop < 0 ? stop + (int64_t)n : stop;
    if (lo < 0) lo = 0;
    if (hi > (int64_t)n) hi = (int64_t)n;
    if (arr_packed(a)) {
        /* Packed int64 storage: classify the needle once — a non-numeric
           needle can never match, an int needle is a bare int64 compare,
           a float needle compares as (double)e == d. */
        int64_t iv = 0;
        double dv = 0.0;
        int nc = needle_class(item, &iv, &dv);
        if (nc == A_NEEDLE_INT) {
            for (int64_t i = lo; i < hi; ++i)
                if (a->iitems[i] == iv) return mkint_full(i);
        } else if (nc == A_NEEDLE_DBL) {
            for (int64_t i = lo; i < hi; ++i)
                if ((double)a->iitems[i] == dv) return mkint_full(i);
        }
        panicf("Value is not in the list.");
        return A_NONE;
    }
    arr_ensure_aval(a);
    aval *items = a->items;
    for (int64_t i = lo; i < hi; ++i)
        if (aval_eq_fast(items[i], item)) return mkint_full(i);
    panicf("Value is not in the list.");
    return A_NONE;
}

int seq_contains(aval v, aval item) {
    AArr *a = (AArr *)obj_of(v);
    if (arr_packed(a)) {
        int64_t iv = 0;
        double dv = 0.0;
        int nc = needle_class(item, &iv, &dv);
        if (nc == A_NEEDLE_INT) {
            for (uint64_t i = 0; i < a->h.len; ++i)
                if (a->iitems[i] == iv) return 1;
        } else if (nc == A_NEEDLE_DBL) {
            for (uint64_t i = 0; i < a->h.len; ++i)
                if ((double)a->iitems[i] == dv) return 1;
        }
        return 0;
    }
    arr_ensure_aval(a);
    aval *items = a->items;
    for (uint64_t i = 0; i < a->h.len; ++i)
        if (aval_eq_fast(items[i], item)) return 1;
    return 0;
}
aval list_pop(aval v, int64_t idx, int has_idx) {
    AArr *a = (AArr *)obj_of(v);
    if (a->h.len == 0) panicf("Cannot pop from an empty array.");
    int64_t i = has_idx ? normalize_index(idx, a->h.len) : (int64_t)a->h.len - 1;
    if (arr_packed(a)) {
        /* Stay packed: popping an int from an int list must not force the
           8-byte-per-element aval conversion (same footprint, but the
           conversion itself is an O(n) alloc+copy and the array would
           only re-pack on the next arr_push_int). */
        int64_t r = a->iitems[i];
        memmove(a->iitems + i, a->iitems + i + 1,
                (size_t)(a->h.len - (uint64_t)i - 1) * sizeof(int64_t));
        a->h.len--;
        arr_shrink(a);
        return mkint_full(r);
    }
    arr_ensure_aval(a);
    aval r = a->items[i];
    memmove(a->items + i, a->items + i + 1,
            (size_t)(a->h.len - (uint64_t)i - 1) * sizeof(aval));
    a->h.len--;
    arr_shrink(a);
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

/* ---- set-op membership hash ----
   Sets are plain aval arrays, so union/intersection/difference/subset
   were O(n·m) values_equal scans. When every element on both sides is a
   "simple" scalar — numeric, str, char, bytes, bool, or none, the kinds
   whose values_equal class never crosses into container kinds — an
   open-addressing hash gives identical membership answers in O(n+m).
   Any complex element (list/tuple/view/dict/set/range/result/other)
   falls back to the quadratic scan below. The hash is only a filter:
   probes still compare with aval_eq_fast (== values_equal), so equal
   values always land in the same chain by construction of aval_hash. */

typedef struct {
    aval *keys;
    unsigned char *occ;
    uint64_t mask;   /* capacity - 1 (capacity is a power of two) */
} ASetHash;

static uint64_t sh_mix(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* Hash consistent with values_equal for simple kinds:
   - numerics hash the double value of num_to_double (int 1 and float
     1.0 must collide; int-int exactness only costs collisions, never
     wrong answers, because probes re-compare with values_equal).
     -0.0 is normalized to +0.0 since -0.0 == 0.0; NaN never equals so
     any hash is fine.
   - str/char/bytes hash their byte content.
   - bool/none hash the constant aval itself. */
static uint64_t aval_hash(aval v) {
    if (is_num(v)) {
        double d = a_is_double(v) ? a_double_val(v) : (double)int_val_full(v);
        if (d == 0.0) d = 0.0;
        uint64_t u;
        memcpy(&u, &d, sizeof u);
        return sh_mix(u);
    }
    int k = kind_of(v);
    if (k == K_STR || k == K_CHAR) {
        uint64_t n;
        const char *p = str_data(v, &n);
        uint64_t h = 0xcbf29ce484222325ULL;
        for (uint64_t i = 0; i < n; ++i) {
            h ^= (unsigned char)p[i];
            h *= 0x100000001b3ULL;
        }
        h ^= n;
        h *= 0x100000001b3ULL;
        return sh_mix(h);
    }
    if (k == K_BYTES) {
        uint64_t n;
        const char *p = a_bytes_data(v, &n);
        uint64_t h = 0xcbf29ce484222325ULL;
        for (uint64_t i = 0; i < n; ++i) {
            h ^= (unsigned char)p[i];
            h *= 0x100000001b3ULL;
        }
        h ^= n;
        h *= 0x100000001b3ULL;
        return sh_mix(h);
    }
    return sh_mix(v);
}

static int aval_simple(aval v) {
    if (is_num(v)) return 1;
    switch (kind_of(v)) {
        case K_STR: case K_CHAR: case K_BYTES:
        case K_BOOL: case K_NONE:
            return 1;
        default:
            return 0;
    }
}

static int arr_all_simple(AArr *a) {
    if (arr_packed(a)) return 0;   /* defensive: set-op inputs are never packed */
    for (uint64_t i = 0; i < a->h.len; ++i)
        if (!aval_simple(a->items[i])) return 0;
    return 1;
}

static void sethash_init(ASetHash *h, uint64_t want) {
    uint64_t cap = 16;
    while (cap < want * 2) cap <<= 1;
    h->keys = (aval *)xmalloc((size_t)cap * sizeof(aval));
    h->occ = (unsigned char *)xmalloc((size_t)cap);
    memset(h->occ, 0, (size_t)cap);
    h->mask = cap - 1;
}

static void sethash_free(ASetHash *h) {
    free(h->keys);
    free(h->occ);
}

/* Returns 1 when v was not present (and inserts it); 0 when an equal
   element was already stored. */
static int sethash_add(ASetHash *h, aval v) {
    uint64_t i = aval_hash(v) & h->mask;
    while (h->occ[i]) {
        if (aval_eq_fast(h->keys[i], v)) return 0;
        i = (i + 1) & h->mask;
    }
    h->occ[i] = 1;
    h->keys[i] = v;
    return 1;
}

static int sethash_contains(const ASetHash *h, aval v) {
    uint64_t i = aval_hash(v) & h->mask;
    while (h->occ[i]) {
        if (aval_eq_fast(h->keys[i], v)) return 1;
        i = (i + 1) & h->mask;
    }
    return 0;
}

static void sethash_fill(ASetHash *h, AArr *a) {
    for (uint64_t i = 0; i < a->h.len; ++i) sethash_add(h, a->items[i]);
}

/* Quadratic fallback shared by method_set_op/method_subset: identical
   to the original nested values_equal scans, with the scalar fast path
   inlined per comparison. */
static int set_op_in(AArr *hay, aval needle) {
    for (uint64_t j = 0; j < hay->h.len; ++j)
        if (aval_eq_fast(hay->items[j], needle)) return 1;
    return 0;
}

static int set_op_out_has(AArr *out, aval v) {
    for (uint64_t j = 0; j < out->h.len; ++j)
        if (aval_eq_fast(out->items[j], v)) return 1;
    return 0;
}

static AArr *set_out_new(uint64_t cap_elems) {
    AArr *out = (AArr *)xmalloc(sizeof(AArr));
    out->h.type = T_SET;
    out->h.len = 0;
    out->h.cap = cap_elems;
    out->items = (aval *)xmalloc((size_t)(cap_elems ? cap_elems : 1) * sizeof(aval));
    out->iitems = NULL;
    out->flags = 0;
    return out;
}

/* Release spare capacity once the result size is known (same policy the
   quadratic path used: sets are never view parents, so shrinking the
   buffer is always safe). */
static void set_out_shrink(AArr *out) {
    if (out->h.cap > out->h.len * 2 && out->h.cap - out->h.len > 16) {
        out->items = (aval *)xrealloc(out->items,
                (size_t)(out->h.len ? out->h.len : 1) * sizeof(aval));
        out->h.cap = out->h.len;
    }
}

static aval method_set_op_hash(AArr *s, AArr *o, int mode) {
    /* mode: 0 union, 1 intersection, 2 difference, 3 symmetric_difference */
    ASetHash ho, hs, hout;
    int need_s = (mode == 0 || mode == 3);
    sethash_init(&ho, o->h.len);
    sethash_init(&hout, s->h.len + o->h.len);
    if (need_s) sethash_init(&hs, s->h.len);
    sethash_fill(&ho, o);
    if (need_s) sethash_fill(&hs, s);
    AArr *out = set_out_new(s->h.len + o->h.len);
    for (uint64_t i = 0; i < s->h.len; ++i) {
        aval x = s->items[i];
        int in_o = sethash_contains(&ho, x);
        int keep = mode == 0 ? 1 : mode == 1 ? in_o : mode == 2 ? !in_o : !in_o;
        if (keep && sethash_add(&hout, x)) arr_push(out, x);
    }
    if (need_s) {
        for (uint64_t j = 0; j < o->h.len; ++j) {
            aval y = o->items[j];
            int in_s = sethash_contains(&hs, y);
            if ((mode == 0 || !in_s) && sethash_add(&hout, y))
                arr_push(out, y);
        }
    }
    sethash_free(&ho);
    sethash_free(&hout);
    if (need_s) sethash_free(&hs);
    set_out_shrink(out);
    return mkobj(&out->h);
}

aval method_set_op(aval v, aval other, int mode) {
    /* mode: 0 union, 1 intersection, 2 difference, 3 symmetric_difference */
    aval tmp = collect_iterable(other);
    AArr *o = (AArr *)obj_of(tmp);
    AArr *s = (AArr *)obj_of(v);
    if (arr_all_simple(s) && arr_all_simple(o))
        return method_set_op_hash(s, o, mode);
    AArr *out = set_out_new(s->h.len + o->h.len);
    for (uint64_t i = 0; i < s->h.len; ++i) {
        int in_o = set_op_in(o, s->items[i]);
        int keep = mode == 0 ? 1 : mode == 1 ? in_o : mode == 2 ? !in_o : (in_o ? 0 : 1);
        /* symmetric_difference keeps elements in exactly one side; first pass adds self-only */
        if (mode == 3) keep = !in_o;
        if (keep && !set_op_out_has(out, s->items[i])) arr_push(out, s->items[i]);
    }
    if (mode == 0 || mode == 3) {
        for (uint64_t j = 0; j < o->h.len; ++j) {
            int in_s = set_op_in(s, o->items[j]);
            if ((mode == 0 || !in_s) && !set_op_out_has(out, o->items[j]))
                arr_push(out, o->items[j]);
        }
    }
    /* The buffer was sized for the union upper bound (|s| + |o|); an
       intersection/difference result can be far smaller, and the slack
       would be retained for the object's whole lifetime (objects are
       never reclaimed). Release it once the result size is known. */
    set_out_shrink(out);
    return mkobj(&out->h);
}

aval method_subset(aval v, aval other, int sup) {
    aval tmp = collect_iterable(other);
    AArr *o = (AArr *)obj_of(tmp);
    AArr *s = (AArr *)obj_of(v);
    AArr *base = sup ? s : o;
    AArr *test = sup ? o : s;
    if (arr_all_simple(base) && arr_all_simple(test)) {
        ASetHash hb;
        sethash_init(&hb, base->h.len);
        sethash_fill(&hb, base);
        int ok = 1;
        for (uint64_t i = 0; i < test->h.len; ++i) {
            if (!sethash_contains(&hb, test->items[i])) { ok = 0; break; }
        }
        sethash_free(&hb);
        return bool_aval(ok);
    }
    for (uint64_t i = 0; i < test->h.len; ++i) {
        int found = 0;
        for (uint64_t j = 0; j < base->h.len; ++j)
            if (aval_eq_fast(test->items[i], base->items[j])) { found = 1; break; }
        if (!found) return A_FALSE;
    }
    return A_TRUE;
}
