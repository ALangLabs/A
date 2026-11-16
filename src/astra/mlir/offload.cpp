// GPU offload extraction (Mojo-style compile_offload).
//
// The frontend stamps kernel/device functions with an `a.offload`
// attribute on their llvm.func. Before the host module is translated to
// LLVM IR, this pass:
//   1. clones the module and compiles the clone for the device target
//      (NVPTX -> PTX text, AMDGPU -> code object) via the same
//      MLIR -> LLVM IR -> TargetMachine path used for host code;
//   2. erases every non-offload function from the device LLVM module so
//      host/runtime calls can never leak into device code;
//   3. writes the device blob plus a generated `.offload.c` registration
//      unit next to the host object. linkWithRuntime compiles and links
//      that unit, so kernels self-register through a constructor before
//      main runs — no toolkit or GPU needed at build time.
//
// Device target selection: A_GPU_TARGET (default nvptx64-nvidia-cuda),
// A_GPU_CPU (default sm_60 / gfx900), A_GPU_ATTRS (feature string).
// A_GPU_OFFLOAD=0 disables extraction entirely.

#include "codegen.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Target/LLVMIR/Export.h"

namespace a {
namespace {

struct OffloadFn {
  std::string sym;   // llvm.func symbol (e.g. __a_tc___a_f_m0_add_3)
  std::string kind;  // "kernel" | "device"
  int nparams = 0;
};

// The `a.offload` attribute may live in the op's attribute dict or inside
// LLVMFuncOp's `passthrough` array (LLVM IR attribute groups round-trip
// through passthrough on some MLIR versions).
std::string offloadKindOf(mlir::LLVM::LLVMFuncOp fn) {
  if (auto sa = fn->getAttrOfType<mlir::StringAttr>("a.offload"))
    return sa.getValue().str();
  if (auto pt = fn->getAttrOfType<mlir::ArrayAttr>("passthrough")) {
    for (mlir::Attribute el : pt) {
      auto da = mlir::dyn_cast<mlir::DictionaryAttr>(el);
      if (!da) continue;
      if (auto sa = da.getAs<mlir::StringAttr>("a.offload"))
        return sa.getValue().str();
    }
  }
  return "";
}

int nparamsOf(mlir::LLVM::LLVMFuncOp fn) {
  auto ft = fn.getFunctionType();
  if (!ft) return 0;
  // LLVMFunctionType wraps an llvm::FunctionType; getNumInputs mirrors it.
  return (int)ft.getNumInputs();
}

// Recover the A-level function name from the mangled core symbol:
// "__a_tc___a_f_m0_vecadd_3" -> "vecadd".
std::string prettyName(const std::string& sym) {
  std::string s = sym;
  if (s.rfind("__a_tc_", 0) == 0) s = s.substr(6);
  if (s.rfind("__a_f_m", 0) == 0) {
    std::size_t i = 6;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
    if (i < s.size() && s[i] == '_') s = s.substr(i + 1);
  }
  std::size_t us = s.find_last_of('_');
  if (us != std::string::npos && us + 1 < s.size()) {
    bool digits = true;
    for (std::size_t i = us + 1; i < s.size(); ++i)
      if (!std::isdigit((unsigned char)s[i])) { digits = false; break; }
    if (digits) s = s.substr(0, us);
  }
  return s.empty() ? sym : s;
}

bool writeFile(const std::string& path, const std::string& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(data.data(), (std::streamsize)data.size());
  return (bool)f;
}

std::string deviceTriple() {
  if (const char* e = std::getenv("A_GPU_TARGET"); e && *e) return e;
  return "nvptx64-nvidia-cuda";
}

std::string deviceCPU(const llvm::Triple& t) {
  if (const char* e = std::getenv("A_GPU_CPU"); e && *e) return e;
  switch (t.getArch()) {
    case llvm::Triple::NVPTX: return "sm_60";
    case llvm::Triple::AMDGPU: return "gfx900";
    default: return "";
  }
}

}  // namespace

int prepareGpuOffload(mlir::ModuleOp module, const std::string& objPath,
                      int optLevelArg, std::ostream& err) {
  if (!module) return 0;
  if (const char* e = std::getenv("A_GPU_OFFLOAD"); e && e[0] == '0')
    return 0;

  std::vector<OffloadFn> offloads;
  for (auto fn : module.getOps<mlir::LLVM::LLVMFuncOp>()) {
    std::string k = offloadKindOf(fn);
    if (k != "kernel" && k != "device") continue;
    OffloadFn o;
    o.sym = fn.getSymName().str();
    o.kind = std::move(k);
    o.nparams = nparamsOf(fn);
    offloads.push_back(std::move(o));
  }
  if (offloads.empty()) return 0;

  initMLIRBackend();

  // Clone the whole module for the device: host func bodies are stripped
  // after LLVM IR translation (cheap llvm::Function::eraseFromParent).
  mlir::Operation* clonedOp = module->clone();
  mlir::ModuleOp devModule = mlir::cast<mlir::ModuleOp>(clonedOp);

  llvm::Triple triple(deviceTriple());
  const llvm::Target* target =
      llvm::TargetRegistry::lookupTarget(triple.normalize(), err);
  if (!target) {
    err << "error: gpu offload: no LLVM target for '" << triple.normalize()
        << "' (supported: nvptx64-*, amdgcn-*)\n";
    return 1;
  }

  int kind = 0;  // 1 = PTX text, 2 = code object
  llvm::CodeGenFileType fileType;
  switch (triple.getArch()) {
    case llvm::Triple::NVPTX:
      kind = 1;
      fileType = llvm::CodeGenFileType::AssemblyFile;
      break;
    case llvm::Triple::AMDGPU:
      kind = 2;
      fileType = llvm::CodeGenFileType::ObjectFile;
      break;
    default:
      err << "error: gpu offload: unsupported device target '"
          << triple.normalize()
          << "' (supported: nvptx64-nvidia-cuda, amdgcn-amd-amdhsa)\n";
      return 1;
  }

  mlir::MLIRContext* ctx = module.getContext();
  mlir::registerBuiltinDialectTranslation(*ctx);
  mlir::registerLLVMDialectTranslation(*ctx);

  llvm::LLVMContext llvmCtx;
  std::unique_ptr<llvm::Module> devLLVM =
      mlir::translateModuleToLLVMIR(devModule.getOperation(), llvmCtx,
                                    "AOffloadModule");
  if (!devLLVM) {
    err << "error: gpu offload: MLIR -> LLVM IR translation failed\n";
    return 1;
  }

  // Strip every non-offload function: host and runtime calls must never
  // reach device code. Declarations stay (they are dropped by codegen when
  // unreferenced).
  std::unordered_set<std::string> keep;
  for (const OffloadFn& o : offloads) keep.insert(o.sym);
  for (llvm::Function& fn : devLLVM->functions()) {
    if (fn.isDeclaration()) continue;
    if (keep.count(fn.getName().str())) continue;
    if (!fn.use_empty())
      fn.replaceAllUsesWith(llvm::UndefValue::get(fn.getType()));
    fn.eraseFromParent();
  }

  devLLVM->setTargetTriple(triple.normalize());

  int level = optLevelArg;
  if (const char* e = std::getenv("A_MLIR_OPT")) {
    try { level = std::stoi(e); } catch (...) {}
  }
  level = std::max(0, std::min(3, level));

  // Same opt pipeline as the host path.
  {
    llvm::PassBuilder pb;
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
        pb.buildPerModuleDefaultPipeline(
            level == 0   ? llvm::OptimizationLevel::O0
            : level == 1 ? llvm::OptimizationLevel::O1
            : level == 3 ? llvm::OptimizationLevel::O3
                         : llvm::OptimizationLevel::O2);
    mpm.run(*devLLVM, mam);
  }

  const std::string cpu = deviceCPU(triple);
  const std::string feats =
      (const char* e = std::getenv("A_GPU_ATTRS"); e && *e) ? e : "";

  llvm::TargetOptions topts;
  topts.AllowFPOpFusion = llvm::FPOpFusion::Fast;
  std::optional<llvm::Reloc::Model> rm = llvm::Reloc::Static;
  std::optional<llvm::CodeModel::Model> cm = llvm::CodeModel::Small;
  std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
      triple.normalize(), cpu, feats, topts, rm, cm,
      level == 0   ? llvm::CodeGenOptLevel::None
      : level == 1 ? llvm::CodeGenOptLevel::Less
      : level == 3 ? llvm::CodeGenOptLevel::Aggressive
                   : llvm::CodeGenOptLevel::Default,
      /*JIT=*/false));
  if (!tm) {
    err << "error: gpu offload: device target machine creation failed for '"
        << triple.normalize() << "'\n";
    return 1;
  }
  if (devLLVM->getDataLayout().isDefault())
    devLLVM->setDataLayout(tm->createDataLayout());

  std::string blob;
  {
    llvm::raw_string_ostream os(blob);
    llvm::legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, os, nullptr, fileType)) {
      err << "error: gpu offload: device target cannot emit "
          << (kind == 1 ? "assembly" : "object") << " for '"
          << triple.normalize() << "'\n";
      return 1;
    }
    pm.run(*devLLVM);
    os.flush();
  }
  if (blob.empty()) {
    err << "error: gpu offload: device emission produced no output\n";
    return 1;
  }

  // Remove offload funcs from the HOST module — they must not reach host
  // codegen (no thunk exists for host callers; sema blocks direct calls).
  llvm::SmallVector<mlir::Operation*> toErase;
  for (auto fn : module.getOps<mlir::LLVM::LLVMFuncOp>()) {
    std::string k = offloadKindOf(fn);
    if (k == "kernel" || k == "device") toErase.push_back(fn);
  }
  for (mlir::Operation* op : toErase) op->erase();

  // Write the blob + generated registration unit next to the host object.
  const std::string blobPath = objPath + ".offload.bin";
  const std::string cPath = objPath + ".offload.c";
  if (!writeFile(blobPath, blob)) {
    err << "error: gpu offload: cannot write '" << blobPath << "'\n";
    return 1;
  }

  std::string c;
  c.reserve(blob.size() + offloads.size() * 96 + 512);
  c += "/* Generated by a: GPU offload module registration (Mojo-style "
       "compile_offload). */\n";
  c += "static const char __a_offblob_0[] = \"";
  // Chunk the literal so no single line gets pathological.
  {
    std::size_t col = 0;
    for (unsigned char ch : blob) {
      std::string esc;
      switch (ch) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
          if (ch >= 32 && ch < 127) esc = std::string(1, (char)ch);
          else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\%03o", (int)ch);
            esc = buf;
          }
      }
      c += esc;
      col += esc.size();
      if (col > 72) { c += "\"\n\""; col = 0; }
    }
  }
  c += "\";\n\n";
  c += "extern long a_gpu_module_load(long kind, const void *data, long len);\n";
  c += "extern long a_gpu_register_kernel(long mod, const char *name, "
       "const char *ksym, long nparams);\n\n";
  c += "__attribute__((constructor)) static void __a_gpu_offload_ctor(void) {\n";
  c += "  long m0 = a_gpu_module_load(" + std::to_string(kind) +
       ", __a_offblob_0, (long)sizeof(__a_offblob_0) - 1);\n";
  auto appendEscaped = [&](const std::string& s) {
    for (char ch : s) {
      if (ch == '"' || ch == '\\') c += '\\';
      c += ch;
    }
  };
  for (const OffloadFn& o : offloads) {
    if (o.kind != "kernel") continue;
    c += "  a_gpu_register_kernel(m0, \"";
    appendEscaped(prettyName(o.sym));
    c += "\", \"";
    appendEscaped(o.sym);
    c += "\", " + std::to_string(o.nparams) + ");\n";
  }
  c += "}\n";

  if (!writeFile(cPath, c)) {
    err << "error: gpu offload: cannot write '" << cPath << "'\n";
    return 1;
  }
  return 0;
}

}  // namespace a
