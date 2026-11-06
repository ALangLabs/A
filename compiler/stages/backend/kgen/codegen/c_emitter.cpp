// C99 emitter for the native subset, consumed by the JIT (jit.cpp) and
// by `a build` via the builtin native compiler. Every A value is an `aval` — a tagged union:
//
//   typedef struct { a_i64 i; double d; int f; } aval;
//
// `f` says which member is live, so values of unknown type (function params,
// call results, mixed arithmetic) carry their type with them and dispatch at
// runtime through the ai()/ad() helpers. Statically-known locals and globals
// still take the direct .i/.d fast path via the slot/global type tables.

#include "backend/kgen/codegen/llvm_emitter.hpp"

#include <cctype>
#include <cstdio>
#include <ostream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace a {
namespace {

bool isStdlibModuleC(const ModuleGraph& graph, const Module& mod) {
  if (mod.id == graph.entry) return false;
  return mod.path.find("stdlib") != std::string::npos;
}

std::string extractStringC(const StringLitExpr& s) {
  std::string result;
  for (const auto& p : s.parts) {
    if (!p.isExpr) result += p.text;
  }
  return result;
}

enum class VT : char { Int = 0, Float = 1, Unknown = 2 };

class CEmitter {
 public:
  CEmitter(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out)
      : graph_(graph), sema_(sema), out_(out) {}

