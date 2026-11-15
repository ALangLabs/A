// In-process bridge from MLIR text to the native backend. See native.hpp.

#include "native.hpp"

#include <memory>
#include <sstream>
#include <string>

#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Index/IR/IndexDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/IR/AsmState.h"  // ParserConfig
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Transforms/Passes.h"

#include "codegen.hpp"  // a::initMLIR

namespace a {

void registerCoreDialects(mlir::MLIRContext& ctx) {
  // Register only what the emitter/backend path needs; keeps the link
  // surface small vs mlir::registerAllDialects (InitAllDialects.h).
  mlir::DialectRegistry registry;
  registry.insert<mlir::func::FuncDialect, mlir::LLVM::LLVMDialect,
                  mlir::cf::ControlFlowDialect, mlir::scf::SCFDialect,
                  mlir::arith::ArithDialect, mlir::index::IndexDialect,
                  mlir::vector::VectorDialect>();
  ctx.appendDialectRegistry(registry);
  ctx.loadDialect<mlir::func::FuncDialect, mlir::LLVM::LLVMDialect,
                  mlir::cf::ControlFlowDialect, mlir::scf::SCFDialect,
                  mlir::arith::ArithDialect, mlir::index::IndexDialect,
                  mlir::vector::VectorDialect>();
  // Ensure the llvm- and builtin-dialect -> LLVM IR translation interfaces
  // are registered on this context (registerAllTranslations is global and
  // must precede context creation; these per-context calls cover contexts
  // that already exist, e.g. sharedMlirContext).
  initMLIR();
  mlir::registerBuiltinDialectTranslation(ctx);
  mlir::registerLLVMDialectTranslation(ctx);
}

mlir::MLIRContext& sharedMlirContext() {
  // Leaked on purpose: the context must outlive every parsed ModuleOp and
  // function-local static destruction ordering is fragile for MLIR.
  static mlir::MLIRContext* ctx = [] {
    auto* c = new mlir::MLIRContext();
    registerCoreDialects(*c);
    return c;
  }();
  return *ctx;
}

mlir::OwningOpRef<mlir::ModuleOp> parseMlirText(mlir::MLIRContext& ctx,
                                               llvm::StringRef text,
                                               std::string& error) {
  error.clear();
  std::ostringstream captured;
  // Capture diagnostics instead of letting them go to errs(); returning
  // success() from the handler marks each diagnostic as fully processed.
  mlir::ScopedDiagnosticHandler handler(
      &ctx, [&](mlir::Diagnostic& diag) {
        if (diag.getSeverity() == mlir::DiagnosticSeverity::Error ||
            diag.getSeverity() == mlir::DiagnosticSeverity::Warning) {
          if (!captured.str().empty()) captured << '\n';
          captured << diag.str();
        }
      });

  mlir::ParserConfig config(&ctx);
  mlir::OwningOpRef<mlir::ModuleOp> module =
      mlir::parseSourceString<mlir::ModuleOp>(text, config, "mlir-input");
  if (!module) {
    error = captured.str();
    if (error.empty()) error = "failed to parse MLIR input";
    return mlir::OwningOpRef<mlir::ModuleOp>();
  }
  error = captured.str();
  return module;
}

int nativeCompileMlirTextToObject(const std::string& mlirText,
                                  const std::string& arch, int optLevel,
                                  const std::string& objPath,
                                  std::ostream& err) {
  std::string parseError;
  mlir::OwningOpRef<mlir::ModuleOp> module =
      parseMlirText(sharedMlirContext(), mlirText, parseError);
  if (!module) {
    err << "error: mlir backend: "
        << (parseError.empty() ? "failed to parse MLIR text" : parseError)
        << "\n";
    return 1;
  }

  // Lightweight MLIR-level cleanup before LLVM translation: canonicalizer +
  // CSE on the llvm dialect module. The heavy optimization still happens in
  // the LLVM opt pipeline inside mlirModuleToObject.
  mlir::PassManager pm(module->getContext());
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(mlir::createCSEPass());
  if (failed(pm.run(module->getOperation()))) {
    err << "error: mlir backend: MLIR canonicalize/cse pipeline failed\n";
    return 1;
  }

  return mlirModuleToObject(module.get(), arch, optLevel, objPath, err);
}

std::string mlirModuleToLlvmIrText(mlir::ModuleOp module,
                                   std::string& error) {
  error.clear();
  if (!module) {
    error = "null module";
    return {};
  }
  initMLIR();  // registers all MLIR translations (idempotent)
  mlir::registerLLVMDialectTranslation(*module.getContext());

  llvm::LLVMContext llvmCtx;
  std::unique_ptr<llvm::Module> llvmMod = mlir::translateModuleToLLVMIR(
      module.getOperation(), llvmCtx, "LLVMDialectModule");
  if (!llvmMod) {
    error = "MLIR -> LLVM IR translation failed";
    return {};
  }

  std::string out;
  llvm::raw_string_ostream os(out);
  llvmMod->print(os, nullptr);
  return out;
}

}  // namespace a
