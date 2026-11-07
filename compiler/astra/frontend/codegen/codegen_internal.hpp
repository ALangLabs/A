#pragma once
// Internal shared types/helpers for the codegen backends (C emitter in
// codegen.cpp, asm emitter in codegen_emitter.hpp + codegen_*.cpp). Not part of the public
// codegen.hpp interface.
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/ast.hpp"
#include "ast/builtins.hpp"

namespace a {

// Runtime builtin ids; order must match the enum in a_runtime.h.
inline int builtinRuntimeId(const std::string& name) {
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
  };
  auto it = kMap.find(name);
  return it == kMap.end() ? -1 : it->second;
}

inline int builtinIdForBuiltin(BuiltinId id) {
  switch (id) {
    case BuiltinId::Print: return 0;
    case BuiltinId::Eprint: return 1;
    case BuiltinId::Len: return 2;
    case BuiltinId::Str: return 3;
    case BuiltinId::Sqrt: return 12;
    case BuiltinId::F64: return 5;
    case BuiltinId::I64: return 4;
    case BuiltinId::Abs: return 7;
    case BuiltinId::Round: return 23;
    case BuiltinId::Floor: return 21;
    case BuiltinId::Ceil: return 22;
    case BuiltinId::Pow: return 11;
    case BuiltinId::Join: return 56;
    case BuiltinId::Map: return 32;
    case BuiltinId::Filter: return 33;
    case BuiltinId::Sum: return 10;
    case BuiltinId::Any: return 34;
    case BuiltinId::All: return 35;
    case BuiltinId::Some: return 36;
    case BuiltinId::Ok: return 37;
    case BuiltinId::Err: return 38;
    case BuiltinId::Panic: return 39;
    case BuiltinId::Assert: return 40;
    case BuiltinId::AssertEq: return 41;
    case BuiltinId::DebugRepr: return 53;  // __repr__/__debug_repr__ -> a_repr
    default: return -1;
  }
}

inline std::string cEscape(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\%03o", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

inline std::string utf8Encode(char32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

inline std::string floatText(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  std::string s = buf;
  if (s.find_first_of(".eEnN") == std::string::npos) s += ".0";
  return s;
}

inline std::string stemOf(const std::string& path) {
  auto slash = path.find_last_of("/\\");
  std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  auto dot = base.find_last_of('.');
  if (dot != std::string::npos) base.resize(dot);
  return base;
}

struct FnInfo {
  const FnDecl* decl = nullptr;
  std::string cname;
  std::string globalVar;  // aval global holding the FNON value
  std::vector<std::string> paramNames;
  bool isMethod = false;
  std::string structName;
  int moduleId = -1;
  std::string name;
  bool variadic = false;  // last param collects extra args into a list
};

struct StructInfo {
  const StructDecl* decl = nullptr;
  std::string ctorCname;
  std::string globalVar;
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
};

enum class BKind { Local, Global, Param, Capture, FnValue, Builtin, StructCtor, EnumValue, Module, PyModule, PyFunc, NativeConst };

}  // namespace a
