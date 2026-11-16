#pragma once
// Small helpers shared verbatim by the two MLIR emitters:
// mlir_emit.cpp (boxed llvm-dialect emitter) and mlir_emit_ad.cpp
// (`a`-dialect emitter). Keep this header to truly emitter-agnostic,
// identical utilities — the emit()/evalExpr() method bodies intentionally
// stay per-emitter.
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace a {
namespace mlirutil {

// A_TAG NaN-box tag and the inline-int magnitude limit shared by both
// emitters' aval encoding.
inline constexpr std::int64_t kAvalTag = -281474976710656LL;  // A_TAG
inline constexpr std::int64_t kInlineLim = INT64_C(1) << 46;

inline std::int64_t avalIntEncode(std::int64_t v) {
  std::uint64_t p = ((std::uint64_t)v << 1) | 1ULL;
  p &= 281474976710655ULL;
  return (std::int64_t)(kAvalTag | (std::int64_t)p);
}

// filename stem of a module path: "lib/mathx.astra" -> "mathx",
// "pkg/__init__.astra" -> "pkg".
// NOTE: distinct from name-resolution's stemOf() (module_loader.cpp),
// which uses std::filesystem and has different __init__ semantics.
inline std::string stemOf(const std::string& p) {
  std::string s = p;
  auto slash = s.find_last_of('/');
  if (slash != std::string::npos) s = s.substr(slash + 1);
  auto dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  if (s == "__init__") {
    std::string d = p.substr(0, slash == std::string::npos ? 0 : slash);
    auto s2 = d.find_last_of('/');
    d = s2 == std::string::npos ? d : d.substr(s2 + 1);
    return d;
  }
  return s;
}

// Builtin constants resolved for bare identifiers in evalIdent
// (mirrors LlvmEmitter::evalIdent kConsts).
inline const std::unordered_map<std::string, double> kBuiltinDConsts = {
    {"PI", 3.14159265358979323846}, {"E", 2.71828182845904523536},
    {"F32_MIN", -3.4028234663852886e38}, {"F32_MAX", 3.4028234663852886e38},
    {"F64_MIN", -1.7976931348623157e308}, {"F64_MAX", 1.7976931348623157e308},
};
inline const std::unordered_map<std::string, std::int64_t> kBuiltinIConsts = {
    {"I8_MIN", -128}, {"I8_MAX", 127},
    {"I16_MIN", -32768}, {"I16_MAX", 32767},
    {"I32_MIN", -2147483648LL}, {"I32_MAX", 2147483647LL},
    {"U8_MAX", 255}, {"U16_MAX", 65535}, {"U32_MAX", 4294967295LL},
};

// ---- auto-parfor helpers ---------------------------------------------------
// Deny-list of callee names that must not run inside a parfor worker
// (shared mutable state / side-effecting builtins).
inline bool pfDenyName(const std::string& n) {
  static const char* k[] = {
      "print","println","eprint","eprintln","printerr","panic","assert",
      "assert_eq","input","exit","write_file","append_file","write_bytes",
      "remove_file","remove_dir","create_dir","rename","system","popen",
      "kill","exec","spawn","join","done","cancel","append","extend",
      "insert","remove","pop","clear","update","add","discard","push",
      "send","sendall","sendto","write","writeall","recv","recvfrom",
      "accept","connect","bind","listen"};
  for (const char* d : k) if (n == d) return true;
  return false;
}

// A_PARFOR=0/off/no/false disables auto-parfor; default on.
inline bool parforEnabled() {
  const char* e = std::getenv("A_PARFOR");
  if (!e) return true;
  std::string v = e;
  return !(v == "0" || v == "off" || v == "no" || v == "false");
}

// Renders "  llvm.func @<name>(<typed args>) -> <ret>" for a compact
// "(args) -> ret" signature string (the runtime_sigs.hpp form; also used
// for the `a`-dialect emitter's local *_slow/floor/fmod sig table).
inline std::string runtimeDeclText(const std::string& name, const char* sig) {
  std::string s = sig;
  std::string args = s.substr(1, s.find(')') - 1);
  std::string ret = s.substr(s.find("->") + 3);
  while (!ret.empty() && ret.front() == ' ') ret.erase(ret.begin());
  // split arg types on commas not inside <>
  std::vector<std::string> tys;
  {
    std::string cur;
    int depth = 0;
    for (char ch : args) {
      if (ch == '<') ++depth;
      if (ch == '>') --depth;
      if (ch == ',' && depth == 0) { tys.push_back(cur); cur.clear(); }
      else cur += ch;
    }
    if (!cur.empty()) tys.push_back(cur);
  }
  std::string decl = "  llvm.func @" + name + "(";
  for (size_t i = 0; i < tys.size(); ++i) {
    if (i) decl += ", ";
    decl += "%a" + std::to_string(i) + ": " + tys[i];
  }
  decl += ")";
  if (ret != "()") decl += " -> " + ret;
  return decl;
}

}  // namespace mlirutil
}  // namespace a
