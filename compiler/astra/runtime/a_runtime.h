/* Astra A runtime: value model + public runtime API.
 *
 * Every A value is a 64-bit NaN-boxed word (aval):
 *
 *   - "plain" doubles: any bit pattern whose top 16 bits are NOT 0xFFFF
 *     (this includes all normal doubles; only negative-NaN payloads collide,
 *     which A arithmetic never produces).
 *   - tagged values:   top 16 bits == 0xFFFF. The low 48 bits (payload) are:
 *        payload & 1        -> inline integer (47-bit signed, payload>>1)
 *        payload & 0xF == 0 -> pointer to a heap object (16-byte aligned)
 *        otherwise          -> small constants (A_NONE/A_FALSE/A_TRUE)
 *
 * Heap objects share a 16-byte header:
 *   struct AObj { uint32_t type; uint32_t len; uint64_t cap; }
 * followed by type-specific payload. Object types are internal to
 * a_runtime.c; generated code never inspects them.
 *
 * Functions: every A function (declaration or lambda) is emitted with the
 * uniform signature  aval f(aval *argv)  where argv[0] is the closure env
 * (A_NONE for plain functions) and argv[1..n] are the parameters.
 * Builtins are FNON objects flagged builtin; a_call dispatches them to host
 * functions of signature  aval (*)(uint64_t n, aval *argv).
 */
#ifndef ASTRA_A_RUNTIME_H
#define ASTRA_A_RUNTIME_H

#include <stdint.h>
#include <stddef.h>
#ifndef INT64_MAX
#define INT64_MAX 0x7fffffffffffffffLL
#endif
#ifndef INT64_MIN
#define INT64_MIN (-INT64_MAX-1LL)
#endif

typedef uint64_t aval;
typedef aval (*a_builtin_fn)(uint64_t n, aval *argv);

#define A_TAG        0xFFFF000000000000ULL
#define A_TAG_MASK   0xFFFF000000000000ULL
#define A_PAYLOAD(v) ((v) & 0x0000FFFFFFFFFFFFULL)
#define A_IS_TAGGED(v) ((((v) & A_TAG_MASK) == A_TAG))

/* small constants: even payloads with a nonzero low nibble (never a pointer,
 * never an inline int which requires bit0 == 1) */
#define A_NONE   (A_TAG | 0x2ULL)
#define A_FALSE  (A_TAG | 0x4ULL)
#define A_TRUE   (A_TAG | 0x6ULL)

/* inline int: 47-bit signed payload (payload>>1, sign-extended from bit 46) */
static inline aval a_mkint(int64_t i) {
    uint64_t p = (((uint64_t)i) << 1) | 1ULL;
    return A_TAG | (p & 0x0000FFFFFFFFFFFFULL);
}
static inline int a_is_int(aval v) { return A_IS_TAGGED(v) && (v & 1ULL); }
static inline int64_t a_int_val(aval v) {
    uint64_t p = A_PAYLOAD(v) >> 1;
    int64_t r = (int64_t)p;
    if (r & (1LL << 46)) r |= ~((1LL << 47) - 1);
    return r;
}
static inline aval a_mkdouble(double d) {
    union { double d; uint64_t u; } c;
    c.d = d;
    return c.u; /* may collide with tag space only for negative NaN payloads */
}
static inline int a_is_double(aval v) { return !A_IS_TAGGED(v); }
static inline double a_double_val(aval v) {
    union { uint64_t u; double d; } c;
    c.u = v;
    return c.d;
}
static inline int a_is_ptr(aval v) {
    return A_IS_TAGGED(v) && (A_PAYLOAD(v) & 0xFULL) == 0 && A_PAYLOAD(v) != 0;
}
static inline int a_is_none(aval v) { return v == A_NONE; }

