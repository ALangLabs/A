#pragma once

#include <string>

#include "memory/value.hpp"

namespace a {

// Renders a value for display. Strings are quoted when `quoteStrings` is true
// (used when they appear inside containers) and raw otherwise.
std::string formatValue(const Value& value, bool quoteStrings);

// Optional hook for values owned by an optional embedded runtime (e.g. CPython
// objects from `import python:<pkg>`). The backend registers it once the
// runtime is initialized; formatValue falls back to a placeholder otherwise.
// Returns an empty string when the hook does not handle the value.
using ForeignValueRepr = std::string (*)(const Value& value);
void setForeignValueRepr(ForeignValueRepr fn);

}  // namespace a
