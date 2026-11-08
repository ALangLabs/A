// Native per-arch assembly backend (A -> .s -> astra -> machine code).
// Unlike emitC, this emits target assembly directly: every A value is an
// aval word held in an 8-byte frame slot; expressions evaluate into a
// single accumulator register; all semantics go through the a_*_slow
// runtime externs (the header fast paths are C-only). Unsupported
// constructs fail emission so the pipeline can fall back to emitC.
//
// The NatTarget interface abstracts the per-arch primitives (frame layout,
// slot addressing, constant materialization, symbol access, calls, and
// branches). Arm64 is implemented first; other targets plug in by adding a
// NatTarget subclass.
#include "codegen/codegen.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <memory>
#include <set>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast.hpp"
#include "codegen/codegen_internal.hpp"
#include "name-resolution/module_loader.hpp"
#include "name-resolution/native_modules.hpp"
#include "type-checker/sema.hpp"
#include "type-checker/method_signatures.hpp"
#include "type-checker/types.hpp"

namespace a {
namespace {

// ---------------------------------------------------------------- target --
// Per-arch primitive layer. All aval values are 64-bit words; on 64-bit
// targets one register holds an aval, on 32-bit targets pairs/slots are
// used (added later — emitNative returns false for unimplemented arches).
struct NatTarget {
  virtual ~NatTarget() = default;
  virtual int frameReserve() const { return 0; }  // extra bytes inside frame (e.g. ra)
  virtual std::string extraRodata() const { return ""; }
  virtual int ptrSize() const { return 8; }  // C pointer width (a_test_main arrays)
  // Bare-metal targets (i8086): no libc, tiny address space — skip
  // function-name/struct registration metadata and GC dead functions.
  virtual bool bareMetal() const { return false; }
  virtual void prologue(std::string& o, int frameBytes) const = 0;
  virtual void epilogue(std::string& o, int frameBytes) const = 0;
  virtual std::string slotRef(int i) const = 0;   // e.g. "[sp, #8]"
  virtual std::string acc() const = 0;            // accumulator reg
  virtual int nArgRegs() const = 0;
  virtual std::string argReg(int i) const = 0;
  virtual std::string tmpReg(int i) const = 0;    // caller-saved scratch
  virtual void movImm(std::string& o, uint64_t v, const std::string& r) const = 0;
  virtual void movReg(std::string& o, const std::string& d, const std::string& s) const = 0;
  virtual void loadSlot(std::string& o, int slot, const std::string& r) const = 0;
  virtual void storeSlot(std::string& o, const std::string& r, int slot) const = 0;
  // Load [baseReg + byteOff] into dstReg (for argv[i] access).
  virtual void loadSlotAt(std::string& o, const std::string& baseReg,
                          int64_t byteOff, const std::string& dstReg) const = 0;
  virtual void copySlot(std::string& o, int from, int to) const = 0;
  virtual void leaSlot(std::string& o, int slot, const std::string& r) const = 0;
  virtual void leaSym(std::string& o, const std::string& sym, const std::string& r) const = 0;
  virtual void loadSym(std::string& o, const std::string& sym, const std::string& r) const = 0;
  virtual void storeSym(std::string& o, const std::string& sym, const std::string& r) const = 0;
  virtual void callSym(std::string& o, const std::string& sym) const = 0;
  virtual void jmp(std::string& o, const std::string& l) const = 0;
  // cc in {"eq","ne","lt","le","gt","ge"}
  virtual void jcc(std::string& o, const std::string& cc, const std::string& l) const = 0;
  virtual void jz(std::string& o, const std::string& r, const std::string& l) const = 0;
  virtual void jnz(std::string& o, const std::string& r, const std::string& l) const = 0;
  virtual void cmpRegs(std::string& o, const std::string& a, const std::string& b) const = 0;
  virtual void addImm(std::string& o, const std::string& r, int64_t v) const = 0;
  virtual void zeroReg(std::string& o, const std::string& r) const = 0;
  virtual void ret(std::string& o) const = 0;
};

// AArch64 (AAPCS64): acc/arg0 = x0, args x0..x7, scratch x8..x11.
// Frame: stp x29,x30,[sp,#-16]!; sub sp,#frame; slots at [sp,#8*i].
struct Arm64Target : NatTarget {
  void prologue(std::string& o, int frameBytes) const override {
    o += "    stp x29, x30, [sp, #-16]!\n    sub sp, sp, #" +
         std::to_string(frameBytes) + "\n";
  }
  void epilogue(std::string& o, int frameBytes) const override {
    o += "    add sp, sp, #" + std::to_string(frameBytes) + "\n" +
         "    ldp x29, x30, [sp], #16\n    ret\n";
  }
  std::string slotRef(int i) const override {
    return "[sp, #" + std::to_string(8 * i) + "]";
  }
  std::string acc() const override { return "x0"; }
  int nArgRegs() const override { return 8; }
  std::string argReg(int i) const override { return "x" + std::to_string(i); }
  std::string tmpReg(int i) const override { return "x" + std::to_string(8 + i); }
  void movImm(std::string& o, uint64_t v, const std::string& r) const override {
    // movz + movk chain; skip zero chunks (movz clears the rest).
    unsigned chunks[4] = {(unsigned)(v & 0xFFFF), (unsigned)((v >> 16) & 0xFFFF),
                          (unsigned)((v >> 32) & 0xFFFF), (unsigned)((v >> 48) & 0xFFFF)};
    char buf[64];
    std::snprintf(buf, sizeof buf, "    movz %s, #0x%x\n", r.c_str(), chunks[0]);
    o += buf;
    for (int i = 1; i < 4; ++i) {
      if (!chunks[i]) continue;
      std::snprintf(buf, sizeof buf, "    movk %s, #0x%x, lsl #%d\n",
                    r.c_str(), chunks[i], i * 16);
      o += buf;
    }
  }
  void movReg(std::string& o, const std::string& d, const std::string& s) const override {
    o += "    mov " + d + ", " + s + "\n";
  }
  void loadSlot(std::string& o, int slot, const std::string& r) const override {
    o += "    ldr " + r + ", " + slotRef(slot) + "\n";
  }
  void storeSlot(std::string& o, const std::string& r, int slot) const override {
    o += "    str " + r + ", " + slotRef(slot) + "\n";
  }
  void loadSlotAt(std::string& o, const std::string& baseReg,
                  int64_t byteOff, const std::string& dstReg) const override {
    o += "    ldr " + dstReg + ", [" + baseReg + ", #" +
         std::to_string(byteOff) + "]\n";
  }
  void copySlot(std::string& o, int from, int to) const override {
    o += "    ldr x9, " + slotRef(from) + "\n    str x9, " + slotRef(to) + "\n";
  }
  void leaSlot(std::string& o, int slot, const std::string& r) const override {
    o += "    add " + r + ", sp, #" + std::to_string(8 * slot) + "\n";
  }
  void leaSym(std::string& o, const std::string& sym, const std::string& r) const override {
    o += "    adr " + r + ", " + sym + "\n";
  }
  void loadSym(std::string& o, const std::string& sym, const std::string& r) const override {
    o += "    adr x9, " + sym + "\n    ldr " + r + ", [x9]\n";
  }
  void storeSym(std::string& o, const std::string& sym, const std::string& r) const override {
    o += "    adr x9, " + sym + "\n    str " + r + ", [x9]\n";
  }
  void callSym(std::string& o, const std::string& sym) const override {
    o += "    bl " + sym + "\n";
  }
  void jmp(std::string& o, const std::string& l) const override {
    o += "    b " + l + "\n";
  }
  void jcc(std::string& o, const std::string& cc, const std::string& l) const override {
    static const std::unordered_map<std::string, std::string> m = {
        {"eq", "beq"}, {"ne", "bne"}, {"lt", "blt"}, {"le", "ble"},
        {"gt", "bgt"}, {"ge", "bge"}};
    o += "    " + m.at(cc) + " " + l + "\n";
  }
  void jz(std::string& o, const std::string& r, const std::string& l) const override {
    o += "    cbz " + r + ", " + l + "\n";
  }
  void jnz(std::string& o, const std::string& r, const std::string& l) const override {
    o += "    cbnz " + r + ", " + l + "\n";
  }
  void cmpRegs(std::string& o, const std::string& a, const std::string& b) const override {
    o += "    subs xzr, " + a + ", " + b + "\n";
  }
  void addImm(std::string& o, const std::string& r, int64_t v) const override {
    o += "    add " + r + ", " + r + ", #" + std::to_string(v) + "\n";
  }
  void zeroReg(std::string& o, const std::string& r) const override {
    o += "    mov " + r + ", #0\n";
  }
  void ret(std::string& o) const override { o += "    ret\n"; }
};

// ------------------------------------------------------------- emitter ----
int builtinRuntimeId(const std::string& name) {
  static const std::unordered_map<std::string, int> kMap = {
      {"print", 0}, {"eprint", 1}, {"len", 2}, {"str", 3}, {"int", 4},
      {"float", 5}, {"f64", 5}, {"i64", 4}, {"bool", 6}, {"abs", 7},
      {"min", 8}, {"max", 9}, {"sum", 10}, {"pow", 11}, {"sqrt", 12},
      {"sin", 13}, {"cos", 14}, {"tan", 15}, {"asin", 16}, {"acos", 17},
      {"atan", 18}, {"exp", 19}, {"log", 20}, {"floor", 21}, {"ceil", 22},
      {"round", 23}, {"trunc", 24}, {"random", 25}, {"random_int", 26},
      {"range", 27}, {"set", 28}, {"tuple", 29}, {"list", 30}, {"dict", 31},
      {"map", 32}, {"filter", 33}, {"any", 34}, {"all", 35}, {"some", 36},
      {"ok", 37}, {"err", 38}, {"panic", 39}, {"assert", 40},
      {"assert_eq", 41}, {"input", 42}, {"read_file", 43},
      {"write_file", 44}, {"append_file", 45}, {"file_exists", 46},
      {"list_dir", 47}, {"remove_file", 48}, {"create_dir", 49},
      {"remove_dir", 50}, {"rename", 51}, {"current_dir", 52}, {"repr", 53},
      {"type", 54}, {"isinstance", 55}, {"join", 56}, {"sorted", 57},
      {"reversed", 58}, {"enumerate", 59}, {"zip", 60}, {"reduce", 61},
      {"log2", 62}, {"log10", 63}, {"atan2", 64}, {"flush", 65}, {"i8", 66},
      {"i16", 67}, {"i32", 68}, {"u8", 69}, {"u16", 70}, {"u32", 71}, {"f32", 72},
      {"ctz", 73}, {"clz", 74}, {"popcount", 75}, {"bswap", 76}, {"rotl", 77},
      {"rotr", 78}, {"fma", 79}, {"fabs", 80}, {"copysign", 81},
      {"smin", 82}, {"smax", 83}, {"umin", 84}, {"umax", 85},
      {"sadd_overflow", 86}, {"ssub_overflow", 87}, {"smul_overflow", 88},
      {"uadd_overflow", 89}, {"usub_overflow", 90}, {"umul_overflow", 91},
      {"expect", 92}, {"likely", 93}, {"unlikely", 94}, {"env_get", 95},
      {"now", 96}, {"sleep", 97},
      {"terminal_isatty", 98}, {"terminal_raw_mode", 99},
      {"terminal_read_char", 100}, {"terminal_read_char_ms", 101},
      {"terminal_write", 102}, {"terminal_flush", 103},
      {"terminal_size", 104}, {"terminal_home", 105},
      {"repl_eval", 106},
      {"json_parse", 107}, {"json_stringify", 108}, {"json_pretty", 109},
      {"json_is_valid", 110}, {"json_get", 111}, {"json_at", 112},
      {"json_len", 113}, {"json_has", 114}, {"json_keys", 115},
      {"json_type", 116}, {"json_as_str", 117}, {"json_as_int", 118},
      {"json_as_float", 119}, {"json_as_bool", 120},
      {"json_as_array", 121}, {"json_as_map", 122},
      {"repl_quiet", 123},
      {"binary_encode", 124}, {"binary_decode", 125},
      {"log_set_level", 126}, {"log_set_json", 127},
      {"log_debug", 128}, {"log_info", 129}, {"log_warn", 130},
      {"log_error", 131},
      {"metrics_counter", 132}, {"metrics_gauge", 133},
      {"metrics_histogram", 134}, {"metrics_render", 135},
      {"metrics_reset", 136},
      {"trace_start", 137}, {"trace_end", 138}, {"trace_current", 139},
      {"trace_set_attribute", 140}, {"trace_add_event", 141},
      {"trace_inject", 142}, {"trace_extract", 143}, {"trace_spans", 144},
      {"trace_reset", 145},
      {"net_resolve", 146}, {"net_tls_available", 147},
      {"arena_new", 148}, {"arena_pool", 149}, {"arena_region", 150},
      {"arena_alloc", 151}, {"arena_release", 152}, {"arena_reset", 153},
      {"arena_used", 154}, {"arena_total", 155},
      {"unsafe_get", 156}, {"unsafe_set", 157},
      {"unsafe_address_of", 158}, {"unsafe_load", 159},
      {"unsafe_store", 160}, {"unsafe_offset", 161},
      {"py_call", 162}, {"py_buffer", 163},
      {"py_import_module", 164}, {"py_try_import_module", 165},
      {"py_getattr", 166}, {"py_try_getattr", 167},
      {"py_setattr", 168}, {"py_hasattr", 169}, {"py_delattr", 170},
      {"py_getitem", 171}, {"py_setitem", 172}, {"py_delitem", 173},
      {"py_call_obj", 174}, {"py_try_call_obj", 175},
      {"py_call_method", 176}, {"py_try_call_method", 177},
      {"py_is_pyobject", 178}, {"py_str", 179}, {"py_repr", 180},
      {"py_len", 181}, {"py_contains", 182}, {"py_truthy", 183},
      {"py_type", 184}, {"py_buffer_info", 185},
      {"json_set", 186}, {"json_delete", 187},
      {"struct_name", 188}, {"struct_methods", 189},
      {"fn_signature", 190}, {"fn_doc", 191},
      {"file_size", 192},
      {"linalg_dot", 193}, {"linalg_matvec", 194}, {"linalg_matvec_t", 195},
      {"dot", 193}, {"matvec", 194}, {"matvec_t", 195},
      {"linalg_matmul", 219}, {"linalg_softmax", 220}, {"linalg_attention", 221},
      {"matmul", 219}, {"softmax", 220}, {"attention", 221},
      {"net_tcp_connect", 196}, {"net_send", 197}, {"net_recv", 198},
      {"net_close", 199}, {"net_set_timeout", 200}, {"net_local_port", 201},
      {"net_tls_connect", 202},
      {"net_recv_status", 203},
      {"read_bytes", 204}, {"write_bytes", 205},
      {"net_tcp_listen", 206}, {"net_accept", 207},
      {"net_set_nonblocking", 208}, {"net_poll", 209},
      {"net_udp_socket", 210}, {"net_udp_bind", 211},
      {"net_udp_send", 212}, {"net_udp_recv", 213},
      {"net_send_bytes", 214}, {"net_recv_bytes", 215},
      {"net_tls_wrap", 216}, {"net_inflate_bytes", 217},
            {"net_inflate_available", 218},
      {"bytes_decode_utf8", 222}, {"str_encode_utf8", 223},
      {"net_recv_exact", 224}, {"net_recv_body", 225}, {"net_recv_body_all", 226},
      {"bytes_new", 227}, {"Bytes", 227}, {"bytes_append", 228}, {"bytes_to_str", 229},
      {"net_recv_buf", 230}, {"net_recv_buf_all", 231},
  };
  auto it = kMap.find(name);
  return it == kMap.end() ? -1 : it->second;
}

struct FnInfo {
  const FnDecl* decl = nullptr;
  std::string cname;
  std::string globalVar;
  std::vector<std::string> paramNames;
  std::vector<const Expr*> paramDefaults;
  bool variadic = false;
  bool isMethod = false;
  std::string structName;
  int moduleId = -1;
  std::string name;
};

struct StructInfo {
  const StructDecl* decl = nullptr;
  std::string ctorCname;
  std::string globalVar;
  std::string nameLabel;  // unique __na_sn_* rodata label (names may repeat
                          // across modules, e.g. prelude vs std/prelude)
  std::vector<std::string> fieldNames;
  std::unordered_map<std::string, FnInfo*> methods;
  int moduleId = -1;
  std::string name;
};

struct EnumVariantInfo {
  std::string globalVar;
  std::int64_t value = 0;
  bool algebraic = false;
  int tag = 0;
  std::vector<std::string> payloadFields;
  int moduleId = -1;
  std::string enumName;
  std::string variantName;
};

enum class BKind { Local, Global, Param, FnValue, Builtin, StructCtor,
                   EnumValue, Module, Capture, NativeConst, PyModule, PyFunc };

struct ABinding {
  BKind kind = BKind::Local;
  int slot = -1;
  std::string sym;
  int builtinId = -1;
  FnInfo* fn = nullptr;
  StructInfo* st = nullptr;
  EnumVariantInfo* en = nullptr;
  int moduleId = -1;
  int envIdx = -1;   // BKind::Capture: index into closure env
  std::string aux;   // BKind::Module: native module name (moduleId < 0);
                     // BKind::PyModule/PyFunc: python module path
  std::string pyMember;  // BKind::PyFunc: python member name
  FnInfo* dropFn = nullptr;  // RAII: struct `drop` method for this binding
};

class NativeEmitter {
 public:
  NativeEmitter(const ModuleGraph& graph, const SemaResult& sema,
                const NatTarget& target, bool testMode,
                const std::string* testFilter = nullptr)
      : graph_(graph), sema_(sema), t_(target), testMode_(testMode),
        testFilter_(testFilter ? *testFilter : "") {}

  bool isStdlibModule(int moduleId) const {
    // Stdlib/prelude bodies are emitted for real: stubbing them to A_NONE
    // produced silently wrong values (e.g. stdlib/std/simd). If a stdlib
    // body hits an unsupported construct, emitNative fails and the pipeline
    // falls back to emitC — always correct, never a silent None.
    (void)moduleId;
    return false;
  }

  bool run(std::string& out, std::string& error) {
    error_ = &error;
    collectGlobals();
    buildGlobalScope();
    for (StructInfo* st : structList_) emitStructCtor(st);
    for (FnInfo* f : fnList_) {
      if (!failed_) emitFunction(f);
    }
    emitMain();
    emitLambdas();
    if (failed_) return false;
    out = bss_.str() + rodata_.str() + t_.extraRodata() + text_.str();
    return true;
  }
  // Per-unit split (native): functions/ctors per module; top-levels stay
  // inlined in the entry unit's `main` (native top-levels are frame
  // locals, so moving them into per-module functions would break sharing).
  // Symbols are stable (Phase 1), so units link as ordinary externs.
  bool runPerUnit(std::vector<AsmUnit>& out, std::string& error,
                  const std::vector<int>* only = nullptr) {
    error_ = &error;
    perUnitMode_ = true;
    collectGlobals();
    buildGlobalScope();
    std::vector<int> order = graph_.order;
    if (order.empty() && graph_.entry >= 0) order.push_back(graph_.entry);
    std::set<int> inOrder(order.begin(), order.end());
    for (const Module& mod : graph_.modules) {
      if (!inOrder.count(mod.id)) {
        order.push_back(mod.id);
        inOrder.insert(mod.id);
      }
    }
    std::vector<int> wanted = order;
    if (only && !only->empty()) {
      std::set<int> keep(only->begin(), only->end());
      wanted.clear();
      for (int mid : order) if (keep.count(mid)) wanted.push_back(mid);
      for (int mid : *only)
        if (!inOrder.count(mid) && keep.count(mid)) wanted.push_back(mid);
    }
    for (int mid : wanted) {
      if (failed_) return false;
      AsmUnit unit;
      unit.moduleId = mid;
      emitUnit(mid, unit.text);
      if (failed_) return false;
      out.push_back(std::move(unit));
    }
    return true;
  }
  void emitUnit(int moduleId, std::string& out) {
    text_ = std::ostringstream();
    rodata_ = std::ostringstream();
    bss_ = std::ostringstream();
    labelCounter_ = 0;
    counter_ = 0;
    lambdaSeq_.erase(moduleId);
    for (StructInfo* st : structList_)
      if (st->moduleId == moduleId) emitStructCtor(st);
    for (FnInfo* f : fnList_) {
      if (failed_) break;
      if (f->moduleId == moduleId) emitFunction(f);
    }
    if (!failed_ && moduleId == graph_.entry) emitMain();
    if (!failed_) drainLambdasFor(moduleId);
    if (failed_) return;
    emitModuleBss(moduleId);
    out = bss_.str() + rodata_.str() + t_.extraRodata() + text_.str();
  }
  void drainLambdasFor(int moduleId) {
    // lambdaQueue_ is in creation order (nested appended); emit this
    // module's jobs in order, handling newly queued nested jobs.
    for (std::size_t i = 0; i < lambdaQueue_.size(); ++i) {
      if (failed_) return;
      if (lambdaQueue_[i].moduleId != moduleId) continue;
      // Skip already-emitted jobs (marked by empty decl? no: track via
      // emitted flag in decl pointer? Use a separate emitted set).
      // Simpler: emitLambdas originally emits all in order; for split,
      // emit only this module's pending jobs and remove them.
      LambdaJob job = lambdaQueue_[i];
      lambdaQueue_.erase(lambdaQueue_.begin() + (std::ptrdiff_t)i);
      --i;
      emitLambdaJob(job);
    }
  }
  void emitModuleBss(int moduleId) {
    std::vector<std::string> tops;
    for (const auto& kv : topLevelGlobals_) {
      const std::string& key = kv.first;
      auto colon = key.find(':');
      if (colon == std::string::npos) continue;
      if (key.substr(0, colon) != std::to_string(moduleId)) continue;
      tops.push_back(kv.second);
    }
    std::sort(tops.begin(), tops.end());
    tops.erase(std::unique(tops.begin(), tops.end()), tops.end());
    for (const std::string& g : tops)
      bss_ << "    .section .bss\n    .globl " << g << "\n" << g << ":\n    .space 8\n";
    for (FnInfo* f : fnList_)
      if (!f->isMethod && f->moduleId == moduleId)
        bss_ << "    .section .bss\n    .globl " << f->globalVar << "\n"
             << f->globalVar << ":\n    .space 8\n";
    for (StructInfo* st : structList_)
      if (st->moduleId == moduleId)
        bss_ << "    .section .bss\n    .globl " << st->globalVar << "\n"
             << st->globalVar << ":\n    .space 8\n";
    for (auto& kv : enums_)
      if (!kv.second.algebraic && kv.second.moduleId == moduleId)
        bss_ << "    .section .bss\n    .globl " << kv.second.globalVar << "\n"
             << kv.second.globalVar << ":\n    .space 8\n";
  }

 private:
  const ModuleGraph& graph_;
  const SemaResult& sema_;
  const NatTarget& t_;
  bool testMode_ = false;
  std::string testFilter_;  // `a test --filter`: substring matched on test names
  std::string* error_ = nullptr;
  bool failed_ = false;
  int counter_ = 0;
  bool perUnitMode_ = false;
  mutable std::unordered_map<int, std::string> moduleTags_;
  std::unordered_map<int, int> lambdaSeq_;
  int labelCounter_ = 0;
  int slotCounter_ = 0;
  std::ostringstream text_;
  std::ostringstream rodata_;
  std::ostringstream bss_;

  std::unordered_map<const FnDecl*, FnInfo*> fns_;
  std::vector<FnInfo*> fnList_;
  std::unordered_map<std::string, StructInfo*> structs_;
  std::vector<StructInfo*> structList_;
  std::unordered_map<std::string, EnumVariantInfo> enums_;
  std::vector<std::string> globals_;
  std::unordered_map<std::string, std::string> topLevelGlobals_;

  std::vector<std::unordered_map<std::string, ABinding>> scopes_;
  std::vector<std::vector<std::string>> scopeDeclOrder_;  // parallel to scopes_
  std::vector<std::string> loopBreak_;
  std::vector<std::string> loopCont_;
  bool inFunction_ = false;
  int curModuleId_ = -1;
  std::string retLabel_;  // per-function return target ("" outside functions)
  // defer: stack of frames; each frame is a list of deferred stmt bodies.
  // A frame is pushed per function and per for-loop body (matching emitC).
  std::vector<std::vector<const Stmt*>> deferStack_;
  std::vector<int> loopDeferDepth_;
  // lambdas queued for emission as separate functions.
  struct LambdaJob {
    const LambdaExpr* decl = nullptr;
    std::string cname;
    int moduleId = -1;
    struct Cap { std::string name; int slot; };
    std::vector<Cap> captures;  // slot = caller frame slot at creation
  };
  std::vector<LambdaJob> lambdaQueue_;

  static constexpr uint64_t kTag = 0xFFFF000000000000ULL;
  static constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
  static constexpr int64_t kIntLim = INT64_C(1) << 46;

  void fail(const std::string& msg) {
    if (!failed_) {
      failed_ = true;
      if (error_) *error_ = "native backend: " + msg +
          (curFnName_.empty() ? "" : " (in " + curFnName_ + ")");
    }
  }
  std::string curFnName_;
  int curEnvSlot_ = -1;   // slot holding argv[0] (closure env) in current fn
  int curArgvSlot_ = -1;  // slot holding the argv pointer in current fn
  void emit(const std::string& s) {
    if (s.empty() || failed_) return;
    text_ << s;
    if (s.back() != '\n') text_ << '\n';
  }
  std::string u64s(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
  }
  int allocSlot() { return slotCounter_++; }
  int frameBytes() const { return ((slotCounter_ * 8) + t_.frameReserve() + 15) & ~15; }
  std::string newLabel(const std::string& pfx = "L") {
    return "__na_" + pfx + std::to_string(labelCounter_++);
  }
  void emitLabel(const std::string& l) { emit(l + ":"); }

