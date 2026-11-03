// Trap stubs for imports without an implementation, plus small Darwin ABI thunks
// that have to be written in assembly (variadic arguments, setjmp, ___chkstk_darwin).
    .text

// 4096 stubs of 8 bytes: `adr x17, .` then branch to the common handler, which
// turns x17 into an index, logs the symbol once, and returns 0.
    .p2align 4
    .globl mk_trap_stubs
mk_trap_stubs:
    .rept 4096
    adr x17, .
    b mk_trap_common
    .endr

mk_trap_common:
    stp x29, x30, [sp, #-96]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    adrp x0, mk_trap_stubs
    add x0, x0, :lo12:mk_trap_stubs
    sub x0, x17, x0
    lsr x0, x0, #3
    mov x1, x30
    bl mk_trap_hit
    ldp x29, x30, [sp], #96
    mov x0, #0
    movi d0, #0
    ret

// Darwin arm64 passes every variadic argument on the stack, so for the
// variadic libc entry points the first variadic slot is at [sp] on entry.
// open(path, flags, ...mode)
    .globl darwin_open
darwin_open:
    ldr x2, [sp]
    b shim_open
// openat-style helpers follow the same pattern
    .globl darwin_fcntl
darwin_fcntl:
    ldr x2, [sp]
    b shim_fcntl
    .globl darwin_ioctl
darwin_ioctl:
    ldr x2, [sp]
    b shim_ioctl
// sprintf(buf, fmt, ...) -> shim_vsprintf(buf, fmt, char *args)
    .globl darwin_sprintf
darwin_sprintf:
    mov x2, sp
    b shim_vsprintf
    .globl darwin_snprintf
darwin_snprintf:
    mov x3, sp
    b shim_vsnprintf
    .globl darwin_printf
darwin_printf:
    mov x1, sp
    b shim_vprintf
    .globl darwin_fprintf
darwin_fprintf:
    mov x2, sp
    b shim_vfprintf
    .globl darwin_syslog
darwin_syslog:
    mov x2, sp
    b shim_vsyslog
    .globl darwin_sscanf
darwin_sscanf:
    mov x2, sp
    b shim_vsscanf
    .globl darwin_asprintf
darwin_asprintf:
    mov x2, sp
    b shim_vasprintf

// std::operator+(const char*, const std::string&): sret pointer x8 -> x2
    .globl darwin_str_plus
darwin_str_plus:
    mov x2, x8
    b shim_str_plus

// ___chkstk_darwin: stack probe with a custom convention (size in x9); our
// thread stacks are fully mapped, so probing is not needed.
    .globl darwin_chkstk
darwin_chkstk:
    ret

// setjmp/longjmp with a private layout that fits Darwin's 192-byte jmp_buf
// (bionic's own uses 256 bytes and would overflow it). No signal mask.
    .globl darwin_setjmp
darwin_setjmp:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x1, sp
    str x1, [x0, #96]
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mov x0, #0
    ret

    .globl darwin_longjmp
darwin_longjmp:
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x2, [x0, #96]
    mov sp, x2
    ldp d8, d9, [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]
    cmp w1, #0
    csinc w0, w1, wzr, ne
    ret

// Apple -> AAPCS64 stack-argument adapter for engine icalls.
// x16 = descriptor { target, frame_size, n, ret_ext, entries[n] {u16 src, u16 dst, u16 size, u16 pad} }.
// Apple packs stack arguments by natural size; AAPCS64 gives each one an 8-byte slot.
// Uses only x9-x17 so the register arguments x0-x7 / d0-d7 pass through untouched.
    .globl mk_icall_adapter
mk_icall_adapter:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x16, [sp, #16]
    ldr x9, [x16, #8]          // frame_size (multiple of 16)
    sub sp, sp, x9
    ldr w10, [x16, #16]        // n
    add x11, x16, #24          // entries
    add x12, x29, #32          // stack arguments as the Apple caller laid them out
1:  cbz w10, 3f
    ldrh w13, [x11, #0]        // src
    ldrh w14, [x11, #2]        // dst
    ldrh w15, [x11, #4]        // size in bytes (1..8)
    add x13, x12, x13
    add x14, sp, x14
    str xzr, [x14]
2:  ldrb w17, [x13], #1
    strb w17, [x14], #1
    subs w15, w15, #1
    b.ne 2b
    add x11, x11, #8
    sub w10, w10, #1
    b 1b
3:  ldr x16, [x29, #16]
    ldr x17, [x16, #0]
    blr x17
    ldr x16, [x29, #16]
    ldr w9, [x16, #20]         // ret_ext: 0 none, 1 uxtb, 2 sxtb, 3 uxth, 4 sxth
    cmp w9, #1
    b.ne 4f
    and w0, w0, #0xff
4:  cmp w9, #2
    b.ne 5f
    sxtb w0, w0
5:  cmp w9, #3
    b.ne 6f
    and w0, w0, #0xffff
6:  cmp w9, #4
    b.ne 7f
    sxth w0, w0
7:  mov sp, x29
    ldp x29, x30, [sp], #32
    ret
