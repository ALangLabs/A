#include "backend/kgen/astra_backend.hpp"

#include <cerrno>
#include <filesystem>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <ostream>
#include <unistd.h>

#include "backend/kgen/native_backend.hpp"

namespace a {

bool buildNativeModule(const ModuleGraph& graph, const SemaResult& sema,
                       const std::string& outputPath, std::ostream& err) {
  // Astra (compiler/astra/) is the ONLY native compiler for A:
  // A -> emitCSource() (C99 aval ABI) -> in-process native EXE output.
  return kgen::buildExecutableWithNative(graph, sema, outputPath, err);
}

bool compileNativeModule(const ModuleGraph& graph, const SemaResult& sema,
                         const std::string& outputBase, std::ostream& err) {
  return buildNativeModule(graph, sema, outputBase, err);
}

int runBuiltNativeModule(const std::string& outputBase, std::istream& in, std::ostream& out,
                         std::ostream& err) {
  // fork/exec: child inherits our stdout/stderr (no popen shell, no temp
  // err file), parent reaps and reports the exit status.
  (void)in; (void)out;
  pid_t pid = fork();
  if (pid < 0) {
    err << "error: failed to execute Astra-built binary\n";
    return 2;
  }
  if (pid == 0) {
    execl(outputBase.c_str(), outputBase.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) return 2;
  }
  std::filesystem::remove(outputBase);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 2;
}

int runNativeModule(const ModuleGraph& graph, const SemaResult& sema,
                    std::istream& in, std::ostream& out, std::ostream& err) {
  std::string tmpBase = "/tmp/a_astra_run_" + std::to_string(getpid()) + "_" + std::to_string(rand());
  if (!compileNativeModule(graph, sema, tmpBase, err)) {
    return 1;
  }
  return runBuiltNativeModule(tmpBase, in, out, err);
}

}  // namespace a
