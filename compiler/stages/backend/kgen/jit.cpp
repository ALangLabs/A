#include "backend/kgen/jit.hpp"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sstream>
#include <string>
#include <unistd.h>

#include "libtcc.h"

#include "backend/kgen/codegen/llvm_emitter.hpp"

namespace a::kgen {
namespace {

// Where TinyCC's staged runtime lives: A_TCCLIB override, else tcclib/ next to
// the compiler binary (the layout cmake/libtcc.cmake produces).
std::string jitLibPath() {
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

void jitDiag(void* opaque, const char* msg) {
  auto* s = static_cast<std::string*>(opaque);
  *s += msg;
  *s += '\n';
}

}  // namespace

int runJitModule(const ModuleGraph& graph, const SemaResult& sema, std::ostream& err) {
  std::string libpath = jitLibPath();
  if (libpath.empty()) return -1;

  std::ostringstream cs;
  if (!emitCSource(graph, sema, cs)) return -1;
  const std::string src = cs.str();

  TCCState* s = tcc_new();
  if (!s) return -1;

  std::string diags;
  tcc_set_error_func(s, &diags, jitDiag);
  tcc_set_lib_path(s, libpath.c_str());
  tcc_set_options(s, "-nostdinc -nostdlib -Wl,-nostdlib");
  tcc_add_sysinclude_path(s, (libpath + "/include").c_str());
  if (tcc_set_output_type(s, TCC_OUTPUT_MEMORY) < 0) {
    tcc_delete(s);
    return -1;
  }

  // TinyCC lowers aggregate copies/zero-init to these libc calls.
  tcc_add_symbol(s, "memset", (void*)memset);
  tcc_add_symbol(s, "memcpy", (void*)memcpy);
  tcc_add_symbol(s, "memmove", (void*)memmove);

  // Symbols the generated C calls, bound to this process's own libc/libm.
  tcc_add_symbol(s, "printf", (void*)printf);
  tcc_add_symbol(s, "sqrt", (void*)(double(*)(double))sqrt);
  tcc_add_symbol(s, "pow", (void*)(double(*)(double,double))pow);
  tcc_add_symbol(s, "fmod", (void*)(double(*)(double,double))fmod);
  tcc_add_symbol(s, "floor", (void*)(double(*)(double))floor);
  tcc_add_symbol(s, "ceil", (void*)(double(*)(double))ceil);
  tcc_add_symbol(s, "round", (void*)(double(*)(double))round);
  tcc_add_symbol(s, "exp", (void*)(double(*)(double))exp);
  tcc_add_symbol(s, "log", (void*)(double(*)(double))log);
  tcc_add_symbol(s, "sin", (void*)(double(*)(double))sin);
  tcc_add_symbol(s, "cos", (void*)(double(*)(double))cos);
  tcc_add_symbol(s, "tan", (void*)(double(*)(double))tan);
  tcc_add_symbol(s, "fabs", (void*)(double(*)(double))fabs);

  if (tcc_compile_string(s, src.c_str()) < 0 ||
      tcc_add_file(s, (libpath + "/libtcc1.a").c_str()) < 0 ||
      tcc_relocate(s) < 0) {
    if (std::getenv("A_DEBUG_JIT")) err << "jit: " << diags;
    tcc_delete(s);
    return -1;
  }

  auto entry = reinterpret_cast<int (*)(void)>(tcc_get_symbol(s, "main"));
  if (!entry) {
    tcc_delete(s);
    return -1;
  }
  int rc = entry();
  tcc_delete(s);
  return rc;
}

}  // namespace a::kgen