/* builtin function ids (order is ABI: sema/codegen reference these) */
enum {
    A_BI_PRINT = 0, A_BI_EPRINT, A_BI_LEN, A_BI_STR, A_BI_INT, A_BI_FLOAT,
    A_BI_BOOL, A_BI_ABS, A_BI_MIN, A_BI_MAX, A_BI_SUM, A_BI_POW,
    A_BI_SQRT, A_BI_SIN, A_BI_COS, A_BI_TAN, A_BI_ASIN, A_BI_ACOS, A_BI_ATAN,
    A_BI_EXP, A_BI_LOG, A_BI_FLOOR, A_BI_CEIL, A_BI_ROUND, A_BI_TRUNC,
    A_BI_RANDOM, A_BI_RANDOM_INT, A_BI_RANGE, A_BI_SET, A_BI_TUPLE,
    A_BI_LIST, A_BI_DICT, A_BI_MAP, A_BI_FILTER, A_BI_ANY, A_BI_ALL,
    A_BI_SOME, A_BI_OK, A_BI_ERR, A_BI_PANIC, A_BI_ASSERT, A_BI_ASSERT_EQ,
    A_BI_INPUT, A_BI_READ_FILE, A_BI_WRITE_FILE, A_BI_APPEND_FILE,
    A_BI_FILE_EXISTS, A_BI_LIST_DIR, A_BI_REMOVE_FILE, A_BI_CREATE_DIR,
    A_BI_REMOVE_DIR, A_BI_RENAME, A_BI_CURRENT_DIR, A_BI_REPR, A_BI_TYPE,
    A_BI_ISINSTANCE, A_BI_JOIN, A_BI_SORTED, A_BI_REVERSED, A_BI_ENUMERATE,
    A_BI_ZIP, A_BI_REDUCE, A_BI_LOG2, A_BI_LOG10, A_BI_ATAN2, A_BI_FLUSH,
    A_BI_I8, A_BI_I16, A_BI_I32, A_BI_U8, A_BI_U16, A_BI_U32, A_BI_F32,
    /* LLVM-style intrinsics: bit ops (ctz/clz/popcount/bswap/rotl/rotr),
       float ops (fma/fabs/copysign), integer min/max (smin/smax/umin/umax),
       overflowing arithmetic, and branch-hint expect/likely/unlikely. */
    A_BI_CTZ, A_BI_CLZ, A_BI_POPCOUNT, A_BI_BSWAP, A_BI_ROTL, A_BI_ROTR,
    A_BI_FMA, A_BI_FABS, A_BI_COPYSIGN,
    A_BI_SMIN, A_BI_SMAX, A_BI_UMIN, A_BI_UMAX,
    A_BI_SADD_OVF, A_BI_SSUB_OVF, A_BI_SMUL_OVF,
    A_BI_UADD_OVF, A_BI_USUB_OVF, A_BI_UMUL_OVF,
    A_BI_EXPECT, A_BI_LIKELY, A_BI_UNLIKELY,
    /* native-module members with no core builtin equivalent */
    A_BI_ENV_GET,
    /* time module */
    A_BI_TIME_NOW, A_BI_TIME_SLEEP,
    /* terminal module (POSIX only) */
    A_BI_TERMINAL_ISATTY, A_BI_TERMINAL_RAW_MODE, A_BI_TERMINAL_READ_CHAR,
    A_BI_TERMINAL_READ_CHAR_MS, A_BI_TERMINAL_WRITE, A_BI_TERMINAL_FLUSH,
    A_BI_TERMINAL_SIZE, A_BI_TERMINAL_HOME,
    /* repl.eval: evaluate A source through the `a` toolchain (A_REPL_DEPTH
       guards against nested evaluation; A_A_BIN locates the driver). */
    A_BI_REPL_EVAL,
    /* json module: JSON text <-> aval values (dict/list/str/num/bool/none) */
    A_BI_JSON_PARSE, A_BI_JSON_STRINGIFY, A_BI_JSON_PRETTY, A_BI_JSON_IS_VALID,
    A_BI_JSON_GET, A_BI_JSON_AT, A_BI_JSON_LEN, A_BI_JSON_HAS, A_BI_JSON_KEYS,
    A_BI_JSON_TYPE, A_BI_JSON_AS_STR, A_BI_JSON_AS_INT, A_BI_JSON_AS_FLOAT,
    A_BI_JSON_AS_BOOL, A_BI_JSON_AS_ARRAY, A_BI_JSON_AS_MAP,
    /* repl.quiet: suppress print/eprint output (used to replay session
       history silently inside repl.eval'd code). */
    A_BI_REPL_QUIET,
    /* binary module: tagged-value <-> byte-array codec */
    A_BI_BINARY_ENCODE, A_BI_BINARY_DECODE,
    /* log module: leveled logging with optional JSON fields */
    A_BI_LOG_SET_LEVEL, A_BI_LOG_SET_JSON,
    A_BI_LOG_DEBUG, A_BI_LOG_INFO, A_BI_LOG_WARN, A_BI_LOG_ERROR,
    /* metrics module: counters/gauges/histograms + text render */
    A_BI_METRICS_COUNTER, A_BI_METRICS_GAUGE, A_BI_METRICS_HISTOGRAM,
    A_BI_METRICS_RENDER, A_BI_METRICS_RESET,
    /* trace module: in-process span tracking */
    A_BI_TRACE_START, A_BI_TRACE_END, A_BI_TRACE_CURRENT,
    A_BI_TRACE_SET_ATTRIBUTE, A_BI_TRACE_ADD_EVENT,
    A_BI_TRACE_INJECT, A_BI_TRACE_EXTRACT, A_BI_TRACE_SPANS,
    A_BI_TRACE_RESET,
    /* net module: name resolution + TLS availability probe */
    A_BI_NET_RESOLVE, A_BI_NET_TLS_AVAILABLE,
    /* arena module: bump/pool allocators */
    A_BI_ARENA_NEW, A_BI_ARENA_POOL, A_BI_ARENA_REGION,
    A_BI_ARENA_ALLOC, A_BI_ARENA_RELEASE, A_BI_ARENA_RESET,
    A_BI_ARENA_USED, A_BI_ARENA_TOTAL,
    /* unsafe module: raw-ish container/memory access */
    A_BI_UNSAFE_GET, A_BI_UNSAFE_SET, A_BI_UNSAFE_ADDRESS_OF,
    A_BI_UNSAFE_LOAD, A_BI_UNSAFE_STORE, A_BI_UNSAFE_OFFSET,
    /* python interop: argv = [module:str, member:str, args...] */
    A_BI_PY_CALL,
    /* python interop: zero-copy buffer-protocol view over a PyObject */
    A_BI_PY_BUFFER,
    A_BI_PY_IMPORT_MODULE,
    A_BI_PY_TRY_IMPORT_MODULE,
    A_BI_PY_GETATTR,
    A_BI_PY_TRY_GETATTR,
    A_BI_PY_SETATTR,
    A_BI_PY_HASATTR,
    A_BI_PY_DELATTR,
    A_BI_PY_GETITEM,
    A_BI_PY_SETITEM,
    A_BI_PY_DELITEM,
    A_BI_PY_CALL_OBJ,
    A_BI_PY_TRY_CALL_OBJ,
    A_BI_PY_CALL_METHOD,
    A_BI_PY_TRY_CALL_METHOD,
    A_BI_PY_IS_PYOBJECT,
    A_BI_PY_STR,
    A_BI_PY_REPR,
    A_BI_PY_LEN,
    A_BI_PY_CONTAINS,
    A_BI_PY_TRUTHY,
    A_BI_PY_TYPE,
    A_BI_PY_BUFFER_INFO,
    A_BI_COUNT
};

