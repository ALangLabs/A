/* Internal runtime header: object model, shared helpers and builtin
 * prototypes shared by the rt_*.c runtime modules. Not part of the
 * public a_runtime.h API. */
#ifndef A_RUNTIME_INTERNAL_H
#define A_RUNTIME_INTERNAL_H

#include "a_runtime.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <dirent.h>
/* Real thread pool (rt_pool.c) is only enabled when the runtime archive is
   built by a system C compiler: the astra-compiled fallback (cross targets,
   A_RUNTIME_CC=off, 16-bit) defines __TINYC__ and gets the serial path, as
   does Windows. pthread.h is only pulled in on the threaded configuration. */
#if !defined(__TINYC__)
#include <pthread.h>
#define A_PAR_REAL 1
#else
#define A_PAR_REAL 0
#endif
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

#if defined(_WIN32) || defined(__TINYC__)
#define A_PAR_REAL 0
#endif

/* Cooperative fibers (async.c): ucontext + pthreads required. Off on
   Windows and TinyCC/astoc-built runtimes; A_ASYNC=0 disables at
   runtime. When 0, all a_fib_* entry points take the sync path. */
#if !defined(_WIN32) && !defined(__TINYC__)
#define A_FIBERS 1
#else
#define A_FIBERS 0
#endif

/* Per-thread scratch state. When the worker pool is real (A_PAR_REAL),
   runtime helpers reachable from parallel kernels (map/filter/sum/linalg)
   must not share mutable globals: traceback frames, the small-string bump
   arenas and the int->str cache each get a __thread copy so pool workers
   never race the main thread. On serial-only builds the macro is empty and
   the state stays a plain global (MSVC has no __thread; TinyCC-built
   runtimes are single-threaded anyway). */
#if A_PAR_REAL
#define A_RT_TLS __thread
#else
#define A_RT_TLS
#endif
/* noinline: MSVC spells it __declspec, GCC/clang use __attribute__. */
#if defined(_MSC_VER)
#define A_RT_NOINLINE __declspec(noinline)
#else
#define A_RT_NOINLINE __attribute__((noinline))
#endif
#if defined(_WIN32) && !defined(_WIN64)
/* 32-bit Windows CRT malloc only guarantees 8-byte alignment, but heap
   objects must be 16-byte aligned for NaN-boxing (mkobj panics otherwise).
   Route every runtime allocation through the aligned CRT entry points.
   All runtime .c files share this header, so malloc/free/realloc stay
   paired: every allocation and every release goes through _aligned_*. */
#include <malloc.h>
#define malloc(n) _aligned_malloc((n), 16)
#define realloc(p, n) _aligned_realloc((p), (n), 16)
/* NOTE: free() is NOT redefined to _aligned_free here — every free in the
   runtime is routed through a_gc_free() (declared/defined below), which
   pairs allocations with the right release on every target. */
#endif

#if defined(_WIN32)
/* The Win32 surface comes from the platform SDK headers, which every
   Windows toolchain ships (MSVC, clang-cl, MinGW, TCC). Using the real
   headers keeps the runtime's types and constants in lockstep with the
   SDK — hand-maintained prototypes drifted (wrong linkage on x64, mirrors
   of WIN32_FIND_DATAA / CONSOLE_SCREEN_BUFFER_INFO) and broke as soon as
   a TU also pulled in <windows.h>. winsock2.h must precede windows.h, so
   both are included here rather than only in net.c. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>      /* _read, _mktemp, _access */
#include <process.h> /* _spawnvp */
#include <stdlib.h>  /* _putenv */

/* msvcrt.dll exports vsnprintf but not snprintf (that lives in libmingwex,
   which is not shipped); the runtime provides the C99 wrapper. MSVC/UCRT
   already declare it in <stdio.h> (with dllimport linkage), so only the
   pre-UCRT Windows CRTs need this prototype. */
#if !defined(_MSC_VER) && !defined(_UCRT)
int snprintf(char *s, size_t n, const char *format, ...);
#endif
/* Win32 timing helpers implemented in core_util.c. */
int64_t a_win_time_ms(void);    /* wall clock, Unix epoch milliseconds */
int64_t a_win_mono_ms(void);    /* monotonic milliseconds (QPC) */
void a_win_sleep_ms(int64_t ms);
#endif

/* object model */
/* ------------------------------------------------------------------ */
/* object model                                                        */
/* ------------------------------------------------------------------ */

typedef struct AObj {
    uint32_t type;
    uint32_t len;
    uint64_t cap;
} AObj;

/* AArr flags (offset 32 in the struct; items/iitems pointers at 16/24):
   ARR_F_PACKED — element storage is `iitems` (int64_t *), `items` is NULL.
     Only ever set on T_LIST objects; every element is an integer.
   ARR_F_NOMIX  — sticky "known to contain a non-int aval" marker on an
     aval-mode T_LIST, so arr_push_int skips its all-int promotion scan. */
#define ARR_F_PACKED 1u
#define ARR_F_NOMIX  2u
#define ARR_F_F64    4u  /* aval items verified to be all unboxed doubles */
#define ARR_F_HAS_VIEW 8u /* a T_VIEW aliases this array's items: never shrink */

enum {
    T_STR = 1, T_LIST, T_TUPLE, T_DICT, T_SET, T_RANGE, T_ITER,
    T_FNON, T_ENV, T_RESULT, T_INT64, T_CHAR, T_VIEW, T_ARENA, T_PYOBJ,
    T_PYBUF, T_STRUCT, T_BYTES, T_ROPE
};

typedef struct { AObj h; char data[]; } AStr;

/* Rope string: the result of concatenating two string-kind values when the
   combined size is large (or either side is already a rope). Keeping the
   concat as a tree makes `s += x` / `s = s + x` loops O(1) per append
   instead of O(n) bytes copied per step; the tree is flattened lazily into
   a canonical AStr on first read (str_data/str_of/astr_flat).
   - h.type   = T_ROPE
   - h.len    = total BYTE length (same meaning as AStr.h.len)
   - h.cap    = code-point cache, identical layout to AStr: high 32 bits
                hold cp+1 (0 = not computed), low 32 bits unused.
   - left/right = the two concatenated string-kind avals (T_STR/T_CHAR/
                T_ROPE); byte order is left followed by right.
   - flat     = A_NONE until flattened, then the aval of the canonical
                AStr every reader goes through.
   Ropes are immutable like AStr, so sharing/aliasing is always safe. */
typedef struct { AObj h; aval left, right, flat; } ARope;

/* Concatenations whose result is smaller than this stay on the flat
   exact-size path: a rope node costs an allocation plus a later flatten
   copy, which only pays off once the same buffer would otherwise be
   re-copied (the `s += x` loop shape). */
#define A_STR_ROPE_MIN ((uint64_t)2048)

/* len() on a string is a code-point count, i.e. a UTF-8 scan. The result is
   cached in the otherwise-unused high half of the string's cap word (the low
   half stays len+1, and `data` therefore stays at a fixed offset that
   generated code may address directly). Stored count+1; 0 means "not
   computed yet", which is what every constructor but a_str_i64() sets. */
