#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addMathDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // math (signature + docstring + doctest example)
      {"sqrt", {"sqrt", "sqrt(x) -> f64", "Square root of x.",
                ">>> import std:math\n>>> math.sqrt(16)\n4.0",
                "floor, ceil, round"}},
      {"abs", {"abs", "abs(x) -> number", "Absolute value, keeping the argument type.",
               ">>> abs(-7)\n7",
               "min, max"}},
      {"min", {"min", "min(a, b) -> number", "The smaller of two values of the same numeric type.",
               ">>> min(3, 7)\n3",
               "max"}},
      {"max", {"max", "max(a, b) -> number", "The larger of two values of the same numeric type.",
               ">>> max(3, 7)\n7",
               "min"}},
      {"pow", {"pow", "pow(base, exponent) -> f64", "Raises base to the exponent.",
               ">>> pow(2, 10)\n1024.0"}},

      {"sin", {"sin", "sin(x) -> f64", "Sine of x, in radians."}},
      {"cos", {"cos", "cos(x) -> f64", "Cosine of x, in radians."}},
      {"tan", {"tan", "tan(x) -> f64", "Tangent of x, in radians."}},
      {"asin", {"asin", "asin(x) -> f64", "Arc sine, returning radians."}},
      {"acos", {"acos", "acos(x) -> f64", "Arc cosine, returning radians."}},
      {"atan", {"atan", "atan(x) -> f64", "Arc tangent, returning radians."}},
      {"atan2", {"atan2", "atan2(y, x) -> f64", "Angle of the vector (x, y) in radians."}},
      {"exp", {"exp", "exp(x) -> f64", "e raised to the x."}},
      {"log", {"log", "log(x) -> f64", "Natural logarithm."}},
      {"log2", {"log2", "log2(x) -> f64", "Base-2 logarithm."}},
      {"log10", {"log10", "log10(x) -> f64", "Base-10 logarithm."}},
      {"floor", {"floor", "floor(x) -> f64", "Rounds down to the nearest integer value."}},
      {"ceil", {"ceil", "ceil(x) -> f64", "Rounds up to the nearest integer value."}},
      {"round", {"round", "round(x) -> f64", "Rounds to the nearest integer value."}},
      {"trunc", {"trunc", "trunc(x) -> f64", "Truncates toward zero."}},
      {"random", {"random", "random() -> f64", "A pseudo-random float in [0, 1)."}},
      {"random_int", {"random_int", "random_int(low, high) -> i64",
                      "A pseudo-random integer between low and high, inclusive."}},
      {"PI", {"PI", "PI: f64", "The ratio of a circle's circumference to its diameter."}},
      {"E", {"E", "E: f64", "Euler's number, the base of the natural logarithm."}},
  });
}

}  // namespace a
