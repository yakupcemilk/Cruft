section .data
x:       dq 0            ; global var x initialized to 0

y:       dq 0            ; global const y initialized to 0 (default)

fmt_int: db "%d", 10, 0  ; format string for printf

section .text
extern printf

global main

main:
    push rbp                  // save base pointer
    mov rbp, rsp              // establish new stack frame
    sub rsp, 24               // allocate space for 3 locals: x_param, y_param, z

    ; --- move function parameters into locals ---
    mov [rbp-8],  rdi        // param x -> [rbp-8]
    mov [rbp-16], rsi        // param y -> [rbp-16]

    ; --- z: i32 = x + y ---
    mov rax, [rbp-8]          // load x
    push rax                  // save x
    mov rax, [rbp-16]         // load y
    pop rcx                   // rcx = x
    add rax, rcx              // rax = y + x
    mov [rbp-24], rax         // store z at [rbp-24]

    ; --- print(z) ---
    mov rax, [rbp-24]         // load z
    mov rdi, fmt_int          // first arg: format string
    mov rsi, rax              // second arg: value
    xor rax, rax              // clear rax for variadic call
    call printf               // call printf

    ; --- return z ---
    mov rax, [rbp-24]         // return z in rax
    mov rsp, rbp              // deallocate locals
    pop rbp                   // restore base pointer
    ret                       // return from main
