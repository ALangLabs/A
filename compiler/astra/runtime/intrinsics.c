#include "a_runtime_internal.h"

/* ---- LLVM-style intrinsics (bit ops, min/max, overflow, branch hints) -- */

/* Portable bit scans (the Astra backend has no __builtin_ctz family). */
static int a_ctz64(uint64_t x) {
    if (!x) return 64;
    int n = 0;
    if (!(x & UINT64_C(0xFFFFFFFF))) { n += 32; x >>= 32; }
    if (!(x & UINT64_C(0xFFFF))) { n += 16; x >>= 16; }
    if (!(x & UINT64_C(0xFF))) { n += 8; x >>= 8; }
    if (!(x & UINT64_C(0xF))) { n += 4; x >>= 4; }
    if (!(x & UINT64_C(0x3))) { n += 2; x >>= 2; }
    if (!(x & UINT64_C(0x1))) { n += 1; }
    return n;
}
static int a_clz64(uint64_t x) {
    if (!x) return 64;
    int n = 0;
    if (!(x & UINT64_C(0xFFFFFFFF00000000))) { n += 32; x <<= 32; }
    if (!(x & UINT64_C(0xFFFF000000000000))) { n += 16; x <<= 16; }
    if (!(x & UINT64_C(0xFF00000000000000))) { n += 8; x <<= 8; }
    if (!(x & UINT64_C(0xF000000000000000))) { n += 4; x <<= 4; }
    if (!(x & UINT64_C(0xC000000000000000))) { n += 2; x <<= 2; }
    if (!(x & UINT64_C(0x8000000000000000))) { n += 1; }
    return n;
}
static int a_popcount64(uint64_t x) {
    x -= (x >> 1) & UINT64_C(0x5555555555555555);
    x = (x & UINT64_C(0x3333333333333333)) + ((x >> 2) & UINT64_C(0x3333333333333333));
    x = (x + (x >> 4)) & UINT64_C(0x0F0F0F0F0F0F0F0F);
    return (int)((x * UINT64_C(0x0101010101010101)) >> 56);
}

aval bi_ctz(uint64_t n, aval *argv) {
    want_args("ctz", n, 1, 1);
    uint64_t x = (uint64_t)expect_int64(argv[0], "ctz() argument");
    return mkint_full(a_ctz64(x));
}
aval bi_clz(uint64_t n, aval *argv) {
    want_args("clz", n, 1, 1);
    uint64_t x = (uint64_t)expect_int64(argv[0], "clz() argument");
    return mkint_full(a_clz64(x));
}
aval bi_popcount(uint64_t n, aval *argv) {
    want_args("popcount", n, 1, 1);
    uint64_t x = (uint64_t)expect_int64(argv[0], "popcount() argument");
    return mkint_full(a_popcount64(x));
}
aval bi_bswap(uint64_t n, aval *argv) {
    want_args("bswap", n, 1, 1);
    uint64_t x = (uint64_t)expect_int64(argv[0], "bswap() argument");
    x = ((x & UINT64_C(0x00000000000000FF)) << 56) |
        ((x & UINT64_C(0x000000000000FF00)) << 40) |
        ((x & UINT64_C(0x0000000000FF0000)) << 24) |
        ((x & UINT64_C(0x00000000FF000000)) << 8) |
        ((x & UINT64_C(0x000000FF00000000)) >> 8) |
        ((x & UINT64_C(0x0000FF0000000000)) >> 24) |
        ((x & UINT64_C(0x00FF000000000000)) >> 40) |
        ((x & UINT64_C(0xFF00000000000000)) >> 56);
    return mkint_full((int64_t)x);
}
aval bi_rotl(uint64_t n, aval *argv) {
    want_args("rotl", n, 2, 2);
    uint64_t x = (uint64_t)expect_int64(argv[0], "rotl() value");
    uint64_t k = (uint64_t)(expect_int64(argv[1], "rotl() amount") & 63);
    return mkint_full((int64_t)(k ? ((x << k) | (x >> (64 - k))) : x));
}
aval bi_rotr(uint64_t n, aval *argv) {
    want_args("rotr", n, 2, 2);
    uint64_t x = (uint64_t)expect_int64(argv[0], "rotr() value");
    uint64_t k = (uint64_t)(expect_int64(argv[1], "rotr() amount") & 63);
    return mkint_full((int64_t)(k ? ((x >> k) | (x << (64 - k))) : x));
}

aval bi_fma(uint64_t n, aval *argv) {
    want_args("fma", n, 3, 3);
    double a = math_arg(argv[0], "fma");
    double b = math_arg(argv[1], "fma");
    double c = math_arg(argv[2], "fma");
    return mkdouble(fma(a, b, c));
}
aval bi_fabs(uint64_t n, aval *argv) {
    want_args("fabs", n, 1, 1);
    return mkdouble(fabs(math_arg(argv[0], "fabs")));
}
aval bi_copysign(uint64_t n, aval *argv) {
    want_args("copysign", n, 2, 2);
    return mkdouble(copysign(math_arg(argv[0], "copysign"),
                            math_arg(argv[1], "copysign")));
}