  bool emit() {
    collectGlobals();
    pretypeGlobals();
    collectStrings();
    collectLambdas();

    out_ << "typedef long long a_i64;\n";
    out_ << "typedef struct { a_i64 i; double d; int f; } aval;\n";
    out_ << "static a_i64 ai(aval v) { return v.f ? (a_i64)v.d : v.i; }\n";
    out_ << "static double ad(aval v) { return v.f ? v.d : (double)v.i; }\n";
    out_ << "int printf(const char *, ...);\n";
    out_ << "int fprintf(void *, const char *, ...);\n";
    out_ << "int fputs(const char *, void *);\n";
    out_ << "int fflush(void *);\n";
    out_ << "void *fopen(const char *, const char *);\n";
    out_ << "int fclose(void *);\n";
    out_ << "extern void *stderr;\n";
    out_ << "extern void *stdout;\n";
    // File/stream handles for print(file=...): 1 = stdout, 2 = stderr,
    // 3+ = open() results, matching the interpreter's numbering.
    out_ << "static void *a_files[1024];\n"
            "static int a_nextfile = 3;\n"
            "static a_i64 a_open(const char *path, const char *mode) {\n"
            "  void *f = fopen(path, mode); if (!f) return -1;\n"
            "  if (a_nextfile >= 1024) { fclose(f); return -1; }\n"
            "  a_files[a_nextfile] = f; return a_nextfile++;\n"
            "}\n"
            "static void *a_stream(a_i64 h) {\n"
            "  if (h == 1) return stdout; if (h == 2) return stderr;\n"
            "  if (h >= 3 && h < 1024) return a_files[h]; return stdout;\n"
            "}\n"
            "static void a_close(a_i64 h) {\n"
            "  if (h >= 3 && h < 1024 && a_files[h]) { fclose(a_files[h]); a_files[h] = 0; }\n"
            "}\n";
    out_ << "void *malloc(unsigned long);\n";
    out_ << "void free(void *);\n";
    out_ << "void *memcpy(void *, const void *, unsigned long);\n";
    out_ << "void *memset(void *, int, unsigned long);\n";
    out_ << "void *memmove(void *, const void *, unsigned long);\n";
    out_ << "int memcmp(const void *, const void *, unsigned long);\n";
    out_ << "double sqrt(double);\n";
    out_ << "double pow(double, double);\n";
    out_ << "double fmod(double, double);\n";
    out_ << "double floor(double);\n";
    out_ << "double ceil(double);\n";
    out_ << "double round(double);\n";
    out_ << "double exp(double);\n";
    out_ << "double log(double);\n";
    out_ << "double sin(double);\n";
    out_ << "double cos(double);\n";
    out_ << "double tan(double);\n";
    out_ << "double fabs(double);\n";
    out_ << "void exit(int);\n";
    out_ << "typedef unsigned long size_t;\n";
    out_ << "int snprintf(char *, unsigned long, const char *, ...);\n";
    out_ << "double strtod(const char *, char **);\n";
    out_ << "char *strchr(const char *, int);\n";
    out_ << "char *strcat(char *, const char *);\n";
    // Python-style float repr: shortest %g precision that round-trips, with a
    // trailing ".0" when the result would otherwise look like an int.
    out_ << "static void a_fmt_double(char *buf, double v) {\n"
            "  int p;\n"
            "  for (p = 1; p <= 17; ++p) {\n"
            "    snprintf(buf, 64, \"%.*g\", p, v);\n"
            "    if (strtod(buf, 0) == v) break;\n"
            "  }\n"
            "  if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E') &&\n"
            "      !strchr(buf, 'n') && !strchr(buf, 'i') && !strchr(buf, 'N') &&\n"
            "      !strchr(buf, 'I')) strcat(buf, \".0\");\n"
            "}\n"
            "static void a_print_double(double v) {\n"
            "  char buf[64]; a_fmt_double(buf, v); printf(\"%s\\n\", buf);\n"
            "}\n";
    // Heap objects: aval.f == 2 means .i holds an a_obj*. type 1 = string
    // (data is char*, len bytes), type 2 = list (data is aval*, len elems).
    out_ << "typedef struct a_obj { int type; a_i64 len; a_i64 cap; void *data; } a_obj;\n";
    out_ << "static a_obj *a_new_obj(int t, a_i64 len, a_i64 cap, void *d) {\n"
            "  a_obj *o = (a_obj *)malloc(sizeof(a_obj));\n"
            "  if (!o) { printf(\"out of memory\\n\"); exit(1); }\n"
            "  o->type = t; o->len = len; o->cap = cap; o->data = d;\n"
            "  return o;\n"
            "}\n"
            "static aval a_strof(aval v);\n"
            "static aval a_mul(aval l, aval r) {\n"
            "  int ls = (l.f == 2 && ((a_obj *)l.i)->type == 1);\n"
            "  int rs = (r.f == 2 && ((a_obj *)r.i)->type == 1);\n"
            "  if (ls || rs) {\n"
            "    aval S = ls ? l : r;\n"
            "    aval N = ls ? r : l;\n"
            "    a_obj *so = (a_obj *)S.i;\n"
            "    a_i64 cnt = ai(N);\n"
            "    if (cnt < 0) cnt = 0;\n"
            "    a_i64 n = so->len * cnt;\n"
            "    char *b = (char *)malloc((size_t)n + 1);\n"
            "    if (!b) { printf(\"out of memory\\n\"); exit(1); }\n"
            "    for (a_i64 k = 0; k < cnt; ++k)\n"
            "      memcpy(b + k * so->len, so->data, (size_t)so->len);\n"
            "    b[n] = 0;\n"
            "    aval v; v.f = 2; v.i = (a_i64)a_new_obj(1, n, 0, b); v.d = 0;\n"
            "    return v;\n"
            "  }\n"
            "  if (l.f == 1 || r.f == 1) {\n"
            "    aval v; v.f = 1; v.d = ad(l) * ad(r); v.i = 0;\n"
            "    return v;\n"
            "  }\n"
            "  aval v; v.i = ai(l) * ai(r); v.d = 0; v.f = 0;\n"
            "  return v;\n"
            "}\n"
            "static aval a_div(aval l, aval r) {\n"
            "  if (l.f == 1 || r.f == 1) {\n"
            "    aval v; v.f = 1; v.d = ad(l) / ad(r); v.i = 0;\n"
            "    return v;\n"
            "  }\n"
            "  if (ai(r) == 0) {\n"
            "    fprintf(stderr, \"error: Division by zero.\\n\");\n"
            "    fprintf(stderr, \"  = help: Check the divisor before dividing.\\n\");\n"
            "    exit(2);\n"
            "  }\n"
            "  aval v; v.i = ai(l) / ai(r); v.d = 0; v.f = 0;\n"
            "  return v;\n"
            "}\n"
            "static aval a_mod(aval l, aval r) {\n"
            "  if (ad(r) == 0.0) {\n"
            "    fprintf(stderr, \"error: Remainder by zero.\\n\");\n"
            "    fprintf(stderr, \"  = help: Check the divisor before dividing.\\n\");\n"
            "    exit(2);\n"
            "  }\n"
            "  if (l.f == 1 || r.f == 1) {\n"
            "    aval v; v.f = 1; v.d = fmod(ad(l), ad(r)); v.i = 0;\n"
            "    return v;\n"
            "  }\n"
            "  aval v; v.i = ai(l) % ai(r); v.d = 0; v.f = 0;\n"
            "  return v;\n"
            "}\n"
            "static aval a_cat(aval l, aval r) {\n"
            "  int ls = (l.f == 2 && ((a_obj *)l.i)->type == 1);\n"
            "  int rs = (r.f == 2 && ((a_obj *)r.i)->type == 1);\n"
            "  if (ls || rs) {\n"
            "    aval L = ls ? l : a_strof(l);\n"
            "    aval R = rs ? r : a_strof(r);\n"
            "    a_obj *lo = (a_obj *)L.i, *ro = (a_obj *)R.i;\n"
            "    char *b = (char *)malloc((size_t)(lo->len + ro->len + 1));\n"
            "    if (!b) { printf(\"out of memory\\n\"); exit(1); }\n"
            "    memcpy(b, lo->data, (size_t)lo->len);\n"
            "    memcpy(b + lo->len, ro->data, (size_t)ro->len);\n"
            "    b[lo->len + ro->len] = 0;\n"
            "    aval v; v.f = 2; v.i = (a_i64)a_new_obj(1, lo->len + ro->len, 0, b); v.d = 0;\n"
            "    return v;\n"
            "  }\n"
            "  if (l.f == 1 || r.f == 1) {\n"
            "    aval v; v.f = 1; v.d = ad(l) + ad(r); v.i = 0;\n"
            "    return v;\n"
            "  }\n"
            "  aval v; v.i = ai(l) + ai(r); v.d = 0; v.f = 0;\n"
            "  return v;\n"
            "}\n"
            "static aval a_lenof(aval v) {\n"
            "  aval r; r.f = 0; r.d = 0;\n"
            "  if (v.f == 2) r.i = ((a_obj *)v.i)->len; else r.i = 0;\n"
            "  return r;\n"
            "}\n"
            "static aval a_dict_get(aval d, aval k);\n"
            "static aval a_index(aval o, aval ix) {\n"
            "  aval r; r.i = 0; r.d = 0; r.f = 0;\n"
            "  if (o.f != 2) return r;\n"
            "  a_obj *ob = (a_obj *)o.i;\n"
            "  if (ob->type == 3) return a_dict_get(o, ix);\n"
            "  a_i64 k = ai(ix);\n"
            "  if (k < 0 || k >= ob->len) return r;\n"
            "  if (ob->type == 2) return ((aval *)ob->data)[k];\n"
            "  if (ob->type == 1) {\n"
            "    char *b = (char *)malloc(2);\n"
            "    b[0] = ((char *)ob->data)[k]; b[1] = 0;\n"
            "    r.f = 2; r.i = (a_i64)a_new_obj(1, 1, 0, b);\n"
            "    return r;\n"
            "  }\n"
            "  return r;\n"
            "}\n"
            "static aval a_join(aval l, aval sep) {\n"
            "  aval r; r.i = 0; r.d = 0; r.f = 0;\n"
            "  if (l.f != 2 || ((a_obj *)l.i)->type != 2) return r;\n"
            "  a_obj *lo = (a_obj *)l.i;\n"
            "  aval sv = (sep.f == 2 && ((a_obj *)sep.i)->type == 1) ? sep : a_strof(sep);\n"
            "  a_obj *so = (a_obj *)sv.i;\n"
            "  a_i64 total = 0;\n"
            "  for (a_i64 k = 0; k < lo->len; ++k) {\n"
            "    aval e = ((aval *)lo->data)[k];\n"
            "    aval es = (e.f == 2 && ((a_obj *)e.i)->type == 1) ? e : a_strof(e);\n"
            "    total += ((a_obj *)es.i)->len;\n"
            "    if (k + 1 < lo->len) total += so->len;\n"
            "  }\n"
            "  char *b = (char *)malloc((size_t)total + 1);\n"
            "  if (!b) { printf(\"out of memory\\n\"); exit(1); }\n"
            "  a_i64 pos = 0;\n"
            "  for (a_i64 k = 0; k < lo->len; ++k) {\n"
            "    aval e = ((aval *)lo->data)[k];\n"
            "    aval es = (e.f == 2 && ((a_obj *)e.i)->type == 1) ? e : a_strof(e);\n"
            "    a_obj *eo = (a_obj *)es.i;\n"
            "    memcpy(b + pos, eo->data, (size_t)eo->len); pos += eo->len;\n"
            "    if (k + 1 < lo->len) {\n"
            "      memcpy(b + pos, so->data, (size_t)so->len); pos += so->len;\n"
            "    }\n"
            "  }\n"
            "  b[total] = 0;\n"
            "  r.f = 2; r.i = (a_i64)a_new_obj(1, total, 0, b);\n"
            "  return r;\n"
            "}\n"
            "static aval a_append(aval l, aval v) {\n"
            "  aval r; r.i = 0; r.d = 0; r.f = 0;\n"
            "  if (l.f == 2 && ((a_obj *)l.i)->type == 2) {\n"
            "    a_obj *o = (a_obj *)l.i;\n"
            "    if (o->len >= o->cap) {\n"
            "      a_i64 nc = o->cap ? o->cap * 2 : 8;\n"
            "      aval *nd = (aval *)malloc(sizeof(aval) * (size_t)nc);\n"
            "      if (!nd) { printf(\"out of memory\\n\"); exit(1); }\n"
            "      memcpy(nd, o->data, sizeof(aval) * (size_t)o->len);\n"
            "      o->data = nd; o->cap = nc;\n"
            "    }\n"
            "    ((aval *)o->data)[o->len++] = v;\n"
            "  }\n"
            "  return r;\n"
            "}\n"
            "static int a_aval_eq(aval a, aval b) {\n"
            "  if (a.f == 2 && b.f == 2) {\n"
            "    a_obj *ao = (a_obj *)a.i, *bo = (a_obj *)b.i;\n"
            "    if (ao->type == 1 && bo->type == 1)\n"
            "      return ao->len == bo->len && memcmp(ao->data, bo->data, (size_t)ao->len) == 0;\n"
            "    return ao == bo;\n"
            "  }\n"
            "  if (a.f == 1 || b.f == 1) return ad(a) == ad(b);\n"
            "  return ai(a) == ai(b);\n"
            "}\n"
            "static aval a_dict_get(aval d, aval k) {\n"
            "  aval r; r.i = 0; r.d = 0; r.f = 0;\n"
            "  if (d.f != 2 || ((a_obj *)d.i)->type != 3) return r;\n"
            "  a_obj *o = (a_obj *)d.i;\n"
            "  aval *pairs = (aval *)o->data;\n"
            "  for (a_i64 i = 0; i < o->len; ++i)\n"
            "    if (a_aval_eq(pairs[i * 2], k)) return pairs[i * 2 + 1];\n"
            "  {\n"
            "    char kb[96];\n"
            "    if (k.f == 2 && ((a_obj *)k.i)->type == 1)\n"
            "      snprintf(kb, sizeof kb, \"%s\", (char *)((a_obj *)k.i)->data);\n"
            "    else if (k.f == 1) a_fmt_double(kb, k.d);\n"
            "    else snprintf(kb, sizeof kb, \"%lld\", k.i);\n"
            "    fprintf(stderr, \"error: Key %s is not in the map.\\n\", kb);\n"
            "    exit(2);\n"
            "  }\n"
            "}\n"
            "static void a_dict_set(aval d, aval k, aval v) {\n"
            "  if (d.f != 2 || ((a_obj *)d.i)->type != 3) return;\n"
            "  a_obj *o = (a_obj *)d.i;\n"
            "  aval *pairs = (aval *)o->data;\n"
            "  for (a_i64 i = 0; i < o->len; ++i) {\n"
            "    if (a_aval_eq(pairs[i * 2], k)) { pairs[i * 2 + 1] = v; return; }\n"
            "  }\n"
            "  if (o->len >= o->cap) {\n"
            "    a_i64 nc = o->cap ? o->cap * 2 : 8;\n"
            "    aval *nd = (aval *)malloc(sizeof(aval) * (size_t)nc * 2);\n"
            "    if (!nd) { printf(\"out of memory\\n\"); exit(1); }\n"
            "    memcpy(nd, o->data, sizeof(aval) * (size_t)o->len * 2);\n"
            "    o->data = nd; o->cap = nc;\n"
            "    pairs = (aval *)o->data;\n"
            "  }\n"
            "  pairs[o->len * 2] = k;\n"
            "  pairs[o->len * 2 + 1] = v;\n"
            "  o->len += 1;\n"
            "}\n"
            "static aval a_dict_new(void) {\n"
            "  aval r; r.f = 2; r.d = 0;\n"
            "  r.i = (a_i64)a_new_obj(3, 0, 0, 0);\n"
            "  return r;\n"
            "}\n"
            "static aval a_abs(aval v) {\n"
            "  if (v.f == 1) { aval r; r.f = 1; r.d = fabs(v.d); r.i = 0; return r; }\n"
            "  aval r; r.i = v.i < 0 ? -v.i : v.i; r.d = 0; r.f = 0; return r;\n"
            "}\n"
            "static int a_aval_lt(aval a, aval b) {\n"
            "  if (a.f == 1 || b.f == 1) return ad(a) < ad(b);\n"
            "  return ai(a) < ai(b);\n"
            "}\n"
            "static aval a_minmax(aval l, aval r, int wantMax) {\n"
            "  int takeL = wantMax ? !a_aval_lt(l, r) : a_aval_lt(l, r);\n"
            "  return takeL ? l : r;\n"
            "}\n"
            "static aval a_sorted(aval l) {\n"
            "  aval r; r.f = 0; r.d = 0; r.i = 0;\n"
            "  if (l.f != 2 || ((a_obj *)l.i)->type != 2) return r;\n"
            "  a_obj *lo = (a_obj *)l.i;\n"
            "  aval *out = (aval *)malloc(sizeof(aval) * (size_t)(lo->len ? lo->len : 1));\n"
            "  if (!out) { printf(\"out of memory\\n\"); exit(1); }\n"
            "  for (a_i64 i = 0; i < lo->len; ++i) {\n"
            "    aval e = ((aval *)lo->data)[i]; a_i64 j = i - 1;\n"
            "    while (j >= 0 && a_aval_lt(e, out[j])) {\n"
            "      out[j + 1] = out[j]; --j;\n"
            "    }\n"
            "    out[j + 1] = e;\n"
            "  }\n"
            "  r.f = 2; r.i = (a_i64)a_new_obj(2, lo->len, lo->len, out);\n"
            "  return r;\n"
            "}\n"
            "static aval a_reversed(aval l) {\n"
            "  aval r; r.f = 0; r.d = 0; r.i = 0;\n"
            "  if (l.f != 2 || ((a_obj *)l.i)->type != 2) return r;\n"
            "  a_obj *lo = (a_obj *)l.i;\n"
            "  aval *out = (aval *)malloc(sizeof(aval) * (size_t)(lo->len ? lo->len : 1));\n"
            "  if (!out) { printf(\"out of memory\\n\"); exit(1); }\n"
            "  for (a_i64 i = 0; i < lo->len; ++i)\n"
            "    out[i] = ((aval *)lo->data)[lo->len - 1 - i];\n"
            "  r.f = 2; r.i = (a_i64)a_new_obj(2, lo->len, lo->len, out);\n"
            "  return r;\n"
            "}\n"
            "typedef aval (*a_fn)(aval);\n"
            "static aval a_map(aval l, aval f) {\n"
            "  aval r; r.f = 2; r.i = (a_i64)a_new_obj(2, 0, 0, 0); r.d = 0;\n"
            "  if (l.f == 2 && ((a_obj *)l.i)->type == 2 && f.f == 3) {\n"
            "    a_obj *lo = (a_obj *)l.i;\n"
            "    for (a_i64 k = 0; k < lo->len; ++k)\n"
            "      (void)a_append(r, ((a_fn)f.i)(((aval *)lo->data)[k]));\n"
            "  }\n"
            "  return r;\n"
            "}\n"
            "static aval a_filter(aval l, aval f) {\n"
            "  aval r; r.f = 2; r.i = (a_i64)a_new_obj(2, 0, 0, 0); r.d = 0;\n"
            "  if (l.f == 2 && ((a_obj *)l.i)->type == 2 && f.f == 3) {\n"
            "    a_obj *lo = (a_obj *)l.i;\n"
            "    for (a_i64 k = 0; k < lo->len; ++k) {\n"
            "      aval e = ((aval *)lo->data)[k];\n"
            "      aval keep = ((a_fn)f.i)(e);\n"
            "      if (keep.f ? keep.d != 0.0 : keep.i != 0) (void)a_append(r, e);\n"
            "    }\n"
            "  }\n"
            "  return r;\n"
            "}\n"
            "static aval a_sum(aval l) {\n"
            "  aval acc; acc.i = 0; acc.d = 0; acc.f = 0;\n"
            "  if (l.f == 2 && ((a_obj *)l.i)->type == 2) {\n"
            "    a_obj *lo = (a_obj *)l.i;\n"
            "    for (a_i64 k = 0; k < lo->len; ++k)\n"
            "      acc = a_cat(acc, ((aval *)lo->data)[k]);\n"
            "  }\n"
            "  return acc;\n"
            "}\n"
            "static void a_print_obj(a_obj *o) {\n"
            "  if (o->type == 3) {\n"
            "    printf(\"{\");\n"
            "    aval *pairs = (aval *)o->data;\n"
            "    for (a_i64 k = 0; k < o->len; ++k) {\n"
            "      if (k) printf(\", \");\n"
            "      aval key = pairs[k * 2];\n"
            "      if (key.f == 2 && ((a_obj *)key.i)->type == 1)\n"
            "        printf(\"\\\"%s\\\": \", (char *)((a_obj *)key.i)->data);\n"
            "      else if (key.f == 2) a_print_obj((a_obj *)key.i), printf(\": \");\n"
            "      else if (key.f == 1) { char b[64]; a_fmt_double(b, key.d); printf(\"%s: \", b); }\n"
            "      else printf(\"%lld: \", key.i);\n"
            "      aval val = pairs[k * 2 + 1];\n"
            "      if (val.f == 2) a_print_obj((a_obj *)val.i);\n"
            "      else if (val.f == 1) { char b[64]; a_fmt_double(b, val.d); printf(\"%s\", b); }\n"
            "      else printf(\"%lld\", val.i);\n"
            "    }\n"
            "    printf(\"}\");\n"
            "    return;\n"
            "  }\n"
            "  if (o->type == 1) { printf(\"\\\"%s\\\"\", (char *)o->data); return; }\n"
            "  if (o->type != 2) { printf(\"<obj>\"); return; }\n"
            "  printf(\"[\");\n"
            "  for (a_i64 k = 0; k < o->len; ++k) {\n"
            "    if (k) printf(\", \");\n"
            "    aval e = ((aval *)o->data)[k];\n"
            "    if (e.f == 2) a_print_obj((a_obj *)e.i);\n"
            "    else if (e.f == 1) { char b[64]; a_fmt_double(b, e.d); printf(\"%s\", b); }\n"
            "    else printf(\"%lld\", e.i);\n"
            "  }\n"
            "  printf(\"]\");\n"
            "}\n"
            "static void a_print_obj_to(void *s, a_obj *o) {\n"
            "  if (o->type == 3) {\n"
            "    fprintf(s, \"{\");\n"
            "    aval *pairs = (aval *)o->data;\n"
            "    for (a_i64 k = 0; k < o->len; ++k) {\n"
            "      if (k) fprintf(s, \", \");\n"
            "      aval key = pairs[k * 2];\n"
            "      if (key.f == 2 && ((a_obj *)key.i)->type == 1)\n"
            "        fprintf(s, \"\\\"%s\\\": \", (char *)((a_obj *)key.i)->data);\n"
            "      else if (key.f == 2) a_print_obj_to(s, (a_obj *)key.i), fprintf(s, \": \");\n"
            "      else if (key.f == 1) { char b[64]; a_fmt_double(b, key.d); fprintf(s, \"%s: \", b); }\n"
            "      else fprintf(s, \"%lld: \", key.i);\n"
            "      aval val = pairs[k * 2 + 1];\n"
            "      if (val.f == 2) a_print_obj_to(s, (a_obj *)val.i);\n"
            "      else if (val.f == 1) { char b[64]; a_fmt_double(b, val.d); fprintf(s, \"%s\", b); }\n"
            "      else fprintf(s, \"%lld\", val.i);\n"
            "    }\n"
            "    fprintf(s, \"}\");\n"
            "    return;\n"
            "  }\n"
            "  if (o->type == 1) { fprintf(s, \"\\\"%s\\\"\", (char *)o->data); return; }\n"
            "  if (o->type != 2) { fprintf(s, \"<obj>\"); return; }\n"
            "  fprintf(s, \"[\");\n"
            "  for (a_i64 k = 0; k < o->len; ++k) {\n"
            "    if (k) fprintf(s, \", \");\n"
            "    aval e = ((aval *)o->data)[k];\n"
            "    if (e.f == 2) a_print_obj_to(s, (a_obj *)e.i);\n"
            "    else if (e.f == 1) { char b[64]; a_fmt_double(b, e.d); fprintf(s, \"%s\", b); }\n"
            "    else fprintf(s, \"%lld\", e.i);\n"
            "  }\n"
            "  fprintf(s, \"]\");\n"
            "}\n"
            "static aval a_strof(aval v) {\n"
            "  char buf[64];\n"
            "  if (v.f == 2) {\n"
            "    a_obj *o = (a_obj *)v.i;\n"
            "    char *b = (char *)malloc((size_t)o->len + 1);\n"
            "    memcpy(b, o->data, (size_t)o->len); b[o->len] = 0;\n"
            "    aval r; r.f = 2; r.i = (a_i64)a_new_obj(1, o->len, 0, b); r.d = 0;\n"
            "    return r;\n"
            "  }\n"
            "  if (v.f) snprintf(buf, sizeof buf, \"%.*g\", 17, v.d);\n"
            "  else snprintf(buf, sizeof buf, \"%lld\", v.i);\n"
            "  a_i64 n = 0; while (buf[n]) ++n;\n"
            "  char *b = (char *)malloc((size_t)n + 1);\n"
            "  memcpy(b, buf, (size_t)n + 1);\n"
            "  aval r; r.f = 2; r.i = (a_i64)a_new_obj(1, n, 0, b); r.d = 0;\n"
            "  return r;\n"
            "}\n";
    // math decls live at the top (before helpers) to avoid implicit-decl conflicts
    out_ << "\n";

    for (const auto& n : globalNames_) out_ << "static aval g_" << n << " = {0};\n";
    if (!globalNames_.empty()) out_ << "\n";

    // Static string objects for literals (aval.f == 2 -> a_obj*).
    for (std::size_t si = 0; si < stringLits_.size(); ++si) {
      const std::string& lit = stringLits_[si];
      out_ << "static char a_sd_" << si << "[] = \"" << escapeC(lit) << "\";\n";
      out_ << "static a_obj a_so_" << si << " = {1, " << lit.size() << ", "
           << lit.size() << ", a_sd_" << si << "};\n";
    }
    if (!stringLits_.empty()) out_ << "\n";

    std::vector<const FnDecl*> fns;
    for (std::size_t i = 0; i < graph_.modules.size(); ++i) {
      const auto& mod = graph_.modules[i];
      if (!mod.program || isStdlibModuleC(graph_, mod)) continue;
      for (const auto& stmt : mod.program->statements) {
        if (stmt->kind == StmtKind::FnDecl)
          fns.push_back(static_cast<const FnDecl*>(stmt.get()));
      }
    }
    for (const FnDecl* fn : fns) {
      out_ << "aval afn_" << cIdent(fn->name) << "(";
      for (std::size_t i = 0; i < fn->params.size(); ++i) {
        if (i) out_ << ", ";
        out_ << "aval v_" << cIdent(fn->params[i].name);
      }
      out_ << ");\n";
    }
    if (!fns.empty()) out_ << "\n";
    for (const FnDecl* fn : fns) emitFunction(*fn);
    emitLambdaFunctions();
    emitMain();
    return true;
  }

