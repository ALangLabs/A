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

enum {
    T_STR = 1, T_LIST, T_TUPLE, T_DICT, T_SET, T_RANGE, T_ITER,
    T_FNON, T_ENV, T_RESULT, T_INT64, T_CHAR, T_VIEW, T_ARENA, T_PYOBJ,
    T_PYBUF, T_STRUCT
};

typedef struct { AObj h; char data[]; } AStr;
typedef struct { AObj h; aval *items; int64_t *iitems; uint32_t flags; } AArr;
typedef struct { aval k, v; } APair;
/* pairs keeps insertion order; phash[i] is the cached hash of pairs[i].k;
   slots is an open-addressed index (power-of-two scap, 0 = empty, else
   pair index + 1) mapping hash -> pair index for O(1) dict_find. */
typedef struct {
    AObj h;
    APair *pairs;
    uint64_t *phash;
    uint32_t *slots;
    uint64_t scap;
} ADict;
/* Class instance: ADict embedded first so every (ADict *)obj_of(v) cast
   keeps working; `name` is the declared class name (NUL-terminated str
   data), `methods` an aval list of method-name strings (A_NONE when the
   class declares none). kind_of() reports K_DICT for T_STRUCT. */
typedef struct { ADict d; aval name; aval methods; } AStruct;
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
    K_SET, K_RANGE, K_ITER, K_FNON, K_ENV, K_RESULT, K_OTHER, K_CHAR, K_VIEW
};


typedef struct { char *p; size_t len, cap; } Buf;
typedef struct { aval *p; size_t len, cap; } AVec;

/* shared helpers (defined across the rt_*.c modules) */
void panicf(const char *fmt, ...);
void *xmalloc(size_t n);
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
aval a_str_new_uninit(uint64_t len);
aval a_char_new(const char *p, uint64_t len);
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
ADict *dict_of(aval v, const char *what);
int64_t dict_find(ADict *d, aval k);
void dict_put(ADict *d, aval k, aval v);
void dict_remove_at(ADict *d, uint64_t i);
void dict_clear(ADict *d);
int set_contains(AArr *s, aval v);
int kind_of(aval v);
const char *type_name_cstr(aval v);
aval a_type_name(aval v);
int a_obj_is_instance(aval v, const char *type_name);
void format_into(Buf *b, aval v, int quote);
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
aval a_struct_new(aval name, aval methods);
aval a_dict_set(aval d, aval k, aval v);
aval a_dict_get(aval d, aval k, aval dflt);
aval a_set_new(void);
aval a_set_add(aval s, aval v);
aval a_range_new(aval lo, aval hi, aval step);
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
aval dict_new_empty(void);
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
aval bi_linalg_softmax(uint64_t n, aval *argv);
aval bi_linalg_attention(uint64_t n, aval *argv);
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

/* Python interop bridge (compiler/astra/backend/python/python.c): opaque
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
aval a_py_buffer_view(aval obj);              /* T_PYOBJ -> zero-copy T_PYBUF view */
aval a_py_hasattr(aval obj, const char *name);
aval a_py_delattr(aval obj, const char *name);
aval a_py_delitem(aval obj, aval key);
aval a_py_import_module(const char *name);
aval a_printv(uint64_t n, aval *argv);
aval a_eprintv(uint64_t n, aval *argv);
aval a_panic_msg(const char *msg);
aval a_list_push(aval list, aval v);
aval a_list_push_int_str(aval list, int64_t v);
aval a_list_fill_str_range(aval list, int64_t lo, int64_t hi, int64_t step);
aval a_set_add_int_str(aval s, int64_t v);
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
aval bi_terminal_unsupported(uint64_t n, aval *argv);
#if defined(_WIN32)
#define bi_terminal_isatty bi_terminal_unsupported
#define bi_terminal_raw_mode bi_terminal_unsupported
#define bi_terminal_read_char bi_terminal_unsupported
#define bi_terminal_read_char_ms bi_terminal_unsupported
#define bi_terminal_write bi_terminal_unsupported
#define bi_terminal_flush bi_terminal_unsupported
#define bi_terminal_size bi_terminal_unsupported
#define bi_terminal_home bi_terminal_unsupported
#endif
int a_rt_repl_quiet(void);

#endif /* A_RUNTIME_INTERNAL_H */
