#include "backend/kgen/mlir/kgen_mlir_codegen.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include "backend/kgen/codegen/llvm_emitter.hpp"
namespace a::kgen::mlir_backend {
bool available() { return true; }

static std::string tool(const char* name) {
  namespace fs = std::filesystem;
  std::string v = std::string("/usr/bin/") + name + "-18";
  if (fs::exists(v)) return v;
  v = std::string("/usr/lib/llvm-18/bin/") + name;
  if (fs::exists(v)) return v;
  return name;
}

bool emitLLVMIR(const ModuleGraph& g, const SemaResult& s, std::ostream& out, std::ostream& /*err*/) {
  return a::emitLLVMIR(g, s, out);
}

bool buildExecutable(const ModuleGraph& g, const SemaResult& s, const std::string& outputPath, std::ostream& err) {
  namespace fs = std::filesystem;
  fs::path tmp = fs::temp_directory_path() / ("a_kgen_" + std::to_string(::getpid()));
  std::string ll = tmp.string() + ".ll";
  std::string obj = tmp.string() + ".o";
  {
    std::ofstream ofs(ll);
    if (!ofs) { err << "error: cannot create temp IR\n"; return false; }
    if (!a::emitLLVMIR(g, s, ofs)) { err << "error: LLVM IR emission failed\n"; fs::remove(ll); return false; }
  }
  std::string cmd1 = tool("llc") + " -filetype=obj -relocation-model=pic -O2 " + ll + " -o " + obj + " 2>&1";
  FILE* p = popen(cmd1.c_str(), "r");
  std::string o1; char buf[512];
  if (p) { while (fgets(buf, sizeof(buf), p)) o1 += buf; int rc = pclose(p); if (rc != 0) { err << "error: KGEN object compilation (llc) failed\n" << o1; fs::remove(ll); fs::remove(obj); return false; } }
  else { err << "error: failed to invoke llc\n"; fs::remove(ll); return false; }
  std::string cmd2 = tool("clang") + " -O2 " + obj + " -o " + outputPath + " -lm 2>&1";
  p = popen(cmd2.c_str(), "r"); std::string o2;
  if (p) { while (fgets(buf, sizeof(buf), p)) o2 += buf; int rc = pclose(p); if (rc != 0) { err << "error: KGEN link failed\n" << o2; fs::remove(ll); fs::remove(obj); return false; } }
  else { err << "error: failed to invoke linker\n"; fs::remove(ll); fs::remove(obj); return false; }
  fs::remove(ll); fs::remove(obj);
  return true;
}

int runModule(const ModuleGraph& g, const SemaResult& s, std::istream& /*in*/, std::ostream& /*out*/, std::ostream& err) {
  namespace fs = std::filesystem;
  fs::path tmp = fs::temp_directory_path() / ("a_kgen_run_" + std::to_string(::getpid()));
  if (!buildExecutable(g, s, tmp.string(), err)) return 1;
  int rc = std::system(tmp.string().c_str());
  fs::remove(tmp);
  if (rc == -1) return 1;
  if (WIFEXITED(rc)) return WEXITSTATUS(rc);
  return 1;
}
}
