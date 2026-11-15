#include "package.hpp"

#include <iosfwd>
#include <ostream>

namespace a {

// MLIR dev files were not found at configure time: .ambc precompilation is
// unavailable. The `a` binary still links this TU so the symbol exists.
int runPrecompileCli(int argc, char** argv, std::ostream& out,
                     std::ostream& err) {
  (void)argc;
  (void)argv;
  (void)out;
  err << "error: 'a precompile' requires MLIR 18 dev files "
         "(rebuild with MLIR/LLVM found by find_package)\n";
  return 1;
}

} // namespace a
