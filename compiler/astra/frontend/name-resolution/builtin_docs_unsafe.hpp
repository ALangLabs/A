#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addUnsafeDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // unsafe
      {"unsafe_get", {"unsafe_get", "unsafe_get(array, index) -> T",
                       "Unchecked element access; no bounds check."}},
      {"unsafe_set", {"unsafe_set", "unsafe_set(array, index, value)",
                       "Unchecked element assignment; no bounds check."}},
      {"unsafe_address_of", {"unsafe_address_of", "unsafe_address_of(array, index) -> *T",
                             "Raw pointer to an array element."}},
      {"unsafe_load", {"unsafe_load", "unsafe_load(pointer) -> T",
                       "Reads the value a pointer refers to."}},
      {"unsafe_store", {"unsafe_store", "unsafe_store(pointer, value)",
                        "Writes through a raw pointer."}},
      {"unsafe_offset", {"unsafe_offset", "unsafe_offset(pointer, count) -> *T",
                         "Moves a pointer by a number of elements."}},
  });
}

}  // namespace a
