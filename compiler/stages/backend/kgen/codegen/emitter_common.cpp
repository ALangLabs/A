#include "backend/kgen/codegen/emitter_common.hpp"

namespace a::detail {

bool isStdlibModule(const ModuleGraph& graph, const Module& mod) {
  if (mod.id == graph.entry) return false;
  return mod.path.find("stdlib") != std::string::npos;
}

std::string extractString(const StringLitExpr& s) {
  std::string result;
  for (const auto& p : s.parts) {
    if (!p.isExpr) {
      result += p.text;
    }
  }
  return result;
}

}  // namespace a::detail
