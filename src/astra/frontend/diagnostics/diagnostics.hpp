#pragma once

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lexer/source.hpp"
#include "lexer/span.hpp"

namespace a {

using FileId = int;

enum class Severity { Error, Warning };

struct Diagnostic {
  Severity severity = Severity::Error;
  std::string code;  // stable identifier when available, e.g. "W0001"
  Span span;
  std::string message;
  std::string help;
};

// Collects diagnostics and renders them with source context. The sink keeps
// insertion order: errors from later compiler phases are reported after
// earlier phases, matching the pipeline. It also owns the source registry used
// to map spans back to files in multi-file programs.
//
// Thread-safe (ty/pyrefly checkers emit diagnostics from worker threads):
// error/warning/addFile/diagnosticsCopy/flush may run concurrently. The
// `diagnostics()` accessor returns the live vector and must only be called
// when no writer is active; prefer `diagnosticsCopy()` otherwise.
class DiagnosticSink {
 public:
  DiagnosticSink() = default;
  DiagnosticSink(const DiagnosticSink&) = delete;
  DiagnosticSink& operator=(const DiagnosticSink&) = delete;

  FileId addFile(std::shared_ptr<const SourceFile> source);
  const SourceFile* file(FileId id) const;

  void error(Span span, std::string message, std::string help = {});
  // Errors may also carry a stable code, like ty/pyrefly: E0001 undefined
  // name, E0002 type mismatch, E0003 missing return, E0004 bad call,
  // E0005 bad member, E0006 duplicate declaration, E0007 control flow.
  void errorWithCode(Span span, std::string code, std::string message, std::string help = {});
  // Warnings carry a stable code (for example "W0001") so tooling and
  // documentation can refer to them unambiguously.
  void warning(Span span, std::string code, std::string message, std::string help = {});

  // Suppression comments, like ty (`# ty: ignore`) and pyrefly
  // (`# pyrefly: ignore`): a trailing `# a: ignore`, `# type: ignore`,
  // `# ty: ignore`, or `# pyrefly: ignore` on a line suppresses diagnostics
  // whose span starts on that line. An optional `[CODE]` filters by code,
  // e.g. `# a: ignore[W0001]`.
  bool isSuppressed(Span span, const std::string& code = {}) const;
  bool isSuppressed(Span span, Severity severity, const std::string& code) const;

  bool hasErrors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return errors_ > 0;
  }
  int errorCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return errors_;
  }

  // Diagnostics collected so far, in insertion order. Language tooling uses
  // this to translate compiler diagnostics into editor diagnostics without
  // going through the terminal renderer.
  const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

  // Thread-safe snapshot of everything collected so far.
  std::vector<Diagnostic> diagnosticsCopy() const;

  // Prints diagnostics added since the previous flush. This lets each
  // pipeline stage flush without reprinting earlier output.
  void flush(std::ostream& os, bool useColor);

  // Writes every collected diagnostic as a JSON object
  // ({"diagnostics":[...]}), for machine-readable editor/tooling output.
  void writeJson(std::ostream& os) const;

 private:
  // file() body without locking; callers that already hold mutex_ must use
  // this to avoid self-deadlock on the non-recursive mutex.
  const SourceFile* fileLocked(FileId id) const;

  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<const SourceFile>> files_;
  std::vector<Diagnostic> diagnostics_;
  std::size_t flushed_ = 0;
  int errors_ = 0;
};

}  // namespace a
