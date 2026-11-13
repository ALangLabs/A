#include "pipeline/pipeline.hpp"
#include "pipeline/comptime.hpp"
#include "exe_path.hpp"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <set>
#include <system_error>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif
#include <utility>
#include <numeric>
#include <unordered_map>
#include <vector>
#include "diagnostics/diagnostics.hpp"
#include "driver/version.hpp"
#include "name-resolution/module_loader.hpp"
#include "ownership/ownership.hpp"
#include "type-checker/sema.hpp"
#include "vm/vm.hpp"
#include "llvm/llvm_emit.hpp"
namespace a {
namespace {
constexpr int kOk=0,kCErr=1,kNoIn=66;
bool flushErrors(DiagnosticSink& s,const PipelineConfig& c,std::ostream& o,std::ostream& e){
  if(!s.hasErrors()) return true;
  if(c.jsonDiagnostics&&c.checkOnly) s.writeJson(o); else s.flush(e,c.useColor);
  return false;
}
void flushFinal(DiagnosticSink& s,const PipelineConfig& c,std::ostream& o,std::ostream& e){
  if(c.jsonDiagnostics&&c.checkOnly) s.writeJson(o); else s.flush(e,c.useColor);
}
bool readAll(const std::string& p,std::string& t,std::ostream& e){
  std::error_code ec;
  if(std::filesystem::is_directory(p,ec)){
    e<<"error: '"<<p<<"' is a directory, not a source file\n"; return false;
  }
  std::ifstream f(p,std::ios::binary); if(!f){e<<"error: cannot open '"<<p<<"'\n"; return false;}
  std::ostringstream b; b<<f.rdbuf(); if(f.bad()){e<<"error: failed to read '"<<p<<"'\n"; return false;} t=b.str(); return true;
}

// ---------------------------------------------------------------- LLVM exec
// `a run`/`a build`/`a test` are thin wrappers over the LLVM toolchain:
// the frontend lowers the checked AST straight to LLVM IR (emitLLVM),
// then `opt` (when optimizing), `llc` and `clang` turn it into a machine-
// code executable linked against the prebuilt libaruntime.a. LLVM is the
// only native backend and is host-only — there is no assembly emitter
// fallback and no cross-compile path.
// Compile-time host description.
std::string hostArch() {
#if defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
  return "i386";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#elif defined(__riscv) && (__riscv_xlen == 64)
  return "riscv64";
#elif defined(__riscv)
  return "riscv32";
#else
  return "unknown";
#endif
}
std::string hostOs() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

struct TargetDesc {
  std::string arch;    // canonical arch name
  std::string os;      // always the host OS (no cross-OS builds)
  bool native = false; // arch == host arch
};

std::string lowerStr(std::string s) {
  for (char& c : s) c = (char)::tolower((unsigned char)c);
  return s;
}

// Parses "<arch>[-<os>[-<env>]]" or bare aliases like "i386", "arm64",
// "aarch64-linux-gnu", "x86_64-pc-windows-msvc". OS tokens that disagree
// with the host OS are rejected.
bool resolveTarget(const std::string& spec, TargetDesc& t, std::ostream& err) {
  t = TargetDesc{};
  t.os = hostOs();
  // A_CODEGEN=c selected the C emitter, which was removed. Reject it here so
  // every path fails alike.
  if (const char* forced = std::getenv("A_CODEGEN"); forced && lowerStr(forced) == "c") {
    err << "error: A_CODEGEN=c is no longer supported: the C emitter was "
           "removed — the LLVM backend is the only native backend\n";
    return false;
  }
  std::string s = spec;
  // env override for tests / unusual setups
  const char* hostEnv = std::getenv("A_TARGET");
  if (s.empty() && hostEnv && *hostEnv) s = hostEnv;
  s = lowerStr(s);
  if (s.empty() || s == "native" || s == "host") s = hostArch();
  for (char& c : s) if (c == '_') c = '-';
  // "x86-64" contains a '-' that would tokenize as "x86","64" and
  // misresolve to i386; peel the arch token off first so trailing
  // os/env tokens still classify normally.
  std::string arch;
  if (s == "x86-64" || s.rfind("x86-64-", 0) == 0) {
    arch = "x86_64";
    s = s.size() == 6 ? "" : s.substr(7);
  }
  // Tokenize on '-' and classify.
  std::string osTok;
  std::istringstream ts(s);
  std::string tok;
  while (std::getline(ts, tok, '-')) {
    if (tok.empty()) continue;
    if (arch.empty()) {
      if (tok == "x86-64" || tok == "x86_64" || tok == "amd64" || tok == "x64") arch = "x86_64";
      else if (tok == "i386" || tok == "i486" || tok == "i586" || tok == "i686" || tok == "x86" || tok == "ia32") arch = "i386";
      else if (tok == "aarch64" || tok == "arm64") arch = "arm64";
      else if (tok == "arm" || tok == "armv4" || tok == "armv5" || tok == "armv6" || tok == "armv7" || tok == "armv7l" || tok == "armv7a" || tok == "arm-linux-gnueabi" || tok == "arm-linux-gnueabihf") arch = "arm";
      else if (tok == "riscv64" || tok == "riscv64gc") arch = "riscv64";
      else if (tok == "riscv32") arch = "riscv32";
      else if (tok == "i8086" || tok == "8086" || tok == "i86" || tok == "8088") arch = "i8086";
    }
    if (tok == "linux" || tok == "linux-gnu" || tok == "gnu" || tok == "elf") { if (osTok.empty()) osTok = "linux"; }
    else if (tok == "windows" || tok == "win32" || tok == "mingw" || tok == "mingw32" || tok == "mingw64" || tok == "msvc" || tok == "cygwin" || tok == "pe") { if (osTok.empty()) osTok = "windows"; }
    else if (tok == "macos" || tok == "darwin" || tok == "apple" || tok == "osx") { if (osTok.empty()) osTok = "macos"; }
    else if (tok == "freebsd" || tok == "openbsd" || tok == "netbsd" || tok == "bsd") { if (osTok.empty()) osTok = tok; }
    // vendor/env tokens (pc, gnu, gnueabi, gnueabihf, musl, none, apple-darwin handled above) ignored
  }
  if (arch.empty()) {
    err << "error: unknown target '" << spec << "' (expected x86_64, i386, aarch64/arm64, arm, riscv64, or i8086)\n";
    return false;
  }
  // OS tokens that disagree with the host OS are rejected — the LLVM
  // backend cannot cross operating systems. Exception: 'windows' parses
  // as an explicit cross target (td.native stays false) so `a build`
  // can report the host-only error cleanly.
  std::string reqOs;
  if (!osTok.empty() && osTok != t.os) {
    if (osTok == "windows") {
      reqOs = "windows";
    } else {
      err << "error: target OS '" << osTok << "' does not match host OS '" << t.os
          << "' — the LLVM backend cannot cross operating systems\n";
      return false;
    }
  }
  t.arch = arch;
  t.os = reqOs.empty() ? t.os : reqOs;
  t.native = (t.os == hostOs()) &&
             ((arch == hostArch()) ||
              // x86-64 Linux/macOS kernels run i386 binaries natively
              // (IA32 emulation / Rosetta-free 32-bit support).
              (arch == "i386" && hostArch() == "x86_64"));
  if (arch == "riscv32") {
    err << "error: target 'riscv32' is not supported: astra only ships a "
           "riscv64 backend; the runtime value model is 64-bit\n";
    return false;
  }
  // The LLVM backend is host-only: only the host arch on the host OS can
  // produce a runnable binary. resolveTarget still parses foreign arches
  // so `a build --target ...` can report a clean error downstream.
  return true;
}

// Locates the astra driver for `arch`/`os`: A_ASTRA_<ARCH>[_<OS>] override
// (plus A_ASTRA for the host arch/os), then the binary staged with the
// runtime (build/astra/astra-<arch>, or plain `astra` for the host arch/os),
// then PATH. For a non-host OS (windows PE cross builds) only OS-specific
// driver names are tried — falling back to the host-OS astra-<arch> would
// silently produce the wrong object format.
std::string findAstra(const std::string& arch, const std::string& os = "") {
  std::string envName = "A_ASTRA_";
  for (char c : arch) envName += (c == '-' ? '_' : (char)::toupper((unsigned char)c));
  if (!os.empty() && os != hostOs()) {
    envName += "_";
    for (char c : os) envName += (c == '-' ? '_' : (char)::toupper((unsigned char)c));
  }
  if (const char* e = std::getenv(envName.c_str()); e && *e) return e;
  if (os.empty() || os == hostOs()) {
    if (arch == hostArch()) {
      if (const char* e = std::getenv("A_ASTRA"); e && *e) return e;
    }
  }
  std::vector<std::string> names;
  if (!os.empty() && os != hostOs()) {
    names.push_back("astra-" + arch + "-" + os);
  } else {
    names.push_back("astra-" + arch);
    if (arch == hostArch()) names.push_back("astra");
  }
#ifdef _WIN32
  for (auto& nm : names) nm += ".exe";
  wchar_t wexe[MAX_PATH];
  DWORD wn = GetModuleFileNameW(nullptr, wexe, MAX_PATH);
  if (wn > 0 && wn < MAX_PATH) {
    int n = WideCharToMultiByte(CP_UTF8, 0, wexe, (int)wn, nullptr, 0, nullptr, nullptr);
    std::string exe((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wexe, (int)wn, exe.data(), n, nullptr, nullptr);
    exe.resize((size_t)n);
    std::string dir = exe;
    auto pos = dir.find_last_of("/\\");
    if (pos != std::string::npos) {
      dir = dir.substr(0, pos);
      for (const std::string& base : {dir, dir + "/astra", dir + "/astralib/..", dir + "/astralib"}) {
        for (const auto& nm : names) {
          std::string c = base + "/" + nm;
          std::error_code ec;
          if (std::filesystem::is_regular_file(c, ec)) return c;
        }
      }
    }
  }
  // PATH search via SearchPath (handles .exe/PATHEXT).
  for (const auto& nm : names) {
    wchar_t wn2[MAX_PATH];
    std::wstring wnm(nm.begin(), nm.end());
    if (SearchPathW(nullptr, wnm.c_str(), L".exe", MAX_PATH, wn2, nullptr) > 0) {
      int n = WideCharToMultiByte(CP_UTF8, 0, wn2, -1, nullptr, 0, nullptr, nullptr);
      std::string c((size_t)n - 1, '\0');
      WideCharToMultiByte(CP_UTF8, 0, wn2, -1, c.data(), n, nullptr, nullptr);
      return c;
    }
  }
  return names.front();  // let CreateProcess report the failure
#else
  const std::string self = exePath();
  if (!self.empty()) {
    std::string dir = self;
    auto pos = dir.find_last_of('/');
    if (pos != std::string::npos) {
      dir = dir.substr(0, pos);
      for (const std::string& base : {dir, dir + "/astra", dir + "/astralib/..", dir + "/astralib"}) {
        for (const auto& nm : names) {
          std::string c = base + "/" + nm;
          std::error_code ec;
          if (std::filesystem::is_regular_file(c, ec) && access(c.c_str(), X_OK) == 0) return c;
        }
      }
    }
  }
  for (const auto& nm : names) if (access(nm.c_str(), X_OK) == 0) return nm;
  return names.front();  // let execvp report the failure
#endif
}

#ifdef _WIN32
// Spawn `prog` with the given arguments via CreateProcess; the child
// inherits our stdin/stdout/stderr. Returns the exit code, 127 on failure.
int runChild(const std::string& prog, const std::vector<std::string>& args){
  std::string cmd = "\"" + prog + "\"";
  for (const auto& a : args) {
    cmd += " \"";
    for (char c : a) { if (c == '"') cmd += "\\\""; else cmd += c; }
    cmd += "\"";
  }
  std::vector<char> buf(cmd.begin(), cmd.end());
  buf.push_back('\0');
  STARTUPINFOA si{}; si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE,
                      0, nullptr, nullptr, &si, &pi))
    return 127;
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 127;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  return (int)code;
}
#else
// fork/exec `prog` with the given arguments; the child inherits our
// stdin/stdout/stderr. Returns the exit code, 127 on exec failure.
int runChild(const std::string& prog, const std::vector<std::string>& args){
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(prog.c_str()));
  for(const auto& a:args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid=fork();
  if(pid<0) return 127;
  if(pid==0){ execvp(prog.c_str(),argv.data()); _exit(127); }
  int status=0;
  while(waitpid(pid,&status,0)<0){ if(errno!=EINTR) return 127; }
  if(WIFEXITED(status)) return WEXITSTATUS(status);
  return 128+WTERMSIG(status);
}
#endif

#ifdef _WIN32
// CreateProcess a built binary directly; inherits stdin/stdout/stderr.
// Returns the exit code, 127 on failure.
int runBinary(const std::string& path){
  return runChild(path, {});
}
#else
// fork/exec a built binary directly; the child inherits our
// stdin/stdout/stderr. Returns the exit code, 127 on exec failure.
int runBinary(const std::string& path){
  pid_t pid=fork();
  if(pid<0) return 127;
  if(pid==0){ execl(path.c_str(),path.c_str(),static_cast<char*>(nullptr)); _exit(127); }
  int status=0;
  while(waitpid(pid,&status,0)<0){ if(errno!=EINTR) return 127; }
  if(WIFEXITED(status)) return WEXITSTATUS(status);
  return 128+WTERMSIG(status);
}
#endif

std::string tempPath(const char* tag,const std::string& name=""){
#ifdef _WIN32
  std::string base;
  {
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    base = (n > 0 && n < MAX_PATH) ? std::string(tmp, n) : ".\\";
    if (!base.empty() && base.back() != '\\' && base.back() != '/') base += '\\';
  }
#else
  std::string base="/tmp/a_";
#endif
  base += "a_" + std::string(tag) + "_" + std::to_string(
#ifdef _WIN32
      GetCurrentProcessId()
#else
      ::getpid()
#endif
      ) + "_" + std::to_string(::rand());
  if(!name.empty()){
    std::string ext=name.substr(name.find_last_of('.')==std::string::npos?name.size():name.find_last_of('.'));
    base+=ext;
  }
  return base;
}

// ------------------------------------------------------- run binary cache
// `a run`/`a test`/REPL recompile the same source (via LLVM) on every
// invocation, costing ~20ms even for a noop program, while the cached two-
// phase-free binary execs in ~2ms like a native binary. The cache is
// content-addressed: every module source, the runtime files, the toolchain
// binaries, target, opt level, codegen-forcing env vars and the toolchain
// version feed one FNV-1a key, so any edit invalidates. Disable with
// A_NO_CACHE=1; override the directory with A_CACHE_DIR.

// Forward declaration for findRuntime (defined below); run_cache::plan
// needs the resolved astra/runtime paths.
bool findRuntime(const std::string& arch, const std::string& os,
                 std::string& astraBin, std::string& runtimeC,
                 std::string& runtimeDir);

// Central cache root shared by the run cache and the per-module object
// cache. A_CACHE_DIR overrides it (tests/CI); otherwise caches live under
// the platform user-cache directory so source trees stay clean:
//   Linux:   $XDG_CACHE_HOME/a or $HOME/.cache/a
//   macOS:   $HOME/Library/Caches/a
//   Windows: %LOCALAPPDATA%\a\cache
// Falls back to the temp dir when no home/location is available.
inline std::string defaultCacheRoot() {
  if (const char* d = std::getenv("A_CACHE_DIR"); d && *d) return d;
#ifdef _WIN32
  if (const char* l = std::getenv("LOCALAPPDATA"); l && *l)
    return std::string(l) + "\\a\\cache";
  char tmp[MAX_PATH];
  DWORD n = GetTempPathA(MAX_PATH, tmp);
  return std::string(n > 0 && n < MAX_PATH ? tmp : ".\\") + "a-cache";
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME"); home && *home)
    return std::string(home) + "/Library/Caches/a";
  return "/tmp/a-cache";
#else
  if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x)
    return std::string(x) + "/a";
  if (const char* home = std::getenv("HOME"); home && *home)
    return std::string(home) + "/.cache/a";
  if (const char* tmp = std::getenv("TMPDIR"); tmp && *tmp)
    return std::string(tmp) + "/a-cache";
  return "/tmp/a-cache";
#endif
}

