#include "driver/driver.hpp"

#include <fstream>
#include <ios>
#include <istream>
#include <ostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "ast/ast.hpp"
#include "driver/version.hpp"
#include "backend/interpreter.hpp"
#include "backend/native/native_codegen.hpp"
#include "backend/llvm/llvm_backend.hpp"
#include "diagnostics/diagnostics.hpp"
#include "lexer/lexer.hpp"
#include "lexer/source.hpp"
#include "tooling/docgen.hpp"
#include "tooling/formatter.hpp"
#include "lexer/token.hpp"
#include "ir/ir.hpp"
#include "name-resolution/module_loader.hpp"
#include "optimization/optimizer.hpp"
#include "ownership/ownership.hpp"
#include "type-checker/sema.hpp"

namespace a {
namespace {

constexpr int kExitOk = 0;
constexpr int kExitCompileError = 1;
constexpr int kExitRuntimeError = 2;
constexpr int kExitNoInput = 66;

}  // namespace

int runSource(const std::string& displayPath, std::string source, const DriverConfig& config,
              std::istream& in, std::ostream& out, std::ostream& err) {
  DiagnosticSink sink;

  ModuleLoaderOptions options;
  options.modulePaths = config.modulePaths;
  options.stdlibRoot = config.stdlibRoot;
  ModuleLoader loader(std::move(options), sink);
  ModuleGraph graph = loader.load(displayPath, std::move(source));

  const bool json = config.jsonDiagnostics && config.checkOnly;
  if (sink.hasErrors()) {
    if (json) {
      sink.writeJson(out);
    } else {
      sink.flush(err, config.useColor);
    }
    return kExitCompileError;
  }
  if (!json) sink.flush(err, config.useColor);

  Sema sema(graph, sink);
  SemaResult result = sema.analyze();
  if (sink.hasErrors()) {
    if (json) {
      sink.writeJson(out);
    } else {
      sink.flush(err, config.useColor);
    }
    return kExitCompileError;
  }

  analyzeOwnership(graph, sink);
  if (json) {
    sink.writeJson(out);
  } else {
    sink.flush(err, config.useColor);
  }
  if (sink.hasErrors()) return kExitCompileError;

  if (config.checkOnly) return kExitOk;

  if (config.backend == "interpreter") {
    Interpreter interp(graph, result, sink, in, out, err);
    int code = interp.run();
    sink.flush(err, config.useColor);
    return code;
  }
  if (config.backend == "llvm" && isLLVMSupported(graph)) {
    // Run directly via high-speed LLVM code generation
    return runLLVMModule(graph, result, in, out, err);
  }

  if (isNativeSupported(graph)) {
    int code = runNativeBinary(graph, result, in, out, err);
    if (code >= 0) {
      sink.flush(err, config.useColor);
      return code;
    }
    // The native emitter accepted the AST but g++ could not compile the
    // result (e.g. a type mismatch it did not anticipate): fall back to the
    // interpreter so the program still runs correctly.
  }

  Interpreter interp(graph, result, sink, in, out, err);
  int code = interp.run();
  sink.flush(err, config.useColor);
  return code;
}

int runFile(const std::string& path, const DriverConfig& config, std::istream& in,
            std::ostream& out, std::ostream& err) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    err << "error: cannot open '" << path << "'\n";
    return kExitNoInput;
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (file.bad()) {
    err << "error: failed to read '" << path << "'\n";
    return kExitNoInput;
  }

  return runSource(path, buffer.str(), config, in, out, err);
}

int testFile(const std::string& path, const DriverConfig& config, std::istream& in,
             std::ostream& out, std::ostream& err) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    err << "error: cannot open '" << path << "'\n";
    return kExitNoInput;
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (file.bad()) {
    err << "error: failed to read '" << path << "'\n";
    return kExitNoInput;
  }

  DiagnosticSink sink;
  ModuleLoaderOptions options;
  options.modulePaths = config.modulePaths;
  options.stdlibRoot = config.stdlibRoot;
  ModuleLoader loader(std::move(options), sink);
  ModuleGraph graph = loader.load(path, buffer.str());

  sink.flush(err, config.useColor);
  if (sink.hasErrors()) return kExitCompileError;

  Sema sema(graph, sink);
  SemaResult result = sema.analyze();
  if (!sink.hasErrors()) analyzeOwnership(graph, sink);
  sink.flush(err, config.useColor);
  if (sink.hasErrors()) return kExitCompileError;

  Interpreter interp(graph, result, sink, in, out, err);
  int code = interp.runTests(out);
  sink.flush(err, config.useColor);
  return code;
}