/* ---- runtime entry points (implemented in a_runtime.c) ----
 * All return aval unless noted. n/argv-style helpers take (uint64_t, aval*). */

/* value predicates / conversion */
aval a_truthy_raw_slow(aval v);     /* returns 0 or 1 (plain u64) */
aval a_not(aval v);                 /* logical not -> A_TRUE/A_FALSE */
aval a_neg_slow(aval v);            /* unary - */
aval a_bnot_slow(aval v);           /* ~ */

/* inline int limit: inline ints cover [-2^46, 2^46). */
#define A_INT_LIM ((int64_t)(INT64_C(1) << 46))
static inline int a_in_inline_range(int64_t v) { return v >= -A_INT_LIM && v < A_INT_LIM; }
static inline aval a_bool_inline(int b) { return b ? A_TRUE : A_FALSE; }
static inline aval a_double_canon(double d) {
    /* Mirror mkdouble() in the .c: canonicalize NaN so its bit pattern
       never collides with the tagged space. */
    if (d != d) return (aval)0x7FF8000000000000ULL;
    return a_mkdouble(d);
}
static inline aval a_truthy_raw(aval v) {
    if (a_is_int(v)) return (aval)(a_int_val(v) != 0);
    if (a_is_double(v)) return (aval)(a_double_val(v) != 0.0);
    if (v == A_NONE) return (aval)0;
    if (v == A_TRUE) return (aval)1;
    if (v == A_FALSE) return (aval)0;
    return a_truthy_raw_slow(v);
}
static inline aval a_neg(aval v) {
    if (a_is_int(v)) {
        int64_t i = a_int_val(v);
        if (i != -A_INT_LIM) return a_mkint(-i);
    }
    return a_neg_slow(v);
}
static inline aval a_bnot(aval v) {
    if (a_is_int(v)) {
        int64_t r = ~a_int_val(v);
        if (a_in_inline_range(r)) return a_mkint(r);
    }
    return a_bnot_slow(v);
}
aval a_str(aval v);                 /* str(v): display format */
aval a_repr(aval v);                /* repr(v): quoted strings */
aval a_len(aval v);                 /* len(v) */
aval a_int(aval v);                 /* int(v) */
aval a_float(aval v);               /* float(v) */
aval a_bool(aval v);                /* bool(v) */
aval a_type_name(aval v);           /* type name string */

