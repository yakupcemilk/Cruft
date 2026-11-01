#include "Target.h"

#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

namespace cruft {

TargetFacts TargetFacts::from(std::string triple) {
  if (triple.empty())
    triple = llvm::sys::getDefaultTargetTriple();
  // LLVM's legacy `arch-w64-mingw32` spelling is parsed as ELF in some 16.x
  // builds. Canonicalise it to the Windows-GNU form so we emit PE/COFF.
  // This is the same rewrite CodeGen used before `cfg` existed.
  if (triple.find("mingw32") != std::string::npos ||
      (llvm::Triple(triple).isOSWindows() &&
       llvm::Triple(triple).getObjectFormat() == llvm::Triple::ELF)) {
    llvm::Triple t(llvm::Triple::normalize(triple));
    t.setOS(llvm::Triple::Win32);
    t.setEnvironment(llvm::Triple::GNU);
    t.setObjectFormat(llvm::Triple::COFF);
    triple = t.str();
  }
  llvm::Triple parsed(triple);
  TargetFacts facts;
  facts.triple = std::move(triple);
  facts.arch = parsed.getArchName().str();
  facts.os = parsed.getOSName().str();
  facts.env = parsed.getEnvironmentName().str();
  facts.pointerBits = parsed.isArch64Bit() ? 64u : 32u;
  return facts;
}

} // namespace cruft
