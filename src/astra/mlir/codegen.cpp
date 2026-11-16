// In-process MLIR -> LLVM IR -> object codegen.
//
// Pipeline (replaces the subprocess chain in pipeline.cpp):
//   mlir::ModuleOp (llvm dialect)
//     -> mlir::translateModuleToLLVMIR            (mlir-translate --mlir-to-llvmir)
//     -> llvm::PassBuilder::buildPerModuleDefaultPipeline  (opt -O<n>)
//     -> llvm::TargetMachine::addPassesToEmitFile         (llc -filetype=obj)
//   final link still goes through the clang driver (linkWithRuntime).
//
// Host-only, mirroring llvmBuild: CPU/features come from
// llvm::sys::getHostCPUName/getHostCPUFeatures (-mcpu=native), opt and codegen
// share the same opt level, and -fp-contract=fast is applied for parity with
// the llc invocation.

#include "codegen.hpp"

#include "frontend/toolchain_tools.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/InitAllTranslations.h"

#ifndef _WIN32
#include <sys/types.h>
#include <unistd.h>
#endif

namespace a {
namespace {

// ---- init -----------------------------------------------------------------

void doInitMLIR() {
  // Registers all dialect->LLVM IR translation interfaces (llvm dialect,
  // builtin dialect, etc.) for subsequently created MLIRContexts.
  // Must run before any MLIRContext that will use translateModuleToLLVMIR
  // is created; existing contexts additionally get the per-context
  // registration in mlirModuleToObject.
  mlir::registerAllTranslations();
}

void doInitLLVM() {
  // Native target + asm printer/parser + MC for the host (the backend is
  // host-only, like the llc subprocess it replaces).
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();
}

// ---- helpers ---------------------------------------------------------------

// The `arch` argument maps to a triple: empty / "host" / "native" select the
// process triple (llc -mcpu=native analog); anything else is passed through
// to TargetRegistry (host-only callers reject non-host triples earlier).
llvm::Triple resolveTriple(const std::string& arch) {
  std::string t = arch;
  if (t.empty() || t == "host" || t == "native") {
    return llvm::Triple(llvm::sys::getProcessTriple());
  }
  // Accept bare arch names like "x86_64" by completing to the host triple.
  llvm::Triple raw(t);
  if (raw.getArch() != llvm::Triple::UnknownArch) {
    llvm::Triple host(llvm::sys::getProcessTriple());
    if (raw.getArchName().find('-') == std::string::npos &&
        raw.getOS() == llvm::Triple::UnknownOS) {
      raw.setVendor(host.getVendor());
      raw.setOS(host.getOS());
      raw.setEnvironment(host.getEnvironment());
    }
    return raw;
  }
  return llvm::Triple(t);
}

// CPU + feature string, mirroring llc's defaults: -mcpu=native resolves to
// the host CPU/features via TargetParser. Env overrides A_LLVM_CPU / A_LLVM_ATTRS
// match the A_LLVM_* tool overrides used by the subprocess backend.
std::string hostCPU() {
  if (const char* e = std::getenv("A_LLVM_CPU"); e && *e) return e;
  llvm::StringRef cpu = llvm::sys::getHostCPUName();
  return cpu.str();
}

std::string hostFeatures() {
  if (const char* e = std::getenv("A_LLVM_ATTRS"); e && *e) return e;
  llvm::StringMap<bool, llvm::MallocAllocator> feats;
  if (!llvm::sys::getHostCPUFeatures(feats)) return "";
  llvm::SmallVector<std::string> on, off;
  for (auto& kv : feats) {
    // Enabled features must carry an explicit '+' — createTargetMachine
    // parses bare names as disables (mirrors llc's SubtargetFeatures
    // convention; e.g. bare "64bit" would clear HasX86_64 and abort codegen).
    if (kv.second) on.push_back("+" + kv.first().str());
    else off.push_back("-" + kv.first().str());
  }
  std::sort(on.begin(), on.end());
  std::sort(off.begin(), off.end());
  std::string out;
  auto append = [&](const llvm::SmallVectorImpl<std::string>& v) {
    for (const auto& f : v) {
      if (!out.empty()) out += ',';
      out += f;
    }
  };
  append(on);
  append(off);
  return out;
}

llvm::CodeGenOptLevel codeGenOptLevel(int level) {
  switch (level) {
    case 0: return llvm::CodeGenOptLevel::None;
    case 1: return llvm::CodeGenOptLevel::Less;
    case 3: return llvm::CodeGenOptLevel::Aggressive;
    default: return llvm::CodeGenOptLevel::Default;
  }
}

llvm::OptimizationLevel optLevel(int level) {
  switch (level) {
    case 0: return llvm::OptimizationLevel::O0;
    case 1: return llvm::OptimizationLevel::O1;
    case 3: return llvm::OptimizationLevel::O3;
    default: return llvm::OptimizationLevel::O2;
  }
}

}  // namespace

void initMLIR() {
  static std::once_flag flag;
  std::call_once(flag, doInitMLIR);
}

void initLLVM() {
  static std::once_flag flag;
  std::call_once(flag, doInitLLVM);
}

void initMLIRBackend() {
  initMLIR();
  initLLVM();
}

int mlirModuleToObject(mlir::ModuleOp module, const std::string& arch,
                       int optLevelArg, const std::string& objPath,
                       std::ostream& err) {
  if (!module) {
    err << "error: mlir backend: null module\n";
    return 1;
  }
  initMLIRBackend();

  int level = optLevelArg;
  if (const char* e = std::getenv("A_MLIR_OPT")) {
    try { level = std::stoi(e); } catch (...) { /* keep */ }
  }
  level = std::max(0, std::min(3, level));

  // (a) llvm-dialect MLIR -> LLVM IR module.
  mlir::MLIRContext* ctx = module.getContext();
  // Ensure the builtin + llvm dialect translation interfaces are registered
  // on this context (initMLIR already ran registerAllTranslations globally;
  // this is belt-and-braces for contexts created before initMLIR).
  mlir::registerBuiltinDialectTranslation(*ctx);
  mlir::registerLLVMDialectTranslation(*ctx);

  llvm::LLVMContext llvmCtx;
  llvm::StringRef modName = "LLVMDialectModule";
  std::string modNameStorage;
  if (auto name = module.getName()) {
    modNameStorage = (*name).str();
    if (!modNameStorage.empty()) modName = modNameStorage;
  }
  std::unique_ptr<llvm::Module> llvmMod =
      mlir::translateModuleToLLVMIR(module.getOperation(), llvmCtx, modName);
  if (!llvmMod) {
    err << "error: mlir backend: MLIR -> LLVM IR translation failed\n";
    return 1;
  }

  // The a_*_fast fast-path helpers are emitted as llvm.func definitions
  // (fastpath_mlir.hpp) precisely so their NaN-box tag checks fold into
  // callers. Their multi-block bodies exceed the default inline cost model,
  // so force alwaysinline; linkonce_odr still lets fully-inlined copies be
  // discarded.
  for (llvm::Function& fn : llvmMod->functions()) {
    llvm::StringRef nm = fn.getName();
    if (nm.starts_with("a_") && nm.ends_with("_fast"))
      fn.addFnAttr(llvm::Attribute::AlwaysInline);
  }

  // DIAGNOSTIC (temporary): A_MLIR_DUMP_OPT_IR=<path> writes post-opt LLVM IR.
  if (const char* dop = std::getenv("A_MLIR_DUMP_OPT_IR"); dop && *dop) {
    std::error_code oec;
    llvm::raw_fd_ostream od(dop, oec);
    if (!oec) {
      llvmMod->print(od, nullptr);
      od.flush();
    }
  }

  if (const char* e0 = std::getenv("A_MLIR_DUMP_OPT0_IR")) { std::string e; llvm::raw_string_ostream os3(e); llvmMod->print(os3, nullptr); FILE* f = std::fopen(e0, "wb"); if (f) { std::fwrite(e.data(), 1, e.size(), f); std::fclose(f); } }

  // Set the triple/datalayout explicitly: emitted modules do not carry
  // llvm.target_triple / llvm.data_layout attributes.
  llvm::Triple triple = resolveTriple(arch);
  llvmMod->setTargetTriple(triple.normalize());

  // (b) opt pipeline in-process: opt -O<n> -mcpu=native.
  // The module pass manager must outlive the run; the analysis manager is
  // owned by the pass manager via the ModuleAnalysisManager parameter.
  if (level > 0) {
    llvm::PassBuilder pb;
    // Register all standard analyses + instrumentation callbacks so the
    // pipeline's PassInstrumentationAnalysis queries succeed (same setup
    // `opt` performs before running the per-module pipeline).
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    pb.registerLoopAnalyses(lam);
    pb.registerFunctionAnalyses(fam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerModuleAnalyses(mam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    llvm::ModulePassManager mpm =
        pb.buildPerModuleDefaultPipeline(optLevel(level));
    mpm.run(*llvmMod, mam);
  }

  // (c) TargetMachine -> .o (llc -filetype=obj -mcpu=native -fp-contract=fast).
  const std::string cpu = hostCPU();
  const std::string feats = hostFeatures();

  // Mirror llc -fp-contract=fast (FPOpFusionMode::Fast fuses mul+add into
  // FMA, same contraction clang applies to C by default).
  llvm::TargetOptions topts;
  topts.AllowFPOpFusion = llvm::FPOpFusion::Fast;

  // llc defaults: static relocations, small code model, non-JIT codegen.
  std::optional<llvm::Reloc::Model> rm = llvm::codegen::getExplicitRelocModel();
  if (!rm) rm = llvm::Reloc::Static;
  std::optional<llvm::CodeModel::Model> cm =
      llvm::codegen::getExplicitCodeModel();
  if (!cm) cm = llvm::CodeModel::Small;

  std::string targetErr;
  const llvm::Target* target =
      llvm::TargetRegistry::lookupTarget(triple.normalize(), targetErr);
  if (!target) {
    err << "error: mlir backend: no target for triple '"
        << triple.normalize() << "': " << targetErr << "\n";
    return 1;
  }
  std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
      triple.normalize(), cpu, feats, topts, rm, cm,
      codeGenOptLevel(level), /*JIT=*/false));
  if (!tm) {
    err << "error: mlir backend: target machine creation failed for triple '"
        << triple.normalize() << "'\n";
    return 1;
  }

  // The translator only populates the datalayout when the MLIR module
  // carries llvm.data_layout; emitted modules do not, and codegen needs a
  // real 64-bit layout (an empty/default layout makes the x86 subtarget
  // treat pointers as non-64-bit and abort). Fall back to the target
  // machine's layout, but keep an explicitly translated one.
  if (llvmMod->getDataLayout().isDefault()) {
    llvmMod->setDataLayout(tm->createDataLayout());
  }

  std::error_code ec;
  std::filesystem::path objP(objPath);
  if (objP.has_parent_path()) {
    std::filesystem::create_directories(objP.parent_path(), ec);
  }
  llvm::raw_fd_ostream dest(objPath, ec);
  if (ec) {
    err << "error: mlir backend: cannot open '" << objPath << "': "
        << ec.message() << "\n";
    return 1;
  }

  llvm::legacy::PassManager pm;
  if (tm->addPassesToEmitFile(pm, dest, nullptr,
                              llvm::CodeGenFileType::ObjectFile)) {
    err << "error: mlir backend: target does not support object emission\n";
    return 1;
  }
  pm.run(*llvmMod);
  dest.flush();
  if (dest.has_error()) {
    err << "error: mlir backend: failed writing '" << objPath << "'\n";
    return 1;
  }
  return 0;
}

namespace {

// Locate clang/clang++/cc candidates, mirroring findLLVMTool("A_LLVM_CC", ...)
// in pipeline.cpp: env override, bundled llvm-tools/bin dirs, then PATH.
// The probe base is derived from `anchor` — the resolved libaruntime.a path —
// since staged layouts keep clang next to the runtime (bin/../llvm-tools,
// bin/, astralib/..). Returns every viable driver in priority order: a
// bundled tool that cannot actually run (missing shared libs on the target
// machine) is skipped at link time in favour of the next candidate.
std::vector<std::string> findClangDrivers(const std::string& anchor) {
  // clang first (it is what the opt/llc pipeline was designed against), then
  // the system C compiler: the final step is a plain .o + libaruntime.a link,
  // which gcc/cc handle identically, so release installs work on machines
  // with no LLVM toolchain at all.
  static const char* kNames[] = {
      "clang", "clang-18", "clang-17", "clang-16", "clang-15",
      "clang++", "clang++-18", "clang++-17",
      "cc", "gcc", "c++", "g++"};
  if (const char* e = std::getenv("A_LLVM_CC"); e && *e) return {e};
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  auto add = [&](const std::string& cand) {
    if (cand.empty() || !seen.insert(cand).second) return;
    out.push_back(cand);
  };
  auto tryDir = [&](const std::string& dir) {
    // One driver per directory is enough; shared probe keeps the
    // exists()+X_OK semantics identical to pipeline.cpp's findLLVMTool.
    std::string cand = a::toolchain::findToolInDir(dir, kNames);
    if (!cand.empty()) add(cand);
  };
  // Bundled toolchain next to the runtime archive: libaruntime.a lives in
  // <root>/astralib[-arch]/ or <root>/bin/../astralib; clang ships under
  // llvm-tools/ or bin/ of the same root.
  {
    std::filesystem::path p(anchor);
    std::string dir = p.parent_path().string();          // .../astralib[-arch]
    std::string root = std::filesystem::path(dir).parent_path().string();
    std::vector<std::string> bases = {
        root + "/llvm-tools", root + "/bin", root,
        dir + "/llvm-tools", dir + "/bin", dir,
        // src/astra/runtime/libaruntime.a -> repo root is ../../..
        std::filesystem::path(dir).parent_path().parent_path().parent_path()
            .string() + "/llvm-tools",
    };
    for (const auto& base : bases) {
      if (base.empty() || base == ".") continue;
      tryDir(base);
    }
  }
  // Fall back to the directory of the running executable (covers layouts
  // where the driver sits next to `a` itself).
  {
    std::error_code ec;
    std::filesystem::path self =
        std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
      std::string dir = self.parent_path().string();
      for (const std::string& base :
           {dir + "/llvm-tools", dir + "/bin", dir}) {
        tryDir(base);
      }
    }
  }
  for (const std::string& dir : a::toolchain::pathEntries()) {
    tryDir(dir);
  }
  return out;
}

}  // namespace

