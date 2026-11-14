#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#define A_HAVE_ISATTY 1
#endif

#include "driver/driver.hpp"
#include "driver/version.hpp"
#include "exe_path.hpp"

namespace {

constexpr int kExitUsage = 64;

// Walks up from the entry file's directory looking for an `apm.json`
// project manifest and returns that directory ("" when there is none).
// Every file command (`run`, `build`, `check`, `test`) uses it to
// find apm-installed packages automatically: `<root>/.apm/packages` is
// added to the module search paths, so `import <pkg>` resolves without
// needing `apm run` or an explicit `-I`.
std::string findProjectDir(const std::string& entryPath) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path dir = fs::path(entryPath).parent_path();
  if (dir.empty()) dir = ".";
  for (fs::path d = fs::absolute(dir, ec); !d.empty(); d = d.parent_path()) {
    if (fs::is_regular_file(d / "apm.json", ec)) return d.string();
    if (d == d.parent_path()) break;
  }
  return {};
}

void printUsage(std::ostream& os) {
  os << "A language toolchain " << a::kVersion << " (v2)\n"
     << "\n"
     << "Usage:\n"
     << "  a <command> [options] <file.astra>\n"
     << "\n"
     << "Commands:\n"
     << "  repl             start the interactive REPL\n"
     << "  run <file.astra>     compile and execute a program\n"
     << "  build <file.astra>   compile to a native binary (-o <bin>)\n"
     << "  check <file.astra>   type-check without running\n"
     << "  test [path]      run fn test_*() in a file, or discover tests in a dir/codebase (default: .); --filter <substr> selects by name\n"
     << "  fmt <file.astra>     format source (stdout; -w to write)\n"
     << "  doc <file.astra>     generate documentation (markdown; --json)\n"
     << "  version          print version\n"
     << "  help             show this message\n"
     << "\n"
     << "Options:\n"
     << "  -o <file>        output path (build/doc)\n"
     << "  -c <code>        run inline A source instead of a file (with run, or alone)\n"
     << "  -w, --write      rewrite source in place (fmt)\n"
     << "  --check          exit nonzero if file needs formatting (fmt)\n"
     << "  --indent <n>     indentation width (fmt, default 4)\n"
     << "  --json           structured JSON (doc, check)\n"
     << "  --parallel       check modules in parallel (async type checking; same diagnostics)\n"
     << "  -j <n>, --jobs <n>  parallel jobs (default: serial; -j 0 = all cores)\n"
     << "  -O0/-O1/-O2/-O3/-Os/-Oz/-Ofast  optimization level (clang style)\n"
     << "  --opt-level=N  optimization level 0..3 (alias of -O)\n"
     << "  --optnone    disable all optimizations (LLVM optnone attribute)\n"
     << "  -I <dir>         add module search directory (repeatable)\n"
      << "                     (.apm/packages of an apm.json project is\n"
      << "                     added automatically for run/build/check/test)\n"
     << "  --stdlib <dir>   override standard library directory\n"
     << "  --target <t>     cross-compile target arch:\n"
     << "                    x86_64, i386, arm, aarch64/arm64, riscv64, i8086\n"
     << "                    (optional -linux/-windows/-macos suffix; must\n"
     << "                    match the host OS — the LLVM backend cannot\n"
     << "                    cross OSes or arches; host target only)\n"
     << "  --native         force the native codegen backend (default)\n"
     << "  --backend <b>    select backend: 'llvm' (default; 'asm'/'native'\n"
     << "                    are accepted aliases)\n"
     << "  --no-color       disable colored diagnostics\n"
     << "  --filter <s>     test name substring filter (test only)\n"
     << "  -h, --help       show this message\n"
     << "  -V, --version    print version\n";
}

bool isTerminal() {
#ifdef A_HAVE_ISATTY
  return isatty(fileno(stderr)) != 0;
#else
  return false;
#endif
}

std::string lowerAscii(std::string s) {
  for (char& c : s) c = (char)::tolower((unsigned char)c);
  return s;
}

struct Options {
  std::string command;
  std::string path;
  std::string commandCode;
  std::string outputPath;
  std::vector<std::string> modulePaths;
  std::string stdlibRoot;
  std::string target;
  std::string testFilter;
  bool useColor = false;
  bool checkOnly = false;
  bool writeInPlace = false;
  bool jsonOutput = false;
  bool optnone = false;
  bool native = false;
  std::string backend = "llvm";
  int optimizationLevel = 3;  // 0..3 (Os/Oz map to 1, Ofast to 3)
  int indentWidth = 4;
  int jobs = 1;
};

