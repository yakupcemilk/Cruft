# Cruft

Cruft is a systems language for games and other programs that care about layout, linkage, and what the generated code actually calls. `cruftc` is a C++20 compiler that targets LLVM 16. It emits C-ABI LLVM IR, erases cubical proofs before code generation, and links a small runtime whose entry points are `cruft_alloc`, `cruft_arena_*`, and the `fs` / `io` hooks. Those symbols wrap libc. They do not define `malloc` or `fopen`.

> [!NOTE]
> Language syntax, ownership, arenas, and the standard library are in **[How to Cruft?](docs/how_to_cruft.md)**. This page is the map: what the compiler is, how to build it, and where each idea shows up in a chunk of voxels.

```cruft
package chunk

pub fn main(): i32
    var blocks = [1, 0, 1, 1]
    scope arena
        var scratch = [blocks.at(0), blocks.at(2)]
        print(scratch.sum())
    end
    print(blocks.length())
    ret 0
end
```

`blocks` is a heap `Vector(i32)`. `.at` checks the index and aborts through `cruft_bounds_fail` when it is out of range. `scratch` dies with the `scope arena`. `print` is the numeric builtin.

## What you get

| | |
|---|---|
| Compiler | `cruftc`. C++20. LLVM 16 or newer (`llvm-config` on `PATH`) |
| Calls | `math.add(1, 2)`. Blocks end with `end` |
| ABI | C calling convention. `extern fn` and `pub fn main` keep their source names |
| Stable export | `_C_<package>_<module>_<symbol>` when a public name would collide, and as an alias otherwise |
| Proofs | `Path`, `Equiv`, univalence, higher inductive types. Deleted before LLVM |
| Memory | Affine `Vector`, `Map`, and `String`. `shared var` for a shared buffer. `scope arena` for a bump region |
| Library | `std/mem.cruft`, `slice.cruft`, `string.cruft`, `vector.cruft`, `fs.cruft`, `io.cruft` |

## Build and test

CMake 3.20 or newer, a C++20 compiler, LLVM 16 or newer, and Python 3 for the suite.

Linux or macOS:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows, MinGW:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

If the LLVM C++ headers are not installed beside the libraries, extract `llvm-16.x.src/include` into `third_party/llvm-16.0.6.src/include`. CMake uses that tree when `llvm/IR/IRBuilder.h` is missing from the install.

```bash
build/cruftc --check examples/hello.cruft
build/cruftc -c -o hello.o examples/hello.cruft
build/cruftc --link --cc gcc -o hello examples/hello.cruft
```

`--link` walks parents of the source file and adds `src/runtime/arena.c`, `mem.c`, `fs.c`, and `io.c` when it finds them. Link a smaller set yourself when a program does not need every hook:

```bash
gcc hello.o src/runtime/mem.c src/runtime/arena.c -o hello
gcc app.o src/runtime/mem.c src/runtime/fs.c src/runtime/io.c -o app
```

| You use | Link |
|---|---|
| `print` of `i32`, `i64`, `f32`, `f64`, `bool` | libc `printf` |
| `Vector`, `String`, `alloc_string`, `.at` | `src/runtime/mem.c` |
| `scope arena` | `src/runtime/arena.c` |
| `import fs` | `src/runtime/fs.c` and `mem.c` |
| `import io` | `src/runtime/io.c` and `mem.c` |

| Flag | Effect |
|---|---|
| `-emit-llvm` | Textual LLVM IR. This is the default |
| `-c` / `-S` | Object file / assembly |
| `--check` | Stop after type checking |
| `-O0` … `-O3` | LLVM optimisation level |
| `--target <triple>` | Host triple, unless `cruft.toml` sets `[target] triple` |
| `--lib <dir>` | Extra search directory for `import` |
| `--link --cc <cc>` | Link an executable and the runtime C files |

A `cruft.toml` next to the first input may also set `[link] libraries`, `[link] search_paths`, and `[modules] paths`.

## Language in one page

