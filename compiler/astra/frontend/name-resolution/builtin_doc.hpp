#pragma once

namespace a {

// Documentation for every interpreter builtin that a native module or the
// bare language surface can reach. Keyed by builtin name (the value that
// `nativeFunction()` maps a module member to), so the type checker, the
// language server, and signature help all read the same table.
struct BuiltinDoc {
  const char* name;
  const char* signature;
  const char* documentation;
  // Doctest example (`>>>` lines), rendered as "Example" in hover like
  // pydoc docstrings. Empty when not provided.
  const char* example = nullptr;
  const char* seeAlso = nullptr;
};

}  // namespace a
