#include "driver.hpp"

#include <iosfwd>
#include <ostream>

namespace a {

// MLIR dev files were not found at configure time: `a mlir-opt` is
// unavailable. The `a` binary still links this TU so the symbol exists.
int runMlirOptCli(int argc, char** argv, std::ostream& out, std::ostream& err) {
  (void)argc;
  (void)argv;
  (void)out;
  err << "error: 'a mlir-opt' requires MLIR 18 dev files "
         "(rebuild with MLIR/LLVM found by find_package)\n";
  return 1;
}

} // namespace a