  std::string acc() const { return t_.acc(); }
  std::string A(int i) const { return t_.argReg(i); }
  std::string T(int i) const { return t_.tmpReg(i); }

  // value movement -----------------------------------------------------------
  void movImm(uint64_t v, const std::string& reg) {
    std::string o;
    t_.movImm(o, v, reg);
    emit(o);
  }
  void storeAcc(int slot) {
    std::string o;
    t_.storeSlot(o, acc(), slot);
    emit(o);
  }
  void loadSlot(int slot, const std::string& reg) {
    std::string o;
    t_.loadSlot(o, slot, reg);
    emit(o);
  }
  void storeSlot(int slot, const std::string& reg) {
    std::string o;
    t_.storeSlot(o, reg, slot);
    emit(o);
  }
  void copySlot(int from, int to) {
    std::string o;
    t_.copySlot(o, from, to);
    emit(o);
  }
  void leaSlot(int slot, const std::string& reg) {
    std::string o;
    t_.leaSlot(o, slot, reg);
    emit(o);
  }
  void leaSym(const std::string& sym, const std::string& reg) {
    std::string o;
    t_.leaSym(o, sym, reg);
    emit(o);
  }
  void loadSym(const std::string& sym, const std::string& reg) {
    std::string o;
    t_.loadSym(o, sym, reg);
    emit(o);
  }
  void storeSym(const std::string& sym, const std::string& reg) {
    std::string o;
    t_.storeSym(o, sym, reg);
    emit(o);
  }
  void callSym(const std::string& sym) {
    std::string o;
    t_.callSym(o, sym);
    emit(o);
  }
  void movReg(const std::string& d, const std::string& s) {
    std::string o;
    t_.movReg(o, d, s);
    emit(o);
  }
  void jmp(const std::string& l) {
    std::string o;
    t_.jmp(o, l);
    emit(o);
  }
  void jcc(const std::string& cc, const std::string& l) {
    std::string o;
    t_.jcc(o, cc, l);
    emit(o);
  }
  void jz(const std::string& r, const std::string& l) {
    std::string o;
    t_.jz(o, r, l);
    emit(o);
  }
  void jnz(const std::string& r, const std::string& l) {
    std::string o;
    t_.jnz(o, r, l);
    emit(o);
  }
  void cmpRegs(const std::string& a, const std::string& b) {
    std::string o;
    t_.cmpRegs(o, a, b);
    emit(o);
  }
  void addImm(const std::string& r, int64_t v) {
    std::string o;
    t_.addImm(o, r, v);
    emit(o);
  }
  void zeroReg(const std::string& r) {
    std::string o;
    t_.zeroReg(o, r);
    emit(o);
  }
  void retInsn() {
    std::string o;
    t_.ret(o);
    emit(o);
  }

  // ---- scopes / bindings ----------------------------------------------------
  void pushScope() {
    scopes_.emplace_back();
    scopeDeclOrder_.emplace_back();
  }
  void popScope() {
    scopes_.pop_back();
    scopeDeclOrder_.pop_back();
  }
  ABinding* lookup(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) return &f->second;
    }
    return nullptr;
  }
  void bindLocal(const std::string& name, int slot) {
    ABinding b;
    b.kind = BKind::Local;
    b.slot = slot;
    scopes_.back()[name] = b;
    if (!scopeDeclOrder_.empty()) scopeDeclOrder_.back().push_back(name);
  }

  // ---- RAII drops (mirrors emitC) -------------------------------------------
  FnInfo* findDrop(const Expr* init) {
    if (!init) return nullptr;
    TypePtr ty = resolveType(init->inferredType);
    if (!ty || ty->kind != TypeKind::Struct) return nullptr;
    std::string sname = ty->structName;
    std::size_t sep = sname.find_last_of(".:");
    if (sep != std::string::npos) sname = sname.substr(sep + 1);
    StructInfo* st = findStructByName(sname);
    if (!st) return nullptr;
    auto mit = st->methods.find("drop");
    return mit == st->methods.end() ? nullptr : mit->second;
  }
  // Emit dropFn(self) for a binding; self loaded from slot or global sym.
  void emitDropCall(const ABinding& b) {
    if (!b.dropFn || failed_) return;
    int selfS;
    if (b.kind == BKind::Local || b.kind == BKind::Param) {
      selfS = b.slot;
    } else if (b.kind == BKind::Global || b.kind == BKind::FnValue ||
               b.kind == BKind::StructCtor || b.kind == BKind::EnumValue) {
      selfS = allocSlot();
      loadSym(b.sym, acc());
      storeAcc(selfS);
    } else {
      return;
    }
    emitOrderedCall(b.dropFn->cname, {}, selfS);
  }
  // Drop all droppable bindings in the current (top) scope, decl order reversed.
  void emitDropsForScope() {
    if (scopes_.empty() || scopeDeclOrder_.empty()) return;
    if (std::getenv("A_DEBUG_NATIVE")) {
      std::fprintf(stderr, "native: drops scope size=%zu decls=%zu\n",
                   scopes_.back().size(), scopeDeclOrder_.back().size());
      for (auto& kv : scopes_.back())
        std::fprintf(stderr, "  bind %s kind=%d drop=%p\n", kv.first.c_str(),
                     (int)kv.second.kind, (const void*)kv.second.dropFn);
    }
    auto& scope = scopes_.back();
    auto& decls = scopeDeclOrder_.back();
    std::unordered_set<std::string> seen;
    for (auto it = decls.rbegin(); it != decls.rend(); ++it) {
      seen.insert(*it);
      auto f = scope.find(*it);
      if (f != scope.end()) emitDropCall(f->second);
      if (failed_) return;
    }
    for (auto& kv : scope) {
      if (!seen.count(kv.first)) emitDropCall(kv.second);
      if (failed_) return;
    }
  }
  // Pop the top defer frame emitting its stmts LIFO, then drop scope bindings.
  void emitDeferredFrameAndDrops() {
    if (!deferStack_.empty()) {
      auto frame = deferStack_.back();
      deferStack_.pop_back();
      for (auto it = frame.rbegin(); it != frame.rend(); ++it) emitStmt(*it);
      if (failed_) return;
    }
    emitDropsForScope();
  }

  // ---- literals / data ------------------------------------------------------
  uint64_t mkintA(int64_t v) const {
    uint64_t p = (((uint64_t)v) << 1) | 1ULL;
    return kTag | (p & kPayloadMask);
  }
  void emitIntLit(int64_t v) {
    // a_mkint is ((v<<1)|1)&mask | A_TAG — identical to mkintA for all v.
    movImm(mkintA(v), acc());
  }
  void emitDoubleLit(double d) {
    // a_mkdouble is a bit-copy of the double — no runtime call needed.
    uint64_t bits;
    std::memcpy(&bits, &d, 8);
    movImm(bits, acc());
  }
  // Native module constant (PI, E) -> boxed double aval. Same lowering as
  // emitC's NativeConst binding.
  void emitNativeConst(const std::string& name) {
    emitDoubleLit(name == "PI"   ? 3.14159265358979323846
                 : name == "E"    ? 2.71828182845904523536
                                   : 0.0);
  }
  // Enum variant as a value: simple variants load their global aval;
  // algebraic variants build {__tag, __name} (mirrors emitC).
  void emitEnumVariantValue(const EnumVariantInfo& ev,
                            const std::string& fieldName) {
    if (!ev.algebraic) {
      loadSym(ev.globalVar, acc());
      return;
    }
    int dS = allocSlot();
    callSym("a_dict_new");
    storeAcc(dS);
    int tagS = strLitSlot("__tag");
    int tagV = allocSlot();
    movImm(mkintA(ev.tag), acc());
    storeAcc(tagV);
    loadSlot(dS, A(0));
    loadSlot(tagS, A(1));
    loadSlot(tagV, A(2));
    callSym("a_dict_set");
    int nameS = strLitSlot("__name");
    int nameV = allocSlot();
    emitStringLit(fieldName);
    storeAcc(nameV);
    loadSlot(dS, A(0));
    loadSlot(nameS, A(1));
    loadSlot(nameV, A(2));
    callSym("a_dict_set");
    loadSlot(dS, acc());
  }
  std::string emitStrData(const std::string& bytes, bool nulTerm) {
    std::string l = "__na_d" + std::to_string(counter_++);
    rodata_ << "    .section .rodata\n    .align 1\n" << l << ":\n    .byte ";
    bool first = true;
    auto put = [&](int b) {
      if (!first) rodata_ << ",";
      rodata_ << b;
      first = false;
    };
    for (unsigned char c : bytes) put(c);
    if (nulTerm) put(0);
    if (first) put(0);
    rodata_ << "\n";
    return l;
  }
  // aval string literal -> acc holds the aval.
  void emitStringLit(const std::string& text) {
    std::string l = emitStrData(text, false);
    leaSym(l, A(0));
    movImm(text.size(), A(1));
    callSym("a_str_new");
  }
  int strLitSlot(const std::string& text) {
    emitStringLit(text);
    int s = allocSlot();
    storeAcc(s);
    return s;
  }

  // argv array: consecutive slots; argv[k] -> slot(base + n - 1 - k) is NOT
  // used here — instead we lay argv[0..n) at base..base+n-1 directly and
  // pass &slot(base). a_call expects argv[0]=fn/self placeholder.
  struct Argv {
    int base = 0;
    int n = 0;
  };
  Argv allocArgv(int count) {
    Argv a;
    a.base = slotCounter_;
    a.n = count;
    for (int i = 0; i < count; ++i) allocSlot();
    return a;
  }
  void argvStore(const Argv& a, int k, int valSlot) {
    copySlot(valSlot, a.base + k);
  }
  void argvStoreSym(const Argv& a, int k, const std::string& sym) {
    // store a symbol ADDRESS into argv[k] (for names[]/fns[] arrays).
    leaSym(sym, T(0));
    storeSlot(a.base + k, T(0));
  }

  // ---- runtime call helpers -------------------------------------------------
  // Evaluate each arg expr to a fresh slot, then load arg regs and call.
  void callN(const char* sym, const std::vector<const Expr*>& args) {
    if ((int)args.size() > t_.nArgRegs()) {
      fail("native: too many call args");
      return;
    }
    std::vector<int> slots;
    for (const Expr* a : args) {
      int s = allocSlot();
      emitExpr(a);
      storeAcc(s);
      slots.push_back(s);
    }
    for (std::size_t i = 0; i < slots.size(); ++i) loadSlot(slots[i], A((int)i));
    callSym(sym);
  }
  // a_bi_call(id, n, argv): builtin dispatch.
  void emitBiCall(int id, const std::vector<const Expr*>& args) {
    Argv av = allocArgv((int)args.size());
    for (std::size_t i = 0; i < args.size(); ++i) {
      int s = allocSlot();
      emitExpr(args[i]);
      storeAcc(s);
      argvStore(av, (int)i, s);
    }
    movImm((uint64_t)id, A(0));
    movImm(args.size(), A(1));
    if (args.empty()) zeroReg(A(2));
    else leaSlot(av.base, A(2));
    callSym("a_bi_call");
  }

  // a_call(fn, n, argv): generic call through an FNON/aval.
  void emitACall(const Expr* callee, const std::vector<const Expr*>& args) {
    int fnS = allocSlot();
    emitExpr(callee);
    storeAcc(fnS);
    Argv av = allocArgv((int)args.size());
    for (std::size_t i = 0; i < args.size(); ++i) {
      int s = allocSlot();
      emitExpr(args[i]);
      storeAcc(s);
      argvStore(av, (int)i, s);
    }
    loadSlot(fnS, A(0));
    movImm(args.size(), A(1));
    if (args.empty()) zeroReg(A(2));
    else leaSlot(av.base, A(2));
    callSym("a_call");
  }

  // py_call(module, member, nkw, pos..., (kname, kval)*nkw): argv[0..1] are
  // the module/member strings, argv[2] the kwargs count, then positional
  // args, then name/value pairs. The runtime resolves libpython. Mirrors
  // emitAsm's emitPyCall / emitC's emitPyCall.
  void emitPyCall(const std::string& module, const std::string& member,
                  const CallExpr& call) {
    int ms = strLitSlot(module);
    int fs = strLitSlot(member);
    std::vector<int> posSlots;
    std::vector<std::pair<std::string, int>> kwSlots;
    for (const CallArg& a : call.args) {
      int s = evalToSlot(a.value.get());
      if (a.name.empty()) {
        posSlots.push_back(s);
      } else {
        kwSlots.push_back({a.name, s});
      }
    }
    int nkw = (int)kwSlots.size();
    Argv av = allocArgv((int)posSlots.size() + 2 * nkw + 3);
    argvStore(av, 0, ms);
    argvStore(av, 1, fs);
    int ks = allocSlot();
    movImm(mkintA(nkw), acc());
    storeAcc(ks);
    argvStore(av, 2, ks);
    for (std::size_t i = 0; i < posSlots.size(); ++i)
      argvStore(av, (int)i + 3, posSlots[i]);
    for (std::size_t i = 0; i < kwSlots.size(); ++i) {
      int kns = strLitSlot(kwSlots[i].first);
      argvStore(av, (int)(3 + posSlots.size() + 2 * i), kns);
      argvStore(av, (int)(3 + posSlots.size() + 2 * i + 1), kwSlots[i].second);
    }
    int bid = builtinRuntimeId("py_call");
    if (bid < 0) {
      fail("native: py_call builtin unavailable");
      return;
    }
    movImm((uint64_t)bid, A(0));
    movImm(av.n, A(1));
    if (av.n == 0) zeroReg(A(2));
    else leaSlot(av.base, A(2));
    callSym("a_bi_call");
  }

  // Direct call to a known A function: f(aval *argv), argv[0]=env.
  void emitDirectCall(FnInfo* f, const std::vector<const Expr*>& args) {
    int n = (int)args.size() + 1;  // argv[0] = env placeholder
    Argv av = allocArgv(n);
    movImm(kTag | 0x2ULL, acc());
    storeAcc(av.base);  // argv[0] = A_NONE env
    for (int i = 0; i < (int)args.size(); ++i) {
      int s = allocSlot();
      emitExpr(args[i]);
      storeAcc(s);
      argvStore(av, i + 1, s);
    }
    leaSlot(av.base, A(0));
    callSym(f->cname);
  }

  // ---- binary ops -----------------------------------------------------------
  static const char* binopSlow(const std::string& op) {
    static const std::unordered_map<std::string, const char*> kMap = {
        {"+", "a_add_slow"}, {"-", "a_sub_slow"}, {"*", "a_mul_slow"},
        {"/", "a_div_slow"}, {"%", "a_mod_slow"}, {"**", "a_pow"},
        {"==", "a_eq_slow"}, {"!=", "a_ne_slow"}, {"<", "a_lt_slow"},
        {"<=", "a_le_slow"}, {">", "a_gt_slow"}, {">=", "a_ge_slow"},
        {"&", "a_band_slow"}, {"|", "a_bor_slow"}, {"^", "a_bxor_slow"},
        {"<<", "a_shl_slow"}, {">>", "a_shr_slow"},
    };
    auto it = kMap.find(op);
    return it == kMap.end() ? nullptr : it->second;
  }
  // slots la, lb already evaluated -> call f(la, lb) leaving result in acc.
  void call2slot(const char* sym, int la, int lb) {
    loadSlot(lb, A(1));
    loadSlot(la, A(0));
    callSym(sym);
  }

  // ---- extended helpers -----------------------------------------------------
  // Evaluate expr into a fresh slot; returns the slot index.
  int evalToSlot(const Expr* e) {
    int s = allocSlot();
    emitExpr(e);
    storeAcc(s);
    return s;
  }
  // call sym(a0..an) where ai are slot indices.
  void callSlots(const char* sym, const std::vector<int>& slots) {
    if ((int)slots.size() > t_.nArgRegs()) {
      fail("native: too many runtime args");
      return;
    }
    for (std::size_t i = 0; i < slots.size(); ++i)
      loadSlot(slots[i], A((int)i));
    callSym(sym);
  }
  // Branch helpers on plain 0/1 in acc.
  void jzAcc(const std::string& l) { jz(acc(), l); }
  void jnzAcc(const std::string& l) { jnz(acc(), l); }
  // acc holds aval; jump to l if it equals A_NONE / is not A_NONE.
  void jmpIfNone(const std::string& l) {
    movImm(kTag | 0x2ULL, T(0));
    cmpRegs(acc(), T(0));
    jcc("eq", l);
  }
  void jmpIfNotNone(const std::string& l) {
    movImm(kTag | 0x2ULL, T(0));
    cmpRegs(acc(), T(0));
    jcc("ne", l);
  }
  // acc holds aval; call a_truthy_raw_slow -> plain 0/1 in acc.
  void truthyAcc() { callSym("a_truthy_raw_slow"); }

  // Mirror of emitC orderArgs: named args matched to paramNames, positionals
  // fill in order, trailing *args packed into a list, unfilled params get the
  // missing sentinel (A_TAG for fns so callees apply defaults, A_NONE for
  // struct ctors). Returns slot indices aligned with paramNames.
  std::vector<int> orderArgSlots(const CallExpr& call,
                                 const std::vector<std::string>& paramNames,
                                 int variadicIdx, bool structCtor) {
    std::vector<int> ordered(paramNames.size(), -1);
    std::vector<bool> filled(paramNames.size(), false);
    std::vector<int> pos;
    for (auto& a : call.args) {
      int s = evalToSlot(a.value.get());
      if (a.name.empty()) {
        pos.push_back(s);
      } else {
        for (std::size_t i = 0; i < paramNames.size(); ++i) {
          if (paramNames[i] == a.name) { ordered[i] = s; filled[i] = true; }
        }
      }
    }
    std::size_t fixed = variadicIdx >= 0 ? (std::size_t)variadicIdx
                                         : paramNames.size();
    for (std::size_t i = 0; i < pos.size() && i < fixed && i < paramNames.size(); ++i) {
      ordered[i] = pos[i];
      filled[i] = true;
    }
    if (variadicIdx >= 0 && (std::size_t)variadicIdx < paramNames.size()) {
      int lst = allocSlot();
      movImm(0, A(0));
      zeroReg(A(1));
      callSym("a_list_new");
      storeAcc(lst);
      for (std::size_t i = fixed; i < pos.size(); ++i) {
        loadSlot(lst, A(0));
        loadSlot(pos[i], A(1));
        callSym("a_list_push");
      }
      ordered[variadicIdx] = lst;
      filled[variadicIdx] = true;
    }
    uint64_t missing = structCtor ? (kTag | 0x2ULL) : kTag;
    for (std::size_t i = 0; i < ordered.size(); ++i) {
      if (!filled[i]) {
        ordered[i] = allocSlot();
        movImm(missing, acc());
        storeAcc(ordered[i]);
      }
    }
    return ordered;
  }

  // argv = [env=A_NONE, (self,) args...] then call cname.
  void emitOrderedCall(const std::string& cname, const std::vector<int>& argSlots,
                       int selfSlot = -1) {
    int extra = selfSlot >= 0 ? 1 : 0;
    int n = (int)argSlots.size() + 1 + extra;
    Argv av = allocArgv(n);
    movImm(kTag | 0x2ULL, acc());
    storeAcc(av.base);
    int idx = 1;
    if (selfSlot >= 0) { argvStore(av, idx++, selfSlot); }
    for (int s : argSlots) argvStore(av, idx++, s);
    leaSlot(av.base, A(0));
    callSym(cname);
  }
  void emitFnCallOrdered(FnInfo* f, const CallExpr& call) {
    int vi = f->variadic ? (int)f->paramNames.size() - 1 : -1;
    auto slots = orderArgSlots(call, f->paramNames, vi, false);
    emitOrderedCall(f->cname, slots);
  }
  void emitStructCall(StructInfo* st, const CallExpr& call) {
    auto slots = orderArgSlots(call, st->fieldNames, -1, true);
    emitOrderedCall(st->ctorCname, slots);
  }

  FnInfo* findModuleFn(int moduleId, const std::string& name) {
    for (FnInfo* f : fnList_)
      if (!f->isMethod && f->moduleId == moduleId && f->name == name) return f;
    return nullptr;
  }
  StructInfo* findModuleStruct(int moduleId, const std::string& name) {
    for (StructInfo* st : structList_)
      if (st->moduleId == moduleId && st->name == name) return st;
    return nullptr;
  }
  // top-level `let`/`fn`/struct global symbol in module moduleId.
  bool moduleGlobalSym(int moduleId, const std::string& name,
                       std::string& sym) {
    auto it = topLevelGlobals_.find(std::to_string(moduleId) + ":" + name);
    if (it != topLevelGlobals_.end()) { sym = it->second; return true; }
    for (FnInfo* f : fnList_)
      if (!f->isMethod && f->moduleId == moduleId && f->name == name) {
        sym = f->globalVar; return true;
      }
    for (StructInfo* st : structList_)
      if (st->moduleId == moduleId && st->name == name) {
        sym = st->globalVar; return true;
      }
    return false;
  }

  // Follow `from "x.a" import name` re-exports (package __init__ modules) to
  // the module that actually declares `name`. Returns {moduleId, name};
  // unchanged when there is no re-export. Mirrors emitC's
  // resolveModuleMember; native/python re-exports are not followed.
  std::pair<int, std::string> resolveModuleMember(int moduleId,
                                                  const std::string& name) {
    std::string cur = name;
    for (int depth = 0; moduleId >= 0 && depth < 16; ++depth) {
      if (moduleId >= (int)graph_.modules.size()) break;
      const Module& mod = graph_.at(moduleId);
      if (!mod.program) break;
      bool declared = false;
      for (const StmtPtr& s : mod.program->statements) {
        if ((s->kind == StmtKind::FnDecl &&
             static_cast<const FnDecl*>(s.get())->name == cur) ||
            (s->kind == StmtKind::StructDecl &&
             static_cast<const StructDecl*>(s.get())->name == cur) ||
            (s->kind == StmtKind::EnumDecl &&
             static_cast<const EnumDecl*>(s.get())->name == cur) ||
            (s->kind == StmtKind::Let &&
             static_cast<const LetStmt*>(s.get())->name == cur)) {
          declared = true;
          break;
        }
      }
      if (declared) break;
      int nextId = -1;
      std::string nextName;
      for (const StmtPtr& s : mod.program->statements) {
        if (s->kind != StmtKind::FromImport) continue;
        const FromImportStmt* im = static_cast<const FromImportStmt*>(s.get());
        if (im->nativeModule || im->pythonModuleImport || im->moduleId < 0)
          continue;
        for (const auto& item : im->items) {
          // `import x as y` re-exports under the alias; recurse on the
          // source name in the source module.
          if (item.name == cur || (!item.alias.empty() && item.alias == cur)) {
            nextId = im->moduleId;
            nextName = item.name;
            break;
          }
        }
        if (nextId >= 0) break;
      }
      if (nextId < 0) break;
      moduleId = nextId;
      cur = nextName;
    }
    return {moduleId, cur};
  }

  // ---- defer ----------------------------------------------------------------
  void emitDeferredForReturn() {
    for (int fi = (int)deferStack_.size() - 1; fi >= 0; --fi) {
      auto& frame = deferStack_[fi];
      for (auto it = frame.rbegin(); it != frame.rend(); ++it) emitStmt(*it);
    }
  }
  void emitDeferredDownTo(int depth) {
    for (int fi = (int)deferStack_.size() - 1; fi >= depth; --fi) {
      auto& frame = deferStack_[fi];
      for (auto it = frame.rbegin(); it != frame.rend(); ++it) emitStmt(*it);
    }
  }

