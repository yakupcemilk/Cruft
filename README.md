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

An example for game

```cruft
package npc_walk 

import 'std' 
import 'linear'

pub fn walk(directions: array, npcID: i32): void
    const directions: array = [x, y]
    npcID: i32 = Math.random(100)
      
    pri fn direct(directions: array, speed: i32): void
        const speed: f16 = 0.5
        TO_FORWARD = while(W_PUSHED == true) { x + speed }
        TO_BACK= while(S_PUSHED == true) { x + speed * -1 }
        TO_LEFT = while(A_PUSHED == true) { y + speed }
        TO_RIGHT = while(D_PUSHED == true) { y + speed * -1 }
    
    direct(directions, 0.5)
```

Backend provided by QBE, frontend provided by flex/bison