namespace {

// Drivers that actually execute, in probe order. `--version` is used as a
// viability check because a bundled LLVM tool can be present yet unusable on
// the target machine (missing libclang-cpp/libLLVM shared libraries); the
// result is cached per runtime-archive anchor.
std::vector<std::string> viableLinkDrivers(const std::string& anchor) {
  static std::unordered_map<std::string, std::vector<std::string>> cache;
  auto it = cache.find(anchor);
  if (it != cache.end()) return it->second;
  std::vector<std::string> viable;
  for (const auto& c : findClangDrivers(anchor)) {
    // Viability probe: discard the child's stdout so the `clang --version`
    // banner cannot leak into the compiled program's stdout; stderr stays
    // inherited so loader/driver diagnostics still reach the user.
    if (a::toolchain::runToolProcess(c, {"--version"},
                                     /*discardStdout=*/true) == 0)
      viable.push_back(c);
  }
  cache.emplace(anchor, viable);
  return viable;
}

}  // namespace

int linkWithRuntime(const std::string& objPath, const std::string& outputPath,
                    const std::string& runtimeLib, std::ostream& err) {
#ifdef _WIN32
  (void)objPath; (void)outputPath; (void)runtimeLib;
  err << "error: llvm backend is not supported on Windows yet\n";
  return 1;
#else
  const auto drivers = viableLinkDrivers(runtimeLib);
  if (drivers.empty()) {
    err << "error: llvm backend: no clang driver found (set A_LLVM_CC)\n";
    return 1;
  }
  std::error_code ec;
  if (runtimeLib.empty() || !std::filesystem::exists(runtimeLib, ec)) {
    err << "error: llvm backend: cannot locate libaruntime.a\n";
    return 1;
  }
  // llc emits a non-PIC object; -no-pie keeps the link consistent with it.
  std::vector<std::string> args =
      a::toolchain::runtimeLinkArgs(objPath, runtimeLib, outputPath);
  // mold is dramatically faster than ld/lld for the final link; use it when
  // present and retry with the default linker if it fails (mirrors llvmBuild).
  const bool useMold = a::toolchain::moldAvailable();
  for (const auto& cc : drivers) {
    if (useMold) {
      if (a::toolchain::runToolProcess(cc, a::toolchain::moldLinkArgs(args))
              == 0)
        return 0;
      // mold missing/broken: fall through to the default linker.
    }
    if (a::toolchain::runToolProcess(cc, args) == 0) return 0;
  }
  err << "error: llvm backend: clang link failed\n";
  return 1;
#endif
}

}  // namespace a