  // ---- lambdas ---------------------------------------------------------------
  void collectRefs(const Expr* e, std::unordered_set<std::string>& bound,
                   std::vector<std::string>& refs,
                   std::unordered_set<std::string>& seen) {
    if (!e) return;
    auto addRef = [&](const std::string& n) {
      if (!bound.count(n) && !seen.count(n)) {
        seen.insert(n);
        refs.push_back(n);
      }
    };
    switch (e->kind) {
      case ExprKind::Ident:
        addRef(static_cast<const IdentExpr&>(*e).name);
        return;
      case ExprKind::Lambda: {
        auto& l = static_cast<const LambdaExpr&>(*e);
        std::unordered_set<std::string> inner = bound;
        for (auto& p : l.params) inner.insert(p.name);
        collectRefs(l.body.get(), inner, refs, seen);
        return;
      }
      case ExprKind::Comprehension: {
        auto& c = static_cast<const ComprehensionExpr&>(*e);
        collectRefs(c.iterable.get(), bound, refs, seen);
        std::unordered_set<std::string> inner = bound;
        inner.insert(c.var);
        collectRefs(c.element.get(), inner, refs, seen);
        collectRefs(c.condition.get(), inner, refs, seen);
        return;
      }
      case ExprKind::Match: {
        auto& m = static_cast<const MatchExpr&>(*e);
        collectRefs(m.subject.get(), bound, refs, seen);
        for (auto& arm : m.arms) {
          std::unordered_set<std::string> inner = bound;
          collectPatternRefs(arm.pattern, inner);
          collectRefs(arm.guard.get(), inner, refs, seen);
          collectRefs(arm.result.get(), inner, refs, seen);
        }
        return;
      }
      default: break;
    }
    for (const Expr* child : childExprs(e)) collectRefs(child, bound, refs, seen);
  }
  void collectPatternRefs(const Pattern& p, std::unordered_set<std::string>& bound) {
    if (p.kind == Pattern::Kind::Binding) bound.insert(p.name);
    if (p.value) {
      std::vector<std::string> r;
      std::unordered_set<std::string> s;
      collectRefs(p.value.get(), bound, r, s);
    }
    for (auto& sub : p.subpatterns) collectPatternRefs(sub, bound);
  }
  std::vector<const Expr*> childExprs(const Expr* e) {
    std::vector<const Expr*> out;
    auto add = [&](const ExprPtr& p) { if (p) out.push_back(p.get()); };
    switch (e->kind) {
      case ExprKind::Unary: add(static_cast<const UnaryExpr*>(e)->operand); break;
      case ExprKind::Binary: { auto& x = *static_cast<const BinaryExpr*>(e); add(x.lhs); add(x.rhs); break; }
      case ExprKind::Call: {
        auto& c = static_cast<const CallExpr&>(*e);
        add(c.callee);
        for (auto& a : c.args) add(a.value);
        break;
      }
      case ExprKind::Index: { auto& x = *static_cast<const IndexExpr*>(e); add(x.object); add(x.index); break; }
      case ExprKind::Slice: { auto& x = *static_cast<const SliceExpr*>(e); add(x.object); add(x.start); add(x.end); add(x.step); break; }
      case ExprKind::Field: add(static_cast<const FieldExpr*>(e)->object); break;
      case ExprKind::SafeNav: add(static_cast<const SafeNavExpr*>(e)->object); break;
      case ExprKind::ArrayLit: for (auto& el : static_cast<const ArrayLitExpr*>(e)->elements) add(el); break;
      case ExprKind::Range: { auto& x = *static_cast<const RangeExpr*>(e); add(x.start); add(x.end); add(x.step); break; }
      case ExprKind::Ternary: { auto& x = *static_cast<const TernaryExpr*>(e); add(x.cond); add(x.thenValue); add(x.elseValue); break; }
      case ExprKind::Tuple: for (auto& el : static_cast<const TupleExpr*>(e)->elements) add(el); break;
      case ExprKind::Comprehension: {
        const auto* c = static_cast<const ComprehensionExpr*>(e);
        add(c->element); add(c->iterable); add(c->condition); break;
      }
      case ExprKind::Match: {
        const auto* m = static_cast<const MatchExpr*>(e);
        add(m->subject);
        for (auto& arm : m->arms) { add(arm.guard); add(arm.result); }
        break;
      }
      case ExprKind::Try: add(static_cast<const TryExpr*>(e)->operand); break;
      case ExprKind::MapLit: for (auto& kv : static_cast<const MapLitExpr*>(e)->entries) { add(kv.first); add(kv.second); } break;
      case ExprKind::SetLit: for (auto& el : static_cast<const SetLitExpr*>(e)->elements) add(el); break;
      default: break;
    }
    return out;
  }

  StructInfo* findStructByName(const std::string& name) const {
    if (curModuleId_ >= 0) {
      for (StructInfo* st : structList_)
        if (st->name == name && st->moduleId == curModuleId_) return st;
    }
    for (StructInfo* st : structList_)
      if (st->name == name) return st;
    auto it = structs_.find(name);
    return it == structs_.end() ? nullptr : it->second;
  }
  std::string moduleTag(int moduleId) const {
    auto it = moduleTags_.find(moduleId);
    if (it != moduleTags_.end()) return it->second;
    std::string path;
    if (moduleId >= 0 && moduleId < (int)graph_.modules.size())
      path = graph_.at(moduleId).path;
    std::string canonical = path;
    {
      std::error_code ec;
      auto c = std::filesystem::weakly_canonical(path, ec);
      if (!ec) canonical = c.string();
      if (canonical.empty()) canonical = path;
    }
    std::string tag = stableTag(canonical);
    moduleTags_[moduleId] = tag;
    return tag;
  }
  const EnumVariantInfo* findEnumVariant(const std::string& enumName,
                                         const std::string& variant,
                                         int preferModule = -2) const {
    int prefer = preferModule == -2 ? curModuleId_ : preferModule;
    const EnumVariantInfo* fallback = nullptr;
    for (const auto& kv : enums_) {
      const EnumVariantInfo& ev = kv.second;
      if (ev.enumName == enumName && ev.variantName == variant) {
        if (ev.moduleId == prefer) return &ev;
        if (!fallback) fallback = &ev;
      }
    }
    return fallback;
  }
  const EnumVariantInfo* findEnumByShort(const std::string& variant,
                                         int preferModule = -2) const {
    int prefer = preferModule == -2 ? curModuleId_ : preferModule;
    const EnumVariantInfo* fallback = nullptr;
    for (const auto& kv : enums_) {
      const EnumVariantInfo& ev = kv.second;
      if (ev.variantName == variant) {
        if (ev.moduleId == prefer) return &ev;
        if (!fallback) fallback = &ev;
      }
    }
    return fallback;
  }
  // ---- registration (mirrors emitC) -----------------------------------------
  void collectGlobals() {
    std::unordered_map<std::string, int> seenSym;
    auto symWithSuffix = [&](const std::string& base) -> std::string {
      int n = seenSym[base]++;
      if (n == 0) return base;
      return base + "_" + std::to_string(n);
    };
    for (const Module& mod : graph_.modules) {
      if (mod.id >= 0) (void)moduleTag(mod.id);
    }
    for (const Module& mod : graph_.modules) {
      if (!mod.program) continue;
      for (const StmtPtr& s : mod.program->statements) {
        if (s->kind == StmtKind::FnDecl) {
          const FnDecl* fn = static_cast<const FnDecl*>(s.get());
          FnInfo* info = new FnInfo();
          info->decl = fn;
          info->name = fn->name;
          info->moduleId = mod.id;
          {
            std::string mtag = moduleTag(mod.id);
            std::size_t arity = fn->comptimeParams.size() + fn->params.size();
            std::string dh = stableTag(fn->name + "#" + std::to_string(arity));
            info->cname = symWithSuffix("__na_f_" + mtag + "_" + dh);
            info->globalVar = symWithSuffix("__na_gf_" + mtag + "_" + dh);
          }
          for (const ComptimeParam& p : fn->comptimeParams) {
            info->paramNames.push_back(p.name);
            info->paramDefaults.push_back(nullptr);
          }
          for (const ParamDecl& p : fn->params) {
            info->paramNames.push_back(p.name);
            info->paramDefaults.push_back(p.defaultValue.get());
            info->variadic = info->variadic || p.variadic;
          }
          fns_[fn] = info;
          fnList_.push_back(info);
        } else if (s->kind == StmtKind::StructDecl) {
          const StructDecl* st = static_cast<const StructDecl*>(s.get());
          StructInfo* info = new StructInfo();
          info->decl = st;
          info->name = st->name;
          info->moduleId = mod.id;
          {
            std::string mtag = moduleTag(mod.id);
            std::string sh = stableTag(st->name);
            info->ctorCname = symWithSuffix("__na_ctor_" + mtag + "_" + sh);
            info->globalVar = symWithSuffix("__na_gs_" + mtag + "_" + sh);
            info->nameLabel = symWithSuffix("__na_sn_" + mtag + "_" + sh);
          }
          for (const FieldDecl& f : st->fields) info->fieldNames.push_back(f.name);
          structs_[st->name] = info;
          structList_.push_back(info);
          for (const auto& m : st->methods) {
            FnInfo* mi = new FnInfo();
            mi->decl = m.get();
            mi->name = m->name;
            mi->moduleId = mod.id;
            mi->isMethod = true;
            mi->structName = st->name;
            {
              std::string mtag = moduleTag(mod.id);
              std::string sh = stableTag(st->name);
              std::size_t arity = m->comptimeParams.size() + m->params.size();
              std::string dh = stableTag(m->name + "#" + std::to_string(arity));
              mi->cname = symWithSuffix("__na_m_" + mtag + "_" + sh + "_" + dh);
              mi->globalVar = symWithSuffix("__na_gf_" + mtag + "_" + sh + "_" + dh);
            }
            for (const ComptimeParam& p : m->comptimeParams) {
              mi->paramNames.push_back(p.name);
              mi->paramDefaults.push_back(nullptr);
            }
            for (const ParamDecl& p : m->params) {
              mi->paramNames.push_back(p.name);
              mi->paramDefaults.push_back(p.defaultValue.get());
              mi->variadic = mi->variadic || p.variadic;
            }
            fns_[m.get()] = mi;
            fnList_.push_back(mi);
            info->methods[m->name] = mi;
          }
        } else if (s->kind == StmtKind::EnumDecl) {
          const EnumDecl* e = static_cast<const EnumDecl*>(s.get());
          std::int64_t next = 0;
          int tag = 0;
          for (const EnumVariant& v : e->variants) {
            EnumVariantInfo ev;
            ev.algebraic = e->isAlgebraic();
            ev.tag = tag++;
            ev.value = v.value ? evalConstInt(v.value.get(), next) : next;
            next = ev.value + 1;
            ev.moduleId = mod.id;
            ev.enumName = e->name;
            ev.variantName = v.name;
            {
              std::string mtag = moduleTag(mod.id);
              std::string eh = stableTag(e->name);
              std::string vh = stableTag(v.name);
              ev.globalVar = symWithSuffix("__na_ev_" + mtag + "_" + eh + "_" + vh);
            }
            for (const FieldDecl& f : v.fields) ev.payloadFields.push_back(f.name);
            enums_[std::to_string(mod.id) + ":" + e->name + "." + v.name] = ev;
          }
        } else if (s->kind == StmtKind::Let) {
          const LetStmt* let = static_cast<const LetStmt*>(s.get());
          std::string cname = symWithSuffix("__na_top_" + moduleTag(mod.id) + "_" + stableTag(let->name));
          globals_.push_back(cname);
          topLevelGlobals_[std::to_string(mod.id) + ":" + let->name] = cname;
        } else if (s->kind == StmtKind::Assign) {
          const AssignStmt* as = static_cast<const AssignStmt*>(s.get());
          if (as->target->kind == ExprKind::Ident) {
            const std::string& nm = static_cast<const IdentExpr*>(as->target.get())->name;
            std::string key = std::to_string(mod.id) + ":" + nm;
            if (!topLevelGlobals_.count(key)) {
              std::string cname = symWithSuffix("__na_top_" + moduleTag(mod.id) + "_" + stableTag(nm));
              globals_.push_back(cname);
              topLevelGlobals_[key] = cname;
            }
          }
        }
      }
    }
  }

  std::int64_t evalConstInt(const Expr* e, std::int64_t fallback) {
    if (!e) return fallback;
    if (e->kind == ExprKind::IntLit)
      return static_cast<const IntLitExpr*>(e)->value;
    if (e->kind == ExprKind::Unary) {
      const UnaryExpr* u = static_cast<const UnaryExpr*>(e);
      if (u->op == "-") return -evalConstInt(u->operand.get(), 0);
    }
    if (e->kind == ExprKind::Ident) {
      const std::string& nm = static_cast<const IdentExpr*>(e)->name;
      if (const EnumVariantInfo* ev = findEnumByShort(nm))
        return ev->value;
    }
    return fallback;
  }

  void buildGlobalScope() {
    scopes_.clear();
    scopeDeclOrder_.clear();
    scopes_.emplace_back();
    scopeDeclOrder_.emplace_back();
    auto& g = scopes_.back();
    for (const Module& mod : graph_.modules) {
      if (!mod.program) continue;
      for (const StmtPtr& s : mod.program->statements) {
        if (s->kind == StmtKind::Let) {
          const LetStmt* let = static_cast<const LetStmt*>(s.get());
          auto it = topLevelGlobals_.find(std::to_string(mod.id) + ":" + let->name);
          if (it != topLevelGlobals_.end()) {
            ABinding b;
            b.kind = BKind::Global;
            b.sym = it->second;
            g[let->name] = b;
          }
        } else if (s->kind == StmtKind::Assign) {
          const AssignStmt* as = static_cast<const AssignStmt*>(s.get());
          if (as->target->kind == ExprKind::Ident) {
            const std::string& nm = static_cast<const IdentExpr*>(as->target.get())->name;
            auto it = topLevelGlobals_.find(std::to_string(mod.id) + ":" + nm);
            if (it != topLevelGlobals_.end()) {
              ABinding b;
              b.kind = BKind::Global;
              b.sym = it->second;
              g[nm] = b;
            }
          }
        }
      }
    }
    for (FnInfo* f : fnList_) {
      if (f->isMethod) continue;
      ABinding b;
      b.kind = BKind::FnValue;
      b.sym = f->globalVar;
      b.fn = f;
      if (!g.count(f->name)) g[f->name] = b;
    }
    for (StructInfo* st : structList_) {
      ABinding b;
      b.kind = BKind::StructCtor;
      b.sym = st->globalVar;
      b.st = st;
      if (!g.count(st->name)) g[st->name] = b;
    }
    {
      std::vector<std::string> _ekeys;
      _ekeys.reserve(enums_.size());
      for (auto& _e : enums_) _ekeys.push_back(_e.first);
      std::sort(_ekeys.begin(), _ekeys.end());
      for (const auto& _ek : _ekeys) {
        auto& kv = *enums_.find(_ek);
        ABinding b;
        b.kind = BKind::EnumValue;
        b.sym = kv.second.globalVar;
        b.en = &kv.second;
        b.aux = kv.second.variantName;
        std::string qual = kv.second.enumName + "." + kv.second.variantName;
        if (!g.count(qual)) g[qual] = b;
        if (!g.count(b.aux)) g[b.aux] = b;
      }
    }
    for (const char* nm : {"math", "io", "fs", "strings", "collections", "core",
                           "time", "json", "binary", "testing", "log", "metrics",
                           "trace", "net", "http", "terminal", "env", "repl",
                           "kernels", "unsafe", "arena", "std", "py", "kernel_ops"}) {
      ABinding b;
      b.kind = BKind::Module;
      b.moduleId = -1;
      b.aux = nm;
      g[nm] = b;
    }
    for (const Module& mod : graph_.modules) {
      ABinding b;
      b.kind = BKind::Module;
      b.moduleId = mod.id;
      b.aux = mod.name;
      // Never shadow a builtin or an existing binding (e.g. a file named
      // input.a must not turn the `input` builtin into a module).
      if (!g.count(mod.name) && builtinRuntimeId(mod.name) < 0) g[mod.name] = b;
      std::string stem = stemOf(mod.path);
      if (!g.count(stem) && builtinRuntimeId(stem) < 0) g[stem] = b;
    }
  }

  // Register a module's import aliases (import x / from x import y,
  // including native `std:` modules and `python:` modules) into the
  // current scope. Mirrors emitC's bindModuleImportStmts.
  void bindModuleImports(const Module& mod) {
    if (!mod.program) return;
    for (const StmtPtr& s : mod.program->statements) {
      if (s->kind == StmtKind::Import) {
        const ImportStmt* im = static_cast<const ImportStmt*>(s.get());
        if (im->pythonModuleImport) {
          // import python:<mod> [as x]: dynamic module, calls go through
          // the runtime py_call builtin.
          std::string alias = !im->alias.empty() ? im->alias
                               : stemOf(im->pythonModule);
          ABinding b;
          b.kind = BKind::PyModule;
          b.aux = im->pythonModule;
          scopes_.back()[alias] = b;
          continue;
        }
        std::string alias = !im->alias.empty() ? im->alias
                             : !im->bareName.empty() ? im->bareName
                             : stemOf(im->path);
        if (im->nativeModule || im->moduleId >= 0) {
          ABinding b;
          b.kind = BKind::Module;
          b.moduleId = im->moduleId;
          b.aux = im->bareName;
          scopes_.back()[alias] = b;
        }
      } else if (s->kind == StmtKind::FromImport) {
        const FromImportStmt* im = static_cast<const FromImportStmt*>(s.get());
        if (im->pythonModuleImport) {
          // from python:<mod> import name [as x]: dynamic member call.
          for (const auto& item : im->items) {
            ABinding b;
            b.kind = BKind::PyFunc;
            b.aux = im->pythonModule;
            b.pyMember = item.name;
            scopes_.back()[item.alias.empty() ? item.name : item.alias] = b;
          }
          continue;
        }
        for (const auto& item : im->items) {
          std::string alias = item.alias.empty() ? item.name : item.alias;
          if (im->nativeModule) {
            if (const std::string* bn = nativeFunction(im->bareName, item.name)) {
              int bid = builtinRuntimeId(*bn);
              if (bid >= 0) {
                ABinding b;
                b.kind = BKind::Builtin;
                b.builtinId = bid;
                scopes_.back()[alias] = b;
                continue;
              }
            }
            if (const std::string* cn = nativeConstant(im->bareName, item.name)) {
              ABinding b;
              b.kind = BKind::NativeConst;
              b.aux = *cn;
              scopes_.back()[alias] = b;
              continue;
            }
            fail("native: module '" + im->bareName + "' has no member '" +
                 item.name + "'");
            return;
          }
          if (im->moduleId >= 0) {
            ABinding b;
            if (FnInfo* mf = findModuleFn(im->moduleId, item.name)) {
              b.kind = BKind::FnValue;
              b.sym = mf->globalVar;
              b.fn = mf;
            } else if (StructInfo* mst = findModuleStruct(im->moduleId, item.name)) {
              b.kind = BKind::StructCtor;
              b.sym = mst->globalVar;
              b.st = mst;
            } else {
              std::string gsym;
              if (moduleGlobalSym(im->moduleId, item.name, gsym)) {
                b.kind = BKind::Global;
                b.sym = gsym;
              } else {
                // Unresolved member: leave unbound; use fails -> emitC fallback.
                continue;
              }
            }
            scopes_.back()[alias] = b;
          }
        }
      }
    }
  }

  // ---- function emission ----------------------------------------------------
  void emitStubBody() {
    movImm(kTag | 0x2ULL, acc());
  }

  void emitStructCtor(StructInfo* st) {
    // Two-pass: emit body into scratch, then wrap with prologue/epilogue.
    std::ostringstream saved;
    saved.swap(text_);
    slotCounter_ = 0;
    if (isStdlibModule(st->moduleId)) {
      emitStubBody();
    } else {
      scopes_.clear();
    scopeDeclOrder_.clear();
      buildGlobalScope();
      pushScope();
      if (st->moduleId >= 0 && st->moduleId < (int)graph_.modules.size())
        bindModuleImports(graph_.at(st->moduleId));
      int argvS = allocSlot();
      // argv arrives in arg0.
      storeSlot(argvS, A(0));
      int dictS = allocSlot();
      callSym("a_dict_new");
      storeAcc(dictS);
      for (std::size_t i = 0; i < st->fieldNames.size(); ++i) {
        int ks = allocSlot(), vs = allocSlot();
        std::string lbl = "__na_cs_" + st->ctorCname + "_" + std::to_string(i);
        leaSym(lbl, A(0));
        movImm(st->fieldNames[i].size(), A(1));
        callSym("a_str_new");
        storeAcc(ks);
        rodata_ << "    .section .rodata\n" << lbl << ":\n    .byte ";
        for (unsigned char c : st->fieldNames[i]) rodata_ << (int)c << ",";
        rodata_ << "0\n";
        // field value = argv[i+1]
        emitLoadArgv(argvS, (int)i + 1, vs);
        loadSlot(dictS, A(0));
        loadSlot(ks, A(1));
        loadSlot(vs, A(2));
        callSym("a_dict_set");
      }
      loadSlot(dictS, acc());
      scopes_.clear();
    scopeDeclOrder_.clear();
    }
    int frame = frameBytes();
    std::ostringstream body;
    body.swap(text_);
    text_.swap(saved);
    emit("    .text\n    .globl " + st->ctorCname + "\n" + st->ctorCname + ":\n");
    std::string pro;
    t_.prologue(pro, frame);
    text_ << pro;
    text_ << body.str();
    std::string epi;
    t_.epilogue(epi, frame);
    text_ << epi;
  }

  // Load argv[k] (k>=1) from the argv pointer stored in slot argvS into slot vs.
  void emitLoadArgv(int argvS, int k, int vs) {
    loadSlot(argvS, T(0));
    std::string o;
    t_.loadSlotAt(o, T(0), 8 * k, acc());
    emit(o);
    storeAcc(vs);
  }

  void emitFunction(FnInfo* f) {
    std::ostringstream saved;
    saved.swap(text_);
    slotCounter_ = 0;
    bool savedInFn = inFunction_;
    std::string savedRet = retLabel_;
    if (isStdlibModule(f->moduleId)) {
      // Incremental: emit A_NONE stub for stdlib/prelude bodies not yet
      // covered by the native backend. Keeps the .s linkable (main takes
      // addresses of all fn globals) without requiring full stdlib parity.
      movImm(kTag | 0x2ULL, acc());
    } else {
      scopes_.clear();
    scopeDeclOrder_.clear();
      buildGlobalScope();
      pushScope();
      if (f->moduleId >= 0 && f->moduleId < (int)graph_.modules.size())
        bindModuleImports(graph_.at(f->moduleId));
      curArgvSlot_ = allocSlot();
      storeSlot(curArgvSlot_, A(0));
      curEnvSlot_ = allocSlot();
      loadSlot(curArgvSlot_, T(0));
      {
        std::string o;
        t_.loadSlotAt(o, T(0), 0, acc());
        emit(o);
      }
      storeAcc(curEnvSlot_);
      // Params: argv[i+1], with A_TAG -> default substitution.
      for (std::size_t i = 0; i < f->paramNames.size(); ++i) {
        int ps = allocSlot();
        loadSlot(curArgvSlot_, T(0));
        std::string o;
        t_.loadSlotAt(o, T(0), 8 * ((int)i + 1), acc());
        emit(o);
        storeAcc(ps);
        ABinding b;
        b.kind = BKind::Param;
        b.slot = ps;
        scopes_.back()[f->paramNames[i]] = b;
        if (i < f->paramDefaults.size() && f->paramDefaults[i]) {
          std::string skipL = newLabel("def");
          loadSlot(ps, A(0));
          movImm(kTag, A(1));
          cmpRegs(A(0), A(1));
          jcc("ne", skipL);
          emitExpr(f->paramDefaults[i]);
          storeAcc(ps);
          emitLabel(skipL);
        }
      }
      inFunction_ = true;
      retLabel_ = newLabel("ret");
      int savedModule = curModuleId_;
      curModuleId_ = f->moduleId;
      deferStack_.emplace_back();
      curFnName_ = f->name;
      for (const StmtPtr& s : f->decl->body) emitStmt(s.get());
      curFnName_.clear();
      // fallthrough exit: defers then RAII drops, then default A_NONE.
      // explicit `return` paths emit defers inline and jump to retLabel_,
      // which sits after this block — they never double-run.
      emitDeferredFrameAndDrops();
      curModuleId_ = savedModule;
      movImm(kTag | 0x2ULL, acc());
      emitLabel(retLabel_);
      retLabel_.clear();
      scopes_.clear();
    scopeDeclOrder_.clear();
      scopeDeclOrder_.clear();
      curEnvSlot_ = -1;
      curArgvSlot_ = -1;
    }
    int frame = frameBytes();
    std::ostringstream body;
    body.swap(text_);
    text_.swap(saved);
    emit("    .text\n    .globl " + f->cname + "\n" + f->cname + ":\n");
    std::string pro;
    t_.prologue(pro, frame);
    text_ << pro;
    text_ << body.str();
    std::string epi;
    t_.epilogue(epi, frame);
    text_ << epi;
    inFunction_ = savedInFn;
    retLabel_ = savedRet;
  }

  // Emit a queued lambda as a function: argv[0]=env, argv[1..]=params.
  void emitLambdaJob(LambdaJob& job) {
    const LambdaExpr* lam = job.decl;
    std::ostringstream saved;
    saved.swap(text_);
    slotCounter_ = 0;
    scopes_.clear();
    scopeDeclOrder_.clear();
    buildGlobalScope();
    pushScope();
    if (job.moduleId >= 0 && job.moduleId < (int)graph_.modules.size())
      bindModuleImports(graph_.at(job.moduleId));
    curArgvSlot_ = allocSlot();
    storeSlot(curArgvSlot_, A(0));
    curEnvSlot_ = allocSlot();
    loadSlot(curArgvSlot_, T(0));
    {
      std::string o;
      t_.loadSlotAt(o, T(0), 0, acc());
      emit(o);
    }
    storeAcc(curEnvSlot_);
    std::vector<std::string> names;
    std::vector<const Expr*> defaults;
    bool lamVariadic = false;
    for (const ParamDecl& p : lam->params) {
      names.push_back(p.name);
      defaults.push_back(p.defaultValue.get());
      lamVariadic = lamVariadic || p.variadic;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
      int ps = allocSlot();
      loadSlot(curArgvSlot_, T(0));
      std::string o;
      t_.loadSlotAt(o, T(0), 8 * ((int)i + 1), acc());
      emit(o);
      storeAcc(ps);
      ABinding b;
      b.kind = BKind::Param;
      b.slot = ps;
      scopes_.back()[names[i]] = b;
      if (i < defaults.size() && defaults[i]) {
        std::string skipL = newLabel("def");
        loadSlot(ps, A(0));
        movImm(kTag, A(1));
        cmpRegs(A(0), A(1));
        jcc("ne", skipL);
        emitExpr(defaults[i]);
        storeAcc(ps);
        emitLabel(skipL);
      }
    }
    for (std::size_t i = 0; i < job.captures.size(); ++i) {
      ABinding b;
      b.kind = BKind::Capture;
      b.envIdx = (int)i;
      scopes_.back()[job.captures[i].name] = b;
    }
    bool savedInFn = inFunction_;
    inFunction_ = true;
    int savedModule = curModuleId_;
    curModuleId_ = job.moduleId;
    std::string savedRet = retLabel_;
    retLabel_ = newLabel("ret");
    deferStack_.emplace_back();
    if (lam->body) emitExpr(lam->body.get());
    else movImm(kTag | 0x2ULL, acc());
    emitLabel(retLabel_);
    retLabel_.clear();
    deferStack_.pop_back();
    int frame = frameBytes();
    std::ostringstream body;
    body.swap(text_);
    text_.swap(saved);
    emit("    .text\n    .globl " + job.cname + "\n" + job.cname + ":\n");
    std::string pro;
    t_.prologue(pro, frame);
    text_ << pro;
    text_ << body.str();
    std::string epi;
    t_.epilogue(epi, frame);
    text_ << epi;
    scopes_.clear();
    scopeDeclOrder_.clear();
    inFunction_ = savedInFn;
    retLabel_ = savedRet;
    curModuleId_ = savedModule;
    curEnvSlot_ = -1;
    curArgvSlot_ = -1;
    (void)lamVariadic;
  }

