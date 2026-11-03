// Darwin (iOS libSystem / libc++abi) imports of UnityFramework on Windows, for the recompiled code.
//
// Each function shim reads its arguments from the guest registers (Apple arm64: integers in
// x0-x7, floats in v0-v7, variadic arguments in 8-byte slots on the guest stack) and writes the
// result to x0 / v0. The Android shims (loader/src/darwin_*.c) are the reference for the Darwin
// structure layouts. Imports nobody provides fail with their name when called (win_rt.c).
#define _CRT_SECURE_NO_WARNINGS
#include <errno.h>
#include <io.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

#include "win_rt.h"

#define SHIM(name) static void s_##name(cpu *c)
#define P(i) ((void *)(uintptr_t)X(i))
#define S(i) ((const char *)(uintptr_t)X(i))
#define I(i) ((int32_t)X(i))
#define F(i) (c->v[i].f[0])
#define D(i) (c->v[i].d[0])
#define RETD(val) do { double r_ = (val); c->v[0].u[0] = 0; c->v[0].u[1] = 0; c->v[0].d[0] = r_; } while (0)
#define RETF(val) do { float r_ = (val); c->v[0].u[0] = 0; c->v[0].u[1] = 0; c->v[0].f[0] = r_; } while (0)

// ---- memory -------------------------------------------------------------------------------------
// Everything the guest may free comes from this allocator: a header in front of each block keeps
// the offset to the CRT block and the size, so aligned blocks, realloc and malloc_size all work
// with one free().
typedef struct { uint64_t size, offset; } mk_hdr;
void *mk_malloc_aligned(size_t n, size_t align) {
    if (align < 16) align = 16;
    uint8_t *raw = malloc(n + align + sizeof(mk_hdr));
    if (!raw) return NULL;
    uint8_t *p = (uint8_t *)(((uintptr_t)raw + sizeof(mk_hdr) + align - 1) & ~(uintptr_t)(align - 1));
    ((mk_hdr *)p)[-1].size = n;
    ((mk_hdr *)p)[-1].offset = (uint64_t)(p - raw);
    return p;
}
void *mk_malloc(size_t n) { return mk_malloc_aligned(n, 16); }
void mk_free(void *p) { if (p) free((uint8_t *)p - ((mk_hdr *)p)[-1].offset); }
size_t mk_msize(const void *p) { return p ? ((const mk_hdr *)p)[-1].size : 0; }
void *mk_calloc(size_t a, size_t b) { void *p = mk_malloc(a * b); if (p) memset(p, 0, a * b); return p; }
void *mk_realloc(void *p, size_t n) {
    if (!p) return mk_malloc(n);
    void *q = mk_malloc(n);
    if (!q) return NULL;
    size_t old = mk_msize(p);
    memcpy(q, p, old < n ? old : n);
    mk_free(p);
    return q;
}
char *mk_strdup(const char *s) { size_t n = strlen(s) + 1; char *d = mk_malloc(n); memcpy(d, s, n); return d; }
SHIM(malloc) { RET(mk_malloc(X(0))); }
SHIM(calloc) { RET(mk_calloc(X(0), X(1))); }
SHIM(realloc) { RET(mk_realloc(P(0), X(1))); }
SHIM(free) { mk_free(P(0)); }
SHIM(posix_memalign) { void *p = mk_malloc_aligned(X(2), X(1)); *(void **)P(0) = p; RET(p ? 0 : ENOMEM); }
SHIM(malloc_size) { RET(mk_msize(P(0))); }
SHIM(valloc) { RET(mk_malloc_aligned(X(0), 4096)); }
SHIM(aligned_alloc) { RET(mk_malloc_aligned(X(1), X(0))); }
// ---- strings -----------------------------------------------------------------------------------
SHIM(memcpy) { RET(memcpy(P(0), P(1), X(2))); }
SHIM(memmove) { RET(memmove(P(0), P(1), X(2))); }
SHIM(memset) { RET(memset(P(0), I(1), X(2))); }
SHIM(memcmp) { RET(memcmp(P(0), P(1), X(2))); }
SHIM(memchr) { RET(memchr(P(0), I(1), X(2))); }
SHIM(bzero) { memset(P(0), 0, X(1)); }
SHIM(__memcpy_chk) { RET(memcpy(P(0), P(1), X(2))); }
SHIM(__memmove_chk) { RET(memmove(P(0), P(1), X(2))); }
SHIM(__memset_chk) { RET(memset(P(0), I(1), X(2))); }
SHIM(strlen) { RET(strlen(S(0))); }
SHIM(strnlen) { RET(strnlen(S(0), X(1))); }
SHIM(strcmp) { RET(strcmp(S(0), S(1))); }
SHIM(strncmp) { RET(strncmp(S(0), S(1), X(2))); }
SHIM(strcasecmp) { RET(_stricmp(S(0), S(1))); }
SHIM(strncasecmp) { RET(_strnicmp(S(0), S(1), X(2))); }
SHIM(strcpy) { RET(strcpy(P(0), S(1))); }
SHIM(strncpy) { RET(strncpy(P(0), S(1), X(2))); }
SHIM(strcat) { RET(strcat(P(0), S(1))); }
SHIM(strchr) { RET(strchr(S(0), I(1))); }
SHIM(strrchr) { RET(strrchr(S(0), I(1))); }
SHIM(strstr) { RET(strstr(S(0), S(1))); }
SHIM(strdup) { RET(mk_strdup(S(0))); }
SHIM(strlcpy) {
    size_t n = strlen(S(1));
    if (X(2)) { size_t k = n < X(2) - 1 ? n : X(2) - 1; memcpy(P(0), S(1), k); ((char *)P(0))[k] = 0; }
    RET(n);
}
SHIM(strtol) { RET(strtoll(S(0), P(1), I(2))); }  // Darwin long is 64-bit
SHIM(strtoul) { RET(strtoull(S(0), P(1), I(2))); }
SHIM(strtoull) { RET(strtoull(S(0), P(1), I(2))); }
SHIM(strtoll) { RET(strtoll(S(0), P(1), I(2))); }
SHIM(strtod) { RETD(strtod(S(0), P(1))); }
SHIM(strtof) { RETF(strtof(S(0), P(1))); }
SHIM(atoi) { RET(atoi(S(0))); }
SHIM(getenv) { RET(getenv(S(0))); }
SHIM(strerror) { RET(strerror(I(0))); }
SHIM(__error) { RET(_errno()); }
SHIM(abort) { mk_fatal("guest called abort()"); }
SHIM(exit) { mk_log("guest exit(%d)", I(0)); ExitProcess((UINT)I(0)); }
SHIM(__stack_chk_fail) { mk_fatal("stack smashing detected in guest code"); }
SHIM(__assert_rtn) { mk_fatal("assert %s at %s:%d (%s)", S(3), S(1), I(2), S(0)); }