int buildFile(const std::string& path, const std::string& outputPath,
              const DriverConfig& config, const std::string& backend, std::ostream& err) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    err << "error: cannot open '" << path << "'\n";
    return kExitNoInput;
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (file.bad()) {
    err << "error: failed to read '" << path << "'\n";
    return kExitNoInput;
  }

  DiagnosticSink sink;
  ModuleLoaderOptions options;
  options.modulePaths = config.modulePaths;
  options.stdlibRoot = config.stdlibRoot;
  ModuleLoader loader(std::move(options), sink);
  ModuleGraph graph = loader.load(path, buffer.str());

  sink.flush(err, config.useColor);
  if (sink.hasErrors()) return kExitCompileError;

  Sema sema(graph, sink);
  SemaResult result = sema.analyze();
  if (!sink.hasErrors()) analyzeOwnership(graph, sink);
  sink.flush(err, config.useColor);
  if (sink.hasErrors()) return kExitCompileError;

  if (backend == "llvm") {
    if (!isLLVMSupported(graph)) {
      err << "error: the LLVM backend cannot compile this program. Closures, dot methods, "
             "and the standard-library modules (json, binary, testing, log, metrics, trace, "
             "net, http) run only in the interpreter: use 'a run'. For anything else, "
             "'--backend=native' may still work.\n";
      return kExitCompileError;
    }
    if (!buildLLVMModule(graph, result, outputPath, err)) {
      return kExitCompileError;
    }
  } else {
    if (!isNativeSupported(graph)) {
      err << "error: the native backend cannot compile this program. Closures, dot methods, "
             "and the standard-library modules (json, binary, testing, log, metrics, trace, "
             "net, http) run only in the interpreter: use 'a run'. For anything else, "
             "'--backend=llvm' may still work.\n";
      return kExitCompileError;
    }
    if (!buildNativeBinary(graph, result, outputPath, err)) {
      return kExitCompileError;
    }
  }
  return kExitOk;
}

int emitLLVMFile(const std::string& path, const std::string& outputPath,
                 const DriverConfig& config, std::ostream& err) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    err << "error: cannot open '" << path << "'\n";
    return kExitNoInput;
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (file.bad()) {
    err << "error: failed to read '" << path << "'\n";
    return kExitNoInput;
  }

  DiagnosticSink sink;
  ModuleLoaderOptions options;
  options.modulePaths = config.modulePaths;
  options.stdlibRoot = config.stdlibRoot;
  ModuleLoader loader(std::move(options), sink);
  ModuleGraph graph = loader.load(path, buffer.str());

  sink.flush(err, config.useColor);
  if (sink.hasErrors()) return kExitCompileError;

  Sema sema(graph, sink);
  SemaResult result = sema.analyze();
  if (!sink.hasErrors()) analyzeOwnership(graph, sink);
  sink.flush(err, config.useColor);
  if (sink.hasErrors()) return kExitCompileError;

  std::ofstream out(outputPath);
  if (!out) {
    err << "error: cannot open '" << outputPath << "' for writing\n";
    return kExitCompileError;
  }

  if (!emitLLVMIR(graph, result, out)) {
    return kExitCompileError;
  }
  return kExitOk;
}

int emitIRFile(const std::string& path, const DriverConfig& config, bool optimize,
               std::ostream& out, std::ostream& err) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    err << "error: cannot open '" << path << "'\n";
    return kExitNoInput;
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (file.bad()) {
    err << "error: failed to read '" << path << "'\n";
    return kExitNoInput;
  }

  DiagnosticSink sink;
  ModuleLoaderOptions options;
  options.modulePaths = config.modulePaths;
  options.stdlibRoot = config.stdlibRoot;
  ModuleLoader loader(std::move(options), sink);
  ModuleGraph graph = loader.load(path, buffer.str());

  sink.flush(err, config.useColor);
  if (sink.hasErrors()) return kExitCompileError;

  Sema sema(graph, sink);
  SemaResult result = sema.analyze();
  if (!sink.hasErrors()) analyzeOwnership(graph, sink);
  sink.flush(err, config.useColor);
  if (sink.hasErrors()) return kExitCompileError;

  ir::Program program = ir::lowerProgram(graph, result);
  if (optimize) ir::runAll(program);
  out << ir::printProgram(program);
  return kExitOk;
}