// Absolute, lexically-normal form of a module path for cache keys.
// Display paths may be relative ("src/main.a"), and the shared central
// cache must not confuse identical relative paths in different projects —
// emitted symbols embed canonical paths, so keys must too.
inline std::string absCachePath(const std::string& path) {
  std::error_code ec;
  std::string abs =
      std::filesystem::absolute(path, ec).lexically_normal().string();
  return ec ? path : abs;
}

// Defined below with the LLVM backend; the run cache folds the resolved
// tool paths into its key so toolchain upgrades invalidate binaries.
std::string findLLVMTool(const char* envName,
                         std::initializer_list<const char*> names);

namespace run_cache {

inline std::uint64_t mixBytes(std::uint64_t h, const std::string& s) {
  h ^= static_cast<std::uint64_t>(s.size());
  h *= 1099511628211ULL;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
  return h;
}

inline std::uint64_t mixInt(std::uint64_t h, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) { h ^= (v >> (8 * i)) & 0xff; h *= 1099511628211ULL; }
  return h;
}

inline bool readFileText(const std::string& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream b;
  b << f.rdbuf();
  if (f.bad()) return false;
  out = b.str();
  return true;
}

inline std::uint64_t mixFileStat(std::uint64_t h, const std::string& p) {
  std::error_code ec;
  auto st = std::filesystem::status(p, ec);
  if (!std::filesystem::exists(st)) return mixBytes(h, "<missing>:" + p);
  std::uint64_t sz = static_cast<std::uint64_t>(std::filesystem::file_size(p, ec));
  std::uint64_t mt = static_cast<std::uint64_t>(
      std::filesystem::last_write_time(p, ec).time_since_epoch().count());
  h = mixBytes(h, p);
  h = mixInt(h, sz);
  return mixInt(h, mt);
}

// Fold the resolved LLVM tool binaries (opt/llc/clang) into the cache key:
// a toolchain upgrade must invalidate cached binaries even when sources,
// runtime and the `a` binary itself are unchanged.
inline void mixLLVMToolStats(std::uint64_t& h) {
  const std::string opt = findLLVMTool(
      "A_LLVM_OPT", {"opt", "opt-18", "opt-17", "opt-16", "opt-15"});
  const std::string llc = findLLVMTool(
      "A_LLVM_LLC", {"llc", "llc-18", "llc-17", "llc-16", "llc-15"});
  const std::string cc = findLLVMTool(
      "A_LLVM_CC",
      {"clang", "clang-18", "clang-17", "clang-16", "clang-15"});
  if (!opt.empty()) h = mixFileStat(h, opt);
  if (!llc.empty()) h = mixFileStat(h, llc);
  if (!cc.empty()) h = mixFileStat(h, cc);
}

// Runtime C/headers are compiled into every binary, so a change to any of
// them must invalidate cached binaries: without this, editing net.c or
// bytes_conv.c and re-running `a run` silently reused the old executable.
// Process-local memoization for runtime file stats: precheck, binary plan
// and per-unit keys all hash the same runtime files in one `a` invocation.
// Files do not change mid-invocation, so stats are captured once. Hashes
// are byte-identical to the uncached path (same mix calls, same order).
struct RuntimeStat {
  std::string path;
  std::uint64_t sz = 0;
  std::uint64_t mt = 0;
  bool missing = false;
};
inline const std::vector<RuntimeStat>& runtimeStats(const std::string& runtimeC) {
  static std::unordered_map<std::string, std::vector<RuntimeStat>> cache;
  auto it = cache.find(runtimeC);
  if (it != cache.end()) return it->second;
  std::vector<RuntimeStat> stats;
  if (!runtimeC.empty()) {
    std::filesystem::path src = std::filesystem::path(runtimeC).parent_path();
    if (!src.empty()) {
      std::vector<std::string> files;
      std::function<void(const std::filesystem::path&, int)> collect =
          [&](const std::filesystem::path& dir, int depth) {
        std::error_code dec;
        std::filesystem::directory_iterator dit(
            dir, std::filesystem::directory_options::skip_permission_denied, dec);
        if (dec) return;
        for (std::filesystem::directory_iterator end; dit != end; dit.increment(dec)) {
          if (dec) return;
          std::error_code sec;
          const std::filesystem::path p = dit->path();
          std::filesystem::file_status st = dit->symlink_status(sec);
          if (std::filesystem::is_directory(st)) {
            if (depth < 1) collect(p, depth + 1);
            continue;
          }
          const std::string ext = p.extension().string();
          if (ext == ".c" || ext == ".h") files.push_back(p.string());
        }
      };
      collect(src, 0);
      std::sort(files.begin(), files.end());
      for (const std::string& f : files) {
        RuntimeStat rs;
        rs.path = f;
        std::error_code ec;
        auto st = std::filesystem::status(f, ec);
        if (!std::filesystem::exists(st)) rs.missing = true;
        else {
          rs.sz = static_cast<std::uint64_t>(std::filesystem::file_size(f, ec));
          rs.mt = static_cast<std::uint64_t>(
              std::filesystem::last_write_time(f, ec).time_since_epoch().count());
        }
        stats.push_back(std::move(rs));
      }
    }
  }
  auto res = cache.emplace(runtimeC, std::move(stats)).first;
  return res->second;
}
inline void mixRuntimeSources(std::uint64_t& h, const std::string& runtimeC) {
  if (runtimeC.empty()) return;
  for (const RuntimeStat& rs : runtimeStats(runtimeC)) {
    if (rs.missing) {
      h = mixBytes(h, "<missing>:" + rs.path);
    } else {
      h = mixBytes(h, rs.path);
      h = mixInt(h, rs.sz);
      h = mixInt(h, rs.mt);
    }
  }
}