/* arithmetic / comparison (richly typed, with header fast paths) */
aval a_add_slow(aval l, aval r);
aval a_sub_slow(aval l, aval r);
aval a_mul_slow(aval l, aval r);
aval a_div_slow(aval l, aval r);
aval a_mod_slow(aval l, aval r);
aval a_pow(aval l, aval r);
aval a_eq_slow(aval l, aval r);
aval a_ne_slow(aval l, aval r);
aval a_lt_slow(aval l, aval r);
aval a_le_slow(aval l, aval r);
aval a_gt_slow(aval l, aval r);
aval a_ge_slow(aval l, aval r);

static inline aval a_add(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t a = a_int_val(l), b = a_int_val(r), res;
        if (!((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))) {
            res = a + b;
            if (a_in_inline_range(res)) return a_mkint(res);
        }
        return a_add_slow(l, r);
    }
    if (a_is_double(l) && a_is_double(r))
        return a_double_canon(a_double_val(l) + a_double_val(r));
    return a_add_slow(l, r);
}
static inline aval a_sub(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t a = a_int_val(l), b = a_int_val(r), res;
        if (!((b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b))) {
            res = a - b;
            if (a_in_inline_range(res)) return a_mkint(res);
        }
        return a_sub_slow(l, r);
    }
    if (a_is_double(l) && a_is_double(r))
        return a_double_canon(a_double_val(l) - a_double_val(r));
    return a_sub_slow(l, r);
}
static inline aval a_mul(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t a = a_int_val(l), b = a_int_val(r);
        if (a == 0 || b == 0) return a_mkint(0);
        /* Check overflow before multiplying (avoid signed-overflow UB):
           require |a| <= A_INT_LIM/|b| so the product stays in range. */
        int64_t ub = b < 0 ? -b : b;
        int64_t ua = a < 0 ? -a : a;
        if (ua <= A_INT_LIM / ub) {
            int64_t res = a * b;
            if (a_in_inline_range(res)) return a_mkint(res);
        }
        return a_mul_slow(l, r);
    }
    if (a_is_double(l) && a_is_double(r))
        return a_double_canon(a_double_val(l) * a_double_val(r));
    return a_mul_slow(l, r);
}
static inline aval a_div(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t a = a_int_val(l), b = a_int_val(r);
        if (b != 0 && !(a == INT64_MIN && b == -1)) {
            int64_t res = a / b;
            if (a_in_inline_range(res)) return a_mkint(res);
        }
        return a_div_slow(l, r);
    }
    if (a_is_double(l) && a_is_double(r)) {
        double b = a_double_val(r);
        if (b == 0.0) return a_div_slow(l, r);
        return a_double_canon(a_double_val(l) / b);
    }
    return a_div_slow(l, r);
}
static inline aval a_mod(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t a = a_int_val(l), b = a_int_val(r);
        if (b != 0 && b != -1) {
            int64_t res = a % b;
            if (a_in_inline_range(res)) return a_mkint(res);
        } else if (b == -1) {
            return a_mkint(0);
        }
        return a_mod_slow(l, r);
    }
    return a_mod_slow(l, r);
}
static inline aval a_eq(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r))
        return a_bool_inline(a_int_val(l) == a_int_val(r));
    if (a_is_double(l) && a_is_double(r))
        return a_bool_inline(a_double_val(l) == a_double_val(r));
    if (l == r && (l == A_TRUE || l == A_FALSE || l == A_NONE))
        return A_TRUE;
    return a_eq_slow(l, r);
}
static inline aval a_ne(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r))
        return a_bool_inline(a_int_val(l) != a_int_val(r));
    if (a_is_double(l) && a_is_double(r))
        return a_bool_inline(a_double_val(l) != a_double_val(r));
    if (l == r && (l == A_TRUE || l == A_FALSE || l == A_NONE))
        return A_FALSE;
    return a_ne_slow(l, r);
}
static inline aval a_lt(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r))
        return a_bool_inline(a_int_val(l) < a_int_val(r));
    if ((a_is_int(l) || a_is_double(l)) && (a_is_int(r) || a_is_double(r))) {
        double a = a_is_int(l) ? (double)a_int_val(l) : a_double_val(l);
        double b = a_is_int(r) ? (double)a_int_val(r) : a_double_val(r);
        return a_bool_inline(a < b);
    }
    return a_lt_slow(l, r);
}
static inline aval a_le(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r))
        return a_bool_inline(a_int_val(l) <= a_int_val(r));
    if ((a_is_int(l) || a_is_double(l)) && (a_is_int(r) || a_is_double(r))) {
        double a = a_is_int(l) ? (double)a_int_val(l) : a_double_val(l);
        double b = a_is_int(r) ? (double)a_int_val(r) : a_double_val(r);
        return a_bool_inline(a <= b);
    }
    return a_le_slow(l, r);
}
static inline aval a_gt(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r))
        return a_bool_inline(a_int_val(l) > a_int_val(r));
    if ((a_is_int(l) || a_is_double(l)) && (a_is_int(r) || a_is_double(r))) {
        double a = a_is_int(l) ? (double)a_int_val(l) : a_double_val(l);
        double b = a_is_int(r) ? (double)a_int_val(r) : a_double_val(r);
        return a_bool_inline(a > b);
    }
    return a_gt_slow(l, r);
}
static inline aval a_ge(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r))
        return a_bool_inline(a_int_val(l) >= a_int_val(r));
    if ((a_is_int(l) || a_is_double(l)) && (a_is_int(r) || a_is_double(r))) {
        double a = a_is_int(l) ? (double)a_int_val(l) : a_double_val(l);
        double b = a_is_int(r) ? (double)a_int_val(r) : a_double_val(r);
        return a_bool_inline(a >= b);
    }
    return a_ge_slow(l, r);
}
aval a_contains(aval cont, aval item);
aval a_in_range(aval v, aval lo, aval hi); /* lo <= v < hi */
aval a_in_range_inclusive(aval v, aval lo, aval hi); /* lo <= v <= hi */

