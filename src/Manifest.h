// Manifest.h - the cruft.toml package manifest.
//
// Supported sections:
//   [package]  name, version
//   [target]   triple
//   [link]     libraries, search_paths
//   [modules]  paths
//   [dependencies]
//       <package> = "<path or name>"
// The parser accepts a deliberate subset: comments, quoted strings, and
// arrays of quoted strings. Unknown keys are ignored.
#pragma once

#include "Diagnostics.h"

#include <string>
#include <utility>
#include <vector>

namespace cruft {

struct Manifest {
  std::string packageName;
  std::string version;
  std::string targetTriple;
  std::vector<std::string> libraries;
  std::vector<std::string> libraryPaths;
  std::vector<std::string> modulePaths;
  std::vector<std::string> dependencies;

  /// Empty string when no manifest sits beside `sourceFile`.
  [[nodiscard]] static std::string discover(const std::string &sourceFile);

  bool loadFile(const std::string &path, DiagnosticEngine &diags);
};

} // namespace cruft