// ---- C++ runtime ----------------------------------------------------------------------------------
SHIM(_Znwm) { RET(mk_malloc(X(0))); }  // operator new(size_t)
SHIM(_ZdlPv) { mk_free(P(0)); }        // operator delete(void *)
SHIM(_Znam) { RET(mk_malloc(X(0))); }
SHIM(_ZdaPv) { mk_free(P(0)); }
SHIM(_ZnwmRKSt9nothrow_t) { RET(mk_malloc(X(0))); }
SHIM(_ZnamRKSt9nothrow_t) { RET(mk_malloc(X(0))); }
SHIM(_ZdlPvRKSt9nothrow_t) { mk_free(P(0)); }
SHIM(_ZdaPvRKSt9nothrow_t) { mk_free(P(0)); }
// static local guards: byte 0 = initialized, byte 1 = in progress (one global lock is enough)
static CRITICAL_SECTION g_guard_mu;
SHIM(__cxa_guard_acquire) {
    uint8_t *g = P(0);
    if (g[0]) { RET(0); return; }
    EnterCriticalSection(&g_guard_mu);
    if (g[0]) { LeaveCriticalSection(&g_guard_mu); RET(0); return; }
    RET(1);  // caller runs the initializer, then __cxa_guard_release
}
SHIM(__cxa_guard_release) { ((uint8_t *)P(0))[0] = 1; LeaveCriticalSection(&g_guard_mu); }
SHIM(__cxa_guard_abort) { LeaveCriticalSection(&g_guard_mu); }
SHIM(__cxa_atexit) { RET(0); }  // static destructors: the process exits without running them
SHIM(atexit) { RET(0); }
SHIM(__cxa_pure_virtual) { mk_fatal("pure virtual call in guest code"); }