// The compiler binary itself is part of every cache key: rebuilding `a`
// changes codegen, and the version string does not move on every commit. A
// stale hit here silently ran an executable produced by an older compiler
// (which is exactly how a codegen regression hides during development).
inline std::string selfExePath() {
#ifdef _WIN32
  char buf[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return std::string();
  return std::string(buf, n);
#else
  return exePath();
#endif
}

struct Plan {
  bool active = false;
  std::string file;  // final cache file path
};

// A_CACHE_DIR overrides the run-cache root (used by tests/CI). By default
// the root is the platform user-cache directory (see defaultCacheRoot);
// caches no longer drop `_acache_/` folders next to source files.
inline bool cacheDirOverridden() {
  if (const char* d = std::getenv("A_CACHE_DIR"); d && *d) return true;
  return false;
}
inline std::string cacheDir() { return defaultCacheRoot(); }

// Run/build caching is content-addressed and on by default; A_NO_CACHE=1
// disables it (useful when chasing heisenbugs or benchmarking cold compiles).
inline bool disabled() {
  return std::getenv("A_NO_CACHE") != nullptr;
}

// Computes the content-addressed cache path, or leaves Plan inactive when
// caching does not apply (cross target, disabled, unreadable module).
// Callers check CT via std::filesystem::exists on Plan::file; on a miss
// they compile to a temp name in the cache dir and rename over `file`.
inline Plan plan(CompiledModule& m, const TargetDesc& td, bool testMode,
                 int optimizationLevel, const std::string* entrySource,
                 const std::string* testFilter = nullptr) {
  Plan p;
  if (!td.native || disabled()) return p;
  std::string astraBin, runtimeC, runtimeDir;
  if (!findRuntime(td.arch, td.os, astraBin, runtimeC, runtimeDir)) return p;
  std::uint64_t h = 1469598103934665603ULL;
  h = mixBytes(h, kVersion);
  h = mixBytes(h, td.arch);
  h = mixInt(h, static_cast<std::uint64_t>(optimizationLevel));
  h = mixInt(h, testMode ? 1 : 0);
  // `a test --filter` changes which tests run, so it must key the cache:
  // two different filters over the same source produce different binaries.
  if (testMode) h = mixBytes(h, testFilter ? *testFilter : "");
  const char* codegen = std::getenv("A_CODEGEN");
  h = mixBytes(h, codegen ? codegen : "");
  h = mixFileStat(h, astraBin);
  {
    const std::string self = selfExePath();
    if (!self.empty()) h = mixFileStat(h, self);
  }
  // Library dir that astra links against, when staged.
  for (const std::string& cand : {runtimeDir + "/libastra1.a",
                                  runtimeDir + "/lib/libastra1.a"}) {
    if (std::filesystem::exists(cand)) { h = mixFileStat(h, cand); break; }
  }
  h = mixBytes(h, runtimeDir);
  mixRuntimeSources(h, runtimeC);
  h = mixFileStat(h, runtimeDir + "/libaruntime.a");
  mixLLVMToolStats(h);
  // Module sources: entry first-class (inline "<repl>"/"-c" sources come
  // from entrySource), every other module must read from disk.
  const ModuleGraph& g = *m.graph;
  for (const Module& mod : g.modules) {
    h = mixBytes(h, absCachePath(mod.path));
    std::string text;
    if (!readFileText(mod.path, text)) {
      if (mod.id == g.entry && entrySource) {
        text = *entrySource;
      } else {
        return Plan{};
      }
    }
    h = mixBytes(h, text);
  }
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
  p.active = true;
  std::string dir = cacheDir();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  p.file = dir + (cacheDirOverridden() ? "/" : "/run.") + hex;
  return p;
}

// One import edge found by the textual scan, tagged with the resolution
// table ModuleLoader would use for it.
struct PreImport {
  int kind = 3;  // 0 bare name, 1 std:<package>, 2 quoted path, 3 unknown
  std::string target;
};

// Collects every module import in a source text. Statement position is
// decided textually (`import`/`from` as the first token of a line), which is
// also true for imports nested in blocks: treating a nested import as a
// dependency can only make the key more sensitive, never less. An import
// whose target cannot be classified is reported as kind 3 so the caller can
// fall back to a full frontend run instead of trusting an incomplete key.
inline void scanImports(const std::string& text, std::vector<PreImport>& out) {
  std::istringstream ss(text);
  std::string line;
  const auto isName = [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '.' || c == '/' ||
           c == '-' || c == '$';
  };
  while (std::getline(ss, line)) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (line.compare(i, 7, "import ") == 0) i += 7;
    else if (line.compare(i, 5, "from ") == 0) i += 5;
    else continue;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size()) continue;
    PreImport imp;
    if (line[i] == '"') {
      const std::size_t end = line.find('"', i + 1);
      if (end == std::string::npos) {
        imp.kind = 3;
      } else {
        imp.kind = 2;
        imp.target = line.substr(i + 1, end - i - 1);
      }
    } else if (line.compare(i, 4, "std:") == 0) {
      std::size_t j = i + 4;
      while (j < line.size() && isName((unsigned char)line[j])) ++j;
      imp.kind = 1;
      imp.target = line.substr(i + 4, j - (i + 4));
    } else if (line.compare(i, 7, "python:") == 0) {
      continue;  // Python interop: no A module on disk
    } else {
      std::size_t j = i;
      while (j < line.size() && isName((unsigned char)line[j])) ++j;
      imp.kind = 0;
      imp.target = line.substr(i, j - i);
    }
    if (imp.target.empty()) imp.kind = 3;
    out.push_back(std::move(imp));
  }
}

// Walks the same candidate tables as ModuleLoader, but without compiling:
// each resolved module contributes its content to the key, and every
// candidate checked before it contributes an "absent" marker, so creating a
// file that would shadow a resolved module changes the key. Depth is the
// real dependency graph, so a directory full of unrelated `.a` files (or a
// temp dir) costs nothing and can never disable the cache.
struct PreScan {
  const PipelineConfig& config;
  std::string stdRoot;
  std::vector<std::string> envDirs;
  std::set<std::string> seen;
  std::uint64_t h;
  std::size_t files = 0;
  bool ok = true;
};

constexpr std::size_t kPreMaxFiles = 512;

inline void preAbsent(PreScan& s, const std::string& p) {
  s.h = mixBytes(s.h, "<absent>:" + absCachePath(p));
}

inline void preWalk(PreScan& s, const std::string& fromDir, const PreImport& imp);

inline bool preVisitor(PreScan& s, const std::string& path) {
  std::error_code ec;
  const std::string key =
      std::filesystem::absolute(path, ec).lexically_normal().string();
  if (!s.seen.insert(key).second) return true;  // already hashed
  if (++s.files > kPreMaxFiles) { s.ok = false; return false; }
  std::string text;
  if (!readFileText(path, text)) { s.ok = false; return false; }
  s.h = mixBytes(s.h, key);
  s.h = mixBytes(s.h, text);
  std::vector<PreImport> imps;
  scanImports(text, imps);
  std::filesystem::path pp(path);
  const std::string dir = pp.parent_path().empty() ? std::string(".")
                                                   : pp.parent_path().string();
  for (const PreImport& imp : imps) preWalk(s, dir, imp);
  return s.ok;
}

// Hashes one import: candidates in resolution order, first regular file wins.
inline void preWalk(PreScan& s, const std::string& fromDir, const PreImport& imp) {
  if (!s.ok) return;
  if (imp.kind == 3) { s.ok = false; return; }
  std::vector<std::string> cands;
  if (imp.kind == 2) {
    const std::filesystem::path requested(imp.target);
    if (requested.is_absolute()) {
      cands.push_back(requested.string());
    } else {
      cands.push_back((std::filesystem::path(fromDir) / requested).string());
      for (const std::string& d : s.config.modulePaths)
        cands.push_back((std::filesystem::path(d) / requested).string());
      for (const std::string& d : s.envDirs)
        cands.push_back((std::filesystem::path(d) / requested).string());
      if (!s.stdRoot.empty())
        cands.push_back((std::filesystem::path(s.stdRoot) / requested).string());
    }
  } else if (imp.kind == 1) {
    std::string rel = imp.target;
    std::replace(rel.begin(), rel.end(), '.', '/');
    const std::size_t slash = rel.find_last_of('/');
    const std::string leaf =
        slash == std::string::npos ? rel : rel.substr(slash + 1);
    const std::filesystem::path root =
        std::filesystem::path(s.stdRoot) / "std";
    cands.push_back((root / (rel + ".a")).string());
    cands.push_back((root / rel / "__init__.a").string());
    cands.push_back((root / rel / (leaf + ".a")).string());
  } else {
    // Bare name: stdlib package layout first, then each search dir, exactly
    // as ModuleLoader::resolveBare orders them.
    const std::string& name = imp.target;
    const auto addRoot = [&](const std::filesystem::path& root) {
      cands.push_back((root / (name + ".a")).string());
      cands.push_back((root / name / (name + ".a")).string());
      cands.push_back((root / name / "__init__.a").string());
    };
    if (!s.stdRoot.empty()) {
      const std::filesystem::path root(s.stdRoot);
      addRoot(root);
      addRoot(root / "std");
      cands.push_back((root / "std" / (name + ".a")).string());
      cands.push_back((root / "std" / name / (name + ".a")).string());
      cands.push_back((root / "std" / name / "__init__.a").string());
    }
    for (const std::string& d : s.config.modulePaths) addRoot(d);
  }
  for (const std::string& c : cands) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(c, ec) && !ec) {
      preVisitor(s, c);
      return;
    }
    preAbsent(s, c);
  }
}

// Pre-parse key (executePipeline): hashes the entry content, the prelude and
// every module the entry transitively resolves to, following ModuleLoader's
// candidates so that a shadowing file invalidates the key. A hit lets
// `a run` skip the frontend entirely (lex/parse/sema/ownership) and exec the
// cached binary. Returns "" when the program is too large to key this way or
// an import could not be classified — the caller then falls back to the
// frontend path, which is always correct.
inline std::string precheckKey(const std::string& path,
                               const std::string& source,
                               const PipelineConfig& config,
                               const TargetDesc& td,
                               int optimizationLevel) {
  if (!td.native || disabled()) return "";
  std::string astraBin, runtimeC, runtimeDir;
  if (!findRuntime(td.arch, td.os, astraBin, runtimeC, runtimeDir)) return "";
  std::uint64_t h = 1469598103934665603ULL;
  h = mixBytes(h, "pre1");
  h = mixBytes(h, kVersion);
  {
    const std::string self = selfExePath();
    if (!self.empty()) h = mixFileStat(h, self);
  }
  h = mixBytes(h, td.arch);
  h = mixInt(h, static_cast<std::uint64_t>(optimizationLevel));
  const char* codegen = std::getenv("A_CODEGEN");
  h = mixBytes(h, codegen ? codegen : "");
  const char* apath = std::getenv("A_PATH");
  h = mixBytes(h, apath ? apath : "");
  h = mixBytes(h, source);
  h = mixFileStat(h, astraBin);
  h = mixBytes(h, runtimeDir);
  PreScan ps{config, std::string(), {}, {}, h};
  ps.stdRoot = config.stdlibRoot.empty() ? defaultStdlibRoot()
                                         : config.stdlibRoot;
  {
    const char* apath = std::getenv("A_PATH");
    if (apath && *apath) {
      std::string rest(apath);
      std::size_t pos = 0;
      while (pos <= rest.size()) {
        const std::size_t sep = rest.find_first_of(":;", pos);
        std::string part = rest.substr(
            pos, sep == std::string::npos ? std::string::npos : sep - pos);
        if (!part.empty()) ps.envDirs.push_back(part);
        if (sep == std::string::npos) break;
        pos = sep + 1;
      }
    }
  }
  for (const std::string& d : config.modulePaths) ps.h = mixBytes(ps.h, "I:" + d);
  for (const std::string& d : ps.envDirs) ps.h = mixBytes(ps.h, "P:" + d);
  ps.h = mixBytes(ps.h, "S:" + ps.stdRoot);
  // The prelude is loaded before the entry in every program.
  {
    const std::filesystem::path root(ps.stdRoot);
    bool found = false;
    for (const std::filesystem::path& cand :
         {root / "std" / "prelude.a", root / "prelude.a"}) {
      std::error_code ec;
      if (std::filesystem::is_regular_file(cand, ec) && !ec) {
        preVisitor(ps, cand.string());
        found = true;
        break;
      }
      preAbsent(ps, cand.string());
    }
    if (!found) ps.ok = false;  // no stdlib on disk: let the frontend decide
  }
  // The entry: its text is already hashed above, so only its imports are new.
  {
    std::filesystem::path pp(path);
    const std::string dir = pp.parent_path().empty()
                                ? std::string(".")
                                : pp.parent_path().string();
    std::vector<PreImport> imps;
    scanImports(source, imps);
    for (const PreImport& imp : imps) preWalk(ps, dir, imp);
  }
  if (!ps.ok) return "";
  // Runtime sources and the archived runtime objects (mtime/size).
  std::uint64_t h2 = ps.h;
  mixRuntimeSources(h2, runtimeC);
  h2 = mixFileStat(h2, runtimeDir + "/libaruntime.a");
  mixLLVMToolStats(h2);
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx",
                static_cast<unsigned long long>(h2));
  std::string dir = cacheDir();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir + (cacheDirOverridden() ? "/pre-" : "/pre.") + hex;
}

}  // namespace run_cache