int formatFile(const std::string& path, const FormatFileOptions& options, std::ostream& out,
               std::ostream& err) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    err << "error: cannot open '" << path << "'\n";
    return kExitNoInput;
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (file.bad()) {
    err << "error: failed to read '" << path << "'\n";
    return kExitNoInput;
  }
  const std::string source = buffer.str();

  DiagnosticSink sink;
  sink.addFile(std::make_shared<SourceFile>(path, source));

  tooling::FormatOptions formatOptions;
  formatOptions.indentWidth = options.indentWidth;
  tooling::FormatResult result = tooling::formatSource(source, sink, formatOptions);
  sink.flush(err, options.useColor);
  if (!result.ok || sink.hasErrors()) return kExitCompileError;

  if (options.checkOnly) {
    if (result.text != source) {
      err << path << ": needs formatting\n";
      return kExitCompileError;
    }
    return kExitOk;
  }

  if (options.writeInPlace) {
    std::ofstream target(path, std::ios::binary | std::ios::trunc);
    if (!target) {
      err << "error: cannot open '" << path << "' for writing\n";
      return kExitCompileError;
    }
    target << result.text;
    if (!target) {
      err << "error: failed to write '" << path << "'\n";
      return kExitCompileError;
    }
    return kExitOk;
  }

  out << result.text;
  return kExitOk;
}