  void emitLambdas() {
    for (std::size_t i = 0; i < lambdaQueue_.size(); ++i) {
      if (failed_) return;
      emitLambdaJob(lambdaQueue_[i]);
    }
  }

  void emitMain() {
    std::ostringstream saved;
    saved.swap(text_);
    slotCounter_ = 0;
    scopes_.clear();
    scopeDeclOrder_.clear();
    buildGlobalScope();
    pushScope();
    allocSlot();  // slot 0 scratch
    callSym("a_runtime_init");
    // fn/struct/enum globals = a_fnon_new(entry_addr, A_NONE, name)
    // Bare-metal targets skip this: it references every function symbol
    // (defeating dead-code elimination) and fn-name metadata is unused
    // without a libc/runtime introspection layer.
    if (!t_.bareMetal()) {
    for (StructInfo* st : structList_) {
      std::string lbl = st->nameLabel;
      leaSym(st->ctorCname, A(0));
      movImm(kTag | 0x2ULL, A(1));
      leaSym(lbl, A(2));
      callSym("a_fnon_new");
      storeSym(st->globalVar, acc());
      rodata_ << "    .section .rodata\n" << lbl << ":\n    .byte ";
      for (unsigned char c : st->name) rodata_ << (int)c << ",";
      rodata_ << "0\n";
    }
    for (FnInfo* f : fnList_) {
      if (f->isMethod) continue;
      std::string lbl = "__na_sn_" + f->cname;
      leaSym(f->cname, A(0));
      movImm(kTag | 0x2ULL, A(1));
      leaSym(lbl, A(2));
      callSym("a_fnon_new");
      storeSym(f->globalVar, acc());
      rodata_ << "    .section .rodata\n" << lbl << ":\n    .byte ";
      for (unsigned char c : f->name) rodata_ << (int)c << ",";
      rodata_ << "0\n";
    }
    }
    {
      std::vector<std::string> _ekeys;
      _ekeys.reserve(enums_.size());
      for (auto& _e : enums_) _ekeys.push_back(_e.first);
      std::sort(_ekeys.begin(), _ekeys.end());
      for (const auto& _ek : _ekeys) {
        auto& kv = *enums_.find(_ek);
        if (kv.second.algebraic) continue;
        movImm(mkintA((int64_t)kv.second.value), acc());
        storeSym(kv.second.globalVar, acc());
      }
    }

    retLabel_ = newLabel("ret");
    if (testMode_) {
      emitTestMainBody();
    } else {
      std::vector<int> order = graph_.order;
      if (order.empty()) order.push_back(graph_.entry);
      for (int mid : order) {
        if (mid < 0 || failed_) continue;
        const Module& mod = graph_.at(mid);
        if (!mod.program) continue;
        if (std::getenv("A_DEBUG_NATIVE"))
          std::fprintf(stderr, "native: module id=%d name='%s' path='%s'\n",
                       mid, mod.name.c_str(), mod.path.c_str());
        if (mod.name == "prelude" ||
            mod.path.find("stdlib/") != std::string::npos ||
            mod.path.find("/stdlib/") != std::string::npos) continue;
        bindModuleImports(mod);
        for (const StmtPtr& s : mod.program->statements) {
          curFnName_ = "<toplevel:" + mod.name + ">";
          emitStmt(s.get());
        }
        curFnName_.clear();
      }
      // `fn main()` entry point: invoked once after all module top-levels,
      // entry module only. Never runs for imports or under `a test`.
      if (FnInfo* mf = findModuleFn(graph_.entry, "main")) {
        std::vector<int> argSlots;
        for (std::size_t i = 0; i < mf->paramNames.size(); ++i) {
          int s = allocSlot();
          movImm(kTag, acc());
          storeAcc(s);
          argSlots.push_back(s);
        }
        if (mf->variadic && !argSlots.empty()) {
          int lst = allocSlot();
          movImm(0, A(0));
          zeroReg(A(1));
          callSym("a_list_new");
          storeAcc(lst);
          argSlots.back() = lst;
        }
        emitOrderedCall(mf->cname, argSlots);
      }
      // program exit: drop RAII bindings in every remaining scope
      // (global scope holds module-level `x = ...` bindings).
      for (std::size_t si = 0; si < scopes_.size(); ++si) {
        auto& scope = scopes_[si];
        auto& decls = scopeDeclOrder_[si];
        std::unordered_set<std::string> seen;
        for (auto it = decls.rbegin(); it != decls.rend(); ++it) {
          seen.insert(*it);
          auto f = scope.find(*it);
          if (f != scope.end()) emitDropCall(f->second);
        }
        for (auto& kv : scope) {
          if (!seen.count(kv.first)) emitDropCall(kv.second);
        }
      }
      zeroReg(acc());
    }
    emitLabel(retLabel_);
    retLabel_.clear();

    int frame = frameBytes();
    std::ostringstream body;
    body.swap(text_);
    text_.swap(saved);
    emit("    .text\n    .globl main\nmain:\n");
    std::string pro;
    t_.prologue(pro, frame);
    text_ << pro;
    text_ << body.str();
    std::string epi;
    t_.epilogue(epi, frame);
    text_ << epi;

    // .bss globals (single-unit only; per-unit bss is partitioned by
    // emitModuleBss to avoid duplicate definitions across objects).
    if (!perUnitMode_) {
    for (const std::string& g : globals_) {
      bss_ << "    .section .bss\n    .globl " << g << "\n" << g << ":\n    .space 8\n";
    }
    for (FnInfo* f : fnList_) {
      if (!f->isMethod)
        bss_ << "    .section .bss\n    .globl " << f->globalVar << "\n"
             << f->globalVar << ":\n    .space 8\n";
    }
    for (StructInfo* st : structList_) {
      bss_ << "    .section .bss\n    .globl " << st->globalVar << "\n"
           << st->globalVar << ":\n    .space 8\n";
    }
    {
      std::vector<std::string> _ekeys;
      _ekeys.reserve(enums_.size());
      for (auto& _e : enums_) _ekeys.push_back(_e.first);
      std::sort(_ekeys.begin(), _ekeys.end());
      for (const auto& _ek : _ekeys) {
        auto& kv = *enums_.find(_ek);
        if (!kv.second.algebraic)
          bss_ << "    .section .bss\n    .globl " << kv.second.globalVar << "\n"
               << kv.second.globalVar << ":\n    .space 8\n";
      }
    }
    }
    scopes_.clear();
    scopeDeclOrder_.clear();
  }

  // Test mode: a_test_main(n, names, fns) — portable, no fork needed.
  void emitTestMainBody() {
    std::vector<FnInfo*> tests;
    for (FnInfo* f : fnList_) {
      if (!f->isMethod && f->moduleId == graph_.entry &&
          f->name.rfind("test_", 0) == 0 &&
          (testFilter_.empty() ||
           f->name.find(testFilter_) != std::string::npos))
        tests.push_back(f);
    }
    std::sort(tests.begin(), tests.end(),
              [](FnInfo* a, FnInfo* b) { return a->name < b->name; });
    if (tests.empty()) {
      zeroReg(acc());
      return;
    }
    if (t_.ptrSize() != 8) {
      // 32-bit: names[]/fns[] are packed C pointer arrays — emit them as
      // .long/.word entries in .data (aval slots are 8 bytes, wrong stride).
      std::vector<std::string> nameLbls;
      for (std::size_t i = 0; i < tests.size(); ++i) {
        std::string lbl = "__na_tn_" + std::to_string(i);
        nameLbls.push_back(lbl);
        rodata_ << "    .section .rodata\n" << lbl << ":\n    .byte ";
        for (unsigned char c : tests[i]->name) rodata_ << (int)c << ",";
        rodata_ << "0\n";
      }
      std::string namesLbl = "__na_tnames", fnsLbl = "__na_tfns";
      rodata_ << "    .section .data\n    .align 4\n" << namesLbl << ":\n";
      for (auto& l : nameLbls) rodata_ << "    .long " << l << "\n";
      rodata_ << fnsLbl << ":\n";
      for (auto* f : tests) rodata_ << "    .long " << f->cname << "\n";
      movImm(tests.size(), A(0));
      leaSym(namesLbl, A(1));
      leaSym(fnsLbl, A(2));
      callSym("a_test_main");
      return;
    }
    // Build names[] and fns[] pointer arrays in consecutive slots.
    int namesBase = allocSlot();
    for (std::size_t i = 1; i < tests.size(); ++i) allocSlot();
    int fnsBase = allocSlot();
    for (std::size_t i = 1; i < tests.size(); ++i) allocSlot();
    for (std::size_t i = 0; i < tests.size(); ++i) {
      // names[i] = ptr to NUL-terminated test name
      std::string lbl = "__na_tn_" + std::to_string(i);
      rodata_ << "    .section .rodata\n" << lbl << ":\n    .byte ";
      for (unsigned char c : tests[i]->name) rodata_ << (int)c << ",";
      rodata_ << "0\n";
      leaSym(lbl, T(0));
      storeSlot(namesBase + (int)i, T(0));
      // fns[i] = fn address
      leaSym(tests[i]->cname, T(0));
      storeSlot(fnsBase + (int)i, T(0));
    }
    movImm(tests.size(), A(0));
    leaSlot(namesBase, A(1));
    leaSlot(fnsBase, A(2));
    callSym("a_test_main");
    // result (0 pass / 1 fail) -> exit code in acc
  }

  // ---- statements -----------------------------------------------------------
  void emitBody(const std::vector<StmtPtr>& stmts) {
    pushScope();
    deferStack_.emplace_back();
    for (const StmtPtr& s : stmts) {
      if (failed_) break;
      emitStmt(s.get());
    }
    emitDeferredFrameAndDrops();
    popScope();
  }

  void emitStmt(const Stmt* s) {
    if (failed_ || !s) return;
    switch (s->kind) {
      case StmtKind::Let: {
        const LetStmt& st = static_cast<const LetStmt&>(*s);
        int sl = allocSlot();
        if (st.init) emitExpr(st.init.get());
        else movImm(kTag | 0x2ULL, acc());
        storeAcc(sl);
        bindLocal(st.name, sl);
        if (ABinding* b = lookup(st.name)) b->dropFn = findDrop(st.init.get());
        return;
      }
      case StmtKind::Assign: {
        const AssignStmt& st = static_cast<const AssignStmt&>(*s);
        // 1) compute the value (compound ops read the target first).
        int valS;
        if (st.op != "=") {
          std::string baseOp = st.op.substr(0, st.op.size() - 1);
          int curS = allocSlot(), vs = allocSlot();
          emitExpr(st.target.get());
          storeAcc(curS);
          emitExpr(st.value.get());
          storeAcc(vs);
          const char* slow = binopSlow(baseOp);
          if (!slow) {
            fail("native: unsupported compound assign " + baseOp);
            return;
          }
          call2slot(slow, curS, vs);
          valS = allocSlot();
          storeAcc(valS);
        } else {
          valS = evalToSlot(st.value.get());
        }
        // 2) store into the target.
        if (st.target->kind == ExprKind::Ident) {
          const std::string& nm =
              static_cast<const IdentExpr&>(*st.target.get()).name;
          ABinding* b = lookup(nm);
          if (!b) {
            int sl = allocSlot();
            bindLocal(nm, sl);
            b = lookup(nm);
          }
          // RAII: drop the old value before overwriting.
          if (b->dropFn) {
            ABinding old = *b;
            emitDropCall(old);
          }
          loadSlot(valS, acc());
          if (b->kind == BKind::Local || b->kind == BKind::Param) {
            storeAcc(b->slot);
          } else if (b->kind == BKind::Global || b->kind == BKind::FnValue ||
                     b->kind == BKind::StructCtor || b->kind == BKind::EnumValue) {
            storeSym(b->sym, acc());
          } else if (b->kind == BKind::Capture && curEnvSlot_ >= 0) {
            loadSlot(curEnvSlot_, A(0));
            movImm((uint64_t)b->envIdx, A(1));
            movReg(A(2), acc());
            callSym("a_env_set");
          } else {
            fail("native: unsupported assignment target");
          }
          b->dropFn = findDrop(st.value.get());
          return;
        }
        if (st.target->kind == ExprKind::Index) {
          const IndexExpr& ix = static_cast<const IndexExpr&>(*st.target.get());
          int os = allocSlot(), is = allocSlot();
          emitExpr(ix.object.get());
          storeAcc(os);
          emitExpr(ix.index.get());
          storeAcc(is);
          loadSlot(valS, A(2));
          loadSlot(is, A(1));
          loadSlot(os, A(0));
          callSym("a_setitem");
          return;
        }
        if (st.target->kind == ExprKind::Field) {
          const FieldExpr& fx = static_cast<const FieldExpr&>(*st.target.get());
          // module attribute: mod.name = v -> top-level global symbol
          if (fx.object->kind == ExprKind::Ident) {
            const std::string& mid =
                static_cast<const IdentExpr*>(fx.object.get())->name;
            ABinding* mb = lookup(mid);
            if (mb && mb->kind == BKind::Module && mb->moduleId >= 0) {
              auto rm = resolveModuleMember(mb->moduleId, fx.field);
              std::string gsym;
              if (moduleGlobalSym(rm.first, rm.second, gsym)) {
                loadSlot(valS, A(0));
                storeSym(gsym, A(0));
                return;
              }
            }
          }
          int os = allocSlot();
          emitExpr(fx.object.get());
          storeAcc(os);
          std::string lbl = "__na_fs_" + std::to_string(labelCounter_++);
          rodata_ << "    .section .rodata\n" << lbl << ":\n    .byte ";
          for (unsigned char c : fx.field) rodata_ << (int)c << ",";
          rodata_ << "0\n";
          loadSlot(os, A(0));
          leaSym(lbl, A(1));
          loadSlot(valS, A(2));
          callSym("a_setattr");
          return;
        }
        fail("native: unsupported assignment target");
        return;
      }
      case StmtKind::Expr: {
        const ExprStmt& st = static_cast<const ExprStmt&>(*s);
        emitExpr(st.expr.get());
        return;
      }
      case StmtKind::Return: {
        const ReturnStmt& st = static_cast<const ReturnStmt&>(*s);
        if (st.value) emitExpr(st.value.get());
        else movImm(kTag | 0x2ULL, acc());
        if (retLabel_.empty()) {
          fail("native: return outside function");
          return;
        }
        // deferred stmts clobber acc — park the return value first.
        int rs = allocSlot();
        storeAcc(rs);
        emitDeferredForReturn();
        loadSlot(rs, acc());
        jmp(retLabel_);
        return;
      }
      case StmtKind::If: {
        const IfStmt& st = static_cast<const IfStmt&>(*s);
        std::string endL = newLabel("ifend");
        for (std::size_t i = 0; i < st.branches.size(); ++i) {
          std::string nextL = newLabel("ifnext");
          emitExpr(st.branches[i].cond.get());
          callSym("a_truthy_raw_slow");
          jz(acc(), nextL);
          emitBody(st.branches[i].body);
          if (!failed_) jmp(endL);
          emitLabel(nextL);
        }
        if (!st.elseBody.empty()) emitBody(st.elseBody);
        emitLabel(endL);
        return;
      }
      case StmtKind::While: {
        const WhileStmt& st = static_cast<const WhileStmt&>(*s);
        std::string loopL = newLabel("while"), endL = newLabel("wend");
        emitLabel(loopL);
        emitExpr(st.cond.get());
        callSym("a_truthy_raw_slow");
        jz(acc(), endL);
        loopBreak_.push_back(endL);
        loopCont_.push_back(loopL);
        loopDeferDepth_.push_back((int)deferStack_.size());
        emitBody(st.body);
        loopDeferDepth_.pop_back();
        loopBreak_.pop_back();
        loopCont_.pop_back();
        jmp(loopL);
        emitLabel(endL);
        return;
      }
      case StmtKind::DoWhile: {
        const DoWhileStmt& st = static_cast<const DoWhileStmt&>(*s);
        std::string loopL = newLabel("dowhile"), endL = newLabel("dwend");
        emitLabel(loopL);
        loopBreak_.push_back(endL);
        loopCont_.push_back(loopL);
        loopDeferDepth_.push_back((int)deferStack_.size());
        emitBody(st.body);
        loopDeferDepth_.pop_back();
        loopBreak_.pop_back();
        loopCont_.pop_back();
        emitExpr(st.cond.get());
        callSym("a_truthy_raw_slow");
        jnz(acc(), loopL);
        emitLabel(endL);
        return;
      }
      case StmtKind::Loop: {
        const LoopStmt& st = static_cast<const LoopStmt&>(*s);
        std::string loopL = newLabel("loop"), endL = newLabel("loopend");
        emitLabel(loopL);
        loopBreak_.push_back(endL);
        loopCont_.push_back(loopL);
        loopDeferDepth_.push_back((int)deferStack_.size());
        emitBody(st.body);
        loopDeferDepth_.pop_back();
        loopBreak_.pop_back();
        loopCont_.pop_back();
        jmp(loopL);
        emitLabel(endL);
        return;
      }
      case StmtKind::For: {
        const ForStmt& st = static_cast<const ForStmt&>(*s);
        // general iterator: a_iter_new(iterable) + a_iter_next loop.
        // Ranges/lists/strings all lower to iterable avals correctly.
        int itS = allocSlot(), varS = allocSlot();
        emitExpr(st.iterable.get());
        callSym("a_iter_new");
        storeAcc(itS);
        movImm(kTag | 0x2ULL, acc());
        storeAcc(varS);
        std::string loopL = newLabel("for"), endL = newLabel("fend");
        std::string contL = newLabel("fcont");
        emitLabel(loopL);
        loadSlot(itS, A(0));
        leaSlot(varS, A(1));
        callSym("a_iter_next");
        jz(acc(), endL);
        pushScope();
        bindLocal(st.var, varS);
        loopBreak_.push_back(endL);
        loopCont_.push_back(contL);
        loopDeferDepth_.push_back((int)deferStack_.size());
        emitBody(st.body);  // scope + defers + RAII drops per iteration
        loopDeferDepth_.pop_back();
        loopBreak_.pop_back();
        loopCont_.pop_back();
        popScope();
        emitLabel(contL);
        jmp(loopL);
        emitLabel(endL);
        return;
      }
      case StmtKind::Break: {
        if (!loopBreak_.empty()) {
          int depth = loopDeferDepth_.empty() ? 0 : loopDeferDepth_.back();
          emitDeferredDownTo(depth);
          jmp(loopBreak_.back());
        } else fail("native: break outside loop");
        return;
      }
      case StmtKind::Continue: {
        if (!loopCont_.empty()) {
          int depth = loopDeferDepth_.empty() ? 0 : loopDeferDepth_.back();
          emitDeferredDownTo(depth);
          jmp(loopCont_.back());
        } else fail("native: continue outside loop");
        return;
      }
      case StmtKind::Destructure: {
        const DestructureStmt& st = static_cast<const DestructureStmt&>(*s);
        int vs = evalToSlot(st.value.get());
        for (std::size_t i = 0; i < st.names.size(); ++i) {
          if (st.names[i] == "_") continue;
          int ns = allocSlot();
          movImm(mkintA((int64_t)i), A(1));
          loadSlot(vs, A(0));
          callSym("a_getitem");
          storeAcc(ns);
          bindLocal(st.names[i], ns);
        }
        return;
      }
      case StmtKind::Defer: {
        const DeferStmt& st = static_cast<const DeferStmt&>(*s);
        if (!deferStack_.empty() && st.body) deferStack_.back().push_back(st.body.get());
        return;
      }
      case StmtKind::Import:
      case StmtKind::FromImport:
      case StmtKind::TypeAlias:
      case StmtKind::TraitDecl:
      case StmtKind::ImplDecl:
      case StmtKind::FnDecl:
      case StmtKind::StructDecl:
      case StmtKind::EnumDecl:
        return;  // declarations handled at top level / prelude stubs
      default:
        fail("native: unsupported statement");
        return;
    }
  }

  // ---- expressions ----------------------------------------------------------
  std::vector<const Expr*> positionalArgs(const CallExpr& call) {
    std::vector<const Expr*> out;
    for (auto& a : call.args) out.push_back(a.value.get());
    return out;
  }

  // ---- small helpers used by emitExpr ---------------------------------------
  // utf8Encode: shared copy in the a_runtime lib (strings/utf8.hpp).
  int noneSlot() {
    int s = allocSlot();
    movImm(kTag | 0x2ULL, acc());
    storeAcc(s);
    return s;
  }
  bool emitIdentConst(const std::string& n) {
    static const std::unordered_map<std::string, std::pair<bool, double>> k = {
        {"PI", {true, 3.141592653589793}}, {"E", {true, 2.718281828459045}},
        {"I8_MIN", {false, -128}}, {"I8_MAX", {false, 127}},
        {"I16_MIN", {false, -32768}}, {"I16_MAX", {false, 32767}},
        {"I32_MIN", {false, -2147483648}}, {"I32_MAX", {false, 2147483647}},
        {"I64_MIN", {false, -9223372036854775807.0 - 1}},
        {"I64_MAX", {false, 9223372036854775807.0}},
        {"U8_MAX", {false, 255}}, {"U16_MAX", {false, 65535}},
        {"U32_MAX", {false, 4294967295.0}},
        {"F32_MIN", {true, -3.4028234663852886e38}},
        {"F32_MAX", {true, 3.4028234663852886e38}},
        {"F64_MIN", {true, -1.7976931348623157e308}},
        {"F64_MAX", {true, 1.7976931348623157e308}},
    };
    auto it = k.find(n);
    if (it == k.end()) return false;
    if (it->second.first) {
      uint64_t bits;
      double d = it->second.second;
      std::memcpy(&bits, &d, 8);
      movImm(bits, acc());
    } else {
      // a_mkint truncates to the 47-bit payload — mkintA matches it exactly.
      movImm(mkintA((int64_t)it->second.second), acc());
    }
    return true;
  }
  void storeAccToBinding(ABinding* b) {
    if (!b) { fail("native: ++/-- on non-assignable"); return; }
    if (b->kind == BKind::Local || b->kind == BKind::Param) {
      storeAcc(b->slot);
    } else if (b->kind == BKind::Global || b->kind == BKind::FnValue ||
               b->kind == BKind::StructCtor || b->kind == BKind::EnumValue) {
      storeSym(b->sym, acc());
    } else if (b->kind == BKind::Capture && curEnvSlot_ >= 0) {
      loadSlot(curEnvSlot_, A(0));
      movImm((uint64_t)b->envIdx, A(1));
      movReg(A(2), acc());
      callSym("a_env_set");
    } else {
      fail("native: ++/-- unsupported target");
    }
  }
  void emitIncDec(const UnaryExpr& x) {
    // operand must be Ident for store-back (mirrors emitC).
    int delta = x.op == "++" ? 1 : -1;
    int oldS = allocSlot();
    emitExpr(x.operand.get());
    storeAcc(oldS);
    int oneS = allocSlot();
    movImm(mkintA(delta), acc());
    storeAcc(oneS);
    loadSlot(oneS, A(1));
    loadSlot(oldS, A(0));
    callSym("a_add_slow");
    int newS = allocSlot();
    storeAcc(newS);
    if (x.operand->kind == ExprKind::Ident) {
      ABinding* b = lookup(static_cast<const IdentExpr&>(*x.operand).name);
      loadSlot(newS, acc());
      storeAccToBinding(b);
    }
    loadSlot(x.postfix ? oldS : newS, acc());
  }

  // ---- lambda / comprehension / match ----------------------------------------
  void emitLambdaExpr(const LambdaExpr& lam) {
    std::unordered_set<std::string> bound;
    for (auto& p : lam.params) bound.insert(p.name);
    std::vector<std::string> refs;
    std::unordered_set<std::string> seen;
    collectRefs(lam.body.get(), bound, refs, seen);
    LambdaJob job;
    job.decl = &lam;
    job.moduleId = curModuleId_;
    job.cname = "__na_lam_" + moduleTag(curModuleId_) + "_" + std::to_string(lambdaSeq_[curModuleId_]++);
    for (auto& name : refs) {
      ABinding* b = lookup(name);
      if (b && (b->kind == BKind::Local || b->kind == BKind::Param ||
                b->kind == BKind::Capture)) {
        int vs;
        if (b->kind == BKind::Capture && curEnvSlot_ >= 0) {
          // materialize captured value out of the current env
          vs = allocSlot();
          loadSlot(curEnvSlot_, A(0));
          movImm((uint64_t)b->envIdx, A(1));
          callSym("a_env_get");
          storeAcc(vs);
        } else {
          vs = b->slot;
        }
        job.captures.push_back({name, vs});
      }
    }
    int envS = allocSlot();
    if (job.captures.empty()) {
      movImm(kTag | 0x2ULL, acc());
      storeAcc(envS);
    } else {
      movImm((uint64_t)job.captures.size(), A(0));
      callSym("a_closure_env_new");
      storeAcc(envS);
      for (std::size_t i = 0; i < job.captures.size(); ++i) {
        loadSlot(envS, A(0));
        movImm((uint64_t)i, A(1));
        loadSlot(job.captures[i].slot, A(2));
        callSym("a_env_set");
      }
    }
    std::string nm = "__na_lnm_" + moduleTag(curModuleId_) + "_" + std::to_string(lambdaSeq_[curModuleId_]++);
    rodata_ << "    .section .rodata\n" << nm << ":\n    .byte ";
    for (unsigned char c : "lambda") rodata_ << (int)c << ",";
    rodata_ << "0\n";
    leaSym(job.cname, A(0));
    loadSlot(envS, A(1));
    leaSym(nm, A(2));
    callSym("a_fnon_new");
    lambdaQueue_.push_back(std::move(job));
  }