/* bitwise / shifts (with header fast paths) */
aval a_band_slow(aval l, aval r);
aval a_bor_slow(aval l, aval r);
aval a_bxor_slow(aval l, aval r);
aval a_shl_slow(aval l, aval r);
aval a_shr_slow(aval l, aval r);

static inline aval a_band(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t v = a_int_val(l) & a_int_val(r);
        if (a_in_inline_range(v)) return a_mkint(v);
    }
    return a_band_slow(l, r);
}
static inline aval a_bor(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t v = a_int_val(l) | a_int_val(r);
        if (a_in_inline_range(v)) return a_mkint(v);
    }
    return a_bor_slow(l, r);
}
static inline aval a_bxor(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t v = a_int_val(l) ^ a_int_val(r);
        if (a_in_inline_range(v)) return a_mkint(v);
    }
    return a_bxor_slow(l, r);
}
static inline aval a_shl(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t s = a_int_val(r);
        if (s >= 0 && s <= 63) {
            int64_t v = (int64_t)((uint64_t)a_int_val(l) << s);
            if (a_in_inline_range(v)) return a_mkint(v);
        }
    }
    return a_shl_slow(l, r);
}
static inline aval a_shr(aval l, aval r) {
    if (a_is_int(l) && a_is_int(r)) {
        int64_t s = a_int_val(r);
        if (s >= 0 && s <= 63) {
            int64_t v = a_int_val(l) >> s;
            if (a_in_inline_range(v)) return a_mkint(v);
        }
    }
    return a_shr_slow(l, r);
}

