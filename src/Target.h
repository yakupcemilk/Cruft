// Target.h - the LLVM triple the driver is compiling for.
//
// `cfg` and code generation must agree. `TargetFacts::from` is the one place
// that turns `--target`, `cruft.toml`, or the host default into the triple
// actually emitted.
#pragma once

#include <string>

namespace cruft {

struct TargetFacts {
  std::string triple;
  std::string arch; ///< `x86_64`, `aarch64`, ...
  std::string os;   ///< `windows`, `linux`, `macos`, ...
  std::string env;  ///< `gnu`, `msvc`, `musl`, ... empty when the triple has none
  unsigned pointerBits = 64; ///< width of `usize` / `isize` and of `ptr`

  /// Empty `triple` means the host. Applies the same MinGW canonicalisation
  /// as object emission.
  static TargetFacts from(std::string triple);
};

} // namespace cruft
