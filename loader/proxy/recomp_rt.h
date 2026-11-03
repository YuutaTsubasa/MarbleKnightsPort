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
    uint64_t ex_a, ex_v, ex_v2;                        // exclusive monitor: address and value(s) loaded
} cpu;
_Static_assert(__builtin_offsetof(cpu, v) == 272, "cpu.v");
_Static_assert(sizeof(cpu) <= 832, "recomp_rt.S CPU_SIZE");

extern uint64_t IMG;  // base the image's segments are mapped at (vmaddr 0)
void host_call(cpu *c, uint64_t target);  // call untranslated code with the guest registers
void dispatch(cpu *c, uint64_t target);   // indirect call: translated if we have it
void trap(cpu *c, uint64_t pc);
void fcmp_flags(cpu *c, double a, double b);
uint64_t mk_tsd_base(void);  // mrs tpidrro_el0: this thread's pthread TSD slots

// stxr / stxp: succeed only if memory still holds what the exclusive load saw
static inline int mk_cas64pair(uint64_t a, uint64_t e0, uint64_t e1, uint64_t n0, uint64_t n1) {
    uint64_t e = (uint32_t)e0 | (e1 << 32), n = (uint32_t)n0 | (n1 << 32);
    return __atomic_compare_exchange_n((volatile uint64_t *)(uintptr_t)a, &e, n, 0, 5, 5);
}
#ifdef _WIN32
unsigned char _InterlockedCompareExchange128(volatile long long *, long long, long long, long long *);
static inline int mk_cas128(uint64_t a, uint64_t e0, uint64_t e1, uint64_t n0, uint64_t n1) {
    long long cmp[2] = { (long long)e0, (long long)e1 };
    return _InterlockedCompareExchange128((volatile long long *)(uintptr_t)a, (long long)n1, (long long)n0, cmp);
}
#else
static inline int mk_cas128(uint64_t a, uint64_t e0, uint64_t e1, uint64_t n0, uint64_t n1) {
    unsigned __int128 e = (unsigned __int128)e1 << 64 | e0, n = (unsigned __int128)n1 << 64 | n0;
    return __sync_bool_compare_and_swap((volatile unsigned __int128 *)(uintptr_t)a, e, n);
}
#endif

// C++ landing pads (Windows build): a call site that has one, and the registers the unwinder
// would restore (callee-saved x19..x30, sp, d8..d15)
typedef struct { int32_t n; const int64_t *sel; const uint64_t *ti; } mk_eh_site;
typedef struct { uint64_t x[13]; uint64_t d[8]; } mk_eh_state;
static inline void mk_eh_save(const cpu *c, mk_eh_state *s) {
    for (int i = 0; i < 13; i++) s->x[i] = c->x[19 + i];
    for (int i = 0; i < 8; i++) s->d[i] = c->v[8 + i].u[0];
}
static inline void mk_eh_restore(cpu *c, const mk_eh_state *s) {
    for (int i = 0; i < 13; i++) c->x[19 + i] = s->x[i];
    for (int i = 0; i < 8; i++) { c->v[8 + i].u[0] = s->d[i]; c->v[8 + i].u[1] = 0; }
}
int mk_eh_filter(void *exception_pointers, cpu *c, const mk_eh_site *site);

// whole-image build (Windows): every translated function and the import stubs
typedef struct { uint64_t off; void (*fn)(cpu *); } mk_rc_fn;
typedef struct { uint64_t off; int import; } mk_rc_stub;

// a batch of translated functions (work/recomp/batch.c, generated; not in Git)
typedef struct { uint64_t off; void (*fn)(cpu *); void (*thunk)(void); } mk_rc_entry;
