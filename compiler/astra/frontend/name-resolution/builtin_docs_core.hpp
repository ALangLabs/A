#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addCoreDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // core
      {"__debug_repr__", {"__debug_repr__", "__debug_repr__(value) -> String", "Returns a debug string for a value."}},
      {"__ic_format__", {"__ic_format__", "__ic_format__(context[, label, value]) -> String",
                          "Formats a litprinter-style `ic|` line. With one arg it formats a breadcrumb; with label/value it formats an inspection."}},
      {"panic", {"panic", "panic(message)", "Aborts with exit status 2 and the message."}},
      {"assert", {"assert", "assert(condition[, message])",
                  "Runtime error when the condition is false."}},
      {"assert_eq", {"assert_eq", "assert_eq(actual, expected[, message])",
                     "Runtime error when the two values differ."}},
      {"ic", {"ic", "ic(*values) -> value | [values] | void",
              "Debug-inspects values to stderr with file/function context and source labels (litprinter-style), and returns the inspected value(s) unchanged.",
              ">>> x = 42\n>>> ic(x)\nic| x: 42\n>>> ic(x + 1)\nic| [app.a:2 in main()] >>> x + 1: 43"}},

      // time
      {"now", {"now", "now() -> i64", "Current time as Unix epoch milliseconds."}},
      {"sleep", {"sleep", "sleep(milliseconds)", "Pauses for a non-negative duration."}},

      // struct introspection (class instances)
      {"struct_name", {"struct_name", "struct_name(value) -> String",
                       "Declared class name for a struct instance; empty string otherwise."}},
      {"struct_methods", {"struct_methods", "struct_methods(value) -> [String]",
                          "Method names declared on a struct (including inherited); empty list otherwise."}},
      {"fn_signature", {"fn_signature", "fn_signature(fn) -> String",
                        "Compile-time signature text \"name(p1, p2)\" for a function value; name only when unknown."}},
      {"fn_doc", {"fn_doc", "fn_doc(fn) -> String",
                  "Docstring attached to a function at compile time; empty string when absent."}},
      {"bytes_decode_utf8", {"bytes_decode_utf8", "core.bytes_utf8_decode(bytes) -> String?",
                             "Validating one-pass UTF-8 decode of an [i64] byte array; None when malformed."}},
      {"str_encode_utf8", {"str_encode_utf8", "core.bytes_utf8_encode(s) -> [i64]",
                           "One-pass UTF-8 byte array of a string (std:bytes hot path)."}},
  });
}

}  // namespace a
