#include "pipeline/pipeline.hpp"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
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
#include <vector>
#include "diagnostics/diagnostics.hpp"
#include "codegen/codegen.hpp"
#include "ir/ir.hpp"
#include "driver/version.hpp"
#include "name-resolution/module_loader.hpp"
#include "optimization/optimizer.hpp"
#include "ownership/ownership.hpp"
#include "type-checker/sema.hpp"
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
  std::ifstream f(p,std::ios::binary); if(!f){e<<"error: cannot open '"<<p<<"'\n"; return false;}
  std::ostringstream b; b<<f.rdbuf(); if(f.bad()){e<<"error: failed to read '"<<p<<"'\n"; return false;} t=b.str(); return true;
}

// ---------------------------------------------------------------- Astra exec
// `a run`/`a build`/`a test` are thin wrappers over the Astra toolchain
// driver (compiler/astra/): the frontend emits target assembly (x86-64
// SysV via emitAsm, other arches via emitNative), `astra` assembles that
// together with the C A runtime (a_runtime.c) to machine code and links
// the executable. There is no C-source intermediate: every target goes
// A -> asm -> machine code.
//
// Cross-compilation: astra ships one driver binary per target architecture
// (astra-<arch>, built by cmake/libastra.cmake). The frontend picks the
// emitter by target: x86-64 SysV targets use emitAsm; every other arch
// goes through emitNative (native per-arch assembly). i8086 is
// asm-only — astra rejects C input for it. Astra
// binaries are bound to the host OS at build time, so the target OS must
// match the host — only the architecture is selectable.

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
  std::string arch;    // canonical astra arch name
  std::string os;      // always the host OS (astra cannot cross OSes)
  bool native = false; // arch == host arch
  bool useAsm = false; // x86-64 SysV: the asm emitter is valid
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
  // OS tokens that disagree with the host OS are rejected — astra drivers
  // are built per-OS and cannot cross operating systems. Exception:
  // 'windows' is allowed as an explicit cross target because the astra
  // PE backend (ASTRA_TARGET_PE) can be built as a host-OS binary that
  // emits PE (e.g. an ELF astra-x86_64-windows on Linux); the driver is
  // located via A_ASTRA_<ARCH>_WINDOWS / astra-<arch>-windows and the
  // produced .exe cannot run on this host (td.native stays false).
  std::string reqOs;
  if (!osTok.empty() && osTok != t.os) {
    if (osTok == "windows") {
      reqOs = "windows";
    } else {
      err << "error: target OS '" << osTok << "' does not match host OS '" << t.os
          << "' — astra drivers are built per-OS and cannot cross operating systems\n";
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
  // The asm emitter is x86-64 SysV only: valid for ELF (linux) and Mach-O
  // (macos) x86-64 targets. i386, ARM, RISC-V, i8086 and Windows x86-64
  // (MS ABI) go through emitNative. A declined emission is a hard error.
  t.useAsm = (arch == "x86_64" && (t.os == "linux" || t.os == "macos"));
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
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n > 0) {
    exe[n] = '\0';
    std::string dir = exe;
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

// fork/exec `astra` for `arch`/`os` with the given arguments; the child
// inherits our stdin/stdout/stderr. Returns the exit code, 127 on exec
// failure.
int runAstra(const std::string& arch, const std::string& os,
             const std::vector<std::string>& args){
  return runChild(findAstra(arch, os),args);
}

std::vector<std::string> splitArgs(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string tok;
  while (in >> tok) out.push_back(tok);
  return out;
}

bool runtimeCcDisabled(const char* v) {
  if (!v || !*v) return false;
  std::string s = lowerStr(v);
  return s == "0" || s == "off" || s == "no" || s == "none" || s == "astra";
}

// Host-target builds can use the system C compiler for the runtime archive.
// Astra's C frontend is a tiny-compiler fork and emits unoptimized frame code;
// GCC/Clang produce substantially better code for the numeric runtime kernels.
// Cross targets still use astra because they need the target libc/sysroot.
bool buildRuntimeArchiveCC(const std::string& arch, const std::string& runtimeDir,
                           const std::vector<std::string>& rtSrcs,
                           const std::string& rtArchive) {
  if (arch != hostArch() || hostOs() == "windows" || rtSrcs.empty()) return false;
  if (runtimeCcDisabled(std::getenv("A_RUNTIME_CC"))) return false;
  const char* ccEnv = std::getenv("A_RUNTIME_CC");
  std::string cc = (ccEnv && *ccEnv) ? ccEnv : "cc";
  if (runChild(cc, {"--version"}) != 0) return false;

  std::vector<std::string> cflags;
  const char* flagEnv = std::getenv("A_RUNTIME_CFLAGS");
  if (flagEnv && *flagEnv) {
    cflags = splitArgs(flagEnv);
  } else {
    cflags = {"-O3", "-std=gnu11", "-w", "-fno-semantic-interposition"};
    if (arch == "x86_64" || arch == "i386") {
      cflags.push_back("-march=native");
      cflags.push_back("-mtune=native");
    }
  }

  const std::string objDir = runtimeDir + "/rt_obj_cc";
  std::error_code ec;
  std::filesystem::create_directories(objDir, ec);
  if (ec) return false;

  std::vector<std::string> objs;
  objs.reserve(rtSrcs.size());
  for (const std::string& src : rtSrcs) {
    std::string obj = objDir + "/" +
        std::filesystem::path(src).stem().string() + ".o";
    std::vector<std::string> args = cflags;
    args.push_back("-I" + runtimeDir);
    args.push_back("-c");
    args.push_back(src);
    args.push_back("-o");
    args.push_back(obj);
    if (runChild(cc, args) != 0) return false;
    objs.push_back(obj);
  }

  std::vector<std::string> aa = {"rcs", rtArchive};
  aa.insert(aa.end(), objs.begin(), objs.end());
  if (runChild("ar", aa) == 0) return true;
  return runChild("llvm-ar", aa) == 0;
}

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
// i8086 produces a flat boot image, not a host executable. Run it under
// qemu-system-i386 with COM1 on stdio and isa-debug-exit on port 0xf4.
// The guest _entry writes 42, which QEMU reports as exit status 85; map
// that to success and propagate real failures.
int runI8086Image(const std::string& img){
  pid_t pid=fork();
  if(pid<0) return 127;
  if(pid==0){
    std::vector<std::string> args={"qemu-system-i386",
      "-drive", "format=raw,file="+img,
      "-serial", "stdio",
      "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
      "-display", "none", "-no-reboot"};
    std::vector<char*> av;
    for(auto& s:args) av.push_back(const_cast<char*>(s.c_str()));
    av.push_back(nullptr);
    execvp(av[0], av.data());
    _exit(127);
  }
  int status=0;
  while(waitpid(pid,&status,0)<0){ if(errno!=EINTR) return 127; }
  if(WIFEXITED(status)){ int rc=WEXITSTATUS(status); return rc==85?0:rc; }
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
// `a run`/`a test`/REPL recompile the same source (via astra) on every
// invocation, costing ~20ms even for a noop program, while the cached two-
// phase-free binary execs in ~2ms like a native binary. The cache is
// content-addressed: every module source, the runtime files, the astra
// binary, target, opt level, codegen-forcing env vars and the toolchain
// version feed one FNV-1a key, so any edit invalidates. Disable with
// A_NO_CACHE=1; override the directory with A_CACHE_DIR.

// Forward declaration for findRuntime (defined below); run_cache::plan
// needs the resolved astra/runtime paths.
bool findRuntime(const std::string& arch, const std::string& os,
                 std::string& astraBin, std::string& runtimeC,
                 std::string& runtimeDir);

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

// Runtime C/headers are compiled into every binary, so a change to any of
// them must invalidate cached binaries: without this, editing net.c or
// bytes_conv.c and re-running `a run` silently reused the old executable.
inline void mixRuntimeSources(std::uint64_t& h, const std::string& runtimeC) {
  if (runtimeC.empty()) return;
  std::error_code ec;
  std::filesystem::path src = std::filesystem::path(runtimeC).parent_path();
  if (src.empty()) return;
  std::vector<std::string> files;
  std::function<void(const std::filesystem::path&, int)> collect =
      [&](const std::filesystem::path& dir, int depth) {
    std::error_code dec;
    std::filesystem::directory_iterator it(
        dir, std::filesystem::directory_options::skip_permission_denied, dec);
    if (dec) return;
    for (std::filesystem::directory_iterator end; it != end; it.increment(dec)) {
      if (dec) return;
      std::error_code sec;
      const std::filesystem::path p = it->path();
      std::filesystem::file_status st = it->symlink_status(sec);
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
  for (const std::string& f : files) h = mixFileStat(h, f);
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
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return std::string();
  buf[n] = '\0';
  return std::string(buf);
#endif
}

struct Plan {
  bool active = false;
  std::string file;  // final cache file path
};

inline std::string cacheDir() {
  if (const char* d = std::getenv("A_CACHE_DIR"); d && *d) return d;
#ifdef _WIN32
  char tmp[MAX_PATH];
  DWORD n = GetTempPathA(MAX_PATH, tmp);
  return std::string(n > 0 && n < MAX_PATH ? tmp : ".\\") + "a_cache_run";
#else
  if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x)
    return std::string(x) + "/a/run";
  if (const char* home = std::getenv("HOME"); home && *home)
    return std::string(home) + "/.cache/a/run";
  return "/tmp/a_cache_run";
#endif
}

inline bool disabled() {
  const char* v = std::getenv("A_NO_CACHE");
  return v && *v && lowerStr(v) != "0";
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
  const char* rcc = std::getenv("A_RUNTIME_CC");
  h = mixBytes(h, rcc ? rcc : "");
  const char* rcflags = std::getenv("A_RUNTIME_CFLAGS");
  h = mixBytes(h, rcflags ? rcflags : "");
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
  // Module sources: entry first-class (inline "<repl>"/"-c" sources come
  // from entrySource), every other module must read from disk.
  const ModuleGraph& g = *m.graph;
  for (const Module& mod : g.modules) {
    h = mixBytes(h, mod.path);
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
  p.file = cacheDir() + "/" + hex;
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
  s.h = mixBytes(s.h, "<absent>:" + p);
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
  s.h = mixBytes(s.h, path);
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
  const char* rcc = std::getenv("A_RUNTIME_CC");
  h = mixBytes(h, rcc ? rcc : "");
  const char* rcflags = std::getenv("A_RUNTIME_CFLAGS");
  h = mixBytes(h, rcflags ? rcflags : "");
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
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx",
                static_cast<unsigned long long>(h2));
  return cacheDir() + "/pre-" + hex;
}

}  // namespace run_cache

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

// Debian-style target triplet for sysroot discovery. Empty for the host
// arch (host headers/libs are already on the default search path).
std::string targetTriplet(const std::string& arch) {
  if (arch == "i386") return "i386-linux-gnu";
  if (arch == "arm64") return "aarch64-linux-gnu";
  if (arch == "riscv64") return "riscv64-linux-gnu";
  if (arch == "arm") return "arm-linux-gnueabihf";
  if (arch == "riscv32") return "riscv32-linux-gnu";
  return "";
}

// Cross-target header/library search dirs for `arch`, mirroring what a
// gcc cross toolchain would use. A_SYSROOT_<ARCH> (e.g. A_SYSROOT_AARCH64)
// or A_SYSROOT points at an explicit sysroot root and replaces
// auto-detection; otherwise the Debian multiarch layouts installed by
// gcc-*-linux-gnu / *-linux-gnu libc dev packages are probed.
void crossSearchDirs(const std::string& arch, std::vector<std::string>& includes,
                     std::vector<std::string>& libs) {
  std::string triplet = targetTriplet(arch);
  if (triplet.empty()) return;
  std::string envName = "A_SYSROOT_";
  for (char c : arch) envName += (c == '-' ? '_' : (char)::toupper((unsigned char)c));
  const char* env = std::getenv(envName.c_str());
  if (!env || !*env) env = std::getenv("A_SYSROOT");
  auto isDir = [](const std::string& d) {
    std::error_code ec; return std::filesystem::is_directory(d, ec);
  };
  auto add = [&isDir](std::vector<std::string>& v, const std::string& d) {
    if (isDir(d) && std::find(v.begin(), v.end(), d) == v.end()) v.push_back(d);
  };
  // i386 on an amd64 Debian/Ubuntu host is multilib, not foreign: glibc
  // headers are shared (/usr/include) and the arch bits live in the
  // multiarch include dir (which also carries gnu/stubs-32.h).
  auto addHostIncludes = [&]() {
    add(includes, "/usr/include");
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator("/usr/include", ec)) {
      std::string n = de.path().filename().string();
      if (n.size() >= 10 && n.compare(n.size() - 10, 10, "-linux-gnu") == 0)
        add(includes, de.path().string());
    }
  };
  if (env && *env) {
    std::string root = env;
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    add(includes, root + "/usr/include");
    add(includes, root + "/usr/" + triplet + "/include");
    add(includes, root + "/include");
    // Multiarch include dirs inside the sysroot (e.g. x86_64-linux-gnu
    // carrying gnu/stubs-32.h for i386).
    std::error_code ec2;
    for (const auto& de : std::filesystem::directory_iterator(root + "/usr/include", ec2)) {
      std::string n = de.path().filename().string();
      if (n.size() >= 10 && n.compare(n.size() - 10, 10, "-linux-gnu") == 0)
        add(includes, de.path().string());
    }
    if (arch == "i386") addHostIncludes();
    add(libs, root + "/usr/lib/" + triplet);
    add(libs, root + "/lib/" + triplet);
    add(libs, root + "/usr/" + triplet + "/lib");
    add(libs, root + "/usr/lib");
    add(libs, root + "/lib");
    // Debian/Ubuntu i386 multilib lives in lib32 (usrmerge: usr/lib32).
    if (arch == "i386") {
      add(libs, root + "/usr/lib32");
      add(libs, root + "/lib32");
    }
    // gcc-cross layout: /usr/lib/gcc-cross/<triplet>/<ver>/{include,lib}
    for (const std::string& base : {root + "/usr/lib/gcc-cross/" + triplet,
                                    root + "/usr/lib/gcc/" + triplet}) {
      std::error_code ec;
      for (const auto& de : std::filesystem::directory_iterator(base, ec)) {
        if (!de.is_directory(ec)) continue;
        add(includes, de.path().string() + "/include");
        add(libs, de.path().string() + "/lib");
      }
    }
    return;
  }
  // Auto-detect: Debian/Ubuntu cross packages.
  const std::string t = triplet;
  add(includes, "/usr/" + t + "/include");
  add(includes, "/usr/include/" + t);
  add(libs, "/usr/lib/" + t);
  add(libs, "/lib/" + t);
  add(libs, "/usr/" + t + "/lib");
  if (arch == "i386") addHostIncludes();
  for (const std::string& base : {"/usr/lib/gcc-cross/" + t, "/usr/lib/gcc/" + t}) {
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(base, ec)) {
      if (!de.is_directory(ec)) continue;
      add(includes, de.path().string() + "/include");
      add(libs, de.path().string() + "/lib");
    }
  }
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
// Compiles emitted target assembly plus the runtime modules with the
// astra driver for `arch` into outPath. The runtime is modular: every *.c
// next to the located a_runtime.c (dispatch/printing plus the domain .c
// modules) is compiled and linked in, unless the prebuilt libaruntime.a
// archive exists for the default opt level. -B points astra at the staged
// per-arch lib dir (libastra1.a + headers) so cross targets never pick up
// the host-arch archive.
int astraBuild(const std::string& arch, const std::string& os,
               const std::string& srcPath, const std::string& outPath,
               std::ostream& err, int optimizationLevel = 2) {
  std::string astraBin, runtimeC, runtimeDir;
  if (!findRuntime(arch, os, astraBin, runtimeC, runtimeDir)) {
    err << "error: cannot locate A runtime (a_runtime.c) for target '" << arch
        << "'; set A_RUNTIME_C or build with cmake\n";
    return 1;
  }
  // i8086 is a bare-metal real-mode target: no libc/sysroot, the runtime
  // is a hand-written assembly subset, and the output is a flat boot
  // image (not an ELF executable).
  if (arch == "i8086") {
    std::string rtS = runtimeDir + "/i8086_runtime.s";
    if (!std::filesystem::exists(rtS)) rtS = runtimeDir + "/../i8086_runtime.s";
    std::vector<std::string> args = {srcPath, rtS,
        "-nostdlib", "-static",
        "-Wl,-e,_start", "-Wl,-Ttext=0x7C00", "-Wl,-oformat=binary",
        "-o", outPath};
    int rc = runAstra(arch, os, args);
    if (rc != 0) { err << "error: astra failed to compile '" << srcPath << "'\n"; return 1; }
    // Pad to a whole number of 512-byte sectors, stamp the boot signature,
    // and record the sector count at offset 0x1F8 for the .boot loader
    // (which loads sectors 1..n-1 via int 13h AH=42h).
    std::error_code ec;
    auto sz = std::filesystem::file_size(outPath, ec);
    if (!ec) {
      const std::uint64_t sectors = (sz + 511) / 512;
      const std::uint64_t padded = sectors * 512;
      std::fstream f(outPath, std::ios::in | std::ios::out | std::ios::binary);
      if (f) {
        if (sz < padded) {
          std::string pad((std::size_t)(padded - sz), '\x90');
          f.seekp(0, std::ios::end); f.write(pad.data(), (std::streamsize)pad.size());
        }
        const std::uint32_t ns = (std::uint32_t)sectors;
        f.seekp(0x1F8);
        char nb[4] = {(char)(ns & 0xFF), (char)((ns >> 8) & 0xFF),
                      (char)((ns >> 16) & 0xFF), (char)((ns >> 24) & 0xFF)};
        f.write(nb, 4);
        f.seekp(510);
        char sig[2] = {(char)0x55, (char)0xAA};
        f.write(sig, 2);
      }
    }
    return 0;
  }
  std::vector<std::string> rtSrcs;
  if (const char* e = std::getenv("A_RUNTIME_C"); e && *e) {
    rtSrcs.push_back(runtimeC);
  } else {
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(runtimeDir, ec)) {
      if (de.path().extension() == ".c") rtSrcs.push_back(de.path().string());
    }
    if (rtSrcs.empty()) rtSrcs.push_back(runtimeC);
    else std::sort(rtSrcs.begin(), rtSrcs.end());
  }
  std::string optFlag = "-O" + std::to_string(optimizationLevel < 0 ? 0 : (optimizationLevel > 3 ? 3 : optimizationLevel));
  // -B points astra at the staged per-arch lib dir (libastra1.a + headers).
  // Only pass it when such a dir exists — a source-tree runtimeDir has no
  // astra lib artifacts and astra's own exe-relative probe does the job.
  std::vector<std::string> args = {optFlag, "-I" + runtimeDir};
  auto hasFile = [](const std::string& p) {
    std::error_code ec; return std::filesystem::exists(p, ec);
  };
  std::string libDir;
  for (const std::string& cand : {runtimeDir + "/libastra1.a",
                                  runtimeDir + "/lib/libastra1.a"}) {
    if (hasFile(cand)) { libDir = runtimeDir; break; }
  }
  if (libDir.empty()) {
    // Look for a staged astralib-<arch>/astralib next to the astra binary.
    std::string astraBin = findAstra(arch, os);
    std::vector<std::string> libnames = {"astralib-" + arch};
    if (arch == hostArch()) libnames.push_back("astralib");
    auto slash = astraBin.find_last_of('/');
    std::vector<std::string> bases;
    if (slash != std::string::npos) {
      std::string d = astraBin.substr(0, slash);
      bases = {d, d + "/..", d + "/../.."};
    } else bases = {".", ".."};
    for (const auto& b : bases) {
      for (const auto& ln : libnames) {
        std::string cand = b + "/" + ln;
        if (hasFile(cand + "/libastra1.a") || hasFile(cand + "/lib/libastra1.a")) {
          libDir = std::filesystem::absolute(cand).string();
          break;
        }
      }
      if (!libDir.empty()) break;
    }
  }
  if (!libDir.empty()) args.push_back("-B" + libDir);
  // Header/library search paths. Native targets keep the host POSIX paths
  // (needed on every arch: astra's bundled include/ lacks fcntl.h/
  // termios.h/etc.). Cross targets get their libc headers and crt/lib
  // dirs from the detected sysroot instead — mixing host headers with a
  // foreign arch produces broken binaries.
  if (os == "windows") {
    // PE target: the runtime .c files need the bundled mingw-style headers
    // and winapi/windows.h staged next to libastra1.a (astralib/include).
    for (const std::string& base : {libDir, runtimeDir}) {
      if (base.empty()) continue;
      for (const char* sub : {"/include", "/include/winapi"}) {
        std::string d = base + sub;
        std::error_code ec2;
        if (std::filesystem::is_directory(d, ec2))
          args.push_back("-I" + d);
      }
    }
  } else if (arch == hostArch()) {
    args.insert(args.end(), {"-I/usr/include", "-I/usr/include/x86_64-linux-gnu",
                     "-I/usr/lib/gcc/x86_64-linux-gnu/13/include", "-I/usr/local/include"});
  } else {
    std::vector<std::string> xinc, xlibs;
    crossSearchDirs(arch, xinc, xlibs);
    for (const auto& d : xinc) args.push_back("-I" + d);
    for (const auto& d : xlibs) args.push_back("-L" + d);
    if (xinc.empty() && xlibs.empty()) {
      err << "note: no sysroot found for target '" << arch
          << "'; set A_SYSROOT or A_SYSROOT_" << arch << " to a cross toolchain root\n";
    }
  }
  // Lazily build a per-arch libaruntime.a: the first cross build compiles
  // each runtime .c once into runtimeDir/rt_obj/*.o and archives them, so
  // subsequent builds link the archive instead of recompiling ~40 C files
  // per program. The archive is keyed by arch via runtimeDir (astralib-<arch>).
  const std::string rtArchive = runtimeDir + "/libaruntime.a";
  if (optimizationLevel == 2 && !hasFile(rtArchive) && os != "windows" &&
      !buildRuntimeArchiveCC(arch, runtimeDir, rtSrcs, rtArchive)) {
    std::error_code ec;
    const std::string objDir = runtimeDir + "/rt_obj";
    std::filesystem::create_directories(objDir, ec);
    std::vector<std::string> cargs = args;  // optFlag -I -B -I/-L prefix
    std::vector<std::string> robjs;
    bool ok = true;
    for (const std::string& src : rtSrcs) {
      std::string obj = objDir + "/" +
          std::filesystem::path(src).stem().string() + ".o";
      if (!hasFile(obj)) {
        std::vector<std::string> ca = cargs;
        ca.push_back("-c"); ca.push_back(src); ca.push_back("-o"); ca.push_back(obj);
        if (runAstra(arch, os, ca) != 0) { ok = false; break; }
      }
      robjs.push_back(obj);
    }
    if (ok && !robjs.empty()) {
      std::vector<std::string> aa = {"-ar", "rcs", rtArchive};
      aa.insert(aa.end(), robjs.begin(), robjs.end());
      ok = runAstra(arch, os, aa) == 0 && hasFile(rtArchive);
    }
    if (!ok) err << "note: could not prebuild libaruntime.a for '" << arch
                 << "'; falling back to per-program runtime compile\n";
  }
  args.push_back(srcPath);
  // The staged libaruntime.a is built once (O2); linking it avoids
  // recompiling every runtime module for each program. Non-default opt levels
  // and targets without the archive keep the compile-sources path.
  if (optimizationLevel == 2 && std::filesystem::exists(rtArchive)) {
    args.push_back(rtArchive);
  } else {
    args.insert(args.end(), rtSrcs.begin(), rtSrcs.end());
  }
  args.push_back("-o"); args.push_back(outPath);
  int rc = runAstra(arch, os, args);
  if (rc != 0) { err << "error: astra failed to compile '" << srcPath << "'\n"; return 1; }
  return 0;
}

// Emits code for a checked program, compiles+links it with the target's
// astra into runPath, and (for native targets) runs the binary.
// `canExecute` is false for cross targets: the link still happens, but no
// child process is spawned.
int compileAndRun(CompiledModule& m, std::istream& in, std::ostream& out, std::ostream& err,
                  bool testMode = false, int optimizationLevel = 2,
                  const std::string* keepBin = nullptr,
                  const TargetDesc* td = nullptr,
                  const std::string* entrySource = nullptr,
                  const std::string* testFilter = nullptr) {
  (void)in; (void)out;
  TargetDesc localTd;
  if (!td) {
    if (!resolveTarget("", localTd, err)) return kCErr;
    td = &localTd;
  }
  // Run cache: a hit skips emit+astra entirely (~2ms exec vs ~25ms build).
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
  std::string srcSource, srcError;
  const char* forced = std::getenv("A_CODEGEN");
  if (forced && lowerStr(forced) == "c") {
    err << "error: A_CODEGEN=c is no longer supported: the C emitter was "
           "removed — every target compiles A -> assembly -> machine code\n";
    return kCErr;
  }
  bool strictAsm = forced && lowerStr(forced) == "asm";
  // The target's assembly emitter is the only codegen path: emitAsm on
  // x86-64 SysV (linux/macos), emitNative on every other arch (including
  // Windows x86-64 via the Win64 MS ABI). A decline is a hard error
  // carrying the emitter's reason — there is no C fallback.
  bool asmOk = td->useAsm
      ? emitAsm(*m.graph, *m.sema, srcSource, srcError, testMode, testFilter)
      : emitNative(*m.graph, *m.sema, td->arch, td->os, srcSource, srcError,
                   testMode, testFilter);
  if (!asmOk) {
    if (td->arch == "i8086")
      err << "error: code generation failed for i8086 (asm-only target): "
          << srcError << "\n";
    else if (strictAsm)
      err << "error: A_CODEGEN=asm: assembly backend declined: "
          << srcError << "\n";
    else
      err << "error: code generation failed: " << srcError << "\n";
    return kCErr;
  }
  std::string srcPath = stageSrc(srcSource, ".s", err);
  if (srcPath.empty()) return kCErr;
  std::string tmpBin = tempPath("bin");
  std::string runPath = (keepBin && !keepBin->empty()) ? *keepBin : tmpBin;
  // On a cache miss compile to a temp name in the cache dir so the rename
  // below is atomic and same-filesystem.
  if (cache.active) {
    std::error_code ec;
    std::filesystem::create_directories(run_cache::cacheDir(), ec);
    runPath = run_cache::cacheDir() + "/tmp-" +
              std::to_string(::getpid()) + "-" + std::to_string(::rand());
  }
  int rc = astraBuild(td->arch, td->os, srcPath, runPath, err, optimizationLevel);
  if (cache.active && rc == 0) {
    std::error_code ec;
    std::filesystem::rename(runPath, cache.file, ec);
    if (!ec) runPath = cache.file;
  }
  std::error_code ec;
  std::filesystem::remove(srcPath, ec);
  if (rc != 0) { std::filesystem::remove(runPath, ec); return kCErr; }
#ifndef _WIN32
  if (td->arch == "i8086") {
    int qrc = runI8086Image(runPath);
    if (!keepBin) std::filesystem::remove(runPath, ec);
    if (qrc == 127) { err << "error: qemu-system-i386 not found (required for --target i8086 run)\n"; return kCErr; }
    return qrc;
  }
#endif
  if (!td->native) {
    // Cross target: the binary was produced but cannot run on this host.
    if (!keepBin) std::filesystem::remove(runPath, ec);
    err << "note: built '" << runPath << "' for " << td->arch << " (cannot execute on this host)\n";
    return 0;
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
CompiledModule runFrontendPipeline(const std::string& path,std::string source,const PipelineConfig& config,DiagnosticSink& sink,std::ostream& diagOut,std::ostream& diagErr){
  ModuleLoaderOptions opts; opts.modulePaths=config.modulePaths; opts.stdlibRoot=config.stdlibRoot;
  ModuleLoader loader(std::move(opts),sink); ModuleGraph graph=loader.load(path,std::move(source));
  if(!flushErrors(sink,config,diagOut,diagErr)) return {};
  Sema sema(graph,sink); SemaResult result = config.jobs > 1 ? sema.analyzeParallel(config.jobs) : sema.analyze(); if(!flushErrors(sink,config,diagOut,diagErr)) return {};
  analyzeOwnership(graph,sink); if(!flushErrors(sink,config,diagOut,diagErr)) return {};
  flushFinal(sink,config,diagOut,diagErr);
  CompiledModule m; m.graph=std::make_unique<ModuleGraph>(std::move(graph)); m.sema=std::make_unique<SemaResult>(std::move(result)); m.ok=true; return m;
}
int executePipeline(const std::string& path,std::string source,const PipelineConfig& config,std::istream& in,std::ostream& out,std::ostream& err){
  // Execution: the frontend emits target assembly, Astra
  // assembles it with the runtime into a binary, and the binary runs
  // as a child process — host targets only.
  TargetDesc td;
  if(!resolveTarget(config.target,td,err)) return kCErr;
  if(!td.native && !(td.arch=="i8086" && hostOs()=="linux")){ err<<"error: cannot execute target '"<<td.arch<<"' on this host ('"<<hostArch()<<"'); use 'a build' to cross-compile\n"; return kCErr; }
  if(config.checkOnly){
    DiagnosticSink sink; auto m=runFrontendPipeline(path,source,config,sink,out,err); if(!m.ok) return kCErr; return kOk;
  }
  // Pre-parse cache hit: skip the frontend entirely and exec the cached
  // binary (depfile + entry-content key, see run_cache::precheckKey).
  // Compile-time diagnostics (warnings) are observable behaviour, so they
  // are recorded alongside the binary on a miss and replayed on a hit.
  std::string preBin;
  if(td.native){
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
      std::filesystem::create_directories(run_cache::cacheDir(), ec);
    }
  }
  std::string diagCapture;
  {
    std::ostringstream diagStream;
    DiagnosticSink sink; auto m=runFrontendPipeline(path,source,config,sink,out,diagStream); if(!m.ok){ err << diagStream.str(); return kCErr; }
    diagCapture = diagStream.str();
    err << diagCapture;
    if(!preBin.empty()){
      std::error_code ec;
      std::ofstream sidecar(preBin + ".diag", std::ios::binary | std::ios::trunc);
      if(sidecar) sidecar << diagCapture;
    }
    return compileAndRun(m,in,out,err,false,config.optimizationLevel,
                       preBin.empty() ? nullptr : &preBin, &td, &source);
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
  // --filter with no matching test_* function: report and exit cleanly
  // instead of compiling a binary that would run zero tests. Mirrors the
  // emitter's discovery predicate (top-level FnDecl in the entry module).
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
    if(!any){ err<<"a test: no tests matched --filter '"<<config.testFilter<<"'\n"; return kOk; }
  }
  return compileAndRun(m,in,out,err,true,config.optimizationLevel,nullptr,&td,&srcCopy,
                       config.testFilter.empty()?nullptr:&config.testFilter);
}
int buildPipeline(const std::string& path,const std::string& outputPath,const PipelineConfig& config,std::ostream& err){
  std::string src; if(!readAll(path,src,err)) return kNoIn;
  DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,err,err); if(!m.ok) return kCErr;
  // `a build`: emit target assembly (x86-64 SysV asm, per-arch native),
  // then astra links a native executable for the target arch.
  TargetDesc td;
  if(!resolveTarget(config.target,td,err)) return kCErr;
  std::string srcSource, srcError;
  const char* forced = std::getenv("A_CODEGEN");
  if (forced && lowerStr(forced) == "c") {
    err << "error: A_CODEGEN=c is no longer supported: the C emitter was "
           "removed — every target compiles A -> assembly -> machine code\n";
    return kCErr;
  }
  bool strictAsm = forced && lowerStr(forced) == "asm";
  // The target's assembly emitter is the only codegen path (emitAsm on
  // x86-64 SysV, emitNative elsewhere); a decline is a hard error.
  bool asmOk = td.useAsm
      ? emitAsm(*m.graph, *m.sema, srcSource, srcError)
      : emitNative(*m.graph, *m.sema, td.arch, td.os, srcSource, srcError);
  if (!asmOk) {
    if (td.arch == "i8086")
      err << "error: code generation failed for i8086 (asm-only target): "
          << srcError << "\n";
    else if (strictAsm)
      err << "error: A_CODEGEN=asm: assembly backend declined: "
          << srcError << "\n";
    else
      err << "error: code generation failed: " << srcError << "\n";
    return kCErr;
  }
  std::string srcPath = stageSrc(srcSource, ".s", err);
  if (srcPath.empty()) return kCErr;
  int rc = astraBuild(td.arch, td.os, srcPath, outputPath, err, config.optimizationLevel);
  std::error_code ec; std::filesystem::remove(srcPath, ec);
  return rc;
}
int emitIRPipeline(const std::string& path,const PipelineConfig& config,bool optimize,std::ostream& out,std::ostream& err){
  std::string src; if(!readAll(path,src,err)) return kNoIn; DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,err,err); if(!m.ok) return kCErr;
  ir::Program p=ir::lowerProgram(*m.graph,*m.sema);
  if(optimize){ ir::OptLevel level = ir::OptLevel::O2; ir::optLevelFromInt(config.optimizationLevel, level); a::ir::runOptPipeline(p, level); }
  out<<ir::printProgram(p); return kOk;
}
ReplSession::~ReplSession() = default;
int replEval(ReplSession& session,const std::string& source,const PipelineConfig& config,std::istream& in,std::ostream& out,std::ostream& err){
  (void)session;
  DiagnosticSink sink; auto m=runFrontendPipeline("<repl>",source,config,sink,out,err); if(!m.ok) return 1;
  TargetDesc td;
  if(!resolveTarget(config.target,td,err)) return 1;
  if(!td.native){ err<<"error: repl requires the host target\n"; return 1; }
  return compileAndRun(m,in,out,err,false,config.optimizationLevel,nullptr,&td,&source);
}
}