bool parseOptions(int argc, char** argv, Options& options, std::ostream& err) {
  std::vector<std::string> positional;
  bool colorDisabled = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    // Python-style passthrough: once `run <file>` or `test <path>` is seen,
    // everything after belongs to the program (exported via A_ARGS).
    // `--filter` stays a compiler option for `test` even after the path.
    if (positional.size() >= 2 &&
        (positional[0] == "run" ||
         (positional[0] == "test" && arg != "--filter" &&
          arg.rfind("--filter=", 0) != 0))) {
      positional.push_back(arg);
      continue;
    }
  // LLVM/clang-style optimization levels: -O0..-O3, -Os, -Oz, -Ofast.
  auto parseOptFlag = [&](const std::string& value, bool& ok) {
    ok = true;
    if (value == "O0" || value == "-O0") options.optimizationLevel = 0;
    else if (value == "O1" || value == "-O1") options.optimizationLevel = 1;
    else if (value == "O2" || value == "-O2") options.optimizationLevel = 2;
    else if (value == "O3" || value == "-O3") options.optimizationLevel = 3;
    else if (value == "Os" || value == "-Os") options.optimizationLevel = 1;
    else if (value == "Oz" || value == "-Oz") options.optimizationLevel = 1;
    else if (value == "Ofast" || value == "-Ofast") options.optimizationLevel = 3;
    else if (value.rfind("--opt-level=", 0) == 0) {
      try {
        int n = std::stoi(value.substr(12));
        // Documented range is 0..3; reject out-of-range values like -O99
        // does instead of silently clamping.
        if (n < 0 || n > 3) { ok = false; }
        else options.optimizationLevel = n;
      } catch (...) { ok = false; }
    } else if (value == "--opt-level") {
      ok = false;  // requires =N form
    } else {
      ok = false;
    }
    return ok;
  };
  bool isOptFlag = (arg == "-O0" || arg == "-O1" || arg == "-O2" || arg == "-O3" ||
                    arg == "-Os" || arg == "-Oz" || arg == "-Ofast" ||
                    arg.rfind("--opt-level=", 0) == 0 || arg == "--opt-level");
  if (arg == "--optnone") options.optnone = true;
  else if (arg == "--native") { options.native = true; }
  else if (isOptFlag) {
    bool ok = false;
    if (!parseOptFlag(arg, ok)) { err << "error: invalid optimization level '" << arg << "'\n"; return false; }
  } else if (arg == "--no-color") colorDisabled = true;
  else if (arg == "--check") options.checkOnly = true;
  else if (arg == "-w" || arg == "--write") options.writeInPlace = true;
  else if (arg == "--json") options.jsonOutput = true;
    else if (arg == "--parallel") options.jobs = 0;
    else if (arg == "--jobs" || arg == "-j") {
      if (i + 1 >= argc) { err << "error: '--jobs' requires a number\n"; return false; }
      try { options.jobs = std::stoi(argv[++i]); } catch (...) { err << "error: invalid jobs value\n"; return false; }
      if (options.jobs < 0) { err << "error: jobs must be >= 0\n"; return false; }
    } else if (arg.rfind("--jobs=", 0) == 0 || arg.rfind("-j=", 0) == 0) {
      try { options.jobs = std::stoi(arg.substr(arg.find('=') + 1)); } catch (...) { err << "error: invalid jobs value\n"; return false; }
      if (options.jobs < 0) { err << "error: jobs must be >= 0\n"; return false; }
    } else if (arg.rfind("-j", 0) == 0 && arg.size() > 2) {
      try { options.jobs = std::stoi(arg.substr(2)); } catch (...) { err << "error: invalid jobs value\n"; return false; }
      if (options.jobs < 0) { err << "error: jobs must be >= 0\n"; return false; }
    }
    else if (arg == "--backend") {
      if (i + 1 >= argc) { err << "error: '--backend' requires a name\n"; return false; }
      std::string name = lowerAscii(argv[++i]);
      // 'llvm' is the only backend; 'asm'/'native' map to it for
      // compatibility.
      if (name == "llvm" || name == "native" || name == "asm") { options.native = true; options.backend = "llvm"; }
      else { err << "error: unknown backend '" << name << "' (expected 'llvm')\n"; return false; }
    } else if (arg.rfind("--backend=", 0) == 0) {
      std::string name = lowerAscii(arg.substr(10));
      if (name == "llvm" || name == "native" || name == "asm") { options.native = true; options.backend = "llvm"; }
      else { err << "error: unknown backend '" << name << "' (expected 'llvm')\n"; return false; }
    } else if (arg == "--indent") {
      if (i + 1 >= argc) { err << "error: '--indent' requires a number\n"; return false; }
      try { options.indentWidth = std::stoi(argv[++i]); } catch (...) { err << "error: invalid indentation width\n"; return false; }
      if (options.indentWidth < 1 || options.indentWidth > 16) { err << "error: indentation width must be 1-16\n"; return false; }
    } else if (arg.rfind("--indent=", 0) == 0) {
      try { options.indentWidth = std::stoi(arg.substr(9)); } catch (...) { err << "error: invalid indentation width\n"; return false; }
      if (options.indentWidth < 1 || options.indentWidth > 16) { err << "error: indentation width must be 1-16\n"; return false; }
    } else if (arg == "-o") {
      if (i + 1 >= argc) { err << "error: '-o' requires a path\n"; return false; }
      options.outputPath = argv[++i];
    } else if (arg == "-I") {
      if (i + 1 >= argc) { err << "error: '-I' requires a directory\n"; return false; }
      options.modulePaths.push_back(argv[++i]);
    } else if (arg.rfind("-I", 0) == 0 && arg.size() > 2) {
      options.modulePaths.push_back(arg.substr(2));
    } else if (arg == "-c" || arg == "--command") {
      if (i + 1 >= argc) { err << "error: '-c' requires source code\n"; return false; }
      options.commandCode = argv[++i];
    } else if (arg.rfind("-c", 0) == 0 && arg.size() > 2) {
      options.commandCode = arg.substr(2);
    } else if (arg == "--stdlib") {
      if (i + 1 >= argc) { err << "error: '--stdlib' requires a directory\n"; return false; }
      options.stdlibRoot = argv[++i];
    } else if (arg == "--target" || arg == "-target") {
      if (i + 1 >= argc) { err << "error: '--target' requires a value\n"; return false; }
      options.target = argv[++i];
    } else if (arg.rfind("--target=", 0) == 0) {
      options.target = arg.substr(9);
    } else if (arg == "--filter") {
      if (i + 1 >= argc) { err << "error: '--filter' requires a value\n"; return false; }
      options.testFilter = argv[++i];
    } else if (arg.rfind("--filter=", 0) == 0) {
      options.testFilter = arg.substr(9);
    } else if (arg == "-h" || arg == "--help") { printUsage(std::cout); std::exit(0); }
    else if (arg == "-V" || arg == "--version") { std::cout << a::kVersionLine << "\n"; std::exit(0); }
    else if (!arg.empty() && arg[0] == '-') { err << "error: unknown option '" << arg << "'\n"; return false; }
    else positional.push_back(arg);
  }
  if (positional.empty()) {
    if (options.commandCode.empty()) { printUsage(std::cerr); return false; }
    options.command = "run";  // `a -c "..."` implies run, like `python -c`
  } else {
    options.command = positional[0];
  }
  if (options.command == "version") { std::cout << a::kVersionLine << "\n"; std::exit(0); }
  if (options.command == "help") { printUsage(std::cout); std::exit(0); }
  if (options.command == "repl") {
    if (positional.size() > 1) { err << "error: 'repl' takes no file argument\n"; return false; }
  } else {
    if (!options.commandCode.empty() && options.command != "run") {
      err << "error: '-c' is only valid with 'run'\n"; return false;
    }
    if (!options.commandCode.empty() && positional.size() > 1) {
      err << "error: '-c' cannot be combined with a file argument\n"; return false;
    }
    if (options.commandCode.empty()) {
      if (options.command == "test") {
        if (positional.size() > 2) { err << "error: unexpected argument '" << positional[2] << "'\n"; return false; }
        if (positional.size() == 2) options.path = positional[1];
        // empty path => scan current directory
      } else {
        if (positional.size() < 2) { err << "error: missing input file for '" << options.command << "'\n"; return false; }
        options.path = positional[1];
        // Extra args after the file are forwarded to the program via A_ARGS
        // (like `python script.py args...`); std:swiftcli's args_from_env
        // reads them. Only meaningful for run/test/repl-style execution.
        if (positional.size() > 2 &&
            (options.command == "run" || options.command == "test" ||
             options.command == "repl")) {
          std::string joined;
          for (std::size_t i = 2; i < positional.size(); ++i) {
            if (i > 2) joined += ' ';
            const std::string& a = positional[i];
            bool simple = !a.empty();
            for (char ch : a)
              if (!(std::isalnum((unsigned char)ch) || ch == '_' || ch == '-' ||
                    ch == '.' || ch == '/' || ch == ':' || ch == '='))
                simple = false;
            if (simple) joined += a;
            else {
              joined += '\'';
              for (char ch : a) {
                if (ch == '\'') joined += "'\\''";
                else joined += ch;
              }
              joined += '\'';
            }
          }
          setenv("A_ARGS", joined.c_str(), 1);
        } else if (positional.size() > 2) {
          err << "error: unexpected argument '" << positional[2] << "'\n"; return false;
        }
      }
    } else if (positional.size() > 2) {
      err << "error: unexpected argument '" << positional[2] << "'\n"; return false;
    }
  }
  if (options.command != "build" && options.command != "doc" && !options.outputPath.empty()) { err << "error: '-o' is only valid with 'build' or 'doc'\n"; return false; }
  if (options.command != "fmt" && (options.writeInPlace || options.checkOnly || options.indentWidth != 4)) {
    err << "error: fmt options are only valid with 'fmt'\n"; return false;
  }
  if (options.command != "doc" && options.command != "check" && options.jsonOutput) {
    err << "error: '--json' is only valid with 'doc' or 'check'\n"; return false;
  }
  if (options.optnone) options.optimizationLevel = 0;
  if (options.command != "test" && !options.testFilter.empty()) { err << "error: '--filter' is only valid with 'test'\n"; return false; }
  if (options.writeInPlace && options.checkOnly) { err << "error: '--write' and '--check' cannot be combined\n"; return false; }
  options.useColor = isTerminal() && !colorDisabled;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc <= 1) { printUsage(std::cerr); return kExitUsage; }