  void emitComprehensionExpr(const ComprehensionExpr& c) {
    int res = allocSlot();
    if (c.isSet) callSym("a_set_new");
    else {
      movImm(0, A(0));
      zeroReg(A(1));
      callSym("a_list_new");
    }
    storeAcc(res);
    int it = allocSlot();
    emitExpr(c.iterable.get());
    callSym("a_iter_new");
    storeAcc(it);
    int var = allocSlot();
    movImm(kTag | 0x2ULL, acc());
    storeAcc(var);
    std::string loopL = newLabel("comp"), endL = newLabel("cend");
    emitLabel(loopL);
    loadSlot(it, A(0));
    leaSlot(var, A(1));
    callSym("a_iter_next");
    jz(acc(), endL);
    pushScope();
    bindLocal(c.var, var);
    std::string skipL = newLabel("cmpskip");
    bool hasCond = c.condition != nullptr;
    if (hasCond) {
      emitExpr(c.condition.get());
      callSym("a_truthy_raw_slow");
      jz(acc(), skipL);
    }
    int es = allocSlot();
    emitExpr(c.element.get());
    storeAcc(es);
    if (c.isSet) {
      loadSlot(es, A(1));
      loadSlot(res, A(0));
      callSym("a_set_add");
    } else {
      loadSlot(res, A(0));
      loadSlot(es, A(1));
      callSym("a_list_push");
    }
    if (hasCond) emitLabel(skipL);
    popScope();
    jmp(loopL);
    emitLabel(endL);
    loadSlot(res, acc());
  }

  // Pattern test: emits code leaving plain 0/1 in acc; bindings side-effect.
  void emitPatternTestVal(const Pattern& p, int subjS) {
    switch (p.kind) {
      case Pattern::Kind::Wildcard:
      case Pattern::Kind::Binding:
        movImm(1, acc());
        return;
      case Pattern::Kind::Literal: {
        int vs = evalToSlot(p.value.get());
        loadSlot(vs, A(1));
        loadSlot(subjS, A(0));
        callSym("a_eq_slow");
        callSym("a_truthy_raw_slow");
        return;
      }
      case Pattern::Kind::Range: {
        if (std::getenv("A_DEBUG_NATIVE"))
          std::fprintf(stderr, "native: range pat rs=%p re=%p\n",
                       (const void*)p.rangeStart.get(), (const void*)p.rangeEnd.get());
        int lo = evalToSlot(p.rangeStart.get());
        int hi = evalToSlot(p.rangeEnd.get());
        loadSlot(hi, A(2));
        loadSlot(lo, A(1));
        loadSlot(subjS, A(0));
        callSym("a_in_range");
        callSym("a_truthy_raw_slow");
        return;
      }
      case Pattern::Kind::Variant: {
        int cs = evalToSlot(p.ctor.get());
        if (p.subpatterns.empty()) {
          loadSlot(cs, A(1));
          loadSlot(subjS, A(0));
          callSym("a_eq_slow");
        } else {
          // compare dict __tag entry against the variant's integer tag
          std::int64_t tagVal = -1;
          if (auto* fe = dynamic_cast<const FieldExpr*>(p.ctor.get())) {
            if (auto* ie = dynamic_cast<const IdentExpr*>(fe->object.get())) {
              if (const EnumVariantInfo* ev = findEnumVariant(ie->name, fe->field))
                tagVal = ev->tag;
            }
            if (tagVal < 0) {
              if (const EnumVariantInfo* ev = findEnumByShort(fe->field))
                tagVal = ev->tag;
            }
          }
          if (tagVal >= 0) {
            movImm(mkintA(tagVal), acc());
            storeAcc(cs);
          }
          int tagS = strLitSlot("__tag");
          int noneS = noneSlot();
          loadSlot(noneS, A(2));
          loadSlot(tagS, A(1));
          loadSlot(subjS, A(0));
          callSym("a_dict_get");
          int ds = allocSlot();
          storeAcc(ds);
          loadSlot(cs, A(1));
          loadSlot(ds, A(0));
          callSym("a_eq_slow");
        }
        callSym("a_truthy_raw_slow");
        return;
      }
      case Pattern::Kind::Tuple: {
        // len check + each subpattern; short-circuit via jumps.
        int n = (int)p.subpatterns.size();
        int lenS = allocSlot();
        loadSlot(subjS, A(0));
        callSym("a_len");
        storeAcc(lenS);
        int wantS = allocSlot();
        movImm(mkintA(n), acc());
        storeAcc(wantS);
        loadSlot(wantS, A(1));
        loadSlot(lenS, A(0));
        callSym("a_eq_slow");
        callSym("a_truthy_raw_slow");
        std::string failL = newLabel("ptf");
        jz(acc(), failL);
        for (int i = 0; i < n; ++i) {
          int es = allocSlot();
          movImm(mkintA(i), A(1));
          loadSlot(subjS, A(0));
          callSym("a_getitem");
          storeAcc(es);
          emitPatternTestVal(p.subpatterns[i], es);
          jz(acc(), failL);
        }
        movImm(1, acc());
        std::string doneL = newLabel("ptd");
        jmp(doneL);
        emitLabel(failL);
        movImm(0, acc());
        emitLabel(doneL);
        return;
      }
      case Pattern::Kind::Or: {
        std::string okL = newLabel("pto"), nextL;
        for (std::size_t i = 0; i < p.subpatterns.size(); ++i) {
          nextL = newLabel("pton");
          emitPatternTestVal(p.subpatterns[i], subjS);
          jnz(acc(), okL);
          emitLabel(nextL);
        }
        movImm(0, acc());
        std::string doneL = newLabel("ptd");
        jmp(doneL);
        emitLabel(okL);
        movImm(1, acc());
        emitLabel(doneL);
        return;
      }
    }
    movImm(0, acc());
  }

  void declarePatternBindings(const Pattern& p, int subjS) {
    if (std::getenv("A_DEBUG_NATIVE"))
      std::fprintf(stderr, "native: declbind kind=%d name='%s' subs=%zu\n",
                   (int)p.kind, p.name.c_str(), p.subpatterns.size());
    switch (p.kind) {
      case Pattern::Kind::Binding: {
        int ns = allocSlot();
        loadSlot(subjS, acc());
        storeAcc(ns);
        bindLocal(p.name, ns);
        return;
      }
      case Pattern::Kind::Tuple: {
        for (std::size_t i = 0; i < p.subpatterns.size(); ++i) {
          int es = allocSlot();
          movImm(mkintA((int64_t)i), A(1));
          loadSlot(subjS, A(0));
          callSym("a_getitem");
          storeAcc(es);
          declarePatternBindings(p.subpatterns[i], es);
        }
        return;
      }
      case Pattern::Kind::Variant: {
        for (std::size_t i = 0; i < p.subpatterns.size(); ++i) {
          std::string key = i < p.variantFields.size() ? p.variantFields[i]
                                                       : p.subpatterns[i].name;
          int tagS = strLitSlot(key);
          int noneS = noneSlot();
          int vs = allocSlot();
          loadSlot(noneS, A(2));
          loadSlot(tagS, A(1));
          loadSlot(subjS, A(0));
          callSym("a_dict_get");
          storeAcc(vs);
          declarePatternBindings(p.subpatterns[i], vs);
        }
        return;
      }
      case Pattern::Kind::Or:
        for (auto& sub : p.subpatterns) declarePatternBindings(sub, subjS);
        return;
      default:
        return;
    }
  }

  void emitMatchExpr(const MatchExpr& m) {
    if (std::getenv("A_DEBUG_NATIVE"))
      std::fprintf(stderr, "native: match arms=%zu\n", m.arms.size());
    int subjS = evalToSlot(m.subject.get());
    int res = allocSlot();
    movImm(kTag | 0x2ULL, acc());
    storeAcc(res);
    std::string endL = newLabel("mtend");
    for (auto& arm : m.arms) {
      std::string nextL = newLabel("mtnext");
      pushScope();
      declarePatternBindings(arm.pattern, subjS);
      if (std::getenv("A_DEBUG_NATIVE"))
        std::fprintf(stderr, "native: arm kind=%d\n", (int)arm.pattern.kind);
      emitPatternTestVal(arm.pattern, subjS);
      if (std::getenv("A_DEBUG_NATIVE")) std::fprintf(stderr, "native: tested\n");
      jz(acc(), nextL);
      if (arm.guard) {
        emitExpr(arm.guard.get());
        callSym("a_truthy_raw_slow");
        jz(acc(), nextL);
      }
      if (std::getenv("A_DEBUG_NATIVE")) std::fprintf(stderr, "native: result\n");
      emitExpr(arm.result.get());
      storeAcc(res);
      popScope();
      jmp(endL);
      emitLabel(nextL);
      // scope already popped above — jz target shares the same emitter scope
    }
    // no arm matched -> a_match_fail(subj)
    loadSlot(subjS, A(0));
    callSym("a_match_fail");
    storeAcc(res);
    emitLabel(endL);
    loadSlot(res, acc());
  }

  void emitExpr(const Expr* e) {
    if (failed_ || !e) {
      movImm(kTag | 0x2ULL, acc());
      return;
    }
    switch (e->kind) {
      case ExprKind::IntLit:
        emitIntLit(static_cast<const IntLitExpr*>(e)->value);
        return;
      case ExprKind::FloatLit:
        emitDoubleLit(static_cast<const FloatLitExpr*>(e)->value);
        return;
      case ExprKind::BoolLit:
        movImm(static_cast<const BoolLitExpr*>(e)->value ? (kTag | 0x6)
                                                        : (kTag | 0x4),
               acc());
        return;
      case ExprKind::StringLit: {
        auto& x = static_cast<const StringLitExpr&>(*e);
        if (x.parts.size() == 1 && !x.parts[0].isExpr) {
          emitStringLit(x.parts[0].text);
          return;
        }
        if (x.parts.empty()) {
          emitStringLit("");
          return;
        }
        // Interpolation: a_str_joinsep("", n, argv) over str(part)/str_new(text).
        int n = (int)x.parts.size();
        Argv av = allocArgv(n);
        int sepS = allocSlot();
        emitStringLit("");
        storeAcc(sepS);
        for (int i = 0; i < n; ++i) {
          auto& p = x.parts[i];
          if (p.isExpr) {
            int es = allocSlot();
            emitExpr(p.expr.get());
            callSym("a_str");
            storeAcc(es);
            argvStore(av, i, es);
          } else {
            int ts = allocSlot();
            emitStringLit(p.text);
            storeAcc(ts);
            argvStore(av, i, ts);
          }
        }
        loadSlot(sepS, A(0));
        movImm((uint64_t)n, A(1));
        leaSlot(av.base, A(2));
        callSym("a_str_joinsep");
        return;
      }
      case ExprKind::CharLit: {
        auto& x = static_cast<const CharLitExpr&>(*e);
        std::string u = utf8Encode(x.value);
        std::string l = emitStrData(u, false);
        leaSym(l, A(0));
        movImm(u.size(), A(1));
        callSym("a_char_new");
        return;
      }
      case ExprKind::Ident: {
        auto& x = static_cast<const IdentExpr&>(*e);
        if (x.name == "None") {
          movImm(kTag | 0x2ULL, acc());
          return;
        }
        if (x.name == "true" || x.name == "True") {
          movImm(kTag | 0x6, acc());
          return;
        }
        if (x.name == "false" || x.name == "False") {
          movImm(kTag | 0x4, acc());
          return;
        }
        if (emitIdentConst(x.name)) return;
        ABinding* b = lookup(x.name);
        if (b) {
          if (b->kind == BKind::Local || b->kind == BKind::Param) {
            loadSlot(b->slot, acc());
            return;
          }
          if (b->kind == BKind::Capture) {
            // env = curEnvSlot_; a_env_get(env, idx)
            loadSlot(curEnvSlot_, A(0));
            movImm((uint64_t)b->envIdx, A(1));
            callSym("a_env_get");
            return;
          }
          if (b->kind == BKind::Global || b->kind == BKind::FnValue ||
              b->kind == BKind::StructCtor || b->kind == BKind::EnumValue) {
            loadSym(b->sym, acc());
            return;
          }
          if (b->kind == BKind::NativeConst) {
            emitNativeConst(b->aux);
            return;
          }
          if (b->kind == BKind::Builtin) {
            movImm((uint64_t)b->builtinId, A(0));
            callSym("a_builtin");
            return;
          }
          if (b->kind == BKind::Module || b->kind == BKind::PyModule ||
              b->kind == BKind::PyFunc) {
            fail("native: module '" + x.name + "' is not a value");
            return;
          }
        }
        int bid = builtinRuntimeId(x.name);
        if (bid >= 0) {
          movImm((uint64_t)bid, A(0));
          callSym("a_builtin");
          return;
        }
        fail("native: undefined name '" + x.name + "'");
        return;
      }
      case ExprKind::Unary: {
        auto& x = static_cast<const UnaryExpr&>(*e);
        if (x.op == "!") {
          // postfix unwrap ('!' is always unwrap; 'not' is logical not)
          emitExpr(x.operand.get());
          callSym("a_unwrap");
          return;
        }
        if (x.op == "++" || x.op == "--") {
          emitIncDec(x);
          return;
        }
        emitExpr(x.operand.get());
        if (x.op == "-") {
          movReg(A(0), acc());
          callSym("a_neg_slow");
          return;
        }
        if (x.op == "not") {
          movReg(A(0), acc());
          callSym("a_not");
          return;
        }
        if (x.op == "~") {
          movReg(A(0), acc());
          callSym("a_bnot_slow");
          return;
        }
        fail("native: unsupported unary " + x.op);
        return;
      }
      case ExprKind::Binary: {
        auto& x = static_cast<const BinaryExpr&>(*e);
        if (x.op == "and" || x.op == "or") {
          std::string endL = newLabel("sc");
          int ls = allocSlot();
          emitExpr(x.lhs.get());
          storeAcc(ls);
          loadSlot(ls, A(0));
          callSym("a_truthy_raw_slow");
          if (x.op == "and") jz(acc(), endL);
          else jnz(acc(), endL);
          emitExpr(x.rhs.get());
          storeAcc(ls);
          emitLabel(endL);
          loadSlot(ls, acc());
          return;
        }
        if (x.op == "??") {
          // is_none(lhs) ? rhs : unwrap(lhs)
          int ls = allocSlot(), res = allocSlot();
          std::string elseL = newLabel("qqe"), endL = newLabel("qqt");
          emitExpr(x.lhs.get());
          storeAcc(ls);
          loadSlot(ls, A(0));
          jmpIfNone(elseL);
          loadSlot(ls, A(0));
          callSym("a_unwrap");
          storeAcc(res);
          jmp(endL);
          emitLabel(elseL);
          emitExpr(x.rhs.get());
          storeAcc(res);
          emitLabel(endL);
          loadSlot(res, acc());
          return;
        }
        if (x.op == "..") {
          int ls = allocSlot(), rs = allocSlot();
          emitExpr(x.lhs.get());
          storeAcc(ls);
          emitExpr(x.rhs.get());
          storeAcc(rs);
          movImm(mkintA(1), A(2));
          loadSlot(rs, A(1));
          loadSlot(ls, A(0));
          callSym("a_range_new");
          return;
        }
        if (x.op == "in" || x.op == "not in") {
          int ls = allocSlot(), rs = allocSlot();
          emitExpr(x.lhs.get());
          storeAcc(ls);
          emitExpr(x.rhs.get());
          storeAcc(rs);
          // a_contains(container=r, item=l)
          loadSlot(ls, A(1));
          loadSlot(rs, A(0));
          callSym("a_contains");
          if (x.op == "not in") callSym("a_not");
          return;
        }
        const char* slow = binopSlow(x.op);
        if (!slow) {
          fail("native: unsupported binary op " + x.op);
          return;
        }
        int ls = allocSlot(), rs = allocSlot();
        emitExpr(x.lhs.get());
        storeAcc(ls);
        emitExpr(x.rhs.get());
        storeAcc(rs);
        call2slot(slow, ls, rs);
        return;
      }
      case ExprKind::Range: {
        auto& x = static_cast<const RangeExpr&>(*e);
        int ls = evalToSlot(x.start.get());
        int hs = evalToSlot(x.end.get());
        int ss;
        if (x.step) ss = evalToSlot(x.step.get());
        else { ss = allocSlot(); movImm(mkintA(1), acc()); storeAcc(ss); }
        loadSlot(ss, A(2));
        loadSlot(hs, A(1));
        loadSlot(ls, A(0));
        callSym("a_range_new");
        return;
      }
      case ExprKind::Slice: {
        auto& x = static_cast<const SliceExpr&>(*e);
        int os = evalToSlot(x.object.get());
        int lo = x.start ? evalToSlot(x.start.get()) : noneSlot();
        int hi = x.end ? evalToSlot(x.end.get()) : noneSlot();
        int st = x.step ? evalToSlot(x.step.get()) : noneSlot();
        loadSlot(st, A(3));
        loadSlot(hi, A(2));
        loadSlot(lo, A(1));
        loadSlot(os, A(0));
        callSym("a_slice");
        return;
      }
      case ExprKind::ArrayLit: {
        auto& x = static_cast<const ArrayLitExpr&>(*e);
        if (x.elements.empty()) {
          movImm(0, A(0));
          zeroReg(A(1));
          callSym("a_list_new");
          return;
        }
        Argv av = allocArgv((int)x.elements.size());
        for (std::size_t i = 0; i < x.elements.size(); ++i) {
          int s = allocSlot();
          emitExpr(x.elements[i].get());
          storeAcc(s);
          argvStore(av, (int)i, s);
        }
        movImm((uint64_t)x.elements.size(), A(0));
        leaSlot(av.base, A(1));
        callSym("a_list_new");
        return;
      }
      case ExprKind::Tuple: {
        auto& x = static_cast<const TupleExpr&>(*e);
        if (x.elements.empty()) {
          movImm(0, A(0));
          zeroReg(A(1));
          callSym("a_tuple_new");
          return;
        }
        Argv av = allocArgv((int)x.elements.size());
        for (std::size_t i = 0; i < x.elements.size(); ++i) {
          int s = allocSlot();
          emitExpr(x.elements[i].get());
          storeAcc(s);
          argvStore(av, (int)i, s);
        }
        movImm((uint64_t)x.elements.size(), A(0));
        leaSlot(av.base, A(1));
        callSym("a_tuple_new");
        return;
      }
      case ExprKind::MapLit: {
        auto& x = static_cast<const MapLitExpr&>(*e);
        int ds = allocSlot();
        callSym("a_dict_new");
        storeAcc(ds);
        for (auto& kv : x.entries) {
          int ks = evalToSlot(kv.first.get());
          int vs = evalToSlot(kv.second.get());
          loadSlot(vs, A(2));
          loadSlot(ks, A(1));
          loadSlot(ds, A(0));
          callSym("a_dict_set");
        }
        loadSlot(ds, acc());
        return;
      }
      case ExprKind::SetLit: {
        auto& x = static_cast<const SetLitExpr&>(*e);
        int ss = allocSlot();
        callSym("a_set_new");
        storeAcc(ss);
        for (auto& el : x.elements) {
          int vs = evalToSlot(el.get());
          loadSlot(vs, A(1));
          loadSlot(ss, A(0));
          callSym("a_set_add");
        }
        loadSlot(ss, acc());
        return;
      }
      case ExprKind::Comprehension: {
        emitComprehensionExpr(static_cast<const ComprehensionExpr&>(*e));
        return;
      }
      case ExprKind::Match: {
        emitMatchExpr(static_cast<const MatchExpr&>(*e));
        return;
      }
      case ExprKind::Try: {
        auto& x = static_cast<const TryExpr&>(*e);
        int vs = evalToSlot(x.operand.get());
        loadSlot(vs, A(0));
        callSym("a_try_result");
        int ts = allocSlot();
        storeAcc(ts);
        if (inFunction_ && !retLabel_.empty()) {
          std::string skipL = newLabel("tryok");
          loadSlot(ts, A(0));
          callSym("a_is_propagate");
          jz(acc(), skipL);
          emitDeferredForReturn();
          loadSlot(ts, acc());
          jmp(retLabel_);
          emitLabel(skipL);
        }
        loadSlot(ts, acc());
        return;
      }
      case ExprKind::Lambda: {
        emitLambdaExpr(static_cast<const LambdaExpr&>(*e));
        return;
      }
      case ExprKind::SafeNav: {
        auto& x = static_cast<const SafeNavExpr&>(*e);
        int os = evalToSlot(x.object.get());
        int rs = allocSlot();
        movImm(kTag | 0x2ULL, acc());
        storeAcc(rs);
        std::string endL = newLabel("safenav");
        loadSlot(os, A(0));
        jmpIfNone(endL);
        loadSlot(os, A(0));
        callSym("a_unwrap");
        std::string lbl = "__na_snv_" + std::to_string(labelCounter_++);
        rodata_ << "    .section .rodata\n" << lbl << ":\n    .byte ";
        for (unsigned char c : x.field) rodata_ << (int)c << ",";
        rodata_ << "0\n";
        leaSym(lbl, A(1));
        callSym("a_getattr");
        // Already-optional field values are returned as-is so `a?.b` flattens
        // to None instead of Some(None); for non-optional/dynamic fields wrap a
        // present value in Some but flatten a None and pass an already-Some
        // value through (untyped optional fields).
        if (!x.fieldOptional) {
          int valS = allocSlot();
          storeAcc(valS);
          std::string vnone = newLabel("safevn");
          std::string vkeep = newLabel("safevk");
          std::string vdone = newLabel("safevd");
          loadSlot(valS, A(0));
          jmpIfNone(vnone);
          loadSlot(valS, A(0));
          callSym("a_is_option");
          jnz(acc(), vkeep);
          loadSlot(valS, A(0));
          callSym("a_some_new");
          jmp(vdone);
          emitLabel(vnone);
          movImm(kTag | 0x2ULL, acc());
          jmp(vdone);
          emitLabel(vkeep);
          loadSlot(valS, acc());
          emitLabel(vdone);
        }
        storeAcc(rs);
        emitLabel(endL);
        loadSlot(rs, acc());
        return;
      }
      case ExprKind::Call: {
        auto& x = static_cast<const CallExpr&>(*e);
        auto args = positionalArgs(x);
        // Sema-resolved callee FnDecl (prelude fallback / desugars).
        if (x.calleeFunction) {
          for (FnInfo* f : fnList_) {
            if (!f->isMethod && f->decl == x.calleeFunction) {
              emitFnCallOrdered(f, x);
              return;
            }
          }
        }
        if (x.calleeStruct) {
          for (StructInfo* st : structList_) {
            if (st->decl == x.calleeStruct) {
              emitStructCall(st, x);
              return;
            }
          }
        }
        if (x.builtinId != BuiltinId::None) {
          int id = builtinIdForBuiltin(x.builtinId);
          if (id >= 0) {
            emitBiCall(id, args);
            return;
          }
        }
        if (x.callee->kind == ExprKind::Ident) {
          const std::string& nm =
              static_cast<const IdentExpr*>(x.callee.get())->name;
          // Sema-resolved call to a runtime builtin: use the builtin id,
          // never the ambiguous cross-module bare-name binding table.
          if (x.calleeIsBuiltin) {
            int bid = builtinRuntimeId(x.calleeBuiltin.empty() ? nm
                                                                 : x.calleeBuiltin);
            if (bid >= 0) {
              emitBiCall(bid, args);
              return;
            }
          }
          ABinding* b = lookup(nm);
          if (b && b->kind == BKind::PyFunc) {
            emitPyCall(b->aux, b->pyMember, x);
            return;
          }
          if (b && b->kind == BKind::FnValue && b->fn) {
            emitFnCallOrdered(b->fn, x);
            return;
          }
          if (b && b->kind == BKind::StructCtor && b->st) {
            emitStructCall(b->st, x);
            return;
          }
          if (b && b->kind == BKind::Builtin) {
            emitBiCall(b->builtinId, args);
            return;
          }
          if (b && b->kind == BKind::Module) {
            fail("native: cannot call module '" + nm + "'");
            return;
          }
          if (b && b->kind == BKind::NativeConst) {
            fail("native: value is not callable");
            return;
          }
          int bid = builtinRuntimeId(nm);
          if (bid >= 0) {
            emitBiCall(bid, args);
            return;
          }
          if (b) {
            // variable holding a function -> generic a_call
            emitACall(x.callee.get(), args);
            return;
          }
          fail("native: undefined function '" + nm + "'");
          return;
        }
        if (x.callee->kind == ExprKind::Field) {
          auto& fx = static_cast<const FieldExpr&>(*x.callee.get());
          if (fx.object->kind == ExprKind::Ident) {
            const std::string& mid =
                static_cast<const IdentExpr*>(fx.object.get())->name;
            ABinding* mb = lookup(mid);
            if (mb && mb->kind == BKind::PyModule) {
              emitPyCall(mb->aux, fx.field, x);
              return;
            }
            if (mb && mb->kind == BKind::Module) {
              if (mb->moduleId >= 0) {
                // A module member: mod.fn / mod.Struct / mod.CONST. Resolve
                // the member first; a builtin with the same name is only a
                // fallback (kernels.matmul is the A function).
                // Follow __init__ re-exports to the declaring module.
                auto rm = resolveModuleMember(mb->moduleId, fx.field);
                if (FnInfo* mf = findModuleFn(rm.first, rm.second)) {
                  emitFnCallOrdered(mf, x);
                  return;
                }
                if (StructInfo* mst = findModuleStruct(rm.first, rm.second)) {
                  emitStructCall(mst, x);
                  return;
                }
                // Module-level let holding a callable: mod.handler(...).
                std::string gsym;
                if (moduleGlobalSym(rm.first, rm.second, gsym)) {
                  int fs = allocSlot();
                  loadSym(gsym, acc());
                  storeAcc(fs);
                  Argv av = allocArgv((int)args.size());
                  for (std::size_t i = 0; i < args.size(); ++i) {
                    int s = allocSlot();
                    emitExpr(args[i]);
                    storeAcc(s);
                    argvStore(av, (int)i, s);
                  }
                  loadSlot(fs, A(0));
                  movImm(args.size(), A(1));
                  if (args.empty()) zeroReg(A(2));
                  else leaSlot(av.base, A(2));
                  callSym("a_call");
                  return;
                }
                // Member not defined in the file module: fall back to the
                // builtin with the same name.
                int bid = builtinRuntimeId(fx.field);
                if (bid >= 0) { emitBiCall(bid, args); return; }
                fail("native: unknown module member '" + mid + "." + fx.field + "'");
                return;
              }
              // native module member: math.sqrt etc -> builtin
              int bid = -1;
              if (const std::string* fn = nativeFunction(mb->aux, fx.field))
                bid = builtinRuntimeId(*fn);
              if (bid < 0) bid = builtinRuntimeId(fx.field);
              if (bid >= 0) { emitBiCall(bid, args); return; }
              fail("native: native module function '" + mb->aux + "." +
                   fx.field + "' not supported");
              return;
            }
          }
          // Algebraic enum variant construction: Enum.Variant(args...).
          {
            TypePtr eot = resolveType(fx.object->inferredType);
            if (eot && eot->kind == TypeKind::Enum) {
              std::string en = eot->structName;
              std::size_t sep = en.find_last_of(".:");
              if (sep != std::string::npos) en = en.substr(sep + 1);
              const EnumVariantInfo* ev =
                  findEnumVariant(en, fx.field, eot->moduleId);
              if (ev && ev->algebraic) {
                emitEnumVariantValue(*ev, fx.field);
                int ds = allocSlot();
                storeAcc(ds);
                for (std::size_t i = 0;
                     i < args.size() && i < ev->payloadFields.size(); ++i) {
                  int vs = evalToSlot(args[i]);
                  int ks = strLitSlot(ev->payloadFields[i]);
                  loadSlot(ds, A(0));
                  loadSlot(ks, A(1));
                  loadSlot(vs, A(2));
                  callSym("a_dict_set");
                }
                loadSlot(ds, acc());
                return;
              }
            }
          }
          // struct method with known type -> direct call (self = argv[1])
          TypePtr ot = resolveType(fx.object->inferredType);
          if (std::getenv("A_DEBUG_NATIVE"))
            std::fprintf(stderr, "native: method %s.%s ot=%p kind=%d sname='%s'\n",
                         fx.object->kind == ExprKind::Ident
                             ? static_cast<const IdentExpr*>(fx.object.get())->name.c_str()
                             : "?",
                         fx.field.c_str(), (const void*)ot.get(),
                         ot ? (int)ot->kind : -1,
                         ot ? ot->structName.c_str() : "");
          if (ot && ot->kind == TypeKind::Struct) {
            StructInfo* st = nullptr;
            std::string sname = ot->structName;
            std::size_t sep = sname.find_last_of(".:");
            if (sep != std::string::npos) sname = sname.substr(sep + 1);
            st = findStructByName(sname);
            if (st) {
              auto mit = st->methods.find(fx.field);
              if (mit != st->methods.end()) {
                FnInfo* mf = mit->second;
                // order args against params minus self
                std::vector<std::string> rest(mf->paramNames.begin() + 1,
                                              mf->paramNames.end());
                int vi = mf->variadic
                             ? (int)rest.size() - 1 : -1;
                auto slots = orderArgSlots(x, rest, vi, false);
                int os = evalToSlot(fx.object.get());
                emitOrderedCall(mf->cname, slots, os);
                return;
              }
            }
          }
          // Fallback: method name provided by exactly one struct (untyped
          // receiver), unless the static type is a known runtime collection.
          {
            bool isRuntimeValue = ot && ot->kind != TypeKind::Struct &&
                                  ot->kind != TypeKind::Error &&
                                  ot->kind != TypeKind::Var;
            FnInfo* candidate = nullptr;
            int candidates = 0;
            if (!isRuntimeValue && !isBuiltinValueMethod(fx.field)) {
              for (StructInfo* st : structList_) {
                auto mit = st->methods.find(fx.field);
                if (mit != st->methods.end()) {
                  candidate = mit->second;
                  ++candidates;
                }
              }
            }
            if (candidates == 1 && candidate) {
              std::vector<std::string> rest(candidate->paramNames.begin() + 1,
                                            candidate->paramNames.end());
              int vi = candidate->variadic ? (int)rest.size() - 1 : -1;
              auto slots = orderArgSlots(x, rest, vi, false);
              int os = evalToSlot(fx.object.get());
              emitOrderedCall(candidate->cname, slots, os);
              return;
            }
          }
          // generic method call: a_method(obj, name, n, nkw=0, argv)
          int os = allocSlot();
          emitExpr(fx.object.get());
          storeAcc(os);
          Argv av = allocArgv((int)args.size());
          for (std::size_t i = 0; i < args.size(); ++i) {
            int s = allocSlot();
            emitExpr(args[i]);
            storeAcc(s);
            argvStore(av, (int)i, s);
          }
          std::string lbl = "__na_mn_" + std::to_string(labelCounter_++);
          rodata_ << "    .section .rodata\n" << lbl << ":\n    .byte ";
          for (unsigned char c : fx.field) rodata_ << (int)c << ",";
          rodata_ << "0\n";
          loadSlot(os, A(0));
          leaSym(lbl, A(1));
          movImm(args.size(), A(2));
          zeroReg(A(3));  // nkw = 0
          if (args.empty()) zeroReg(A(4));
          else leaSlot(av.base, A(4));
          callSym("a_method");
          return;
        }
        emitACall(x.callee.get(), args);
        return;
      }
      case ExprKind::Index: {
        auto& x = static_cast<const IndexExpr&>(*e);
        int os = allocSlot(), is = allocSlot();
        emitExpr(x.object.get());
        storeAcc(os);
        emitExpr(x.index.get());
        storeAcc(is);
        loadSlot(is, A(1));
        loadSlot(os, A(0));
        callSym("a_getitem");
        return;
      }
      case ExprKind::Field: {
        auto& x = static_cast<const FieldExpr&>(*e);
        // Module-qualified chain: c.Color.Red, c.LIMIT, mod.fn (as value)
        {
          std::vector<std::string> chain;
          const Expr* cur = e;
          while (cur && cur->kind == ExprKind::Field) {
            chain.push_back(static_cast<const FieldExpr*>(cur)->field);
            cur = static_cast<const FieldExpr*>(cur)->object.get();
          }
          if (cur && cur->kind == ExprKind::Ident && !chain.empty()) {
            std::reverse(chain.begin(), chain.end());
            const std::string& first =
                static_cast<const IdentExpr*>(cur)->name;
            ABinding* mb = lookup(first);
            if (mb && mb->kind == BKind::Module) {
              if (mb->moduleId >= 0) {
                // Enum variant via module: c.Color.Red
                if (chain.size() >= 2) {
                  if (const EnumVariantInfo* evp =
                          findEnumVariant(chain[chain.size() - 2],
                                          chain.back(), mb->moduleId)) {
                    emitEnumVariantValue(*evp, chain.back());
                    return;
                  }
                }
                // Top-level let / fn / struct in that module: c.LIMIT
                // (following `from x import y` re-exports in __init__ files).
                if (chain.size() == 1) {
                  auto rm = resolveModuleMember(mb->moduleId, chain[0]);
                  if (FnInfo* mf = findModuleFn(rm.first, rm.second)) {
                    loadSym(mf->globalVar, acc());
                    return;
                  }
                  if (StructInfo* mst =
                          findModuleStruct(rm.first, rm.second)) {
                    loadSym(mst->globalVar, acc());
                    return;
                  }
                  std::string gsym;
                  if (moduleGlobalSym(rm.first, rm.second, gsym)) {
                    loadSym(gsym, acc());
                    return;
                  }
                }
              } else if (chain.size() == 1) {
                // Native module member as a value: math.PI, math.sqrt
                if (const std::string* cn =
                        nativeConstant(mb->aux, chain[0])) {
                  emitNativeConst(*cn);
                  return;
                }
                if (const std::string* fn =
                        nativeFunction(mb->aux, chain[0])) {
                  int bid = builtinRuntimeId(*fn);
                  if (bid >= 0) {
                    movImm((uint64_t)bid, A(0));
                    callSym("a_builtin");
                    return;
                  }
                }
                int bid = builtinRuntimeId(chain[0]);
                if (bid >= 0) {
                  movImm((uint64_t)bid, A(0));
                  callSym("a_builtin");
                  return;
                }
                fail("native: unknown native module member '" + first + "." +
                     chain[0] + "'");
                return;
              }
            }
          }
        }
        // Enum variant: Color.Red
        if (x.object->kind == ExprKind::Ident) {
          const std::string& oname =
              static_cast<const IdentExpr*>(x.object.get())->name;
          if (const EnumVariantInfo* evp = findEnumVariant(oname, x.field)) {
            emitEnumVariantValue(*evp, x.field);
            return;
          }
          ABinding* mb = lookup(oname);
          if (mb && mb->kind == BKind::Module) {
            if (const EnumVariantInfo* evp =
                    findEnumByShort(x.field, mb->moduleId)) {
              emitEnumVariantValue(*evp, x.field);
              return;
            }
          }
        }
        int os = allocSlot();
        emitExpr(x.object.get());
        storeAcc(os);
        std::string lbl = "__na_fld_" + std::to_string(labelCounter_++);
        rodata_ << "    .section .rodata\n" << lbl << ":\n    .byte ";
        for (unsigned char c : x.field) rodata_ << (int)c << ",";
        rodata_ << "0\n";
        loadSlot(os, A(0));
        leaSym(lbl, A(1));
        callSym("a_getattr");
        return;
      }
      case ExprKind::Ternary: {
        auto& x = static_cast<const TernaryExpr&>(*e);
        std::string elseL = newLabel("tern"), endL = newLabel("tend");
        int rs = allocSlot();
        emitExpr(x.cond.get());
        callSym("a_truthy_raw_slow");
        jz(acc(), elseL);
        emitExpr(x.thenValue.get());
        storeAcc(rs);
        jmp(endL);
        emitLabel(elseL);
        emitExpr(x.elseValue.get());
        storeAcc(rs);
        emitLabel(endL);
        loadSlot(rs, acc());
        return;
      }
      default:
        fail("native: unsupported expression kind " +
             std::to_string((int)e->kind));
        movImm(kTag | 0x2ULL, acc());
        return;
    }
  }
};