 private:
  const ModuleGraph& graph_;
  const SemaResult& sema_;
  std::ostream& out_;
  bool inMain_ = false;
  int tmpTop_ = 0;
  std::set<std::string> globalNames_;
  std::unordered_map<std::string, VT> globalType_;
  std::set<std::string> locals_;
  std::unordered_map<std::string, VT> slotType_;
  std::vector<std::string> stringLits_;
  std::unordered_map<std::string, int> stringLitIdx_;
  std::vector<const LambdaExpr*> lambdas_;
  std::unordered_map<const LambdaExpr*, int> lambdaIdx_;

  static std::string cIdent(const std::string& name) {
    std::string r;
    for (char c : name) {
      r += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    }
    return r.empty() ? "_" : r;
  }

  void collectGlobals() {
    if (graph_.entry < 0 ||
        static_cast<std::size_t>(graph_.entry) >= graph_.modules.size()) return;
    const auto& root = graph_.modules[static_cast<std::size_t>(graph_.entry)];
    if (!root.program) return;
    for (const auto& stmt : root.program->statements) {
      std::string name;
      if (stmt->kind == StmtKind::Let) {
        name = static_cast<const LetStmt&>(*stmt).name;
      } else if (stmt->kind == StmtKind::Assign &&
                 static_cast<const AssignStmt&>(*stmt).target->kind == ExprKind::Ident) {
        name = static_cast<const IdentExpr&>(*static_cast<const AssignStmt&>(*stmt).target).name;
      }
      if (!name.empty()) globalNames_.insert(cIdent(name));
    }
  }

