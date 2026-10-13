// Diagnostics.h - source locations and diagnostic reporting shared by every
// compiler phase (Lexer -> Parser -> TypeChecker -> CodeGen).
#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace cruft {

/// Precise position of a token / AST node inside a source file.
struct SourceLocation {
  std::shared_ptr<const std::string> file; ///< shared so copies stay cheap
  unsigned line = 0;                       ///< 1-based
  unsigned column = 0;                     ///< 1-based

  [[nodiscard]] std::string str() const {
    std::string out = file ? *file : std::string("<unknown>");
    out += ':' + std::to_string(line) + ':' + std::to_string(column);
    return out;
  }
};

enum class Severity { Note, Warning, Error };

struct Diagnostic {
  Severity severity;
  SourceLocation loc;
  std::string message;
};

/// Collects diagnostics for the whole pipeline. Phases never throw for user
/// errors; they report here and the driver decides whether to continue.
///
/// Panic mode (parser recovery) suppresses every diagnostic that follows the
/// root fault until `endPanic()`, so one bad token does not flood the user
/// with cascading errors. `print` renders a source snippet and a caret.
class DiagnosticEngine {
public:
  void registerSource(const std::string &filename, std::string source);

  void report(Severity sev, SourceLocation loc, std::string msg);
  void error(SourceLocation loc, std::string msg);
  void warning(SourceLocation loc, std::string msg);
  void note(SourceLocation loc, std::string msg);

  /// Drop further diagnostics until `endPanic`. The root error is reported
  /// by the caller before entering panic mode.
  void beginPanic();
  void endPanic();
  [[nodiscard]] bool inPanic() const { return panic_; }

  /// Count a diagnostic that was deliberately not emitted (lexer junk runs).
  void noteSuppressed() { ++suppressed_; }

  [[nodiscard]] bool hasErrors() const { return errorCount_ > 0; }
  [[nodiscard]] unsigned errorCount() const { return errorCount_; }
  [[nodiscard]] unsigned suppressedCount() const { return suppressed_; }
  [[nodiscard]] const std::vector<Diagnostic> &all() const { return diags_; }

  void print(std::ostream &os) const;

private:
  void printSnippet(std::ostream &os, const SourceLocation &loc) const;

  std::vector<Diagnostic> diags_;
  std::unordered_map<std::string, std::string> sources_;
  unsigned errorCount_ = 0;
  unsigned suppressed_ = 0;
  bool panic_ = false;
};

} // namespace cruft