aval bi_smin(uint64_t n, aval *argv) {
    want_args("smin", n, 2, 2);
    int64_t a = expect_int64(argv[0], "smin() argument");
    int64_t b = expect_int64(argv[1], "smin() argument");
    return mkint_full(a < b ? a : b);
}
aval bi_smax(uint64_t n, aval *argv) {
    want_args("smax", n, 2, 2);
    int64_t a = expect_int64(argv[0], "smax() argument");
    int64_t b = expect_int64(argv[1], "smax() argument");
    return mkint_full(a > b ? a : b);
}
aval bi_umin(uint64_t n, aval *argv) {
    want_args("umin", n, 2, 2);
    uint64_t a = (uint64_t)expect_int64(argv[0], "umin() argument");
    uint64_t b = (uint64_t)expect_int64(argv[1], "umin() argument");
    return mkint_full((int64_t)(a < b ? a : b));
}
aval bi_umax(uint64_t n, aval *argv) {
    want_args("umax", n, 2, 2);
    uint64_t a = (uint64_t)expect_int64(argv[0], "umax() argument");
    uint64_t b = (uint64_t)expect_int64(argv[1], "umax() argument");
    return mkint_full((int64_t)(a > b ? a : b));
}

/* Overflow builtins return (wrapped result, overflowed?) like the LLVM
   *.with.overflow intrinsics. */
static aval ovf_result(const char *name, int64_t r, int ovf) {
    aval pair[2];
    pair[0] = mkint_full(r);
    pair[1] = bool_aval(ovf);
    return a_tuple_new(2, pair);
}

aval bi_sadd_ovf(uint64_t n, aval *argv) {
    want_args("sadd_overflow", n, 2, 2);
    int64_t a = expect_int64(argv[0], "sadd_overflow() argument");
    int64_t b = expect_int64(argv[1], "sadd_overflow() argument");
    int64_t r = (int64_t)((uint64_t)a + (uint64_t)b);
    int ovf = (b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b);
    return ovf_result("sadd_overflow", r, ovf);
}
aval bi_ssub_ovf(uint64_t n, aval *argv) {
    want_args("ssub_overflow", n, 2, 2);
    int64_t a = expect_int64(argv[0], "ssub_overflow() argument");
    int64_t b = expect_int64(argv[1], "ssub_overflow() argument");
    int64_t r = (int64_t)((uint64_t)a - (uint64_t)b);
    int ovf = (b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b);
    return ovf_result("ssub_overflow", r, ovf);
}
aval bi_smul_ovf(uint64_t n, aval *argv) {
    want_args("smul_overflow", n, 2, 2);
    int64_t a = expect_int64(argv[0], "smul_overflow() argument");
    int64_t b = expect_int64(argv[1], "smul_overflow() argument");
    int64_t r = (int64_t)((uint64_t)a * (uint64_t)b);
    int ovf;
    if (a == 0 || b == 0) ovf = 0;
    else if (a == -1) ovf = (b == INT64_MIN);
    else if (b == -1) ovf = (a == INT64_MIN);
    else {
        int64_t q = r / a;
        ovf = (q != b);
    }
    return ovf_result("smul_overflow", r, ovf);
}
aval bi_uadd_ovf(uint64_t n, aval *argv) {
    want_args("uadd_overflow", n, 2, 2);
    uint64_t a = (uint64_t)expect_int64(argv[0], "uadd_overflow() argument");
    uint64_t b = (uint64_t)expect_int64(argv[1], "uadd_overflow() argument");
    uint64_t r = a + b;
    return ovf_result("uadd_overflow", (int64_t)r, r < a);
}
aval bi_usub_ovf(uint64_t n, aval *argv) {
    want_args("usub_overflow", n, 2, 2);
    uint64_t a = (uint64_t)expect_int64(argv[0], "usub_overflow() argument");
    uint64_t b = (uint64_t)expect_int64(argv[1], "usub_overflow() argument");
    uint64_t r = a - b;
    return ovf_result("usub_overflow", (int64_t)r, r > a);
}
aval bi_umul_ovf(uint64_t n, aval *argv) {
    want_args("umul_overflow", n, 2, 2);
    uint64_t a = (uint64_t)expect_int64(argv[0], "umul_overflow() argument");
    uint64_t b = (uint64_t)expect_int64(argv[1], "umul_overflow() argument");
    uint64_t r = a * b;
    int ovf = (a != 0 && r / a != b) || (a == 0 && b != 0 && r != 0);
    return ovf_result("umul_overflow", (int64_t)r, ovf);
}

/* llvm.expect analog: a branch-hint wrapper; the runtime ignores the hint
   and returns the condition unchanged. */
aval bi_expect(uint64_t n, aval *argv) {
    want_args("expect", n, 1, 2);
    (void)argv;
    return argv[0];
}
aval bi_likely(uint64_t n, aval *argv) {
    want_args("likely", n, 1, 1);
    return argv[0];
}
aval bi_unlikely(uint64_t n, aval *argv) {
    want_args("unlikely", n, 1, 1);
    return argv[0];
}
