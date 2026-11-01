// Manifest.h - the cruft.toml package manifest.
//
// Supported sections:
//   [package]  name, version
//   [target]   triple
//   [link]     libraries, search_paths
//   [modules]  paths
//   [dependencies]
//       <package> = "<path>"
// The parser accepts a deliberate subset: comments, quoted strings, and
// arrays of quoted strings. Unknown keys are ignored.
//
// `cruft.lock`, beside the manifest, pins each dependency path and the
// SHA-256 of the file it resolves to. `--lock` rewrites it. A compile that
// finds a lockfile rejects a path or digest that drifted.
#pragma once

#include "Diagnostics.h"

#include <string>
#include <utility>
#include <vector>

namespace cruft {

struct Dependency {
  std::string name;
  std::string path;
};

/// One manifest dependency after the path has been resolved to a file.
struct ResolvedDependency {
  std::string name;
  std::string path;     ///< path recorded in the lockfile
  std::string absolute; ///< file that will be compiled
  std::string digest;   ///< lowercase SHA-256 of that file
};

struct Manifest {
  std::string packageName;
  std::string version;
  std::string targetTriple;
  std::vector<std::string> libraries;
  std::vector<std::string> libraryPaths;
  std::vector<std::string> modulePaths;
  std::vector<Dependency> dependencies;

  /// Empty string when no manifest sits beside `sourceFile`.
  [[nodiscard]] static std::string discover(const std::string &sourceFile);

  bool loadFile(const std::string &path, DiagnosticEngine &diags);

  /// Resolve `[dependencies]` against `manifestFile`'s directory.
  /// When `update` is set, rewrite `cruft.lock`. When a lockfile is already
  /// there and `update` is clear, its paths and digests must match.
  bool lockDependencies(const std::string &manifestFile, DiagnosticEngine &diags, bool update,
                        std::vector<ResolvedDependency> &out) const;
};

} // namespace cruft
