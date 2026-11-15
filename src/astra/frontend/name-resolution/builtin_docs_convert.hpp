#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addConvertDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // conversions / optionals
      {"int", {"int", "int(value) -> i64", "Converts a number, bool, or numeric string to i64."}},
      {"float", {"float", "float(value) -> f64",
                  "Converts a number, bool, or numeric string to f64."}},
      {"Some", {"Some", "Some(value) -> T?", "Wraps a value in an optional."}},
      {"Ok", {"Ok", "Ok(value) -> Result", "Wraps a value as a successful result."}},
      {"Err", {"Err", "Err(error) -> Result", "Wraps a value as a failed result."}},
      {"move", {"move", "move(value) -> T", "Moves ownership out of a binding."}},
      {"copy", {"copy", "copy(value) -> T", "Deep-copies a value."}},
      {"borrow", {"borrow", "borrow(variable)", "Immutable borrow tracked by the analyzer."}},
      {"borrow_mut", {"borrow_mut", "borrow_mut(variable)",
                      "Mutable borrow tracked by the analyzer."}},
  });
}

}  // namespace a