namespace objcache {

inline bool envFlagOff(const char* name) {
  const char* v = std::getenv(name);
  if (!v || !*v) return false;
  std::string s = lowerStr(v);
  return s == "0" || s == "off" || s == "no" || s == "none" || s == "false";
}
inline bool splitUnitsEnabled() {
  // Kept for the object-cache key helpers; the per-module split build that
  // consumed this was removed with the assembly backend.
  return !envFlagOff("A_SPLIT_UNITS");
}
inline bool objCacheEnabled() {
  return !run_cache::disabled();
}
inline bool debugUnits() {
  const char* v = std::getenv("A_DEBUG_UNITS");
  return v && *v && lowerStr(v) != "0";
}
// Cached objects live in <cache root>/obj (see defaultCacheRoot). Unit
// keys are content+path hashed, so a single shared dir is collision-safe
// and source trees stay free of `_acache_/` folders.
inline std::string objCacheDir() {
  return (std::filesystem::path(defaultCacheRoot()) / "obj").string();
}
inline std::string hex64(std::uint64_t h) {
  char b[17];
  std::snprintf(b, sizeof b, "%016llx",
                static_cast<unsigned long long>(h));
  return std::string(b);
}

inline std::string typeSig(const TypeExprPtr& t) {
  if (!t) return "_";
  if (const auto* n = dynamic_cast<const NamedTypeExpr*>(t.get())) {
    std::string s = "N:";
    for (const auto& q : n->qualifiers) { s += q; s += "::"; }
    s += n->name;
    if (!n->typeArgs.empty()) {
      s += "<";
      for (std::size_t i = 0; i < n->typeArgs.size(); ++i) {
        if (i) s += ",";
        s += typeSig(n->typeArgs[i]);
      }
      s += ">";
    }
    return s;
  }
  if (const auto* a = dynamic_cast<const ArrayTypeExpr*>(t.get())) {
    return "A[" + std::to_string(a->length) + "]:" + typeSig(a->element);
  }
  if (const auto* o = dynamic_cast<const OptionalTypeExpr*>(t.get())) {
    return "O:" + typeSig(o->element);
  }
  if (const auto* tp = dynamic_cast<const TupleTypeExpr*>(t.get())) {
    std::string s = "T(";
    for (std::size_t i = 0; i < tp->elements.size(); ++i) {
      if (i) s += ",";
      s += typeSig(tp->elements[i]);
    }
    return s + ")";
  }
  if (const auto* m = dynamic_cast<const MapTypeExpr*>(t.get())) {
    return "D:" + typeSig(m->key) + ":" + typeSig(m->value);
  }
  if (const auto* s = dynamic_cast<const SetTypeExpr*>(t.get())) {
    return "S:" + typeSig(s->element);
  }
  if (const auto* f = dynamic_cast<const FunctionTypeExpr*>(t.get())) {
    std::string s = "F(";
    for (std::size_t i = 0; i < f->params.size(); ++i) {
      if (i) s += ",";
      s += typeSig(f->params[i]);
    }
    return s + ")->" + typeSig(f->returnType);
  }
  return "?";
}
inline std::string exprSig(const Expr* e) {
  if (!e) return "_";
  switch (e->kind) {
    case ExprKind::IntLit: {
      return "I" + std::to_string(static_cast<const IntLitExpr*>(e)->value);
    }
    case ExprKind::FloatLit: {
      std::uint64_t bits = 0;
      double d = static_cast<const FloatLitExpr*>(e)->value;
      std::memcpy(&bits, &d, 8);
      return "F" + hex64(bits);
    }
    case ExprKind::Ident: {
      return "V" + static_cast<const IdentExpr*>(e)->name;
    }
    case ExprKind::Unary: {
      const UnaryExpr* u = static_cast<const UnaryExpr*>(e);
      return "U" + u->op + exprSig(u->operand.get());
    }
    case ExprKind::Binary: {
      const BinaryExpr* b = static_cast<const BinaryExpr*>(e);
      return "B" + b->op + "(" + exprSig(b->lhs.get()) + "," +
             exprSig(b->rhs.get()) + ")";
    }
    default:
      return "E" + std::to_string(static_cast<int>(e->kind));
  }
}
inline void mixFnSig(std::uint64_t& h, const FnDecl* fn) {
  if (!fn) return;
  h = run_cache::mixBytes(h, "F:" + fn->name);
  std::size_t arity = fn->comptimeParams.size() + fn->params.size();
  h = run_cache::mixInt(h, static_cast<std::uint64_t>(arity));
  for (const ComptimeParam& p : fn->comptimeParams) {
    h = run_cache::mixBytes(h, "C:" + p.name + ":" + typeSig(p.type));
  }
  for (const ParamDecl& p : fn->params) {
    h = run_cache::mixBytes(h, "P:" + p.name + ":" + typeSig(p.type) +
                                   (p.variadic ? ":v" : ""));
  }
  h = run_cache::mixBytes(h, "R:" + typeSig(fn->returnType));
  for (const auto& tp : fn->typeParams) h = run_cache::mixBytes(h, "T:" + tp);
  for (const auto& w : fn->whereClause) h = run_cache::mixBytes(h, "W:" + w);
}
inline std::uint64_t sighash(const Module& mod) {
  std::uint64_t h = 1469598103934665603ULL;
  h = run_cache::mixBytes(h, "sig1");
  if (!mod.program) return h;
  for (const StmtPtr& s : mod.program->statements) {
    switch (s->kind) {
      case StmtKind::FnDecl: {
        mixFnSig(h, static_cast<const FnDecl*>(s.get()));
        break;
      }
      case StmtKind::StructDecl: {
        const StructDecl* st = static_cast<const StructDecl*>(s.get());
        h = run_cache::mixBytes(h, "S:" + st->name + ":" + st->baseClass);
        for (const auto& tp : st->typeParams)
          h = run_cache::mixBytes(h, "T:" + tp);
        for (const FieldDecl& f : st->fields)
          h = run_cache::mixBytes(h, "f:" + f.name + ":" + typeSig(f.type));
        for (const auto& m : st->methods) mixFnSig(h, m.get());
        break;
      }
      case StmtKind::EnumDecl: {
        const EnumDecl* e = static_cast<const EnumDecl*>(s.get());
        h = run_cache::mixBytes(h, "E:" + e->name);
        for (const auto& tp : e->typeParams)
          h = run_cache::mixBytes(h, "T:" + tp);
        for (const EnumVariant& v : e->variants) {
          h = run_cache::mixBytes(h, "v:" + v.name + "=" + exprSig(v.value.get()));
          for (const FieldDecl& f : v.fields)
            h = run_cache::mixBytes(h, "p:" + f.name + ":" + typeSig(f.type));
        }
        break;
      }
      case StmtKind::Let: {
        const LetStmt* let = static_cast<const LetStmt*>(s.get());
        h = run_cache::mixBytes(h, "L:" + let->name + ":" + typeSig(let->type));
        break;
      }
      case StmtKind::Assign: {
        // Top-level `x = ...` creates an implicit global (__a_top_*) just
        // like `let`; it must be part of the interface hash or dependent
        // units keep stale objects when the global appears/disappears.
        const AssignStmt* as = static_cast<const AssignStmt*>(s.get());
        if (as->target && as->target->kind == ExprKind::Ident) {
          const std::string& nm =
              static_cast<const IdentExpr*>(as->target.get())->name;
          h = run_cache::mixBytes(h, "AS:" + nm + ":" + exprSig(as->value.get()));
        }
        break;
      }
      case StmtKind::TypeAlias: {
        const TypeAliasDecl* ta = static_cast<const TypeAliasDecl*>(s.get());
        h = run_cache::mixBytes(h, "A:" + ta->name + ":" + typeSig(ta->type));
        break;
      }
      case StmtKind::TraitDecl: {
        const TraitDecl* tr = static_cast<const TraitDecl*>(s.get());
        h = run_cache::mixBytes(h, "R:" + tr->name);
        for (const TraitSig& m : tr->methods)
          h = run_cache::mixBytes(h, "m:" + m.name + ":" +
              std::to_string(m.params.size()) + ":" + typeSig(m.returnType));
        for (const auto& tp : tr->typeParams)
          h = run_cache::mixBytes(h, "T:" + tp);
        break;
      }
      case StmtKind::ImplDecl: {
        const ImplDecl* im = static_cast<const ImplDecl*>(s.get());
        h = run_cache::mixBytes(
            h, "I:" + im->traitName + ":" + im->targetName);
        for (const auto& m : im->methods) mixFnSig(h, m.get());
        break;
      }
      case StmtKind::Import: {
        const ImportStmt* im = static_cast<const ImportStmt*>(s.get());
        std::string tag = "IP:";
        if (im->pythonModuleImport) tag += "py:" + im->pythonModule;
        else if (im->nativeModule) tag += "nat:" + im->bareName;
        else if (im->stdModuleImport) tag += "std:" + im->stdModule;
        else tag += im->path + ":" + im->bareName;
        h = run_cache::mixBytes(h, tag);
        break;
      }
      case StmtKind::FromImport: {
        const FromImportStmt* im = static_cast<const FromImportStmt*>(s.get());
        std::string tag = "FP:";
        if (im->pythonModuleImport) tag += "py:" + im->pythonModule;
        else if (im->nativeModule) tag += "nat:" + im->bareName;
        else if (im->stdModuleImport) tag += "std:" + im->stdModule;
        else tag += im->path + ":" + im->bareName;
        h = run_cache::mixBytes(h, tag);
        for (const auto& item : im->items)
          h = run_cache::mixBytes(h, "item:" + item.name + ":" + item.alias);
        break;
      }
      default:
        break;
    }
  }
  return h;
}
// Names a module contributes to the emitter's flat global scope
// (buildGlobalScope binds top-level lets/assigns, functions, structs, enum
// variants and module names across ALL modules, first/last-wins in module
// order). A unit's object can therefore reference `__a_top_*`/`__a_f_*`
// symbols owned by modules outside its import closure, so the unit key must
// capture the whole graph's *name namespace* — but not signatures or bodies:
// callers reach cross-module functions through adaptive/runtime dispatch, so
// an interface-only edit (e.g. an extra parameter) does not change the
// symbols a dependent unit references.
inline std::uint64_t scopeNamesHash(const Module& mod) {
  std::uint64_t h = 1469598103934665603ULL;
  h = run_cache::mixBytes(h, "scope1");
  h = run_cache::mixBytes(h, mod.name);
  h = run_cache::mixBytes(h, stemOf(mod.path));
  if (mod.program) {
    for (const StmtPtr& s : mod.program->statements) {
      switch (s->kind) {
        case StmtKind::Let: {
          h = run_cache::mixBytes(h, "L:" + static_cast<const LetStmt*>(s.get())->name);
          break;
        }
        case StmtKind::Assign: {
          const AssignStmt* as = static_cast<const AssignStmt*>(s.get());
          if (as->target && as->target->kind == ExprKind::Ident)
            h = run_cache::mixBytes(
                h, "A:" + static_cast<const IdentExpr*>(as->target.get())->name);
          break;
        }
        case StmtKind::FnDecl:
          h = run_cache::mixBytes(h, "F:" + static_cast<const FnDecl*>(s.get())->name);
          break;
        case StmtKind::StructDecl:
          h = run_cache::mixBytes(h, "S:" + static_cast<const StructDecl*>(s.get())->name);
          break;
        case StmtKind::EnumDecl: {
          const EnumDecl* e = static_cast<const EnumDecl*>(s.get());
          h = run_cache::mixBytes(h, "E:" + e->name);
          for (const EnumVariant& v : e->variants)
            h = run_cache::mixBytes(h, "V:" + v.name + "=" + exprSig(v.value.get()));
          break;
        }
        default:
          break;
      }
    }
  }
  return h;
}
inline std::set<int> directImports(const ModuleGraph& g, int moduleId) {
  std::set<int> out;
  if (moduleId < 0 || moduleId >= (int)g.modules.size()) return out;
  const Module& mod = g.at(moduleId);
  if (mod.program) {
    for (const StmtPtr& s : mod.program->statements) {
      if (s->kind == StmtKind::Import) {
        int id = static_cast<const ImportStmt*>(s.get())->moduleId;
        if (id >= 0) out.insert(id);
      } else if (s->kind == StmtKind::FromImport) {
        int id = static_cast<const FromImportStmt*>(s.get())->moduleId;
        if (id >= 0) out.insert(id);
      }
    }
  }
  // The prelude is implicitly loaded before every program.
  if (g.preludeId >= 0 && g.preludeId != moduleId && moduleId >= 0)
    out.insert(g.preludeId);
  return out;
}
inline std::set<int> transitiveImports(const ModuleGraph& g, int moduleId) {
  std::set<int> seen;
  std::vector<int> stack;
  for (int d : directImports(g, moduleId)) stack.push_back(d);
  while (!stack.empty()) {
    int cur = stack.back();
    stack.pop_back();
    if (!seen.insert(cur).second) continue;
    for (int d : directImports(g, cur)) {
      if (!seen.count(d)) stack.push_back(d);
    }
  }
  return seen;
}
// Unit key (Phase 3): content of M plus the interface closure it was built
// against. The entry unit additionally keys on every module's interface
// (its fnon setup table covers all modules) and on the test filter.
inline std::string unitKey(CompiledModule& m, const TargetDesc& td,
                           int optimizationLevel, bool testMode,
                           const std::string* testFilter,
                           const std::string* entrySource, int moduleId,
                           std::uint64_t sigh, const std::set<int>& deps) {
  std::string astraBin, runtimeC, runtimeDir;
  std::uint64_t h = 1469598103934665603ULL;
  h = run_cache::mixBytes(h, "unit1");
  h = run_cache::mixBytes(h, kVersion);
  h = run_cache::mixBytes(h, "split1");
  h = run_cache::mixBytes(h, td.arch);
  h = run_cache::mixBytes(h, td.os);
  h = run_cache::mixInt(h, static_cast<std::uint64_t>(optimizationLevel));
  const char* codegen = std::getenv("A_CODEGEN");
  h = run_cache::mixBytes(h, codegen ? codegen : "");
  if (findRuntime(td.arch, td.os, astraBin, runtimeC, runtimeDir)) {
    h = run_cache::mixFileStat(h, astraBin);
    h = run_cache::mixBytes(h, runtimeDir);
    run_cache::mixRuntimeSources(h, runtimeC);
    h = run_cache::mixFileStat(h, runtimeDir + "/libaruntime.a");
    for (const std::string& cand : {runtimeDir + "/libastra1.a",
                                    runtimeDir + "/lib/libastra1.a"}) {
      if (std::filesystem::exists(cand)) { h = run_cache::mixFileStat(h, cand); break; }
    }
  }
  {
    const std::string self = run_cache::selfExePath();
    if (!self.empty()) h = run_cache::mixFileStat(h, self);
  }
  const ModuleGraph& g = *m.graph;
  const Module& mod = g.at(moduleId);
  h = run_cache::mixBytes(h, "M:" + absCachePath(mod.path));
  std::string text;
  if (!run_cache::readFileText(mod.path, text)) {
    if (mod.id == g.entry && entrySource) text = *entrySource;
    else { h = run_cache::mixBytes(h, "<missing>"); }
  }
  h = run_cache::mixBytes(h, text);
  h = run_cache::mixInt(h, sigh);
  // Dependencies in sorted order for determinism.
  std::vector<int> sorted(deps.begin(), deps.end());
  for (int d : sorted) {
    h = run_cache::mixBytes(h, "D:" + absCachePath(g.at(d).path));
    h = run_cache::mixInt(h, sighash(g.at(d)));
  }
  if (moduleId == g.entry) {
    h = run_cache::mixInt(h, testMode ? 1 : 0);
    if (testMode) h = run_cache::mixBytes(h, testFilter ? *testFilter : "");
  }
  return hex64(h);
}
// Shared per-build key inputs (Phase 3 perf): runtime/astra/compiler
// identity and per-module sighashes/sources are computed once, not per unit.
struct KeyContext {
  std::uint64_t runtimeBase = 0;
  // Hash of the flat global scope every unit is emitted against: each
  // module's path, module name/stem and top-level exported names, in
  // g.modules order (the order buildGlobalScope resolves collisions).
  // Signature/body edits leave this unchanged, so unaffected units keep
  // their cached objects; adding/removing/reordering a module or renaming
  // a top-level symbol changes it and rebuilds everything.
  std::uint64_t graphKey = 0;
  std::unordered_map<int, std::uint64_t> sighById;
  std::unordered_map<int, std::string> sourceById;
};
inline KeyContext makeKeyContext(CompiledModule& m, const TargetDesc& td,
                                 int optimizationLevel,
                                 const std::string* entrySource) {
  KeyContext ctx;
  std::uint64_t h = 1469598103934665603ULL;
  h = run_cache::mixBytes(h, "unit1");
  h = run_cache::mixBytes(h, kVersion);
  h = run_cache::mixBytes(h, "split1");
  h = run_cache::mixBytes(h, td.arch);
  h = run_cache::mixBytes(h, td.os);
  h = run_cache::mixInt(h, static_cast<std::uint64_t>(optimizationLevel));
  const char* codegen = std::getenv("A_CODEGEN");
  h = run_cache::mixBytes(h, codegen ? codegen : "");
  std::string astraBin, runtimeC, runtimeDir;
  if (findRuntime(td.arch, td.os, astraBin, runtimeC, runtimeDir)) {
    h = run_cache::mixFileStat(h, astraBin);
    h = run_cache::mixBytes(h, runtimeDir);
    run_cache::mixRuntimeSources(h, runtimeC);
    h = run_cache::mixFileStat(h, runtimeDir + "/libaruntime.a");
    for (const std::string& cand : {runtimeDir + "/libastra1.a",
                                    runtimeDir + "/lib/libastra1.a"}) {
      if (std::filesystem::exists(cand)) { h = run_cache::mixFileStat(h, cand); break; }
    }
  }
  {
    const std::string self = run_cache::selfExePath();
    if (!self.empty()) h = run_cache::mixFileStat(h, self);
  }
  ctx.runtimeBase = h;
  const ModuleGraph& g = *m.graph;
  for (const Module& mod : g.modules) {
    ctx.sighById[mod.id] = sighash(mod);
    std::string text;
    if (!run_cache::readFileText(mod.path, text)) {
      if (mod.id == g.entry && entrySource) text = *entrySource;
      else text = "<missing>";
    }
    ctx.sourceById[mod.id] = std::move(text);
  }
  {
    std::uint64_t gh = 1469598103934665603ULL;
    gh = run_cache::mixBytes(gh, "graph1");
    gh = run_cache::mixInt(gh, static_cast<std::uint64_t>(g.entry));
    gh = run_cache::mixInt(gh, static_cast<std::uint64_t>(g.preludeId));
    for (const Module& mod : g.modules) {
      gh = run_cache::mixBytes(gh, "G:" + absCachePath(mod.path));
      gh = run_cache::mixInt(gh, scopeNamesHash(mod));
    }
    ctx.graphKey = gh;
  }
  return ctx;
}
inline std::string unitKeyWithCtx(CompiledModule& m, const KeyContext& ctx,
                                  bool testMode,
                                  const std::string* testFilter, int moduleId,
                                  const std::set<int>& deps) {
  std::uint64_t h = ctx.runtimeBase;
  h = run_cache::mixInt(h, ctx.graphKey);
  const ModuleGraph& g = *m.graph;
  const Module& mod = g.at(moduleId);
  h = run_cache::mixBytes(h, "M:" + absCachePath(mod.path));
  auto sit = ctx.sourceById.find(moduleId);
  h = run_cache::mixBytes(h, sit == ctx.sourceById.end() ? "<missing>" : sit->second);
  auto sighIt = ctx.sighById.find(moduleId);
  h = run_cache::mixInt(h, sighIt == ctx.sighById.end() ? 0 : sighIt->second);
  std::vector<int> sorted(deps.begin(), deps.end());
  for (int d : sorted) {
    h = run_cache::mixBytes(h, "D:" + absCachePath(g.at(d).path));
    auto dit = ctx.sighById.find(d);
    h = run_cache::mixInt(h, dit == ctx.sighById.end() ? 0 : dit->second);
  }
  if (moduleId == g.entry) {
    // g.order is the module init-call order emitted into the entry unit;
    // it is not implied by module-id order, so hash it explicitly.
    for (int id : g.order) h = run_cache::mixInt(h, static_cast<std::uint64_t>(id));
    h = run_cache::mixInt(h, testMode ? 1 : 0);
    if (testMode) h = run_cache::mixBytes(h, testFilter ? *testFilter : "");
  }
  return hex64(h);
}
inline std::string unitCachePath(const std::string& key) {
  return objCacheDir() + "/" + key + ".o";
}

}  // namespace objcache