// RISC-V 64 (LP64D, RV64GC): acc/arg0 = a0, args a0..a7, scratch t0..t3.
// Frame: addi sp,sp,-frame; sd ra,(frame-8)(sp); slots at 8*i(sp).
// No flags: cmpRegs records operands, jcc emits the appropriate branch.
struct Riscv64Target : NatTarget {
  mutable std::string lastA, lastB;
  mutable std::ostringstream cpool_;
  mutable int cpoolN_ = 0;
  int frameReserve() const override { return 8; }  // ra saved inside frame
  std::string extraRodata() const override { return cpool_.str(); }
  static int alignFrame(int n) { return n < 16 ? 16 : (n + 15) & ~15; }
  void prologue(std::string& o, int frameBytes) const override {
    frameBytes = alignFrame(frameBytes);
    o += "    addi sp, sp, -" + std::to_string(frameBytes) + "\n";
    // sd/ld imm is ±2048; large frames need an explicit address.
    if (frameBytes - 8 <= 2047)
      o += "    sd ra, " + std::to_string(frameBytes - 8) + "(sp)\n";
    else {
      o += "    li t4, " + std::to_string(frameBytes - 8) + "\n";
      o += "    add t4, sp, t4\n    sd ra, 0(t4)\n";
    }
  }
  void epilogue(std::string& o, int frameBytes) const override {
    frameBytes = alignFrame(frameBytes);
    if (frameBytes - 8 <= 2047)
      o += "    ld ra, " + std::to_string(frameBytes - 8) + "(sp)\n";
    else {
      o += "    li t4, " + std::to_string(frameBytes - 8) + "\n";
      o += "    add t4, sp, t4\n    ld ra, 0(t4)\n";
    }
    o += "    addi sp, sp, " + std::to_string(frameBytes) + "\n";
    o += "    ret\n";
  }
  std::string slotRef(int i) const override {
    return std::to_string(8 * i) + "(sp)";
  }
  std::string acc() const override { return "a0"; }
  int nArgRegs() const override { return 8; }
  std::string argReg(int i) const override { return "a" + std::to_string(i); }
  std::string tmpReg(int i) const override {
    static const char* t[] = {"t0", "t1", "t2", "t3"};
    return t[i < 4 ? i : 3];
  }
  void movImm(std::string& o, uint64_t v, const std::string& r) const override {
    char buf[96];
    if (v == 0) {
      std::snprintf(buf, sizeof buf, "    li %s, 0\n", r.c_str());
      o += buf;
      return;
    }
    // astra-riscv64's li expansion is unreliable for some 64-bit values
    // (notably NaN-boxed aval constants). Load from a .rodata constant.
    std::string lbl = "__rv_lc_" + std::to_string(cpoolN_++);
    cpool_ << "    .section .rodata\n    .align 8\n" << lbl
           << ":\n    .quad 0x";
    std::snprintf(buf, sizeof buf, "%llx", (unsigned long long)v);
    cpool_ << buf << "\n";
    o += "    lla " + r + ", " + lbl + "\n";
    o += "    ld " + r + ", 0(" + r + ")\n";
  }
  void movReg(std::string& o, const std::string& d, const std::string& s) const override {
    if (d != s) o += "    mv " + d + ", " + s + "\n";
  }
  void loadSlot(std::string& o, int slot, const std::string& r) const override {
    int64_t off = 8 * (int64_t)slot;
    if (off >= -2048 && off <= 2047) {
      o += "    ld " + r + ", " + slotRef(slot) + "\n";
    } else {
      o += "    li t4, " + std::to_string(off) + "\n";
      o += "    add t4, sp, t4\n    ld " + r + ", 0(t4)\n";
    }
  }
  void storeSlot(std::string& o, const std::string& r, int slot) const override {
    int64_t off = 8 * (int64_t)slot;
    if (off >= -2048 && off <= 2047) {
      o += "    sd " + r + ", " + slotRef(slot) + "\n";
    } else {
      o += "    li t4, " + std::to_string(off) + "\n";
      o += "    add t4, sp, t4\n    sd " + r + ", 0(t4)\n";
    }
  }
  void loadSlotAt(std::string& o, const std::string& baseReg,
                  int64_t byteOff, const std::string& dstReg) const override {
    // ld supports 12-bit signed imm; for larger offsets use add+ld.
    if (byteOff >= -2048 && byteOff <= 2047) {
      o += "    ld " + dstReg + ", " + std::to_string(byteOff) + "(" + baseReg + ")\n";
    } else {
      o += "    li t4, " + std::to_string(byteOff) + "\n";
      o += "    add t4, " + baseReg + ", t4\n";
      o += "    ld " + dstReg + ", 0(t4)\n";
    }
  }
  void copySlot(std::string& o, int from, int to) const override {
    loadSlot(o, from, "t5");
    storeSlot(o, "t5", to);
  }
  void leaSlot(std::string& o, int slot, const std::string& r) const override {
    int64_t off = 8 * (int64_t)slot;
    if (off >= -2048 && off <= 2047)
      o += "    addi " + r + ", sp, " + std::to_string(off) + "\n";
    else {
      o += "    li " + r + ", " + std::to_string(off) + "\n";
      o += "    add " + r + ", sp, " + r + "\n";
    }
  }
  void leaSym(std::string& o, const std::string& sym, const std::string& r) const override {
    o += "    lla " + r + ", " + sym + "\n";
  }
  void loadSym(std::string& o, const std::string& sym, const std::string& r) const override {
    o += "    lla t5, " + sym + "\n    ld " + r + ", 0(t5)\n";
  }
  void storeSym(std::string& o, const std::string& sym, const std::string& r) const override {
    o += "    lla t5, " + sym + "\n    sd " + r + ", 0(t5)\n";
  }
  void callSym(std::string& o, const std::string& sym) const override {
    // astra-riscv64's `call` pseudo mis-assembles; `jal ra` is correct.
    o += "    jal ra, " + sym + "\n";
  }
  void jmp(std::string& o, const std::string& l) const override {
    o += "    j " + l + "\n";
  }
  void jcc(std::string& o, const std::string& cc, const std::string& l) const override {
    // RV base has beq,bne,blt,bge,bltu,bgeu. Map gt/le via swap.
    if (cc == "eq") o += "    beq " + lastA + ", " + lastB + ", " + l + "\n";
    else if (cc == "ne") o += "    bne " + lastA + ", " + lastB + ", " + l + "\n";
    else if (cc == "lt") o += "    blt " + lastA + ", " + lastB + ", " + l + "\n";
    else if (cc == "ge") o += "    bge " + lastA + ", " + lastB + ", " + l + "\n";
    else if (cc == "gt") o += "    blt " + lastB + ", " + lastA + ", " + l + "\n";
    else if (cc == "le") o += "    bge " + lastB + ", " + lastA + ", " + l + "\n";
    else o += "    bne " + lastA + ", " + lastB + ", " + l + "\n";
  }
  void jz(std::string& o, const std::string& r, const std::string& l) const override {
    o += "    beq " + r + ", zero, " + l + "\n";
  }
  void jnz(std::string& o, const std::string& r, const std::string& l) const override {
    o += "    bne " + r + ", zero, " + l + "\n";
  }
  void cmpRegs(std::string& o, const std::string& a, const std::string& b) const override {
    (void)o;
    lastA = a;
    lastB = b;
  }
  void addImm(std::string& o, const std::string& r, int64_t v) const override {
    if (v >= -2048 && v <= 2047)
      o += "    addi " + r + ", " + r + ", " + std::to_string(v) + "\n";
    else {
      o += "    li t4, " + std::to_string(v) + "\n";
      o += "    add " + r + ", " + r + ", t4\n";
    }
  }
  void zeroReg(std::string& o, const std::string& r) const override {
    o += "    li " + r + ", 0\n";
  }
  void ret(std::string& o) const override { o += "    ret\n"; }
};

// ---------------------------------------------------------------- i386 ----
// i386 SysV: aval (u64) does not fit one register. All "registers" are
// memory slots in our frame; callSym relocates staged args into the
// outgoing area per the callee's C signature. Returns arrive in edx:eax.
//
// Frame layout (esp-relative, esp constant — no pushes):
//   +0    outgoing arg area (16 x 8B)
//   +128  ACC (8B)
//   +136  TMP0..TMP3 (4 x 8B)
//   +168  ARG staging G0..G7 (8 x 8B)
//   +232  emitter slots (8B each)
struct I386Target : NatTarget {
  static constexpr int kReserve = 232;
  static constexpr int kOut = 0, kAcc = 128, kTmp = 136, kArg = 168;
  int ptrSize() const override { return 4; }
  mutable std::string lastA, lastB;
  mutable int jcN_ = 0;
  mutable std::set<int> usedArgs_;