Qualified names use a dot. `pri fn` is visible only inside its package and is emitted with LLVM internal linkage as `package__symbol`. `pub fn` is visible to importers. `extern fn` keeps the exact C name. `pub fn main` stays `main`.

```cruft
package app

import math

pub fn main(): i32
    print(math.add(20, 22))
    ret 0
end
```

Every other public symbol also has the spelling `_C_<package>_<module>_<symbol>`. The module is the file stem, or the identifier in `package app module world`. Characters that are not part of a C identifier become `_`.

> [!IMPORTANT]
> `Vector`, `Map`, and `String` (`str` is the same type) are affine. Passing one to a function moves it. A method such as `blocks.at(i)` or `label.length()` does not. `shared var` adds a reference count so two names can share one buffer. `i32`, `i64`, `f32`, `f64`, `bool`, `ptr`, and `Slice(T)` are copied.

`scope arena` saves a bump watermark and restores it on every exit, including `ret`. Values allocated in the region cannot leave it. A region that allocates nothing is deleted before code generation.

`Path`, `Equiv`, and higher-inductive path constructors are type-checked and then erased. `transport` along `refl` compiles to the transported value. The IR does not mention `Path`.

## Runtime

| Symbols | File | Role |
|---|---|---|
| `cruft_alloc`, `cruft_free`, `cruft_realloc`, `cruft_bounds_fail` | `mem.c` | Heap blocks and the `.at` abort. Addresses at the C boundary are `i64` |
| `cruft_arena_push`, `cruft_arena_pop`, `cruft_arena_alloc` | `arena.c` | `scope arena` |
| `cruft_fopen`, `cruft_fread`, `cruft_fwrite`, `cruft_fseek`, `cruft_ftell`, `cruft_fclose`, `cruft_exists` | `fs.c` | `import fs`. Offsets are `i64` (`_fseeki64` on Windows, `fseeko` elsewhere) |
| `cruft_stdout`, `cruft_stderr`, `cruft_stdin`, `cruft_io_write`, `cruft_read_line` | `io.c` | `import io` |

`import fs` loads `std/fs.cruft` (`package fs`). The same rule applies to `mem`, `slice`, `string`, `vector`, and `io`. The loader finds `std/` by walking parents until it sees `std/mem.cruft`.

## Hex chunks

A chunk is a dense `Vector(i32)` of block ids. A mesh that should die at the end of the frame is a `scope arena` value. A `Slice(i32)` is a `{ptr, i64}` view of those ids and does not own them.

```cruft
package chunk

import fs
import io

pub fn count_solid(blocks: Vector(i32)): i32
    var view: Slice(i32) = raw_slice(blocks.data().addr(), 4)
    var i: i64 = 0
    var filled: i32 = 0
    while i < 4 do
        if view.at(i) != 0 then
            filled = filled + 1
        end
        i = i + 1
    end
    ret filled
end

pub fn main(): i32
    var blocks = [1, 0, 1, 1]
    scope arena
        var face = [blocks.at(0), blocks.at(2)]
        print(face.sum())
    end
    print(count_solid(blocks))
    if fs.write_file("chunk.txt", "1,0,1,1") then
        io.println(fs.read_to_string("chunk.txt"))
    end
    ret 0
end
```

> [!WARNING]
> `count_solid(blocks)` moves `blocks`. The `.at` calls in `main` happen before that move. `face` is arena memory and cannot be returned. `fs.write_file` and `fs.read_to_string` each take their strings by move, so the calls above pass literals. `io.println` writes the file text. Builtin `print` is only for numbers and `bool`.

Step-by-step versions of `import`, `use`, `while`, arenas, `fs`, and `io` are in [How to Cruft?](docs/how_to_cruft.md).

## Repository

```
src/           lexer, parser, ast, sema, middle end, LLVM codegen, driver
src/runtime/   mem.c, arena.c, fs.c, io.c
std/           mem, slice, string, vector, fs, io
tests/         validation suite (ctest target cruft_validation)
examples/      hello.cruft
docs/          how_to_cruft.md
```

## Contact

ycemilkayabas@gmail.com