  // Static type of an expression: Int, Float, or Unknown (runtime tag decides).
  VT exprType(const Expr& e) {
    switch (e.kind) {
      case ExprKind::IntLit:
      case ExprKind::BoolLit:
      case ExprKind::Range:
        return VT::Int;
      case ExprKind::FloatLit:
        return VT::Float;
      case ExprKind::Ident: {
        std::string n = cIdent(static_cast<const IdentExpr&>(e).name);
        auto si = slotType_.find(n);
        if (si != slotType_.end()) return si->second;
        auto gi = globalType_.find(n);
        if (gi != globalType_.end()) return gi->second;
        return VT::Unknown;
      }
      case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(e);
        static const std::set<std::string> cmpOps = {"==","!=","<","<=",">",">="};
        static const std::set<std::string> bitOps = {"&","|","^","<<",">>","and","or"};
        if (cmpOps.count(b.op) || bitOps.count(b.op)) return VT::Int;
        static const std::set<std::string> arith = {"+","-","*","/","%"};
        if (!arith.count(b.op)) return VT::Int;
        VT l = exprType(*b.lhs), r = exprType(*b.rhs);
        if (l == VT::Float || r == VT::Float) return VT::Float;
        if (l == VT::Unknown || r == VT::Unknown) return VT::Unknown;
        return VT::Int;
      }
      case ExprKind::Unary: {
        const auto& u = static_cast<const UnaryExpr&>(e);
        if (u.op == "-") return exprType(*u.operand);
        return VT::Int;
      }
      case ExprKind::Call: {
        const auto& c = static_cast<const CallExpr&>(e);
        if (c.callee->kind != ExprKind::Ident) return VT::Unknown;
        std::string n = cIdent(static_cast<const IdentExpr&>(*c.callee).name);
        static const std::set<std::string> intFns = {"print", "int"};
        if (intFns.count(n)) return VT::Int;
        static const std::set<std::string> floatFns = {
            "sqrt","pow","fmod","floor","ceil","round","exp","log","sin","cos","tan",
            "fabs","f64","float"};
        if (floatFns.count(n)) return VT::Float;
        return VT::Unknown;  // user function: tag carried in the return value
      }
      default:
        return VT::Unknown;
    }
  }

  // Pre-type globals so functions (emitted above main) know whether a global
  // holds a double. Walks top-level statements in order.
  void pretypeGlobals() {
    if (graph_.entry < 0 ||
        static_cast<std::size_t>(graph_.entry) >= graph_.modules.size()) return;
    const auto& root = graph_.modules[static_cast<std::size_t>(graph_.entry)];
    if (!root.program) return;
    for (const auto& stmt : root.program->statements) {
      if (stmt->kind == StmtKind::Let) {
        const auto& s = static_cast<const LetStmt&>(*stmt);
        if (s.init && globalNames_.count(cIdent(s.name)))
          globalType_[cIdent(s.name)] = exprType(*s.init);
      } else if (stmt->kind == StmtKind::Assign &&
                 static_cast<const AssignStmt&>(*stmt).target->kind == ExprKind::Ident) {
        const auto& s = static_cast<const AssignStmt&>(*stmt);
        std::string n = cIdent(static_cast<const IdentExpr&>(*s.target).name);
        if (!globalNames_.count(n)) continue;
        VT vt = exprType(*s.value);
        if (s.op == "=") globalType_[n] = vt;
        else if (vt == VT::Float || globalType_[n] == VT::Float ||
                 vt == VT::Unknown || globalType_[n] == VT::Unknown)
          globalType_[n] = (vt == VT::Int && globalType_[n] == VT::Int) ? VT::Int
                                                                       : (vt == VT::Float || globalType_[n] == VT::Float ? VT::Float : VT::Unknown);
      }
    }
  }

  void collectLocalNames(const std::vector<StmtPtr>& body) {
    for (const auto& sp : body) {
      const Stmt& s = *sp;
      switch (s.kind) {
        case StmtKind::Let: {
          std::string n = cIdent(static_cast<const LetStmt&>(s).name);
          if (!inMain_ || !globalNames_.count(n)) locals_.insert(n);
          break;
        }
        case StmtKind::Assign: {
          const auto& a = static_cast<const AssignStmt&>(s);
          if (a.target->kind == ExprKind::Ident) {
            std::string n = cIdent(static_cast<const IdentExpr&>(*a.target).name);
            if (!globalNames_.count(n)) locals_.insert(n);
          }
          break;
        }
        case StmtKind::For: {
          const auto& f = static_cast<const ForStmt&>(s);
          std::string v = cIdent(f.var);
          locals_.insert(v);
          locals_.insert("__iter_" + v);
          locals_.insert("__end_" + v);
          collectLocalNames(f.body);
          break;
        }
        case StmtKind::If: {
          const auto& i = static_cast<const IfStmt&>(s);
          for (const auto& br : i.branches) collectLocalNames(br.body);
          collectLocalNames(i.elseBody);
          break;
        }
        case StmtKind::While:
          collectLocalNames(static_cast<const WhileStmt&>(s).body);
          break;
        default:
          break;
      }
    }
  }