/* objects */
aval a_str_new(const char *p, uint64_t len);
aval a_char_new(const char *p, uint64_t len);
aval a_str_concat(aval l, aval r);
aval a_str_join(uint64_t n, aval *items);       /* join with "" */
aval a_str_joinsep(aval sep, uint64_t n, aval *items);
aval a_list_new(uint64_t n, aval *items);       /* list literal */
aval a_list_from_iter(aval it);                 /* collect iterable -> list */
aval a_tuple_new(uint64_t n, aval *items);
aval a_dict_new(void);
aval a_dict_set(aval d, aval k, aval v);
aval a_dict_get(aval d, aval k, aval dflt);     /* dflt may be A_NONE */
aval a_set_new(void);
aval a_set_add(aval s, aval v);
aval a_range_new(aval lo, aval hi, aval step);
aval a_range_contains(aval r, aval v);

/* indexing / slicing */
aval a_getitem(aval obj, aval idx);             /* str/list/tuple/dict */
aval a_setitem(aval obj, aval idx, aval val);   /* list/dict */
aval a_list_fill_range(aval obj, int64_t lo, int64_t hi, int64_t step,
                       aval val);                /* xs[lo..hi step] = val */
aval a_slice(aval obj, aval lo, aval hi, aval step); /* lo/hi/step may be A_NONE */

/* iteration protocol: iter = a_iter_new(v); loop a_iter_next(it,&out) */
aval a_iter_new(aval v);
aval a_iter_next(aval it, aval *out);           /* returns 1/0 */

/* functions / calls */
aval a_fnon_new(uint64_t entry, aval env, const char *name);
aval a_closure_env_new(uint64_t n);
aval a_env_set(aval env, uint64_t i, aval v);
aval a_env_get(aval env, uint64_t i);
aval a_call(aval fn, uint64_t n, aval *argv);   /* dispatch A-fn or builtin */
/* argv holds n positional args followed by nkw (name, value) pairs.
   Named args are only meaningful for T_PYOBJ receivers (Python kwargs);
   other receiver kinds panic when nkw > 0. */
aval a_method(aval obj, const char *name, uint64_t n, uint64_t nkw, aval *argv);
aval a_getattr(aval obj, const char *name);     /* field access */
aval a_unwrap(aval v);                          /* postfix ! */
aval a_some_new(aval v);                        /* Some(v) option value */
aval a_try_result(aval v);                      /* helper for err propagation */

/* builtins */
aval a_builtin(uint64_t id);                    /* materialize builtin FNON */
aval a_bi_call(uint64_t id, uint64_t n, aval *argv); /* direct builtin dispatch */

/* `a test` harness: runs each (name, fn) pair in a forked child so a panic
   in one test does not stop the suite. Returns 0 when all pass. */
typedef aval (*a_test_fn)(aval *argv);
int a_test_main(uint64_t n, const char * const *names, a_test_fn const *fns);
int64_t a_int_full(aval v);                     /* unbox any int aval */
uint64_t a_builtin_count(void);
const char *a_builtin_name(uint64_t id);

/* misc runtime */
aval a_panic_msg(const char *msg);              /* prints + exit(1), noreturn-ish */
aval a_printv(uint64_t n, aval *argv);          /* print: space-sep + \n, stdout */
aval a_eprintv(uint64_t n, aval *argv);         /* eprint -> stderr */
void a_runtime_init(void);                      /* idempotent */

/* object predicates used by codegen-free helpers */
int a_obj_is_instance(aval v, const char *type_name); /* isinstance-ish */

#endif /* ASTRA_A_RUNTIME_H */

/* codegen-support helpers */
aval a_i64n(long long v);
int a_is_propagate(aval v);
aval a_match_fail(aval subj);
aval a_list_push(aval list, aval v);
aval a_setattr(aval obj, const char *name, aval v);
