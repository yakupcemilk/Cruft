// Diagnostics.h - source locations and diagnostic reporting shared by every
// compiler phase (Lexer -> Parser -> TypeChecker -> CodeGen).
#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <string_view>
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
class DiagnosticEngine {
public:
  void report(Severity sev, SourceLocation loc, std::string msg) {
    if (sev == Severity::Error)
      ++errorCount_;
    diags_.push_back({sev, std::move(loc), std::move(msg)});
  }
  void error(SourceLocation loc, std::string msg) {
    report(Severity::Error, std::move(loc), std::move(msg));
  }
  void warning(SourceLocation loc, std::string msg) {
    report(Severity::Warning, std::move(loc), std::move(msg));
  }
  void note(SourceLocation loc, std::string msg) {
    report(Severity::Note, std::move(loc), std::move(msg));
  }

  [[nodiscard]] bool hasErrors() const { return errorCount_ > 0; }
  [[nodiscard]] unsigned errorCount() const { return errorCount_; }
  [[nodiscard]] const std::vector<Diagnostic> &all() const { return diags_; }

  void print(std::ostream &os) const {
    for (const auto &d : diags_) {
      os << d.loc.str() << ": ";
      switch (d.severity) {
      case Severity::Note:
        os << "note: ";
        break;
      case Severity::Warning:
        os << "warning: ";
        break;
      case Severity::Error:
        os << "error: ";
        break;
      }
      os << d.message << '\n';
    }
  }

private:
  std::vector<Diagnostic> diags_;
  unsigned errorCount_ = 0;
};

} // namespace cruft
