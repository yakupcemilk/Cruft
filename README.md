# Cruft
Goofy-ass language for goofy-ass things like games and etc.

## Example
```cruft
package mathematics

var y: i32, 
    z: i32 

const a, b, x: i32

pub fn add(y: i32, z: i32): i32
    ret (y + z)
end

pub fn arithmetic(a: i32, b: i32, x: i32): i32
    x: i32 = add(a, b)
    ret x: i32
end 

if (x: i32 > 0)
    print(x)
else (x: i32 < 0)
    print('x is smol')
end
```

An example for game

```cruft
package npc_walk 

import std 
import linear

pub fn walk(directions: array, npcID: i32): void
    const directions: array = [x, y]
    npcID: i32 = Math.random(100)
      
    pri fn direct(directions: array, speed: f16): void
        const speed: f16 = 0.5
        TO_FORWARD = while(W_PUSHED == true) { x + speed }
        TO_BACK= while(S_PUSHED == true) { x + speed * -1 }
        TO_LEFT = while(A_PUSHED == true) { y + speed }
        TO_RIGHT = while(D_PUSHED == true) { y + speed * -1 }
    end

    direct(directions, 0.5)
end
```

Backend provided by QBE, frontend provided by flex/bison