// Locates the A runtime C source (compiler/astra/runtime/a_runtime.c) that
// is compiled/linked with the emitted source. Candidates: $A_RUNTIME_C, next
// to the astra binary (staged astralib-<arch> copy), the source tree
// relative to the astra binary (build/astra/astra-<arch> ->
// ../../compiler/astra/runtime), and the cwd. The staged per-arch dir
// (astralib-<arch>) is preferred; plain astralib only matches the host arch
// because its libastra1.a is host-arch code.
bool findRuntime(const std::string& arch, const std::string& os,
                 std::string& astraBin, std::string& runtimeC, std::string& runtimeDir) {
  astraBin = findAstra(arch, os);
  std::vector<std::string> libnames;
  libnames.push_back("astralib-" + arch);
  if (arch == hostArch()) libnames.push_back("astralib");
  std::vector<std::string> candidates;
  if (const char* e = std::getenv("A_RUNTIME_C"); e && *e) candidates.push_back(e);
  auto addDir = [&](const std::string& d) {
    if (d.empty()) return;
    for (const auto& ln : libnames) {
      candidates.push_back(d + "/" + ln + "/a_runtime.c");
      candidates.push_back(d + "/../" + ln + "/a_runtime.c");
      candidates.push_back(d + "/../../" + ln + "/a_runtime.c");
      candidates.push_back(d + "/../../../" + ln + "/a_runtime.c");
    }
    // Source-tree fallbacks last: a staged astralib* dir for ANY libname
    // must win over the (portable but lib-less) compiler/astra/runtime dir.
    candidates.push_back(d + "/../../compiler/astra/runtime/a_runtime.c");
    candidates.push_back(d + "/../../../compiler/astra/runtime/a_runtime.c");
  };
  auto slash = astraBin.find_last_of('/');
  if (slash != std::string::npos) addDir(astraBin.substr(0, slash));
  else addDir(".");
  for (const auto& ln : libnames) {
    candidates.push_back(ln + "/a_runtime.c");
    candidates.push_back("./" + ln + "/a_runtime.c");
  }
  candidates.push_back("compiler/astra/runtime/a_runtime.c");
  candidates.push_back("./compiler/astra/runtime/a_runtime.c");
  for (const auto& c : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(c, ec)) {
      runtimeC = std::filesystem::absolute(c, ec).string();
      auto pos = runtimeC.find_last_of('/');
      runtimeDir = pos == std::string::npos ? "." : runtimeC.substr(0, pos);
      return true;
    }
  }
  return false;
}

