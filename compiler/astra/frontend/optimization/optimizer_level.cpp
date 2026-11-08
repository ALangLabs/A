#include "optimization/optimizer_internal.hpp"

namespace a {
namespace ir {
bool parseOptLevel(const std::string& flag, OptLevel& level) {
  if (flag == "O0" || flag == "-O0") level = OptLevel::O0;
  else if (flag == "O1" || flag == "-O1") level = OptLevel::O1;
  else if (flag == "O2" || flag == "-O2") level = OptLevel::O2;
  else if (flag == "O3" || flag == "-O3") level = OptLevel::O3;
  else if (flag == "Os" || flag == "-Os") level = OptLevel::Os;
  else if (flag == "Oz" || flag == "-Oz") level = OptLevel::Oz;
  else if (flag == "Ofast" || flag == "-Ofast") level = OptLevel::Ofast;
  // "--opt-level=N" (clamped to 0..3, mirroring clang's clamping)
  else if (flag.rfind("--opt-level=", 0) == 0) {
    try {
      const int n = std::stoi(flag.substr(12));
      if (n < 0) level = OptLevel::O0;
      else if (n == 1) level = OptLevel::O1;
      else if (n == 2) level = OptLevel::O2;
      else level = OptLevel::O3;
    } catch (...) {
      return false;
    }
  } else {
    return false;
  }
  return true;
}

bool optLevelFromInt(int value, OptLevel& level) {
  switch (value) {
    case 0: level = OptLevel::O0; return true;
    case 1: level = OptLevel::O1; return true;
    case 2: level = OptLevel::O2; return true;
    case 3: level = OptLevel::O3; return true;
    case -1: level = OptLevel::Os; return true;
    case -2: level = OptLevel::Oz; return true;
    case 4: level = OptLevel::Ofast; return true;
    default: return false;
  }
}
}  // namespace ir
}  // namespace a
