#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addPyDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // py (Python interop)
      {"py_buffer", {"py_buffer", "py_buffer(obj) -> pybuffer",
                     "Zero-copy view over a Python object's buffer protocol "
                     "(numpy arrays, array.array, memoryview, bytearray). "
                     "Supports C-contiguous numeric buffers; index, len(), "
                     "iteration, and item assignment (when writable) work "
                     "directly on the exported memory."}},
  });
}

}  // namespace a
