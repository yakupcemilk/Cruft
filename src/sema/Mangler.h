// Mangler.h - deterministic C ABI names for exported Cruft entities.
//
// The object symbol is `_C_<package>_<module>_<symbol>`. Characters that are
// not part of a C identifier are rewritten to '_'. `extern` declarations and
// the process entry point `main` keep the spelling the foreign language
// expects; every other `pub` symbol uses this name when the unmangled
// spelling would collide, and otherwise also exposes it as an alias.
#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace cruft {

inline std::string mangleComponent(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (unsigned char c : text) {
    if (std::isalnum(c) || c == '_')
      out.push_back(static_cast<char>(c));
    else
      out.push_back('_');
  }
  if (out.empty())
    out = "_";
  return out;
}

[[nodiscard]] inline std::string mangleAbi(std::string_view package, std::string_view module,
                                           std::string_view symbol) {
  return "_C_" + mangleComponent(package) + "_" + mangleComponent(module) + "_" +
         mangleComponent(symbol);
}

} // namespace cruft