// ---- pthread ------------------------------------------------------------------------------------
// Darwin pthread_mutex_t / pthread_cond_t: { long sig; char opaque[] }. Static initializers only set
// sig, so the Win32 object is created on first use and its pointer kept in opaque[0..7].
#define MUTEX_SIG 0x32AAABA7
#define MUTEX_RECURSIVE_SIG 0x32AAABA1
#define OUR_SIG 0x4d4b4d58  // "MKMX": opaque[0] holds a CRITICAL_SECTION *
static CRITICAL_SECTION *mutex_cs(void *m) {
    volatile int64_t *sig = m;
    CRITICAL_SECTION **slot = (CRITICAL_SECTION **)((char *)m + 8);
    if (*sig == OUR_SIG && *slot) return *slot;
    CRITICAL_SECTION *cs = malloc(sizeof(*cs));
    InitializeCriticalSectionAndSpinCount(cs, 1000);
    if (InterlockedCompareExchangePointer((void *volatile *)slot, cs, NULL) != NULL) { DeleteCriticalSection(cs); free(cs); }
    *sig = OUR_SIG;
    return *slot;
}
SHIM(pthread_mutex_init) { int64_t *m = P(0); m[0] = MUTEX_SIG; m[1] = 0; mutex_cs(m); RET(0); }
SHIM(pthread_mutex_lock) { EnterCriticalSection(mutex_cs(P(0))); RET(0); }
SHIM(pthread_mutex_trylock) { RET(TryEnterCriticalSection(mutex_cs(P(0))) ? 0 : EBUSY); }
SHIM(pthread_mutex_unlock) { LeaveCriticalSection(mutex_cs(P(0))); RET(0); }
SHIM(pthread_mutex_destroy) { RET(0); }
SHIM(pthread_mutexattr_init) { memset(P(0), 0, 16); RET(0); }
SHIM(pthread_mutexattr_settype) { RET(0); }  // CRITICAL_SECTION is always recursive
SHIM(pthread_mutexattr_setprotocol) { RET(0); }
SHIM(pthread_mutexattr_destroy) { RET(0); }
static CONDITION_VARIABLE *cond_cv(void *cv) {
    volatile int64_t *sig = cv;
    CONDITION_VARIABLE **slot = (CONDITION_VARIABLE **)((char *)cv + 8);
    if (*sig == OUR_SIG && *slot) return *slot;
    CONDITION_VARIABLE *v = malloc(sizeof(*v));
    InitializeConditionVariable(v);
    if (InterlockedCompareExchangePointer((void *volatile *)slot, v, NULL) != NULL) free(v);
    *sig = OUR_SIG;
    return *slot;
}
SHIM(pthread_cond_init) { int64_t *v = P(0); v[0] = 0; v[1] = 0; cond_cv(v); RET(0); }
SHIM(pthread_cond_wait) { SleepConditionVariableCS(cond_cv(P(0)), mutex_cs(P(1)), INFINITE); RET(0); }
SHIM(pthread_cond_timedwait) {  // abstime: Darwin timespec { long sec; long nsec }
    const int64_t *ts = P(2);
    double now = (double)time(NULL), until = (double)ts[0] + ts[1] / 1e9;
    DWORD ms = until > now ? (DWORD)((until - now) * 1000) : 0;
    RET(SleepConditionVariableCS(cond_cv(P(0)), mutex_cs(P(1)), ms) ? 0 : ETIMEDOUT);
}
SHIM(pthread_cond_timedwait_relative_np) {
    const int64_t *ts = P(2);
    DWORD ms = (DWORD)(ts[0] * 1000 + ts[1] / 1000000);
    RET(SleepConditionVariableCS(cond_cv(P(0)), mutex_cs(P(1)), ms) ? 0 : ETIMEDOUT);
}
SHIM(pthread_cond_signal) { WakeConditionVariable(cond_cv(P(0))); RET(0); }
SHIM(pthread_cond_broadcast) { WakeAllConditionVariable(cond_cv(P(0))); RET(0); }
SHIM(pthread_cond_destroy) { RET(0); }
// pthread_rwlock_t { long sig; char opaque[192] }: opaque[0..7] SRWLOCK *, opaque[8..11] writer thread id
static SRWLOCK *rw_lock(void *l) {
    volatile int64_t *sig = l;
    SRWLOCK **slot = (SRWLOCK **)((char *)l + 8);
    if (*sig == OUR_SIG && *slot) return *slot;
    SRWLOCK *w = malloc(sizeof(*w));
    InitializeSRWLock(w);
    if (InterlockedCompareExchangePointer((void *volatile *)slot, w, NULL) != NULL) free(w);
    *sig = OUR_SIG;
    return *slot;
}
SHIM(pthread_rwlock_init) { int64_t *l = P(0); l[0] = 0; l[1] = 0; l[2] = 0; rw_lock(l); RET(0); }
SHIM(pthread_rwlock_rdlock) { AcquireSRWLockShared(rw_lock(P(0))); RET(0); }
SHIM(pthread_rwlock_tryrdlock) { RET(TryAcquireSRWLockShared(rw_lock(P(0))) ? 0 : EBUSY); }
SHIM(pthread_rwlock_wrlock) { AcquireSRWLockExclusive(rw_lock(P(0))); *(volatile DWORD *)((char *)P(0) + 16) = GetCurrentThreadId(); RET(0); }
SHIM(pthread_rwlock_trywrlock) {
    if (!TryAcquireSRWLockExclusive(rw_lock(P(0)))) { RET(EBUSY); return; }
    *(volatile DWORD *)((char *)P(0) + 16) = GetCurrentThreadId();
    RET(0);
}
SHIM(pthread_rwlock_unlock) {
    volatile DWORD *w = (volatile DWORD *)((char *)P(0) + 16);
    if (*w == GetCurrentThreadId()) { *w = 0; ReleaseSRWLockExclusive(rw_lock(P(0))); }
    else ReleaseSRWLockShared(rw_lock(P(0)));
    RET(0);
}
SHIM(pthread_rwlock_destroy) { RET(0); }
// pthread_once_t { long sig; char opaque[8] }: opaque[0] = 0 (not run), 1 (running), 2 (done)
SHIM(pthread_once) {
    volatile LONG *st = (volatile LONG *)((char *)P(0) + 8);
    if (*st == 2) { RET(0); return; }
    if (InterlockedCompareExchange(st, 1, 0) == 0) {
        uint64_t x[1] = { 0 };
        mk_guest_enter(X(1), x, 0, NULL, 0, NULL);
        *st = 2;
    } else while (*st != 2) Sleep(0);
    RET(0);
}
// keys are slots of the per-thread TSD array (the guest may also read them through tpidrro_el0)
static volatile LONG g_next_key = 256;
SHIM(pthread_key_create) { LONG k = InterlockedIncrement(&g_next_key) - 1; *(uint64_t *)P(0) = (uint64_t)k; RET(k < 1024 ? 0 : EAGAIN); }
SHIM(pthread_key_delete) { RET(0); }
SHIM(pthread_getspecific) { RET(X(0) < 1024 ? mk_self()->tsd[X(0)] : 0); }
SHIM(pthread_setspecific) { if (X(0) < 1024) mk_self()->tsd[X(0)] = X(1); RET(0); }
SHIM(pthread_self) { RET(mk_self()); }
SHIM(pthread_equal) { RET(X(0) == X(1)); }
SHIM(pthread_setname_np) { RET(0); }
SHIM(pthread_attr_init) { memset(P(0), 0, 64); RET(0); }  // Darwin pthread_attr_t is 64 bytes
SHIM(pthread_attr_destroy) { RET(0); }
SHIM(pthread_attr_setstacksize) { RET(0); }
SHIM(pthread_attr_setdetachstate) { RET(0); }
SHIM(pthread_attr_setschedpolicy) { RET(0); }
SHIM(pthread_attr_setschedparam) { RET(0); }
SHIM(sched_yield) { SwitchToThread(); RET(0); }

