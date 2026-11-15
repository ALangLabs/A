//===- driver.cpp - `a mlir-opt` pass-debugging driver -------------*- C++ -*-===//
//
// Part of the A toolchain (src/astra/mlir). See docs/mlir-architecture.md.
//
// kgen-opt/mlir-opt style frontend for the A MLIR pass pipeline: loads a
// `.mlir` text module (or `.ambc` bytecode) into an MLIR context, applies
// either the default `a` pipeline or a user-specified pipeline parsed via
// `mlir::parsePassPipeline`, and prints the resulting IR. Optionally dumps
// LLVM IR text via the native bridge (mlirModuleToLlvmIrText).
//
//===----------------------------------------------------------------------===//

#include "driver.hpp"

#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include "mlir/Bytecode/BytecodeWriter.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Index/IR/IndexDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Support/FileUtilities.h"
#include "mlir/Transforms/Passes.h"

#include "bytecode.hpp"
#include "native.hpp"
#include "pipeline.hpp"

// The pipeline workstream provides pipeline.hpp (lands alongside this file)
// with registerAllDialects/registerAPasses/buildAOptPipeline. Include it
// directly; it only forward-declares MLIR types so it is cheap.

namespace a {

namespace {

// Registers the dialect set needed to parse `a` dialect modules plus the
// llvm-dialect backend path: func, llvm, cf, scf, arith, index, vector.
// `registerAllDialects` (pipeline.hpp) adds the `a` dialect and any others
// the pipeline requires. Idempotent.
void registerDriverDialects(mlir::MLIRContext &ctx) {
  mlir::DialectRegistry registry;
  registry.insert<mlir::func::FuncDialect, mlir::LLVM::LLVMDialect,
                  mlir::cf::ControlFlowDialect, mlir::scf::SCFDialect,
                  mlir::arith::ArithDialect, mlir::index::IndexDialect,
                  mlir::vector::VectorDialect>();
  ctx.appendDialectRegistry(registry);
  // `registerAllDialects` (pipeline.hpp) adds the `a` dialect and any others
  // the pipeline requires. It is idempotent on already-loaded dialects.
  a::registerAllDialects(ctx);
}

void usage(std::ostream &err) {
  err << "usage: a mlir-opt <file.mlir|file.ambc> [options] [passes...]\n"
      << "\n"
      << "options:\n"
      << "  --mlir-print-ir-before-all     print IR before each pass\n"
      << "  --mlir-print-ir-after-all      print IR after each pass\n"
      << "  --mlir-print-ir-before=<pass>  print IR before <pass>\n"
      << "  --mlir-print-ir-after=<pass>   print IR after <pass>\n"
      << "  --verify-each                  run the verifier after each pass\n"
      << "  --opt-level <0-3> | -O<n>      optimization level (default 2)\n"
      << "  --no-emit-llvm-dialect         keep `a` dialect (skip lowering)\n"
      << "  --emit-llvmir                  print LLVM IR text instead of MLIR\n"
      << "  --emit-bytecode                emit MLIR bytecode instead of text\n"
      << "  -o|--o <file>                  output file (default: stdout)\n"
      << "  --version                      print version and exit\n"
      << "  --help                         show this help\n"
      << "\n"
      << "passes:\n"
      << "  Positional arguments are joined and parsed as an MLIR pass\n"
      << "  pipeline (e.g. `cse canonicalize`, `func.func(cse)`, or a\n"
      << "  full `builtin.module(...)` string). With no passes, the default\n"
      << "  `a` pipeline (buildAOptPipeline) runs.\n";
}

// Reads an entire file into a string. Returns false on failure.
bool readFile(const std::string &path, std::string &contents) {
  auto buf = llvm::MemoryBuffer::getFile(path);
  if (!buf) {
    return false;
  }
  contents = (*buf)->getBuffer().str();
  return true;
}

// Writes `data` to `path`, or to `out` when path is empty.
bool emitOutput(const std::string &path, const std::string &data,
                std::ostream &out, std::ostream &err) {
  if (path.empty()) {
    out << data;
    if (!data.empty() && data.back() != '\n') out << '\n';
    return true;
  }
  std::ofstream os(path, std::ios::binary | std::ios::trunc);
  if (!os) {
    err << "error: mlir-opt: cannot open output file '" << path << "'\n";
    return false;
  }
  os << data;
  return static_cast<bool>(os);
}

// Returns the pass-pipeline string for the positional arguments, or "" when
// none were given (meaning: use the default a pipeline).
std::string joinPassArgs(const std::vector<std::string> &args) {
  if (args.empty()) return {};
  std::string pipeline;
  for (size_t i = 0; i < args.size(); ++i) {
    // MLIR pass-pipeline grammar separates nested pass elements with ','.
    if (i) pipeline += ",";
    pipeline += args[i];
  }
  return pipeline;
}

// mlir-opt accepts bare pass names like `cse canonicalize`; pm is an
// OpPassManager anchored at any-op, so top-level bare pass names are valid
// pipeline elements and pass through unchanged. Wrapping them in
// `builtin.module(...)` would look for builtin.module ops nested BELOW the
// root — the root itself is never matched, silently running nothing.
std::string normalizePipelineText(std::string pipeline) {
  return pipeline;
}

} // namespace

int runMlirOptCli(int argc, char **argv, std::ostream &out, std::ostream &err) {
  if (argc < 1) {
    usage(err);
    return 2;
  }

  std::string inputPath;
  std::string outputPath;
  std::vector<std::string> passArgs;
  bool printBeforeAll = false;
  bool printAfterAll = false;
  bool verifyEach = false;
  bool emitLlvmIr = false;
  bool emitBytecode = false;
  int optLevel = 2;
  bool emitLlvmDialect = true;
  bool showHelp = false;
  bool showVersion = false;
  std::string printBeforePass;
  std::string printAfterPass;

  for (int i = 0; i < argc; ++i) {
    std::string arg = argv[i];
    auto takeValue = [&](std::string &dst) -> bool {
      std::string eq;
      auto eqPos = arg.find('=');
      if (eqPos != std::string::npos) {
        dst = arg.substr(eqPos + 1);
        return true;
      }
      if (i + 1 >= argc) return false;
      dst = argv[++i];
      return true;
    };

    if (arg == "--help" || arg == "-h") {
      showHelp = true;
    } else if (arg == "--version") {
      showVersion = true;
    } else if (arg == "--mlir-print-ir-before-all") {
      printBeforeAll = true;
    } else if (arg == "--mlir-print-ir-after-all") {
      printAfterAll = true;
    } else if (arg.rfind("--mlir-print-ir-before=", 0) == 0) {
      printBeforePass = arg.substr(std::string("--mlir-print-ir-before=").size());
    } else if (arg.rfind("--mlir-print-ir-after=", 0) == 0) {
      printAfterPass = arg.substr(std::string("--mlir-print-ir-after=").size());
    } else if (arg == "--verify-each") {
      verifyEach = true;
    } else if (arg == "--opt-level" || arg.rfind("--opt-level=", 0) == 0) {
      std::string v;
      if (!takeValue(v)) {
        err << "error: mlir-opt: '" << arg << "' requires a value\n";
        return 2;
      }
      try {
        optLevel = std::stoi(v);
      } catch (...) {
        err << "error: mlir-opt: invalid --opt-level '" << v << "'\n";
        return 2;
      }
      if (optLevel < 0 || optLevel > 3) {
        err << "error: mlir-opt: --opt-level must be 0..3\n";
        return 2;
      }
    } else if ((arg.rfind("-O", 0) == 0 && arg.size() <= 3) ||
               arg == "-O") {
      // mlir-opt style -O0/-O1/-O2/-O3 (also accepts "-O 2").
      std::string v = arg.size() > 2 ? arg.substr(2) : "";
      if (v.empty()) {
        if (i + 1 >= argc || argv[i + 1][0] == '-') {
          err << "error: mlir-opt: '" << arg << "' requires a level 0..3\n";
          return 2;
        }
        v = argv[++i];
      }
      try {
        optLevel = std::stoi(v);
      } catch (...) {
        err << "error: mlir-opt: invalid optimization level '" << v << "'\n";
        return 2;
      }
      if (optLevel < 0 || optLevel > 3) {
        err << "error: mlir-opt: optimization level must be 0..3\n";
        return 2;
      }
    } else if (arg == "--no-emit-llvm-dialect" || arg == "--emit-a-dialect") {
      emitLlvmDialect = false;
    } else if (arg == "--emit-llvmir" || arg == "--mlir-to-llvmir") {
      emitLlvmIr = true;
    } else if (arg == "--emit-bytecode") {
      emitBytecode = true;
    } else if (arg == "-o" || arg == "--o" || arg.rfind("--o=", 0) == 0 ||
               arg.rfind("-o=", 0) == 0 || arg.rfind("-o", 0) == 0) {
      // -ofoo and -o=foo forms are also accepted for mlir-opt parity.
      std::string v;
      if (!takeValue(v)) {
        err << "error: mlir-opt: '" << arg << "' requires a value\n";
        return 2;
      }
      outputPath = v;
    } else if (!arg.empty() && arg[0] == '-' && arg != "-") {
      err << "error: mlir-opt: unknown option '" << arg << "'\n";
      usage(err);
      return 2;
    } else if (inputPath.empty()) {
      inputPath = arg;
    } else {
      passArgs.push_back(arg);
    }
  }

  if (showHelp) {
    usage(out);
    return 0;
  }
  if (showVersion) {
    out << "a mlir-opt (A toolchain) version " << a::kAmbcVersion << "\n";
    return 0;
  }
  if (inputPath.empty()) {
    usage(err);
    return 2;
  }

  // Load the input module. `.ambc` goes through the bytecode reader; anything
  // else is parsed as MLIR text.
  mlir::MLIRContext *ctxOwner = nullptr;
  std::unique_ptr<mlir::MLIRContext> ownedCtx;
  mlir::OwningOpRef<mlir::ModuleOp> module;

  bool isBytecode = inputPath.size() >= 5 &&
                    inputPath.compare(inputPath.size() - 5, 5, ".ambc") == 0;

  if (isBytecode) {
    // Bytecode needs a context with every dialect the payload may use;
    // registerDriverDialects covers the core set and the `a` dialect.
    ownedCtx = std::make_unique<mlir::MLIRContext>();
    ctxOwner = ownedCtx.get();
    registerDriverDialects(*ctxOwner);
    std::string bcError;
    module = readModuleBytecode(*ctxOwner, inputPath, bcError);
    if (!module) {
      // Fall back to text parse in case the file is actually .mlir text
      // with an .ambc name.
      std::string text;
      if (readFile(inputPath, text)) {
        std::string parseError;
        module = parseMlirText(*ctxOwner, text, parseError);
        if (!module) {
          err << "error: mlir-opt: cannot read '" << inputPath
              << "': " << (bcError.empty() ? parseError : bcError) << "\n";
          return 1;
        }
      } else {
        err << "error: mlir-opt: cannot read '" << inputPath << "': "
            << (bcError.empty() ? "unknown error" : bcError) << "\n";
        return 1;
      }
    }
  } else {
    ctxOwner = &sharedMlirContext();
    registerCoreDialects(*ctxOwner);  // idempotent; covers core dialects
    // `a` dialect may not be in sharedMlirContext's registry; ensure it is.
    // registerAllDialects is safe to call on an already-populated context.
    registerDriverDialects(*ctxOwner);
    std::string text;
    if (!readFile(inputPath, text)) {
      err << "error: mlir-opt: cannot read input file '" << inputPath << "'\n";
      return 1;
    }
    std::string parseError;
    module = parseMlirText(*ctxOwner, text, parseError);
    if (!module) {
      err << "error: mlir-opt: failed to parse '" << inputPath << "'"
          << (parseError.empty() ? "" : ": " + parseError) << "\n";
      return 1;
    }
  }

  mlir::ModuleOp moduleOp = module.get();

  // --emit-llvmir: translate straight to LLVM IR text and skip the pass
  // pipeline entirely (equivalent of `mlir-translate --mlir-to-llvmir`).
  if (emitLlvmIr) {
    std::string llvmError;
    std::string llvmIr = mlirModuleToLlvmIrText(moduleOp, llvmError);
    if (llvmIr.empty()) {
      err << "error: mlir-opt: LLVM IR translation failed: "
          << (llvmError.empty() ? "unknown error" : llvmError) << "\n";
      return 1;
    }
    return emitOutput(outputPath, llvmIr, out, err) ? 0 : 1;
  }

  mlir::PassManager pm(ctxOwner);
  if (verifyEach) pm.enableVerifier(true);

  // IR-printing instrumentation (mlir-opt style). --mlir-print-ir-{before,
  // after}-all print around every pass; --mlir-print-ir-{before,after}=<pass>
  // filters by pass argument name (comma-separated args also match).
  auto passNameMatches = [](mlir::Pass *pass, llvm::StringRef wanted) {
    llvm::StringRef arg = pass->getArgument();
    if (arg.empty()) arg = pass->getName();
    if (arg == wanted ||
        (arg.starts_with(wanted) && arg.drop_front(wanted.size()).starts_with(",")))
      return true;
    for (llvm::StringRef seg : llvm::split(arg, ',')) {
      seg = seg.trim();
      if (seg == wanted ||
          (seg.starts_with(wanted) && seg.drop_front(wanted.size()).starts_with("(")))
        return true;
    }
    return false;
  };

  bool anyPrint = printBeforeAll || printAfterAll || !printBeforePass.empty() ||
                  !printAfterPass.empty();
  if (anyPrint) {
    std::string before = printBeforePass;
    std::string after = printAfterPass;
    auto shouldBefore = [=](mlir::Pass *p, mlir::Operation *) {
      if (printBeforeAll) return true;
      return !before.empty() && passNameMatches(p, before);
    };
    auto shouldAfter = [=](mlir::Pass *p, mlir::Operation *) {
      if (printAfterAll) return true;
      return !after.empty() && passNameMatches(p, after);
    };
    // Route the dumps through errs() so stdout stays clean for -o / piping.
    pm.enableIRPrinting(std::move(shouldBefore), std::move(shouldAfter),
                        /*printModuleScope=*/true,
                        /*printAfterOnlyOnChange=*/false,
                        /*printAfterOnlyOnFailure=*/false, llvm::errs());
  }

  a::registerAPasses();
  // Stock MLIR transform passes (cse, canonicalizer, mem2reg, ...) must be
  // registered before parsePassPipeline can resolve bare pass names.
  mlir::registerTransformsPasses();

  std::string pipelineText = joinPassArgs(passArgs);
  if (pipelineText.empty()) {
    // Default: the `a` pipeline (canonicalizer + a-lower-to-llvm + ...).
    a::buildAOptPipeline(pm, optLevel, emitLlvmDialect);
  } else {
    std::string normalized = normalizePipelineText(pipelineText);
    // Parse errors go to a raw_ostream; capture into a buffer so we can
    // route them through `err`.
    std::string parseErrors;
    llvm::raw_string_ostream errStream(parseErrors);
    mlir::LogicalResult parseResult =
        mlir::parsePassPipeline(normalized, pm, errStream);
    errStream.flush();
    if (mlir::failed(parseResult)) {
      err << "error: mlir-opt: invalid pass pipeline '" << pipelineText << "'"
          << (parseErrors.empty() ? "" : ": " + parseErrors) << "\n";
      return 2;
    }
  }

  if (failed(pm.run(moduleOp.getOperation()))) {
    err << "error: mlir-opt: pass pipeline failed\n";
    return 1;
  }

  if (emitBytecode) {
    if (outputPath.empty()) {
      err << "error: mlir-opt: --emit-bytecode requires -o/--o <file>\n";
      return 2;
    }
    std::string bcError;
    if (!writeModuleBytecode(moduleOp, outputPath, bcError)) {
      err << "error: mlir-opt: bytecode write failed: " << bcError << "\n";
      return 1;
    }
    return 0;
  }

  std::string printed;
  llvm::raw_string_ostream ros(printed);
  moduleOp.print(ros);
  ros.flush();
  return emitOutput(outputPath, printed, out, err) ? 0 : 1;
}

} // namespace a
