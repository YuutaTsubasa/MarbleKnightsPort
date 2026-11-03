// Entry and exit trampolines between native ARM64 code and recompiled functions (recomp_rt.h).
//
// mk_rc_enter: the original function's first instructions are redirected to a per-function thunk
//   that sets x16 = translated function, x17 = its table index and branches here. We store the
//   argument registers (x0-x8, q0-q7) in a cpu struct on the stack, run it (mk_rc_run gives it a
//   guest stack), then return x0/x1 and q0-q3 like the original would.
// mk_rc_host(cpu *c, target): call native code with the guest's registers; the 256 bytes at the
//   guest sp are copied to the real stack so stack-passed arguments line up.
// Both keep frame records and CFI so C++ exceptions unwind through them.

#define CPU_SIZE 832
#define CPU_V 272

    .text
    .p2align 2
    .globl mk_rc_enter
    .hidden mk_rc_enter
    .type mk_rc_enter, %function
mk_rc_enter:
    .cfi_startproc
    stp x29, x30, [sp, #-16]!
    .cfi_def_cfa_offset 16
    .cfi_offset w30, -8
    .cfi_offset w29, -16
    mov x29, sp
    .cfi_def_cfa w29, 16
    sub sp, sp, #CPU_SIZE
    mov x9, sp
    stp x0, x1, [x9, #0]
    stp x2, x3, [x9, #16]
    stp x4, x5, [x9, #32]
    stp x6, x7, [x9, #48]
    str x8, [x9, #64]
    add x10, x9, #CPU_V
    stp q0, q1, [x10, #0]
    stp q2, q3, [x10, #32]
    stp q4, q5, [x10, #64]
    stp q6, q7, [x10, #96]
    mov x0, x9             // cpu
    mov x1, x16            // translated function
    add x2, x29, #16       // caller's sp: stack-passed arguments
    mov x3, x17            // table index
    bl mk_rc_run
    mov x9, sp
    ldp x0, x1, [x9, #0]
    add x10, x9, #CPU_V
    ldp q0, q1, [x10, #0]
    ldp q2, q3, [x10, #32]
    mov sp, x29
    ldp x29, x30, [sp], #16
    .cfi_def_cfa sp, 0
    .cfi_restore w30
    .cfi_restore w29
    ret
    .cfi_endproc
    .size mk_rc_enter, . - mk_rc_enter

    .p2align 2
    .globl mk_rc_host
    .hidden mk_rc_host
    .type mk_rc_host, %function
mk_rc_host:
    .cfi_startproc
    stp x29, x30, [sp, #-32]!
    .cfi_def_cfa_offset 32
    .cfi_offset w30, -24
    .cfi_offset w29, -32
    stp x19, x20, [sp, #16]
    .cfi_offset w20, -8
    .cfi_offset w19, -16
    mov x29, sp
    .cfi_def_cfa w29, 32
    mov x19, x0
    mov x20, x1
    sub sp, sp, #256
    ldr x9, [x19, #248]    // guest sp
    mov x10, #0
1:  ldr x11, [x9, x10]
    str x11, [sp, x10]
    add x10, x10, #8
    cmp x10, #256
    b.lt 1b
    add x10, x19, #CPU_V
    ldp q0, q1, [x10, #0]
    ldp q2, q3, [x10, #32]
    ldp q4, q5, [x10, #64]
    ldp q6, q7, [x10, #96]
    ldp x0, x1, [x19, #0]
    ldp x2, x3, [x19, #16]
    ldp x4, x5, [x19, #32]
    ldp x6, x7, [x19, #48]
    ldr x8, [x19, #64]
    blr x20
    stp x0, x1, [x19, #0]
    add x10, x19, #CPU_V
    stp q0, q1, [x10, #0]
    stp q2, q3, [x10, #32]
    mov sp, x29
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    .cfi_def_cfa sp, 0
    .cfi_restore w30
    .cfi_restore w29
    .cfi_restore w20
    .cfi_restore w19
    ret
    .cfi_endproc
    .size mk_rc_host, . - mk_rc_host