  static int offOf(const std::string& r) {
    if (r == "@a") return kAcc;
    if (r.size() >= 3 && r[0] == '@' && r[1] == 't')
      return kTmp + 8 * (r[2] - '0');
    if (r.size() >= 3 && r[0] == '@' && r[1] == 'g')
      return kArg + 8 * (r[2] - '0');
    return -1;
  }
  static std::string ref(int off) {
    return std::to_string(off) + "(%esp)";
  }
  static std::string refAdd(int off, int add) {
    return std::to_string(off + add) + "(%esp)";
  }
  int frameReserve() const override { return kReserve; }
  void prologue(std::string& o, int frameBytes) const override {
    // +8 keeps esp 16-aligned in the body (entry esp%16 == 8).
    int total = frameBytes + 8;
    o += "    subl $" + std::to_string(total) + ", %esp\n";
    // Incoming argv pointer (our single arg) sits above the frame:
    // retaddr at esp+total, arg0 at esp+total+4. Fetch into @g0.
    o += "    movl " + std::to_string(total + 4) + "(%esp), %eax\n";
    o += "    movl %eax, " + ref(kArg) + "\n";
    o += "    movl $0, " + refAdd(kArg, 4) + "\n";
  }
  void epilogue(std::string& o, int frameBytes) const override {
    int total = frameBytes + 8;
    // aval return value -> edx:eax (acc lives in @g0).
    o += "    movl " + ref(kArg) + ", %eax\n";
    o += "    movl " + refAdd(kArg, 4) + ", %edx\n";
    o += "    addl $" + std::to_string(total) + ", %esp\n";
    o += "    ret\n";
  }
  std::string slotRef(int i) const override {
    return std::to_string(kReserve + 8 * i) + "(%esp)";
  }
  // acc aliases arg0 staging: unary runtime calls (a_truthy_raw_slow etc.)
  // rely on acc == argReg(0), matching the 64-bit targets where acc is x0/a0.
  std::string acc() const override { return "@g0"; }
  int nArgRegs() const override { return 8; }
  std::string argReg(int i) const override {
    usedArgs_.insert(i);
    return "@g" + std::to_string(i);
  }
  std::string tmpReg(int i) const override { return "@t" + std::to_string(i); }
  void movImm(std::string& o, uint64_t v, const std::string& r) const override {
    int off = offOf(r);
    o += "    movl $" + std::to_string((uint32_t)(v & 0xFFFFFFFFu)) + ", " +
         ref(off) + "\n";
    o += "    movl $" + std::to_string((uint32_t)(v >> 32)) + ", " +
         refAdd(off, 4) + "\n";
  }
  void movReg(std::string& o, const std::string& d, const std::string& s) const override {
    int so = offOf(s), doff = offOf(d);
    o += "    movl " + ref(so) + ", %eax\n    movl %eax, " + ref(doff) + "\n";
    o += "    movl " + refAdd(so, 4) + ", %eax\n    movl %eax, " + refAdd(doff, 4) + "\n";
  }
  void loadSlot(std::string& o, int slot, const std::string& r) const override {
    int so = kReserve + 8 * slot, d = offOf(r);
    o += "    movl " + ref(so) + ", %eax\n    movl %eax, " + ref(d) + "\n";
    o += "    movl " + refAdd(so, 4) + ", %eax\n    movl %eax, " + refAdd(d, 4) + "\n";
  }
  void storeSlot(std::string& o, const std::string& r, int slot) const override {
    int so = offOf(r), d = kReserve + 8 * slot;
    o += "    movl " + ref(so) + ", %eax\n    movl %eax, " + ref(d) + "\n";
    o += "    movl " + refAdd(so, 4) + ", %eax\n    movl %eax, " + refAdd(d, 4) + "\n";
  }
  void loadSlotAt(std::string& o, const std::string& baseReg,
                  int64_t byteOff, const std::string& dstReg) const override {
    int bo = offOf(baseReg), d = offOf(dstReg);
    std::string disp = byteOff >= 0 ? std::to_string(byteOff)
                                    : "-" + std::to_string(-byteOff);
    o += "    movl " + ref(bo) + ", %eax\n";
    o += "    movl " + disp + "(%eax), %ecx\n    movl %ecx, " + ref(d) + "\n";
    o += "    movl " + disp + "+4(%eax), %ecx\n    movl %ecx, " + refAdd(d, 4) + "\n";
  }
  void copySlot(std::string& o, int from, int to) const override {
    int fo = kReserve + 8 * from, d = kReserve + 8 * to;
    o += "    movl " + ref(fo) + ", %eax\n    movl %eax, " + ref(d) + "\n";
    o += "    movl " + refAdd(fo, 4) + ", %eax\n    movl %eax, " + refAdd(d, 4) + "\n";
  }
  void leaSlot(std::string& o, int slot, const std::string& r) const override {
    int d = offOf(r);
    o += "    movl %esp, %eax\n    addl $" +
         std::to_string(kReserve + 8 * slot) + ", %eax\n";
    o += "    movl %eax, " + ref(d) + "\n    movl $0, " + refAdd(d, 4) + "\n";
  }
  void leaSym(std::string& o, const std::string& sym, const std::string& r) const override {
    int d = offOf(r);
    o += "    movl $" + sym + ", %eax\n    movl %eax, " + ref(d) + "\n";
    o += "    movl $0, " + refAdd(d, 4) + "\n";
  }
  void loadSym(std::string& o, const std::string& sym, const std::string& r) const override {
    int d = offOf(r);
    o += "    movl $" + sym + ", %eax\n";
    o += "    movl (%eax), %ecx\n    movl %ecx, " + ref(d) + "\n";
    o += "    movl 4(%eax), %ecx\n    movl %ecx, " + refAdd(d, 4) + "\n";
  }
  void storeSym(std::string& o, const std::string& sym, const std::string& r) const override {
    int d = offOf(r);
    o += "    movl $" + sym + ", %eax\n";
    o += "    movl " + ref(d) + ", %ecx\n    movl %ecx, (%eax)\n";
    o += "    movl " + refAdd(d, 4) + ", %ecx\n    movl %ecx, 4(%eax)\n";
  }
  // C arg sizes (bytes) per runtime symbol; unknown -> {4} (our fns take
  // a single aval* argv). Second flag: edx must be zeroed after the call
  // for callees returning a 32-bit int rather than a full aval.
  static const std::unordered_map<std::string, std::pair<std::vector<int>, bool>>& sigs() {
    static const std::unordered_map<std::string, std::pair<std::vector<int>, bool>> m = {
        {"a_add_slow",{{8,8},false}},{"a_sub_slow",{{8,8},false}},
        {"a_mul_slow",{{8,8},false}},{"a_div_slow",{{8,8},false}},
        {"a_mod_slow",{{8,8},false}},{"a_pow",{{8,8},false}},
        {"a_eq_slow",{{8,8},false}},{"a_ne_slow",{{8,8},false}},
        {"a_lt_slow",{{8,8},false}},{"a_le_slow",{{8,8},false}},
        {"a_gt_slow",{{8,8},false}},{"a_ge_slow",{{8,8},false}},
        {"a_band_slow",{{8,8},false}},{"a_bor_slow",{{8,8},false}},
        {"a_bxor_slow",{{8,8},false}},{"a_shl_slow",{{8,8},false}},
        {"a_shr_slow",{{8,8},false}},
        {"a_not",{{8},false}},{"a_neg_slow",{{8},false}},{"a_bnot_slow",{{8},false}},
        {"a_truthy_raw_slow",{{8},false}},
        {"a_bi_call",{{8,8,4},false}},{"a_call",{{8,8,4},false}},
        {"a_builtin",{{8},false}},
        {"a_str_new",{{4,8},false}},{"a_char_new",{{4,8},false}},
        {"a_str",{{8},false}},{"a_repr",{{8},false}},{"a_len",{{8},false}},
        {"a_str_concat",{{8,8},false}},{"a_str_joinsep",{{8,8,4},false}},
        {"a_list_new",{{8,4},false}},{"a_list_push",{{8,8},false}},
        {"a_tuple_new",{{8,4},false}},
        {"a_dict_new",{{},false}},{"a_dict_set",{{8,8,8},false}},
        {"a_dict_get",{{8,8,8},false}},
        {"a_set_new",{{},false}},{"a_set_add",{{8,8},false}},
        {"a_range_new",{{8,8,8},false}},{"a_range_contains",{{8,8},false}},
        {"a_getitem",{{8,8},false}},{"a_setitem",{{8,8,8},false}},
        {"a_slice",{{8,8,8,8},false}},{"a_contains",{{8,8},false}},
        {"a_in_range",{{8,8,8},false}},{"a_in_range_inclusive",{{8,8,8},false}},
        {"a_iter_new",{{8},false}},{"a_iter_next",{{8,4},false}},
        {"a_fnon_new",{{8,8,4},false}},
        {"a_env_set",{{8,8,8},false}},{"a_env_get",{{8,8},false}},
        {"a_closure_env_new",{{8},false}},
        {"a_method",{{8,4,8,8,4},false}},{"a_getattr",{{8,4},false}},
        {"a_setattr",{{8,4,8},false}},
        {"a_unwrap",{{8},false}},{"a_some_new",{{8},false}},
        {"a_try_result",{{8},false}},{"a_match_fail",{{8},false}},
        {"a_is_propagate",{{8},true}},{"a_obj_is_instance",{{8,4},true}},
        {"a_test_main",{{8,4,4},false}},
        {"a_runtime_init",{{},false}},
    };
    return m;
  }
  void callSym(std::string& o, const std::string& sym) const override {
    // Always stage arg0: acc aliases @g0 and unary runtime calls rely on it.
    int n = usedArgs_.empty() ? 1 : *usedArgs_.rbegin() + 1;
    auto it = sigs().find(sym);
    std::vector<int> sizes = it != sigs().end() ? it->second.first
                                              : std::vector<int>{4};
    bool ret32 = it != sigs().end() && it->second.second;
    int dst = kOut;
    for (int i = 0; i < n; ++i) {
      int sz = i < (int)sizes.size() ? sizes[i] : 4;
      int src = kArg + 8 * i;
      o += "    movl " + ref(src) + ", %eax\n    movl %eax, " + ref(dst) + "\n";
      if (sz == 8) {
        o += "    movl " + refAdd(src, 4) + ", %eax\n    movl %eax, " +
             refAdd(dst, 4) + "\n";
      }
      dst += sz;
    }
    usedArgs_.clear();
    o += "    call " + sym + "\n";
    if (ret32) o += "    movl $0, %edx\n";
    // Return value arrives in edx:eax; acc is the @g0 staging slot.
    o += "    movl %eax, " + ref(kArg) + "\n";
    o += "    movl %edx, " + refAdd(kArg, 4) + "\n";
  }
  void jmp(std::string& o, const std::string& l) const override {
    o += "    jmp " + l + "\n";
  }
  void jcc(std::string& o, const std::string& cc, const std::string& l) const override {
    int ao = offOf(lastA), bo = offOf(lastB);
    auto hi = [&]() {
      o += "    movl " + refAdd(ao, 4) + ", %eax\n    movl " +
           refAdd(bo, 4) + ", %ecx\n    cmpl %ecx, %eax\n";
    };
    auto lo = [&]() {
      o += "    movl " + ref(ao) + ", %eax\n    movl " + ref(bo) +
           ", %ecx\n    cmpl %ecx, %eax\n";
    };
    std::string sk = "__jc" + std::to_string(jcN_++);
    if (cc == "eq") {
      hi(); o += "    jne " + sk + "\n"; lo(); o += "    je " + l + "\n" + sk + ":\n";
    } else if (cc == "ne") {
      hi(); o += "    jne " + l + "\n"; lo(); o += "    jne " + l + "\n";
    } else if (cc == "lt") {
      hi(); o += "    jl " + l + "\n    jg " + sk + "\n"; lo();
      o += "    jb " + l + "\n" + sk + ":\n";
    } else if (cc == "le") {
      hi(); o += "    jl " + l + "\n    jg " + sk + "\n"; lo();
      o += "    jbe " + l + "\n" + sk + ":\n";
    } else if (cc == "gt") {
      hi(); o += "    jg " + l + "\n    jl " + sk + "\n"; lo();
      o += "    ja " + l + "\n" + sk + ":\n";
    } else {  // ge
      hi(); o += "    jg " + l + "\n    jl " + sk + "\n"; lo();
      o += "    jae " + l + "\n" + sk + ":\n";
    }
  }
  void jz(std::string& o, const std::string& r, const std::string& l) const override {
    int off = offOf(r);
    o += "    movl " + ref(off) + ", %eax\n    orl " + refAdd(off, 4) +
         ", %eax\n    jz " + l + "\n";
  }
  void jnz(std::string& o, const std::string& r, const std::string& l) const override {
    int off = offOf(r);
    o += "    movl " + ref(off) + ", %eax\n    orl " + refAdd(off, 4) +
         ", %eax\n    jnz " + l + "\n";
  }
  void cmpRegs(std::string& o, const std::string& a, const std::string& b) const override {
    (void)o;
    lastA = a;
    lastB = b;
  }
  void addImm(std::string& o, const std::string& r, int64_t v) const override {
    int off = offOf(r);
    o += "    addl $" + std::to_string((int32_t)v) + ", " + ref(off) + "\n";
    o += "    adcl $0, " + refAdd(off, 4) + "\n";
  }
  void zeroReg(std::string& o, const std::string& r) const override {
    int off = offOf(r);
    o += "    movl $0, " + ref(off) + "\n    movl $0, " + refAdd(off, 4) + "\n";
  }
  void ret(std::string& o) const override { o += "    ret\n"; }
};

// --------------------------------------------------------------- ARM32 ----
// ARMv7 AAPCS: same memory-backed pseudo-register model as i386, but call
// args marshal into r0-r3 (8-byte args take even reg pairs) then stack.
// aval returns arrive in r0:r1. Frame: push {fp,lr}; sub sp,#frame.
// Layout identical to i386: +0 outgoing stack args, +128 ACC(unused),
// +136 TMP0-3, +168 ARG staging G0-7, +232 emitter slots.
struct Arm32Target : NatTarget {
  static constexpr int kReserve = 232;
  static constexpr int kOut = 0, kTmp = 136, kArg = 168;
  int ptrSize() const override { return 4; }
  mutable std::string lastA, lastB;
  mutable int jcN_ = 0;
  mutable std::set<int> usedArgs_;

  static int offOf(const std::string& r) {
    if (r == "@a") return kArg;
    if (r.size() >= 3 && r[0] == '@' && r[1] == 't')
      return kTmp + 8 * (r[2] - '0');
    if (r.size() >= 3 && r[0] == '@' && r[1] == 'g')
      return kArg + 8 * (r[2] - '0');
    return -1;
  }
  static std::string ref(int off) { return "[sp, #" + std::to_string(off) + "]"; }
  static std::string refAdd(int off, int add) {
    return "[sp, #" + std::to_string(off + add) + "]";
  }
  static void imm32(std::string& o, uint32_t v, const char* r) {
    o += "    movw " + std::string(r) + ", #" + std::to_string(v & 0xFFFF) + "\n";
    o += "    movt " + std::string(r) + ", #" + std::to_string(v >> 16) + "\n";
  }
  static void ldr32(std::string& o, int off, const char* r) {
    o += "    ldr " + std::string(r) + ", " + ref(off) + "\n";
  }
  static void str32(std::string& o, const char* r, int off) {
    o += "    str " + std::string(r) + ", " + ref(off) + "\n";
  }
  int frameReserve() const override { return kReserve; }
  void prologue(std::string& o, int frameBytes) const override {
    o += "    push {fp, lr}\n";
    imm32(o, (uint32_t)frameBytes, "r12");
    o += "    sub sp, sp, r12\n";
    // Incoming argv pointer (our single arg) arrives in r0.
    str32(o, "r0", kArg);
    o += "    mov r12, #0\n";
    str32(o, "r12", kArg + 4);
  }
  void epilogue(std::string& o, int frameBytes) const override {
    // aval return value -> r0:r1 (acc lives in @g0).
    ldr32(o, kArg, "r0");
    ldr32(o, kArg + 4, "r1");
    imm32(o, (uint32_t)frameBytes, "r12");
    o += "    add sp, sp, r12\n";
    o += "    pop {fp, lr}\n";
    o += "    bx lr\n";
  }
  std::string slotRef(int i) const override {
    return "[sp, #" + std::to_string(kReserve + 8 * i) + "]";
  }
  std::string acc() const override { return "@g0"; }
  int nArgRegs() const override { return 4; }
  std::string argReg(int i) const override {
    usedArgs_.insert(i);
    return "@g" + std::to_string(i);
  }
  std::string tmpReg(int i) const override { return "@t" + std::to_string(i); }
  void movImm(std::string& o, uint64_t v, const std::string& r) const override {
    int off = offOf(r);
    imm32(o, (uint32_t)(v & 0xFFFFFFFFu), "r12");
    str32(o, "r12", off);
    imm32(o, (uint32_t)(v >> 32), "r12");
    str32(o, "r12", off + 4);
  }
  void movReg(std::string& o, const std::string& d, const std::string& s) const override {
    int so = offOf(s), dfo = offOf(d);
    ldr32(o, so, "r12"); str32(o, "r12", dfo);
    ldr32(o, so + 4, "r12"); str32(o, "r12", dfo + 4);
  }
  void loadSlot(std::string& o, int slot, const std::string& r) const override {
    int so = kReserve + 8 * slot, d = offOf(r);
    ldr32(o, so, "r12"); str32(o, "r12", d);
    ldr32(o, so + 4, "r12"); str32(o, "r12", d + 4);
  }
  void storeSlot(std::string& o, const std::string& r, int slot) const override {
    int so = offOf(r), d = kReserve + 8 * slot;
    ldr32(o, so, "r12"); str32(o, "r12", d);
    ldr32(o, so + 4, "r12"); str32(o, "r12", d + 4);
  }
  void loadSlotAt(std::string& o, const std::string& baseReg,
                  int64_t byteOff, const std::string& dstReg) const override {
    int bo = offOf(baseReg), d = offOf(dstReg);
    ldr32(o, bo, "r12");
    std::string br = "r12";
    if (byteOff == 0) {
      o += "    ldr r3, [" + br + "]\n    str r3, " + ref(d) + "\n";
      o += "    ldr r3, [" + br + ", #4]\n    str r3, " + refAdd(d, 4) + "\n";
    } else {
      std::string d1 = byteOff >= 0 ? "#" + std::to_string(byteOff)
                                    : "-#" + std::to_string(-byteOff);
      o += "    ldr r3, [" + br + ", " + d1 + "]\n    str r3, " + ref(d) + "\n";
      int64_t o2 = byteOff + 4;
      std::string d2 = o2 >= 0 ? "#" + std::to_string(o2) : "-#" + std::to_string(-o2);
      o += "    ldr r3, [" + br + ", " + d2 + "]\n    str r3, " + refAdd(d, 4) + "\n";
    }
  }
  void copySlot(std::string& o, int from, int to) const override {
    int fo = kReserve + 8 * from, d = kReserve + 8 * to;
    ldr32(o, fo, "r12"); str32(o, "r12", d);
    ldr32(o, fo + 4, "r12"); str32(o, "r12", d + 4);
  }
  void leaSlot(std::string& o, int slot, const std::string& r) const override {
    int d = offOf(r);
    imm32(o, (uint32_t)(kReserve + 8 * slot), "r12");
    o += "    add r12, sp, r12\n";
    str32(o, "r12", d);
    o += "    mov r12, #0\n";
    str32(o, "r12", d + 4);
  }
  void leaSym(std::string& o, const std::string& sym, const std::string& r) const override {
    int d = offOf(r);
    o += "    movw r12, " + sym + "\n    movt r12, " + sym + "\n";
    str32(o, "r12", d);
    o += "    mov r12, #0\n";
    str32(o, "r12", d + 4);
  }
  void loadSym(std::string& o, const std::string& sym, const std::string& r) const override {
    int d = offOf(r);
    // Keep the symbol address in r12; load both words via r3.
    o += "    movw r12, " + sym + "\n    movt r12, " + sym + "\n";
    o += "    ldr r3, [r12]\n";
    str32(o, "r3", d);
    o += "    ldr r3, [r12, #4]\n";
    str32(o, "r3", d + 4);
  }
  void storeSym(std::string& o, const std::string& sym, const std::string& r) const override {
    int d = offOf(r);
    o += "    movw r12, " + sym + "\n    movt r12, " + sym + "\n";
    ldr32(o, d, "r3");
    o += "    str r3, [r12]\n";
    ldr32(o, d + 4, "r3");
    o += "    str r3, [r12, #4]\n";
  }
  static const std::unordered_map<std::string, std::pair<std::vector<int>, bool>>& sigs() {
    static const std::unordered_map<std::string, std::pair<std::vector<int>, bool>> m = {
        {"a_add_slow",{{8,8},false}},{"a_sub_slow",{{8,8},false}},
        {"a_mul_slow",{{8,8},false}},{"a_div_slow",{{8,8},false}},
        {"a_mod_slow",{{8,8},false}},{"a_pow",{{8,8},false}},
        {"a_eq_slow",{{8,8},false}},{"a_ne_slow",{{8,8},false}},
        {"a_lt_slow",{{8,8},false}},{"a_le_slow",{{8,8},false}},
        {"a_gt_slow",{{8,8},false}},{"a_ge_slow",{{8,8},false}},
        {"a_band_slow",{{8,8},false}},{"a_bor_slow",{{8,8},false}},
        {"a_bxor_slow",{{8,8},false}},{"a_shl_slow",{{8,8},false}},
        {"a_shr_slow",{{8,8},false}},
        {"a_not",{{8},false}},{"a_neg_slow",{{8},false}},{"a_bnot_slow",{{8},false}},
        {"a_truthy_raw_slow",{{8},false}},
        {"a_bi_call",{{8,8,4},false}},{"a_call",{{8,8,4},false}},
        {"a_builtin",{{8},false}},
        {"a_str_new",{{4,8},false}},{"a_char_new",{{4,8},false}},
        {"a_str",{{8},false}},{"a_repr",{{8},false}},{"a_len",{{8},false}},
        {"a_str_concat",{{8,8},false}},{"a_str_joinsep",{{8,8,4},false}},
        {"a_list_new",{{8,4},false}},{"a_list_push",{{8,8},false}},
        {"a_tuple_new",{{8,4},false}},
        {"a_dict_new",{{},false}},{"a_dict_set",{{8,8,8},false}},
        {"a_dict_get",{{8,8,8},false}},
        {"a_set_new",{{},false}},{"a_set_add",{{8,8},false}},
        {"a_range_new",{{8,8,8},false}},{"a_range_contains",{{8,8},false}},
        {"a_getitem",{{8,8},false}},{"a_setitem",{{8,8,8},false}},
        {"a_slice",{{8,8,8,8},false}},{"a_contains",{{8,8},false}},
        {"a_in_range",{{8,8,8},false}},{"a_in_range_inclusive",{{8,8,8},false}},
        {"a_iter_new",{{8},false}},{"a_iter_next",{{8,4},false}},
        {"a_fnon_new",{{8,8,4},false}},
        {"a_env_set",{{8,8,8},false}},{"a_env_get",{{8,8},false}},
        {"a_closure_env_new",{{8},false}},
        {"a_method",{{8,4,8,8,4},false}},{"a_getattr",{{8,4},false}},
        {"a_setattr",{{8,4,8},false}},
        {"a_unwrap",{{8},false}},{"a_some_new",{{8},false}},
        {"a_try_result",{{8},false}},{"a_match_fail",{{8},false}},
        {"a_is_propagate",{{8},true}},{"a_obj_is_instance",{{8,4},true}},
        {"a_test_main",{{8,4,4},false}},
        {"a_runtime_init",{{},false}},
    };
    return m;
  }
  void callSym(std::string& o, const std::string& sym) const override {
    int n = usedArgs_.empty() ? 1 : *usedArgs_.rbegin() + 1;
    auto it = sigs().find(sym);
    std::vector<int> sizes = it != sigs().end() ? it->second.first
                                              : std::vector<int>{4};
    bool ret32 = it != sigs().end() && it->second.second;
    // AAPCS: r0-r3 regs (8B args need even pairs), rest on stack at sp+0.
    int nextReg = 0, stackOff = 0;
    bool onStack = false;
    for (int i = 0; i < n; ++i) {
      int sz = i < (int)sizes.size() ? sizes[i] : 4;
      int src = kArg + 8 * i;
      int reg = -1;
      if (!onStack) {
        if (sz == 8) {
          if (nextReg & 1) nextReg++;
          if (nextReg <= 2) { reg = nextReg; nextReg += 2; }
          else onStack = true;
        } else {
          if (nextReg <= 3) { reg = nextReg; nextReg += 1; }
          else onStack = true;
        }
      }
      if (reg >= 0) {
        o += "    ldr r" + std::to_string(reg) + ", " + ref(src) + "\n";
        if (sz == 8)
          o += "    ldr r" + std::to_string(reg + 1) + ", " + refAdd(src, 4) + "\n";
      } else {
        if (sz == 8 && (stackOff & 7)) stackOff += 4;  // 8B stack args align
        int dst = stackOff;
        // r12 is the scratch — r0-r3 may already hold live arg values.
        ldr32(o, src, "r12");
        o += "    str r12, " + ref(dst) + "\n";
        if (sz == 8) {
          ldr32(o, src + 4, "r12");
          o += "    str r12, " + refAdd(dst, 4) + "\n";
        }
        stackOff += sz;
      }
    }
    usedArgs_.clear();
    o += "    bl " + sym + "\n";
    // aval return -> r0:r1 stored into @g0; ret32 callees: zero the high word.
    str32(o, "r0", kArg);
    if (ret32) {
      o += "    mov r3, #0\n";
      str32(o, "r3", kArg + 4);
    } else {
      str32(o, "r1", kArg + 4);
    }
  }
  void jmp(std::string& o, const std::string& l) const override {
    o += "    b " + l + "\n";
  }
  void jcc(std::string& o, const std::string& cc, const std::string& l) const override {
    int ao = offOf(lastA), bo = offOf(lastB);
    auto hi = [&]() {
      ldr32(o, ao + 4, "r0"); ldr32(o, bo + 4, "r1");
      o += "    cmp r0, r1\n";
    };
    auto lo = [&]() {
      ldr32(o, ao, "r0"); ldr32(o, bo, "r1");
      o += "    cmp r0, r1\n";
    };
    std::string sk = "__jc" + std::to_string(jcN_++);
    // signed hi conds; unsigned lo conds (ARM: lo/ls/hi/hs).
    if (cc == "eq") {
      hi(); o += "    bne " + sk + "\n"; lo(); o += "    beq " + l + "\n" + sk + ":\n";
    } else if (cc == "ne") {
      hi(); o += "    bne " + l + "\n"; lo(); o += "    bne " + l + "\n";
    } else if (cc == "lt") {
      hi(); o += "    blt " + l + "\n    bgt " + sk + "\n"; lo();
      o += "    blo " + l + "\n" + sk + ":\n";
    } else if (cc == "le") {
      hi(); o += "    blt " + l + "\n    bgt " + sk + "\n"; lo();
      o += "    bls " + l + "\n" + sk + ":\n";
    } else if (cc == "gt") {
      hi(); o += "    bgt " + l + "\n    blt " + sk + "\n"; lo();
      o += "    bhi " + l + "\n" + sk + ":\n";
    } else {  // ge
      hi(); o += "    bgt " + l + "\n    blt " + sk + "\n"; lo();
      o += "    bhs " + l + "\n" + sk + ":\n";
    }
  }
  void jz(std::string& o, const std::string& r, const std::string& l) const override {
    int off = offOf(r);
    std::string sk = "__jc" + std::to_string(jcN_++);
    ldr32(o, off, "r0");
    o += "    cmp r0, #0\n    bne " + sk + "\n";
    ldr32(o, off + 4, "r1");
    o += "    cmp r1, #0\n    beq " + l + "\n" + sk + ":\n";
  }
  void jnz(std::string& o, const std::string& r, const std::string& l) const override {
    int off = offOf(r);
    ldr32(o, off, "r0");
    o += "    cmp r0, #0\n    bne " + l + "\n";
    ldr32(o, off + 4, "r1");
    o += "    cmp r1, #0\n    bne " + l + "\n";
  }
  void cmpRegs(std::string& o, const std::string& a, const std::string& b) const override {
    (void)o;
    lastA = a;
    lastB = b;
  }
  void addImm(std::string& o, const std::string& r, int64_t v) const override {
    int off = offOf(r);
    imm32(o, (uint32_t)(v & 0xFFFFFFFFu), "r3");
    ldr32(o, off, "r12");
    o += "    adds r12, r12, r3\n";
    str32(o, "r12", off);
    imm32(o, (uint32_t)((uint64_t)v >> 32), "r3");
    ldr32(o, off + 4, "r12");
    o += "    adc r12, r12, r3\n";
    str32(o, "r12", off + 4);
  }
  void zeroReg(std::string& o, const std::string& r) const override {
    int off = offOf(r);
    o += "    mov r12, #0\n";
    str32(o, "r12", off);
    str32(o, "r12", off + 4);
  }
  void ret(std::string& o) const override { o += "    bx lr\n"; }
};

// i8086 real mode: identical instruction dialect to i386 — the backend
// assembler emits 0x66/0x67 prefixes automatically. Only the entry stub
// differs (no crt: _start sets up segments/stack and calls main).
struct I8086Target : I386Target {
  bool bareMetal() const override { return true; }
  // Real mode uses SS=0x9000 while DS=0. Slot *contents* are accessed via
  // SS:ESP, but pointers handed to runtime/code must be flat physical
  // addresses (DS=0), so add the stack segment bias when taking a slot
  // address. ESP stays a 16-bit offset (<64K) for push/pop compatibility.
  void leaSlot(std::string& o, int slot, const std::string& r) const override {
    int d = I386Target::offOf(r);
    uint32_t bias = 0x90000u + (uint32_t)I386Target::kReserve +
                    (uint32_t)(8 * slot);
    o += "    movl %esp, %eax\n    addl $" + std::to_string(bias) +
         ", %eax\n";
    o += "    movl %eax, " + I386Target::ref(d) + "\n    movl $0, " +
         I386Target::refAdd(d, 4) + "\n";
  }
};

// ------------------------------------------------------------- Win64 -----
// Windows x86-64 (MS ABI): retval/acc = rax, integer arg regs
// rcx,rdx,r8,r9; args 5+ go on the stack at [rsp+0x20+8*(i-4)]. Every call
// needs a 32-byte shadow space, so the frame reserves kOut bytes at
// [rsp+0): [0,0x20) shadow, [0x20,kOut) outgoing stack args, then the
// emitter's 8-byte slots. Only volatile regs are used (rax,rcx,rdx,r8,r9,
// r10,r11) so prologue/epilogue just adjust rsp; rbp is never touched on
// the small-frame path.
//
// Stack probing: frames whose total allocation exceeds 4KiB must go
// through __chkstk (win32/lib/chkstk.S inside libastra1.a). That helper
// is passed the slot-region size in eax and performs the subtraction
// itself: it leaves rsp at entry-8-eax — exactly where
// `sub rsp, frameBytes+8` would put it — but also leaves rbp holding a
// frame pointer, with the caller's rbp saved at [rsp+frameBytes]; we
// restore it explicitly. chkstk clobbers rax/rcx (both volatile).
//
// Known gaps: no .pdata/.xdata unwind info is emitted (the astra C
// emitter generates it via pe_add_unwind_data; asm has no equivalent),
// so SEH cannot unwind through these frames — fine until an exception
// or stack walk crosses them. No xmm/float regs are needed: avals are
// integer words and all semantics go through a_* runtime calls.
struct Win64Target : NatTarget {
  static constexpr int kShadow = 0x20;
  static constexpr int kStackArgs = 4;
  static constexpr int kOut = kShadow + 8 * kStackArgs;  // 0x40

