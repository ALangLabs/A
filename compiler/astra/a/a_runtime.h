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
    A_BI_ZIP, A_BI_COUNT
};

/* ---- runtime entry points (implemented in a_runtime.c) ----
 * All return aval unless noted. n/argv-style helpers take (uint64_t, aval*). */

/* value predicates / conversion */
aval a_truthy_raw(aval v);          /* returns 0 or 1 (plain u64) */
aval a_not(aval v);                 /* logical not -> A_TRUE/A_FALSE */
aval a_neg(aval v);                 /* unary - */
aval a_bnot(aval v);                /* ~ */
aval a_str(aval v);                 /* str(v): display format */
aval a_repr(aval v);                /* repr(v): quoted strings */
aval a_len(aval v);                 /* len(v) */
aval a_int(aval v);                 /* int(v) */
aval a_float(aval v);               /* float(v) */
aval a_bool(aval v);                /* bool(v) */
aval a_type_name(aval v);           /* type name string */

/* arithmetic / comparison (richly typed) */
aval a_add(aval l, aval r);
aval a_sub(aval l, aval r);
aval a_mul(aval l, aval r);
aval a_div(aval l, aval r);
aval a_mod(aval l, aval r);
aval a_pow(aval l, aval r);
aval a_eq(aval l, aval r);
aval a_ne(aval l, aval r);
aval a_lt(aval l, aval r);
aval a_le(aval l, aval r);
aval a_gt(aval l, aval r);
aval a_ge(aval l, aval r);
aval a_contains(aval cont, aval item);
aval a_in_range(aval v, aval lo, aval hi); /* lo <= v < hi */

/* bitwise / shifts */
aval a_band(aval l, aval r);
aval a_bor(aval l, aval r);
aval a_bxor(aval l, aval r);
aval a_shl(aval l, aval r);
aval a_shr(aval l, aval r);

/* objects */
aval a_str_new(const char *p, uint64_t len);
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
aval a_method(aval obj, const char *name, uint64_t n, aval *argv);
aval a_getattr(aval obj, const char *name);     /* field access */
aval a_unwrap(aval v);                          /* postfix ! */
aval a_try_result(aval v);                      /* helper for err propagation */

/* builtins */
aval a_builtin(uint64_t id);                    /* materialize builtin FNON */
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
