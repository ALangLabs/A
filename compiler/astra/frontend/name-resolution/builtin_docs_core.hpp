#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addCoreDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // core
      {"__file__", {"__file__", "__file__()", "Returns the current source file path."}},
      {"__line__", {"__line__", "__line__()", "Returns the current 1-based source line."}},
      {"__func__", {"__func__", "__func__()", "Returns the current function name, or `<module>`."}},
      {"__debug_repr__", {"__debug_repr__", "__debug_repr__(value) -> String", "Returns a debug string for a value."}},
      {"__debug_time__", {"__debug_time__", "__debug_time__() -> String", "Returns a local debug timestamp `HH:MM:SS.mmm`."}},
      {"__debug_context__", {"__debug_context__", "__debug_context__() -> String", "Returns `file:line in fn()` or module context."}},
      {"__ic_format__", {"__ic_format__", "__ic_format__(context[, label, value]) -> String",
                          "Formats a litprinter-style `ic|` line. With one arg it formats a breadcrumb; with label/value it formats an inspection."}},
      {"panic", {"panic", "panic(message)", "Aborts with exit status 2 and the message."}},
      {"assert", {"assert", "assert(condition[, message])",
                  "Runtime error when the condition is false."}},
      {"assert_eq", {"assert_eq", "assert_eq(actual, expected[, message])",
                     "Runtime error when the two values differ."}},
      {"ic", {"ic", "ic(*values) -> value | (values) | void",
              "Debug-inspects values to stderr with file/function context and source labels (litprinter-style), and returns the inspected value(s) unchanged.",
              ">>> x = 42\n>>> ic(x)\nic| x: 42\n>>> ic(x + 1)\nic| [app.a:2 in main()] >>> x + 1: 43",
              "print, eprint, __debug_context__, __debug_time__, __debug_repr__"}},

      // time
      {"now", {"now", "now() -> i64", "Current time as Unix epoch milliseconds."}},
      {"sleep", {"sleep", "sleep(milliseconds)", "Pauses for a non-negative duration."}},
  });
}

}  // namespace a
