# How to Cruft?

A tutorial for the language `cruftc` compiles today. The rules, the diagnostics, and the standard library are specified in [cruft_language_reference.md](cruft_language_reference.md). This guide is the path through them: a first program, then ownership, then the constructs you will write every day.

Cruft is a systems language. It lowers to C-ABI LLVM IR, so a `pub fn` can be called from C and a C function can be declared with `extern fn`. It also checks things C does not: an affine buffer cannot be used twice, a `scope arena` value cannot escape, and an `&mut` cannot overlap another borrow. The cubical proofs (`Path`, `Equiv`, univalence) are checked and then deleted. They never appear in the object file.

Blocks end with `end`. `{` and `}` are not braces. Calls use a dot: `math.add(1, 2)`.

## Contents

1. [Why this language](#why-this-language)
2. [A first program](#a-first-program)
3. [Packages and types](#packages-and-types)
4. [Ownership, borrows, and arenas](#ownership-borrows-and-arenas)
5. [Structs, match, and `?`](#structs-match-and-)
6. [When to write `unsafe`](#when-to-write-unsafe)
7. [A small program, compiled](#a-small-program-compiled)
8. [Where to go next](#where-to-go-next)

## Why this language

Three ideas sit on top of an ordinary LLVM backend.

**Affine buffers.** `Vector`, `Map`, and `String` have one owner. Passing one to a function moves it. The compiler rejects a use after that move. A `shared var` is the explicit exception: a reference count, updated with atomics, so two names can share one buffer.

**Lexical arenas.** `scope arena` is a bump region. Everything allocated inside it dies at `end`, including on `ret`. A value that tries to leave is a type error, not a use-after-free you find later.

**Proof erasure.** `Path` and `Equiv` are real types. The checker understands `refl` and `transport`. The code generator then throws the proof away. `transport` along `refl` compiles to the value you transported. You do not pay for a proof you have already checked.

What you do not get, on purpose: a garbage collector, exceptions, inheritance, and virtual calls. `?` returns a `Result` or an `Option`. It does not unwind.

## A first program

Save this as `addition.cruft`.

```cruft
package addition

pub fn main(): i32
    var n: i32 = 40
    print(n + 2)
    ret 0
end
```

`package` names the package. The file name does not. `pub fn main(): i32` is one of the two legal entry points. The other is `pub fn main(argc: i32, argv: ptr): i32`. `print` is a builtin. It writes an integer, a float, or a `bool`, and a newline. It does not print strings.

Check it, then emit an object file, then link:

```bash
cruftc --check addition.cruft
cruftc -c -o addition.o addition.cruft
cruftc --link --cc gcc -o addition addition.cruft
```

`--link` finds `src/runtime/` by walking parents of the source file and adds the C runtime it needs. `print` of a number only needs libc `printf`. A `Vector` also needs `src/runtime/mem.c`. A `scope arena` also needs `src/runtime/arena.c`.

`-O0` through `-O3` select the LLVM pipeline. `-emit-llvm -o addition.ll` writes the IR, which is the default when you name an output and do not pass `-c` or `-S`. `--target <triple>` cross-compiles. A `cruft.toml` beside the file can set the same triple.

## Packages and types

### Another file

`math.cruft`:

```cruft
package math

pub fn add(a: i32, b: i32): i32
    ret a + b
end

pri fn hidden(): i32
    ret 7
end
```

`app.cruft`:

```cruft
package app

import math

pub fn main(): i32
    print(math.add(20, 22))
    ret 0
end
```

`import math` loads a package whose first line is `package math`. Public names stay qualified. `pri fn` is visible only in `math`, and it is emitted as an internal symbol `math__hidden`. `::` is a syntax error. The spelling is a dot.

`use math.add` imports `math` and lets this file call `add` with no prefix. `export math.add` from a third package re-exports that one name, so importers of the third package do not import `math` themselves.

The loader looks for `math.cruft`, then `math/mod.cruft`, then `math/math.cruft`, in the importing file's directory, on `--lib`, and in `std/` once it has seen `std/mem.cruft`.

### Numbers

There is no implicit conversion. An `i32` is not an `i64`. A cast is `as`.

```cruft
var wide: i64 = n as i64
var flag: bool = n > 0 and not false
```

Integer literals are `i32` unless the expected type says otherwise. `var width: usize = 8` is a `usize`. `usize` and `isize` are the pointer width of the target: 64 bits on `x86_64`, 32 bits otherwise.

`and`, `or`, and `not` are boolean. `|`, `^`, and `&` between expressions are bitwise, and they bind *less* tightly than `==`. `<<` binds *less* tightly than `+`, so `1 << 2 + 3` is `1 << 5`. Parenthesise when you want the C reading. `~x` is bitwise complement on an integer.

`f32` and `f64` are IEEE floats. `-1 as i64` negates first and then casts, because `as` sits just outside a unary operator.

### Strings and vectors

`str` and `String` are one type: an owned byte buffer. A literal points at static storage and is not freed.

```cruft
var label: str = "cruft"
print(label.length())
```

`.length()` is `i64`. Builtin `print` still will not accept the string. `import io` and call `io.println(label)` for bytes. That call moves `label`.

A vector literal allocates a heap `Vector`. `.at` checks the index and aborts through `cruft_bounds_fail` when it is out of range. Link `mem.c`.

```cruft
var blocks = [1, 0, 1, 1]
var i: i64 = 0
var filled: i32 = 0
while i < blocks.length() do
    if blocks.at(i) != 0 then
        filled = filled + 1
    end
    i = i + 1
end
print(filled)
```

`.at` takes an `i64`. `.length()` is an `i64`. The `i32` element and the `i64` index are different types on purpose.

`for i in 0 .. 4` is the half-open numeric loop. `break` and `continue` apply to `while` and `for`.

## Ownership, borrows, and arenas

### A move is a use

`Vector`, `Map`, and `String` are affine. This function takes the vector. After the call, `blocks` is gone.

```cruft
pub fn sum_of(xs: Vector(i32)): i32
    ret xs.sum()
end
```

Methods do not move the receiver. `blocks.sum()` and `blocks.at(i)` leave `blocks` in place. The free function in `std/vector.cruft` is different: `at(blocks, 0)` takes the vector by value and moves it. Prefer the method.

A second use of a moved value is an error. So is using a linear (`lin`) binding twice, or forgetting it on one branch of an `if`. A loop cannot consume a linear binding, because the body might run again.

Scalars, `bool`, `ptr`, and `Slice(T)` are copied. Copying a slice copies the pointer and the length. It does not copy the elements, and it does not keep them alive.

### `shared var`

When two names must see one buffer, say so.

```cruft
shared var a = [1, 2, 3]
shared var b = a
print(a.sum())
print(b.length())
```

The copy increments an atomic reference count. The last owner, the drop that sees the old count `1`, frees the buffer. This is the handle you pass to `thread.spawn`. An ordinary affine vector cannot be sent to another thread.

### Exclusive borrows

`&xs` is a shared borrow. `&mut xs` is exclusive, and `xs` must be a `var`. Several shared borrows may be live together. A mutable borrow may not overlap any other borrow of that place.

```cruft
fn bump(xs: &mut Vector(i32))
    xs.push(1)
end

var xs: Vector(i32) = [10, 20, 30]
var a = &xs
var b = &xs
print(a.length())
print(b.at(0))
bump(&mut xs)
```

The shared borrows end at the end of their scope, so the later `&mut` is legal. Two `&mut` of `xs` at the same time is `overlapping mutable borrow`. Mutating `xs` while `a` is still live is `cannot mutate 'xs' while it is borrowed`. Using `xs` while a mutable borrow is live is `cannot use 'xs' while it is mutably borrowed`.

A call parameter of type `&T` or `&mut T` auto-borrows. `bump` can be called as `bump(&mut xs)`. Returning `a` from `main` is `cannot return a value that borrows 'xs'`.

`&mut` does not make a parameter mutable. `&mut` needs a `var`.

### Arenas

Heap vectors live until they are moved into a function that drops them, or until their owner goes out of scope. Scratch that should die at the end of a block belongs in an arena.

```cruft
var numbers = [1, 2, 3, 4, 5]
scope arena
    var temp = [10, 20, 30]
    print(temp.sum())
end
print(numbers.sum())
```

`temp` is bump-allocated. At `end`, and also if you `ret` from inside the block, the compiler pops the watermark. Returning `temp` is a type error. `numbers` was allocated before the arena, so it is still there.

If the block never allocates, the compiler deletes the push and the pop before code generation.

### Slices borrow the vector

`xs.slice()` is a `Slice` of the vector's elements. It does not own them. `.at` on the slice still checks the index. Holding that slice and then `push`ing into `xs` is rejected, because `push` may reallocate and leave the slice dangling.

`for n in iter.range(1, 4)` is the other loop. `range` is a struct `{cur, stop}` — the field is `stop` because `end` is a keyword — and `for` calls `next` until `None`. The standard `Iterator` trait is how you write your own.

## Structs, match, and `?`

### A struct and a trait

```cruft
struct Pair
    a: i32
    b: i32
end

trait Length
    fn len(self: &Self): i64
end

impl Length for Pair
    fn len(self: &Pair): i64
        ret 2
    end
end
```

`Pair(4, 5)` builds a value. `p.a` reads a field. `p.len()` and `len(&p)` are the same monomorphised function. There is no vtable. If you do not impl the method, the call is rejected. A builtin such as `.length()` wins over a trait method of the same name.

One `drop Pair` is allowed. It runs before the fields are freed. It is a Cruft function, not a C destructor.

### Enums are inductives

```cruft
enum Color
    Red
    Blue(n: i32)
end

var n = match c
    case Red then 0
    case Blue(k) then k
end
```

Every constructor appears, or the match is rejected. An arm is an expression. You cannot put `ret` in the middle of `then`. Bind the result, then `if` on it.

`import option` gives `Option(T)` with `None` and `Some`. `import result` gives `Result(T, E)` with `Ok` and `Err`. They are ordinary inductives. Tag 0 is `None` and tag 1 is `Some`. Tag 0 is `Ok` and tag 1 is `Err`.

### `?` returns

`?` is a branch. In a function that returns `Result(R, E)`, `expr?` either unwraps `Ok` or returns the `Err`. The error type must match.

```cruft
import result

fn parse(n: i32): result.Result(i32, i32)
    if n < 0 then
        ret result.Err(7)
    end
    ret result.Ok(n)
end

fn go(n: i32): result.Result(i32, i32)
    var v = parse(n)?
    ret result.Ok(v + 1)
end
```

Nothing unwinds. Locals are dropped on that `ret` the same way they are dropped on a `ret` you wrote yourself. `Option` works the same way: `?` yields the `Some` payload or returns `None`.

### Files, as a user of `Result`

`fs.write_file` and `fs.read_to_string` return `Result`. They do not return `bool`. `fs.exists` does return `bool`, because the C check reports presence, not an error code. `File` owns the `FILE*`. When the `File` drops, it closes the handle. You do not call `fclose` yourself unless you are writing that `drop`.

```cruft
import fs
import io

var wrote = fs.write_file("note.txt", "hello")
var failed = match wrote
    case Ok(n) then false
    case Err(code) then true
end
if failed then
    ret 1
end
var body = fs.read_to_string("note.txt")
var text = match body
    case Ok(s) then s
    case Err(code) then ""
end
io.println(text)
```

Each string argument is moved. The calls above pass literals, so there is nothing left to use again. `io.println` writes the bytes plus a newline. Link `fs.c`, `io.c`, and `mem.c`, or use `--link`.

`io.read_line()` is `Result(String, i32)`. `Ok("")` is a blank line. `Err(1)` is EOF.

## When to write `unsafe`

`unsafe` is a block, and it ends with `end`. It does not turn off the move checker, the borrow checker, or the arena check. It permits operations whose safety the compiler cannot see.

Write it at the boundary, and keep the safe wrapper outside.

### Pointers

`ptr` is an untyped C pointer. `null()` is null. Reading the address is `unsafe`:

```cruft
unsafe
    var addr: i64 = xs.data().addr()
end
```

Casting `ptr` to `usize`, `isize`, or `i64`, and the reverse, is the same kind of operation.

`raw_slice(addr, count)` builds a `Slice` with no owner. `.at_unchecked` skips the bounds check. Both require `unsafe`. The safe version is `xs.slice()` and `.at`, which traps on a bad index instead of reading off the end.

A reasonable wrapper lives in `std/slice.cruft`: `from_raw` is `unsafe` inside, and its callers pass an address they already accept responsibility for.

### A C function

Declare the C function. Mark the Cruft wrapper with the effect it actually has. Put `unsafe` only around the call that touches `ptr`.

```cruft
extern fn cruft_fopen(path: ptr, mode: ptr): ptr

pub fn open(path: str, mode: str): result.Result(fs.File, i32) ! {io}
    unsafe
        var h: ptr = cruft_fopen(path.data(), mode.data())
        if h == null() then
            ret result.Err(1)
        end
        ret result.Ok(fs.File(h))
    end
end
```

`.data()` is valid only while the `String` is alive. Do not store that `ptr` into a struct that outlives the string, unless the struct is the owner of those bytes. `extern fn` parameters must be C types: integers, floats, `bool`, `ptr`, function pointers. A `Vector` cannot cross `extern`. Neither can a `Path`.

`drop` is where a handle gets closed. The standard `File` drop calls `cruft_fclose` when the handle is not null, inside `unsafe`. Callers of `write_file` never see the `FILE*`.

### Atomics, SIMD, and assembly

These are `unsafe` because a wrong order or a wrong lane is a hardware-level bug, not a type error.

```cruft
import atomic

unsafe
    var slot: i64 = 0
    cmpxchg(&slot, 0, 41, atomic.SeqCst, atomic.SeqCst)
    atomicrmw(atomic.Add, &slot, 1, atomic.Relaxed)
end
```

The order and the RMW operation have to be the constructors (`atomic.SeqCst`, `atomic.Add`), not a value you computed. `cmpxchg` returns `bool`. The place is a reference to an 8, 16, 32, or 64 bit integer.

```cruft
unsafe
    var v: Simd(i32, 4) = simd(1, 2, 3, 4)
    var lane = simd_extract(simd_add(v, simd(10, 0, 0, 0)), 0)
    asm("nop")
end
```

`Simd(T, n)` allows 2, 4, 8, or 16 lanes. `asm` takes one string and is side-effecting inline assembly.

### Threads

`thread.spawn` needs a `shared var` and a function `fn(&i64): i32`. The handle is a `ptr`, so the call is `unsafe`. `join` waits and returns the thread's `i32`. Link `thread.c` and `-pthread`. There is no async runtime. If you do not need a second thread, do not start one.

### What `unsafe` will not do

It will not let you use a moved vector, overlap two `&mut` borrows, or return an arena allocation. Those are still errors inside the block. If you can write the operation with `.at`, `slice()`, `Result`, or `&mut`, do that and leave `unsafe` on the other side of a small function.

## A small program, compiled

`note.cruft` counts nonzero values, formats the count, and writes it.

```cruft
package note

import fs
import fmt
import io

pub fn count_solid(): i32
    var blocks = [1, 0, 1, 1]
    var i: i64 = 0
    var filled: i32 = 0
    while i < blocks.length() do
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
    var n: i32 = count_solid()
    print(n)
    var shown: i64 = n as i64
    var line = shown.to_string()
    var wrote = fs.write_file("note.txt", line)
    var failed = match wrote
        case Ok(code) then false
        case Err(code) then true
    end
    if failed then
        ret 1
    end
    ret 0
end
```

A few choices in that file are deliberate.

- `count_solid` owns its vector locally, so `main` does not move a buffer it still needs.
- The arena scratch dies at `end`. `print` of its sum happens before that.
- `to_string` is implemented for `i64`, not for a bare integer literal. The `i64` binding is what selects the impl. It needs `import fmt` and `src/runtime/fmt.c`.
- `write_file` moves `line`. There is no second use.
- `match` turns the `Result` into a `bool`, and `ret` sits in the function body, where a statement is legal.

```bash
cruftc --link --cc gcc -O2 -o note note.cruft
```

`--link` adds `mem.c`, `arena.c`, `fs.c`, `io.c`, and `fmt.c` when it finds them. The program prints the arena sum, prints the count, and writes `note.txt`.

If the file is indented by hand and you want it normalised:

```bash
cruftc --fmt note.cruft
cruftc --fmt-check note.cruft
```

`--fmt-check` exits 1 and prints `would reformat` when the indent would change. The formatter only changes indentation. It is not a new syntax.

## Where to go next

The reference has the rest, with the precedence table, the ABI spelling `_C_<package>_<module>_<symbol>`, and the diagnostics named in this guide.

| You want | Read |
|---|---|
| Every type, operator, and declaration | [Types](cruft_language_reference.md#types) and [Declarations](cruft_language_reference.md#declarations) |
| Borrows, `lin` / `aff`, arenas | [Memory and safety](cruft_language_reference.md#memory-and-safety) |
| `const fn` and `cfg` | [Declarations](cruft_language_reference.md#declarations) |
| Effect rows (`io`, `alloc`, `net`) | [Effects](cruft_language_reference.md#effects) |
| `Secret` and `Leak` | [Secrecy](cruft_language_reference.md#secrecy) |
| `Path`, `Equiv`, quotients | [Proofs that are checked and then erased](cruft_language_reference.md#proofs-that-are-checked-and-then-erased) |
| `cmpxchg`, `spawn`, `Simd`, `asm` | [Atomics, threads, SIMD, and assembly](cruft_language_reference.md#atomics-threads-simd-and-assembly) |
| `cruft.toml`, `cruft.lock`, `--sanitize`, DWARF | [The compiler and its tools](cruft_language_reference.md#the-compiler-and-its-tools) |

Two toolchain pieces that are easy to miss. `cruftc --lock` writes `cruft.lock` next to `cruft.toml`, pinning each dependency path and the SHA-256 of the file. A later compile fails if the file drifts. `--sanitize address` (also `thread` or `memory`) puts the LLVM sanitizer attribute on every defined function. The attribute is the contract. This build does not run the sanitizer pass itself.

`tools/cruftlsp.py` is a small language server. It sends the buffer to `cruftc --check` and publishes the diagnostics. It does not extend the language.

Build the compiler with CMake, LLVM 16, and a C++20 compiler. The repository [README](README.md) has the configure line. The validation suite is `python tests/run_tests.py` against the `cruftc` you just built.