// ---- time ---------------------------------------------------------------------------------------
static LARGE_INTEGER g_qpf;
SHIM(mach_absolute_time) {  // our mach_timebase_info is 1/1: nanoseconds
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    RET((uint64_t)((double)t.QuadPart * 1e9 / (double)g_qpf.QuadPart));
}
SHIM(mach_timebase_info) { uint32_t *tb = P(0); tb[0] = 1; tb[1] = 1; RET(0); }
SHIM(time) { int64_t t = (int64_t)_time64(NULL); if (X(0)) *(int64_t *)P(0) = t; RET(t); }
SHIM(usleep) { Sleep((DWORD)(X(0) / 1000)); RET(0); }
SHIM(sysconf) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    switch (I(0)) {
    case 29: RET(4096); break;                       // _SC_PAGESIZE
    case 57: case 58: RET(si.dwNumberOfProcessors); break;  // _SC_NPROCESSORS_CONF / ONLN
    default: mk_log("sysconf(%d) -> -1", I(0)); RET(-1);
    }
}
SHIM(getpagesize) { RET(4096); }

// ---- data ---------------------------------------------------------------------------------------
static int64_t g_stack_chk_guard = 0x5e1f6a1d2c3b4a59;
static uint64_t g_vm_page_size = 4096, g_vm_page_mask = 4095;
static uint32_t g_mach_task_self = 0x103;
static uint32_t g_ndr_record[2] = { 0x00000100, 0 };
static FILE *g_stdinp, *g_stdoutp, *g_stderrp;
// Darwin _RuneLocale: ctype inlines read __runetype[c] (offset 52 + 4*c) and __maplower/__mapupper
uint8_t mk_rune_locale[0x10000];
static void init_rune_locale(void) {
    uint32_t *type = (uint32_t *)(mk_rune_locale + 52), *lower = type + 256, *upper = lower + 256;
    for (int ch = 0; ch < 256; ch++) {
        uint32_t t = 0;
        if (ch < 128) {
            if (isalpha(ch)) t |= 0x100;   // _CTYPE_A
            if (iscntrl(ch)) t |= 0x200;   // _CTYPE_C
            if (isdigit(ch)) t |= 0x400;   // _CTYPE_D
            if (isgraph(ch)) t |= 0x800;   // _CTYPE_G
            if (islower(ch)) t |= 0x1000;  // _CTYPE_L
            if (ispunct(ch)) t |= 0x2000;  // _CTYPE_P
            if (isspace(ch)) t |= 0x4000;  // _CTYPE_S
            if (isupper(ch)) t |= 0x8000;  // _CTYPE_U
            if (isxdigit(ch)) t |= 0x10000; // _CTYPE_X
            if (ch == ' ' || ch == '\t') t |= 0x20000; // _CTYPE_B
            if (isprint(ch)) t |= 0x40000; // _CTYPE_R
        }
        type[ch] = t;
        lower[ch] = ch < 128 ? (uint32_t)tolower(ch) : (uint32_t)ch;
        upper[ch] = ch < 128 ? (uint32_t)toupper(ch) : (uint32_t)ch;
    }
}
static uint8_t g_dummy_data[1 << 20];  // zeroed storage for data imports we do not model
static size_t g_dummy_used;
static void *dummy_data(size_t n) {
    void *p = g_dummy_data + g_dummy_used;
    g_dummy_used += (n + 15) & ~15u;
    if (g_dummy_used > sizeof(g_dummy_data)) mk_fatal("out of dummy data space");
    return p;
}

