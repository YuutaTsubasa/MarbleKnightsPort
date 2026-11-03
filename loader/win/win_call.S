// uint64_t mk_win64_call(void *fn, const uint64_t *slots, uint64_t n, uint64_t *xmm0_out)
// Call a Windows x64 function with n positional arguments. The first four go into both the integer
// and the xmm register of their position (the callee reads the one its type uses); the rest go to
// the stack above the 32-byte shadow area. slots must have room for at least 4 entries.
// Returns rax; xmm0 (float/double results) is stored to *xmm0_out.
// It has Windows unwind data: exceptions the engine raises (il2cpp_raise_exception from an icall)
// unwind through it back to the guest code that made the call.
    .intel_syntax noprefix
    .text
    .globl mk_win64_call
    .p2align 4
    .def mk_win64_call; .scl 2; .type 32; .endef
    .seh_proc mk_win64_call
mk_win64_call:
    push rbp
    .seh_pushreg rbp
    push rsi
    .seh_pushreg rsi
    push rdi
    .seh_pushreg rdi
    push rbx
    .seh_pushreg rbx
    sub rsp, 40
    .seh_stackalloc 40
    lea rbp, [rsp + 32]
    .seh_setframe rbp, 32
    .seh_endprologue
    mov r10, rcx
    mov rsi, rdx
    mov rdi, r8
    mov rbx, r9
    mov rax, rdi
    cmp rax, 4
    jae 1f
    mov rax, 4
1:  lea rax, [rax * 8 + 15]
    and rax, -16
    sub rsp, rax
    mov rcx, 4
2:  cmp rcx, rdi
    jae 3f
    mov rax, [rsi + rcx * 8]
    mov [rsp + rcx * 8], rax
    inc rcx
    jmp 2b
3:  mov rcx, [rsi]
    mov rdx, [rsi + 8]
    mov r8, [rsi + 16]
    mov r9, [rsi + 24]
    movq xmm0, rcx
    movq xmm1, rdx
    movq xmm2, r8
    movq xmm3, r9
    call r10
    movq [rbx], xmm0
    lea rsp, [rbp + 8]
    pop rbx
    pop rdi
    pop rsi
    pop rbp
    ret
    .seh_endproc
