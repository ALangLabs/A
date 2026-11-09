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
static void cmp_gc_hook(void);

static int eq_trackable_kind(int k) {
    return k == K_LIST || k == K_TUPLE || k == K_DICT || k == K_SET ||
           k == K_VIEW || k == K_RESULT;
}

static int values_equal_body(aval a, aval b);

/* Scalar fast path shared by values_equal and the element-wise container
   loops below: inline ints, plain doubles, and mixed int/double pairs
   decide exactly like values_equal_body's is_num branch, and identical
   tagged non-pointer avals (None/bool constants) are trivially equal —
   the same result the body's `a == b` check gives. Container/string
   pairs fall through to values_equal so the cycle guard still applies. */
static inline int ve_scalar_eq(aval a, aval b) {
    if (a_is_int(a) && a_is_int(b)) return a_int_val(a) == a_int_val(b);
    if (a_is_double(a) && a_is_double(b))
        return a_double_val(a) == a_double_val(b);
    if (a_is_int(a) && a_is_double(b))
        return (double)a_int_val(a) == a_double_val(b);
    if (a_is_double(a) && a_is_int(b))
        return a_double_val(a) == (double)a_int_val(b);
    if (a == b && A_IS_TAGGED(a) && (A_PAYLOAD(a) & 0xFULL) != 0)
        return 1;
    return values_equal(a, b);
}