#define ASTR_CP_MASK UINT64_C(0xFFFFFFFF00000000)
static inline uint32_t astr_cp_cached(const AStr *s) {
    return (uint32_t)(s->h.cap >> 32);
}
static inline void astr_cp_cache(AStr *s, uint32_t cp) {
    s->h.cap = (s->h.cap & ~ASTR_CP_MASK) | ((uint64_t)(cp + 1u) << 32);
}
static inline void astr_cp_uncache(AStr *s) { s->h.cap &= ~ASTR_CP_MASK; }
/* First-class bytes: h.len is the byte count, h.cap the allocation size,
   `data` is NUL-terminated for C interop (length is authoritative). */
typedef struct { AObj h; char *data; } ABytes;
typedef struct { AObj h; aval *items; int64_t *iitems; uint32_t flags; } AArr;
typedef struct { aval k, v; } APair;
/* pairs keeps insertion order; slots is an open-addressed index (power-of-two
   scap, 0 = empty) mapping hash -> pair index for O(1) dict_find. Each slot
   word packs a 32-bit hash tag in the high half and `pair index + 1` in the
   low half, so one load both rejects most collisions and locates the pair:
   the probe chain never touches a separate hash array. */
typedef struct {
    AObj h;
    APair *pairs;
    uint64_t *slots;
    uint64_t scap;
} ADict;
/* Class instance: ADict embedded first so every (ADict *)obj_of(v) cast
   keeps working; `name` is the declared class name (NUL-terminated str
   data), `methods` an aval list of method-name strings (A_NONE when the
   class declares none), `methodFns` an aval dict mapping each method name
   to its fn value so dynamic dispatch (a_method on `var`/generic
   receivers) resolves against the receiver's own class. kind_of()
   reports K_DICT for T_STRUCT. */
typedef struct { ADict d; aval name; aval methods; aval methodFns; } AStruct;
static inline int is_dict_t(uint32_t t) { return t == T_DICT || t == T_STRUCT; }
typedef struct { AObj h; int64_t lo, hi, step; } ARange;
typedef struct {
    AObj h;
    uint64_t entry;
    aval env;
    uint64_t nparams;
    uint32_t flags;
    char *name;
    char *sig;   /* compile-time signature text "name(p1, p2)", NULL when unknown */
    char *doc;   /* compile-time docstring, NULL when absent */
} AFnon;
typedef struct { AObj h; uint64_t n; aval slots[]; } AEnv;
typedef struct { AObj h; uint32_t ok; uint32_t is_option; aval value; } AResult;
typedef struct { AObj h; int64_t v; } AInt64;
typedef struct { AObj h; aval parent; int64_t off; int64_t len; int is_tuple; } AView;
typedef struct {
    AObj h;
    aval src;
    uint64_t idx;
    int64_t cur, end, step;
    /* py_mode: src is a T_PYOBJ wrapping a PyObject* iterator; lookahead
       holds the prefetched item (Mojo _PyIter-style one-item lookahead). */
    int py_mode;
    int has_lookahead;
    aval lookahead;
} AIter;
typedef struct { uint64_t off, size; uint32_t free; } AAlloc;
typedef struct { AObj h; void *obj; } APyObj; /* owned PyObject* (python interop) */

/* Zero-copy view over a Python buffer-protocol export (numpy arrays,
   array.array, memoryview, bytearray, ...). `data` points at element 0 of
   a C-contiguous buffer owned by the Python exporter; `count` elements of
   `itemsize` bytes each. `kind` is one of the APYBUF_* codes below and
   `readonly` mirrors Py_buffer.readonly (item assignment panics on it).
   `state`/`owner` are owned by python.c: `state` is the Py_buffer copy
   released via PyBuffer_Release at process exit, `owner` an owned
   PyObject* used to convert the view back to Python. Element access does
   NOT take the GIL — the exported buffer keeps the exporter alive. */
enum {
    APYBUF_U8 = 1, APYBUF_I8, APYBUF_BOOL,
    APYBUF_U16, APYBUF_I16, APYBUF_U32, APYBUF_I32,
    APYBUF_U64, APYBUF_I64, APYBUF_F32, APYBUF_F64
};
typedef struct {
    AObj h;
    int64_t count;
    int64_t itemsize;
    int kind;
    int readonly;
    char *data;
    void *state;
    void *owner;
} APyBuf;
typedef struct {
    AObj h;
    uint64_t cap, bump, live;
    uint64_t nalloc, capalloc;
    AAlloc *allocs;
} AArena;

#define FNON_BUILTIN 1u
#define NPARAMS_UNKNOWN UINT64_MAX

enum {
    K_NONE, K_BOOL, K_INT, K_FLOAT, K_STR, K_LIST, K_TUPLE, K_DICT,
    K_SET, K_RANGE, K_ITER, K_FNON, K_ENV, K_RESULT, K_OTHER, K_CHAR, K_VIEW,
    K_BYTES
};


typedef struct { char *p; size_t len, cap; } Buf;
typedef struct { aval *p; size_t len, cap; } AVec;