int documentFile(const std::string& path, const DocFileOptions& options, std::ostream& out,
                 std::ostream& err) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    err << "error: cannot open '" << path << "'\n";
    return kExitNoInput;
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (file.bad()) {
    err << "error: failed to read '" << path << "'\n";
    return kExitNoInput;
  }
  const std::string source = buffer.str();

  DiagnosticSink sink;
  sink.addFile(std::make_shared<SourceFile>(path, source));

  tooling::DocOptions docOptions;
  docOptions.sourceName = path;
  tooling::DocResult result = tooling::extractDocs(source, sink, docOptions);
  sink.flush(err, options.useColor);
  if (!result.ok || sink.hasErrors()) return kExitCompileError;

  const std::string text =
      options.json ? tooling::renderJson(result.module) : tooling::renderMarkdown(result.module);

  if (!options.outputPath.empty()) {
    std::ofstream target(options.outputPath, std::ios::binary | std::ios::trunc);
    if (!target) {
      err << "error: cannot open '" << options.outputPath << "' for writing\n";
      return kExitCompileError;
    }
    target << text;
    if (!target) {
      err << "error: failed to write '" << options.outputPath << "'\n";
      return kExitCompileError;
    }
    return kExitOk;
  }

  out << text;
  return kExitOk;
}

}  // namespace a
namespace a {
namespace {
// REPL helpers (translation-unit local).
bool replIsComplete(const std::string& src) {
  // Count bracket depth ignoring strings/comments; complete when balanced and
  // no trailing colon-continuation expected (indent blocks are handled by
  // the parser accepting a trailing blank suite via a sentinel newline).
  int depth = 0;
  bool inStr = false;
  char q = 0;
  for (std::size_t i = 0; i < src.size(); ++i) {
    char c = src[i];
    if (inStr) {
      if (c == '\\') { ++i; continue; }
      if (c == q) inStr = false;
      continue;
    }
    if (c == '"' || c == '\'') { inStr = true; q = c; continue; }
    if (c == '#') { while (i < src.size() && src[i] != '\n') ++i; continue; }
    if (c == '(' || c == '[' || c == '{') ++depth;
    else if (c == ')' || c == ']' || c == '}') --depth;
  }
  return depth <= 0 && !inStr;
}

std::string replTrim(const std::string& s) {
  std::size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  std::size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}
}  // namespace

int repl(const DriverConfig& config, std::istream& in, std::ostream& out, std::ostream& err) {
  out << "A REPL " << kVersion << "\n"
      << "Type 'help' for commands, 'exit' to quit.\n";
  std::string history;      // all accepted source so far (persistent names)
  std::string pending;      // current incomplete chunk
  std::string line;
  while (true) {
    out << (pending.empty() ? ">>> " : "... ");
    out.flush();
    if (!std::getline(in, line)) break;
    std::string t = replTrim(line);
    if (pending.empty()) {
      if (t.empty()) continue;
      if (t == "exit" || t == "quit") break;
      if (t == "help") {
        out << "Commands:\n"
            << "  help    show this help\n"
            << "  exit    quit the REPL (also: quit)\n"
            << "  clear   forget all variables/functions defined so far\n"
            << "Any chunk of A code works: assignments, fn, if/for/while\n"
            << "blocks (indent to continue, blank line to run), print(),\n"
            << "and bare expressions echo their value, like Python.\n";
        continue;
      }
      if (t == "clear") { history.clear(); out << "Session cleared.\n"; continue; }
    }
    bool blank = t.empty();
    if (blank && !pending.empty()) {
      // Blank line terminates a pending block: execute what we have.
    } else {
      pending += line + "\n";
      if (!replIsComplete(pending)) continue;
    }
    // Strip trailing blank lines so a block suite terminates cleanly.
    std::string chunk = pending;
    pending.clear();
    while (!chunk.empty() && (chunk.back() == '\n')) chunk.pop_back();
    if (replTrim(chunk).empty()) continue;
    // Echo bare expressions: wrap a lone expression statement in print().
    DriverConfig c = config;
    c.checkOnly = false;
    c.backend = "interpreter";
    // Echo bare expressions like Python: a lone expression statement
    // (no top-level '=' and not starting with a statement keyword) gets
    // wrapped in print() so its value shows.
    {
      std::string c = replTrim(chunk);
      bool isExpr = true;
      static const char* kKw[] = {"fn ", "if ", "while ", "for ", "loop", "return", "break", "continue", "import ", "from ", "defer ", "class ", "struct ", "enum ", "type ", "do:"};
      for (const char* kw : kKw) {
        std::string k = kw;
        if (c.rfind(k, 0) == 0) { isExpr = false; break; }
      }
      // top-level '=' means assignment, not a bare expression
      int d = 0;
      for (char ch : c) {
        if (ch == '(' || ch == '[' || ch == '{') ++d;
        else if (ch == ')' || ch == ']' || ch == '}') --d;
        else if (ch == '=' && d == 0) { isExpr = false; break; }
      }
      if (isExpr && c.find('\n') == std::string::npos &&
          c.find(':') == std::string::npos && c.rfind("print(", 0) != 0)
        chunk = "print(" + c + ")";
    }
    std::string src = history + chunk + "\n";
    {
      DiagnosticSink probeSink;
      ModuleLoaderOptions po;
      po.modulePaths = c.modulePaths;
      po.stdlibRoot = c.stdlibRoot;
      ModuleLoader probeLoader(std::move(po), probeSink);
      std::string probeSrc = src;
      // A trailing indented block needs a dedent line to parse; probeSrc
      // is only used to decide whether to commit, so a parse failure here
      // caused solely by EOF-after-block is treated as success below.
      if (replTrim(probeSrc).find(':') != std::string::npos) probeSrc += "\n";
      probeLoader.load("<repl>", probeSrc);
      if (probeSink.hasErrors()) {
        // EOF-after-block is a probe artifact, not a real error: the
        // block body is present, the file simply ends. Treat any error
        // mentioning this as success and commit.
        bool eofArtifact = false;
        for (const auto& d : probeSink.diagnostics()) {
          (void)d;
          eofArtifact = true;  // placeholder, refined below
        }
        (void)eofArtifact;
        // Real check: re-probe with a trailing top-level statement.
        DiagnosticSink s2;
        ModuleLoaderOptions o2;
        o2.modulePaths = c.modulePaths;
        o2.stdlibRoot = c.stdlibRoot;
        ModuleLoader l2(std::move(o2), s2);
        std::string s2src = src + "print(0)\n";
        l2.load("<repl>", s2src);
        if (s2.hasErrors()) {
          s2.flush(err, c.useColor);
          continue;
        }
      }
    }
    history += chunk + "\n";
    history += chunk + "\n";
    // Run cumulative source through the interpreter only (fast, stateful via
    // re-execution of definitions; prints occur each time like notebook cells
    // without side-effectful re-runs of print statements in history is NOT
    // guaranteed - see note: we re-run whole history for simplicity).
    DiagnosticSink sink;
    ModuleLoaderOptions mo;
    mo.modulePaths = c.modulePaths;
    mo.stdlibRoot = c.stdlibRoot;
    ModuleLoader loader(std::move(mo), sink);
    ModuleGraph graph = loader.load("<repl>", src);
    sink.flush(err, c.useColor);
    if (!sink.hasErrors()) {
      Sema sema(graph, sink);
      SemaResult result = sema.analyze();
      sink.flush(err, c.useColor);
      if (!sink.hasErrors()) {
        analyzeOwnership(graph, sink);
        sink.flush(err, c.useColor);
        if (!sink.hasErrors()) {
          Interpreter interp(graph, result, sink, in, out, err);
          interp.run();
          sink.flush(err, c.useColor);
        }
      }
    }
  }
  out << "\n";
  return 0;
}
}  // namespace a
