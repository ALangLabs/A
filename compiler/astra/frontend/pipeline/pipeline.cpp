#include "pipeline/pipeline.hpp"
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
  std::error_code ec;
  if(std::filesystem::is_directory(p,ec)){
    e<<"error: '"<<p<<"' is a directory, not a source file\n"; return false;
  }
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

namespace objcache {

inline bool envFlagOff(const char* name) {
  const char* v = std::getenv(name);
  if (!v || !*v) return false;
  std::string s = lowerStr(v);
  return s == "0" || s == "off" || s == "no" || s == "none" || s == "false";
}
inline bool splitUnitsEnabled() {
  // Per-module units are the default after Phase 3; A_SPLIT_UNITS=0 opts back
  // into the single-unit emitAsm/emitNative path.
  return !envFlagOff("A_SPLIT_UNITS");
}
inline bool objCacheEnabled() {
  return !run_cache::disabled();
}
inline bool debugUnits() {
  const char* v = std::getenv("A_DEBUG_UNITS");
  return v && *v && lowerStr(v) != "0";
}
inline std::string objCacheDir() {
  if (const char* d = std::getenv("A_CACHE_DIR"); d && *d) {
    std::error_code ec;
    std::filesystem::path parent = std::filesystem::path(d).parent_path();
    return (parent.empty() ? std::filesystem::path(d) : parent / "obj").string();
  }
  std::string base;
#ifdef _WIN32
  char tmp[MAX_PATH];
  DWORD n = GetTempPathA(MAX_PATH, tmp);
  base = std::string(n > 0 && n < MAX_PATH ? tmp : ".\\") + "a_cache";
#else
  if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x)
    base = std::string(x) + "/a";
  else if (const char* home = std::getenv("HOME"); home && *home)
    base = std::string(home) + "/.cache/a";
  else base = "/tmp/a_cache";
#endif
  return base + "/obj";
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
  const char* rcc = std::getenv("A_RUNTIME_CC");
  h = run_cache::mixBytes(h, rcc ? rcc : "");
  const char* rcflags = std::getenv("A_RUNTIME_CFLAGS");
  h = run_cache::mixBytes(h, rcflags ? rcflags : "");
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
  h = run_cache::mixBytes(h, "M:" + mod.path);
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
    h = run_cache::mixBytes(h, "D:" + g.at(d).path);
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
  const char* rcc = std::getenv("A_RUNTIME_CC");
  h = run_cache::mixBytes(h, rcc ? rcc : "");
  const char* rcflags = std::getenv("A_RUNTIME_CFLAGS");
  h = run_cache::mixBytes(h, rcflags ? rcflags : "");
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
      gh = run_cache::mixBytes(gh, "G:" + mod.path);
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
  h = run_cache::mixBytes(h, "M:" + mod.path);
  auto sit = ctx.sourceById.find(moduleId);
  h = run_cache::mixBytes(h, sit == ctx.sourceById.end() ? "<missing>" : sit->second);
  auto sighIt = ctx.sighById.find(moduleId);
  h = run_cache::mixInt(h, sighIt == ctx.sighById.end() ? 0 : sighIt->second);
  std::vector<int> sorted(deps.begin(), deps.end());
  for (int d : sorted) {
    h = run_cache::mixBytes(h, "D:" + g.at(d).path);
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

// ---- per-module split build (Phase 2 + 3) -------------------------------
struct AstraPlan {
  std::string astraBin;
  std::string runtimeC;
  std::string runtimeDir;
  std::string libDir;
  std::string rtArchive;
  std::vector<std::string> rtSrcs;
  std::vector<std::string> baseArgs;
};
// Replicates the compiler-flag discovery in astraBuild (lib dir, includes,
// lazy libaruntime.a) so per-unit assembly uses identical flags.
bool prepareAstraPlan(const std::string& arch, const std::string& os,
                      int optimizationLevel, std::ostream& err,
                      AstraPlan& plan) {
  if (!findRuntime(arch, os, plan.astraBin, plan.runtimeC, plan.runtimeDir)) {
    err << "error: cannot locate A runtime (a_runtime.c) for target '" << arch
        << "'; set A_RUNTIME_C or build with cmake\n";
    return false;
  }
  if (const char* e = std::getenv("A_RUNTIME_C"); e && *e) {
    plan.rtSrcs.push_back(plan.runtimeC);
  } else {
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(plan.runtimeDir, ec)) {
      if (de.path().extension() == ".c") plan.rtSrcs.push_back(de.path().string());
    }
    if (plan.rtSrcs.empty()) plan.rtSrcs.push_back(plan.runtimeC);
    else std::sort(plan.rtSrcs.begin(), plan.rtSrcs.end());
  }
  std::string optFlag = "-O" + std::to_string(optimizationLevel < 0 ? 0 : (optimizationLevel > 3 ? 3 : optimizationLevel));
  plan.baseArgs = {optFlag, "-I" + plan.runtimeDir};
  auto hasFile = [](const std::string& p) {
    std::error_code ec; return std::filesystem::exists(p, ec);
  };
  plan.libDir = "";
  for (const std::string& cand : {plan.runtimeDir + "/libastra1.a",
                                  plan.runtimeDir + "/lib/libastra1.a"}) {
    if (hasFile(cand)) { plan.libDir = plan.runtimeDir; break; }
  }
  if (plan.libDir.empty()) {
    std::vector<std::string> libnames = {"astralib-" + arch};
    if (arch == hostArch()) libnames.push_back("astralib");
    auto slash = plan.astraBin.find_last_of('/');
    std::vector<std::string> bases;
    if (slash != std::string::npos) {
      std::string d = plan.astraBin.substr(0, slash);
      bases = {d, d + "/..", d + "/../.."};
    } else bases = {".", ".."};
    for (const auto& b : bases) {
      for (const auto& ln : libnames) {
        std::string cand = b + "/" + ln;
        if (hasFile(cand + "/libastra1.a") || hasFile(cand + "/lib/libastra1.a")) {
          plan.libDir = std::filesystem::absolute(cand).string();
          break;
        }
      }
      if (!plan.libDir.empty()) break;
    }
  }
  if (!plan.libDir.empty()) plan.baseArgs.push_back("-B" + plan.libDir);
  if (os == "windows") {
    for (const std::string& base : {plan.libDir, plan.runtimeDir}) {
      if (base.empty()) continue;
      for (const char* sub : {"/include", "/include/winapi"}) {
        std::string d = base + sub;
        std::error_code ec2;
        if (std::filesystem::is_directory(d, ec2))
          plan.baseArgs.push_back("-I" + d);
      }
    }
  } else if (arch == hostArch()) {
    plan.baseArgs.insert(plan.baseArgs.end(), {"-I/usr/include", "-I/usr/include/x86_64-linux-gnu",
                     "-I/usr/lib/gcc/x86_64-linux-gnu/13/include", "-I/usr/local/include"});
  } else {
    std::vector<std::string> xinc, xlibs;
    crossSearchDirs(arch, xinc, xlibs);
    for (const auto& d : xinc) plan.baseArgs.push_back("-I" + d);
    for (const auto& d : xlibs) plan.baseArgs.push_back("-L" + d);
    if (xinc.empty() && xlibs.empty()) {
      err << "note: no sysroot found for target '" << arch
          << "'; set A_SYSROOT or A_SYSROOT_" << arch << " to a cross toolchain root\n";
    }
  }
  plan.rtArchive = plan.runtimeDir + "/libaruntime.a";
  if (optimizationLevel == 2 && !hasFile(plan.rtArchive) && os != "windows" &&
      !buildRuntimeArchiveCC(arch, plan.runtimeDir, plan.rtSrcs, plan.rtArchive)) {
    std::error_code ec;
    const std::string objDir = plan.runtimeDir + "/rt_obj";
    std::filesystem::create_directories(objDir, ec);
    std::vector<std::string> robjs;
    bool ok = true;
    for (const std::string& src : plan.rtSrcs) {
      std::string obj = objDir + "/" +
          std::filesystem::path(src).stem().string() + ".o";
      if (!hasFile(obj)) {
        std::vector<std::string> ca = plan.baseArgs;
        ca.push_back("-c"); ca.push_back(src); ca.push_back("-o"); ca.push_back(obj);
        if (runAstra(arch, os, ca) != 0) { ok = false; break; }
      }
      robjs.push_back(obj);
    }
    if (ok && !robjs.empty()) {
      std::vector<std::string> aa = {"-ar", "rcs", plan.rtArchive};
      aa.insert(aa.end(), robjs.begin(), robjs.end());
      ok = runAstra(arch, os, aa) == 0 && hasFile(plan.rtArchive);
    }
    if (!ok) err << "note: could not prebuild libaruntime.a for '" << arch
                 << "'; falling back to per-program runtime compile\n";
  }
  return true;
}
int assembleOne(const std::string& arch, const std::string& os,
                const std::vector<std::string>& baseArgs,
                const std::string& srcPath, const std::string& objPath) {
  std::vector<std::string> args = baseArgs;
  args.push_back("-c"); args.push_back(srcPath);
  args.push_back("-o"); args.push_back(objPath);
  return runAstra(arch, os, args);
}
#ifndef _WIN32
int assembleAll(const std::string& arch, const std::string& os,
                const std::vector<std::string>& baseArgs,
                const std::vector<std::pair<std::string, std::string>>& jobsList,
                int parallel) {
  if (parallel <= 1 || jobsList.size() <= 1) {
    int rc = 0;
    for (const auto& j : jobsList) {
      rc |= assembleOne(arch, os, baseArgs, j.first, j.second);
    }
    return rc;
  }
  std::string astraBin = findAstra(arch, os);
  struct Child { pid_t pid; std::size_t idx; };
  std::vector<Child> running;
  std::vector<int> failed(jobsList.size(), 0);
  std::size_t next = 0;
  auto tryLaunch = [&]() -> bool {
    if (next >= jobsList.size()) return false;
    if ((int)running.size() >= parallel) return false;
    const std::string& src = jobsList[next].first;
    const std::string& obj = jobsList[next].second;
    std::vector<std::string> args = baseArgs;
    args.push_back("-c"); args.push_back(src);
    args.push_back("-o"); args.push_back(obj);
    std::vector<char*> av;
    av.push_back(const_cast<char*>(astraBin.c_str()));
    for (auto& s : args) av.push_back(const_cast<char*>(s.c_str()));
    av.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
      execvp(astraBin.c_str(), av.data());
      _exit(127);
    }
    running.push_back({pid, next});
    ++next;
    return true;
  };
  while (!running.empty() || next < jobsList.size()) {
    while (tryLaunch()) {}
    if (running.empty()) {
      // Fork failed: fall back to serial for the rest.
      for (; next < jobsList.size(); ++next)
        failed[next] = assembleOne(arch, os, baseArgs, jobsList[next].first, jobsList[next].second);
      break;
    }
    int status = 0;
    pid_t w = waitpid(-1, &status, 0);
    if (w < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (auto it = running.begin(); it != running.end(); ++it) {
      if (it->pid == w) {
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) failed[it->idx] = 1;
        running.erase(it);
        break;
      }
    }
  }
  int rc = 0;
  for (int f : failed) rc |= f;
  return rc;
}
#else
int assembleAll(const std::string& arch, const std::string& os,
                const std::vector<std::string>& baseArgs,
                const std::vector<std::pair<std::string, std::string>>& jobsList,
                int) {
  int rc = 0;
  for (const auto& j : jobsList) rc |= assembleOne(arch, os, baseArgs, j.first, j.second);
  return rc;
}
#endif
int linkUnits(const std::string& arch, const std::string& os,
              const AstraPlan& plan, int optimizationLevel,
              const std::vector<std::string>& objs, const std::string& outPath,
              std::ostream& err) {
  std::vector<std::string> args = plan.baseArgs;
  for (const auto& o : objs) args.push_back(o);
  if (optimizationLevel == 2 && std::filesystem::exists(plan.rtArchive))
    args.push_back(plan.rtArchive);
  else
    args.insert(args.end(), plan.rtSrcs.begin(), plan.rtSrcs.end());
  args.push_back("-o"); args.push_back(outPath);
  int rc = runAstra(arch, os, args);
  if (rc != 0) err << "error: astra failed to link program\n";
  return rc;
}
// Splits a large unit's assembly text into independent chunks for parallel
// `astra -c` (intra-unit parallel assemble). Header (bss+rodata) is kept in
// chunk 0 (bss must appear once); rodata is duplicated in later chunks so
// object-local `.L` string references resolve in each chunk. Text is split
// at function boundaries (`    .text` lines) and balanced by line count.
inline std::vector<std::string> splitUnitText(const std::string& text, int parts) {
  std::vector<std::string> out;
  if (parts <= 1 || text.empty()) { out.push_back(text); return out; }
  std::vector<std::string> lines;
  {
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) lines.push_back(line + "\n");
  }
  long firstText = -1;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i] == "    .text\n") { firstText = (long)i; break; }
  }
  if (firstText < 0) { out.push_back(text); return out; }
  long rodataIdx = -1;
  for (std::size_t i = 0; i < (std::size_t)firstText; ++i) {
    if (lines[i].find(".section .rodata") != std::string::npos) { rodataIdx = (long)i; break; }
  }
  std::string bssText, rodataText;
  if (rodataIdx >= 0) {
    for (long i = 0; i < rodataIdx; ++i) bssText += lines[(std::size_t)i];
    for (long i = rodataIdx; i < firstText; ++i) rodataText += lines[(std::size_t)i];
  } else {
    for (long i = 0; i < firstText; ++i) bssText += lines[(std::size_t)i];
  }
  // Local string labels (.L__a_dN) must be repeated in every chunk because they
  // are file-local and cannot be resolved across objects by the `-r` merge.
  // Global data such as the __a_test_names/__a_test_fns tables must NOT be
  // repeated: only the entry chunk that references them should define them,
  // otherwise the merge fails with "link symbol defined twice".
  std::string rodataLocal = rodataText;
  {
    std::size_t p = rodataLocal.find("    .globl __a_test_names");
    if (p != std::string::npos) {
      std::size_t q = rodataLocal.find("__a_test_fns:", p);
      if (q != std::string::npos) {
        std::size_t nl = rodataLocal.find('\n', q);
        q = (nl == std::string::npos) ? rodataLocal.size() : nl + 1;
        // Consume the .quad entries that follow __a_test_fns:.
        while (q < rodataLocal.size() &&
               rodataLocal.compare(q, 11, "    .quad ") == 0) {
          nl = rodataLocal.find('\n', q);
          q = (nl == std::string::npos) ? rodataLocal.size() : nl + 1;
        }
        rodataLocal.erase(p, q - p);
      }
    }
  }
  std::vector<std::string> funcs;
  std::vector<std::size_t> funcLines;
  // first func starts at firstText; subsequent funcs at each `    .text`
  std::vector<std::size_t> starts;
  for (std::size_t i = (std::size_t)firstText; i < lines.size(); ++i) {
    if (lines[i] == "    .text\n") starts.push_back(i);
  }
  if (starts.empty()) { out.push_back(text); return out; }
  for (std::size_t k = 0; k < starts.size(); ++k) {
    std::size_t s = starts[k];
    std::size_t e = (k + 1 < starts.size()) ? starts[k + 1] : lines.size();
    std::string f;
    for (std::size_t i = s; i < e; ++i) f += lines[i];
    funcs.push_back(std::move(f));
    funcLines.push_back(e - s);
  }
  if ((int)funcs.size() < 2) { out.push_back(text); return out; }
  int nparts = std::min(parts, (int)funcs.size());
  // Greedy balance by line count.
  std::vector<std::vector<int>> groups((std::size_t)nparts);
  std::vector<std::size_t> gsum((std::size_t)nparts, 0);
  std::vector<int> order(funcs.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](int a, int b){ return funcLines[(std::size_t)a] > funcLines[(std::size_t)b]; });
  for (int idx : order) {
    int best = 0;
    for (int g = 1; g < nparts; ++g) if (gsum[(std::size_t)g] < gsum[(std::size_t)best]) best = g;
    groups[(std::size_t)best].push_back(idx);
    gsum[(std::size_t)best] += funcLines[(std::size_t)idx];
  }
  for (int g = 0; g < nparts; ++g) {
    std::string chunk = (g == 0 ? bssText + rodataText : rodataLocal);
    // Preserve source order within group for determinism.
    std::sort(groups[(std::size_t)g].begin(), groups[(std::size_t)g].end());
    for (int idx : groups[(std::size_t)g]) chunk += funcs[(std::size_t)idx];
    out.push_back(std::move(chunk));
  }
  return out;
}
// Builds `outPath` from per-module units (entry unit holds `main`).
// Returns 0 on success, 1 on failure. Emits/assembles only cache misses
// when the object cache is active; A_NO_CACHE disables it, but the split
// layout is kept so cached/uncached binaries are byte-comparable.
int buildSplitUnits(CompiledModule& m, const TargetDesc& td,
                    const std::string& outPath, std::ostream& err,
                    int optimizationLevel, bool testMode,
                    const std::string* testFilter,
                    const std::string* entrySource, int parallel) {
  // Assembly itself is embarrassingly parallel; default to 4 workers even
  // when frontend jobs=1 (A_JOBS/-j still cap it at 1 when explicitly 1
  // and A_JOBS is set? No: explicit -j1 keeps serial assembly for
  // determinism tests; only the default (jobs==0, no A_JOBS) auto-scales).
  // NOTE: compileAndRun passes config.jobs (default 1) here, so without -j
  // we would stay serial; treat 1 as auto=4 for assembly to meet the
  // incremental-build perf gate (frontend jobs remain serial).
  if (parallel <= 0) {
    const char* j = std::getenv("A_JOBS");
    if (j && *j) { try { parallel = std::stoi(j); } catch (...) { parallel = 4; } }
    else parallel = 4;
  } else if (parallel == 1 && !std::getenv("A_JOBS")) {
    // Default `a run`/`a build` (jobs=1, no A_JOBS): parallel assembly.
    parallel = 4;
  }
  const ModuleGraph& g = *m.graph;
  std::vector<int> order = g.order;
  if (order.empty() && g.entry >= 0) order.push_back(g.entry);
  {
    std::set<int> inOrder(order.begin(), order.end());
    for (const Module& mod : g.modules)
      if (!inOrder.count(mod.id)) order.push_back(mod.id);
  }
  bool useObjCache = objcache::objCacheEnabled();  // per-target key; cross .o safe
  std::string cacheDir = objcache::objCacheDir();
  std::error_code ec;
  if (useObjCache) std::filesystem::create_directories(cacheDir, ec);
  struct UnitPlan {
    int moduleId = -1;
    std::uint64_t sigh = 0;
    std::string key;
    std::string cacheObj;
    bool hit = false;
  };
  objcache::KeyContext keyCtx = objcache::makeKeyContext(m, td, optimizationLevel, entrySource);
  std::vector<UnitPlan> plans;
  for (int mid : order) {
    UnitPlan up;
    up.moduleId = mid;
    up.sigh = keyCtx.sighById[mid];
    std::set<int> deps;
    if (mid == g.entry) {
      for (const Module& mod : g.modules)
        if (mod.id != mid) deps.insert(mod.id);
    } else {
      deps = objcache::transitiveImports(g, mid);
      deps.erase(mid);
    }
    up.key = objcache::unitKeyWithCtx(m, keyCtx, testMode, testFilter, mid, deps);
    up.cacheObj = objcache::unitCachePath(up.key);
    if (useObjCache && std::filesystem::is_regular_file(up.cacheObj, ec))
      up.hit = true;
    plans.push_back(up);
  }
  std::vector<int> misses;
  for (const auto& up : plans) if (!up.hit) misses.push_back(up.moduleId);
  bool dbg = objcache::debugUnits();
  if (dbg) {
    for (const auto& up : plans) {
      err << "unit " << up.moduleId << " " << g.at(up.moduleId).path
          << " key=" << up.key << " " << (up.hit ? "hit" : "miss") << "\n";
    }
    err << "units: " << plans.size() << " total, " << misses.size() << " emitted\n";
  }
  AstraPlan azt;
  if (!prepareAstraPlan(td.arch, td.os, optimizationLevel, err, azt)) return 1;
  struct timespec ts0; clock_gettime(CLOCK_MONOTONIC, &ts0);
  auto ms_since = [&](struct timespec a){ struct timespec b; clock_gettime(CLOCK_MONOTONIC, &b); return (b.tv_sec-a.tv_sec)*1000 + (b.tv_nsec-a.tv_nsec)/1000000; };
  bool dotiming = std::getenv("A_DEBUG_TIMING") != nullptr;
  std::unordered_map<int, std::string> unitText;
  if (!misses.empty()) {
    std::vector<AsmUnit> emitted;
    std::string emitErr;
    bool emitOk;
    if (td.useAsm) {
      emitOk = emitAsmPerUnit(*m.graph, *m.sema, emitted, emitErr, testMode, testFilter, &misses);
    } else {
      // Native per-unit split (functions-only; top-levels in entry main).
      emitOk = emitNativePerUnit(*m.graph, *m.sema, td.arch, td.os, emitted, emitErr,
                                 testMode, testFilter, &misses);
    }
    if (!emitOk) {
      err << "error: code generation failed: " << emitErr << "\n";
      return 1;
    }
    for (auto& u : emitted) unitText[u.moduleId] = std::move(u.text);
  }
  if (dotiming) err << "timing emit ms=" << ms_since(ts0) << "\n";
  // Assemble misses (parallel where possible). Large units are split into
  // balanced chunks for intra-unit parallel assemble (see splitUnitText).
  struct MissJob {
    int moduleId = -1;
    std::vector<std::string> chunkSrcs;
    std::vector<std::string> chunkObjs;
    std::string mergedObj;  // single .o for this unit after partial link
    bool split = false;
  };
  std::vector<MissJob> missJobs;
  std::vector<std::pair<std::string, std::string>> jobsList;  // chunk src->obj
  std::vector<std::string> stagedSrcs;
  std::vector<std::string> tempObjs;
  std::unordered_map<int, std::string> objPath;
  for (const auto& up : plans) {
    if (up.hit) {
      objPath[up.moduleId] = up.cacheObj;
    } else {
      auto it = unitText.find(up.moduleId);
      if (it == unitText.end()) {
        err << "error: missing unit emission for module " << up.moduleId << "\n";
        return 1;
      }
      MissJob mj;
      mj.moduleId = up.moduleId;
      std::vector<std::string> chunks;
      std::size_t nlines = 0;
      {
        // Count lines to decide splitting (threshold keeps setup cost low).
        for (char c : it->second) if (c == '\n') ++nlines;
        if (parallel > 1 && nlines > 8000) chunks = splitUnitText(it->second, std::min(parallel, 4));
        else chunks.push_back(it->second);
      }
      mj.split = chunks.size() > 1;
      for (auto& ch : chunks) {
        std::string srcPath = stageSrc(ch, ".s", err);
        if (srcPath.empty()) return 1;
        stagedSrcs.push_back(srcPath);
        std::string tmpObj;
        if (useObjCache) {
          tmpObj = cacheDir + "/tmp-" + std::to_string(::getpid()) + "-" + std::to_string(::rand()) + ".o";
        } else {
          tmpObj = tempPath("obj", "x.o");
        }
        jobsList.push_back({srcPath, tmpObj});
        mj.chunkSrcs.push_back(srcPath);
        mj.chunkObjs.push_back(tmpObj);
      }
      if (!mj.split) {
        mj.mergedObj = mj.chunkObjs.front();
        objPath[up.moduleId] = useObjCache ? up.cacheObj : mj.mergedObj;
        if (!useObjCache) tempObjs.push_back(mj.mergedObj);
        else tempObjs.push_back(mj.mergedObj);  // tmpObj renamed below
      } else {
        // Partial link chunks into one unit .o for cache / final link.
        std::string merged = tempPath("obj", "x.o");
        mj.mergedObj = merged;
        objPath[up.moduleId] = useObjCache ? up.cacheObj : merged;
        tempObjs.push_back(merged);
      }
      missJobs.push_back(std::move(mj));
    }
  }
  // jobsList src->tmpObj ordering must match misses for rename below.
  if (dotiming) err << "timing stagedsrc ms=" << ms_since(ts0) << "\n";
  if (!jobsList.empty()) {
    if (assembleAll(td.arch, td.os, azt.baseArgs, jobsList, parallel) != 0) {
      err << "error: astra failed to assemble per-module units\n";
      for (auto& s : stagedSrcs) std::filesystem::remove(s, ec);
      // Remove partial tmp objs.
      for (auto& o : tempObjs) std::filesystem::remove(o, ec);
      return 1;
    }
  }
  for (auto& s : stagedSrcs) std::filesystem::remove(s, ec);
  for (auto& mj : missJobs) {
    if (mj.split) {
      // Partial-link chunks into one unit .o (relocatable).
      std::vector<std::string> pargs = azt.baseArgs;
      pargs.push_back("-r");
      for (auto& co : mj.chunkObjs) pargs.push_back(co);
      pargs.push_back("-o");
      pargs.push_back(mj.mergedObj);
      if (runAstra(td.arch, td.os, pargs) != 0) {
        err << "error: astra failed to merge split unit " << mj.moduleId << "\n";
        for (auto& o : mj.chunkObjs) std::filesystem::remove(o, ec);
        return 1;
      }
      for (auto& co : mj.chunkObjs) std::filesystem::remove(co, ec);
    }
    if (useObjCache) {
      const std::string& dest = mj.mergedObj;
      int mid = mj.moduleId;
      std::string finalObj;
      for (const auto& u2 : plans) if (u2.moduleId == mid) finalObj = u2.cacheObj;
      std::error_code rec;
      std::filesystem::rename(dest, finalObj, rec);
      if (rec) {
        std::error_code cec;
        std::filesystem::copy_file(dest, finalObj, std::filesystem::copy_options::overwrite_existing, cec);
        std::filesystem::remove(dest, ec);
      }
      std::uint64_t sigh = 0;
      for (const auto& u2 : plans) if (u2.moduleId == mid) sigh = u2.sigh;
      std::ofstream sf(objcache::objCacheDir() + "/" + [&] {
        for (const auto& u2 : plans) if (u2.moduleId == mid) return u2.key;
        return std::string();
      }() + ".sym", std::ios::binary | std::ios::trunc);
      if (sf) sf << objcache::hex64(sigh) << "\n";
    }
  }
  std::vector<std::string> objs;
  objs.reserve(plans.size());
  for (const auto& up : plans) objs.push_back(objPath[up.moduleId]);
  if (dotiming) err << "timing assemble ms=" << ms_since(ts0) << "\n";
  int rc = linkUnits(td.arch, td.os, azt, optimizationLevel, objs, outPath, err);
  if (dotiming) err << "timing link ms=" << ms_since(ts0) << "\n";
  for (auto& o : tempObjs) if (!useObjCache) std::filesystem::remove(o, ec);
  return rc == 0 ? 0 : 1;
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
                  const std::string* testFilter = nullptr, int jobs = 0) {
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
  // Per-module split path (Phase 2 + 3): x86-64 SysV + native targets
  // (functions-only split; top-levels stay in entry main). i8086
  // (bare-metal flat image) stays single-unit. A_SPLIT_UNITS=0 forces
  // the single-unit path everywhere.
  if (objcache::splitUnitsEnabled() && td->arch != "i8086" && td->arch != "8086") {
    std::string tmpBinS = tempPath("bin");
    std::string runPathS = (keepBin && !keepBin->empty()) ? *keepBin : tmpBinS;
    if (cache.active) {
      std::error_code ec2;
      std::filesystem::create_directories(run_cache::cacheDir(), ec2);
      runPathS = run_cache::cacheDir() + "/tmp-" +
                 std::to_string(::getpid()) + "-" + std::to_string(::rand());
    }
    int src2 = buildSplitUnits(m, *td, runPathS, err, optimizationLevel,
                               testMode, testFilter, entrySource, jobs);
    if (cache.active && src2 == 0) {
      std::error_code rec2;
      std::filesystem::rename(runPathS, cache.file, rec2);
      if (!rec2) runPathS = cache.file;
    }
    if (src2 != 0) {
      std::error_code ec2;
      std::filesystem::remove(runPathS, ec2);
      return kCErr;
    }
    std::error_code ec2;
    if (!td->native) {
      if (!keepBin) std::filesystem::remove(runPathS, ec2);
      err << "note: built '" << runPathS << "' for " << td->arch << " (cannot execute on this host)\n";
      return 0;
    }
    int rrc = runBinary(runPathS);
    if (cache.active && runPathS == cache.file) {
    } else if (!keepBin) {
      std::filesystem::remove(runPathS, ec2);
    }
    return rrc < 0 ? kCErr : rrc;
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
// True when the entry module declares `fn main` — the automatic entry
// point invoked after module top-level statements (docs/wiki/modules.md).
// `a run`/`a build` on a file without one used to exit 0 silently.
bool entryHasMain(const ModuleGraph& g){
  if(g.entry<0||g.entry>=(int)g.modules.size()) return false;
  const Module& mod=g.at(g.entry);
  if(!mod.program) return false;
  for(const StmtPtr& s:mod.program->statements)
    if(s->kind==StmtKind::FnDecl&&static_cast<const FnDecl*>(s.get())->name=="main") return true;
  return false;
}
void warnNoMain(const ModuleGraph& g,const std::string& path,std::ostream& err){
  if(entryHasMain(g)) return;
  err<<"warning: '"<<path<<"' has no 'fn main'; only top-level statements will run\n";
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
    // Route through diagStream so the warning is captured in the run-cache
    // .diag sidecar and replayed on precheck hits like other diagnostics.
    warnNoMain(*m.graph,path,diagStream);
    diagCapture = diagStream.str();
    err << diagCapture;
    if(!preBin.empty()){
      std::error_code ec;
      std::ofstream sidecar(preBin + ".diag", std::ios::binary | std::ios::trunc);
      if(sidecar) sidecar << diagCapture;
    }
    return compileAndRun(m,in,out,err,false,config.optimizationLevel,
                       preBin.empty() ? nullptr : &preBin, &td, &source, nullptr, config.jobs);
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
    if(!any){ err<<"a test: no tests matched --filter '"<<config.testFilter<<"'\n"; return kCErr; }
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
  return compileAndRun(m,in,out,err,true,config.optimizationLevel,nullptr,&td,&srcCopy,
                       config.testFilter.empty()?nullptr:&config.testFilter, config.jobs);
}
int buildPipeline(const std::string& path,const std::string& outputPath,const PipelineConfig& config,std::ostream& err){
  // Guard `a build x.a -o x.a`: astra would overwrite the source file with
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
  warnNoMain(*m.graph,path,err);
  // `a build`: emit target assembly (x86-64 SysV asm, per-arch native),
  // then astra links a native executable for the target arch.
  TargetDesc td;
  if(!resolveTarget(config.target,td,err)) return kCErr;
  if (objcache::splitUnitsEnabled() && td.arch != "i8086" && td.arch != "8086") {
    return buildSplitUnits(m, td, outputPath, err, config.optimizationLevel,
                           false, nullptr, nullptr, config.jobs) == 0 ? kOk : kCErr;
  }
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
  return compileAndRun(m,in,out,err,false,config.optimizationLevel,nullptr,&td,&source,nullptr,config.jobs);
}
}
