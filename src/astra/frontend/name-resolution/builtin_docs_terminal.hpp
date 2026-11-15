#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addTerminalDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // terminal (interactive REPL support)
      {"terminal_isatty", {"terminal_isatty", "terminal.isatty() -> bool",
                            "Whether stdin and stdout are terminals."}},
      {"terminal_raw_mode", {"terminal_raw_mode", "terminal.raw_mode(enabled) -> bool",
                              "Switches the terminal to per-character input; returns the previous state."}},
      {"terminal_read_char", {"terminal_read_char", "terminal.read_char() -> String?",
                                "Reads one UTF-8 character; None on EOF."}},
      {"terminal_read_char_ms", {"terminal_read_char_ms", "terminal.read_char_ms(timeoutMs) -> String?",
                                  "Reads one UTF-8 character, or None on timeout/EOF."}},
      {"terminal_write", {"terminal_write", "terminal.write(text)",
                           "Writes text to stdout without a trailing newline."}},
      {"terminal_flush", {"terminal_flush", "terminal.flush()", "Flushes stdout."}},
      {"terminal_size", {"terminal_size", "terminal.size() -> [i64]",
                          "Terminal size as [columns, rows]."}},
      {"terminal_home", {"terminal_home", "terminal.home() -> String", "The user's home directory."}},

      // env
      {"env_get", {"env_get", "env.get(name) -> String", "Environment variable value, or empty."}},

      // repl
      {"repl_eval", {"repl_eval", "repl.eval(source) -> i64",
                      "Evaluates A source in the persistent REPL session; 0 ok, 1 compile error, 2 runtime error."}},
      {"repl_quiet", {"repl_quiet", "repl.quiet(on: bool)",
                      "Suppresses print/eprint output while set; the REPL uses it to replay session history silently."}},
  });
}

}  // namespace a