// ---- table --------------------------------------------------------------------------------------
typedef struct { const char *name; mk_import_fn fn; void *data; } entry;
#define F_(n) { "_" #n, s_##n, NULL }
#define D_(n, p) { n, NULL, (void *)(p) }
static entry g_table[] = {
    F_(malloc), F_(calloc), F_(realloc), F_(free), F_(posix_memalign), F_(malloc_size), F_(valloc), F_(aligned_alloc), F_(memcpy), F_(memmove),
    F_(memset), F_(memcmp), F_(memchr), F_(bzero), F_(__memcpy_chk), F_(__memmove_chk), F_(__memset_chk), F_(strlen),
    F_(strnlen), F_(strcmp), F_(strncmp), F_(strcasecmp), F_(strncasecmp), F_(strcpy), F_(strncpy), F_(strcat),
    F_(strchr), F_(strrchr), F_(strstr), F_(strdup), F_(strlcpy), F_(strtol), F_(strtoul), F_(strtoull), F_(strtoll),
    F_(strtod), F_(strtof), F_(atoi), F_(getenv), F_(strerror), F_(__error), F_(abort), F_(exit), F_(__stack_chk_fail),
    F_(__assert_rtn), F_(_Znwm), F_(_ZdlPv), F_(_Znam), F_(_ZdaPv), F_(_ZnwmRKSt9nothrow_t), F_(_ZnamRKSt9nothrow_t),
    F_(_ZdlPvRKSt9nothrow_t), F_(_ZdaPvRKSt9nothrow_t), F_(__cxa_guard_acquire), F_(__cxa_guard_release),
    F_(__cxa_guard_abort), F_(__cxa_atexit), F_(atexit), F_(__cxa_pure_virtual), F_(pthread_mutex_init),
    F_(pthread_mutex_lock), F_(pthread_mutex_trylock), F_(pthread_mutex_unlock), F_(pthread_mutex_destroy),
    F_(pthread_mutexattr_init), F_(pthread_mutexattr_settype), F_(pthread_mutexattr_setprotocol),
    F_(pthread_mutexattr_destroy), F_(pthread_cond_init), F_(pthread_cond_wait), F_(pthread_cond_timedwait),
    F_(pthread_cond_timedwait_relative_np), F_(pthread_cond_signal), F_(pthread_cond_broadcast),
    F_(pthread_cond_destroy), F_(pthread_once), F_(pthread_rwlock_init), F_(pthread_rwlock_rdlock),
    F_(pthread_rwlock_tryrdlock), F_(pthread_rwlock_wrlock), F_(pthread_rwlock_trywrlock), F_(pthread_rwlock_unlock),
    F_(pthread_rwlock_destroy), F_(pthread_key_create), F_(pthread_key_delete),
    F_(pthread_getspecific), F_(pthread_setspecific), F_(pthread_self), F_(pthread_equal), F_(pthread_setname_np),
    F_(pthread_attr_init), F_(pthread_attr_destroy), F_(pthread_attr_setstacksize), F_(pthread_attr_setdetachstate),
    F_(pthread_attr_setschedpolicy), F_(pthread_attr_setschedparam), F_(sched_yield), F_(mach_absolute_time),
    F_(mach_timebase_info), F_(time), F_(usleep), F_(sysconf), F_(getpagesize),
    D_("___stack_chk_guard", &g_stack_chk_guard), D_("_vm_page_size", &g_vm_page_size),
    D_("_vm_page_mask", &g_vm_page_mask), D_("_mach_task_self_", &g_mach_task_self), D_("_NDR_record", g_ndr_record),
    D_("__DefaultRuneLocale", mk_rune_locale), D_("___stdinp", &g_stdinp), D_("___stdoutp", &g_stdoutp),
    D_("___stderrp", &g_stderrp),
    { 0, 0, 0 } };

