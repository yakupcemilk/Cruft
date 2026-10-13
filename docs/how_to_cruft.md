# How to Cruft?

Quick reference for the language `cruftc` parses today. Packages are one identifier, calls use a dot (`math.add`), and every block ends with `end`.

| Need | Rule |
|---|---|
| 64-bit integer | `i64`. The other integer is `i32`. Both are signed |
| Text type | `str` and `String` are the same type |
| `Vector(T).length()` | `i32` |
| `Slice(T).length()` and `.at(i)` | `i64` |
| Numbers on stdout | builtin `print` |
| Text on stdout | `io.print` / `io.println` |

**Contents**

1. [Packages, imports, and visibility](#packages-imports-and-visibility)
2. [Variables, primitives, and core types](#variables-primitives-and-core-types)
3. [Control flow](#control-flow)
4. [Scope arenas](#scope-arenas)
5. [Standard library](#standard-library)
6. [A chunk, end to end](#a-chunk-end-to-end)
7. [Compile commands](#compile-commands)

## Packages, imports, and visibility

A file starts with `package`. That name is the package. The file name is not. `module` sets the middle piece of `_C_<package>_<module>_<symbol>`. Omit it and the module is the file stem.

```cruft
package math

pub fn add(a: i32, b: i32): i32
    ret a + b
end

pri fn secret(): i32
    ret 7
end

pub const answer: i32 = 42
```

| Declaration | Who can name it | Object file |
|---|---|---|
| `pub fn` | This package, and any package that imported it | External. Source name when that name is unique. Always also `_C_<pkg>_<mod>_<sym>` |
| `pri fn` | This package only | LLVM internal linkage. Symbol `package__name` |
| `extern fn` | This package, or every package if you write `pub extern fn` | External. Exact source name. C calling convention |
| `pub fn main` | The process | External. Unmangled `main` |

`import` loads a package. Its public names stay qualified: `math.add(2, 3)`.

```cruft
package app

import math

pub fn main(): i32
    print(math.add(2, 3))
    ret 0
end
```

`use package.symbol` imports that package and opens one public symbol in this file. `use vector.at` opens `at` from `std/vector.cruft`. `use math.add` opens `add` from a `math` package you can see (a sibling `math.cruft`, a file on the `cruftc` command line, or `--lib`).

```cruft
package app

use math.add
use vector.at

pub fn main(): i32
    var blocks = [4, 5, 6]
    print(add(2, 3))
    print(at(blocks, 0))
    ret 0
end
```

> [!WARNING]
> `at(blocks, 0)` is the function in `std/vector.cruft`. Its first parameter owns the vector, so `blocks` is moved. `blocks.at(0)` is the method and does not move `blocks`.

`export package.symbol` re-exports one public name. Importers of `wrap` call `wrap.add`. They do not import `math`.

```cruft
package wrap

import math

export math.add
```

```cruft
package app

import wrap

pub fn main(): i32
    print(wrap.add(20, 22))
    ret 0
end
```

Search order, in the importing file's directory, each `--lib` directory, and `std/` after the loader has seen `std/mem.cruft` by walking parents:

1. `<dir>/<package>.cruft`
2. `<dir>/<package>/mod.cruft`
3. `<dir>/<package>/<package>.cruft`

`import fs` loads `std/fs.cruft`, whose first line is `package fs`. The same pattern covers `mem`, `slice`, `string`, `vector`, and `io`.

> [!NOTE]
> The loader looks for `<package>.cruft` in the same directory as the file that wrote `import`. A file named `mem.cruft` whose package is `app` will be found when that file imports `mem`, and the package name will not match.

## Variables, primitives, and core types

`var` is mutable. `const` is immutable and its initialiser is a compile-time value. Write the type after `:` when it cannot be inferred. An empty `[]` needs a type on the binding (`var m: Map(i32, i32) = []`).

```cruft
package demo

const y: i32 = 42

pub fn main(): i32
    var x: i32 = 39
    var label: str = "cruft"
    var flag: bool = true
    var nothing: ptr = null()
    x = x + y
    print(x)
    print(label.length())
    print(flag)
    ret 0
end
```

> [!NOTE]
> A package-level initialiser is an integer, float, or `bool` constant. String literals and `null()` are locals, as in `main` above.

| Type | Meaning |
|---|---|
| `i32`, `i64` | Signed integers. A literal is `i64` when the expected type is `i64`, and `i32` otherwise |
| `f32`, `f64` | IEEE floats. An integer literal in a float position is that float |
| `bool` | `true` or `false` |
| `ptr` | Untyped C pointer. `null()` is the null pointer. `p.addr()` is that address as `i64` |
| `str`, `String` | One owned byte buffer. `.length()` is `i32`. `.byte_len()` is `i64`. `.data()` is a NUL-terminated `ptr` for as long as the value is alive. `.set_len(n)` updates a named variable |
| `Slice(T)` | `{ptr, i64}`. Does not own the bytes. `.length()` is the element count, an `i64` |
| `Vector(T)` | Owned heap buffer. `.length()` is `i32`. Methods: `.data()`, `.at(i)`, `.sum()`, `.filter`, `.map` |
| `Map(K, V)` | Owned map. `.insert(key, value)` updates a named variable. `.contains(key)` returns `bool`. `.length()` is `i32` |

> [!IMPORTANT]
> `Vector`, `Map`, and `String` are affine. A function argument moves the value. Using it again is an error. Methods do not move the receiver. `shared var` stores a reference count so two names can share one buffer. The last drop frees it through `cruft_free`. `i32`, `i64`, `f32`, `f64`, `bool`, `ptr`, and `Slice(T)` are copied.

```cruft
package share

pub fn main(): i32
    shared var a = [1, 2, 3]
    shared var b = a
    print(a.sum())
    print(b.length())
    ret 0
end
```

`raw_slice(addr, n)` builds a `Slice(T)`. `n` is an element count. The expected type has to be written on the binding or the return, because the address is only an `i64`.

```cruft
package demo

pub fn main(): i32
    var xs: Vector(i32) = [4, 5, 6]
    var s: Slice(i32) = raw_slice(xs.data().addr(), 3)
    print(s.length())
    print(s.at(1))
    ret 0
end
```

`std/slice.cruft` wraps that for `Slice(i32)`. Sixteen bytes is four `i32`s. `slice.len` returns `i64`. `Slice` is copied, so `s` is still available after `slice.len(s)`.

```cruft
package app

import slice
import mem

pub fn main(): i32
    var p: i64 = mem.allocate(16)
    var s: Slice(i32) = slice.from_raw(p, 4)
    print(slice.len(s))
    mem.release(p)
    ret 0
end
```

`alloc_string(n)` allocates `n + 1` bytes with `cruft_alloc`, writes a trailing NUL, and returns an owned `String`. Dropping the owner calls `cruft_free`. A string literal points at static bytes. Its capacity is negative, and that storage is not freed.

```cruft
package demo

pub fn main(): i32
    var buf: String = alloc_string(4)
    buf.set_len(0)
    print(buf.byte_len())
    ret 0
end
```

## Control flow

`if` takes a `bool`, uses `then`, and ends with `end`. `else` is optional. `and`, `or`, and `not` combine booleans. `ret` leaves the function. The result type must match. `i32` and `i64` do not convert into each other.

```cruft
package demo

pub fn classify(n: i32): i32
    if n < 0 then
        ret -1
    else
        if n == 0 then
            ret 0
        end
    end
    ret 1
end

pub fn main(): i32
    var n: i32 = 2
    if n > 0 and not false then
        print(classify(n))
    end
    ret 0
end
```

`while` uses `do` and ends with `end`. `.at` takes an `i64`. On a `Vector`, `.length()` is an `i32`, so this loop uses a literal bound of `4`, which is `i64` because `i` is `i64`. The vector has four elements, so every index is in range.

```cruft
package demo

pub fn main(): i32
    var blocks = [1, 0, 1, 1]
    var i: i64 = 0
    var filled: i32 = 0
    while i < 4 do
        if blocks.at(i) != 0 then
            filled = filled + 1
        end
        i = i + 1
    end
    print(filled)
    ret 0
end
```

> [!WARNING]
> `.at(i)` requires `0 <= i < length`. A failing check calls `cruft_bounds_fail`: it writes `cruft: index out of bounds` to stderr and aborts. Link `src/runtime/mem.c` or the call is undefined.

`Slice.length` is `i64`, so the bound can be the length itself:

```cruft
package demo

pub fn main(): i32
    var xs = [4, 5, 6]
    var s: Slice(i32) = raw_slice(xs.data().addr(), 3)
    var i: i64 = 0
    while i < s.length() do
        print(s.at(i))
        i = i + 1
    end
    ret 0
end
```

`.filter` and `.map` take a lambda of one argument. The lambda cannot capture a local. The chain is a dot.

```cruft
package demo

pub fn main(): i32
    var numbers = [1, 2, 3, 4, 5]
    var evens = numbers.filter(fn(x) => x % 2 == 0).map(fn(x) => x * 2)
    print(evens.length())
    print(evens.sum())
    ret 0
end
```

`.filter` and `.map` do not move `numbers`. Each call returns a new owned vector. `evens.length()` and `evens.sum()` leave `evens` in place.

## Scope arenas

`scope arena` pushes a bump watermark and pops it when the block ends, including on `ret`. A `Vector` or `String` created inside the block cannot be returned or stored in a variable from an outer scope. Link `src/runtime/arena.c`. A region that never allocates is deleted, and the object file does not call `cruft_arena_push`.

```cruft
package demo

pub fn main(): i32
    var numbers = [1, 2, 3, 4, 5]
    var evens = numbers.filter(fn(x) => x % 2 == 0)
    scope arena
        var temp = [10, 20, 30]
        print(temp.sum())
    end
    print(evens.sum())
    ret 0
end
```

> [!WARNING]
> `temp` ends with the arena. `evens` was allocated before the arena, on the heap, so `print(evens.sum())` after `end` is valid. Returning `temp` from `main` is rejected.

## Standard library

### Files

`import fs` reads `std/fs.cruft`. Link `src/runtime/fs.c` and `src/runtime/mem.c`.

| Call | Result |
|---|---|
| `fs.write_file(path: str, content: str): bool` | Creates or replaces the file in binary mode. `true` only when every byte is written and `fclose` returns 0 |
| `fs.read_to_string(path: str): String` | The whole file, binary mode, as an owned `String`. `""` when open, seek, or read fails |
| `fs.exists(path: str): bool` | `true` when the path is visible (`_access` / `access`) |

Under those functions the runtime calls `fopen`, `fread`, `fwrite`, `fseek` / `ftell` (64-bit offset: `_fseeki64` or `fseeko`), and `fclose`, through `cruft_fopen`, `cruft_fread`, `cruft_fwrite`, `cruft_fseek`, `cruft_ftell`, `cruft_fclose`, and `cruft_exists`.

> [!IMPORTANT]
> Each string argument is moved. The sample passes a new literal to every call. A `var path: str` can be passed once.

```cruft
package app

import fs
import io

pub fn main(): i32
    if not fs.write_file("note.txt", "hello") then
        ret 1
    end
    if not fs.exists("note.txt") then
        ret 2
    end
    io.println(fs.read_to_string("note.txt"))
    ret 0
end
```

`io.println` receives the `String` returned by `read_to_string` and writes `hello` plus a newline.

### Streams

`import io` reads `std/io.cruft`. Link `src/runtime/io.c` and `src/runtime/mem.c`.

Builtin `print` accepts `i32`, `i64`, `f32`, `f64`, and `bool`, and writes a newline. It does not accept `str`.

| Call | Result |
|---|---|
| `io.print(msg: str)` | The bytes of `msg` on stdout. No added newline |
| `io.println(msg: str)` | `io.print(msg)`, then `"\n"` |
| `io.eprint(msg: str)` | The bytes of `msg` on stderr |
| `io.read_line(): String` | One stdin line, at most 4096 bytes, with the trailing newline removed. `""` on EOF or a failed read |
| `io.stdout()`, `io.stderr()`, `io.stdin()` | The C stream as `ptr` |

```cruft
package app

import io

pub fn main(): i32
    io.print("count=")
    print(3)
    var line: str = io.read_line()
    io.println(line)
    ret 0
end
```

> [!NOTE]
> `io.println(line)` moves `line`. `print(3)` is the builtin. Importing `io` does not hide it. `read_line` allocates with `alloc_string`, so the program needs `mem.c` as well as `io.c`.

### Heap

`import mem` reads `std/mem.cruft`. `allocate` / `release` / `resize` are `cruft_alloc` / `cruft_free` / `cruft_realloc`. A zero size passed to `resize` frees the block and returns `0`. A failed `allocate` returns `0`.

```cruft
package app

import mem

pub fn main(): i32
    var p: i64 = mem.allocate(16)
    if p == 0 then
        ret 1
    end
    mem.release(p)
    ret 0
end
```

## A chunk, end to end

`count_solid` keeps the vector local, so `main` does not move a buffer into it. The arena scratch is printed and dropped at `end`. The file round-trip uses literals, then `io.println` prints the text that was written.

```cruft
package chunk

import fs
import io

pub fn count_solid(): i32
    var blocks = [1, 0, 1, 1]
    var i: i64 = 0
    var filled: i32 = 0
    while i < 4 do
        if blocks.at(i) != 0 then
            filled = filled + 1
        end
        i = i + 1
    end
    ret filled
end

pub fn main(): i32
    scope arena
        var scratch = [1, 0, 1]
        print(scratch.sum())
    end
    print(count_solid())
    if fs.write_file("chunk.txt", "solid") then
        io.println(fs.read_to_string("chunk.txt"))
    end
    ret 0
end
```

```bash
cruftc --link --cc gcc -o chunk chunk.cruft
```

`--link` adds `arena.c`, `mem.c`, `fs.c`, and `io.c` from `src/runtime/` when those files are found above the source.

## Compile commands

```bash
cruftc --check main.cruft
cruftc -emit-llvm -o main.ll main.cruft
cruftc -c -o main.o main.cruft
cruftc -S -o main.s main.cruft
cruftc --link --cc gcc -o main main.cruft
cruftc --lib ./lib -c -o app.o app.cruft
```

`-O0` through `-O3` select the LLVM pipeline. `--target <triple>` overrides the host triple, or the `triple` under `[target]` in a `cruft.toml` beside the first input. Build and test instructions for the compiler itself are in the [repository README](../README.md).
