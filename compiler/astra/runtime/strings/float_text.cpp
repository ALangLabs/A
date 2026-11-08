#include "strings/float_text.hpp"

#include <cstdio>

namespace a {

std::string floatText(double value) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", value);
  std::string s = buf;
  if (s.find_first_of(".eEnN") == std::string::npos) s += ".0";
  return s;
}

}  // namespace a
