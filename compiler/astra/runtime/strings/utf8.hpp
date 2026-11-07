#pragma once

#include <string>

namespace a {

// Encodes one Unicode code point as UTF-8.
std::string utf8Encode(char32_t codepoint);

}  // namespace a
