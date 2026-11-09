#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* equality / ordering                                                 */
/* ------------------------------------------------------------------ */

/* Cycle guard for values_equal. Structural equality recurses over the
   value graph; two distinct but cyclic objects (e.g. two class instances
   whose fields point back to themselves, or isomorphic cyclic lists)
   would otherwise recurse until the C stack overflows. g_eq_active is a
   per-thread stack (A_RT_TLS: comparisons may run on pool workers) of
   the (left, right) object-pointer pairs currently being compared.
   Re-encountering a pair means the comparison has come back around a
   cycle; we treat it as equal — the coinductive reading of structural
   equality ("the subtrees are equal if their unfoldings match"), which
   is what makes `a == b` true for two isomorphic cyclic graphs while
   `a == c` stays false when the cycle shapes differ. This is deliberately
   simpler than a depth cap: deep acyclic comparisons keep working and
   only true cycles take the shortcut. panicf() exits the process, so a
   panic mid-comparison cannot leave stale entries behind. */
typedef struct { const void *a, *b; } AEqPair;
static A_RT_TLS AEqPair *g_eq_active;
static A_RT_TLS size_t g_eq_active_len, g_eq_active_cap;

static int eq_trackable_kind(int k) {
    return k == K_LIST || k == K_TUPLE || k == K_DICT || k == K_SET ||
           k == K_VIEW || k == K_RESULT;
}

static int values_equal_body(aval a, aval b);

int values_equal(aval a, aval b) {
    int ka = kind_of(a), kb = kind_of(b);
    /* Track container pairs only; scalars/strings/bytes are leaves and
       cannot recurse. K_VIEW compares against K_LIST/K_TUPLE too, so the
       check is "both sides are trackable kinds", not ka == kb. */
    if (eq_trackable_kind(ka) && eq_trackable_kind(kb)) {
        AObj *oa = obj_of(a), *ob = obj_of(b);
        if (oa && ob) {
            for (size_t i = 0; i < g_eq_active_len; ++i) {
                if (g_eq_active[i].a == (const void *)oa &&
                    g_eq_active[i].b == (const void *)ob)
                    return 1; /* cycle: coinductive equality */
            }
            if (g_eq_active_len == g_eq_active_cap) {
                g_eq_active_cap = g_eq_active_cap ? g_eq_active_cap * 2 : 64;
                g_eq_active = (AEqPair *)xrealloc(g_eq_active,
                                                  g_eq_active_cap * sizeof(AEqPair));
            }
            g_eq_active[g_eq_active_len].a = (const void *)oa;
            g_eq_active[g_eq_active_len].b = (const void *)ob;
            g_eq_active_len++;
            int r = values_equal_body(a, b);
            g_eq_active_len--;
            return r;
        }
    }
    return values_equal_body(a, b);
}

static int values_equal_body(aval a, aval b) {
    int ka = kind_of(a), kb = kind_of(b);
    if (is_num(a) && is_num(b)) {
        if (is_int_val(a) && is_int_val(b))
            return int_val_full(a) == int_val_full(b);
        return num_to_double(a) == num_to_double(b);
    }
    if (a == b) return 1;
    if (ka == K_BYTES || kb == K_BYTES) {
        ABytes *xa = (ABytes *)obj_of(a), *xb = (ABytes *)obj_of(b);
        if (ka != K_BYTES || kb != K_BYTES) return 0;
        if (xa->h.len != xb->h.len) return 0;
        return memcmp(xa->data, xb->data, (size_t)xa->h.len) == 0;
    }
    /* Views compare as the underlying slice (view == list if elements match). */
    if (ka == K_VIEW || kb == K_VIEW) {
        if (ka != K_VIEW && ka != K_LIST && ka != K_TUPLE) return 0;
        if (kb != K_VIEW && kb != K_LIST && kb != K_TUPLE) return 0;
        int64_t la = ka == K_VIEW ? ((AView *)obj_of(a))->len : (int64_t)((AArr *)obj_of(a))->h.len;
        int64_t lb = kb == K_VIEW ? ((AView *)obj_of(b))->len : (int64_t)((AArr *)obj_of(b))->h.len;
        if (la != lb) return 0;
        /* Views may alias packed-int storage: normalize both sides to
           aval elements once before the element-wise compare. */
        if (ka == K_VIEW)
            arr_ensure_aval((AArr *)obj_of(((AView *)obj_of(a))->parent));
        else if (ka == K_LIST)
            arr_ensure_aval((AArr *)obj_of(a));
        if (kb == K_VIEW)
            arr_ensure_aval((AArr *)obj_of(((AView *)obj_of(b))->parent));
        else if (kb == K_LIST)
            arr_ensure_aval((AArr *)obj_of(b));
        for (int64_t i = 0; i < la; ++i) {
            aval xa = ka == K_VIEW ? ((AArr *)obj_of(((AView *)obj_of(a))->parent))->items[(size_t)(((AView *)obj_of(a))->off + i)]
                                   : ((AArr *)obj_of(a))->items[(size_t)i];
            aval xb = kb == K_VIEW ? ((AArr *)obj_of(((AView *)obj_of(b))->parent))->items[(size_t)(((AView *)obj_of(b))->off + i)]
                                   : ((AArr *)obj_of(b))->items[(size_t)i];
            if (!values_equal(xa, xb)) return 0;
        }
        return 1;
    }
    if (ka != kb) return 0;
    if (ka == K_OTHER) {
        AObj *oa = obj_of(a), *ob = obj_of(b);
        if (oa && ob && oa->type == T_PYOBJ && ob->type == T_PYOBJ)
            return a_py_equals(a, b);
        if (oa && ob && oa->type == T_PYBUF && ob->type == T_PYBUF) {
            APyBuf *x = (APyBuf *)oa, *y = (APyBuf *)ob;
            if (x->count != y->count || x->kind != y->kind ||
                x->itemsize != y->itemsize)
                return 0;
            for (int64_t i = 0; i < x->count; ++i)
                if (!values_equal(pybuf_read(x, i), pybuf_read(y, i))) return 0;
            return 1;
        }
        return 0;
    }
    switch (ka) {
        case K_BOOL: return 0;
        case K_STR: return str_eq(a, b);
        case K_CHAR: return str_eq(a, b);
        case K_LIST:
        case K_TUPLE: {
            AArr *x = (AArr *)obj_of(a), *y = (AArr *)obj_of(b);
            arr_ensure_aval(x);
            arr_ensure_aval(y);
            if (x->h.len != y->h.len) return 0;
            for (uint64_t i = 0; i < x->h.len; ++i)
                if (!values_equal(x->items[i], y->items[i])) return 0;
            return 1;
        }
        case K_DICT: {
            ADict *x = (ADict *)obj_of(a), *y = (ADict *)obj_of(b);
            if (x->h.len != y->h.len) return 0;
            for (uint64_t i = 0; i < x->h.len; ++i) {
                int found = 0;
                for (uint64_t j = 0; j < y->h.len; ++j) {
                    if (values_equal(x->pairs[i].k, y->pairs[j].k) &&
                        values_equal(x->pairs[i].v, y->pairs[j].v)) {
                        found = 1;
                        break;
                    }
                }
                if (!found) return 0;
            }
            return 1;
        }
        case K_SET: {
            AArr *x = (AArr *)obj_of(a), *y = (AArr *)obj_of(b);
            if (x->h.len != y->h.len) return 0;
            for (uint64_t i = 0; i < x->h.len; ++i) {
                int found = 0;
                for (uint64_t j = 0; j < y->h.len; ++j)
                    if (values_equal(x->items[i], y->items[j])) { found = 1; break; }
                if (!found) return 0;
            }
            return 1;
        }
        case K_RANGE: {
            ARange *x = (ARange *)obj_of(a), *y = (ARange *)obj_of(b);
            return x->lo == y->lo && x->hi == y->hi && x->step == y->step;
        }
        case K_RESULT: {
            AResult *x = (AResult *)obj_of(a), *y = (AResult *)obj_of(b);
            return x->ok == y->ok && x->is_option == y->is_option && values_equal(x->value, y->value);
        }
        default: return 0;
    }
}

