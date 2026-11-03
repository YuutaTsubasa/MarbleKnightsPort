// Runtime for recompiled game code on Android (validation harness for the PC build, see
// notes/2026-11-03-pc-recomp-feasibility.md): a batch of functions translated by tools/a64toc.py
// is compiled for arm64 and replaces the originals in UnityFramework. If the game keeps working,
// the translation of that batch is right. The batch (work/recomp/batch.c) is generated from the
// game and never goes into Git; without it this file only provides an empty table.
#include <android/log.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_rt.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "mkrecomp", __VA_ARGS__)
#define GUEST_STACK (48 * 1024)

uint64_t IMG;
extern const mk_rc_entry mk_rc_batch[] __attribute__((weak));
extern const int mk_rc_batch_count __attribute__((weak));
static unsigned *g_counts;

void mk_rc_host(cpu *c, uint64_t target);
void host_call(cpu *c, uint64_t target) { mk_rc_host(c, target); }

static const mk_rc_entry *find(uint64_t off) {
    int lo = 0, hi = (&mk_rc_batch_count ? mk_rc_batch_count : 0) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (mk_rc_batch[mid].off == off) return &mk_rc_batch[mid];
        if (mk_rc_batch[mid].off < off) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

void dispatch(cpu *c, uint64_t target) {
    const mk_rc_entry *e = target >= IMG ? find(target - IMG) : NULL;
    if (e) e->fn(c);
    else mk_rc_host(c, target);
}

void trap(cpu *c, uint64_t pc) {
    LOG("trap at UnityFramework+%#llx (x0=%#llx x1=%#llx)", (unsigned long long)pc, (unsigned long long)c->x[0],
        (unsigned long long)c->x[1]);
    abort();
}

void fcmp_flags(cpu *c, double a, double b) {
    if (a != a || b != b) { c->n = 0; c->z = 0; c->cf = 1; c->vf = 1; }
    else if (a == b) { c->n = 0; c->z = 1; c->cf = 1; c->vf = 0; }
    else if (a < b) { c->n = 1; c->z = 0; c->cf = 0; c->vf = 0; }
    else { c->n = 0; c->z = 0; c->cf = 1; c->vf = 0; }
}

// called from mk_rc_enter: give the function a guest stack on the real stack (so the GC scans it),
// with the caller's stack-passed arguments copied to its top
void mk_rc_run(cpu *c, void (*fn)(cpu *), const uint8_t *caller_sp, uint64_t index) {
    uint8_t region[GUEST_STACK] __attribute__((aligned(16)));
    uint8_t *top = region + GUEST_STACK - 256;
    memcpy(top, caller_sp, 256);
    c->x[31] = (uint64_t)(uintptr_t)top;
    c->x[29] = c->x[30] = 0;
    if (g_counts) __atomic_fetch_add(&g_counts[index], 1, __ATOMIC_RELAXED);
    fn(c);
}

// redirect every function of the batch to its thunk (proxy.c's redirect_image_function)
int mk_rc_install(uint8_t *base, void (*redirect)(uintptr_t, void *, const char *)) {
    IMG = (uint64_t)(uintptr_t)base;
    int n = &mk_rc_batch_count ? mk_rc_batch_count : 0;
    if (!n) return 0;
    g_counts = calloc((size_t)n, sizeof(unsigned));
    for (int i = 0; i < n; i++) redirect(mk_rc_batch[i].off, (void *)mk_rc_batch[i].thunk, NULL);
    LOG("installed %d recompiled functions", n);
    return n;
}

// log how many batch functions have run (with the fps line)
void mk_rc_report(void) {
    int n = &mk_rc_batch_count ? mk_rc_batch_count : 0;
    if (!n || !g_counts) return;
    int ran = 0;
    unsigned long long calls = 0;
    for (int i = 0; i < n; i++) { ran += g_counts[i] != 0; calls += g_counts[i]; }
    LOG("recompiled: %d of %d functions have run, %llu calls", ran, n, calls);
}