// Stages emitted source to a temp file with the given extension.
std::string stageSrc(const std::string& source, const char* ext, std::ostream& err) {
  std::string p = tempPath("gen", ext);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) { err << "error: cannot write temp source '" << p << "'\n"; return ""; }
  f << source;
  f.flush();
  if (const char* dbg = std::getenv("A_DEBUG_SRC"); dbg && *dbg) {
    std::ofstream df(dbg, std::ios::binary | std::ios::trunc);
    if (df) df << source;
  }
  return p;
}
// ---- LLVM backend -----------------------------------------------------------
// The LLVM path lowers the checked AST straight to LLVM IR (.ll), then runs
// the system LLVM toolchain: `opt` (skipped at -O0), `llc` to produce an
// object file, and `clang` to link the object with the staged
// libaruntime.a. Host targets only — there is no cross-compile path.

// Locates an LLVM tool: A_LLVM_<NAME> env override first, then PATH
// (unversioned and -18/-17/-16 suffixed variants). Returns "" when absent.
std::string findLLVMTool(const char* envName,
                         std::initializer_list<const char*> names) {
  static std::unordered_map<std::string, std::string> cache;
  std::string key = envName;
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  std::string found;
  if (const char* e = std::getenv(envName); e && *e) {
    found = e;
  } else {
    // Bundled toolchain first: tools shipped next to the `a` executable
    // (release packages stage opt/llc/clang under llvm-tools/ or bin/)
    // win over whatever happens to be on PATH.
    const std::string self = run_cache::selfExePath();
    if (!self.empty()) {
      std::string dir = self;
      auto pos = dir.find_last_of("/\\");
      if (pos != std::string::npos) {
        dir = dir.substr(0, pos);
        for (const std::string& base :
             {dir + "/llvm-tools", dir + "/bin", dir}) {
          for (const char* n : names) {
            std::string cand = base + "/" + n;
            std::error_code ec;
            if (!std::filesystem::exists(cand, ec)) continue;
#ifndef _WIN32
            if (access(cand.c_str(), X_OK) != 0) continue;
#endif
            found = cand;
            break;
          }
          if (!found.empty()) break;
        }
      }
    }
    if (found.empty()) {
    const char* pathEnv = std::getenv("PATH");
#ifdef _WIN32
    const char delim = ';';
#else
    const char delim = ':';
#endif
    std::istringstream ss(pathEnv ? pathEnv : "");
    std::string dir;
    while (found.empty() && std::getline(ss, dir, delim)) {
      if (dir.empty()) continue;
      for (const char* n : names) {
        std::string cand = dir + "/" + n;
        std::error_code ec;
        if (!std::filesystem::exists(cand, ec)) continue;
#ifndef _WIN32
        if (access(cand.c_str(), X_OK) != 0) continue;
#endif
        found = cand;
        break;
      }
    }
    }
  }
  cache.emplace(key, found);
  return found;
}
// Maps PipelineConfig.optimizationLevel to an -O flag.
std::string llvmOptFlag(int level) {
  if (level <= 0) return "-O0";
  if (level == 1) return "-O1";
  if (level == 2) return "-O2";
  return "-O3";
}
// opt -O<n> -S in.ll -o opt.ll  ->  llc -O<n> -filetype=obj opt.ll -o out.o
//   ->  clang out.o libaruntime.a -o out -lm -lpthread [-ldl].
// Returns 0 on success; the failing stage's diagnostics go to err (the
// tools' own stderr is inherited).
int llvmBuild(const std::string& arch, const std::string& llPath,
              const std::string& outPath, int optimizationLevel,
              std::ostream& err) {
#ifdef _WIN32
  (void)arch; (void)llPath; (void)outPath; (void)optimizationLevel;
  err << "error: llvm backend is not supported on Windows yet\n";
  return 1;
#else
  if (arch != hostArch()) {
    err << "error: llvm backend is host-only; cross compile unsupported"
        << " (target '" << arch << "' != host '" << hostArch() << "')\n";
    return 1;
  }
  const int level = optimizationLevel < 0 ? 0
                   : optimizationLevel > 3 ? 3 : optimizationLevel;
  const std::string optFlag = llvmOptFlag(level);
  std::string cc = findLLVMTool("A_LLVM_CC",
      {"clang", "clang-18", "clang-17", "clang-16", "clang-15",
       "clang++", "clang++-18", "clang++-17"});
  if (cc.empty()) {
    err << "error: llvm backend: no clang driver found (set A_LLVM_CC)\n";
    return 1;
  }
  std::string llc = findLLVMTool("A_LLVM_LLC",
      {"llc", "llc-18", "llc-17", "llc-16", "llc-15"});
  if (llc.empty()) {
    err << "error: llvm backend: no llc found (set A_LLVM_LLC)\n";
    return 1;
  }
  std::string cur = llPath;
  std::string optPath, objPath;
  std::error_code ec;
  auto cleanup = [&]() {
    if (!optPath.empty()) std::filesystem::remove(optPath, ec);
    if (!objPath.empty()) std::filesystem::remove(objPath, ec);
  };
  // Stage 1: opt (skipped at -O0).
  if (level > 0) {
    std::string opt = findLLVMTool("A_LLVM_OPT",
        {"opt", "opt-18", "opt-17", "opt-16", "opt-15"});
    if (opt.empty()) {
      err << "error: llvm backend: no opt found (set A_LLVM_OPT)\n";
      return 1;
    }
    optPath = tempPath("llvm", "x.ll");
    if (runChild(opt, {optFlag, "-S", cur, "-o", optPath}) != 0) {
      err << "error: llvm backend: opt failed on '" << llPath << "'\n";
      cleanup();
      return 1;
    }
    cur = optPath;
  }
  // Stage 2: llc -> object file.
  objPath = tempPath("llvm", "x.o");
  if (runChild(llc, {optFlag, "-filetype=obj", cur, "-o", objPath}) != 0) {
    err << "error: llvm backend: llc failed\n";
    cleanup();
    return 1;
  }
  // Stage 3: link the object with the A runtime archive.
  std::string astraBin, runtimeC, runtimeDir;
  if (!findRuntime(arch, hostOs(), astraBin, runtimeC, runtimeDir)) {
    err << "error: llvm backend: cannot locate A runtime (a_runtime.c)\n";
    cleanup();
    return 1;
  }
  std::vector<std::string> rtCandidates = {
      runtimeDir + "/libaruntime.a",
      runtimeDir + "/../libaruntime.a",
      runtimeDir + "/../../libaruntime.a",
      "compiler/astra/runtime/libaruntime.a",
      "./compiler/astra/runtime/libaruntime.a",
  };
  std::string rtLib;
  for (const auto& c : rtCandidates) {
    std::error_code ec2;
    if (std::filesystem::exists(c, ec2)) { rtLib = c; break; }
  }
  if (rtLib.empty()) {
    err << "error: llvm backend: cannot locate libaruntime.a\n";
    cleanup();
    return 1;
  }
  // llc emits a non-PIC object; -no-pie keeps the link consistent with it.
  std::vector<std::string> args = {objPath, rtLib, "-o", outPath,
                                   "-no-pie", "-lm", "-lpthread"};
#ifndef __APPLE__
  args.push_back("-ldl");
#endif
  int rc = runChild(cc, args);
  cleanup();
  if (rc != 0) err << "error: llvm backend: clang link failed\n";
  return rc == 0 ? 0 : 1;
#endif
}