static int looks_like_data(const char *s) {
    return !strncmp(s, "__ZTV", 5) || !strncmp(s, "__ZTI", 5) || !strncmp(s, "__ZTS", 5) || strstr(s, "OBJC_CLASS_$") ||
           strstr(s, "OBJC_METACLASS_$") || strstr(s, "OBJC_EHTYPE") || !strncmp(s, "_kCF", 4) || !strncmp(s, "_kCG", 4) ||
           !strncmp(s, "_kCV", 4) || !strncmp(s, "_kCM", 4) || !strncmp(s, "_kAudio", 7) || !strncmp(s, "_AV", 3) ||
           !strncmp(s, "_UI", 3) || !strncmp(s, "_NS", 3) || !strncmp(s, "__NSConcrete", 12) || !strcmp(s, "___CFConstantStringClassReference") ||
           !strncmp(s, "__dispatch_", 11) || !strncmp(s, "_GK", 3) || !strncmp(s, "_CK", 3);
}

mk_import mk_resolve_import(const char *sym, const char *lib) {
    (void)lib;
    mk_import r = { NULL, NULL };
    r = mk_cxx_lookup(sym);
    if (r.fn || r.data) return r;
    r = mk_cf_lookup(sym);
    if (r.fn || r.data) return r;
    for (entry *e = g_table; e->name; e++)
        if (!strcmp(e->name, sym)) { r.fn = e->fn; r.data = e->data; return r; }
    if ((r.fn = mk_sys_lookup(sym))) return r;
    if ((r.fn = mk_libcxx_lookup(sym))) return r;
    if (looks_like_data(sym)) {  // one placeholder per symbol: callers compare addresses
        static struct { char *name; void *p; } cache[2048];
        static int n;
        for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, sym)) { r.data = cache[i].p; return r; }
        r.data = dummy_data(256);
        if (n < 2048) { cache[n].name = _strdup(sym); cache[n++].p = r.data; }
    }
    return r;
}

void mk_shims_init(void) {
    mk_sys_init();
    mk_cxx_init();
    mk_libcxx_init();
    InitializeCriticalSection(&g_guard_mu);
    QueryPerformanceFrequency(&g_qpf);
    g_stdinp = stdin;
    g_stdoutp = stdout;
    g_stderrp = stderr;
}

// rune locale must exist before the image's imports are bound
static void __cdecl early_init(void) { init_rune_locale(); }
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) static void (__cdecl *p_early_init)(void) = early_init;
