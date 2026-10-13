#include "Manifest.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string_view>

namespace cruft {

namespace {

std::string trim(std::string_view text) {
  size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
    ++begin;
  size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
    --end;
  return std::string(text.substr(begin, end - begin));
}

bool parseString(std::string_view text, size_t &i, std::string &out) {
  if (i >= text.size() || text[i] != '"')
    return false;
  ++i;
  out.clear();
  while (i < text.size() && text[i] != '"') {
    if (text[i] == '\\' && i + 1 < text.size()) {
      ++i;
      out.push_back(text[i]);
    } else {
      out.push_back(text[i]);
    }
    ++i;
  }
  if (i >= text.size() || text[i] != '"')
    return false;
  ++i;
  return true;
}

bool parseStringList(std::string_view text, std::vector<std::string> &out) {
  size_t i = 0;
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
    ++i;
  if (i >= text.size() || text[i] != '[')
    return false;
  ++i;
  while (i < text.size()) {
    while (i < text.size() && (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ','))
      ++i;
    if (i < text.size() && text[i] == ']')
      return true;
    std::string item;
    if (!parseString(text, i, item))
      return false;
    out.push_back(std::move(item));
  }
  return false;
}

} // namespace

std::string Manifest::discover(const std::string &sourceFile) {
  namespace fs = std::filesystem;
  fs::path dir = fs::path(sourceFile).parent_path();
  fs::path candidate = dir.empty() ? fs::path("cruft.toml") : dir / "cruft.toml";
  std::error_code ec;
  if (fs::is_regular_file(candidate, ec))
    return candidate.string();
  return {};
}

bool Manifest::loadFile(const std::string &path, DiagnosticEngine &diags) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    diags.error(SourceLocation{}, "cannot read manifest '" + path + "'");
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  diags.registerSource(path, text);
  auto file = std::make_shared<const std::string>(path);

  std::string section;
  unsigned lineNo = 0;
  std::istringstream lines(text);
  std::string line;
  bool ok = true;
  while (std::getline(lines, line)) {
    ++lineNo;
    std::string raw = trim(line);
    if (!raw.empty() && raw.back() == '\r')
      raw.pop_back();
    raw = trim(raw);
    if (raw.empty() || raw[0] == '#')
      continue;
    SourceLocation loc{file, lineNo, 1};
    if (raw.front() == '[') {
      if (raw.back() != ']') {
        diags.error(loc, "manifest section is missing ']'");
        ok = false;
        continue;
      }
      section = trim(std::string_view(raw).substr(1, raw.size() - 2));
      if (section.empty()) {
        diags.error(loc, "manifest section name is empty");
        ok = false;
      }
      continue;
    }
    size_t eq = raw.find('=');
    if (eq == std::string::npos) {
      diags.error(loc, "manifest entry must be 'key = value'");
      ok = false;
      continue;
    }
    std::string key = trim(std::string_view(raw).substr(0, eq));
    std::string value = trim(std::string_view(raw).substr(eq + 1));
    if (key.empty() || value.empty()) {
      diags.error(loc, "manifest entry must be 'key = value'");
      ok = false;
      continue;
    }

    auto takeString = [&](std::string &dest) {
      size_t i = 0;
      if (!parseString(value, i, dest) || trim(std::string_view(value).substr(i)).size() != 0) {
        diags.error(loc, "manifest value for '" + key + "' must be a quoted string");
        ok = false;
        return false;
      }
      return true;
    };
    auto takeList = [&](std::vector<std::string> &dest) {
      if (!parseStringList(value, dest)) {
        diags.error(loc, "manifest value for '" + key + "' must be an array of strings");
        ok = false;
        return false;
      }
      return true;
    };

    if (section == "package") {
      if (key == "name")
        takeString(packageName);
      else if (key == "version")
        takeString(version);
    } else if (section == "target") {
      if (key == "triple")
        takeString(targetTriple);
    } else if (section == "link") {
      if (key == "libraries")
        takeList(libraries);
      else if (key == "search_paths")
        takeList(libraryPaths);
    } else if (section == "modules") {
      if (key == "paths")
        takeList(modulePaths);
    } else if (section == "dependencies") {
      std::string ignored;
      if (takeString(ignored))
        dependencies.push_back(key);
    } else if (!section.empty()) {
      diags.warning(loc, "ignoring unknown manifest section '" + section + "'");
    } else {
      diags.error(loc, "manifest entry '" + key + "' is outside a section");
      ok = false;
    }
  }
  return ok && !diags.hasErrors();
}

} // namespace cruft