int values_equal(aval a, aval b) {
    /* Scalars cannot recurse, so they never need the cycle guard: decide
       them before the kind dispatch (this is the hot path for
       index/contains/count/set-op element compares). */
    if (a_is_int(a) && a_is_int(b))
        return a_int_val(a) == a_int_val(b);
    if (a_is_double(a) && a_is_double(b))
        return a_double_val(a) == a_double_val(b);
    if (a_is_int(a) && a_is_double(b))
        return (double)a_int_val(a) == a_double_val(b);
    if (a_is_double(a) && a_is_int(b))
        return a_double_val(a) == (double)a_int_val(b);
    if (a == b && A_IS_TAGGED(a) && (A_PAYLOAD(a) & 0xFULL) != 0)
        return 1;
    int ka = kind_of(a), kb = kind_of(b);
    /* Track container pairs only; scalars/strings/bytes are leaves and
       cannot recurse. K_VIEW compares against K_LIST/K_TUPLE too, so the
       check is "both sides are trackable kinds", not ka == kb. */
    if (eq_trackable_kind(ka) && eq_trackable_kind(kb)) {
        AObj *oa = obj_of(a), *ob = obj_of(b);
        if (oa && ob) {
            cmp_gc_hook();
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
           aval elements once before the element-wise compare. Empty views
           are parentless (view_new drops the unobservable parent ref), so
           skip the ensure — there are no elements to normalize anyway. */
        if (ka == K_VIEW) {
            AView *va = (AView *)obj_of(a);
            if (va->len) arr_ensure_aval((AArr *)obj_of(va->parent));
        } else if (ka == K_LIST) {
            arr_ensure_aval((AArr *)obj_of(a));
        }
        if (kb == K_VIEW) {
            AView *vb = (AView *)obj_of(b);
            if (vb->len) arr_ensure_aval((AArr *)obj_of(vb->parent));
        } else if (kb == K_LIST) {
            arr_ensure_aval((AArr *)obj_of(b));
        }
        for (int64_t i = 0; i < la; ++i) {
            aval xa = ka == K_VIEW ? ((AArr *)obj_of(((AView *)obj_of(a))->parent))->items[(size_t)(((AView *)obj_of(a))->off + i)]
                                   : ((AArr *)obj_of(a))->items[(size_t)i];
            aval xb = kb == K_VIEW ? ((AArr *)obj_of(((AView *)obj_of(b))->parent))->items[(size_t)(((AView *)obj_of(b))->off + i)]
                                   : ((AArr *)obj_of(b))->items[(size_t)i];
            if (!ve_scalar_eq(xa, xb)) return 0;
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
            if (x->h.len != y->h.len) return 0;
            /* Two packed-int lists compare as bare int64s: values_equal
               on int elements is exactly int_val_full equality, so no
               aval boxing or per-element call is needed. */
            if (arr_packed(x) && arr_packed(y)) {
                for (uint64_t i = 0; i < x->h.len; ++i)
                    if (x->iitems[i] != y->iitems[i]) return 0;
                return 1;
            }
            /* aval + aval: read the item pointers once and inline the
               int-int compare (the dominant element kind) instead of two
               arr_elem calls per element. */
            if (!arr_packed(x) && !arr_packed(y)) {
                aval *xi = x->items, *yi = y->items;
                for (uint64_t i = 0; i < x->h.len; ++i) {
                    aval xa = xi[i], xb = yi[i];
                    if (a_is_int(xa) && a_is_int(xb)) {
                        if (a_int_val(xa) != a_int_val(xb)) return 0;
                    } else if (!ve_scalar_eq(xa, xb)) return 0;
                }
                return 1;
            }
            /* arr_elem reads packed int64 storage too, so comparing two
               packed-int lists no longer converts either to aval storage. */
            for (uint64_t i = 0; i < x->h.len; ++i)
                if (!ve_scalar_eq(arr_elem(x, i), arr_elem(y, i))) return 0;
            return 1;
        }
        case K_DICT: {
            ADict *x = (ADict *)obj_of(a), *y = (ADict *)obj_of(b);
            if (x->h.len != y->h.len) return 0;
            for (uint64_t i = 0; i < x->h.len; ++i) {
                int found = 0;
                for (uint64_t j = 0; j < y->h.len; ++j) {
                    if (ve_scalar_eq(x->pairs[i].k, y->pairs[j].k) &&
                        ve_scalar_eq(x->pairs[i].v, y->pairs[j].v)) {
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
                    if (ve_scalar_eq(x->items[i], y->items[j])) { found = 1; break; }
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

/* Cycle guard for cmp_ordered, mirroring g_eq_active above: ordering two
   cyclic lists would otherwise recurse until the C stack overflows.
   Re-encountering the same (left, right) object pair means the comparison
   has come back around a cycle; we treat that position as equal (0) and
   let the remaining elements/length decide — the same coinductive reading
   values_equal uses. Only list/tuple pairs are tracked: they are the only
   kinds cmp_ordered recurses into. */
typedef struct { const void *a, *b; } ACmpPair;
static A_RT_TLS ACmpPair *g_cmp_active;
static A_RT_TLS size_t g_cmp_active_len, g_cmp_active_cap;

/* GC root providers: the in-progress comparison pairs hold the only
   references to objects being compared; mark both guard stacks so an
   allocation mid-comparison cannot collect them. */
static void cmp_gc_roots(void) {
    a_gc_mark_ptr(g_eq_active);
    a_gc_mark_ptr(g_cmp_active);
}
static int g_cmp_gc_hooked;
static void cmp_gc_hook(void) {
    if (!g_cmp_gc_hooked) {
        g_cmp_gc_hooked = 1;
        a_gc_add_root_cb(cmp_gc_roots);
    }
}

static int cmp_ordered(aval a, aval b, const char *op);

static int cmp_ordered_seq(aval a, aval b, const char *op) {
    AArr *x = (AArr *)obj_of(a), *y = (AArr *)obj_of(b);
    AObj *oa = (AObj *)x, *ob = (AObj *)y;
    cmp_gc_hook();
    for (size_t i = 0; i < g_cmp_active_len; ++i) {
        if (g_cmp_active[i].a == (const void *)oa &&
            g_cmp_active[i].b == (const void *)ob)
            return 0; /* cycle: treat this position as equal */
    }
    if (g_cmp_active_len == g_cmp_active_cap) {
        g_cmp_active_cap = g_cmp_active_cap ? g_cmp_active_cap * 2 : 64;
        g_cmp_active = (ACmpPair *)xrealloc(g_cmp_active,
                                            g_cmp_active_cap * sizeof(ACmpPair));
    }
    g_cmp_active[g_cmp_active_len].a = (const void *)oa;
    g_cmp_active[g_cmp_active_len].b = (const void *)ob;
    g_cmp_active_len++;
    int r = -2; /* sentinel: not yet computed */
    /* arr_elem handles packed int64 storage, so ordering two packed-int
       lists does not force either to aval storage first. Two packed
       lists compare as bare int64s — identical to cmp_ordered's int
       branch — so skip the per-element call entirely. */
    uint64_t n = x->h.len < y->h.len ? x->h.len : y->h.len;
    if (arr_packed(x) && arr_packed(y)) {
        for (uint64_t i = 0; i < n; ++i) {
            int64_t p = x->iitems[i], q = y->iitems[i];
            if (p != q) { r = p < q ? -1 : 1; goto seq_done; }
        }
        r = -2;
    seq_done:
        if (r == -2)
            r = x->h.len < y->h.len ? -1 : x->h.len > y->h.len ? 1 : 0;
        g_cmp_active_len--;
        return r;
    }
    for (uint64_t i = 0; i < n; ++i) {
        aval xa = arr_elem(x, i), xb = arr_elem(y, i);
        int c;
        /* Inline the numeric fast path: sequence compares of int/float
           elements are the hot case and would otherwise pay a
           cmp_ordered call per element. Identical results to
           cmp_ordered's is_num branch. */
        if (a_is_int(xa) && a_is_int(xb)) {
            int64_t p = a_int_val(xa), q = a_int_val(xb);
            c = p < q ? -1 : p > q ? 1 : 0;
        } else if (a_is_double(xa) && a_is_double(xb)) {
            double p = a_double_val(xa), q = a_double_val(xb);
            c = p < q ? -1 : p > q ? 1 : 0;
        } else {
            c = cmp_ordered(xa, xb, op);
        }
        if (c) { r = c; break; }
    }
    if (r == -2)
        r = x->h.len < y->h.len ? -1 : x->h.len > y->h.len ? 1 : 0;
    g_cmp_active_len--;
    return r;
}

static int cmp_ordered(aval a, aval b, const char *op) {
    /* Inline int/double fast paths: identical to the is_num branch below
       for the cases a_lt/a_le/a_gt/a_ge's header fast paths miss (boxed
       ints still fall through to is_num). */
    if (a_is_int(a) && a_is_int(b)) {
        int64_t x = a_int_val(a), y = a_int_val(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (a_is_double(a) && a_is_double(b)) {
        double x = a_double_val(a), y = a_double_val(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
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
    /* Sequences compare lexicographically (element-wise, then by length).
       cmp_ordered_seq carries the cycle guard. */
    {
        int kaa = kind_of(a), kbb = kind_of(b);
        if ((kaa == K_LIST || kaa == K_TUPLE) &&
            (kbb == K_LIST || kbb == K_TUPLE))
            return cmp_ordered_seq(a, b, op);
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

