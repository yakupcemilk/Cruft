#!/usr/bin/env python3
"""Validation suite for the Cruft compiler.

Exercises:
  * package-name module resolution (independent of file names)
  * pub / pri visibility across package boundaries
  * acyclic module hierarchy
  * extern fn C ABI (and by construction Rust extern "C") linkage
  * cubical / HoTT features with proof erasure in LLVM IR
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CASES = ROOT / "cases"
FFI_C = ROOT / "ffi" / "c_add.c"
ARENA_C = ROOT.parent / "src" / "runtime" / "arena.c"
MEM_C = ROOT.parent / "src" / "runtime" / "mem.c"
FS_C = ROOT.parent / "src" / "runtime" / "fs.c"
IO_C = ROOT.parent / "src" / "runtime" / "io.c"


class Fail(Exception):
    pass


def run(cmd, **kw):
    kw.setdefault("text", True)
    kw.setdefault("capture_output", True)
    return subprocess.run(cmd, **kw)


def expect_ok(cmd, label):
    r = run(cmd)
    if r.returncode != 0:
        raise Fail(f"{label}: command failed ({r.returncode})\n  cmd: {' '.join(cmd)}\n"
                   f"  stdout:\n{r.stdout}\n  stderr:\n{r.stderr}")
    return r


def expect_fail(cmd, needle, label):
    r = run(cmd)
    blob = (r.stdout or "") + (r.stderr or "")
    if r.returncode == 0:
        raise Fail(f"{label}: expected a compile error containing {needle!r}\n  stderr:\n{blob}")
    if needle not in blob:
        raise Fail(f"{label}: error output did not contain {needle!r}\n  stderr:\n{blob}")
    return r


def ir_has(path: Path, snippet: str, label: str):
    text = path.read_text(encoding="utf-8", errors="replace")
    if snippet not in text:
        raise Fail(f"{label}: expected {snippet!r} in {path.name}\n--- IR ---\n{text}")


def ir_lacks(path: Path, snippet: str, label: str):
    text = path.read_text(encoding="utf-8", errors="replace")
    if snippet in text:
        raise Fail(f"{label}: did not expect {snippet!r} in {path.name}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cruftc", required=True)
    ap.add_argument("--cc", required=True)
    ap.add_argument("--build-dir", required=True)
    args = ap.parse_args()

    cruftc = args.cruftc
    cc = args.cc
    out = Path(args.build_dir)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    passed = 0

    def ok(name):
        nonlocal passed
        passed += 1
        print(f"  PASS  {name}")

    print("Cruft validation suite")
    print(f"  cruftc = {cruftc}")
    print(f"  cc     = {cc}")

    # ------------------------------------------------------------------
    # 1. Single-package compile: IR uses C calling convention, private
    #    symbols are internal + package-prefixed, pub main is unmangled.
    # ------------------------------------------------------------------
    basic_ll = out / "basic.ll"
    expect_ok(
        [cruftc, "-emit-llvm", "-o", str(basic_ll), str(CASES / "basic.cruft")],
        "basic emit-llvm",
    )
    ir_has(basic_ll, "@main(", "basic: unmangled pub main")
    ir_has(basic_ll, "define internal", "basic: pri function is internal")
    ir_has(basic_ll, "@test__add_internal", "basic: pri name is package-prefixed")
    ir_lacks(basic_ll, "x86_stdcallcc", "basic: stdcall is not used (C ABI)")
    ir_lacks(basic_ll, "Path(", "basic: path types must be erased from IR")
    ok("basic package + pub/pri linkage")

    # ------------------------------------------------------------------
    # 2. Multi-package resolution by `package` name, not file name.
    # ------------------------------------------------------------------
    app_ll = out / "app.ll"
    expect_ok(
        [cruftc, "-emit-llvm", "-o", str(app_ll),
         str(CASES / "math.cruft"), str(CASES / "app.cruft")],
        "multi-package emit-llvm",
    )
    ir_has(app_ll, "@add(", "packages: pub math.add is unmangled")
    ir_has(app_ll, "@math__secret", "packages: pri math.secret is prefixed")
    ir_has(app_ll, "define internal", "packages: pri secret is internal")
    ok("multi-package resolution")

    expect_fail(
        [cruftc, "--check", str(CASES / "math.cruft"), str(CASES / "app_bad_priv.cruft")],
        "private to package",
        "pri access",
    )
    ok("pri symbol hidden from other packages")

    expect_fail(
        [cruftc, "--check", str(CASES / "not_imported.cruft"), str(CASES / "math.cruft")],
        "not imported",
        "missing import",
    )
    ok("qualified access requires import")

    expect_fail(
        [cruftc, "--check", str(CASES / "cycle_a.cruft"), str(CASES / "cycle_b.cruft")],
        "cyclic package dependency",
        "module cycle",
    )
    ok("acyclic module hierarchy")

    expect_fail(
        [cruftc, "--check", str(CASES / "leak_priv_type.cruft")],
        "exposes private type",
        "private type leak",
    )
    ok("pub signature cannot leak pri types")

    expect_fail(
        [cruftc, "--check", str(CASES / "coerce.cruft")],
        "no implicit coercions",
        "strict typing",
    )
    ok("no implicit numeric coercions")

    expect_fail(
        [cruftc, "--check", str(CASES / "extern_path.cruft")],
        "not C-ABI compatible",
        "extern Path",
    )
    ok("extern fn rejects non-C layouts")

    # ------------------------------------------------------------------
    # 3. FFI: compile Cruft to an object, link with a C (or Rust extern "C")
    #    implementation of c_add, run it.
    # ------------------------------------------------------------------
    ffi_obj = out / "ffi.o"
    c_obj = out / "c_add.o"
    ffi_exe = out / ("ffi.exe" if os.name == "nt" else "ffi")
    expect_ok(
        [cruftc, "-c", "-o", str(ffi_obj), str(CASES / "ffi.cruft")],
        "ffi object",
    )
    expect_ok([cc, "-c", str(FFI_C), "-o", str(c_obj)], "compile c_add.c")
    expect_ok([cc, str(ffi_obj), str(c_obj), "-o", str(ffi_exe)], "link ffi")
    r = expect_ok([str(ffi_exe)], "run ffi")
    if "81" not in (r.stdout or ""):
        raise Fail(f"ffi: expected stdout to contain 81, got {r.stdout!r}")
    ok("extern fn C ABI / Rust FFI bridge")

    ffi_ll = out / "ffi.ll"
    expect_ok(
        [cruftc, "-emit-llvm", "-o", str(ffi_ll), str(CASES / "ffi.cruft")],
        "ffi ir",
    )
    ir_has(ffi_ll, "declare i32 @c_add(i32", "ffi: unmangled C prototype")
    ir_lacks(ffi_ll, "x86_stdcallcc", "ffi: stdcall is not used (C ABI)")
    ok("extern declaration uses C CC and unmangled name")

    # ------------------------------------------------------------------
    # 4. Cubical / HoTT: type-check paths, univalence, HITs; erase proofs.
    # ------------------------------------------------------------------
    expect_ok([cruftc, "--check", str(CASES / "hott_path.cruft")], "hott path --check")
    path_ll = out / "hott_path.ll"
    expect_ok(
        [cruftc, "-emit-llvm", "-o", str(path_ll), str(CASES / "hott_path.cruft")],
        "hott path ir",
    )
    ir_lacks(path_ll, "Path(", "hott: Path types erased")
    ir_lacks(path_ll, "transport", "hott: transport along refl is identity")
    ok("path types + transport along refl (erased)")

    expect_ok([cruftc, "--check", str(CASES / "hott_univalence.cruft")], "univalence --check")
    ua_ll = out / "hott_univalence.ll"
    expect_ok(
        [cruftc, "-emit-llvm", "-o", str(ua_ll), str(CASES / "hott_univalence.cruft")],
        "univalence ir",
    )
    ir_lacks(ua_ll, "Equiv(", "univalence: Equiv erased")
    ir_has(ua_ll, "call", "univalence: transport along ua(iso) is a function call")
    ok("computational univalence (iso / ua / transport)")

    expect_ok([cruftc, "--check", str(CASES / "hit.cruft")], "HIT --check")
    hit_ll = out / "hit.ll"
    expect_ok(
        [cruftc, "-emit-llvm", "-o", str(hit_ll), str(CASES / "hit.cruft")],
        "HIT ir",
    )
    ir_lacks(hit_ll, "loop", "HIT: path constructor loop is erased")
    ok("higher inductive types (point + path constructors)")

    expect_fail(
        [cruftc, "--check", str(CASES / "hit_bad.cruft")],
        "does not respect the path constructor",
        "HIT coherence",
    )
    ok("HIT eliminator must respect path constructors")

    # ------------------------------------------------------------------
    # 5. Arenas, inferred lifetimes, dot calls, filter/map. No '::'.
    # ------------------------------------------------------------------
    expect_fail(
        [cruftc, "--check", str(CASES / "colon_colon.cruft")],
        "dot notation",
        ":: rejected",
    )
    ok("'::' is rejected in favour of dot notation")

    expect_fail(
        [cruftc, "--check", str(CASES / "arena_escape.cruft")],
        "does not live long enough",
        "arena escape",
    )
    ok("arena values cannot escape their scope")

    expect_fail(
        [cruftc, "--check", str(CASES / "use_after_move.cruft")],
        "use of moved value",
        "affine move",
    )
    ok("owned vectors are affine")

    def link_and_run(name, source, *needles):
        obj = out / f"{name}.o"
        exe = out / (f"{name}.exe" if os.name == "nt" else name)
        expect_ok([cruftc, "-c", "-o", str(obj), str(source)], f"{name} object")
        expect_ok([cc, str(obj), str(ARENA_C), str(MEM_C), "-o", str(exe)], f"link {name}")
        result = expect_ok([str(exe)], f"run {name}")
        text = result.stdout or ""
        for needle in needles:
            if needle not in text:
                raise Fail(f"{name}: expected stdout to contain {needle!r}, got {text!r}")
        return result

    link_and_run("ergonomics", CASES / "ergonomics.cruft", "60", "2", "12")
    ergo_ll = out / "ergonomics.ll"
    expect_ok(
        [cruftc, "-emit-llvm", "-o", str(ergo_ll), str(CASES / "ergonomics.cruft")],
        "ergonomics ir",
    )
    ir_has(ergo_ll, "call void @cruft_arena_push()", "arena: save bump watermark")
    ir_has(ergo_ll, "call void @cruft_arena_pop()", "arena: restore bump watermark")
    ir_has(ergo_ll, "call ptr @cruft_arena_alloc(", "arena: bump allocation")

    ret_ll = out / "arena_return.ll"
    expect_ok(
        [cruftc, "-emit-llvm", "-o", str(ret_ll), str(CASES / "arena_return.cruft")],
        "arena return ir",
    )
    ret_ir = ret_ll.read_text(encoding="utf-8", errors="replace")
    push_at = ret_ir.find("call void @cruft_arena_push()")
    pop_at = ret_ir.find("call void @cruft_arena_pop()")
    ret_at = ret_ir.find("ret i32")
    if not (0 <= push_at < pop_at < ret_at):
        raise Fail(
            "arena return: expected push, then pop, then ret on the early-exit path\n"
            f"--- IR ---\n{ret_ir}"
        )
    ir_has(ergo_ll, "define internal i32 @lambda.", "filter/map lambda")
    ir_lacks(ergo_ll, "Path(", "lifetimes erased")
    ir_lacks(ergo_ll, "::", "no colon-colon in IR")
    ok("scope arena + filter/map pipeline")

    link_and_run("arena_return", CASES / "arena_return.cruft", "60")
    ok("scope arena pop runs before ret")

    link_and_run("shared", CASES / "shared_vec.cruft", "6", "3")
    ok("shared vectors are reference counted")

    link_and_run("collections", CASES / "collections.cruft", "5", "2", "1")
    ok("String.length and Map.insert/contains")

    # ------------------------------------------------------------------
    # 6. Recovery, modules on disk, mangling, slices, manifest.
    # ------------------------------------------------------------------
    recover = expect_fail(
        [cruftc, "--check", str(CASES / "recover.cruft")],
        "unexpected character",
        "parser recovery",
    )
    blob = (recover.stdout or "") + (recover.stderr or "")
    if "^" not in blob or " | " not in blob:
        raise Fail(f"parser recovery: diagnostic is missing a source snippet\n{blob}")
    if "cascading" not in blob:
        raise Fail(f"parser recovery: expected cascade suppression\n{blob}")
    if "expected an expression" not in blob:
        raise Fail(f"parser recovery: did not resume after the first fault\n{blob}")
    ok("panic-mode recovery with snippets")

    dead_ll = out / "dead_arena.ll"
    expect_ok(
        [cruftc, "-emit-llvm", "-o", str(dead_ll), str(CASES / "dead_arena.cruft")],
        "dead arena ir",
    )
    ir_lacks(dead_ll, "cruft_arena_push", "dead arena is deleted")
    link_and_run("dead_arena", CASES / "dead_arena.cruft", "3")
    ok("empty scope arena is erased")

    link_and_run("slice_len", CASES / "slice_len.cruft", "4")
    ok("Slice(T) length")

    link_and_run("use_add", CASES / "use_add.cruft", "5")
    ok("use opens an imported symbol and loads the package")

    expect_fail(
        [cruftc, "--check", str(CASES / "export_priv.cruft")],
        "private to package",
        "export pri",
    )
    ok("export rejects pri symbols")

    link_and_run("modtree", CASES / "modtree" / "app.cruft", "7")
    ok("import resolves a sibling .cruft file")

    link_and_run("exmod", CASES / "exmod" / "app.cruft", "42")
    ok("export re-exports a public symbol")

    lib_obj = out / "libuser.o"
    lib_exe = out / ("libuser.exe" if os.name == "nt" else "libuser")
    expect_ok(
        [cruftc, "-c", "-o", str(lib_obj), "--lib", str(CASES / "libdep"),
         str(CASES / "libuser" / "app.cruft")],
        "lib search object",
    )
    expect_ok([cc, str(lib_obj), str(ARENA_C), "-o", str(lib_exe)], "link libuser")
    lib_run = expect_ok([str(lib_exe)], "run libuser")
    if "9" not in (lib_run.stdout or ""):
        raise Fail(f"libuser: expected 9, got {lib_run.stdout!r}")
    ok("--lib resolves a package outside the source directory")

    collide_ll = out / "collide.ll"
    expect_ok(
        [cruftc, "-emit-llvm", "-o", str(collide_ll), str(CASES / "collide" / "app.cruft")],
        "collide ir",
    )
    ir_has(collide_ll, "@_C_alpha_alpha_add(", "pub alpha.add is mangled")
    ir_has(collide_ll, "@_C_beta_beta_add(", "pub beta.add is mangled")
    ir_lacks(collide_ll, "define i32 @add(", "colliding pub names are not both unmangled")
    ir_has(collide_ll, "@main(", "entry point stays unmangled")
    collide_obj = out / "collide.o"
    collide_exe = out / ("collide.exe" if os.name == "nt" else "collide")
    expect_ok([cruftc, "-c", "-o", str(collide_obj), str(CASES / "collide" / "app.cruft")], "collide object")
    expect_ok([cc, str(collide_obj), str(ARENA_C), "-o", str(collide_exe)], "link collide")
    collide_run = expect_ok([str(collide_exe)], "run collide")
    collide_out = collide_run.stdout or ""
    if "5" not in collide_out or "6" not in collide_out:
        raise Fail(f"collide: expected 5 and 6, got {collide_out!r}")
    ok("colliding pub symbols use _C_ package module symbol names")

    std_obj = out / "usestd.o"
    std_exe = out / ("usestd.exe" if os.name == "nt" else "usestd")
    expect_ok([cruftc, "-c", "-o", str(std_obj), str(CASES / "usestd" / "app.cruft")], "std object")
    expect_ok([cc, str(std_obj), str(MEM_C), "-o", str(std_exe)], "link std")
    std_run = expect_ok([str(std_exe)], "run std")
    if "4" not in (std_run.stdout or ""):
        raise Fail(f"usestd: expected 4, got {std_run.stdout!r}")
    ok("std mem and slice packages")

    expect_fail(
        [cruftc, "-emit-llvm", "-o", str(out / "manifest.ll"), str(CASES / "manifest_proj" / "main.cruft")],
        "unknown target",
        "manifest triple",
    )
    ok("cruft.toml target triple is applied")

    # ------------------------------------------------------------------
    # 7. std fs / io, and bounds-checked slice indexing.
    # ------------------------------------------------------------------
    fs_obj = out / "std_fs.o"
    fs_exe = out / ("std_fs.exe" if os.name == "nt" else "std_fs")
    expect_ok([cruftc, "-c", "-o", str(fs_obj), str(CASES / "std_fs.cruft")], "std fs object")
    expect_ok(
        [cc, str(fs_obj), str(MEM_C), str(FS_C), str(IO_C), "-o", str(fs_exe)],
        "link std fs",
    )
    fs_run = expect_ok([str(fs_exe)], "run std fs")
    if "hello" not in (fs_run.stdout or ""):
        raise Fail(f"std fs: expected hello, got {fs_run.stdout!r}")
    leftover = Path.cwd() / "cruft_std_io.txt"
    if leftover.exists():
        leftover.unlink()
    ok("std fs read_to_string / write_file / exists and io.println")

    line_obj = out / "read_line.o"
    line_exe = out / ("read_line.exe" if os.name == "nt" else "read_line")
    expect_ok([cruftc, "-c", "-o", str(line_obj), str(CASES / "read_line.cruft")], "read_line object")
    expect_ok([cc, str(line_obj), str(MEM_C), str(IO_C), "-o", str(line_exe)], "link read_line")
    line_run = run([str(line_exe)], input="hello\n")
    if line_run.returncode != 0:
        raise Fail(f"read_line: command failed ({line_run.returncode})\n{line_run.stderr}")
    if "hello" not in (line_run.stdout or ""):
        raise Fail(f"read_line: expected hello, got {line_run.stdout!r}")
    ok("io.read_line strips the newline")

    link_and_run("slice_at", CASES / "slice_at.cruft", "5", "3", "4")
    ok("Slice.at and Vector.at bounds-checked indexing")

    print(f"\n{passed} tests passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as e:
        print(f"  FAIL  {e}", file=sys.stderr)
        sys.exit(1)
