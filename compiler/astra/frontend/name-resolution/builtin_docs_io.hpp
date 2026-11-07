#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addIoDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // io
      {"print", {"print",
                 "print(*objects, sep: String = \" \", end: String = \"\\n\", file: IO = stdout, flush: bool = false) -> void",
                 "Writes values to a stream, separated, with an ending.",
                 ">>> print('Hello, A!')\nHello, A!\n>>> print(1, 2, sep='-')\n1-2",
                 "eprint, open, close, flush"}},
      {"eprint", {"eprint",
                  "eprint(*objects, sep: String = \" \", end: String = \"\\n\", file: IO = stderr, flush: bool = false) -> void",
                  "Writes values to standard error (or file), separated, with an ending.",
                  ">>> eprint('Error: bad input')",
                  "print"}},
      {"open", {"open", "open(path: String, mode: String = \"w\") -> IO",
                "Opens a file for writing; mode is \"w\" (create/truncate), \"a\" (append), or \"r\".",
                ">>> f = open('out.txt', 'w')\n>>> print('A!', file=f)\n>>> close(f)",
                "close, print"}},
      {"close", {"close", "close(stream: IO)",
                 "Closes a handle returned by open().",
                 ">>> close(f)",
                 "open"}},
      {"stdout", {"stdout", "stdout: IO", "The standard output stream handle."}},
      {"stderr", {"stderr", "stderr: IO", "The standard error stream handle."}},
      {"input", {"input", "input([prompt]) -> String", "Reads one line from standard input.",
                 ">>> name = input('Name? ')\n>>> name\nAda"}},
      {"flush", {"flush", "flush()", "Flushes buffered standard output.",
                 ">>> flush()"}},

      // fs (mirrors Python's pathlib / os)
      {"read_file", {"read_file", "read_file(path) -> String", "Reads an entire text file.",
                     ">>> text = read_file('notes.txt')\n>>> text\nHello, A!",
                     "write_file, file_exists"}},
      {"write_file", {"write_file", "write_file(path, text)", "Creates or replaces a file with text.",
                      ">>> write_file('out.txt', 'Hello, A!')",
                      "read_file, append_file"}},
      {"append_file", {"append_file", "append_file(path, text)", "Appends text to a file.",
                       ">>> append_file('log.txt', 'line 1\\n')",
                       "write_file"}},
      {"file_exists", {"file_exists", "file_exists(path) -> bool", "Whether a path exists.",
                       ">>> file_exists('main.a')\ntrue",
                       "read_file"}},
      {"list_dir", {"list_dir", "list_dir(path) -> [String]", "Sorted entry names of a directory.",
                    ">>> list_dir('.')\n['README.md', 'build', 'main.a']",
                    "file_exists"}},
      {"remove_file", {"remove_file", "remove_file(path)", "Deletes a file.",
                       ">>> remove_file('tmp.txt')",
                       "file_exists"}},
      {"create_dir", {"create_dir", "create_dir(path)", "Creates a directory and any missing parents.",
                      ">>> create_dir('build')",
                      "remove_dir"}},
      {"remove_dir", {"remove_dir", "remove_dir(path)", "Removes an empty directory.",
                      ">>> remove_dir('build')",
                      "create_dir"}},
      {"rename", {"rename", "rename(old, new)", "Renames or moves a path.",
                  ">>> rename('old.a', 'new.a')",
                  "remove_file"}},
      {"current_dir", {"current_dir", "current_dir() -> String", "The process working directory.",
                       ">>> current_dir()\n'/home/user/project'",
                       "list_dir"}},
  });
}

}  // namespace a
