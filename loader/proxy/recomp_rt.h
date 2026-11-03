// Runtime interface for statically recompiled game code (tools/a64toc.py output).
// The register file layout is shared with recomp_rt.S: keep the offsets in sync.
#pragma once
#include <stdint.h>

typedef union { uint64_t u[2]; uint32_t w[4]; uint16_t h[8]; uint8_t b[16]; float f[4]; double d[2]; } vreg;
typedef struct cpu {
    uint64_t x[32];  // x0..x30, x[31] = sp            (offset 0)
    uint8_t n, z, cf, vf;                              // (offset 256)
    uint32_t pad;
    vreg v[32] __attribute__((aligned(16)));           // (offset 272)
    uint64_t jt;                                       // br target (offset 784)
} cpu;
_Static_assert(__builtin_offsetof(cpu, v) == 272, "cpu.v");

extern uint64_t IMG;  // base the image's segments are mapped at (vmaddr 0)
void host_call(cpu *c, uint64_t target);  // call untranslated code with the guest registers
void dispatch(cpu *c, uint64_t target);   // indirect call: translated if we have it
void trap(cpu *c, uint64_t pc);
void fcmp_flags(cpu *c, double a, double b);

// a batch of translated functions (work/recomp/batch.c, generated; not in Git)
typedef struct { uint64_t off; void (*fn)(cpu *); void (*thunk)(void); } mk_rc_entry;
