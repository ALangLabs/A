#pragma once

#include <string>

namespace a {

// Round-trippable decimal for a double literal (`%.17g`, with `.0`
// appended to integral values so the spelling stays a float).
std::string floatText(double value);

}  // namespace a