int compileAndRun(CompiledModule& m, std::istream& in, std::ostream& out, std::ostream& err,
                  bool testMode = false, int optimizationLevel = 3,
                  const std::string* keepBin = nullptr,
                  const TargetDesc* td = nullptr,
                  const std::string* entrySource = nullptr,
                  const std::string* testFilter = nullptr, int jobs = 0,
                  const std::string& backend = "llvm") {
  (void)in; (void)out; (void)jobs;  // jobs: per-module split assemble removed with the asm backend
  if (std::getenv("A_LLVM_DEBUG"))
    err << "compileAndRun backend='" << backend << "'\n";
  TargetDesc localTd;
  if (!td) {
    if (!resolveTarget("", localTd, err)) return kCErr;
    td = &localTd;
  }
  // Run cache: a hit skips emit+link entirely (~2ms exec vs ~25ms build).
  run_cache::Plan cache;
  if ((!keepBin || keepBin->empty()) && td->native) {
    cache = run_cache::plan(m, *td, testMode, optimizationLevel, entrySource,
                            testFilter);
    std::error_code ec;
    if (cache.active && std::filesystem::exists(cache.file, ec)) {
      int hit = runBinary(cache.file);
      if (hit != 127) return hit < 0 ? kCErr : hit;
      // Unusable stale entry: fall through and recompile.
    }
  }
  const char* forced = std::getenv("A_CODEGEN");
  if (forced && lowerStr(forced) == "c") {
    err << "error: A_CODEGEN=c is no longer supported: the C emitter was "
           "removed — the LLVM backend is the only native backend\n";
    return kCErr;
  }
  // LLVM is the only native backend and is host-only: a declined emission
  // or a toolchain failure is a hard error — there is no asm fallback.
  if (!td->native || td->arch != hostArch()) {
    err << "error: llvm backend is host-only; cross compile unsupported"
        << " (target '" << td->arch << "' != host '" << hostArch() << "')\n";
    return kCErr;
  }
  if (std::getenv("A_LLVM_DEBUG"))
    err << "td.native=" << td->native << " arch=" << td->arch
        << " host=" << hostArch() << "\n";
  {
    std::string llSource, llError;
    if (!emitLLVM(*m.graph, *m.sema, llSource, llError, testMode,
                  testFilter)) {
      err << "error: llvm backend: LLVM IR emission failed: " << llError
          << "\n";
      return kCErr;
    }
    if (std::getenv("A_LLVM_DEBUG"))
      err << "llvm-emit: ok\n";
    std::string llPath = stageSrc(llSource, ".ll", err);
    if (llPath.empty()) return kCErr;
    std::string tmpBin = tempPath("bin");
    std::string runPath = (keepBin && !keepBin->empty()) ? *keepBin : tmpBin;
    // On a cache miss compile to a temp name in the cache dir so the rename
    // below is atomic and same-filesystem.
    if (cache.active) {
      std::error_code ec;
      std::string cdir = std::filesystem::path(cache.file).parent_path().string();
      std::filesystem::create_directories(cdir, ec);
      runPath = cdir + "/tmp-" +
                std::to_string(::getpid()) + "-" + std::to_string(::rand());
    }
    int rc = llvmBuild(td->arch, llPath, runPath, optimizationLevel, err);
    if (rc != 0 && std::getenv("A_LLVM_DEBUG"))
      err << "llvm-cc failed; kept " << llPath << "\n";
    else {
      std::error_code ec;
      std::filesystem::remove(llPath, ec);
    }
    if (cache.active && rc == 0) {
      std::error_code ec2;
      std::filesystem::rename(runPath, cache.file, ec2);
      if (!ec2) runPath = cache.file;
    }
    std::error_code ec;
    if (rc != 0) {
      std::filesystem::remove(runPath, ec);
      return kCErr;
    }
    rc = runBinary(runPath);
    // Delete only temp artifacts; a filled cache entry must stay.
    if (cache.active && runPath == cache.file) {
      // keep
    } else if (!keepBin) {
      std::filesystem::remove(runPath, ec);
    }
    return rc < 0 ? kCErr : rc;
  }
}
}

CompiledModule runFrontendPipeline(const std::string& path,std::string source,const PipelineConfig& config,DiagnosticSink& sink,std::ostream& diagOut,std::ostream& diagErr){
  ModuleLoaderOptions opts; opts.modulePaths=config.modulePaths; opts.stdlibRoot=config.stdlibRoot;
  ModuleLoader loader(std::move(opts),sink); ModuleGraph graph=loader.load(path,std::move(source));
  if(!flushErrors(sink,config,diagOut,diagErr)) return {};
  // Comptime metaprogramming: specialize `comptime` parameters at call sites
  // and resolve `comptime if`/`comptime for` before sema sees the AST, so all
  // backends get ordinary functions and statements.
  runComptimePass(graph,sink); if(!flushErrors(sink,config,diagOut,diagErr)) return {};
  Sema sema(graph,sink); SemaResult result = config.jobs > 1 ? sema.analyzeParallel(config.jobs) : sema.analyze(); if(!flushErrors(sink,config,diagOut,diagErr)) return {};
  analyzeOwnership(graph,sink); if(!flushErrors(sink,config,diagOut,diagErr)) return {};
  flushFinal(sink,config,diagOut,diagErr);
  CompiledModule m; m.graph=std::make_unique<ModuleGraph>(std::move(graph)); m.sema=std::make_unique<SemaResult>(std::move(result)); m.ok=true; return m;
}
int executePipeline(const std::string& path,std::string source,const PipelineConfig& config,std::istream& in,std::ostream& out,std::ostream& err){
  // Execution: the frontend lowers the AST to LLVM IR, the LLVM
  // toolchain (opt/llc/clang) builds a binary linked with the runtime,
  // and the binary runs as a child process — host targets only.
  TargetDesc td;
  if(!resolveTarget(config.target,td,err)) return kCErr;
  if(!td.native){ err<<"error: cannot execute target '"<<td.arch<<"' on this host ('"<<hostArch()<<"'); use 'a build' to cross-compile\n"; return kCErr; }
  // The VM interprets host-arch semantics only; a runnable-but-different
  // arch (e.g. i386 on x86_64) is still not the host target.
  if(config.vm && td.arch!=hostArch()){ err<<"error: --vm runs on the host target only ('"<<td.arch<<"' != '"<<hostArch()<<"')\n"; return kCErr; }
  if(config.checkOnly){
    DiagnosticSink sink; auto m=runFrontendPipeline(path,source,config,sink,out,err); if(!m.ok) return kCErr; return kOk;
  }
  // Pre-parse cache hit: skip the frontend entirely and exec the cached
  // binary (depfile + entry-content key, see run_cache::precheckKey).
  // Compile-time diagnostics (warnings) are observable behaviour, so they
  // are recorded alongside the binary on a miss and replayed on a hit.
  std::string preBin;
  if(td.native && !config.vm){
    preBin = run_cache::precheckKey(path, source, config, td, config.optimizationLevel);
    if(!preBin.empty()){
      std::error_code ec;
      if(std::filesystem::exists(preBin, ec)){
        std::string diag;
        if(run_cache::readFileText(preBin + ".diag", diag) && !diag.empty()) err << diag;
#ifndef _WIN32
        // Fast handoff: replace this process with the cached binary. The
        // shell observes the target's own exit code (and signals) exactly
        // as with fork+wait, minus a second process lifecycle; stdin and
        // stdout/stderr are inherited across execve. On execve failure
        // (e.g. a noexec cache mount) fall through to the fork path.
        err.flush();
        ::execl(preBin.c_str(), preBin.c_str(), static_cast<char*>(nullptr));
#endif
        int hit = runBinary(preBin);
        if(hit != 127) return hit < 0 ? kCErr : hit;
        std::filesystem::remove(preBin, ec);
        std::filesystem::remove(preBin + ".diag", ec);
      }
      std::error_code ec2;
      std::filesystem::create_directories(std::filesystem::path(preBin).parent_path(), ec2);
    }
  }
  std::string diagCapture;
  {
    std::ostringstream diagStream;
    DiagnosticSink sink; auto m=runFrontendPipeline(path,source,config,sink,out,diagStream); if(!m.ok){ err << diagStream.str(); return kCErr; }
    // Route frontend diagnostics through diagStream so the run-cache .diag
    // sidecar can replay them on precheck hits.
    diagCapture = diagStream.str();
    err << diagCapture;
    // --vm: skip all native caching/emit; the interpreter runs the checked
    // module graph in-process and its return value is the process exit code.
    if(config.vm) {
      int rc = vm::runProgram(*m.graph,*m.sema,false,config.testFilter);
      if (rc != vm::kVmFallbackToNative) return rc;
    }
    if(!preBin.empty()){
      std::error_code ec;
      std::ofstream sidecar(preBin + ".diag", std::ios::binary | std::ios::trunc);
      if(sidecar) sidecar << diagCapture;
    }
    return compileAndRun(m,in,out,err,false,config.optimizationLevel,
                       preBin.empty() ? nullptr : &preBin, &td, &source, nullptr,
                       config.jobs, config.backend);
  }
}
int testPipeline(const std::string& path,const PipelineConfig& config,std::istream& in,std::ostream& out,std::ostream& err){
  std::string src; if(!readAll(path,src,err)) return kNoIn;
  std::string srcCopy = src;
  DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,out,err); if(!m.ok) return kCErr;
  // `a test`: discover and run top-level `fn test_*()` functions.
  TargetDesc td;
  if(!resolveTarget(config.target,td,err)) return kCErr;
  if(!td.native){ err<<"error: cannot execute target '"<<td.arch<<"' on this host ('"<<hostArch()<<"'); use 'a build' to cross-compile\n"; return kCErr; }
  if(config.vm && td.arch!=hostArch()){ err<<"error: --vm runs on the host target only ('"<<td.arch<<"' != '"<<hostArch()<<"')\n"; return kCErr; }
  // --filter with no matching test_* function: report and exit nonzero
  // instead of compiling a binary that would run zero tests — a typo'd
  // filter must not pass CI silently. Mirrors the emitter's discovery
  // predicate (top-level FnDecl in the entry module).
  if(!config.testFilter.empty()){
    bool any=false;
    const ModuleGraph& g=*m.graph;
    if(g.entry>=0 && g.entry<(int)g.modules.size()){
      const Module& mod=g.at(g.entry);
      if(mod.program){
        for(const StmtPtr& s:mod.program->statements){
          if(s->kind==StmtKind::FnDecl){
            const FnDecl* f=static_cast<const FnDecl*>(s.get());
            if(f->name.rfind("test_",0)==0 &&
               f->name.find(config.testFilter)!=std::string::npos){ any=true; break; }
          }
        }
      }
    }
    if(!any){
      if(config.scanIgnoreFilterMismatch) return 3; // kTestSkip: no matching tests in this file (scan mode)
      err<<"a test: no tests matched --filter '"<<config.testFilter<<"'\n"; return kCErr;
    }
  }
  // test_* functions are invoked by the runtime harness as f(argv) with no
  // declared arguments; a parameter would silently read a padded A_NONE
  // slot. Sema cannot see test mode, so reject them here — same predicate
  // as the emitter's discovery (top-level FnDecl in the entry module).
  {
    const ModuleGraph& g=*m.graph;
    if(g.entry>=0 && g.entry<(int)g.modules.size()){
      const Module& mod=g.at(g.entry);
      if(mod.program){
        for(const StmtPtr& s:mod.program->statements){
          if(s->kind!=StmtKind::FnDecl) continue;
          const FnDecl* f=static_cast<const FnDecl*>(s.get());
          if(f->name.rfind("test_",0)!=0) continue;
          if(f->params.empty() && f->comptimeParams.empty()) continue;
          Span sp=f->nameSpan.valid()?f->nameSpan:f->span;
          sink.error(sp,
                     "Test function '"+f->name+"' must not declare parameters.",
                     "The test harness calls test_* functions with no arguments.");
        }
      }
    }
    if(sink.hasErrors()){ sink.flush(err,config.useColor); return kCErr; }
  }
  // --vm: the interpreter runs test_* functions in-process; skip emit/link.
  if(config.vm) {
      int rc = vm::runProgram(*m.graph,*m.sema,true,config.testFilter);
      if (rc != vm::kVmFallbackToNative) return rc;
    }
  return compileAndRun(m,in,out,err,true,config.optimizationLevel,nullptr,&td,&srcCopy,
                       config.testFilter.empty()?nullptr:&config.testFilter, config.jobs,
                       config.backend);
}
int buildPipeline(const std::string& path,const std::string& outputPath,const PipelineConfig& config,std::ostream& err){
  // Defensive: the CLI rejects `build --vm`; a build always emits a native
  // binary, so the VM flag must never reach codegen here.
  if(config.vm){ err<<"error: --vm cannot be used with 'build'\n"; return kCErr; }
  // Guard `a build x.a -o x.a`: the linker would overwrite the source file with
  // the ELF. Compare canonical paths so `./x.a`, `sub/../x.a` and symlinks
  // are caught too.
  {
    std::error_code eci, eco;
    std::filesystem::path inAbs = std::filesystem::absolute(path, eci);
    std::filesystem::path outAbs = std::filesystem::absolute(outputPath, eco);
    bool same;
    if (!eci && !eco) {
      std::error_code eci2, eco2;
      std::filesystem::path inC = std::filesystem::weakly_canonical(inAbs, eci2);
      std::filesystem::path outC = std::filesystem::weakly_canonical(outAbs, eco2);
      same = (!eci2 && !eco2)
                 ? (inC == outC)
                 : (inAbs.lexically_normal() == outAbs.lexically_normal());
    } else {
      same = inAbs.lexically_normal() == outAbs.lexically_normal();
    }
    if (same) {
      err << "error: output path '" << outputPath << "' equals input file '"
          << path << "' — refusing to overwrite the source\n";
      return kCErr;
    }
  }
  std::string src; if(!readAll(path,src,err)) return kNoIn;
  DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,err,err); if(!m.ok) return kCErr;
  // `a build`: the LLVM backend is the only native backend and is
  // host-only — a non-host target is a hard error.
  TargetDesc td;
  if(!resolveTarget(config.target,td,err)) return kCErr;
  const char* forced = std::getenv("A_CODEGEN");
  if (forced && lowerStr(forced) == "c") {
    err << "error: A_CODEGEN=c is no longer supported: the C emitter was "
           "removed — the LLVM backend is the only native backend\n";
    return kCErr;
  }
  if (!td.native || td.arch != hostArch()) {
    err << "error: llvm backend is host-only; cross compile unsupported"
        << " (target '" << td.arch << "' != host '" << hostArch() << "')\n";
    return kCErr;
  }
  std::string llSource, llError;
  if (!emitLLVM(*m.graph, *m.sema, llSource, llError, false, nullptr)) {
    err << "error: llvm backend: LLVM IR emission failed: " << llError
        << "\n";
    return kCErr;
  }
  std::string llPath = stageSrc(llSource, ".ll", err);
  if (llPath.empty()) return kCErr;
  int rc = llvmBuild(td.arch, llPath, outputPath,
                     config.optimizationLevel, err);
  std::error_code ec;
  std::filesystem::remove(llPath, ec);
  if (rc != 0) {
    std::filesystem::remove(outputPath, ec);
    return kCErr;
  }
  return 0;
}
ReplSession::~ReplSession() = default;
int replEval(ReplSession& session,const std::string& source,const PipelineConfig& config,std::istream& in,std::ostream& out,std::ostream& err){
  (void)session;
  DiagnosticSink sink; auto m=runFrontendPipeline("<repl>",source,config,sink,out,err); if(!m.ok) return 1;
  // --vm repl: run each eval in the interpreter; no target resolution or
  // native emit is needed (VM is host-only by construction).
  if(config.vm) {
    int rc = vm::runProgram(*m.graph,*m.sema,false,"");
    if (rc != vm::kVmFallbackToNative) return rc;
  }
  TargetDesc td;
  if(!resolveTarget(config.target,td,err)) return 1;
  if(!td.native){ err<<"error: repl requires the host target\n"; return 1; }
  return compileAndRun(m,in,out,err,false,config.optimizationLevel,nullptr,&td,&source,nullptr,config.jobs,config.backend);
}

