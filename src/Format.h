// Format.h - deterministic source formatter.
//
// `cruftc --fmt` reprints a file with a four-space indent derived from
// `end`-terminated blocks. It is a tooling pass: it does not change tokens
// that already sit on a line, and it is not a language feature.
#pragma once

#include <string>

namespace cruft {

[[nodiscard]] std::string formatSource(const std::string &source);

} // namespace cruft
