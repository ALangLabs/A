// Builtin native backend — the ONLY native compiler for A.
// Path: A frontend/sema -> emitCSource() -> Astra
//       (compiler/astra/) -> EXE output -> standalone binary
//       (ELF/PE/Mach-O).
//
// This replaces the old `system("gcc -O2 -no-pie ...")` call in
// llvm_emitter.cpp entirely. Linker + codegen are in-process.
#include "backend/kgen/native_backend.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <string>

#include <unistd.h>

#include "libtcc.h"
#include "backend/kgen/codegen/llvm_emitter.hpp"

namespace a::kgen {
namespace {

// Where the staged runtime lives (same discovery as jit.cpp).
std::string nativeLibPath() {
  if (const char* e = std::getenv("A_TCCLIB")) return e;
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0) return "";
  exe[n] = '\0';
  std::string dir = exe;
  auto pos = dir.find_last_of('/');
  if (pos == std::string::npos) return "";
  dir = dir.substr(0, pos) + "/tcclib";
  std::string probe = dir + "/include/tccdefs.h";
  if (FILE* f = std::fopen(probe.c_str(), "rb")) {
    std::fclose(f);
    return dir;
  }
  return "";
}

void nativeDiag(void* opaque, const char* msg) {
  auto* s = static_cast<std::string*>(opaque);
  *s += msg;
  *s += '\n';
}

}  // namespace

bool buildExecutableWithNative(const ModuleGraph& graph, const SemaResult& sema,
                               const std::string& outputPath, std::ostream& err) {
  std::string libpath = nativeLibPath();
  if (libpath.empty()) {
    err << "error: builtin runtime (tcclib/) not found next to the compiler binary\n"
        << "  hint: build the compiler first (cmake --build build) or set A_TCCLIB\n";
    return false;
  }

  std::ostringstream cs;
  if (!emitCSource(graph, sema, cs)) {
    err << "error: failed to generate C source for native backend\n";
    return false;
  }
  const std::string src = cs.str();
  if (const char* d = std::getenv("A_DEBUG_EMITC")) { std::ofstream o(d); o << src; }

  TCCState* s = tcc_new();
  if (!s) {
    err << "error: failed to create native compiler state\n";
    return false;
  }

  std::string diags;
  tcc_set_error_func(s, &diags, nativeDiag);
  tcc_set_lib_path(s, libpath.c_str());

  // The C emitter is self-contained (no #include <stdio.h> etc.; it
  // forward-declares everything it uses), so -nostdinc is safe and keeps
  // the staged headers authoritative. -nostdlib is NOT used here: EXE
  // output must link crt1/crti/libc from the host sysroot.
  tcc_set_options(s, "-nostdinc");
  tcc_add_sysinclude_path(s, (libpath + "/include").c_str());
  // Builtin optimizer level -O2, non-PIE on Linux (ELF writer defaults
  // to non-PIE ET_EXEC). A_TCC_OPT env overrides the opt level (0|1|2).
  {
    const char* optEnv = std::getenv("A_TCC_OPT");
    int opt = 2;
    if (optEnv) {
      try { opt = std::clamp(std::stoi(optEnv), 0, 2); } catch (...) {}
    }
    char optFlag[16];
    std::snprintf(optFlag, sizeof(optFlag), "-O%d", opt);
    tcc_set_options(s, optFlag);
  }
  tcc_define_symbol(s, "_A_NATIVE", "1");
  tcc_define_symbol(s, "__A_NATIVE_FORK__", "1");

  // EXE needs crt1/crti/libc from the host sysroot. The staged {B} paths
  // only contain the support archive, so add the usual system dirs BEFORE
  // set_output_type (which looks up crt1.o).
#if defined(__linux__)
  tcc_add_library_path(s, "/usr/lib/x86_64-linux-gnu");
  tcc_add_library_path(s, "/lib/x86_64-linux-gnu");
  tcc_add_library_path(s, "/usr/lib");
  tcc_add_library_path(s, "/lib");
#elif defined(__APPLE__)
  tcc_add_library_path(s, "/usr/lib");
#endif

  if (tcc_set_output_type(s, TCC_OUTPUT_EXE) < 0) {
    err << "error: failed to set native output type (EXE)\n";
    if (!diags.empty()) err << diags;
    tcc_delete(s);
    return false;
  }

  // libm is required (sqrt/pow/fmod/exp/log/sin/cos/tan/fabs/floor/ceil/round).
  tcc_add_library(s, "m");

  bool ok = tcc_compile_string(s, src.c_str()) >= 0;
  if (ok) ok = tcc_add_file(s, (libpath + "/libtcc1.a").c_str()) >= 0;
  if (ok) ok = tcc_output_file(s, outputPath.c_str()) >= 0;

  if (!ok) {
    err << "error: native compilation failed\n";
    if (!diags.empty()) err << diags;
    tcc_delete(s);
    return false;
  }

  tcc_delete(s);
  std::error_code ec;
  std::filesystem::permissions(outputPath,
      std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
          std::filesystem::perms::others_read,
      std::filesystem::perm_options::replace, ec);
  return true;
}

}  // namespace a::kgen
