// Format.cpp - indent Cruft source. See Format.h.
#include "Format.h"

#include <cctype>
#include <string>
#include <string_view>

namespace cruft {

namespace {

bool isIdentChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool wordAt(std::string_view s, size_t i, std::string_view word) {
  if (i + word.size() > s.size() || s.substr(i, word.size()) != word)
    return false;
  if (i > 0 && isIdentChar(s[i - 1]))
    return false;
  if (i + word.size() < s.size() && isIdentChar(s[i + word.size()]))
    return false;
  return true;
}

bool startsWithWord(std::string_view s, std::string_view word) { return wordAt(s, 0, word); }

bool endsWithWord(std::string_view s, std::string_view word) {
  size_t end = s.size();
  while (end > 0 && std::isspace(static_cast<unsigned char>(s[end - 1])))
    --end;
  if (end < word.size())
    return false;
  return wordAt(s, end - word.size(), word);
}

bool containsWord(std::string_view s, std::string_view word) {
  for (size_t i = 0; i < s.size(); ++i)
    if (wordAt(s, i, word))
      return true;
  return false;
}

size_t skipPrefixes(std::string_view s) {
  size_t i = 0;
  for (;;) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
      ++i;
    bool skipped = false;
    for (std::string_view pre : {"pub", "pri", "extern", "const"}) {
      if (wordAt(s, i, pre)) {
        i += pre.size();
        skipped = true;
        break;
      }
    }
    if (!skipped)
      return i;
  }
}

std::string codeOf(const std::string &line, bool &inBlock) {
  std::string code;
  bool inString = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (inBlock) {
      if (c == '*' && i + 1 < line.size() && line[i + 1] == '/') {
        inBlock = false;
        ++i;
      }
      continue;
    }
    if (inString) {
      code.push_back(c);
      if (c == '"' && (i == 0 || line[i - 1] != '\\'))
        inString = false;
      continue;
    }
    if (c == '/' && i + 1 < line.size() && line[i + 1] == '/')
      break;
    if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
      inBlock = true;
      ++i;
      continue;
    }
    if (c == '"')
      inString = true;
    code.push_back(c);
  }
  return code;
}

std::string trimEnds(const std::string &line) {
  size_t begin = 0;
  while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t'))
    ++begin;
  size_t end = line.size();
  while (end > begin && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\r'))
    --end;
  return line.substr(begin, end - begin);
}

bool opensBlock(std::string_view code) {
  if (containsWord(code, "end"))
    return false;
  if (endsWithWord(code, "then") || endsWithWord(code, "do"))
    return true;
  size_t i = skipPrefixes(code);
  std::string_view rest = code.substr(i);
  while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front())))
    rest.remove_prefix(1);
  for (std::string_view word : {"fn", "struct", "enum", "trait", "impl", "match", "unsafe", "drop"})
    if (startsWithWord(rest, word))
      return true;
  return false;
}

} // namespace

std::string formatSource(const std::string &source) {
  std::string out;
  int indent = 0;
  bool inBlock = false;
  bool blank = false;
  bool any = false;
  size_t i = 0;
  while (i < source.size()) {
    size_t nl = source.find('\n', i);
    std::string raw = source.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
    i = nl == std::string::npos ? source.size() : nl + 1;
    if (!raw.empty() && raw.back() == '\r')
      raw.pop_back();
    std::string code = codeOf(raw, inBlock);
    size_t c0 = 0;
    while (c0 < code.size() && std::isspace(static_cast<unsigned char>(code[c0])))
      ++c0;
    std::string_view view(code.data() + c0, code.size() - c0);
    std::string text = trimEnds(raw);
    if (text.empty()) {
      if (any)
        blank = true;
      continue;
    }
    if (blank) {
      out.push_back('\n');
      blank = false;
    }
    int shown = indent;
    bool closer = startsWithWord(view, "end");
    bool elseLine = startsWithWord(view, "else");
    if (closer || elseLine)
      shown = indent > 0 ? indent - 1 : 0;
    out.append(static_cast<size_t>(shown) * 4, ' ');
    out += text;
    out.push_back('\n');
    any = true;
    if (closer) {
      if (indent > 0)
        --indent;
    } else if (!elseLine && opensBlock(view)) {
      ++indent;
    }
  }
  return out;
}

} // namespace cruft