#if defined(__unix__) || defined(__APPLE__)
  // Let compiled programs (e.g. repl.eval in the self-hosted REPL) locate
  // this toolchain binary regardless of the caller's PATH/cwd.
  const std::string exePath = a::exePath();
  if (!exePath.empty()) ::setenv("A_A_BIN", exePath.c_str(), 1);
#endif
  Options options;
  if (!parseOptions(argc, argv, options, std::cerr)) return kExitUsage;
  a::DriverConfig config;
  config.checkOnly = options.command == "check";
  config.useColor = options.useColor;
  config.jsonDiagnostics = options.jsonOutput && options.command == "check";
  config.backend = options.backend;
  if (const char* be = std::getenv("A_BACKEND"); be && *be) {
    // 'llvm' is the only native backend; 'asm' is no longer selectable.
    if (lowerAscii(be) == "llvm") config.backend = "llvm";
  }
  config.jobs = options.jobs == 0 ? 0 : options.jobs;  // 0 -> analyzeParallel resolves core count
  config.optimizationLevel = options.optimizationLevel;
  if (const char* env = std::getenv("A_JOBS")) {
    try { if (options.jobs == 1) config.jobs = std::stoi(env); } catch (...) {}
  }
  config.modulePaths = options.modulePaths;
  // apm packages are found automatically: for an `apm.json` project, add
  // `<root>/.apm/packages` to the module search paths for every file
  // command (run/build/check/test), so `import <pkg>` works without
  // `apm run` or an explicit `-I` flag.
  {
    const std::string apmBase = !options.path.empty() ? options.path
                               : (options.command == "test" ? "." : "");
    if (!apmBase.empty()) {
      namespace fs = std::filesystem;
      const std::string root = findProjectDir(apmBase);
      if (!root.empty()) {
        std::error_code ec;
        const fs::path pkgDir = fs::path(root) / ".apm" / "packages";
        if (fs::is_directory(pkgDir, ec)) {
          config.modulePaths.insert(config.modulePaths.begin(), pkgDir.string());
        }
      }
    }
  }
  config.stdlibRoot = options.stdlibRoot;
  config.target = options.target;
  config.testFilter = options.testFilter;
  if (options.command == "run") {
    if (!options.commandCode.empty()) {
      return a::runSource("<command>", options.commandCode, config, std::cin, std::cout, std::cerr);
    }
    return a::runFile(options.path, config, std::cin, std::cout, std::cerr);
  }
  if (options.command == "repl") return a::repl(config, std::cin, std::cout, std::cerr);
  if (options.command == "test") return a::testPath(options.path, config, std::cin, std::cout, std::cerr);
  if (options.command == "build") {
    std::string out = options.outputPath;
    if (out.empty()) out = "a.out";
    return a::buildFile(options.path, out, config, std::cerr);
  }
  if (options.command == "check") return a::runFile(options.path, config, std::cin, std::cout, std::cerr);
  if (options.command == "fmt") {
    a::FormatFileOptions fmt; fmt.checkOnly = options.checkOnly; fmt.writeInPlace = options.writeInPlace; fmt.indentWidth = options.indentWidth; fmt.useColor = options.useColor;
    return a::formatFile(options.path, fmt, std::cout, std::cerr);
  }
  if (options.command == "doc") {
    a::DocFileOptions doc; doc.json = options.jsonOutput; doc.useColor = options.useColor; doc.outputPath = options.outputPath;
    return a::documentFile(options.path, doc, std::cout, std::cerr);
  }
  std::cerr << "error: unknown command '" << options.command << "'\n";
  printUsage(std::cerr);
  return kExitUsage;
}
