#include <iostream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#define A_HAVE_ISATTY 1
#endif

#include "driver/driver.hpp"
#include "driver/version.hpp"

namespace {

constexpr int kExitUsage = 64;

void printUsage(std::ostream& os) {
  os << "A language toolchain " << a::kVersion << " (v2)\n"
     << "\n"
     << "Usage:\n"
     << "  a <command> [options] <file.a>\n"
     << "\n"
     << "Commands:\n"
     << "  repl             start the interactive REPL\n"
     << "  run <file.a>     compile and execute a program\n"
     << "  build <file.a>   compile to a native binary (-o <bin>)\n"
     << "  check <file.a>   type-check without running\n"
     << "  test <file.a>    run every fn test_*()\n"
     << "  fmt <file.a>     format source (stdout; -w to write)\n"
     << "  doc <file.a>     generate documentation (markdown; --json)\n"
     << "  ir <file.a>      print the typed SSA IR\n"
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
     << "  --opt            run optimization passes (ir)\n"
     << "  --emit-llvm      emit LLVM IR (.ll) instead of executable (build)\n"
     << "  -I <dir>         add module search directory (repeatable)\n"
     << "  --stdlib <dir>   override standard library directory\n"
     << "  --no-color       disable colored diagnostics\n"
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

struct Options {
  std::string command;
  std::string path;
  std::string commandCode;
  std::string outputPath;
  bool emitLLVM = false;
  std::vector<std::string> modulePaths;
  std::string stdlibRoot;
  bool useColor = false;
  bool checkOnly = false;
  bool writeInPlace = false;
  bool jsonOutput = false;
  bool optimize = false;
  int indentWidth = 4;
  int jobs = 1;
};

bool parseOptions(int argc, char** argv, Options& options, std::ostream& err) {
  std::vector<std::string> positional;
  bool colorDisabled = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--no-color") colorDisabled = true;
    else if (arg == "--emit-llvm") options.emitLLVM = true;
    else if (arg == "--check") options.checkOnly = true;
    else if (arg == "-w" || arg == "--write") options.writeInPlace = true;
    else if (arg == "--json") options.jsonOutput = true;
    else if (arg == "--opt") options.optimize = true;
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
      // Accepted for compatibility; A v2 uses a single LLVM-oriented backend.
      if (i + 1 >= argc) { err << "error: '--backend' requires a name\n"; return false; }
      ++i;
    } else if (arg.rfind("--backend=", 0) == 0) {
      // compatibility no-op
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
      if (positional.size() < 2) { err << "error: missing input file for '" << options.command << "'\n"; return false; }
      if (positional.size() > 2) { err << "error: unexpected argument '" << positional[2] << "'\n"; return false; }
      options.path = positional[1];
    } else if (positional.size() > 2) {
      err << "error: unexpected argument '" << positional[2] << "'\n"; return false;
    }
  }
  if (options.command != "build" && options.command != "doc" && !options.outputPath.empty()) { err << "error: '-o' is only valid with 'build' or 'doc'\n"; return false; }
  if (options.command != "build" && options.emitLLVM) { err << "error: '--emit-llvm' is only valid with 'build'\n"; return false; }
  if (options.command != "fmt" && (options.writeInPlace || options.checkOnly || options.indentWidth != 4)) {
    err << "error: fmt options are only valid with 'fmt'\n"; return false;
  }
  if (options.command != "doc" && options.command != "check" && options.jsonOutput) {
    err << "error: '--json' is only valid with 'doc' or 'check'\n"; return false;
  }
  if (options.command != "ir" && options.optimize) { err << "error: '--opt' is only valid with 'ir'\n"; return false; }
  if (options.writeInPlace && options.checkOnly) { err << "error: '--write' and '--check' cannot be combined\n"; return false; }
  options.useColor = isTerminal() && !colorDisabled;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc <= 1) { printUsage(std::cerr); return kExitUsage; }
  Options options;
  if (!parseOptions(argc, argv, options, std::cerr)) return kExitUsage;
  a::DriverConfig config;
  config.checkOnly = options.command == "check";
  config.useColor = options.useColor;
  config.jsonDiagnostics = options.jsonOutput && options.command == "check";
  config.backend = "llvm"; // single backend (Mojo-style LLVM pipeline)
  config.jobs = options.jobs == 0 ? 0 : options.jobs;  // 0 -> analyzeParallel resolves core count
  if (const char* env = std::getenv("A_JOBS")) {
    try { if (options.jobs == 1) config.jobs = std::stoi(env); } catch (...) {}
  }
  config.modulePaths = options.modulePaths;
  config.stdlibRoot = options.stdlibRoot;
  if (options.command == "run") {
    if (!options.commandCode.empty()) {
      return a::runSource("<command>", options.commandCode, config, std::cin, std::cout, std::cerr);
    }
    return a::runFile(options.path, config, std::cin, std::cout, std::cerr);
  }
  if (options.command == "repl") return a::repl(config, std::cin, std::cout, std::cerr);
  if (options.command == "test") return a::testFile(options.path, config, std::cin, std::cout, std::cerr);
  if (options.command == "build") {
    std::string out = options.outputPath;
    if (options.emitLLVM) {
      if (out.empty()) out = options.path + ".ll";
      return a::emitLLVMFile(options.path, out, config, std::cerr);
    }
    if (out.empty()) out = "a.out";
    return a::buildFile(options.path, out, config, "llvm", std::cerr);
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
  if (options.command == "ir") return a::emitIRFile(options.path, config, options.optimize, std::cout, std::cerr);
  std::cerr << "error: unknown command '" << options.command << "'\n";
  printUsage(std::cerr);
  return kExitUsage;
}