  void collectExprStrings(const Expr& e) {
    switch (e.kind) {
      case ExprKind::StringLit: {
        const auto& s = static_cast<const StringLitExpr&>(e);
        std::string lit;
        for (const auto& p : s.parts) if (!p.isExpr) lit += p.text;
        if (!stringLitIdx_.count(lit)) {
          stringLitIdx_[lit] = static_cast<int>(stringLits_.size());
          stringLits_.push_back(lit);
        }
        break;
      }
      case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(e);
        collectExprStrings(*b.lhs);
        collectExprStrings(*b.rhs);
        break;
      }
      case ExprKind::Unary:
        collectExprStrings(*static_cast<const UnaryExpr&>(e).operand);
        break;
      case ExprKind::Range: {
        const auto& r = static_cast<const RangeExpr&>(e);
        collectExprStrings(*r.start);
        collectExprStrings(*r.end);
        if (r.step) collectExprStrings(*r.step);
        break;
      }
      case ExprKind::ArrayLit: {
        const auto& a = static_cast<const ArrayLitExpr&>(e);
        for (const auto& el : a.elements) collectExprStrings(*el);
        break;
      }
      case ExprKind::Index: {
        const auto& x = static_cast<const IndexExpr&>(e);
        collectExprStrings(*x.object);
        collectExprStrings(*x.index);
        break;
      }
      case ExprKind::Comprehension: {
        const auto& cp = static_cast<const ComprehensionExpr&>(e);
        collectExprStrings(*cp.element);
        collectExprStrings(*cp.iterable);
        if (cp.condition) collectExprStrings(*cp.condition);
        break;
      }
      case ExprKind::MapLit: {
        for (const auto& kv : static_cast<const MapLitExpr&>(e).entries) {
          collectExprStrings(*kv.first);
          collectExprStrings(*kv.second);
        }
        break;
      }
      case ExprKind::Call: {
        const auto& c = static_cast<const CallExpr&>(e);
        for (const auto& a : c.args) collectExprStrings(*a.value);
        break;
      }
      default:
        break;
    }
  }

  void collectStmtStrings(const Stmt& s) {
    switch (s.kind) {
      case StmtKind::Let: {
        const auto& l = static_cast<const LetStmt&>(s);
        if (l.init) collectExprStrings(*l.init);
        break;
      }
      case StmtKind::Assign: {
        const auto& a = static_cast<const AssignStmt&>(s);
        if (a.target->kind == ExprKind::Index) {
          const auto& ix = static_cast<const IndexExpr&>(*a.target);
          collectExprStrings(*ix.object);
          collectExprStrings(*ix.index);
        }
        collectExprStrings(*a.value);
        break;
      }
      case StmtKind::Expr:
        collectExprStrings(*static_cast<const ExprStmt&>(s).expr);
        break;
      case StmtKind::Return: {
        const auto& r = static_cast<const ReturnStmt&>(s);
        if (r.value) collectExprStrings(*r.value);
        break;
      }
      case StmtKind::If: {
        const auto& i = static_cast<const IfStmt&>(s);
        for (const auto& br : i.branches) {
          collectExprStrings(*br.cond);
          for (const auto& b : br.body) collectStmtStrings(*b);
        }
        for (const auto& b : i.elseBody) collectStmtStrings(*b);
        break;
      }
      case StmtKind::While: {
        const auto& w = static_cast<const WhileStmt&>(s);
        collectExprStrings(*w.cond);
        for (const auto& b : w.body) collectStmtStrings(*b);
        break;
      }
      case StmtKind::For: {
        const auto& f = static_cast<const ForStmt&>(s);
        collectExprStrings(*f.iterable);
        for (const auto& b : f.body) collectStmtStrings(*b);
        break;
      }
      default:
        break;
    }
  }

  void collectStrings() {
    for (const auto& mod : graph_.modules) {
      if (!mod.program || isStdlibModuleC(graph_, mod)) continue;
      for (const auto& stmt : mod.program->statements) {
        if (stmt->kind == StmtKind::FnDecl) {
          for (const auto& b : static_cast<const FnDecl&>(*stmt).body)
            collectStmtStrings(*b);
        } else {
          collectStmtStrings(*stmt);
        }
      }
    }
  }

  // ---- lambda collection (each becomes a real C function) ----
  void collectExprLambdas(const Expr& e) {
    switch (e.kind) {
      case ExprKind::Lambda: {
        const auto& l = static_cast<const LambdaExpr&>(e);
        if (!lambdaIdx_.count(&l)) {
          lambdaIdx_[&l] = static_cast<int>(lambdas_.size());
          lambdas_.push_back(&l);
        }
        collectExprLambdas(*l.body);
        break;
      }
      case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(e);
        collectExprLambdas(*b.lhs);
        collectExprLambdas(*b.rhs);
        break;
      }
      case ExprKind::Unary:
        collectExprLambdas(*static_cast<const UnaryExpr&>(e).operand);
        break;
      case ExprKind::Range: {
        const auto& r = static_cast<const RangeExpr&>(e);
        collectExprLambdas(*r.start);
        collectExprLambdas(*r.end);
        if (r.step) collectExprLambdas(*r.step);
        break;
      }
      case ExprKind::Index: {
        const auto& x = static_cast<const IndexExpr&>(e);
        collectExprLambdas(*x.object);
        collectExprLambdas(*x.index);
        break;
      }
      case ExprKind::ArrayLit: {
        for (const auto& el : static_cast<const ArrayLitExpr&>(e).elements)
          collectExprLambdas(*el);
        break;
      }
      case ExprKind::Comprehension: {
        const auto& cp = static_cast<const ComprehensionExpr&>(e);
        collectExprLambdas(*cp.element);
        collectExprLambdas(*cp.iterable);
        if (cp.condition) collectExprLambdas(*cp.condition);
        break;
      }
      case ExprKind::MapLit: {
        for (const auto& kv : static_cast<const MapLitExpr&>(e).entries) {
          collectExprLambdas(*kv.first);
          collectExprLambdas(*kv.second);
        }
        break;
      }
      case ExprKind::Call: {
        for (const auto& a : static_cast<const CallExpr&>(e).args)
          collectExprLambdas(*a.value);
        break;
      }
      default:
        break;
    }
  }

  void collectStmtLambdas(const Stmt& s) {
    switch (s.kind) {
      case StmtKind::Let: {
        const auto& l = static_cast<const LetStmt&>(s);
        if (l.init) collectExprLambdas(*l.init);
        break;
      }
      case StmtKind::Assign:
        collectExprLambdas(*static_cast<const AssignStmt&>(s).value);
        break;
      case StmtKind::Expr:
        collectExprLambdas(*static_cast<const ExprStmt&>(s).expr);
        break;
      case StmtKind::Return: {
        const auto& r = static_cast<const ReturnStmt&>(s);
        if (r.value) collectExprLambdas(*r.value);
        break;
      }
      case StmtKind::If: {
        const auto& i = static_cast<const IfStmt&>(s);
        for (const auto& br : i.branches) {
          collectExprLambdas(*br.cond);
          for (const auto& b : br.body) collectStmtLambdas(*b);
        }
        for (const auto& b : i.elseBody) collectStmtLambdas(*b);
        break;
      }
      case StmtKind::While: {
        const auto& w = static_cast<const WhileStmt&>(s);
        collectExprLambdas(*w.cond);
        for (const auto& b : w.body) collectStmtLambdas(*b);
        break;
      }
      case StmtKind::For: {
        const auto& f = static_cast<const ForStmt&>(s);
        collectExprLambdas(*f.iterable);
        for (const auto& b : f.body) collectStmtLambdas(*b);
        break;
      }
      default:
        break;
    }
  }

  void collectLambdas() {
    for (const auto& mod : graph_.modules) {
      if (!mod.program || isStdlibModuleC(graph_, mod)) continue;
      for (const auto& stmt : mod.program->statements) {
        if (stmt->kind == StmtKind::FnDecl) {
          for (const auto& b : static_cast<const FnDecl&>(*stmt).body)
            collectStmtLambdas(*b);
        } else {
          collectStmtLambdas(*stmt);
        }
      }
    }
  }

  // Emits each collected lambda as a standalone C function afn_l<N>.
  void emitLambdaFunctions() {
    for (std::size_t i = 0; i < lambdas_.size(); ++i) {
      const LambdaExpr& l = *lambdas_[i];
      locals_.clear();
      slotType_.clear();
      inMain_ = false;
      tmpTop_ = 0;
      for (const auto& p : l.params) {
        std::string n = cIdent(p.name);
        locals_.insert(n);
        slotType_[n] = VT::Unknown;
      }
      out_ << "aval afn_l" << i << "(";
      for (std::size_t j = 0; j < l.params.size(); ++j) {
        if (j) out_ << ", ";
        out_ << "aval v_" << cIdent(l.params[j].name);
      }
      out_ << ") {\n";
      for (const auto& n : locals_) {
        bool isParam = false;
        for (const auto& p : l.params) if (cIdent(p.name) == n) { isParam = true; break; }
        if (!isParam) out_ << "  aval v_" << n << " = {0};\n";
      }
      for (int t = 0; t < 2048; ++t) out_ << "  aval _t" << t << " = {0};\n";
      std::string rv = evalA(*l.body);
      out_ << "  return " << rv << ";\n";
      out_ << "}\n\n";
    }
  }

  std::string lvalue(const std::string& rawName) {
    std::string n = cIdent(rawName);
    if (locals_.count(n)) return "v_" + n;
    if (globalNames_.count(n)) return "g_" + n;
    return "v_" + n;
  }

  std::string identA(const std::string& rawName) {
    std::string n = cIdent(rawName);
    if (locals_.count(n)) return "v_" + n;
    if (globalNames_.count(n)) return "g_" + n;
    // Stream handles 1/2 agree with the interpreter's stdout/stderr.
    if (rawName == "stdout") return "(aval){.i=1,.f=0}";
    if (rawName == "stderr") return "(aval){.i=2,.f=0}";
    // Undeclared names (e.g. comprehension vars bound in an inner block)
    // still resolve to v_<n>; genuinely undefined names fail C compilation
    // and fall back to the interpreter.
    return "v_" + n;
  }
  bool isGlobalLV(const std::string& lv) { return lv.rfind("g_", 0) == 0; }

  std::string floatLit(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    std::string s = buf;
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
  }

  std::string escapeC(const std::string& s) {
    std::string r;
    for (char c : s) {
      switch (c) {
        case '\n': r += "\\n"; break;
        case '\t': r += "\\t"; break;
        case '\r': r += "\\r"; break;
        case '"': r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        default: r += c; break;
      }
    }
    return r;
  }

  // Fresh temp name; the aval _tN slots are declared at each function top.
  std::string tmp() { return "_t" + std::to_string(tmpTop_++); }

  // aval result of `l <op> r` where l/r are aval expressions and vt is the
  // statically-known result requirement (Int/Float emit direct ops; Unknown
  // emits a runtime tag dispatch into a temp).
  std::string avalBin(const std::string& op, const std::string& l,
                      const std::string& r, VT want) {
    // "/" and "%" always go through the runtime helpers: they handle int/float
    // promotion and zero-divisor checks for every operand combination.
    if (op == "/" || op == "%") {
      std::string tn = tmp();
      out_ << "  " << tn << " = a_" << (op == "/" ? "div" : "mod") << "("
           << l << ", " << r << ");\n";
      return tn;
    }
    if (want == VT::Int) {
      static const std::set<std::string> cmp = {"==","!=","<","<=",">",">="};
      if (cmp.count(op)) {
        std::string cop = op;
        return "(aval){.i=(" + std::string("ai(") + l + ") " + cop + " ai(" + r + ")), .f=0}";
      }
      static const std::set<std::string> bit = {"&","|","^","<<",">>","and","or"};
      std::string bop;
      if (op == "&" || op == "and") bop = "&";
      else if (op == "|" || op == "or") bop = "|";
      else if (op == "^") bop = "^";
      else if (op == "<<") bop = "<<";
      else if (op == ">>") bop = ">>";
      else bop = "+";
      if (op == "+" || op == "-") bop = op;
      else if (op == "*") bop = "*";
      if (bit.count(op)) {
        if (op == "<<") return "(aval){.i=(ai(" + l + ") << (ai(" + r + ") & 63)), .f=0}";
        if (op == ">>") return "(aval){.i=(ai(" + l + ") >> (ai(" + r + ") & 63)), .f=0}";
        return "(aval){.i=(ai(" + l + ") " + bop + " ai(" + r + ")), .f=0}";
      }
      // arith int
      if (op == "+") return "(aval){.i=(ai(" + l + ") + ai(" + r + ")), .f=0}";
      if (op == "-") return "(aval){.i=(ai(" + l + ") - ai(" + r + ")), .f=0}";
      if (op == "*") return "(aval){.i=(ai(" + l + ") * ai(" + r + ")), .f=0}";
      return "(aval){.i=0,.f=0}";
    }
    if (want == VT::Float) {
      if (op == "+") return "(aval){.d=(ad(" + l + ") + ad(" + r + ")), .f=1}";
      if (op == "-") return "(aval){.d=(ad(" + l + ") - ad(" + r + ")), .f=1}";
      if (op == "*") return "(aval){.d=(ad(" + l + ") * ad(" + r + ")), .f=1}";
      if (op == "/") return "(aval){.d=(ad(" + l + ") / ad(" + r + ")), .f=1}";
      if (op == "%") return "(aval){.d=fmod(ad(" + l + "), ad(" + r + ")), .f=1}";
      return "(aval){.d=0.0,.f=1}";
    }
    // Unknown: runtime dispatch on the tags.
    static const std::set<std::string> cmp = {"==","!=","<","<=",">",">="};
    static const std::set<std::string> bit = {"&","|","^","<<",">>","and","or"};
    std::string tn = tmp();
    if (cmp.count(op)) {
      out_ << "  if ((" << l << ").f || (" << r << ").f) " << tn
           << ".i = (ad(" << l << ") " << op << " ad(" << r << ")); else " << tn
           << ".i = (ai(" << l << ") " << op << " ai(" << r << "));\n";
      out_ << "  " << tn << ".f = 0;\n";
      return tn;
    }
    if (bit.count(op)) {
      // Bitwise ops are int-valued regardless of operand tags.
      std::string bop;
      if (op == "&" || op == "and") bop = "&";
      else if (op == "|" || op == "or") bop = "|";
      else if (op == "^") bop = "^";
      else if (op == "<<") bop = "SHL";
      else bop = "SHR";
      if (bop == "SHL") out_ << "  " << tn << ".i = ai(" << l << ") << (ai(" << r << ") & 63);\n";
      else if (bop == "SHR") out_ << "  " << tn << ".i = ai(" << l << ") >> (ai(" << r << ") & 63);\n";
      else out_ << "  " << tn << ".i = ai(" << l << ") " << bop << " ai(" << r << ");\n";
      out_ << "  " << tn << ".f = 0;\n";
      return tn;
    }
    // "+" and "*" dispatch through helpers that handle strings and
    // numeric promotion.
    if (op == "+" || op == "*") {
      std::string tn = tmp();
      out_ << "  " << tn << " = a_" << (op == "+" ? "cat" : "mul") << "(" << l << ", " << r << ");\n";
      return tn;
    }
    std::string iop, dop;
    if (op == "+") { iop = "+"; dop = "+"; }
    else if (op == "-") { iop = "-"; dop = "-"; }
    else if (op == "*") { iop = "*"; dop = "*"; }
    else if (op == "/") { iop = "/"; dop = "/"; }
    else if (op == "%") { iop = "%"; dop = "FMOD"; }
    else { iop = "+"; dop = "+"; }
    out_ << "  if ((" << l << ").f || (" << r << ").f) {\n";
    if (dop == "FMOD")
      out_ << "    " << tn << ".d = fmod(ad(" << l << "), ad(" << r << "));\n";
    else
      out_ << "    " << tn << ".d = ad(" << l << ") " << dop << " ad(" << r << ");\n";
    out_ << "    " << tn << ".f = 1;\n  } else {\n";
    out_ << "    " << tn << ".i = ai(" << l << ") " << iop << " ai(" << r << ");\n";
    out_ << "    " << tn << ".f = 0;\n  }\n";
    return tn;
  }

  std::string evalA(const Expr& e) {
    switch (e.kind) {
      case ExprKind::IntLit:
        return "(aval){.i=" + std::to_string(static_cast<const IntLitExpr&>(e).value) + ",.f=0}";
      case ExprKind::BoolLit:
        return std::string("(aval){.i=") + (static_cast<const BoolLitExpr&>(e).value ? "1" : "0") + ",.f=0}";
      case ExprKind::FloatLit:
        return "(aval){.d=" + floatLit(static_cast<const FloatLitExpr&>(e).value) + ",.f=1}";
      case ExprKind::Ident:
        return identA(static_cast<const IdentExpr&>(e).name);
      case ExprKind::StringLit: {
        const auto& s = static_cast<const StringLitExpr&>(e);
        std::string lit;
        for (const auto& p : s.parts) if (!p.isExpr) lit += p.text;
        auto it = stringLitIdx_.find(lit);
        if (it == stringLitIdx_.end()) return "(aval){.i=0,.f=2}";
        return "(aval){.i=(a_i64)&a_so_" + std::to_string(it->second) + ",.f=2}";
      }
      case ExprKind::ArrayLit: {
        const auto& a = static_cast<const ArrayLitExpr&>(e);
        std::string tn = tmp();
        out_ << "  {\n";
        out_ << "    a_obj *la_ = a_new_obj(2, " << a.elements.size() << ", "
             << a.elements.size() << ", malloc(sizeof(aval) * " << (a.elements.size() ? a.elements.size() : 1) << "));\n";
        out_ << "    " << tn << ".f = 2; " << tn << ".i = (a_i64)la_; " << tn << ".d = 0;\n";
        for (std::size_t i = 0; i < a.elements.size(); ++i) {
          std::string av = evalA(*a.elements[i]);
          out_ << "    ((aval *)la_->data)[" << i << "] = " << av << ";\n";
        }
        out_ << "  }\n";
        return tn;
      }
      case ExprKind::Lambda: {
        auto it = lambdaIdx_.find(static_cast<const LambdaExpr*>(&e));
        if (it == lambdaIdx_.end()) return "(aval){.i=0,.f=0}";
        return "(aval){.i=(a_i64)afn_l" + std::to_string(it->second) + ",.f=3}";
      }
      case ExprKind::Comprehension: {
        const auto& cp = static_cast<const ComprehensionExpr&>(e);
        int ti = tmpTop_++;
        std::string it = "_t" + std::to_string(ti);
        std::string res = "_t" + std::to_string(tmpTop_++);
        std::string vn = "v_" + cIdent(cp.var);
        out_ << "  {\n";
        out_ << "    aval " << vn << " = {0};\n";
        out_ << "    " << res << ".f = 2; " << res << ".i = (a_i64)a_new_obj(2, 0, 0, 0); "
             << res << ".d = 0;\n";
        bool isRange = cp.iterable->kind == ExprKind::Range;
        if (isRange) {
          const auto& r = static_cast<const RangeExpr&>(*cp.iterable);
          std::string is = evalI(*r.start);
          std::string ie = evalI(*r.end);
          if (r.step) {
            int si = tmpTop_++;
            std::string st = "_t" + std::to_string(si);
            std::string sv = evalI(*r.step);
            out_ << "    " << st << ".i = " << sv << "; " << st << ".f = 0;\n";
            out_ << "    if (" << st << ".i <= 0) { fprintf(stderr, \"error: The range step must be positive.\\n\"); exit(2); }\n";
            out_ << "    for (a_i64 _k" << ti << " = " << is << "; (" << st
                 << ".i > 0 ? _k" << ti << " < " << ie << " : _k" << ti << " > " << ie
                 << "); _k" << ti << " += " << st << ".i) {\n";
          } else {
            out_ << "    for (a_i64 _k" << ti << " = " << is << "; _k" << ti
                 << " < " << ie << "; ++_k" << ti << ") {\n";
          }
          out_ << "      " << vn << " = (aval){.i=_k" << ti << ",.f=0};\n";
        } else {
          std::string ov = evalA(*cp.iterable);
          out_ << "    " << it << " = " << ov << ";\n";
          out_ << "    if (" << it << ".f == 2) {\n";
          out_ << "      a_obj *_o" << ti << " = (a_obj *)" << it << ".i;\n";
          out_ << "      for (a_i64 _k" << ti << " = 0; _k" << ti << " < _o" << ti
               << "->len; ++_k" << ti << ") {\n";
          out_ << "        " << vn << " = a_index(" << it << ", (aval){.i=_k" << ti
               << ",.f=0});\n";
        }
        std::string indent = isRange ? "      " : "        ";
        if (cp.condition) {
          std::string cv = evalA(*cp.condition);
          out_ << indent << "if (!(" << cv << ".f ? " << cv << ".d != 0.0 : " << cv
               << ".i != 0)) continue;\n";
        }
        std::string ev = evalA(*cp.element);
        out_ << indent << "(void)a_append(" << res << ", " << ev << ");\n";
        out_ << (isRange ? "    }\n" : "      }\n    }\n");
        out_ << "  }\n";
        return res;
      }
      case ExprKind::MapLit: {
        const auto& m = static_cast<const MapLitExpr&>(e);
        int ti = tmpTop_++;
        std::string tn = "_t" + std::to_string(ti);
        out_ << "  " << tn << " = a_dict_new();\n";
        for (const auto& kv : m.entries) {
          std::string kvA = evalA(*kv.first);
          std::string vvA = evalA(*kv.second);
          out_ << "  a_dict_set(" << tn << ", " << kvA << ", " << vvA << ");\n";
        }
        return tn;
      }
      case ExprKind::Index: {
        const auto& x = static_cast<const IndexExpr&>(e);
        std::string ov = evalA(*x.object);
        std::string iv = evalA(*x.index);
        std::string tn = tmp();
        out_ << "  " << tn << " = a_index(" << ov << ", " << iv << ");\n";
        return tn;
      }
      case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(e);
        std::string l = evalA(*b.lhs);
        std::string r = evalA(*b.rhs);
        return avalBin(b.op, l, r, exprType(e));
      }
      case ExprKind::Unary: {
        const auto& u = static_cast<const UnaryExpr&>(e);
        std::string o = evalA(*u.operand);
        VT t = exprType(e);
        if (u.op == "-") {
          if (t == VT::Float) return "(aval){.d=(-ad(" + o + ")), .f=1}";
          if (t == VT::Int) return "(aval){.i=(-ai(" + o + ")), .f=0}";
          std::string tn = tmp();
          out_ << "  " << tn << " = " << o << ";\n";
          out_ << "  if (" << tn << ".f) " << tn << ".d = -" << tn << ".d; else " << tn << ".i = -" << tn << ".i;\n";
          return tn;
        }
        if (u.op == "~")
          return "(aval){.i=(~ai(" + o + ")), .f=0}";
        if (u.op == "not" || u.op == "!")
          return "(aval){.i=(!ai(" + o + ")), .f=0}";
        return "(aval){.i=0,.f=0}";
      }
      case ExprKind::Call: {
        const auto& c = static_cast<const CallExpr&>(e);
        if (c.callee->kind == ExprKind::Field) {
          const auto& f = static_cast<const FieldExpr&>(*c.callee);
          if (f.field == "append" && c.args.size() == 1) {
            std::string ov = evalA(*f.object);
            std::string av = evalA(*c.args[0].value);
            std::string tn = tmp();
            out_ << "  " << tn << " = a_append(" << ov << ", " << av << ");\n";
            return tn;
          }
          return "(aval){.i=0,.f=0}";
        }
        if (c.callee->kind != ExprKind::Ident) return "(aval){.i=0,.f=0}";
        std::string name = cIdent(static_cast<const IdentExpr&>(*c.callee).name);
        if (name == "print" || name == "eprint") {
          emitPrintCall(c, name == "eprint");
          return "(aval){.i=0,.f=0}";
        }
        if (name == "open" && !c.args.empty()) {
          std::string p = evalA(*c.args[0].value);
          std::string pn = tmp();
          out_ << "  " << pn << " = " << p << ";\n";
          std::string modeLit = "\"w\"";
          if (c.args.size() >= 2 && c.args[1].value->kind == ExprKind::StringLit) {
            modeLit = "\"" + escapeC(extractStringC(static_cast<const StringLitExpr&>(*c.args[1].value))) + "\"";
            out_ << "  return (aval){.i=a_open((char *)((a_obj *)" << pn << ".i)->data, " << modeLit << "),.f=0};\n";
          } else if (c.args.size() >= 2) {
            std::string m = evalA(*c.args[1].value);
            std::string mn = tmp();
            out_ << "  " << mn << " = " << m << ";\n";
            out_ << "  return (aval){.i=a_open((char *)((a_obj *)" << pn << ".i)->data, (char *)((a_obj *)" << mn << ".i)->data),.f=0};\n";
          } else {
            out_ << "  return (aval){.i=a_open((char *)((a_obj *)" << pn << ".i)->data, " << modeLit << "),.f=0};\n";
          }
          return "(aval){.i=0,.f=0}";
        }
        if (name == "close" && !c.args.empty()) {
          std::string h = evalA(*c.args[0].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = " << h << ";\n";
          out_ << "  a_close(" << tn << ".i);\n";
          return "(aval){.i=0,.f=0}";
        }
        if (name == "int" && !c.args.empty()) {
          return "(aval){.i=ai(" + evalA(*c.args[0].value) + "),.f=0}";
        }
        if (name == "len" && !c.args.empty()) {
          std::string av = evalA(*c.args[0].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = a_lenof(" << av << ");\n";
          return tn;
        }
        if (name == "str" && !c.args.empty()) {
          std::string av = evalA(*c.args[0].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = a_strof(" << av << ");\n";
          return tn;
        }
        if (name == "join" && c.args.size() >= 2) {
          std::string a0 = evalA(*c.args[0].value);
          std::string a1 = evalA(*c.args[1].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = a_join(" << a0 << ", " << a1 << ");\n";
          return tn;
        }
        if ((name == "map" || name == "filter") && c.args.size() >= 2) {
          std::string a0 = evalA(*c.args[0].value);
          std::string a1 = evalA(*c.args[1].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = a_" << name << "(" << a0 << ", " << a1 << ");\n";
          return tn;
        }
        if (name == "sum" && !c.args.empty()) {
          std::string a0 = evalA(*c.args[0].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = a_sum(" << a0 << ");\n";
          return tn;
        }
        if (name == "abs" && !c.args.empty()) {
          std::string a0 = evalA(*c.args[0].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = a_abs(" << a0 << ");\n";
          return tn;
        }
        if ((name == "sorted" || name == "reversed") && !c.args.empty()) {
          std::string a0 = evalA(*c.args[0].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = a_" << name << "(" << a0 << ");\n";
          return tn;
        }
        if ((name == "min" || name == "max") && c.args.size() >= 2) {
          std::string acc = evalA(*c.args[0].value);
          for (std::size_t i = 1; i < c.args.size(); ++i) {
            std::string av = evalA(*c.args[i].value);
            std::string tn = tmp();
            out_ << "  " << tn << " = a_minmax(" << acc << ", " << av << ", "
                 << (name == "max" ? 1 : 0) << ");\n";
            acc = tn;
          }
          return acc;
        }
        if ((name == "f64" || name == "float") && !c.args.empty()) {
          return "(aval){.d=ad(" + evalA(*c.args[0].value) + "),.f=1}";
        }
        static const std::set<std::string> libm1 = {
            "floor","ceil","round","exp","log","sin","cos","tan","fabs"};
        if (name == "sqrt" && !c.args.empty()) {
          return "(aval){.d=sqrt(ad(" + evalA(*c.args[0].value) + ")), .f=1}";
        }
        if ((name == "pow" || name == "fmod") && c.args.size() >= 2) {
          return "(aval){.d=" + name + "(ad(" + evalA(*c.args[0].value) + "), ad(" +
                 evalA(*c.args[1].value) + ")), .f=1}";
        }
        if (libm1.count(name) && !c.args.empty()) {
          return "(aval){.d=" + name + "(ad(" + evalA(*c.args[0].value) + ")), .f=1}";
        }
        // User function: evaluate args left-to-right, store result to a temp.
        std::string call = "afn_" + name + "(";
        for (std::size_t i = 0; i < c.args.size(); ++i) {
          if (i) call += ", ";
          call += evalA(*c.args[i].value);
        }
        call += ")";
        std::string tn = tmp();
        out_ << "  " << tn << " = " << call << ";\n";
        return tn;
      }
      default:
        return "(aval){.i=0,.f=0}";
    }
  }

  std::string evalI(const Expr& e) {
    std::string av = evalA(e);
    std::string tn = tmp();
    out_ << "  " << tn << " = " << av << ";\n";
    return "ai(" + tn + ")";
  }
  std::string evalD(const Expr& e) {
    std::string av = evalA(e);
    std::string tn = tmp();
    out_ << "  " << tn << " = " << av << ";\n";
    return "ad(" + tn + ")";
  }

  void emitPrint(const Expr& arg) {
    if (arg.kind == ExprKind::StringLit) {
      std::string lit = extractStringC(static_cast<const StringLitExpr&>(arg));
      out_ << "  printf(\"%s\\n\", \"" << escapeC(lit) << "\");\n";
      return;
    }
    std::string av = evalA(arg);
    std::string tn = tmp();
    out_ << "  " << tn << " = " << av << ";\n";
    out_ << "  if (" << tn << ".f == 2) {\n"
         << "    a_obj *o_ = (a_obj *)" << tn << ".i;\n"
         << "    if (o_->type == 1) printf(\"%s\\n\", (char *)o_->data);\n"
         << "    else a_print_obj(o_), printf(\"\\n\");\n"
         << "  } else if (" << tn << ".f) a_print_double(" << tn << ".d);"
         << " else printf(\"%lld\\n\", " << tn << ".i);\n";
  }

  // One printed object into `stream` (no separator/newline).
  void emitPrintValue(const std::string& stream, const Expr& arg) {
    if (arg.kind == ExprKind::StringLit) {
      std::string lit = extractStringC(static_cast<const StringLitExpr&>(arg));
      out_ << "  fputs(\"" << escapeC(lit) << "\", " << stream << ");\n";
      return;
    }
    std::string av = evalA(arg);
    std::string tn = tmp();
    out_ << "  " << tn << " = " << av << ";\n";
    out_ << "  if (" << tn << ".f == 2) {\n"
         << "    a_obj *o_ = (a_obj *)" << tn << ".i;\n"
         << "    if (o_->type == 1) fprintf(" << stream << ", \"%s\", (char *)o_->data);\n"
         << "    else a_print_obj_to(" << stream << ", o_);\n"
         << "  } else if (" << tn << ".f) { char buf_[64]; a_fmt_double(buf_, " << tn << ".d); fprintf(" << stream << ", \"%s\", buf_); }"
         << " else fprintf(" << stream << ", \"%lld\", " << tn << ".i);\n";
  }

  // Full print/eprint call with *objects plus sep/end/file/flush keywords.
  void emitPrintCall(const CallExpr& c, bool toStderr) {
    std::vector<const Expr*> objects;
    const Expr *sep = nullptr, *end = nullptr, *file = nullptr, *flush = nullptr;
    for (const auto& arg : c.args) {
      if (arg.name.empty()) objects.push_back(arg.value.get());
      else if (arg.name == "sep") sep = arg.value.get();
      else if (arg.name == "end") end = arg.value.get();
      else if (arg.name == "file") file = arg.value.get();
      else if (arg.name == "flush") flush = arg.value.get();
    }
    const std::string stream = tmp();
    if (file != nullptr) {
      std::string fh = evalA(*file);
      std::string tn = tmp();
      out_ << "  " << tn << " = " << fh << ";\n";
      out_ << "  void *" << stream << " = a_stream(" << tn << ".i);\n";
    } else {
      out_ << "  void *" << stream << " = " << (toStderr ? "stderr" : "stdout") << ";\n";
    }
    auto emitTextVar = [&](const Expr* expr, const char* def, const std::string& var) {
      if (expr == nullptr) {
        out_ << "  const char *" << var << " = \"" << def << "\";\n";
      } else if (expr->kind == ExprKind::StringLit) {
        out_ << "  const char *" << var << " = \"" << escapeC(extractStringC(static_cast<const StringLitExpr&>(*expr))) << "\";\n";
      } else {
        std::string av = evalA(*expr);
        std::string tn = tmp();
        out_ << "  " << tn << " = " << av << ";\n";
        out_ << "  const char *" << var << " = (" << tn << ".f == 2 ? (char *)((a_obj *)" << tn << ".i)->data : \"\");\n";
      }
    };
    emitTextVar(sep, " ", "sep_");
    emitTextVar(end, "\\n", "end_");
    for (std::size_t i = 0; i < objects.size(); ++i) {
      if (i) out_ << "  fputs(sep_, " << stream << ");\n";
      emitPrintValue(stream, *objects[i]);
    }
    out_ << "  fputs(end_, " << stream << ");\n";
    if (flush != nullptr) {
      std::string av = evalA(*flush);
      std::string tn = tmp();
      out_ << "  " << tn << " = " << av << ";\n";
      out_ << "  if (" << tn << ".i) fflush(" << stream << ");\n";
    } else if (toStderr) {
      out_ << "  fflush(" << stream << ");\n";
    }
  }

  void emitFunction(const FnDecl& fn) {
    locals_.clear();
    slotType_.clear();
    inMain_ = false;
    tmpTop_ = 0;
    for (const auto& p : fn.params) {
      std::string n = cIdent(p.name);
      locals_.insert(n);
      slotType_[n] = VT::Unknown;  // params carry their tag at runtime
    }
    collectLocalNames(fn.body);

    out_ << "aval afn_" << cIdent(fn.name) << "(";
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
      if (i) out_ << ", ";
      out_ << "aval v_" << cIdent(fn.params[i].name);
    }
    out_ << ") {\n";
    for (const auto& n : locals_) {
      bool isParam = false;
      for (const auto& p : fn.params) if (cIdent(p.name) == n) { isParam = true; break; }
      if (!isParam) out_ << "  aval v_" << n << " = {0};\n";
    }
    for (int i = 0; i < 2048; ++i) out_ << "  aval _t" << i << " = {0};\n";
    for (const auto& s : fn.body) emitStmt(*s);
    out_ << "  return (aval){.i=0,.f=0};\n";
    out_ << "}\n\n";
  }

  void setType(const std::string& lv, const std::string& name, VT vt) {
    if (isGlobalLV(lv)) globalType_[name] = vt;
    else slotType_[name] = vt;
  }

  void emitStmt(const Stmt& stmt) {
    // tmpTop_ is monotonic per function: temps must stay live across nested
    // statements (e.g. a for-loop's iterable), so they are only reset when a
    // function/main body starts.
    switch (stmt.kind) {
      case StmtKind::Let: {
        const auto& s = static_cast<const LetStmt&>(stmt);
        if (!s.init) break;
        std::string lv = lvalue(s.name);
        std::string av = evalA(*s.init);
        out_ << "  " << lv << " = " << av << ";\n";
        setType(lv, cIdent(s.name), exprType(*s.init));
        break;
      }
      case StmtKind::Assign: {
        const auto& s = static_cast<const AssignStmt&>(stmt);
        if (s.target->kind == ExprKind::Index && s.op == "=") {
          const auto& ix = static_cast<const IndexExpr&>(*s.target);
          std::string ov = evalA(*ix.object);
          std::string iv = evalA(*ix.index);
          std::string vv = evalA(*s.value);
          int ti = tmpTop_++;
          std::string tn = "_t" + std::to_string(ti);
          out_ << "  {\n    " << tn << " = " << ov << ";\n";
          out_ << "    if (" << tn << ".f == 2 && ((a_obj *)" << tn
               << ".i)->type == 2) {\n";
          out_ << "      a_i64 _k" << ti << " = ai(" << iv << ");\n";
          out_ << "      a_obj *_o" << ti << " = (a_obj *)" << tn << ".i;\n";
          out_ << "      if (_k" << ti << " >= 0 && _k" << ti << " < _o" << ti
               << "->len) ((aval *)_o" << ti << "->data)[_k" << ti << "] = " << vv << ";\n";
          out_ << "    } else if (" << tn << ".f == 2 && ((a_obj *)" << tn
               << ".i)->type == 3) {\n";
          out_ << "      a_dict_set(" << tn << ", " << iv << ", " << vv << ");\n";
          out_ << "    }\n  }\n";
          break;
        }
        if (s.target->kind != ExprKind::Ident) break;
        std::string raw = static_cast<const IdentExpr&>(*s.target).name;
        std::string n = cIdent(raw);
        std::string lv = lvalue(raw);
        VT curT = isGlobalLV(lv) ? globalType_[n] : slotType_[n];
        VT valT = exprType(*s.value);
        if (s.op == "=") {
          std::string av = evalA(*s.value);
          out_ << "  " << lv << " = " << av << ";\n";
          setType(lv, n, valT);
          break;
        }
        std::string base = s.op.substr(0, s.op.size() - 1);
        std::string rhs = evalA(*s.value);
        // Result type of the compound op.
        VT resT;
        static const std::set<std::string> bit = {"&","|","^","<<",">>","and","or"};
        static const std::set<std::string> cmp = {"==","!=","<","<=",">",">="};
        if (bit.count(base) || cmp.count(base)) resT = VT::Int;
        else if (curT == VT::Float || valT == VT::Float) resT = VT::Float;
        else if (curT == VT::Unknown || valT == VT::Unknown) resT = VT::Unknown;
        else resT = VT::Int;
        VT want = (curT == VT::Unknown || valT == VT::Unknown) ? VT::Unknown : resT;
        std::string res = avalBin(base, lv, rhs, want);
        out_ << "  " << lv << " = " << res << ";\n";
        setType(lv, n, resT);
        break;
      }
      case StmtKind::If: {
        const auto& s = static_cast<const IfStmt&>(stmt);
        for (std::size_t i = 0; i < s.branches.size(); ++i) {
          const auto& br = s.branches[i];
          std::string av = evalA(*br.cond);
          std::string tn = tmp();
          out_ << "  " << tn << " = " << av << ";\n";
          out_ << (i == 0 ? "  if (" : "  else if (") << tn << ".f ? " << tn
               << ".d != 0.0 : " << tn << ".i != 0) {\n";
          for (const auto& b : br.body) emitStmt(*b);
          out_ << "  }";
        }
        if (!s.elseBody.empty()) {
          out_ << (s.branches.empty() ? "  {\n" : " else {\n");
          for (const auto& b : s.elseBody) emitStmt(*b);
          out_ << "  }";
        }
        out_ << "\n";
        break;
      }
      case StmtKind::While: {
        const auto& s = static_cast<const WhileStmt&>(stmt);
        std::string tn = tmp();
        out_ << "  aval " << tn << " = {0};\n";
        out_ << "  while (1) {\n";
        std::string av = evalA(*s.cond);
        out_ << "    " << tn << " = " << av << ";\n";
        out_ << "    if (!(" << tn << ".f ? " << tn << ".d != 0.0 : " << tn << ".i != 0)) break;\n";
        for (const auto& b : s.body) emitStmt(*b);
        out_ << "  }\n";
        break;
      }
      case StmtKind::For: {
        const auto& s = static_cast<const ForStmt&>(stmt);
        std::string v = cIdent(s.var);
        std::string varN = "v_" + v;
        slotType_[v] = VT::Unknown;
        if (s.iterable->kind == ExprKind::Range) {
          const auto& r = static_cast<const RangeExpr&>(*s.iterable);
          std::string itN = "v___iter_" + v;
          std::string endN = "v___end_" + v;
          slotType_[v] = VT::Int;
          slotType_["__iter_" + v] = VT::Int;
          slotType_["__end_" + v] = VT::Int;
          std::string is = evalI(*r.start);
          std::string ie = evalI(*r.end);
          out_ << "  " << itN << ".i = " << is << "; " << itN << ".f = 0;\n";
          out_ << "  " << endN << ".i = " << ie << "; " << endN << ".f = 0;\n";
          if (r.step) {
            int si = tmpTop_++;
            std::string st = "_t" + std::to_string(si);
            std::string sv = evalI(*r.step);
            out_ << "  " << st << ".i = " << sv << "; " << st << ".f = 0;\n";
            out_ << "  if (" << st << ".i <= 0) { fprintf(stderr, \"error: The range step must be positive.\\n\"); exit(2); }\n";
            out_ << "  for (; (" << st << ".i > 0 ? " << itN << ".i < " << endN
                 << ".i : " << itN << ".i > " << endN << ".i); "
                 << itN << ".i = " << itN << ".i + " << st << ".i) {\n";
          } else {
            out_ << "  for (; " << itN << ".i < " << endN << ".i; "
                 << itN << ".i = " << itN << ".i + 1) {\n";
          }
          out_ << "    " << varN << " = " << itN << ";\n";
          for (const auto& b : s.body) emitStmt(*b);
          out_ << "  }\n";
          break;
        }
        // Generic iterable (list/string): index-loop via a_index.
        int ti = tmpTop_++;
        std::string it = "_t" + std::to_string(ti);
        std::string ov = evalA(*s.iterable);
        out_ << "  " << it << " = " << ov << ";\n";
        out_ << "  if (" << it << ".f == 2) {\n";
        out_ << "    a_obj *_o" << ti << " = (a_obj *)" << it << ".i;\n";
        out_ << "    for (a_i64 _k" << ti << " = 0; _k" << ti << " < _o" << ti
             << "->len; ++_k" << ti << ") {\n";
        out_ << "      " << varN << " = a_index(" << it << ", (aval){.i=_k" << ti
             << ",.f=0});\n";
        for (const auto& b : s.body) emitStmt(*b);
        out_ << "    }\n  }\n";
        break;
      }
      case StmtKind::Break:
        out_ << "    break;\n";
        break;
      case StmtKind::Continue:
        out_ << "    continue;\n";
        break;
      case StmtKind::Return: {
        const auto& s = static_cast<const ReturnStmt&>(stmt);
        if (inMain_) {
          if (s.value) {
            std::string av = evalA(*s.value);
            std::string tn = tmp();
            out_ << "  " << tn << " = " << av << ";\n";
          }
          out_ << "  return 0;\n";
          break;
        }
        std::string rv = s.value ? evalA(*s.value) : "(aval){.i=0,.f=0}";
        out_ << "  return " << rv << ";\n";
        break;
      }
      case StmtKind::Expr: {
        const auto& s = static_cast<const ExprStmt&>(stmt);
        if (s.expr->kind == ExprKind::Call) {
          const auto& c = static_cast<const CallExpr&>(*s.expr);
          if (c.callee->kind == ExprKind::Ident) {
            const std::string n = cIdent(static_cast<const IdentExpr&>(*c.callee).name);
            if (n == "print" || n == "eprint") {
              emitPrintCall(c, n == "eprint");
              break;
            }
          }
        }
        std::string ev = evalA(*s.expr);
        out_ << "  (void)" << ev << ";\n";
        break;
      }
      default:
        break;
    }
  }

  void emitMain() {
    locals_.clear();
    slotType_.clear();
    inMain_ = true;
    tmpTop_ = 0;

    out_ << "int main(void) {\n";
    if (graph_.entry >= 0 &&
        static_cast<std::size_t>(graph_.entry) < graph_.modules.size()) {
      const auto& root = graph_.modules[static_cast<std::size_t>(graph_.entry)];
      if (root.program) {
        collectLocalNames(root.program->statements);
        for (const auto& n : locals_) out_ << "  aval v_" << n << " = {0};\n";
        for (int i = 0; i < 2048; ++i) out_ << "  aval _t" << i << " = {0};\n";
        for (const auto& stmt : root.program->statements) {
          if (stmt->kind != StmtKind::FnDecl) emitStmt(*stmt);
        }
      }
    }
    out_ << "  return 0;\n";
    out_ << "}\n";
  }
};

}  // namespace

bool emitCSource(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out) {
  CEmitter emitter(graph, sema, out);
  return emitter.emit();
}

}  // namespace a
