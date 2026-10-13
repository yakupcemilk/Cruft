// Diagnostics.cpp - snippet rendering and panic-mode cascade suppression.
#include "Diagnostics.h"

namespace cruft {

void DiagnosticEngine::registerSource(const std::string &filename, std::string source) {
  sources_[filename] = std::move(source);
}

void DiagnosticEngine::report(Severity sev, SourceLocation loc, std::string msg) {
  if (sev == Severity::Error)
    ++errorCount_;
  diags_.push_back({sev, std::move(loc), std::move(msg)});
}

void DiagnosticEngine::error(SourceLocation loc, std::string msg) {
  if (panic_) {
    ++suppressed_;
    return;
  }
  report(Severity::Error, std::move(loc), std::move(msg));
}

void DiagnosticEngine::warning(SourceLocation loc, std::string msg) {
  if (panic_) {
    ++suppressed_;
    return;
  }
  report(Severity::Warning, std::move(loc), std::move(msg));
}

void DiagnosticEngine::note(SourceLocation loc, std::string msg) {
  if (panic_) {
    ++suppressed_;
    return;
  }
  report(Severity::Note, std::move(loc), std::move(msg));
}

void DiagnosticEngine::beginPanic() { panic_ = true; }

void DiagnosticEngine::endPanic() { panic_ = false; }

namespace {

std::string lineText(const std::string &src, unsigned line) {
  if (line == 0)
    return {};
  unsigned current = 1;
  size_t i = 0;
  while (current < line && i < src.size()) {
    if (src[i++] == '\n')
      ++current;
  }
  if (current != line)
    return {};
  size_t start = i;
  while (i < src.size() && src[i] != '\n')
    ++i;
  std::string text = src.substr(start, i - start);
  if (!text.empty() && text.back() == '\r')
    text.pop_back();
  return text;
}

} // namespace

void DiagnosticEngine::printSnippet(std::ostream &os, const SourceLocation &loc) const {
  if (!loc.file || loc.line == 0)
    return;
  auto it = sources_.find(*loc.file);
  if (it == sources_.end())
    return;
  std::string text = lineText(it->second, loc.line);
  unsigned col = loc.column == 0 ? 1 : loc.column;
  if (col > text.size() + 1)
    col = static_cast<unsigned>(text.size()) + 1;
  const std::string gutter = std::to_string(loc.line);
  os << std::string(gutter.size() + 1, ' ') << "|\n";
  os << gutter << " | " << text << '\n';
  os << std::string(gutter.size() + 1, ' ') << "| "
     << std::string(col - 1, ' ') << "^\n";
}

void DiagnosticEngine::print(std::ostream &os) const {
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
    printSnippet(os, d.loc);
  }
  if (suppressed_ > 0)
    os << "note: " << suppressed_ << " cascading diagnostic(s) suppressed\n";
}

} // namespace cruft