aval a_eq_slow(aval l, aval r) { return bool_aval(values_equal(l, r)); }
aval a_ne_slow(aval l, aval r) { return bool_aval(!values_equal(l, r)); }

static int cmp_ordered(aval a, aval b, const char *op) {
    if (is_num(a) && is_num(b)) {
        if (is_int_val(a) && is_int_val(b)) {
            int64_t x = int_val_full(a), y = int_val_full(b);
            return x < y ? -1 : x > y ? 1 : 0;
        }
        double x = num_to_double(a), y = num_to_double(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (kind_of(a) == K_STR && kind_of(b) == K_STR) {
        uint64_t la, lb;
        const char *pa = str_data(a, &la), *pb = str_data(b, &lb);
        size_t n = la < lb ? (size_t)la : (size_t)lb;
        int c = n ? memcmp(pa, pb, n) : 0;
        if (c) return c < 0 ? -1 : 1;
        return la < lb ? -1 : la > lb ? 1 : 0;
    }
    /* Chars order like their single-codepoint strings. */
    if (kind_of(a) == K_CHAR && kind_of(b) == K_CHAR) {
        uint64_t la, lb;
        const char *pa = str_data(a, &la), *pb = str_data(b, &lb);
        size_t n = la < lb ? (size_t)la : (size_t)lb;
        int c = n ? memcmp(pa, pb, n) : 0;
        if (c) return c < 0 ? -1 : 1;
        return la < lb ? -1 : la > lb ? 1 : 0;
    }
    /* Sequences compare lexicographically (element-wise, then by length). */
    {
        int kaa = kind_of(a), kbb = kind_of(b);
        if ((kaa == K_LIST || kaa == K_TUPLE) &&
            (kbb == K_LIST || kbb == K_TUPLE)) {
            AArr *x = (AArr *)obj_of(a), *y = (AArr *)obj_of(b);
            arr_ensure_aval(x);
            arr_ensure_aval(y);
            uint64_t n = x->h.len < y->h.len ? x->h.len : y->h.len;
            for (uint64_t i = 0; i < n; ++i) {
                int c = cmp_ordered(x->items[i], y->items[i], op);
                if (c) return c;
            }
            return x->h.len < y->h.len ? -1 : x->h.len > y->h.len ? 1 : 0;
        }
    }
    if (kind_of(a) == K_BOOL && kind_of(b) == K_BOOL) {
        int x = a == A_TRUE, y = b == A_TRUE;
        return x < y ? -1 : x > y ? 1 : 0;
    }
    panicf("unsupported operand type(s) for %s: '%s' and '%s'", op,
           type_name_cstr(a), type_name_cstr(b));
    return 0;
}

aval a_lt_slow(aval l, aval r) { return bool_aval(cmp_ordered(l, r, "<") < 0); }
aval a_le_slow(aval l, aval r) { return bool_aval(cmp_ordered(l, r, "<=") <= 0); }
aval a_gt_slow(aval l, aval r) { return bool_aval(cmp_ordered(l, r, ">") > 0); }
aval a_ge_slow(aval l, aval r) { return bool_aval(cmp_ordered(l, r, ">=") >= 0); }