namespace {
bool shouldSkipTestDir(const std::string& name){
  if(name.empty()) return false;
  if(name[0]=='.') return true; // .git, .cache, .apm, hidden
  if(name=="build" || name=="node_modules" || name=="third_party") return true;
  if(name.rfind("cmake-build",0)==0) return true;
  if(name.rfind("build-",0)==0) return true;
  return false;
}
bool fileHasTestDecl(const std::filesystem::path& path){
  std::ifstream f(path, std::ios::binary);
  if(!f) return false;
  std::string line;
  while(std::getline(f,line)){
    // skip line comments (common case) to reduce false positives
    size_t i=0; while(i<line.size() && (line[i]==' '||line[i]=='\t')) ++i;
    if(i<line.size() && line[i]=='#') continue;
    // look for: fn <spaces/tabs> test_
    for(size_t p=0; (p=line.find("fn", p))!=std::string::npos; ++p){
      size_t j=p+2;
      if(p>0){ char c=line[p-1]; if(std::isalnum((unsigned char)c)||c=='_') continue; }
      while(j<line.size() && (line[j]==' '||line[j]=='\t')) ++j;
      if(line.compare(j,5,"test_")==0){
        char after = (j+5<line.size()) ? line[j+5] : '\0';
        if(after=='\0' || std::isalnum((unsigned char)after) || after=='_' || after=='(' || after=='<' )
          return true;
      }
    }
  }
  return false;
}
std::vector<std::filesystem::path> discoverTestFiles(const std::filesystem::path& root, std::ostream& err){
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(root,
      std::filesystem::directory_options::skip_permission_denied, ec), end;
  if(ec){ err<<"error: cannot scan '"<<root.string()<<"': "<<ec.message()<<"\n"; return out; }
  for(; it!=end; it.increment(ec)){
    if(ec){ ec.clear(); continue; }
    const auto& entry=*it;
    if(entry.is_directory(ec)){
      if(ec){ ec.clear(); continue; }
      if(shouldSkipTestDir(entry.path().filename().string())) it.disable_recursion_pending();
      continue;
    }
    if(ec){ ec.clear(); continue; }
    if(!entry.is_regular_file(ec)){ ec.clear(); continue; }
    const auto& p=entry.path();
    if(p.extension()==".a" && fileHasTestDecl(p)) out.push_back(p);
  }
  std::sort(out.begin(), out.end());
  return out;
}
bool parseTestSummary(const std::string& s, int& passed, int& failed){
  // match "<int> passed, <int> failed" (last occurrence)
  const std::string needle = " passed, ";
  size_t pos = std::string::npos, search = 0;
  while(true){
    size_t f = s.find(needle, search);
    if(f==std::string::npos) break;
    pos = f; search = f + needle.size();
  }
  if(pos==std::string::npos) return false;
  size_t nstart = pos;
  while(nstart>0 && std::isdigit((unsigned char)s[nstart-1])) --nstart;
  size_t fend = pos + needle.size();
  size_t fstart = fend;
  while(fend<s.size() && std::isdigit((unsigned char)s[fend])) ++fend;
  if(fend + 7 > s.size() || s.compare(fend, 7, " failed")!=0) return false;
  try {
    passed = std::stoi(s.substr(nstart, pos-nstart));
    failed = std::stoi(s.substr(fstart, fend-fstart));
    return true;
  } catch(...) { return false; }
}
} // namespace

int testPath(const std::string& path, const PipelineConfig& config, std::istream& in, std::ostream& out, std::ostream& err){
  const std::string target = path.empty() ? "." : path;
  std::error_code ec;
  if(std::filesystem::is_regular_file(target, ec)){
    return testPipeline(target, config, in, out, err);
  }
  if(ec && ec!=std::errc::no_such_file_or_directory){
    err<<"error: cannot access '"<<target<<"': "<<ec.message()<<"\n"; return kCErr;
  }
  if(!std::filesystem::is_directory(target, ec)){
    // let testPipeline produce the canonical open/diagnostic error
    return testPipeline(target, config, in, out, err);
  }
  auto files = discoverTestFiles(std::filesystem::path(target), err);
  // discoverTestFiles only prints on scan errors; empty is valid to report below
  if(files.empty()){
    err<<"a test: no test files found in '"<<target<<"'\n";
    if(!config.testFilter.empty()) err<<"  (filter: '"<<config.testFilter<<"')\n";
    return kCErr;
  }
  PipelineConfig scanCfg = config;
  scanCfg.scanIgnoreFilterMismatch = true;
  std::filesystem::path root(target);
  int totalPassed=0, totalFailed=0, fileErrors=0, filesRun=0, filesSkipped=0;
  for(const auto& f : files){
    std::ostringstream fout, ferr;
    std::string capturedOut, capturedErr;
    int rc = 0;
#if defined(__unix__) || defined(__APPLE__)
    int outPipe[2] = {-1,-1}, errPipe[2] = {-1,-1};
    int savedOut = -1, savedErr = -1;
    bool captured = false;
    if(::pipe(outPipe)==0 && ::pipe(errPipe)==0){
      savedOut = ::dup(STDOUT_FILENO);
      savedErr = ::dup(STDERR_FILENO);
      ::dup2(outPipe[1], STDOUT_FILENO);
      ::dup2(errPipe[1], STDERR_FILENO);
      ::close(outPipe[1]); ::close(errPipe[1]);
      outPipe[1]=errPipe[1]=-1;
      rc = testPipeline(f.string(), scanCfg, in, fout, ferr);
      ::fflush(stdout); ::fflush(stderr);
      ::dup2(savedOut, STDOUT_FILENO);
      ::dup2(savedErr, STDERR_FILENO);
      ::close(savedOut); ::close(savedErr);
      char buf[4096]; ssize_t n;
      while((n=::read(outPipe[0], buf, sizeof(buf)))>0) capturedOut.append(buf, (size_t)n);
      while((n=::read(errPipe[0], buf, sizeof(buf)))>0) capturedErr.append(buf, (size_t)n);
      ::close(outPipe[0]); ::close(errPipe[0]);
      captured = true;
    } else {
      if(outPipe[0]>=0) ::close(outPipe[0]);
      if(outPipe[1]>=0) ::close(outPipe[1]);
      if(errPipe[0]>=0) ::close(errPipe[0]);
      if(errPipe[1]>=0) ::close(errPipe[1]);
    }
    if(!captured)
#endif
    {
      rc = testPipeline(f.string(), scanCfg, in, fout, ferr);
      capturedOut = fout.str(); capturedErr = ferr.str();
    }
#if defined(__unix__) || defined(__APPLE__)
    if(captured){
      capturedOut += fout.str();
      capturedErr += ferr.str();
    }
#endif
    if(rc == 3){ // filter matched no tests in this file
      filesSkipped++;
      continue;
    }
    filesRun++;
    std::error_code rec;
    std::filesystem::path rel = std::filesystem::relative(f, root, rec);
    std::string display = (rec ? f : rel).string();
    out << "\n== " << display << " ==\n";
    out << capturedOut;
    if(!capturedOut.empty() && capturedOut.back()!='\n') out << "\n";
    err << capturedErr;
    int p=0, fl=0;
    if(parseTestSummary(capturedOut, p, fl)){
      totalPassed += p;
      totalFailed += fl;
      if(rc != 0 && fl == 0) fileErrors++; // nonzero without counted failures -> error
    } else {
      if(rc != 0) fileErrors++;
    }
  }
  out << "\n== summary ==\n";
  out << totalPassed << " passed, " << totalFailed << " failed";
  if(fileErrors) out << ", " << fileErrors << " file error(s)";
  out << " in " << filesRun << " file(s)";
  if(filesSkipped) out << " (" << filesSkipped << " skipped by filter)";
  if(filesRun==0 && !config.testFilter.empty()){
    out << "\n";
    err << "a test: no tests matched --filter '" << config.testFilter << "'\n";
    return kCErr;
  }
  out << "\n";
  return (totalFailed==0 && fileErrors==0) ? kOk : kCErr;
}
}
