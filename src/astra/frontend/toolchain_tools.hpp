#pragma once
// Host toolchain helpers shared by the subprocess LLVM backend
// (frontend/pipeline/pipeline.cpp) and the in-process MLIR backend's
// clang link stage (src/astra/mlir/codegen.cpp): executable-file probing,
// PATH/Bundled tool discovery, the mold fast-path probe, and the
// fork/exec (CreateProcess on Windows) child runner.
//
// Header-only `inline` on purpose: pipeline.cpp is compiled into
// a_pipeline in every configuration (including no-MLIR builds), while
// codegen.cpp only exists inside a_mlir_core — a shared .cpp would need
// CMake plumbing on both sides for no extra benefit.

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace a::toolchain {

// exists() plus (POSIX) the executable bit — the "is this path a runnable
// tool" test every LLVM/clang/mold probe shares.
inline bool isRunnableFile(const std::string& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return false;
#ifndef _WIN32
  if (::access(path.c_str(), X_OK) != 0) return false;
#endif
  return true;
}

// First `dir/name` (in `names` order) that is runnable; "" when none.
// `names` may be a braced initializer list or a `const char*[]` table.
template <typename Names>
inline std::string findToolInDir(const std::string& dir, const Names& names) {
  for (const char* n : names) {
    std::string cand = dir + "/" + n;
    if (isRunnableFile(cand)) return cand;
  }
  return "";
}

// PATH entries in order (";" on Windows, ":" elsewhere). Empty entries are
// dropped, matching the historical tool-search loops.
inline std::vector<std::string> pathEntries() {
  std::vector<std::string> out;
  const char* pathEnv = std::getenv("PATH");
#ifdef _WIN32
  const char delim = ';';
#else
  const char delim = ':';
#endif
  std::istringstream ss(pathEnv ? pathEnv : "");
  std::string dir;
  while (std::getline(ss, dir, delim)) {
    if (!dir.empty()) out.push_back(dir);
  }
  return out;
}

// First runnable tool named by `names` found on PATH; "" when absent.
template <typename Names>
inline std::string findToolOnPath(const Names& names) {
  for (const std::string& dir : pathEntries()) {
    std::string hit = findToolInDir(dir, names);
    if (!hit.empty()) return hit;
  }
  return "";
}

// True when `mold` exists on PATH and A_NO_MOLD is unset — the fast-path
// linker probe shared by llvmBuild (pipeline.cpp) and linkWithRuntime
// (codegen.cpp). Only reached on non-Windows hosts (both callers are
// inside non-Windows branches), so the ':' PATH split matches the
// historical inline code, including empty segments checking "/mold".
inline bool moldAvailable() {
  if (const char* e = std::getenv("A_NO_MOLD"); e && *e) return false;
  const char* path = std::getenv("PATH");
  std::string p(path ? path : "");
  size_t pos = 0;
  while (pos <= p.size()) {
    size_t colon = p.find(':', pos);
    std::string dir = p.substr(pos, colon == std::string::npos
                                    ? std::string::npos : colon - pos);
    std::error_code ec;
    if (std::filesystem::exists(dir + "/mold", ec)) return true;
    if (colon == std::string::npos) break;
    pos = colon + 1;
  }
  return false;
}

// Argument vector for the final `clang obj.o libaruntime.a -o out` link
// shared by llvmBuild (pipeline.cpp) and linkWithRuntime (codegen.cpp):
// llc emits a non-PIC object so -no-pie keeps the link consistent, and
// -ldl is added everywhere except macOS.
inline std::vector<std::string> runtimeLinkArgs(const std::string& objPath,
                                                const std::string& runtimeLib,
                                                const std::string& outputPath) {
  std::vector<std::string> args = {objPath, runtimeLib, "-o", outputPath,
                                   "-no-pie", "-lm", "-lpthread"};
#ifndef __APPLE__
  args.push_back("-ldl");
#endif
  return args;
}

// Copy of a runtimeLinkArgs vector with "-fuse-ld=mold" inserted right
// after the object path (index 1) — the mold fast-path variant both link
// stages try before falling back to the default linker.
inline std::vector<std::string> moldLinkArgs(
    const std::vector<std::string>& args) {
  std::vector<std::string> margs = args;
  margs.insert(margs.begin() + 1, "-fuse-ld=mold");
  return margs;
}

// Runs `prog` with `args` (no shell). The child inherits stdin/stderr and,
// unless `discardStdout` is set, stdout as well; with `discardStdout` the
// child's stdout goes to /dev/null (NUL on Windows) so `--version`-style
// viability probes cannot leak banners into the compiled program's stdout.
// Returns the exit code, 127 on spawn/exec failure, and 128+signal on
// POSIX when the child died to a signal.
inline int runToolProcess(const std::string& prog,
                          const std::vector<std::string>& args,
                          bool discardStdout = false) {
#ifdef _WIN32
  std::string cmd = "\"" + prog + "\"";
  for (const auto& a : args) {
    cmd += " \"";
    for (char c : a) { if (c == '"') cmd += "\\\""; else cmd += c; }
    cmd += "\"";
  }
  std::vector<char> buf(cmd.begin(), cmd.end());
  buf.push_back('\0');
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  HANDLE nul = nullptr;
  if (discardStdout) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    nul = CreateFileA("NUL", GENERIC_WRITE,
                      FILE_SHARE_WRITE | FILE_SHARE_READ, &sa,
                      OPEN_EXISTING, 0, nullptr);
  }
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = nul != nullptr ? nul : GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE,
                      0, nullptr, nullptr, &si, &pi)) {
    if (nul) CloseHandle(nul);
    return 127;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 127;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  if (nul) CloseHandle(nul);
  return (int)code;
#else
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(prog.c_str()));
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = fork();
  if (pid < 0) return 127;
  if (pid == 0) {
    if (discardStdout) {
      int fd = ::open("/dev/null", O_WRONLY);
      if (fd >= 0) {
        if (fd != STDOUT_FILENO) ::dup2(fd, STDOUT_FILENO);
        if (fd > STDERR_FILENO) ::close(fd);
      }
    }
    execvp(prog.c_str(), argv.data());
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) return 127;
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 128 + WTERMSIG(status);
#endif
}

}  // namespace a::toolchain
