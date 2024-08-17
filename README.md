# Cruft
Goofy-ass language for goofy-ass things like games and etc.

## Example
```cruft
package mathematics // giving a name for this code for importing like import 'src/mathematics'

pub fn add(y: i32, z: i32): i32 // public add function
    ret (y + z)

pub fn arithmetic(a: i32, b: i32, x: i32): i32
    const x: i32 = add(a, b)
    ret x

if (x > 0)
    print(x)
```

Backend provided by QBE, frontend provided by flex/bison