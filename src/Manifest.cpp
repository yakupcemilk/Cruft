#include "Manifest.h"

#include <cctype>
#include <cstdint>
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
      std::string path;
      if (takeString(path))
        dependencies.push_back(Dependency{key, std::move(path)});
    } else if (!section.empty()) {
      diags.warning(loc, "ignoring unknown manifest section '" + section + "'");
    } else {
      diags.error(loc, "manifest entry '" + key + "' is outside a section");
      ok = false;
    }
  }
  return ok && !diags.hasErrors();
}

namespace {

uint32_t rotl(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

std::string sha256(const std::string &data) {
  static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
  uint32_t h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
  std::string msg = data;
  uint64_t bits = static_cast<uint64_t>(msg.size()) * 8;
  msg.push_back(static_cast<char>(0x80));
  while ((msg.size() % 64) != 56)
    msg.push_back('\0');
  for (int shift = 56; shift >= 0; shift -= 8)
    msg.push_back(static_cast<char>((bits >> shift) & 0xff));
  for (size_t off = 0; off < msg.size(); off += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      size_t p = off + static_cast<size_t>(i) * 4;
      w[i] = (static_cast<uint32_t>(static_cast<unsigned char>(msg[p])) << 24) |
             (static_cast<uint32_t>(static_cast<unsigned char>(msg[p + 1])) << 16) |
             (static_cast<uint32_t>(static_cast<unsigned char>(msg[p + 2])) << 8) |
             static_cast<uint32_t>(static_cast<unsigned char>(msg[p + 3]));
    }
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = rotl(w[i - 15], 25) ^ rotl(w[i - 15], 14) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotl(w[i - 2], 15) ^ rotl(w[i - 2], 13) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
    for (int i = 0; i < 64; ++i) {
      uint32_t s1 = rotl(e, 26) ^ rotl(e, 21) ^ rotl(e, 7);
      uint32_t ch = (e & f) ^ ((~e) & g);
      uint32_t t1 = h + s1 + ch + k[i] + w[i];
      uint32_t s0 = rotl(a, 30) ^ rotl(a, 19) ^ rotl(a, 10);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
    h5 += f;
    h6 += g;
    h7 += h;
  }
  uint32_t words[8] = {h0, h1, h2, h3, h4, h5, h6, h7};
  static const char *hexd = "0123456789abcdef";
  std::string out(64, '0');
  for (int i = 0; i < 8; ++i) {
    for (int n = 0; n < 8; ++n)
      out[static_cast<size_t>(i * 8 + n)] = hexd[(words[i] >> (28 - n * 4)) & 0xf];
  }
  return out;
}

std::string slashes(std::string path) {
  for (char &c : path)
    if (c == '\\')
      c = '/';
  return path;
}

std::string readBytes(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

} // namespace

bool Manifest::lockDependencies(const std::string &manifestFile, DiagnosticEngine &diags, bool update,
                                std::vector<ResolvedDependency> &out) const {
  namespace fs = std::filesystem;
  SourceLocation loc;
  loc.file = std::make_shared<const std::string>(manifestFile);
  loc.line = 1;
  loc.column = 1;
  const fs::path base = fs::path(manifestFile).parent_path();
  std::vector<ResolvedDependency> resolved;
  for (const Dependency &dep : dependencies) {
    if (dep.path.empty()) {
      diags.error(loc, "dependency '" + dep.name + "' has no path");
      return false;
    }
    if (dep.path.find('#') != std::string::npos || dep.path.find('"') != std::string::npos) {
      diags.error(loc, "dependency '" + dep.name + "' path cannot contain '#' or quotes");
      return false;
    }
    std::string written = slashes(dep.path);
    fs::path candidate = fs::path(dep.path);
    if (candidate.is_relative())
      candidate = base / candidate;
    std::error_code ec;
    fs::path file;
    if (fs::is_directory(candidate, ec)) {
      fs::path mod = candidate / "mod.cruft";
      fs::path named = candidate / (dep.name + ".cruft");
      if (fs::is_regular_file(mod))
        file = mod;
      else if (fs::is_regular_file(named))
        file = named;
      else {
        diags.error(loc, "dependency '" + dep.name + "' path '" + written + "' was not found");
        return false;
      }
      fs::path rel = fs::relative(file, base, ec);
      if (!ec)
        written = slashes(rel.string());
    } else if (fs::is_regular_file(candidate, ec)) {
      file = candidate;
    } else {
      diags.error(loc, "dependency '" + dep.name + "' path '" + written + "' was not found");
      return false;
    }
    ResolvedDependency item;
    item.name = dep.name;
    item.path = written;
    item.absolute = file.string();
    item.digest = sha256(readBytes(file));
    resolved.push_back(std::move(item));
  }

  const fs::path lockPath = base / "cruft.lock";
  if (update) {
    std::ofstream lockedOut(lockPath, std::ios::binary);
    if (!lockedOut) {
      diags.error(loc, "cannot write lockfile '" + lockPath.string() + "'");
      return false;
    }
    lockedOut << "[package]\nname = \"" << packageName << "\"\n\n[dependencies]\n";
    for (const ResolvedDependency &dep : resolved)
      lockedOut << dep.name << " = \"" << dep.path << "#" << dep.digest << "\"\n";
    out = std::move(resolved);
    return true;
  }

  std::error_code ec;
  if (!fs::is_regular_file(lockPath, ec)) {
    out = std::move(resolved);
    return true;
  }

  Manifest locked;
  if (!locked.loadFile(lockPath.string(), diags)) {
    diags.error(loc, "lockfile is not valid");
    return false;
  }
  std::vector<bool> used(locked.dependencies.size(), false);
  for (const ResolvedDependency &dep : resolved) {
    const Dependency *found = nullptr;
    for (size_t i = 0; i < locked.dependencies.size(); ++i) {
      if (locked.dependencies[i].name == dep.name) {
        found = &locked.dependencies[i];
        used[i] = true;
        break;
      }
    }
    if (!found) {
      diags.error(loc, "lockfile has no entry for '" + dep.name + "'");
      return false;
    }
    const std::string &value = found->path;
    auto hash = value.rfind('#');
    if (hash == std::string::npos || hash == 0 || hash + 1 >= value.size()) {
      diags.error(loc, "lockfile entry for '" + dep.name + "' is not a path and digest");
      return false;
    }
    std::string lockedPath = slashes(value.substr(0, hash));
    std::string lockedDigest = value.substr(hash + 1);
    if (lockedPath != dep.path) {
      diags.error(loc, "lockfile path for '" + dep.name + "' does not match the manifest");
      return false;
    }
    if (lockedDigest != dep.digest) {
      diags.error(loc, "lockfile digest for '" + dep.name + "' does not match '" + dep.path + "'");
      return false;
    }
  }
  for (size_t i = 0; i < locked.dependencies.size(); ++i) {
    if (!used[i]) {
      diags.error(loc, "lockfile lists '" + locked.dependencies[i].name +
                           "' which the manifest does not");
      return false;
    }
  }
  out = std::move(resolved);
  return true;
}

} // namespace cruft