/* shared helpers (defined across the rt_*.c modules) */
void panicf(const char *fmt, ...);
void *xmalloc(size_t n);
char *xarena(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrndup(const char *s, size_t n);
char *xstrdup(const char *s);
aval mkobj(void *p);
AObj *obj_of(aval v);
int is_int_val(aval v);
int64_t int_val_full(aval v);
int is_num(aval v);
double num_to_double(aval v);
int64_t expect_int64(aval v, const char *what);
aval mkint_full(int64_t i);
aval mkdouble(double d);
aval bool_aval(int b);
void buf_init(Buf *b);
void buf_reserve(Buf *b, size_t extra);
void buf_addn(Buf *b, const char *s, size_t n);
void buf_adds(Buf *b, const char *s);
void buf_addc(Buf *b, char c);
void buf_addi64(Buf *b, int64_t v);
aval buf_take(Buf *b);
AStr *str_of(aval v, const char *what);
aval a_str_new(const char *p, uint64_t len);
ABytes *a_bytes_obj(aval v);
int a_bytes_is(aval v);
aval a_bytes_new(uint64_t cap);
aval a_bytes_from(const char *p, uint64_t n);
aval a_bytes_take(char *p, uint64_t n, uint64_t cap);
void a_bytes_grow(ABytes *b, uint64_t need);
void a_bytes_append(ABytes *b, const char *p, uint64_t n);
void a_bytes_pushc(ABytes *b, int c);
char *a_bytes_data(aval v, uint64_t *len);
aval a_bytes_to_str(aval v);
/* builtins (bytes_obj.c) */
aval bi_bytes_new(uint64_t n, aval *argv);
aval bi_bytes_append(uint64_t n, aval *argv);
aval bi_bytes_to_str(uint64_t n, aval *argv);
aval bi_net_recv_buf(uint64_t n, aval *argv);
aval bi_net_recv_buf_all(uint64_t n, aval *argv);
aval a_str_new_uninit(uint64_t len);
aval a_char_new(const char *p, uint64_t len);
/* Rope-aware string accessors: astr_flat accepts any string-kind value
   (T_STR/T_CHAR/T_ROPE) and returns the flat AStr readers may use;
   str_of/str_data route through it. a_str_cat concatenates two string-kind
   avals, producing a rope for large results. a_str_cpcount is the cached
   code-point count used by len(). */
AStr *astr_flat(aval v);
aval a_str_cat(aval l, aval r);
uint32_t a_str_cpcount(aval v);
const char *str_data(aval v, uint64_t *len);
int u8_len(unsigned char c);
uint64_t u8_count(const char *p, uint64_t n);
uint64_t cp_to_byte(const char *p, uint64_t n, int64_t cp);
int str_eq(aval a, aval b);
const char *find_bytes(const char *h, size_t hn, const char *n, size_t nn, size_t from, size_t *pos);
int64_t str_find(aval hay, aval needle, int64_t start, int reverse);
aval str_repeat(aval s, int64_t count);
AArr *arr_of(aval v, const char *what);
void arr_reserve(AArr *a, uint64_t need);
void arr_reserve_i64(AArr *a, uint64_t need);
void arr_push(AArr *a, aval v);
int arr_packed(const AArr *a);
void arr_ensure_aval(AArr *a);
void arr_try_pack(AArr *a);
void arr_push_int(AArr *a, aval v);
aval arr_elem(const AArr *a, uint64_t i);
void arr_shrink(AArr *a);
void arr_note_view(aval parent);
void a_gc_set_noscan(void *p, int on);   /* gc.c: exempt a raw-data block
                                           (no pointers inside) from the
                                           conservative mark scan */
/* iitems is packed int64 storage — never avals, so its block is always
   registered GC_F_NOSCAN: int64 bit patterns can alias heap addresses
   and would otherwise retain phantom objects. Call after every (re)alloc
   of a->iitems; a_gc_reregister also preserves the flag across realloc. */
static inline void arr_iitems_raw(AArr *a) { a_gc_set_noscan(a->iitems, 1); }
/* ARR_F_F64 lists hold only unboxed-double avals in items[]: raw IEEE-754
   bits that alias heap addresses just like int64s, so once verified the
   buffer is GC_F_NOSCAN too. Every store path that can introduce a
   non-double MUST call arr_f64_unmark BEFORE the store — it restores
   scanning so the incoming pointer is seen by the marker. */
static inline void arr_f64_mark(AArr *a) {
    a->flags |= ARR_F_F64;
    a_gc_set_noscan(a->items, 1);
}
static inline void arr_f64_unmark(AArr *a) {
    if (!(a->flags & ARR_F_F64)) return;
    a->flags &= ~ARR_F_F64;
    a_gc_set_noscan(a->items, 0);
}
/* Equality of a packed int64 element against an arbitrary aval, matching
   values_equal's numeric rules (int-int compares int64, int-float compares
   doubles, everything else is unequal). Lets read-only scans (index,
   contains, count, remove) run on packed storage without converting the
   whole array to aval elements. */
static inline int arr_i64_eq_aval(int64_t e, aval v) {
    if (is_int_val(v)) return e == int_val_full(v);
    if (is_num(v)) return (double)e == num_to_double(v);
    return 0;
}

/* ------------------------------------------------------------------ */
/* shared inline helpers (deduplicated across the rt_*.c modules)      */
/* ------------------------------------------------------------------ */

/* |v| as uint64 — correct for INT64_MIN. Was iabs_u/iabs_u64/fp_iabs_u
   and inlined copies in buf_addi64/a_str_i64/i64_digits. */
static inline uint64_t a_i64_abs_u64(int64_t v) {
    return v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
}

/* int64 multiply with overflow check: *r = a*b, returns 1 on overflow.
   Was imul_of (value_arith.c) / fp_imul_of (fastpath.c). */
static inline int a_imul_of(int64_t a, int64_t b, int64_t *r) {
    if (a == 0 || b == 0) { *r = 0; return 0; }
    uint64_t ua = a_i64_abs_u64(a), ub = a_i64_abs_u64(b);
    int neg = (a < 0) != (b < 0);
    uint64_t limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    if (ua > limit / ub) return 1;
    uint64_t m = ua * ub;
    if (neg) {
        *r = (m == (uint64_t)INT64_MAX + 1) ? INT64_MIN : -(int64_t)m;
    } else {
        *r = (int64_t)m;
    }
    return 0;
}

/* splitmix64 finalizer — identical to mix64 (core_dict.c) / sh_mix
   (methods_helpers.c). */
static inline uint64_t a_mix64(uint64_t x) {
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31;
    return x;
}

/* murmur3 fmix64 — pointer-table hashes: gc.c gc_mix and value_collect.c
   dcent_hash (the latter shifts the pointer right 4 first). */
static inline uint64_t a_fmix64(uint64_t x) {
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    return x;
}

/* FNV-1a over n bytes with the offset basis applied. Callers keep their
   own finalization: dict_key_hash mixes in a type tag, aval_hash mixes
   in the length — those differ, so only the byte loop is shared. */
static inline uint64_t a_fnv1a(const char *p, uint64_t n) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (uint64_t i = 0; i < n; ++i) {
        h ^= (unsigned char)p[i];
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

/* Rightmost byte == c in p[0..n), or NULL. Word-at-a-time scan from the
   end; portable (memrchr is a GNU/BSD extension absent on MSVC). The
   haszero bit trick can only over-report, and the verify loop re-checks
   real bytes, so no false hits are possible. Was memrchr8 /
   memrchr8_local in core_str.c / methods_str.c. */
static inline const char *a_memrchr8(const char *p, size_t n,
                                     unsigned char c) {
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

/* Code-point-indexed substring search over a raw byte range — the shared
   core of str_find (core_str.c) and str_find_bounded (methods_str.c).
   h/n are BYTES; start is a code-point index (negative counts from the
   end) and reverse selects rfind semantics. Returns the code-point index
   of the match or -1. Identical body in both former copies. */
static inline int64_t a_find_in_bytes(const char *h, uint64_t hn,
                                      const char *n, uint64_t nn,
                                      int64_t start, int reverse) {
    /* The codepoint count is only needed for negative starts, empty needles
       and the "start past the end" check: counting the whole haystack first
       made every contains()/find() two full passes. */
    int64_t st = start;
    if (st < 0) {
        st += (int64_t)u8_count(h, hn);
        if (st < 0) st = 0;
    }
    if (nn == 0) {
        if (st > (int64_t)u8_count(h, hn)) return -1;
        return st;
    }
    uint64_t st_byte = st == 0 ? 0 : cp_to_byte(h, hn, st);
    if (st_byte >= hn) return -1;
    if (!reverse) {
        size_t pos = 0;
        const char *p = find_bytes(h, (size_t)hn, n, (size_t)nn, st_byte,
                                   &pos);
        if (!p) return -1;
        return (int64_t)u8_count(h, pos);
    }
    /* Reverse search: jump between first-byte candidates with a
       word-at-a-time backward memchr and memcmp only on candidates.
       Keeps the old loop's early exit (a match near the end is found in
       a few words) while scanning ~8x faster on misses. */
    if (nn > hn) return -1;
    int64_t i = (int64_t)(hn - nn);
    while (i >= (int64_t)st_byte) {
        const char *hit = a_memrchr8(h + st_byte,
                                     (size_t)(i - (int64_t)st_byte + 1),
                                     (unsigned char)n[0]);
        if (!hit) return -1;
        size_t cand = (size_t)(hit - h);
        if (memcmp(h + cand, n, (size_t)nn) == 0)
            return (int64_t)u8_count(h, cand);
        i = (int64_t)cand - 1;
    }
    return -1;
}

/* Encode one code point as UTF-8 into out (>= 4 bytes); returns the byte
   count. Was utf8_encode_cp (binary.c) / jutf8 (json.c) — byte-identical
   output for every cp < 2^32 (callers bound cp to <= 0x10FFFF; lone
   surrogates encode as 3-byte CESU-8 in both). */
static inline int a_utf8_encode_cp(uint64_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* JSON string literal (surrounding quotes included) appended to b:
   escapes " \ \b \f \n \r \t and other C0 controls as \u00XX (lowercase
   hex), copying runs of plain bytes with memcpy. Was json_escape_str
   (json.c) and log_append_json_str (log.c) — byte-identical output.
   NOTE: this is NOT the repr quoter — format_quoted (core_fmt.c)
   escapes only " \ \n \t \r and passes other control bytes through
   raw, so the two must stay separate. */
static inline void a_buf_json_quote(Buf *b, const char *s, uint64_t n) {
    static const char hexd[] = "0123456789abcdef";
    buf_addc(b, '"');
    uint64_t i = 0;
    while (i < n) {
        /* memcpy runs of unescaped bytes instead of per-byte buf_addc. */
        uint64_t j = i;
        while (j < n) {
            unsigned char c = (unsigned char)s[j];
            if (c == '"' || c == '\\' || c == '\b' || c == '\f' ||
                c == '\n' || c == '\r' || c == '\t' || c < 0x20) break;
            ++j;
        }
        if (j > i) buf_addn(b, s + i, (size_t)(j - i));
        if (j >= n) break;
        unsigned char c = (unsigned char)s[j];
        switch (c) {
            case '"': buf_adds(b, "\\\""); break;
            case '\\': buf_adds(b, "\\\\"); break;
            case '\b': buf_adds(b, "\\b"); break;
            case '\f': buf_adds(b, "\\f"); break;
            case '\n': buf_adds(b, "\\n"); break;
            case '\r': buf_adds(b, "\\r"); break;
            case '\t': buf_adds(b, "\\t"); break;
            default: {
                /* c < 0x20 -> \u00XX (lowercase hex, same as %04x) */
                char tmp[6] = {'\\', 'u', '0', '0',
                               hexd[c >> 4], hexd[c & 15]};
                buf_addn(b, tmp, 6);
                break;
            }
        }
        i = j + 1;
    }
    buf_addc(b, '"');
}

/* Monotonic wall time in milliseconds. Was now_ms (trace.c) /
   fib_now_ms (async.c). */
static inline int64_t a_rt_mono_ms(void) {
#if defined(_WIN32)
    return a_win_mono_ms();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* ---- dense numeric list helpers (shared by linalg.c / math_ew.c) ---- */

/* aval -> double for numeric elements; msg is passed to panicf when the
   value is neither an inline double nor an int. */
static inline double a_num_aval(aval v, const char *msg) {
    if (a_is_double(v)) {
        union { uint64_t u; double d; } c;
        c.u = v;
        return c.d;
    }
    if (is_int_val(v)) return (double)int_val_full(v);
    panicf("%s", msg);
    return 0.0;
}

static inline double a_arr_num(const AArr *a, uint64_t i,
                               const char *msg) {
    if (arr_packed(a)) return (double)a->iitems[i];
    return a_num_aval(a->items[i], msg);
}

/* Strict T_LIST check (unlike arr_of, tuples/sets are rejected).
   Was as_arr in linalg.c / math_ew.c. */
static inline AArr *a_list_arr(aval v, const char *what) {
    AObj *o = obj_of(v);
    if (!o || o->type != T_LIST) panicf("%s must be a list", what);
    return (AArr *)o;
}

/* Verified all-double aval storage -> raw double view (caches
   ARR_F_F64 on the array; mutating writes clear the flag). */
static inline const double *a_arr_f64_ptr(AArr *a) {
    if (arr_packed(a) || !a->items) return NULL;
    if (!(a->flags & ARR_F_F64)) {
        for (uint64_t i = 0; i < a->h.len; i++)
            if (!a_is_double(a->items[i])) return NULL;
        arr_f64_mark(a);   /* items is raw double bits: exempt from GC scan */
    }
    return (const double *)(const void *)a->items;
}

/* Copy the first n elements into a fresh double buffer. nummsg is the
   a_arr_num panic message for non-numeric elements. */
static inline double *a_arr_to_f64(const AArr *a, uint64_t n,
                                   const char *what, const char *nummsg) {
    if ((uint64_t)a->h.len < n) panicf("%s too short", what);
    double *out = (double *)malloc((size_t)(n ? n : 1) * sizeof(double));
    if (!out) panicf("out of memory");
    for (uint64_t i = 0; i < n; i++) out[i] = a_arr_num(a, i, nummsg);
    return out;
}

/* Raw double view of the first `need` elements, materializing a malloc'd
   copy into *tmp when the list is not already verified all-double.
   Caller frees *tmp (NULL when a direct view was returned). */
static inline const double *a_arr_as_f64(const AArr *a, uint64_t need,
                                         double **tmp, const char *what,
                                         const char *nummsg) {
    *tmp = NULL;
    if ((uint64_t)a->h.len < need) panicf("%s too short", what);
    const double *view = a_arr_f64_ptr((AArr *)a);
    if (view) return view;
    *tmp = a_arr_to_f64(a, need, what, nummsg);
    return *tmp;
}

/* New T_LIST (ARR_F_F64-marked) from a double buffer. */
static inline aval a_f64_list(uint64_t n, const double *src) {
    AArr *a = (AArr *)xmalloc(sizeof(AArr));
    a->h.type = T_LIST;
    a->h.len = (uint32_t)n;
    a->h.cap = n;
    a->items = (aval *)xmalloc((size_t)(n ? n : 1) * sizeof(aval));
    a->iitems = NULL;
    a->flags = ARR_F_F64;
    a_gc_set_noscan(a->items, 1);   /* all-double items: no pointers inside */
    for (uint64_t i = 0; i < n; i++) a->items[i] = mkdouble(src[i]);
    return mkobj(a);
}

ADict *dict_of(aval v, const char *what);
int64_t dict_find(ADict *d, aval k);
void dict_put(ADict *d, aval k, aval v);
void dict_remove_at(ADict *d, uint64_t i);
void dict_clear(ADict *d);
int set_contains(AArr *s, aval v);
int kind_of(aval v);
const char *type_name_cstr(aval v);
const char *type_name_error_cstr(aval v);
aval a_type_name(aval v);
int a_obj_is_instance(aval v, const char *type_name);
void format_into(Buf *b, aval v, int quote);
/* Shortest-round-trip double rendering shared by str() and json.stringify:
   writes into caller buffer (>= 72 bytes), returns length. Integral-looking
   results get a trailing ".0"; NaN/Inf render as "nan"/"inf" (json.c maps
   non-finite to null before calling). */
size_t fmt_double_render(double d, char *tmp);
aval a_str(aval v);
aval a_repr(aval v);
int truthy(aval v);
aval a_bool(aval v);
aval a_int(aval v);
aval a_float(aval v);
aval a_len(aval v);
aval a_join_len(aval values, aval sep);
int values_equal(aval a, aval b);
aval binary_arith(aval l, aval r, char op, const char *opname);
aval a_pow(aval l, aval r);
void avec_push(AVec *v, aval x);
aval a_str_joinsep(aval sep, uint64_t n, aval *items);
aval list_from_vec(const aval *p, uint64_t n, int tuple);
aval a_list_new(uint64_t n, aval *items);
aval a_list_from_i64(const int64_t *v, uint64_t n);
aval a_list_take_i64(int64_t *v, uint64_t n);
aval a_tuple_new(uint64_t n, aval *items);
aval a_list_from_iter(aval it);
aval a_dict_new(void);
aval a_deepcopy(aval v);
aval a_struct_new(aval name, aval methods, aval methodFns);
aval a_dict_set(aval d, aval k, aval v);
aval a_dict_get(aval d, aval k, aval dflt);
aval a_dict_has(aval d, aval k);
aval a_set_new(void);
aval a_set_add(aval s, aval v);
aval a_range_new(aval lo, aval hi, aval step);
int64_t a_range_bound(aval v, int64_t is_step);
int64_t normalize_index(int64_t idx, uint64_t len);
AArr *view_root(aval v, int64_t *off);
int64_t slice_bound(aval v, int64_t n, int64_t lower, int64_t upper, int64_t dflt);
aval pybuf_read(APyBuf *b, int64_t i);
void pybuf_write(APyBuf *b, int64_t i, aval v);
void panic_binop(const char *op, aval l, aval r);
aval fnon_new_raw(uint64_t entry, aval env, const char *name, uint32_t flags, uint64_t nparams);
AResult *result_new(int ok, int is_option, aval value);
aval a_some_new(aval value);
int is_result(aval v);
int result_ok(aval v);
aval a_call(aval fn, uint64_t n, aval *argv);
void want_args(const char *name, uint64_t n, uint64_t min, uint64_t max);
void sort_arr(aval *a, uint64_t n);
void sort_ints(int64_t *a, uint64_t n);
aval collect_iterable(aval v);
double math_arg(aval v, const char *name);
aval bi_print(uint64_t n, aval *argv);
aval bi_eprint(uint64_t n, aval *argv);
aval bi_len(uint64_t n, aval *argv);
aval bi_str(uint64_t n, aval *argv);
aval bi_int(uint64_t n, aval *argv);
aval bi_float(uint64_t n, aval *argv);
aval bi_bool(uint64_t n, aval *argv);
aval bi_repr(uint64_t n, aval *argv);
aval bi_type(uint64_t n, aval *argv);
aval bi_abs(uint64_t n, aval *argv);
aval bi_min(uint64_t n, aval *argv);
aval bi_max(uint64_t n, aval *argv);
aval bi_sum(uint64_t n, aval *argv);
aval bi_pow(uint64_t n, aval *argv);
aval bi_divmod(uint64_t n, aval *argv);
aval bi_sqrt(uint64_t n, aval *argv);
aval bi_log(uint64_t n, aval *argv);
aval bi_asin(uint64_t n, aval *argv);
aval bi_acos(uint64_t n, aval *argv);
aval bi_atan(uint64_t n, aval *argv);
aval bi_floor(uint64_t n, aval *argv);
aval bi_ceil(uint64_t n, aval *argv);
aval bi_trunc(uint64_t n, aval *argv);
aval bi_round(uint64_t n, aval *argv);
aval bi_random(uint64_t n, aval *argv);
aval bi_random_int(uint64_t n, aval *argv);
aval bi_range(uint64_t n, aval *argv);
aval bi_set(uint64_t n, aval *argv);
aval bi_tuple(uint64_t n, aval *argv);
aval bi_list(uint64_t n, aval *argv);
aval bi_dict(uint64_t n, aval *argv);
aval bi_map(uint64_t n, aval *argv);
aval bi_filter(uint64_t n, aval *argv);
aval bi_any(uint64_t n, aval *argv);
aval bi_all(uint64_t n, aval *argv);
aval bi_some(uint64_t n, aval *argv);
aval bi_reduce(uint64_t n, aval *argv);
aval bi_ok(uint64_t n, aval *argv);
aval bi_err(uint64_t n, aval *argv);
aval bi_panic(uint64_t n, aval *argv);
aval bi_assert(uint64_t n, aval *argv);
aval bi_assert_eq(uint64_t n, aval *argv);
aval bi_input(uint64_t n, aval *argv);
const char *path_arg(aval v, const char *what);
aval bi_read_file(uint64_t n, aval *argv);
aval bi_file_size(uint64_t n, aval *argv);
aval bi_write_file(uint64_t n, aval *argv);
aval bi_append_file(uint64_t n, aval *argv);
aval bi_file_exists(uint64_t n, aval *argv);
aval bi_list_dir(uint64_t n, aval *argv);
aval bi_remove_file(uint64_t n, aval *argv);
aval bi_create_dir(uint64_t n, aval *argv);
aval bi_remove_dir(uint64_t n, aval *argv);
aval bi_rename(uint64_t n, aval *argv);
aval bi_current_dir(uint64_t n, aval *argv);
/* os.c: process identity, stat, permissions, links, env enumeration. */
aval os_chdir(uint64_t n, aval *argv);
aval os_getpid(uint64_t n, aval *argv);
aval os_getppid(uint64_t n, aval *argv);
aval os_getuid(uint64_t n, aval *argv);
aval os_geteuid(uint64_t n, aval *argv);
aval os_getgid(uint64_t n, aval *argv);
aval os_getegid(uint64_t n, aval *argv);
aval os_getlogin(uint64_t n, aval *argv);
aval os_cpu_count(uint64_t n, aval *argv);
aval os_uname(uint64_t n, aval *argv);
aval os_stat(uint64_t n, aval *argv);
aval os_lstat(uint64_t n, aval *argv);
aval os_access(uint64_t n, aval *argv);
aval os_chmod(uint64_t n, aval *argv);
aval os_symlink(uint64_t n, aval *argv);
aval os_link(uint64_t n, aval *argv);
aval os_readlink(uint64_t n, aval *argv);
aval os_utime(uint64_t n, aval *argv);
aval os_mkdir(uint64_t n, aval *argv);
aval os_rmdir(uint64_t n, aval *argv);
aval os_truncate(uint64_t n, aval *argv);
aval os_getenv(uint64_t n, aval *argv);
aval os_environ_all(uint64_t n, aval *argv);
aval os_setenv(uint64_t n, aval *argv);
aval os_unsetenv(uint64_t n, aval *argv);
aval os_system(uint64_t n, aval *argv);
aval os_urandom(uint64_t n, aval *argv);
aval os_getloadavg(uint64_t n, aval *argv);
aval os_gethostname(uint64_t n, aval *argv);
aval os_umask(uint64_t n, aval *argv);
aval os_getpagesize(uint64_t n, aval *argv);
aval os_chown(uint64_t n, aval *argv);
aval os_ismount(uint64_t n, aval *argv);
aval os_disk_usage(uint64_t n, aval *argv);
aval os_popen(uint64_t n, aval *argv);
aval os_kill(uint64_t n, aval *argv);
aval os_exit(uint64_t n, aval *argv);
aval bi_isinstance(uint64_t n, aval *argv);
aval bi_join(uint64_t n, aval *argv);
aval bi_sorted(uint64_t n, aval *argv);
aval bi_reversed(uint64_t n, aval *argv);
aval bi_enumerate(uint64_t n, aval *argv);
aval bi_zip(uint64_t n, aval *argv);
aval bi_log2(uint64_t n, aval *argv);
aval bi_log10(uint64_t n, aval *argv);
aval bi_atan2(uint64_t n, aval *argv);
aval bi_flush(uint64_t n, aval *argv);
aval bi_i8(uint64_t n, aval *argv);
aval bi_i16(uint64_t n, aval *argv);
aval bi_i32(uint64_t n, aval *argv);
aval bi_u8(uint64_t n, aval *argv);
aval bi_u16(uint64_t n, aval *argv);
aval bi_u32(uint64_t n, aval *argv);
aval bi_f32(uint64_t n, aval *argv);
aval bi_ctz(uint64_t n, aval *argv);
aval bi_clz(uint64_t n, aval *argv);
aval bi_popcount(uint64_t n, aval *argv);
aval bi_bswap(uint64_t n, aval *argv);
aval bi_rotl(uint64_t n, aval *argv);
aval bi_rotr(uint64_t n, aval *argv);
aval bi_fma(uint64_t n, aval *argv);
aval bi_fabs(uint64_t n, aval *argv);
aval bi_copysign(uint64_t n, aval *argv);
aval bi_smin(uint64_t n, aval *argv);
aval bi_smax(uint64_t n, aval *argv);
aval bi_umin(uint64_t n, aval *argv);
aval bi_umax(uint64_t n, aval *argv);
aval bi_sadd_ovf(uint64_t n, aval *argv);
aval bi_ssub_ovf(uint64_t n, aval *argv);
aval bi_smul_ovf(uint64_t n, aval *argv);
aval bi_uadd_ovf(uint64_t n, aval *argv);
aval bi_usub_ovf(uint64_t n, aval *argv);
aval bi_umul_ovf(uint64_t n, aval *argv);
aval bi_expect(uint64_t n, aval *argv);
aval bi_likely(uint64_t n, aval *argv);
aval bi_unlikely(uint64_t n, aval *argv);
aval bi_env_get(uint64_t n, aval *argv);
aval bi_move(uint64_t n, aval *argv);
aval bi_borrow(uint64_t n, aval *argv);
aval bi_borrow_mut(uint64_t n, aval *argv);
aval bi_copy(uint64_t n, aval *argv);
aval bi_time_now(uint64_t n, aval *argv);
aval bi_time_sleep(uint64_t n, aval *argv);
aval bi_terminal_isatty(uint64_t n, aval *argv);
aval bi_terminal_raw_mode(uint64_t n, aval *argv);
aval bi_terminal_read_char(uint64_t n, aval *argv);
aval bi_terminal_read_char_ms(uint64_t n, aval *argv);
aval bi_terminal_write(uint64_t n, aval *argv);
aval bi_terminal_flush(uint64_t n, aval *argv);
aval bi_terminal_size(uint64_t n, aval *argv);
aval bi_terminal_home(uint64_t n, aval *argv);
aval bi_repl_eval(uint64_t n, aval *argv);
aval bi_repl_quiet(uint64_t n, aval *argv);
aval bi_json_stringify(uint64_t n, aval *argv);
aval bi_json_pretty(uint64_t n, aval *argv);
aval bi_json_parse(uint64_t n, aval *argv);
aval bi_json_is_valid(uint64_t n, aval *argv);
aval bi_json_kind(uint64_t n, aval *argv);
aval bi_json_get(uint64_t n, aval *argv);
aval bi_json_at(uint64_t n, aval *argv);
aval bi_json_len(uint64_t n, aval *argv);
aval bi_json_has(uint64_t n, aval *argv);
aval bi_json_keys(uint64_t n, aval *argv);
aval bi_json_as_str(uint64_t n, aval *argv);
aval bi_json_as_int(uint64_t n, aval *argv);
aval bi_json_as_float(uint64_t n, aval *argv);
aval bi_json_as_bool(uint64_t n, aval *argv);
aval bi_json_as_array(uint64_t n, aval *argv);
aval bi_json_as_map(uint64_t n, aval *argv);
aval bi_json_set(uint64_t n, aval *argv);
aval bi_json_delete(uint64_t n, aval *argv);
aval bi_struct_name(uint64_t n, aval *argv);
aval bi_struct_methods(uint64_t n, aval *argv);
aval bi_fn_signature(uint64_t n, aval *argv);
aval bi_fn_doc(uint64_t n, aval *argv);
aval bi_binary_encode(uint64_t n, aval *argv);
aval bi_binary_decode(uint64_t n, aval *argv);
aval bi_log_set_level(uint64_t n, aval *argv);
aval bi_log_set_json(uint64_t n, aval *argv);
aval bi_log_debug(uint64_t n, aval *argv);
aval bi_log_info(uint64_t n, aval *argv);
aval bi_log_warn(uint64_t n, aval *argv);
aval bi_log_error(uint64_t n, aval *argv);
aval bi_metrics_counter(uint64_t n, aval *argv);
aval bi_metrics_gauge(uint64_t n, aval *argv);
aval bi_metrics_histogram(uint64_t n, aval *argv);
aval bi_metrics_render(uint64_t n, aval *argv);
aval bi_metrics_reset(uint64_t n, aval *argv);
aval bi_trace_start(uint64_t n, aval *argv);
aval bi_trace_end(uint64_t n, aval *argv);
aval bi_trace_current(uint64_t n, aval *argv);
aval bi_trace_set_attribute(uint64_t n, aval *argv);
aval bi_trace_add_event(uint64_t n, aval *argv);
aval bi_trace_inject(uint64_t n, aval *argv);
aval bi_trace_extract(uint64_t n, aval *argv);
aval bi_trace_spans(uint64_t n, aval *argv);
aval bi_trace_reset(uint64_t n, aval *argv);
aval bi_net_resolve(uint64_t n, aval *argv);
aval bi_net_tcp_connect(uint64_t n, aval *argv);
aval bi_net_send(uint64_t n, aval *argv);
aval bi_net_recv(uint64_t n, aval *argv);
aval bi_net_close(uint64_t n, aval *argv);
aval bi_net_set_timeout(uint64_t n, aval *argv);
aval bi_net_local_port(uint64_t n, aval *argv);
aval bi_net_tls_connect(uint64_t n, aval *argv);
aval bi_net_tls_available(uint64_t n, aval *argv);
aval bi_net_recv_status(uint64_t n, aval *argv);
aval bi_net_tcp_listen(uint64_t n, aval *argv);
aval bi_net_accept(uint64_t n, aval *argv);
aval bi_net_set_nonblocking(uint64_t n, aval *argv);
aval bi_net_poll(uint64_t n, aval *argv);
aval bi_net_udp_socket(uint64_t n, aval *argv);
aval bi_net_udp_bind(uint64_t n, aval *argv);
aval bi_net_udp_send(uint64_t n, aval *argv);
aval bi_net_udp_recv(uint64_t n, aval *argv);
aval bi_net_send_bytes(uint64_t n, aval *argv);
aval bi_net_recv_bytes(uint64_t n, aval *argv);
aval bi_net_recv_exact(uint64_t n, aval *argv);
aval bi_net_recv_body(uint64_t n, aval *argv);
aval bi_net_recv_body_all(uint64_t n, aval *argv);
aval bi_net_tls_wrap(uint64_t n, aval *argv);
aval bi_net_inflate_bytes(uint64_t n, aval *argv);
aval bi_net_inflate_available(uint64_t n, aval *argv);
/* bytes_conv.c: linear-time bytes <-> string conversion */
int a_utf8_valid(const char *p, uint64_t len);
char *str_mut_data(aval v, uint64_t *len);
aval bi_bytes_decode_utf8(uint64_t n, aval *argv);
aval bi_str_encode_utf8(uint64_t n, aval *argv);
aval bi_read_bytes(uint64_t n, aval *argv);
aval bi_write_bytes(uint64_t n, aval *argv);
aval bi_arena_new(uint64_t n, aval *argv);
aval bi_arena_pool(uint64_t n, aval *argv);
aval bi_arena_region(uint64_t n, aval *argv);
aval bi_arena_alloc(uint64_t n, aval *argv);
aval bi_arena_release(uint64_t n, aval *argv);
aval bi_arena_reset(uint64_t n, aval *argv);
aval bi_arena_used(uint64_t n, aval *argv);
aval bi_arena_total(uint64_t n, aval *argv);
aval bi_linalg_dot(uint64_t n, aval *argv);
aval bi_linalg_matvec(uint64_t n, aval *argv);
aval bi_linalg_matvec_t(uint64_t n, aval *argv);
aval bi_linalg_matmul(uint64_t n, aval *argv);
aval bi_linalg_bmm(uint64_t n, aval *argv);
aval bi_linalg_einsum2(uint64_t n, aval *argv);
aval bi_linalg_softmax(uint64_t n, aval *argv);
aval bi_linalg_attention(uint64_t n, aval *argv);
aval bi_f32s(uint64_t n, aval *argv);
/* math_ew.c: std.math elementwise ufuncs + flat reductions over [f64] */
aval bi_math_ew_un(uint64_t n, aval *argv);
aval bi_math_ew_bin(uint64_t n, aval *argv);
aval bi_math_ew_bin_scalar(uint64_t n, aval *argv);
aval bi_math_ew_reduce(uint64_t n, aval *argv);
aval bi_unsafe_get(uint64_t n, aval *argv);
aval bi_unsafe_set(uint64_t n, aval *argv);
aval bi_unsafe_address_of(uint64_t n, aval *argv);
aval bi_unsafe_load(uint64_t n, aval *argv);
aval bi_unsafe_store(uint64_t n, aval *argv);
aval bi_unsafe_offset(uint64_t n, aval *argv);
aval bi_py_call(uint64_t n, aval *argv);
aval bi_py_buffer(uint64_t n, aval *argv);
aval bi_py_import_module(uint64_t n, aval *argv);
aval bi_py_try_import_module(uint64_t n, aval *argv);
aval bi_py_getattr_builtin(uint64_t n, aval *argv);
aval bi_py_try_getattr_builtin(uint64_t n, aval *argv);
aval bi_py_setattr_builtin(uint64_t n, aval *argv);
aval bi_py_hasattr(uint64_t n, aval *argv);
aval bi_py_delattr(uint64_t n, aval *argv);
aval bi_py_getitem_builtin(uint64_t n, aval *argv);
aval bi_py_setitem_builtin(uint64_t n, aval *argv);
aval bi_py_delitem(uint64_t n, aval *argv);
aval bi_py_call_obj(uint64_t n, aval *argv);
aval bi_py_try_call_obj(uint64_t n, aval *argv);
aval bi_py_call_method_builtin(uint64_t n, aval *argv);
aval bi_py_try_call_method_builtin(uint64_t n, aval *argv);
aval bi_py_is_pyobject(uint64_t n, aval *argv);
aval bi_py_str_builtin(uint64_t n, aval *argv);
aval bi_py_repr_builtin(uint64_t n, aval *argv);
aval bi_py_len_builtin(uint64_t n, aval *argv);
aval bi_py_contains_builtin(uint64_t n, aval *argv);
aval bi_py_truthy_builtin(uint64_t n, aval *argv);
aval bi_py_type_builtin(uint64_t n, aval *argv);
aval bi_py_buffer_info(uint64_t n, aval *argv);

/* Python interop bridge (src/astra/runtime/python.c): opaque
   T_PYOBJ values behave like Python objects — attribute access, indexing,
   method calls, len/contains, iteration and call. Each takes the GIL
   internally; Python errors panicf with the interpreter's error text. */
aval a_py_getattr(aval obj, const char *name);
aval a_py_setattr(aval obj, const char *name, aval v);
aval a_py_getitem(aval obj, aval key);
aval a_py_setitem(aval obj, aval key, aval v);
aval a_py_len(aval obj);
aval a_py_contains(aval obj, aval item);
aval a_py_call(aval fn, uint64_t n, aval *argv);
/* argv = n positional args followed by nkw (name, value) pairs. */
aval a_py_call_kw(aval fn, uint64_t n, uint64_t nkw, aval *argv);
aval a_py_call_method(aval obj, const char *name, uint64_t n, uint64_t nkw, aval *argv);
aval a_py_iter_begin(aval v);                /* T_PYOBJ-wrapped PyObject_GetIter */
int a_py_iter_next(aval it, aval *out);      /* 1 = *out set, 0 = exhausted */
aval a_py_str(aval obj);
aval a_py_repr(aval obj);
int a_py_truthy(aval obj);
int a_py_equals(aval a, aval b);              /* PyObject_RichCompareBool(Py_EQ) */
int a_py_equals_mixed(aval a, aval b);         /* one side T_PYOBJ, other any A value */
void a_py_release_obj(void *p);                /* GC: release owned PyObject/buffer */
void a_gc_shutdown_py(void);                    /* exit: drop live Py refs */
aval a_py_buffer_view(aval obj);              /* T_PYOBJ -> zero-copy T_PYBUF view */
aval a_py_hasattr(aval obj, const char *name);
aval a_py_delattr(aval obj, const char *name);
aval a_py_delitem(aval obj, aval key);
aval a_py_import_module(const char *name);
aval a_printv(uint64_t n, aval *argv);
aval a_eprintv(uint64_t n, aval *argv);
aval a_print_opts(uint64_t n, aval *argv, aval sep, aval end, aval file,
                  int is_stderr);
aval a_panic_msg(const char *msg);
aval a_list_push(aval list, aval v);
/* Fast path for `list.append(v)` emitted by the asm backend when the
   receiver's static type is an array: pushes directly, preserving the
   generic a_method semantics (incl. the view-append panic) for every
   other runtime kind. */
aval a_list_append(aval list, aval v);

/* repl quiet flag lives in rt_terminal.c; printing lives here. */
aval bi_sin(uint64_t n, aval *argv);
aval bi_cos(uint64_t n, aval *argv);
aval bi_tan(uint64_t n, aval *argv);
aval bi_exp(uint64_t n, aval *argv);
int a_rt_repl_quiet(void);

/* ------------------------------------------------------------------ */
/* implicit parallelism (rt_pool.c)                                    */
/* ------------------------------------------------------------------ */
/* Splits [0,n) into chunks of ~`grain` elements and runs fn(ctx,lo,hi) on
   each chunk across the worker pool. Falls back to an inline serial loop
   when the pool is unavailable (Windows, astra-compiled runtime, A_PAR=0,
   single CPU, or when called from a pool worker). Results must be written
   to disjoint per-index storage by fn; fn must not call back into
   a_par_for (nested calls run serially). */
typedef void (*AParFn)(void *ctx, int64_t lo, int64_t hi);
void a_par_for(int64_t n, int64_t grain, AParFn fn, void *ctx);
/* Number of pool workers (>=1); useful for sizing chunk counts. */
int a_par_workers(void);
/* Threshold below which parallel collection paths stay serial. */
#define A_PAR_MIN_ELEMS ((int64_t)1 << 15)

/* ------------------------------------------------------------------ */
/* garbage collector (gc.c)                                            */
/* ------------------------------------------------------------------ */
/* Conservative mark-sweep over every xmalloc/xrealloc block. See gc.c
   for the design. All hooks are cheap no-ops when A_GC=0. */
void a_gc_maybe_collect(void);          /* called at the top of xmalloc/xrealloc */
void a_gc_register(void *p, size_t n);  /* record a fresh allocation        */
void a_gc_reregister(void *oldp, void *newp, size_t n); /* after realloc moved */
size_t a_gc_pool_alloc_size(size_t n); /* malloc size for pooled classes    */
void *a_gc_pool_try_alloc(size_t n);   /* reuse a delisted block, or NULL   */
void a_gc_note_obj(void *p);            /* mkobj: flag block as GC object   */
void a_gc_note_arena(void *p, size_t n, int mapped);/* xarena: record bump chunk */
void a_gc_arena_switch(const void *old_cursor, void *new_chunk);
int a_gc_arena_stale(const void *cursor);
void a_gc_mark_ptr(const void *p);     /* root provider: mark containing blk */
void a_gc_mark_aval(aval v);           /* root provider: mark aval's object */
void a_gc_push_root(aval v);
void a_gc_pop_root(void);
void a_gc_add_root_cb(void (*fn)(void)); /* register a per-collection root provider */
void a_gc_free(void *p);               /* free() replacement: delist + release */
/* True while the calling thread is a pool worker or any a_par_for region
   is active: the collector must not run (worker stacks are not scanned). */
int a_par_gc_blocked(void);

/* Mark every 8-byte word in [lo,hi) as a potential pointer. Only valid
   while the collector is running (g_in_gc); used by async.c to scan
   suspended fiber stacks, which are raw malloc'd and therefore not GC
   blocks themselves. No-op outside a collection. */
void a_gc_scan_region(uintptr_t lo, uintptr_t hi);

/* ------------------------------------------------------------------ */
/* auto-async fibers (async.c)                                         */
/* ------------------------------------------------------------------ */
/* Cooperative ucontext fibers + poll event loop. When A_FIBERS=0 every
   function degrades to the synchronous path and a_fib_on() is always 0.
   AFib is opaque to callers outside async.c. */
typedef struct AFib AFib;
extern A_RT_TLS AFib *a_fib_current; /* running fiber, NULL outside      */
int  a_fib_on(void);                 /* nonzero inside a running fiber   */
int  a_fib_stack_bounds(uintptr_t *lo, uintptr_t *hi); /* current fiber */
void a_fib_sleep_ms(int64_t ms);     /* yields inside a fiber            */
int  a_fib_wait_fd(int fd, short events, int64_t timeout_ms);
void *a_fib_blocking(void *(*fn)(void *), void *arg); /* pool offload */
AFib *a_fib_spawn_fn(aval fnval, uint64_t n, aval *args);
aval a_fib_spawn_handle(aval fnval, uint64_t n, aval *args);
aval a_fib_join_handle(aval h);
AFib *a_fib_spawn_raw(uint64_t entry, aval env, uint64_t n, aval *args);
void a_fib_wait_fiber(AFib *target);
aval a_fib_main(uint64_t entry, aval env, uint64_t n, aval *args);
void a_fib_reap_all(void);
aval bi_fib_spawn(uint64_t n, aval *argv);
aval bi_fib_join(uint64_t n, aval *argv);
aval bi_fib_done(uint64_t n, aval *argv);
aval bi_fib_cancel(uint64_t n, aval *argv);

/* ------------------------------------------------------------------ */
/* deterministic regions (region.c) + gc.c region hooks               */
/* ------------------------------------------------------------------ */
/* a_frame_push/a_frame_pop drive traceback frames and the frame-depth
   counter regions use for lazy young-region creation; mkobj reports
   objects to the innermost region and store sites call
   a_region_store/a_promote. All no-ops when A_REGIONS=0 or GC is off. */
extern A_RT_TLS uint32_t a_region_frame_depth;
const char *a_frame_top_name(void);
void a_frame_note_region(void);
void a_region_enter(const char *name);
void a_region_leave(void);
void a_region_note_obj(void *p);
void a_region_store(void *container, aval child);
void a_region_barrier(aval container, aval *slot);
void a_region_barrier_range(aval container, aval *slots, uint64_t n);
void a_promote(aval v);
int  a_gc_track_region(void *p, uint32_t depth);
int  a_gc_release_obj(void *p);

/* Route every runtime free() through the collector so tracked blocks are
   delisted before release. Defined LAST so it cannot interfere with the
   prototypes above; a_gc_free forwards untracked pointers to real free. */
#define free(p) a_gc_free(p)

#endif /* A_RUNTIME_INTERNAL_H */