  int frameReserve() const override { return kOut; }
  int nArgRegs() const override { return 4 + kStackArgs; }
  std::string acc() const override { return "rax"; }
  std::string argReg(int i) const override {
    static const char* k[] = {"rcx", "rdx", "r8", "r9"};
    if (i >= 0 && i < 4) return k[i];
    // "@sN" pseudo-register: outgoing stack-arg slot N (arg index 4+N).
    return "@s" + std::to_string(i - 4);
  }
  std::string tmpReg(int i) const override {
    return i == 0 ? "r10" : "r11";
  }

  // "@sN" -> stack-arg slot index, else -1.
  static int stackArg(const std::string& r) {
    if (r.size() >= 3 && r[0] == '@' && r[1] == 's' &&
        r[2] >= '0' && r[2] <= '3' && r.size() == 3)
      return r[2] - '0';
    return -1;
  }
  static std::string sref(int i) {
    return std::to_string(kShadow + 8 * i) + "(%rsp)";
  }
  static std::string slotMem(int i) {
    return std::to_string(kOut + 8 * (int64_t)i) + "(%rsp)";
  }
  static std::string pct(const std::string& r) { return "%" + r; }

  void prologue(std::string& o, int frameBytes) const override {
    int total = frameBytes + 8;  // entry rsp%16==8 -> body rsp%16==0
    char buf[96];
    if (total >= 4096) {
      std::snprintf(buf, sizeof buf,
                    "    mov $0x%x, %%eax\n    call __chkstk\n"
                    "    mov %d(%%rsp), %%rbp\n",
                    frameBytes, frameBytes);
      o += buf;
    } else {
      std::snprintf(buf, sizeof buf, "    sub $0x%x, %%rsp\n", total);
      o += buf;
    }
  }
  void epilogue(std::string& o, int frameBytes) const override {
    char buf[64];
    std::snprintf(buf, sizeof buf, "    add $0x%x, %%rsp\n    ret\n",
                  frameBytes + 8);
    o += buf;
  }
  std::string slotRef(int i) const override { return slotMem(i); }
  void movImm(std::string& o, uint64_t v, const std::string& r) const override {
    char buf[80];
    int sa = stackArg(r);
    if (sa >= 0) {
      std::snprintf(buf, sizeof buf, "    mov $0x%llx, %%r11\n    mov %%r11, %s\n",
                    (unsigned long long)v, sref(sa).c_str());
    } else {
      std::snprintf(buf, sizeof buf, "    mov $0x%llx, %%%s\n",
                    (unsigned long long)v, r.c_str());
    }
    o += buf;
  }
  void movReg(std::string& o, const std::string& d,
              const std::string& s) const override {
    if (d == s) return;
    int dd = stackArg(d), ds = stackArg(s);
    if (dd >= 0) {
      if (ds >= 0)
        o += "    mov " + sref(ds) + ", %r11\n";
      else
        o += "    mov " + pct(s) + ", %r11\n";
      o += "    mov %r11, " + sref(dd) + "\n";
    } else if (ds >= 0) {
      o += "    mov " + sref(ds) + ", " + pct(d) + "\n";
    } else {
      o += "    mov " + pct(s) + ", " + pct(d) + "\n";
    }
  }
  void loadSlot(std::string& o, int slot, const std::string& r) const override {
    int sa = stackArg(r);
    if (sa >= 0) {
      o += "    mov " + slotMem(slot) + ", %r11\n    mov %r11, " +
           sref(sa) + "\n";
    } else {
      o += "    mov " + slotMem(slot) + ", " + pct(r) + "\n";
    }
  }
  void storeSlot(std::string& o, const std::string& r, int slot) const override {
    int sa = stackArg(r);
    if (sa >= 0) {
      o += "    mov " + sref(sa) + ", %r11\n    mov %r11, " +
           slotMem(slot) + "\n";
    } else {
      o += "    mov " + pct(r) + ", " + slotMem(slot) + "\n";
    }
  }
  void loadSlotAt(std::string& o, const std::string& baseReg,
                  int64_t byteOff, const std::string& dstReg) const override {
    int sa = stackArg(dstReg);
    if (sa >= 0) {
      o += "    mov " + std::to_string(byteOff) + "(%" + baseReg +
           "), %r11\n    mov %r11, " + sref(sa) + "\n";
    } else {
      o += "    mov " + std::to_string(byteOff) + "(%" + baseReg + "), " +
           pct(dstReg) + "\n";
    }
  }
  void copySlot(std::string& o, int from, int to) const override {
    o += "    mov " + slotMem(from) + ", %r11\n    mov %r11, " +
         slotMem(to) + "\n";
  }
  void leaSlot(std::string& o, int slot, const std::string& r) const override {
    int sa = stackArg(r);
    if (sa >= 0) {
      o += "    lea " + slotMem(slot) + ", %r11\n    mov %r11, " +
           sref(sa) + "\n";
    } else {
      o += "    lea " + slotMem(slot) + ", " + pct(r) + "\n";
    }
  }
  void leaSym(std::string& o, const std::string& sym,
              const std::string& r) const override {
    int sa = stackArg(r);
    if (sa >= 0) {
      o += "    lea " + sym + "(%rip), %%r11\n    mov %%r11, " +
           sref(sa) + "\n";
    } else {
      o += "    lea " + sym + "(%rip), " + pct(r) + "\n";
    }
  }
  void loadSym(std::string& o, const std::string& sym,
               const std::string& r) const override {
    int sa = stackArg(r);
    if (sa >= 0) {
      o += "    mov " + sym + "(%rip), %%r11\n    mov %%r11, " +
           sref(sa) + "\n";
    } else {
      o += "    mov " + sym + "(%rip), " + pct(r) + "\n";
    }
  }
  void storeSym(std::string& o, const std::string& sym,
                const std::string& r) const override {
    int sa = stackArg(r);
    if (sa >= 0) {
      o += "    mov " + sref(sa) + ", %r11\n    mov %r11, " + sym +
           "(%rip)\n";
    } else {
      o += "    mov " + pct(r) + ", " + sym + "(%rip)\n";
    }
  }
  void callSym(std::string& o, const std::string& sym) const override {
    o += "    call " + sym + "\n";
  }
  void jmp(std::string& o, const std::string& l) const override {
    o += "    jmp " + l + "\n";
  }
  void jcc(std::string& o, const std::string& cc,
           const std::string& l) const override {
    static const std::unordered_map<std::string, std::string> m = {
        {"eq", "je"}, {"ne", "jne"}, {"lt", "jl"}, {"le", "jle"},
        {"gt", "jg"}, {"ge", "jge"}};
    o += "    " + m.at(cc) + " " + l + "\n";
  }
  void jz(std::string& o, const std::string& r,
          const std::string& l) const override {
    int sa = stackArg(r);
    if (sa >= 0) {
      o += "    mov " + sref(sa) + ", %r11\n    test %r11, %r11\n";
    } else {
      o += "    test " + pct(r) + ", " + pct(r) + "\n";
    }
    o += "    jz " + l + "\n";
  }
  void jnz(std::string& o, const std::string& r,
           const std::string& l) const override {
    int sa = stackArg(r);
    if (sa >= 0) {
      o += "    mov " + sref(sa) + ", %r11\n    test %r11, %r11\n";
    } else {
      o += "    test " + pct(r) + ", " + pct(r) + "\n";
    }
    o += "    jnz " + l + "\n";
  }
  // cmpRegs(a,b) emits `cmp %b, %a` (computes a-b); jcc maps signed cc.
  // Stack-arg operands are rare (never used by the emitter today); they
  // are materialized through r10/r11 scratch.
  void cmpRegs(std::string& o, const std::string& a,
               const std::string& b) const override {
    int sa = stackArg(a), sb = stackArg(b);
    if (sa >= 0 && sb >= 0) {
      o += "    mov " + sref(sa) + ", %r10\n    mov " + sref(sb) +
           ", %r11\n    cmp %r11, %r10\n";
    } else if (sa >= 0) {
      o += "    mov " + sref(sa) + ", %r10\n    cmp " + pct(b) + ", %r10\n";
    } else if (sb >= 0) {
      o += "    mov " + sref(sb) + ", %r11\n    cmp %r11, " + pct(a) + "\n";
    } else {
      o += "    cmp " + pct(b) + ", " + pct(a) + "\n";
    }
  }
  void addImm(std::string& o, const std::string& r, int64_t v) const override {
    int sa = stackArg(r);
    if (sa >= 0) {
      o += "    mov " + sref(sa) + ", %r11\n    add $" +
           std::to_string(v) + ", %r11\n    mov %r11, " + sref(sa) + "\n";
    } else {
      o += "    add $" + std::to_string(v) + ", " + pct(r) + "\n";
    }
  }
  void zeroReg(std::string& o, const std::string& r) const override {
    int sa = stackArg(r);
    if (sa >= 0) {
      o += "    xor %r11, %r11\n    mov %r11, " + sref(sa) + "\n";
    } else {
      o += "    xor " + pct(r) + ", " + pct(r) + "\n";
    }
  }
  void ret(std::string& o) const override { o += "    ret\n"; }
};

// Dead-function elimination for bare-metal targets: real-mode CS:IP is
// 16-bit so .text must stay under 64KB, and the boot sector only loads
// 512 bytes. The emitter produces every function in the module graph;
// this pass keeps only functions transitively referenced (call/lea/data)
// from non-function code plus `main`/`_start`.
//
// Assembly layout contract (from NativeEmitter): global symbols appear
// as column-0 `name:` labels; each function's `.section`/`.globl`
// directives immediately precede its label; local labels are indented.
void gcAsmFunctions(std::string& src) {
  std::vector<std::string> lines;
  {
    std::istringstream in(src);
    std::string l;
    while (std::getline(in, l)) lines.push_back(l);
  }
  // Column-0 label classification. NativeEmitter emits:
  //  - function bodies as `__na_f<N>`/`__na_m<N>`/`__na_lam<N>`/
  //    `__na_ctor<N>`/`__na_top_<N>` (droppable when unreferenced),
  //  - data/metadata as `__na_d<N>`/`__na_gf<N>`/`__na_gs<N>` and
  //    `__na_fld_*`/`__na_cs_*`/`__na_mn_*`/`__na_ev_*`/`__na_sn_*`/
  //    `__na_tn_*`/`__na_tnames`/`__na_tfns`/`__na_lnm_*`/`__na_snv_*`/
  //    `__na_fs_*` (always kept),
  //  - internal control-flow labels `__na_<word><N>` (`__na_ret`,
  //    `__na_ifnext`, `__na_while`, `__na_for`, `__na_fend`, `__na_fcont`,
  //    `__na_sc`, ...) at column 0 INSIDE function bodies — these are not
  //    block boundaries.
  const std::regex colLabel(R"(^([A-Za-z_][A-Za-z0-9_]*):$)");
  const std::regex fnName(R"(^__na_(?:f|m|lam|ctor|top)_?[0-9]+$)");
  const std::regex shortData(R"(^__na_(?:gf|gs|d)[0-9]+$)");
  const std::regex internalName(R"(^__na_[a-z]+[0-9]+$)");
  // Indented header directives that belong to the block whose label
  // follows them (.section/.globl/.text/.align/.org — never .byte/.long/
  // .space, which are the block's payload).
  const std::regex hdrDir(R"(^\s*\.(?:section|globl|text|align|p2align|org)\b)");
  const std::regex globlDir(R"(^\s*\.globl\b)");
  const std::regex symTok(R"(__na_[A-Za-z0-9_]+)");
  const std::regex gfNum(R"(^__na_gf([0-9]+)$)");
  const std::regex gsNum(R"(^__na_gs([0-9]+)$)");

  auto isHdr = [&](const std::string& l) {
    return !l.empty() && l[0] == ' ' &&
           std::regex_search(l, hdrDir);
  };

  // Boundary labels = column-0 labels that are not internal control-flow
  // labels. Internal labels stay inside the enclosing function's block.
  std::vector<std::size_t> bounds;
  std::vector<std::string> boundNames;
  std::vector<char> boundIsFn;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    std::smatch m;
    if (!std::regex_match(lines[i], m, colLabel)) continue;
    const std::string& name = m[1].str();
    if (std::regex_match(name, internalName) &&
        !std::regex_match(name, fnName) &&
        !std::regex_match(name, shortData))
      continue;  // internal label: part of the enclosing function
    bounds.push_back(i);
    boundNames.push_back(name);
    boundIsFn.push_back(std::regex_match(name, fnName));
  }
  if (bounds.empty()) return;

  // Block = [header start, next block's header start). Header start walks
  // back over the indented directive lines preceding the label.
  struct Block { std::size_t begin, end; std::string name; bool isFn; };
  std::vector<Block> blocks;
  std::vector<std::size_t> hdr;
  hdr.reserve(bounds.size());
  for (std::size_t k = 0; k < bounds.size(); ++k) {
    std::size_t h = bounds[k];
    while (h > 0 && isHdr(lines[h - 1])) --h;
    hdr.push_back(h);
  }
  for (std::size_t k = 0; k < bounds.size(); ++k) {
    std::size_t e = (k + 1 < bounds.size()) ? hdr[k + 1] : lines.size();
    blocks.push_back({hdr[k], e, boundNames[k], boundIsFn[k] != 0});
  }

  std::unordered_map<std::string, std::size_t> byName;
  for (std::size_t i = 0; i < blocks.size(); ++i)
    if (blocks[i].isFn) byName[blocks[i].name] = i;

  // Roots: every non-function block (boot/_start/_entry/main/data/metadata)
  // is kept. Function blocks start unkept and are added by the fixpoint.
  std::vector<char> kept(blocks.size(), 0);
  for (std::size_t i = 0; i < blocks.size(); ++i)
    kept[i] = !blocks[i].isFn;

  // A symbol token keeps a function alive when it is a real operand
  // (call/jmp/mov/lea/.long ...). `.globl`/`.section` directive lines are
  // skipped: `.globl __na_gfN` must not resurrect the function behind the
  // global slot. `__na_gfK` slots pair with the FnInfo whose cname number
  // is K-1 (f or m — they share the fnId counter); `__na_gsK` pairs with
  // `__na_ctorK`.
  bool grew = false;
  auto markIdx = [&](std::size_t bi) {
    if (!kept[bi]) {
      kept[bi] = 1;
      grew = true;
    }
  };
  auto markSym = [&](const std::string& sym) {
    if (std::regex_match(sym, fnName)) {
      auto f = byName.find(sym);
      if (f != byName.end()) markIdx(f->second);
      return;
    }
    std::smatch gm;
    if (std::regex_match(sym, gm, gfNum)) {
      long k = std::stol(gm[1].str());
      if (k >= 1) {
        for (const char* p : {"__na_f", "__na_m"}) {
          auto f = byName.find(std::string(p) + std::to_string(k - 1));
          if (f != byName.end()) markIdx(f->second);
        }
      }
      return;
    }
    if (std::regex_match(sym, gm, gsNum)) {
      auto f = byName.find("__na_ctor" + gm[1].str());
      if (f != byName.end()) markIdx(f->second);
    }
  };

  do {
    grew = false;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
      if (!kept[i]) continue;
      for (std::size_t j = blocks[i].begin; j < blocks[i].end; ++j) {
        const std::string& l = lines[j];
        if (l.find("__na_") == std::string::npos) continue;
        if (std::regex_search(l, globlDir)) continue;
        // Label definitions (`name:` alone on a line) are not references:
        // `__na_gf1:` must not keep `__na_f0` alive via the gf->f mapping.
        if (std::regex_match(l, colLabel)) continue;
        std::sregex_iterator it(l.begin(), l.end(), symTok), end;
        for (; it != end; ++it) markSym((*it).str());
      }
    }
  } while (grew);

  // Rebuild: keep all lines except dropped function blocks. Lines before
  // the first block and non-block gaps are always kept.
  std::vector<char> dropLine(lines.size(), 0);
  for (std::size_t i = 0; i < blocks.size(); ++i)
    if (blocks[i].isFn && !kept[i])
      for (std::size_t j = blocks[i].begin; j < blocks[i].end; ++j)
        dropLine[j] = 1;
  std::string out;
  out.reserve(src.size());
  for (std::size_t i = 0; i < lines.size(); ++i)
    if (!dropLine[i]) { out += lines[i]; out += '\n'; }
  src.swap(out);
}
}  // namespace

bool emitNative(const ModuleGraph& graph, const SemaResult& sema,
                const std::string& arch, const std::string& os,
                std::string& out, std::string& error, bool testMode,
                const std::string* testFilter) {
  if (const char* forced = std::getenv("A_CODEGEN");
      forced && std::string(forced) == "c") {
    error = "native backend: disabled by A_CODEGEN=c";
    return false;
  }
  // Windows x86-64 uses the MS ABI (Win64Target); every other OS/arch
  // combination keeps its existing target (or declines for emitC).
  if (arch == "x86_64" && os == "windows") {
    Win64Target t;
    NativeEmitter e(graph, sema, t, testMode, testFilter);
    return e.run(out, error);
  }
  // The remaining NatTargets emit SysV-style assembly only; on Windows
  // they would produce the wrong ABI, so decline to emitC instead.
  if (os == "windows") {
    error = "native backend: no native target for '" + arch + "' on windows";
    return false;
  }
  if (arch == "arm64" || arch == "aarch64") {
    Arm64Target t;
    NativeEmitter e(graph, sema, t, testMode, testFilter);
    return e.run(out, error);
  }
  if (arch == "riscv64" || arch == "riscv64gc") {
    Riscv64Target t;
    NativeEmitter e(graph, sema, t, testMode, testFilter);
    return e.run(out, error);
  }
  if (arch == "i386" || arch == "x86") {
    I386Target t;
    NativeEmitter e(graph, sema, t, testMode, testFilter);
    return e.run(out, error);
  }
  if (arch == "arm" || arch == "arm32") {
    Arm32Target t;
    NativeEmitter e(graph, sema, t, testMode, testFilter);
    return e.run(out, error);
  }
  if (arch == "i8086" || arch == "8086") {
    if (testMode) {
      error = "native backend: i8086 test mode unsupported";
      return false;
    }
    I8086Target t;
    NativeEmitter e(graph, sema, t, testMode, testFilter);
    if (!e.run(out, error)) return false;
    // Bare-metal boot: BIOS loads sector 0 at 0x7C00. .boot contains an
    // int 13h CHS loader (AH=08h geometry + AH=02h reads) that pulls the
    // rest of the image to 0x7E00+, then far-jumps to _entry at 0x10000
    // (CS=0x1000, IP=0). .boot is padded to 0x8400 so .text starts on a
    // 64KB boundary and stays inside one real-mode code segment. Stack:
    // SS=0x9000, ESP=0xF000 (push/pop use SP, memory operands use ESP;
    // both are <64K so they agree).
    out = std::string(
        "    .section .boot,\"ax\"\n"
        "    .globl _start\n"
        "_start:\n"
        "    cli\n"
        "    movb %dl, 0x7db4\n"
        "    xorl %eax, %eax\n"
        "    movl %eax, %ds\n"
        "    movl %eax, %es\n"
        "    movb 0x7db4, %dl\n"
        "    movb $0x08, %ah\n"
        "    int $0x13\n"
        "    jc __boot_fail\n"
        "    movb %dh, 0x7db8\n"
        "    movb %cl, %al\n"
        "    andb $0x3f, %al\n"
        "    movb %al, 0x7db6\n"
        "    movb $0, 0x7db9\n"
        "    movw $2, 0x7da4\n"
        "    movw $0, 0x7da6\n"
        "    movw $0, 0x7da8\n"
        "    movl $0x7e00, 0x7dac\n"
        "    movl 0x7df8, %eax\n"
        "    decl %eax\n"
        "    movl %eax, 0x7da0\n"
        "__boot_load:\n"
        "    movl 0x7da0, %eax\n"
        "    testl %eax, %eax\n"
        "    jz __boot_go\n"
        "    movb 0x7da4, %cl\n"
        "    movb 0x7da6, %dh\n"
        "    movb 0x7da8, %ch\n"
        "    movl 0x7dac, %eax\n"
        "    shrl $4, %eax\n"
        "    movw %ax, %es\n"
        "    xorl %ebx, %ebx\n"
        "    movb 0x7db4, %dl\n"
        "    movb $0x01, %al\n"
        "    movb $0x02, %ah\n"
        "    int $0x13\n"
        "    jc __boot_fail\n"
        "    movl 0x7da0, %eax\n"
        "    decl %eax\n"
        "    movl %eax, 0x7da0\n"
        "    movl 0x7dac, %eax\n"
        "    addl $512, %eax\n"
        "    movl %eax, 0x7dac\n"
        "    movb 0x7da4, %al\n"
        "    incb %al\n"
        "    movb %al, 0x7da4\n"
        "    movb 0x7da4, %al\n"
        "    cmpb 0x7db6, %al\n"
        "    jbe __boot_load\n"
        "    movw $1, 0x7da4\n"
        "    movb 0x7da6, %al\n"
        "    incb %al\n"
        "    movb %al, 0x7da6\n"
        "    movb 0x7da6, %al\n"
        "    cmpb 0x7db8, %al\n"
        "    jb __boot_load\n"
        "    movw $0, 0x7da6\n"
        "    movb 0x7da8, %al\n"
        "    incb %al\n"
        "    movb %al, 0x7da8\n"
        "    jmp __boot_load\n"
        "__boot_go:\n"
        "    ljmp $0x1000, $0x0000\n"
        "__boot_fail:\n"
        "    movb $'!', %al\n"
        "    movw $0x3f8, %dx\n"
        "    outb %al, %dx\n"
        "    movw $0xf4, %dx\n"
        "    movb $1, %al\n"
        "    outb %al, %dx\n"
        "    hlt\n"
        "    .org 0x1a0\n"
        "boot_rem:\n"
        "    .long 0\n"
        "boot_sec:\n"
        "    .word 0\n"
        "boot_head:\n"
        "    .word 0\n"
        "boot_cyl:\n"
        "    .word 0\n"
        "boot_buf:\n"
        "    .long 0\n"
        "    .org 0x1b4\n"
        "boot_drive:\n"
        "    .byte 0\n"
        "    .byte 0\n"
        "boot_spt:\n"
        "    .byte 0\n"
        "    .byte 0\n"
        "boot_heads:\n"
        "    .word 0\n"
        "    .org 0x8400\n"
        "    .section .text,\"ax\"\n"
        "    .globl _entry\n"
        "_entry:\n"
        "    cli\n"
        "    xorl %eax, %eax\n"
        "    movl %eax, %ds\n"
        "    movl %eax, %es\n"
        "    movw $0x9000, %ax\n"
        "    movw %ax, %ss\n"
        "    movl $0xf000, %esp\n"
        "    movl $boot_argv, %eax\n"
        "    pushl %eax\n"
        "    call main\n"
        "    movw $0xf4, %dx\n"
        "    movb $42, %al\n"
        "    outb %al, %dx\n"
        "    hlt\n"
        "    .section .rodata\n"
        "    .globl boot_argv\n"
        "boot_argv:\n"
        "    .long 2, 0xFFFF0000, 2, 0xFFFF0000\n") + out;
    gcAsmFunctions(out);
    return true;
  }
  error = "native backend: arch '" + arch + "' not implemented";
  return false;
}

bool emitNativePerUnit(const ModuleGraph& graph, const SemaResult& sema,
                       const std::string& arch, const std::string& os,
                       std::vector<AsmUnit>& out, std::string& error,
                       bool testMode, const std::string* testFilter,
                       const std::vector<int>* only) {
  if (const char* forced = std::getenv("A_CODEGEN");
      forced && std::string(forced) == "c") {
    error = "native backend: disabled by A_CODEGEN=c";
    return false;
  }
  if (arch == "i8086" || arch == "8086") {
    error = "native backend: per-unit split not supported for i8086 (bare-metal flat image)";
    return false;
  }
  if (arch == "x86_64" && os == "windows") {
    Win64Target t;
    NativeEmitter e(graph, sema, t, testMode, testFilter);
    return e.runPerUnit(out, error, only);
  }
  if (os == "windows") {
    error = "native backend: no native target for '" + arch + "' on windows";
    return false;
  }
  if (arch == "arm64" || arch == "aarch64") {
    Arm64Target t;
    NativeEmitter e(graph, sema, t, testMode, testFilter);
    return e.runPerUnit(out, error, only);
  }
  if (arch == "riscv64" || arch == "riscv64gc") {
    Riscv64Target t;
    NativeEmitter e(graph, sema, t, testMode, testFilter);
    return e.runPerUnit(out, error, only);
  }
  if (arch == "i386" || arch == "x86") {
    I386Target t;
    NativeEmitter e(graph, sema, t, testMode, testFilter);
    return e.runPerUnit(out, error, only);
  }
  if (arch == "arm" || arch == "arm32") {
    Arm32Target t;
    NativeEmitter e(graph, sema, t, testMode, testFilter);
    return e.runPerUnit(out, error, only);
  }
  error = "native backend: arch '" + arch + "' not implemented";
  return false;
}

}  // namespace a
