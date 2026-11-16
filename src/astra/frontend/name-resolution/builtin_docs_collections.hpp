#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addCollectionsDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // strings / collections (shared builtins, like Python's builtins + itertools)
      {"len", {"len", "len(value) -> i64", "Length of a string, array, map, or set.",
               ">>> len('A language')\n9\n>>> len([1, 2, 3])\n3",
               "str"}},
      {"str", {"str", "str(value) -> String", "Converts a value to its string representation.",
               ">>> str(42)\n'42'\n>>> str([1, 2])\n'[1, 2]'",
               "print"}},
      {"join", {"join", "join(values, separator) -> String",
                "Concatenates an array of strings with a separator.",
                ">>> join(['a', 'b', 'c'], '-')\n'a-b-c'",
                "String.join"}},
      {"map", {"map", "map(values, fn) -> array", "Applies a function to every element.",
               ">>> map([1, 2, 3], fn(x) => x * 2)\n[2, 4, 6]",
               "filter"}},
      {"filter", {"filter", "filter(values, fn) -> array",
                  "Keeps the elements for which a predicate is true.",
                  ">>> filter([1, 2, 3, 4], fn(x) => x % 2 == 0)\n[2, 4]",
                  "map"}},
      {"reduce", {"reduce", "reduce(values, initial, fn) -> value",
                  "Folds an array from left to right.",
                  ">>> reduce([1, 2, 3], 0, fn(acc, x) => acc + x)\n6",
                  "sum"}},
      {"any", {"any", "any(values, fn) -> bool", "True when the predicate holds for some element.",
               ">>> any([1, 2, 3], fn(x) => x > 2)\ntrue",
               "all"}},
      {"all", {"all", "all(values, fn) -> bool", "True when the predicate holds for every element.",
               ">>> all([2, 4, 6], fn(x) => x % 2 == 0)\ntrue",
               "any"}},
      {"sum", {"sum", "sum(values) -> number", "Sum of an array of numbers.",
               ">>> sum([1, 2, 3])\n6",
               "reduce"}},
      {"set", {"set", "set(values) -> Set",
                "Builds a set from an array, range, set, or string.",
                ">>> set([1, 2, 2, 3])\n{1, 2, 3}",
                "Set methods"}},
  });
}

}  // namespace a
