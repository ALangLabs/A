//===- package_cli.cpp - `a precompile` subcommand -------------------------===//
//
// Part of the A toolchain (src/astra/mlir). See docs/mlir-architecture.md
// ("Bytecode format") and package.hpp for the integration spec.
//
// Argument parsing mirrors src/astra/frontend/cli/main.cpp conventions:
// same option spellings (-o, -I, --stdlib, -j/--jobs/--parallel,
// --no-color), same exit codes (0 ok, 1 error, 64 usage / kExitUsage).
//
//===----------------------------------------------------------------------===//

#include "package.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#define A_HAVE_ISATTY 1
#endif

#include "pipeline/pipeline.hpp"

namespace a {
namespace {

constexpr int kExitUsage = 64;

void printPrecompileUsage(std::ostream& os) {
  os << "Usage:\n"
     << "  a precompile <file.astra|package-dir> [-o out.ambc]\n"
     << "\n"
     << "Compiles a source file or package directory through the frontend\n"
     << "pipeline (parse, comptime, Sema, ownership) and writes the checked\n"
     << "module graph as MLIR bytecode (.ambc), the Mojo .mojoc analog.\n"
     << "\n"
     << "Arguments:\n"
     << "  <file.astra|dir>  entry source file, or a package directory\n"
     << "                     (resolved via <dir>/__init__.astra, then\n"
     << "                     <dir>/<dirname>.astra)\n"
     << "\n"
     << "Options:\n"
     << "  -o <file>        output .ambc path (default: <entry>.ambc)\n"
     << "  -I <dir>         add module search directory (repeatable)\n"
     << "  --stdlib <dir>   override standard library directory\n"
     << "  --parallel       check modules in parallel\n"
     << "  -j <n>, --jobs <n>  parallel jobs\n"
     << "  --no-color       disable colored diagnostics\n"
     << "  -h, --help       show this message\n";
}

bool isTerminal() {
#ifdef A_HAVE_ISATTY
  return isatty(fileno(stderr)) != 0;
#else
  return false;
#endif
}

// Same apm project discovery as cli/main.cpp: for a file inside an
// apm.json project, `<root>/.apm/packages` is prepended to the module
// search paths so `import <pkg>` resolves during precompile too.
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

struct PrecompileOptions {
  std::string path;
  std::string outputPath;
  std::vector<std::string> modulePaths;
  std::string stdlibRoot;
  std::string target;
  bool useColor = false;
  int jobs = 1;
};

bool parsePrecompileArgs(int argc, char** argv, int start,
                         PrecompileOptions& options, std::ostream& err) {
  std::vector<std::string> positional;
  bool colorDisabled = false;
  for (int i = start; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-o") {
      if (i + 1 >= argc) { err << "error: '-o' requires a path\n"; return false; }
      options.outputPath = argv[++i];
    } else if (arg == "-I") {
      if (i + 1 >= argc) { err << "error: '-I' requires a directory\n"; return false; }
      options.modulePaths.push_back(argv[++i]);
    } else if (arg.rfind("-I", 0) == 0 && arg.size() > 2) {
      options.modulePaths.push_back(arg.substr(2));
    } else if (arg == "--stdlib") {
      if (i + 1 >= argc) { err << "error: '--stdlib' requires a directory\n"; return false; }
      options.stdlibRoot = argv[++i];
    } else if (arg == "--target" || arg == "-target") {
      if (i + 1 >= argc) { err << "error: '--target' requires a value\n"; return false; }
      options.target = argv[++i];
    } else if (arg.rfind("--target=", 0) == 0) {
      options.target = arg.substr(9);
    } else if (arg == "--parallel") {
      options.jobs = 0;
    } else if (arg == "--jobs" || arg == "-j") {
      if (i + 1 >= argc) { err << "error: '--jobs' requires a number\n"; return false; }
      try { options.jobs = std::stoi(argv[++i]); } catch (...) { err << "error: invalid jobs value\n"; return false; }
      if (options.jobs < 0) { err << "error: jobs must be >= 0\n"; return false; }
    } else if (arg.rfind("--jobs=", 0) == 0 || arg.rfind("-j=", 0) == 0) {
      try { options.jobs = std::stoi(arg.substr(arg.find('=') + 1)); } catch (...) { err << "error: invalid jobs value\n"; return false; }
      if (options.jobs < 0) { err << "error: jobs must be >= 0\n"; return false; }
    } else if (arg.rfind("-j", 0) == 0 && arg.size() > 2) {
      try { options.jobs = std::stoi(arg.substr(2)); } catch (...) { err << "error: invalid jobs value\n"; return false; }
      if (options.jobs < 0) { err << "error: jobs must be >= 0\n"; return false; }
    } else if (arg == "--no-color") {
      colorDisabled = true;
    } else if (arg == "-h" || arg == "--help") {
      printPrecompileUsage(std::cout);
      std::exit(0);
    } else if (!arg.empty() && arg[0] == '-') {
      err << "error: unknown option '" << arg << "'\n";
      return false;
    } else {
      positional.push_back(arg);
    }
  }
  if (positional.empty()) {
    printPrecompileUsage(err);
    return false;
  }
  if (positional.size() > 1) {
    err << "error: unexpected argument '" << positional[1] << "'\n";
    return false;
  }
  options.path = positional[0];
  options.useColor = isTerminal() && !colorDisabled;
  return true;
}

} // namespace

int runPrecompileCli(int argc, char** argv, std::ostream& out,
                     std::ostream& err) {
  // Accept either the raw process argv (argv[1] == "precompile") or argv
  // already shifted past the subcommand.
  int start = 1;
  if (argc > 1 && std::string(argv[1]) == "precompile") start = 2;
  if (start >= argc) {
    printPrecompileUsage(err);
    return kExitUsage;
  }

  PrecompileOptions options;
  if (!parsePrecompileArgs(argc, argv, start, options, err)) return kExitUsage;

  PipelineConfig config;
  config.useColor = options.useColor;
  config.jobs = options.jobs; // 0 -> analyzeParallel resolves core count
  config.modulePaths = options.modulePaths;
  config.stdlibRoot = options.stdlibRoot;
  config.target = options.target; // accepted for flag parity; unused by emit

  // apm packages: same automatic <root>/.apm/packages discovery as the
  // run/build/check/test commands in cli/main.cpp.
  {
    namespace fs = std::filesystem;
    const std::string root = findProjectDir(options.path);
    if (!root.empty()) {
      std::error_code ec;
      const fs::path pkgDir = fs::path(root) / ".apm" / "packages";
      if (fs::is_directory(pkgDir, ec)) {
        config.modulePaths.insert(config.modulePaths.begin(),
                                  pkgDir.string());
      }
    }
  }

  const int rc = precompilePackage(options.path, options.outputPath, config,
                                   err);
  if (rc != 0) return rc;

  const std::string shown =
      options.outputPath.empty()
          ? defaultAmbcOutputPath(options.path)
          : options.outputPath;
  out << "wrote " << (shown.empty() ? "<ambc>" : shown) << "\n";
  return 0;
}

} // namespace a
