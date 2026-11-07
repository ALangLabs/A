#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addTestingDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // testing
      {"testing_seed", {"testing_seed", "testing.seed(value)",
                        "Fixes the generator so a run is reproducible."}},
      {"testing_for_all_int", {"testing_for_all_int",
                                "testing.for_all_int(runs, low, high, fn(i64) -> bool) -> bool",
                                "Random int property; the first counterexample goes to stderr."}},
      {"testing_for_all_float", {"testing_for_all_float",
                                  "testing.for_all_float(runs, low, high, fn(f64) -> bool) -> bool",
                                  "Random float property; the first counterexample goes to stderr."}},
      {"testing_for_all_bool", {"testing_for_all_bool",
                                 "testing.for_all_bool(runs, fn(bool) -> bool) -> bool",
                                 "Random bool property over generated inputs."}},
      {"testing_for_all_string", {"testing_for_all_string",
                                   "testing.for_all_string(runs, maxLength, fn(String) -> bool) -> bool",
                                   "Random printable-ASCII string property."}},
      {"testing_for_all_elem", {"testing_for_all_elem",
                                 "testing.for_all_elem(values, fn(T) -> bool) -> bool",
                                 "Checks the property for every element of an array."}},
  });
}

}  // namespace a
