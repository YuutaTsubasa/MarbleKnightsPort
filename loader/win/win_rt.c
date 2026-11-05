// GameAssembly.dll runtime for the Windows build: runs the recompiled UnityFramework
// (tools/a64toc.py --all) on x64 under the official Unity Windows player.
//
//  - the image prelinked by tools/prelink_win.py (MarbleKnights_Data/mk/) is copied into memory
//    and its pointers slid; code addresses inside it stay guest addresses: every translated
//    function is looked up by its offset (dispatch)
//  - imported pointer slots get either a host data address or an import marker (an address in a
//    reserved, inaccessible range) that dispatch turns into a call of the import handler
//  - host function pointers handed to the guest (engine callbacks, icalls) become callback markers
//  - the il2cpp API exports (api_wrappers.c, generated) enter the guest through mk_guest_call
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <windows.h>

#include <dbghelp.h>

#include "recomp_rt.h"
#include "win_rt.h"

uint64_t IMG;
static size_t g_img_size;
static uint8_t *g_markers;  // MARK_SPAN bytes reserved, no access: [imports | callbacks]
#define MARK_SPAN (1u << 24)
#define MARK_CB 0x800000u  // callbacks start here inside the marker range
#define GUEST_STACK (16u << 20)

extern const mk_rc_fn mk_rc_fns[];
extern const int mk_rc_nfns;
extern const mk_rc_stub mk_rc_stubs[];
extern const int mk_rc_nstubs;
extern const char *const mk_api_names[];
extern const int mk_api_count;

static uint64_t g_exports_n;
static struct { uint64_t off; char *name; } *g_exports;
static FILE *g_log;
static CRITICAL_SECTION g_log_mu;
static char g_dir[MAX_PATH];  // exe directory, with trailing backslash

void mk_log(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    EnterCriticalSection(&g_log_mu);
    fprintf(g_log, "[%5lu] ", GetCurrentThreadId());
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_mu);
    va_end(ap);
}

void mk_fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    mk_log("FATAL: %s", buf);
    MessageBoxA(NULL, buf, "Marble Knights (recompiled)", MB_ICONERROR);
    ExitProcess(1);
}

// ---- imports -----------------------------------------------------------------------------------
typedef struct { char *name; mk_import_fn fn; } import_slot;
static import_slot *g_imports;
static int g_nimports;

static volatile LONG g_traced;  // MK_TRACE_IMPORTS: lines written (reset by il2cpp_init)
void mk_imp(cpu *c, int k) {
    if (k < 0 || k >= g_nimports) mk_fatal("bad import index %d", k);
    import_slot *s = &g_imports[k];
    static int trace = -1;
    if (trace < 0) { char v[8]; trace = GetEnvironmentVariableA("MK_TRACE_IMPORTS", v, sizeof(v)) > 0; }
    if (trace && InterlockedIncrement(&g_traced) < 20000) {
        char a0[48] = "";
        const char *p0 = (const char *)(uintptr_t)c->x[0];
        if (c->x[0] > 0x10000 && !IsBadReadPtr(p0, 1) && p0[0] >= ' ' && p0[0] < 127) snprintf(a0, sizeof(a0), " \"%.40s\"", p0);
        mk_log("call %s(%llx%s, %llx, %llx)", s->name, (unsigned long long)c->x[0], a0, (unsigned long long)c->x[1],
               (unsigned long long)c->x[2]);
    }
    if (!s->fn) {  // like the Android loader's trap stubs: log once, return 0
        static volatile LONG seen[8192];
        if (k < 8192 && !InterlockedExchange(&seen[k], 1))
            mk_log("missing import %s called (x0=%llx x1=%llx)", s->name, (unsigned long long)c->x[0], (unsigned long long)c->x[1]);
        c->x[0] = 0;
        c->v[0].u[0] = c->v[0].u[1] = 0;
        return;
    }
    s->fn(c);
}

// ---- host callbacks ----------------------------------------------------------------------------
// one argument of an icall: where the guest put it and how Windows wants it
typedef struct { uint8_t src, idx, size, fp; uint16_t stack_off; } icall_arg;  // src 0 = x reg, 1 = v reg, 2 = stack
typedef struct { int n; int8_t ret_fp, ret_size, ret_signed; icall_arg a[24]; } icall_sig;
typedef struct { void *fn; const char *what; int logged; int sig_state; icall_sig *sig; } host_cb;
static host_cb g_cbs[1 << 16];
static volatile LONG g_ncbs;

// the engine's implementation of a registered icall ("Ns.Class::Method", ignoring a "(...)" suffix)
void *mk_find_icall(const char *name) {
    size_t n = strlen(name);
    for (LONG i = 0; i < g_ncbs; i++) {
        const char *w = g_cbs[i].what;
        if (w && !strncmp(w, name, n) && (w[n] == 0 || w[n] == '(')) return g_cbs[i].fn;
    }
    return NULL;
}
uint64_t mk_host_callback(void *fn, const char *what) {
    if (!fn) return 0;
    // The player's linker folds identical functions, so different icalls can share one address
    // (CanvasGroup::get_alpha with an integer getter, say). Each icall keeps its own entry: the
    // signature, float or integer return, comes from its own managed method.
    int icall = what && strstr(what, "::");
    for (LONG i = 0; i < g_ncbs; i++) {
        if (g_cbs[i].fn != fn) continue;
        const char *w = g_cbs[i].what;
        if (icall && w && strstr(w, "::") && strcmp(w, what)) {
            mk_log("icall %s shares its code with %s", what, w);
            continue;
        }
        return (uint64_t)(uintptr_t)(g_markers + MARK_CB + 16u * (uint32_t)i);
    }
    LONG i = InterlockedIncrement(&g_ncbs) - 1;
    g_cbs[i].fn = fn;
    g_cbs[i].what = what;
    return (uint64_t)(uintptr_t)(g_markers + MARK_CB + 16u * (uint32_t)i);
}

// ---- calling engine functions (icalls, callbacks) ---------------------------------------------
// Apple arm64 numbers integer and floating-point arguments separately (x0-x7, v0-v7), then packs
// the rest on the stack by natural size; Windows x64 gives argument i the i-th slot (rcx/rdx/r8/r9
// or xmm0-3, then the stack). For icalls the signature comes from the managed method in the
// metadata (like the Android proxy's icall adapters); other callbacks pass integers only.
uint64_t mk_win64_call(void *fn, const uint64_t *slots, uint64_t n, uint64_t *xmm0_out);
typedef uintptr_t U;
U il2cpp_domain_get(void);
U il2cpp_domain_get_assemblies(U domain, U size);
U il2cpp_assembly_get_image(U assembly);
U il2cpp_class_from_name(U image, U ns, U name);
U il2cpp_class_get_nested_types(U klass, U iter);
U il2cpp_class_get_name(U klass);
U il2cpp_class_get_method_from_name(U klass, U name, int32_t argc);
U il2cpp_class_get_methods(U klass, U iter);
U il2cpp_method_get_name(U method);
uint32_t il2cpp_method_get_flags(U method, U iflags);
__declspec(dllexport) uint32_t il2cpp_method_get_param_count(const void *method);
U il2cpp_method_get_param(U method, uint32_t index);
U il2cpp_method_get_return_type(U method);
int32_t il2cpp_type_get_type(U type);
bool il2cpp_type_is_byref(U type);
U il2cpp_class_from_type(U type);
bool il2cpp_class_is_enum(U klass);
U il2cpp_class_enum_basetype(U klass);

static U find_class_path(const char *full) {  // "Ns.Outer/Inner"
    char buf[300];
    snprintf(buf, sizeof(buf), "%s", full);
    char *slash = strchr(buf, '/');
    if (slash) *slash = 0;
    char *dot = strrchr(buf, '.');
    const char *ns = "", *name = buf;
    if (dot) { *dot = 0; ns = buf; name = dot + 1; }
    size_t na = 0;
    U *as = (U *)il2cpp_domain_get_assemblies(il2cpp_domain_get(), (U)&na);
    U k = 0;
    for (size_t i = 0; i < na && !k; i++) k = il2cpp_class_from_name(il2cpp_assembly_get_image(as[i]), (U)ns, (U)name);
    while (k && slash) {
        char *inner = slash + 1;
        slash = strchr(inner, '/');
        if (slash) *slash = 0;
        U it = 0, n, found = 0;
        while ((n = il2cpp_class_get_nested_types(k, (U)&it)))
            if (!strcmp((const char *)il2cpp_class_get_name(n), inner)) { found = n; break; }
        k = found;
    }
    return k;
}
// size in bytes (1, 2, 4, 8), *fp = 1 for float/double, *sig = 1 for signed; 0 = by-value struct
static int arg_kind(U t, int *fp, int *sig) {
    *fp = 0;
    *sig = 0;
    if (il2cpp_type_is_byref(t)) return 8;
    switch (il2cpp_type_get_type(t)) {
    case 2: case 5: return 1;                    // bool, u1
    case 4: *sig = 1; return 1;                  // i1
    case 3: case 7: return 2;                    // char, u2
    case 6: *sig = 1; return 2;                  // i2
    case 8: *sig = 1; return 4;                  // i4
    case 9: return 4;                            // u4
    case 0xa: case 0xb: case 0xe: case 0xf: case 0x12: case 0x14: case 0x15: case 0x18: case 0x19:
    case 0x1b: case 0x1c: case 0x1d: return 8;  // i8, u8, string, ptr, class, array, generic, i, u, fnptr, object, szarray
    case 0xc: *fp = 1; return 4;
    case 0xd: *fp = 1; return 8;
    case 0x11: {                                 // valuetype: enums only
        U k = il2cpp_class_from_type(t);
        if (k && il2cpp_class_is_enum(k)) return arg_kind(il2cpp_class_enum_basetype(k), fp, sig);
        return 0;
    }
    default: return 0;
    }
}
static icall_sig *icall_signature(const char *name) {
    char cls[300], meth[200];
    const char *sep = strstr(name, "::");
    if (!sep || sep - name >= (long)sizeof(cls)) return NULL;
    memcpy(cls, name, (size_t)(sep - name));
    cls[sep - name] = 0;
    snprintf(meth, sizeof(meth), "%s", sep + 2);
    char *paren = strchr(meth, '(');
    if (paren) *paren = 0;
    U k = find_class_path(cls);
    if (!k) return NULL;
    // the overload that is the internal call (GetComponent has GetComponent<T>(), (Type), (string)...)
    U m = 0, it = 0, cand;
    int n_icall = 0;
    uint32_t iflags = 0, flags = 0;
    while ((cand = il2cpp_class_get_methods(k, (U)&it))) {
        if (strcmp((const char *)il2cpp_method_get_name(cand), meth)) continue;
        uint32_t ifl = 0, fl = il2cpp_method_get_flags(cand, (U)&ifl);
        if (!(ifl & 0x1000)) continue;  // METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL
        if (!m) { m = cand; iflags = ifl; flags = fl; }
        n_icall++;
    }
    if (!m) return NULL;
    if (n_icall > 1) mk_log("icall %s: %d internal-call overloads, using the first", name, n_icall);
    icall_sig *sg = calloc(1, sizeof(*sg));
    int ngr = 0, nsr = 0, off = 0, fp, sig;
    if (!(flags & 0x10)) { sg->a[sg->n].src = 0; sg->a[sg->n].idx = (uint8_t)ngr++; sg->a[sg->n].size = 8; sg->n++; }  // this
    uint32_t pc = il2cpp_method_get_param_count((const void *)m);
    for (uint32_t i = 0; i < pc; i++) {
        int sz = arg_kind(il2cpp_method_get_param(m, i), &fp, &sig);
        if (!sz || sg->n >= 24) { free(sg); return NULL; }
        icall_arg *a = &sg->a[sg->n++];
        a->size = (uint8_t)sz;
        a->fp = (uint8_t)fp;
        if (fp ? nsr < 8 : ngr < 8) { a->src = fp ? 1 : 0; a->idx = (uint8_t)(fp ? nsr++ : ngr++); continue; }
        off = (off + sz - 1) & ~(sz - 1);
        a->src = 2;
        a->stack_off = (uint16_t)off;
        off += sz;
    }
    int rfp, rsig, rsz = arg_kind(il2cpp_method_get_return_type(m), &rfp, &rsig);
    sg->ret_fp = (int8_t)rfp;
    sg->ret_size = (int8_t)(rsz ? rsz : 8);
    sg->ret_signed = (int8_t)rsig;
    return sg;
}
typedef uint64_t (*host8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
// the last host callbacks (icalls) made, for tracing where an exception came from (plugins_win.c)
const char *g_recent_cbs[8];
unsigned g_recent_cb_n;
static void call_host_cb(cpu *c, uint32_t i) {
    host_cb *h = &g_cbs[i];
    g_recent_cbs[g_recent_cb_n++ & 7] = h->what;
    if (!h->sig_state) {
        h->sig_state = 1;
        if (strstr(h->what, "::")) h->sig = icall_signature(h->what);
        if (!h->sig && strstr(h->what, "::")) mk_log("icall %s: no signature (integer arguments only)", h->what);
    }
    if (!h->logged) { h->logged = 1; mk_log("host callback %s (first call)", h->what); }
    icall_sig *sg = h->sig;
    if (!sg) { c->x[0] = ((host8)h->fn)(c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], c->x[5], c->x[6], c->x[7]); return; }
    uint64_t slots[24 > 4 ? 24 : 4] = { 0 }, x0f;
    const uint8_t *gs = (const uint8_t *)(uintptr_t)c->x[31];
    for (int k = 0; k < sg->n; k++) {
        const icall_arg *a = &sg->a[k];
        uint64_t v;
        if (a->src == 0) v = c->x[a->idx];
        else if (a->src == 1) v = c->v[a->idx].u[0];
        else { v = 0; memcpy(&v, gs + a->stack_off, a->size); }
        if (!a->fp && a->size < 8) v &= (1ull << (8 * a->size)) - 1;  // Windows expects nothing above
        if (a->fp && a->size == 4) v &= 0xffffffffull;
        slots[k] = v;
    }
    uint64_t r = mk_win64_call(h->fn, slots, (uint64_t)sg->n, &x0f);
    if (sg->ret_fp) {
        c->v[0].u[0] = sg->ret_size == 4 ? (x0f & 0xffffffffull) : x0f;
        c->v[0].u[1] = 0;
        return;
    }
    if (sg->ret_size < 8) {  // Apple callers expect the result extended to 32 bits
        uint64_t m = (1ull << (8 * sg->ret_size)) - 1;
        r &= m;
        if (sg->ret_signed && (r >> (8 * sg->ret_size - 1)) & 1) r |= ~m & 0xffffffffull;
    }
    c->x[0] = r;
}

// ---- dispatch ----------------------------------------------------------------------------------
static void (*find_fn(uint64_t off))(cpu *) {
    int lo = 0, hi = mk_rc_nfns - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (mk_rc_fns[mid].off == off) return mk_rc_fns[mid].fn;
        if (mk_rc_fns[mid].off < off) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}
static int find_stub(uint64_t off) {
    int lo = 0, hi = mk_rc_nstubs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (mk_rc_stubs[mid].off == off) return mk_rc_stubs[mid].import;
        if (mk_rc_stubs[mid].off < off) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

void dispatch(cpu *c, uint64_t target) {
    if (target - IMG < g_img_size) {
        uint64_t off = target - IMG;
        void (*f)(cpu *) = find_fn(off);
        if (f) { f(c); return; }
        int k = find_stub(off);
        if (k >= 0) { mk_imp(c, k); return; }
        // an import stub missing from the table: adrp x16, page; ldr x16, [x16, #slot]; br x16
        const uint32_t *w = (const uint32_t *)(uintptr_t)target;
        if ((w[0] & 0x9f00001f) == 0x90000010 && (w[1] & 0xffc003ff) == 0xf9400210 && w[2] == 0xd61f0200) {
            int64_t page = (int64_t)((((w[0] >> 5) & 0x7ffff) << 2) | ((w[0] >> 29) & 3)) << 12;
            if (page & (1ll << 32)) page -= 1ll << 33;
            uint64_t slot = (off & ~0xfffull) + (uint64_t)page + ((w[1] >> 10) & 0xfff) * 8;
            dispatch(c, *(uint64_t *)(uintptr_t)(IMG + slot));
            return;
        }
        mk_fatal("jump to UnityFramework+%#llx: not a translated function", (unsigned long long)off);
    }
    uint64_t m = target - (uint64_t)(uintptr_t)g_markers;
    if (m < MARK_SPAN) {
        if (m >= MARK_CB) { call_host_cb(c, (uint32_t)((m - MARK_CB) / 16)); return; }
        mk_imp(c, (int)(m / 16));
        return;
    }
    mk_fatal("indirect call to %#llx (outside the image; host pointer without a callback marker?)", (unsigned long long)target);
}
void host_call(cpu *c, uint64_t target) { dispatch(c, target); }

void trap(cpu *c, uint64_t pc) {
    mk_fatal("trap at UnityFramework+%#llx (untranslated instruction or brk; x0=%llx)", (unsigned long long)pc,
             (unsigned long long)c->x[0]);
}

void fcmp_flags(cpu *c, double a, double b) {
    if (a != a || b != b) { c->n = 0; c->z = 0; c->cf = 1; c->vf = 1; }
    else if (a == b) { c->n = 0; c->z = 1; c->cf = 1; c->vf = 0; }
    else if (a < b) { c->n = 1; c->z = 0; c->cf = 0; c->vf = 0; }
    else { c->n = 0; c->z = 0; c->cf = 1; c->vf = 0; }
}

// ---- threads and entering the guest -----------------------------------------------------------
// Each host thread gets one guest stack; nested entries (guest -> host -> guest) continue below
// the guest sp of the outer entry.
#define MAX_THREADS 1024
static mk_thread g_threads[MAX_THREADS];
static volatile LONG g_nthreads;
static DWORD g_tls_self;

mk_thread *mk_thread_new(void) {
    LONG i = InterlockedIncrement(&g_nthreads) - 1;
    if (i >= MAX_THREADS) mk_fatal("too many threads");
    mk_thread *t = &g_threads[i];
    t->port = 0x1000 + (uint32_t)i;
    return t;
}
// A thread whose host stack has room for guest stacks keeps them there (mk_thread.inline_stack);
// the others (small stacks made by Unity) get a separate guest stack, which the GC scans alone.
#define INLINE_MIN_HOST_STACK (8u << 20)
void mk_thread_bind(mk_thread *t) {  // called on the thread itself
    ULONG_PTR lo, hi;
    GetCurrentThreadStackLimits(&lo, &hi);
    t->hlow = (uint8_t *)lo;
    t->hbase = (uint8_t *)hi;
    t->inline_stack = hi - lo >= INLINE_MIN_HOST_STACK;
    if (t->inline_stack) {
        t->gstack = t->hlow;
        t->gsize = (size_t)(hi - lo);
    } else {
        t->gsize = GUEST_STACK;
        t->gstack = VirtualAlloc(NULL, GUEST_STACK, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!t->gstack) mk_fatal("no memory for a guest stack");
        mk_log("thread %lu: host stack %llu KB, guest stack kept apart (the GC does not see its host frames)",
               GetCurrentThreadId(), (unsigned long long)((hi - lo) >> 10));
    }
    t->tid = GetCurrentThreadId();
    t->tsd[0] = (uint64_t)(uintptr_t)t;
    HANDLE h;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    t->handle = h;
    TlsSetValue(g_tls_self, t);
}
mk_thread *mk_self(void) {
    mk_thread *t = TlsGetValue(g_tls_self);
    if (!t) { t = mk_thread_new(); mk_thread_bind(t); }
    return t;
}
mk_thread *mk_thread_by_port(uint32_t port) {
    uint32_t i = port - 0x1000;
    return i < (uint32_t)g_nthreads ? &g_threads[i] : NULL;
}
int mk_thread_count(void) { return (int)g_nthreads; }
mk_thread *mk_thread_at(int i) { return &g_threads[i]; }
uint64_t mk_tsd_base(void) { return (uint64_t)(uintptr_t)mk_self()->tsd; }
static const char *export_name(uint64_t addr) {
    for (uint64_t i = 0; i < g_exports_n; i++)
        if (IMG + g_exports[i].off == addr) return g_exports[i].name;
    return NULL;
}
static int log_escape(void *ep, uint64_t target) {
    const char *n = mk_exception_type_name(ep);
    if (n) {
        const char *e = export_name(target);
        mk_log("C++ exception %s leaves the guest (uncaught) at the entry %s (UnityFramework+%#llx)", n, e ? e : "?",
               (unsigned long long)(target - IMG));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
cpu *mk_current_cpu(void) { mk_thread *t = TlsGetValue(g_tls_self); return t ? t->cur : NULL; }

// guest stack region per entry on an inline_stack thread: outermost entries get more (they run
// whole frames of game code), nested ones (guest -> host -> guest) less; never below the minimum
#define GUEST_REGION_OUTER (2u << 20)
#define GUEST_REGION_NESTED (1u << 20)
#define GUEST_REGION_MIN (128u << 10)
uint64_t mk_guest_enter(uint64_t target, const uint64_t *x, int nx, const double *v, int nv, double *fret) {
    mk_thread *t = mk_self();
    cpu *outer = t->cur;
    uint8_t *outer_glow = t->glow;
    // the host registers at entry, kept in this frame: the caller's callee-saved registers may hold
    // managed pointers, and the GC sees this frame (it is above the guest region)
    volatile CONTEXT regs;
    RtlCaptureContext((CONTEXT *)&regs);
    cpu c;
    memset(&c, 0, sizeof(c));
    for (int i = 0; i < nx && i < 8; i++) c.x[i] = x[i];
    for (int i = 0; i < nv && i < 8; i++) c.v[i].d[0] = v[i];
    uint64_t sp;
    if (t->inline_stack) {
        uint8_t *here = (uint8_t *)&c;
        size_t avail = here > t->hlow + (256u << 10) ? (size_t)(here - t->hlow) - (256u << 10) : 0;
        size_t size = outer ? GUEST_REGION_NESTED : GUEST_REGION_OUTER;
        if (size > avail / 2) size = avail / 2 & ~(size_t)15;
        if (size < GUEST_REGION_MIN) mk_fatal("host stack exhausted entering the guest (%zu bytes left)", avail);
        uint8_t *region = _alloca(size);
        t->glow = region;
        sp = (uint64_t)(uintptr_t)(region + size - 256);
    } else {
        sp = outer ? outer->x[31] - 512 : (uint64_t)(uintptr_t)(t->gstack + t->gsize - 256);
        t->glow = t->gstack;
    }
    sp &= ~15ull;
    if (sp < (uint64_t)(uintptr_t)t->glow + 65536 && !t->inline_stack) mk_fatal("guest stack overflow");
    c.x[31] = sp;
    t->cur = &c;
    __try {
        __try { dispatch(&c, target); }
        __except (log_escape(GetExceptionInformation(), target)) {}
    } __finally { t->cur = outer; t->glow = outer_glow; }
    (void)regs;
    if (fret) *fret = c.v[0].d[0];
    return c.x[0];
}

// il2cpp API (api_wrappers.c): index into mk_api_names -> exported guest function
static uint64_t *g_api;
uint64_t mk_guest_call(int api, const uint64_t *x, int nx, const double *v, int nv, double *fret) {
    if (!g_api[api]) mk_fatal("il2cpp API %s not found in the image", mk_api_names[api]);
    return mk_guest_enter(g_api[api], x, nx, v, nv, fret);
}

// ---- loading -----------------------------------------------------------------------------------
static void *read_file(const char *name, size_t *size) {
    char path[MAX_PATH * 2];
    snprintf(path, sizeof(path), "%sMarbleKnights_Data\\mk\\%s", g_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) mk_fatal("cannot open %s", path);
    _fseeki64(f, 0, SEEK_END);
    size_t n = (size_t)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (!buf || fread(buf, 1, n, f) != n) mk_fatal("cannot read %s", path);
    buf[n] = 0;
    fclose(f);
    if (size) *size = n;
    return buf;
}


uint64_t mk_export(const char *name) {
    for (uint64_t i = 0; i < g_exports_n; i++)
        if (!strcmp(g_exports[i].name, name)) return IMG + g_exports[i].off;
    return 0;
}

static void load_image(void) {
    size_t n;
    uint8_t *img = read_file("image.bin", &n);
    uint8_t *base = VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) mk_fatal("cannot allocate %zu bytes for the image", n);
    memcpy(base, img, n);
    free(img);
    IMG = (uint64_t)(uintptr_t)base;
    g_img_size = n;
    mk_log("image copied to %p (%zu bytes)", base, n);
    size_t rn;
    uint32_t *rb = read_file("rebases.bin", &rn);
    for (size_t i = 0; i < rn / 4; i++) *(uint64_t *)(base + rb[i]) += IMG;
    free(rb);
    mk_log("rebased");
    g_markers = VirtualAlloc(NULL, MARK_SPAN, MEM_RESERVE, PAGE_NOACCESS);
    // exports
    char *ex = read_file("exports.txt", NULL), *save = NULL;
    size_t cap = 2048;
    g_exports = malloc(cap * sizeof(*g_exports));
    for (char *line = strtok_s(ex, "\n", &save); line; line = strtok_s(NULL, "\n", &save)) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        if (g_exports_n == cap) g_exports = realloc(g_exports, (cap *= 2) * sizeof(*g_exports));
        g_exports[g_exports_n].off = strtoull(line, NULL, 16);
        g_exports[g_exports_n++].name = sp + 1;
    }
    g_api = calloc((size_t)mk_api_count, sizeof(uint64_t));
    for (int i = 0; i < mk_api_count; i++) g_api[i] = mk_export(mk_api_names[i]);
    mk_log("%llu exports", (unsigned long long)g_exports_n);
    // imports
    char *bt = read_file("binds.txt", NULL);
    int lines = 0;
    for (char *p = bt; *p; p++) lines += *p == '\n';
    g_imports = calloc((size_t)lines, sizeof(*g_imports));
    int missing_data = 0, k = 0;
    save = NULL;
    for (char *line = strtok_s(bt, "\n", &save); line; line = strtok_s(NULL, "\n", &save), k++) {
        char sym[512], lib[128];
        unsigned long long off;
        long long addend;
        int weak;
        if (sscanf(line, "%llx %511s %127s %lld %d", &off, sym, lib, &addend, &weak) != 5) mk_fatal("bad bind line %s", line);
        g_imports[k].name = _strdup(sym);
        mk_import imp = mk_resolve_import(sym, lib);
        uint64_t *slot = (uint64_t *)(base + off);
        if (imp.data) *slot = (uint64_t)(uintptr_t)imp.data + (uint64_t)addend;
        else if (imp.fn) { g_imports[k].fn = imp.fn; *slot = (uint64_t)(uintptr_t)(g_markers + 16u * (uint32_t)k); }
        else {
            // unknown: a marker that fails with the name when called; data imports read garbage-free zeros
            *slot = (uint64_t)(uintptr_t)(g_markers + 16u * (uint32_t)k);
            missing_data++;
        }
    }
    g_nimports = k;
    mk_log("image at %p (%zu MB), %zu rebases, %d imports (%d not provided), %llu exports", base, n >> 20, rn / 4, k,
           missing_data, (unsigned long long)g_exports_n);
}

static void run_initializers(void) {
    char *it = read_file("inits.txt", NULL), *save = NULL;
    int n = 0;
    for (char *line = strtok_s(it, "\n", &save); line; line = strtok_s(NULL, "\n", &save), n++) {
        uint64_t off = strtoull(line, NULL, 16);
        uint64_t x[5] = { 0 };
        mk_guest_enter(IMG + off, x, 5, NULL, 0, NULL);
    }
    free(it);
    mk_log("ran %d static initializers", n);
}

// MK_WATCHDOG=<seconds>: every <seconds>, log where the first guest thread is (translated
// function names come from GameAssembly.pdb). For finding hangs.
static DWORD WINAPI watchdog(LPVOID p) {
    DWORD secs = (DWORD)(uintptr_t)p;
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);
    for (;;) {
        Sleep(secs * 1000);
        mk_thread *t = g_nthreads ? &g_threads[0] : NULL;
        if (!t || !t->handle) continue;
        if (SuspendThread(t->handle) == (DWORD)-1) continue;
        CONTEXT ctx = { 0 };
        ctx.ContextFlags = CONTEXT_FULL;
        GetThreadContext(t->handle, &ctx);
        STACKFRAME64 sf = { 0 };
        sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
        char line[4096];
        int n = snprintf(line, sizeof(line), "watchdog: thread %u:", t->tid);
        for (int depth = 0; depth < 24 && n < (int)sizeof(line) - 128; depth++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, t->handle, &sf, &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
            char buf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 255;
            DWORD64 disp = 0;
            if (SymFromAddr(proc, sf.AddrPC.Offset, &disp, si)) n += snprintf(line + n, sizeof(line) - n, " %s+%llx", si->Name, (unsigned long long)disp);
            else n += snprintf(line + n, sizeof(line) - n, " %llx", (unsigned long long)sf.AddrPC.Offset);
        }
        ResumeThread(t->handle);
        mk_log("%s", line);
    }
    return 0;
}

// ---- UnityPlayer.dll patches (offsets for 2019.4.17f1 win64_nondevelopment_il2cpp; the PDB names them,
// tools/win_sym.py finds them) -----------------------------------------------------------------------
typedef struct { uint32_t rva; uint8_t expect[6], replace[6]; int n; const char *what; } player_patch;
static const player_patch g_player_patches[] = {
    // InitializeGfxDevice: a forced renderer (-force-gles30 / -force-glcore) must be in BuildSettings'
    // graphics API list, which for the iOS build only holds Metal. `jne ok` -> `jmp ok; nop`.
    { 0x8969c9, { 0x0f, 0x85, 0x36, 0x01, 0x00, 0x00 }, { 0xe9, 0x37, 0x01, 0x00, 0x00, 0x90 }, 6, "InitializeGfxDevice graphics API list" },
    // TestAssetBundleCompatibility(bundle, name, error): the bundles were built for iOS/Android. Return true.
    { 0x2d7ee0, { 0 }, { 0xb0, 0x01, 0xc3 }, 3, "TestAssetBundleCompatibility" },
    { 0x2d7fa0, { 0 }, { 0xb0, 0x01, 0xc3 }, 3, "TestAssetBundleCompatibility (by name)" },
    // SerializedFile::ReadMetadata<false/true>: the file's target platform must be in a bit set of
    // desktop targets (movabs 0x2000b2a003c). Add iOS (9) and Android (13): 0x2000b2a223c.
    { 0x8f5e1d, { 0x3c, 0x00, 0x2a, 0x0b }, { 0x3c, 0x22, 0x2a, 0x0b }, 4, "ReadMetadata<false> build targets" },
    { 0x8f4f3a, { 0x3c, 0x00, 0x2a, 0x0b }, { 0x3c, 0x22, 0x2a, 0x0b }, 4, "ReadMetadata<true> build targets" },
};
static void patch_player(void) {
    uint8_t *base = (uint8_t *)GetModuleHandleA("UnityPlayer.dll");
    if (!base) { mk_log("UnityPlayer.dll not loaded: no patches"); return; }
    for (size_t i = 0; i < sizeof(g_player_patches) / sizeof(g_player_patches[0]); i++) {
        const player_patch *p = &g_player_patches[i];
        uint8_t *at = base + p->rva;
        if (p->expect[0] && memcmp(at, p->expect, (size_t)p->n)) { mk_log("patch %s: unexpected bytes, skipped", p->what); continue; }
        DWORD old;
        VirtualProtect(at, (SIZE_T)p->n, PAGE_EXECUTE_READWRITE, &old);
        memcpy(at, p->replace, (size_t)p->n);
        VirtualProtect(at, (SIZE_T)p->n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, (SIZE_T)p->n);
        mk_log("patched UnityPlayer: %s", p->what);
    }
}

static void patch_fmod(void);
void mk_plugins_install(void);  // plugins_win.c
void mk_plugins_pump(void);
static int g_started;
void mk_runtime_start(void) {
    if (g_started) return;
    g_started = 1;
    patch_player();
    load_image();
    mk_shims_init();
    patch_fmod();
    mk_plugins_install();
    char wd[16];
    if (GetEnvironmentVariableA("MK_WATCHDOG", wd, sizeof(wd))) CreateThread(NULL, 0, watchdog, (LPVOID)(uintptr_t)atoi(wd), 0, NULL);
    run_initializers();
}

// Hardware exceptions (first chance): log the code and where, before a crash handler or a silent
// process exit (a stack overflow leaves no crash report) loses it. Translated functions are named
// from GameAssembly.pdb; on a stack overflow only the address is logged (little stack is left).
// Guest registers at a crash; a register that points at an object or an Il2CppClass gets the class
// name (Il2CppClass: name at +0x10, namespace at +0x18). Reads go through ReadProcessMemory, which
// fails instead of faulting (inside the exception handler a fault would not be caught).
static int safe_read(uint64_t addr, void *out, size_t n) {
    SIZE_T got = 0;
    return addr > 0x10000 && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr, out, n, &got) && got == n;
}
static int class_name_at(uint64_t k, char *out, int n) {
    uint64_t p[2];
    char nm[81] = { 0 }, ns[81] = { 0 };
    if (!safe_read(k + 0x10, p, sizeof(p))) return 0;
    for (int i = 0; i < 80 && safe_read(p[0] + i, nm + i, 1) && nm[i]; i++) {}
    for (int i = 0; i < 80 && safe_read(p[1] + i, ns + i, 1) && ns[i]; i++) {}
    if (!(nm[0] >= 'A' && nm[0] <= 'z') || strlen(nm) >= 80) return 0;
    snprintf(out, n, "%s%s%s", ns, ns[0] ? "." : "", nm);
    return 1;
}
static void log_guest_registers(void) {
    cpu *g = mk_current_cpu();
    if (!g) return;
    for (int i = 0; i < 31; i++) {
        uint64_t v = g->x[i], k = 0;
        char nm[180] = "";
        if (!class_name_at(v, nm, sizeof(nm)) && safe_read(v, &k, 8) && class_name_at(k & ~1ull, nm + 10, sizeof(nm) - 10))
            memcpy(nm, "object of ", 10);
        mk_log("  guest x%d=%#llx %s", i, (unsigned long long)v, nm);
    }
}

static LONG CALLBACK log_exception(EXCEPTION_POINTERS *ep) {
    static volatile LONG logged;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&logged) > 8) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t pc = ep->ContextRecord->Rip;
    HMODULE mod = NULL;
    char modname[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)pc, &mod))
        GetModuleFileNameA(mod, modname, sizeof(modname));
    const char *base = strrchr(modname, '\\');
    base = base ? base + 1 : modname;
    if (code == EXCEPTION_STACK_OVERFLOW) {
        mk_log("exception %#lx (stack overflow) at %s+%#llx rsp=%#llx", code, base, (unsigned long long)(pc - (uint64_t)(uintptr_t)mod),
               (unsigned long long)ep->ContextRecord->Rsp);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static int sym_ready;
    HANDLE proc = GetCurrentProcess();
    if (!sym_ready) { SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(proc, NULL, TRUE); sym_ready = 1; }
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    int named = SymFromAddr(proc, pc, &disp, si);
    mk_log("exception %#lx at %s+%#llx (%s+%#llx), address %#llx", code, base, (unsigned long long)(pc - (uint64_t)(uintptr_t)mod),
           named ? si->Name : "?", (unsigned long long)disp,
           (unsigned long long)(ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0));
    if (mod == GetModuleHandleA("GameAssembly.dll")) log_guest_registers();
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_log_mu);
        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        char *slash = strrchr(g_dir, '\\');
        if (slash) slash[1] = 0;
        char path[MAX_PATH * 2];
        snprintf(path, sizeof(path), "%smk_ga.log", g_dir);
        g_log = fopen(path, "w");
        g_tls_self = TlsAlloc();
        mk_log("GameAssembly.dll (recompiled UnityFramework, %d functions)", mk_rc_nfns);
        AddVectoredExceptionHandler(1, log_exception);
    }
    return TRUE;
}

// ---- hand-written API entries ------------------------------------------------------------------
void real_il2cpp_add_internal_call(uintptr_t name, uintptr_t method);
void real_il2cpp_set_data_dir(uintptr_t path);
int32_t real_il2cpp_init(uintptr_t domain);

// The iOS build strips some methods the Windows engine still asks about (Analytics, Windows.Speech...):
// like the Android proxy, NULL methods are answered instead of passed on.
uint32_t real_il2cpp_method_get_param_count(uintptr_t m);
uintptr_t real_il2cpp_runtime_invoke(uintptr_t m, uintptr_t obj, uintptr_t params, uintptr_t exc);
uintptr_t real_il2cpp_runtime_invoke_convert_args(uintptr_t m, uintptr_t obj, uintptr_t params, int32_t n, uintptr_t exc);
__declspec(dllexport) uint32_t il2cpp_method_get_param_count(const void *m) {
    return m ? real_il2cpp_method_get_param_count((uintptr_t)m) : 0;
}
U il2cpp_method_get_class(U method);
static void log_invoke_exception(const void *m, void **exc) {
    if (!exc || !*exc) return;
    static volatile LONG n;
    if (InterlockedIncrement(&n) > 200) return;
    U k = il2cpp_method_get_class((U)m);
    mk_log("invoke %s::%s -> managed exception", k ? (const char *)il2cpp_class_get_name(k) : "?", (const char *)il2cpp_method_get_name((U)m));
}
__declspec(dllexport) void *il2cpp_runtime_invoke(const void *m, void *obj, void **params, void **exc) {
    if (!m) { if (exc) *exc = NULL; return NULL; }
    mk_plugins_pump();
    void *r = (void *)real_il2cpp_runtime_invoke((uintptr_t)m, (uintptr_t)obj, (uintptr_t)params, (uintptr_t)exc);
    log_invoke_exception(m, exc);
    return r;
}
__declspec(dllexport) void *il2cpp_runtime_invoke_convert_args(const void *m, void *obj, void **params, int n, void **exc) {
    if (!m) { if (exc) *exc = NULL; return NULL; }
    return (void *)real_il2cpp_runtime_invoke_convert_args((uintptr_t)m, (uintptr_t)obj, (uintptr_t)params, n, (uintptr_t)exc);
}

// The data is the iOS build: the game picks AssetBundles/<platform>, Rewired's platform, etc. from
// Application.platform, so report RuntimePlatform.IPhonePlayer (8) like the original.
static int32_t get_platform_ios(void) { return 8; }
// Under OpenGL the Windows player cannot decode video (Media Foundation needs D3D11/12): the intro
// video would never advance and BootstrapVideo.Update waits for time / length to reach its
// threshold. Report a time past the end so the bootstrap carries on.
static double video_time_ended(void *player) { (void)player; return 1e9; }
static int gl_renderer(void) {  // MK_GLES: set by our MarbleKnights.exe, which adds -force-gles30
    const char *cl = GetCommandLineA();
    return strstr(cl, "-force-gl") != NULL || (GetEnvironmentVariableA("MK_GLES", NULL, 0) > 0 && !strstr(cl, "-force-d3d"));
}

void *mk_input_icall(const char *name, void *fn);
__declspec(dllexport) void il2cpp_add_internal_call(const char *name, void *method) {
    mk_runtime_start();
    if (!strcmp(name, "UnityEngine.Application::get_platform")) method = (void *)get_platform_ios;
    if (!strcmp(name, "UnityEngine.Video.VideoPlayer::get_time") && gl_renderer()) method = (void *)video_time_ended;
    method = mk_input_icall(name, method);  // plugins_win.c: the mouse as a touch
    real_il2cpp_add_internal_call((uintptr_t)name, (uintptr_t)mk_host_callback(method, _strdup(name)));
}
void real_il2cpp_shutdown(void);
// The player calls this last, after the scenes are torn down and PlayerPrefs are written. The
// runtime's own shutdown (stopping the GC and the guest threads) never returns here, so the
// process just ends.
__declspec(dllexport) void il2cpp_shutdown(void) {
    mk_log("il2cpp_shutdown: ending the process");
    TerminateProcess(GetCurrentProcess(), 0);
    real_il2cpp_shutdown();
}
__declspec(dllexport) void il2cpp_set_data_dir(const char *path) {
    mk_runtime_start();
    mk_log("il2cpp_set_data_dir(%s)", path);
    real_il2cpp_set_data_dir((uintptr_t)path);
}
// ---- replacing a translated function ------------------------------------------------------------
// Direct calls between translated functions do not go through dispatch, so a function is replaced by
// writing `mov rax, imm64; jmp rax` over the start of its compiled code (in this DLL). The replacement
// has the same shape, void f(cpu *).
static void (*find_fn(uint64_t off))(cpu *);
void mk_override_function(uint64_t off, void (*with)(cpu *), const char *what) {
    uint8_t *f = (uint8_t *)find_fn(off);
    if (!f) { mk_log("override %s: no translated function at %#llx", what, (unsigned long long)off); return; }
    DWORD old;
    VirtualProtect(f, 12, PAGE_EXECUTE_READWRITE, &old);
    f[0] = 0x48; f[1] = 0xb8;
    memcpy(f + 2, &with, 8);
    f[10] = 0xff; f[11] = 0xe0;
    VirtualProtect(f, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, 12);
    mk_log("override %s", what);
}

// FMOD: the iOS output (AudioUnit / AVAudioSession) does not exist here; start FMOD Studio with the
// no-sound output so the game's audio calls succeed (the Android proxy does the same before
// attaching its own output). FMOD_Studio_System_Initialize is `stp; mov; ldp; b <impl>`.
void mk_fmod_attach_output(uint64_t core);
static uint64_t g_fmod_init_impl, g_fmod_get_core, g_fmod_set_output;
static void fmod_studio_initialize(cpu *c) {
    uint64_t studio = c->x[0], args[5] = { c->x[0], c->x[1], c->x[2], c->x[3], c->x[4] };
    uint64_t core = 0, x[2] = { studio, (uint64_t)(uintptr_t)&core };
    uint64_t r1 = mk_guest_enter(g_fmod_get_core, x, 2, NULL, 0, NULL);
    uint64_t r2 = ~0ull;
    if (core) { uint64_t y[2] = { core, 2 /* FMOD_OUTPUTTYPE_NOSOUND */ }; r2 = mk_guest_enter(g_fmod_set_output, y, 2, NULL, 0, NULL); }
    uint64_t r = mk_guest_enter(g_fmod_init_impl, args, 5, NULL, 0, NULL);
    mk_log("FMOD Studio initialize with the no-sound output: core=%d setOutput=%d init=%d", (int)r1, (int)r2, (int)r);
    if (!(int)r && core) mk_fmod_attach_output(core);  // fmod_win.c: the mix goes to waveOut
    c->x[0] = r;
}
static void patch_fmod(void) {
    uint64_t init = mk_export("_FMOD_Studio_System_Initialize");
    g_fmod_get_core = mk_export("_FMOD_Studio_System_GetCoreSystem");
    g_fmod_set_output = mk_export("_FMOD5_System_SetOutput");
    if (!init || !g_fmod_get_core || !g_fmod_set_output) { mk_log("FMOD exports not found"); return; }
    const uint32_t *p = (const uint32_t *)(uintptr_t)init;
    if (p[0] != 0xa9bf7bfd || (p[3] & 0xfc000000) != 0x14000000) { mk_log("FMOD_Studio_System_Initialize: unexpected code"); return; }
    int32_t imm = (int32_t)(p[3] << 6) >> 6;
    g_fmod_init_impl = (uint64_t)(uintptr_t)(p + 3) + (int64_t)imm * 4;
    mk_override_function(init - IMG, fmod_studio_initialize, "FMOD_Studio_System_Initialize");
}

// ---- iOS-only icalls the Windows engine does not register (same values as the Android proxy) ----
U il2cpp_string_new(U str);
static int32_t icall_ios_generation(void) { return 10002; }  // DeviceGeneration.iPadUnknown
static U icall_ios_system_version(void) { return il2cpp_string_new((U)"14.4"); }
static void icall_noop(void) {}
static int32_t icall_false(void) { return 0; }
static int32_t icall_true(void) { return 1; }
static U icall_null(void) { return 0; }
static U icall_player_name(void) { return il2cpp_string_new((U)"Player"); }
static U icall_player_id(void) { return il2cpp_string_new((U)"G:0000000000"); }
static void register_ios_icalls(void) {
#define GC "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::"
    static const struct { const char *name; void *fn; } calls[] = {
        { "UnityEngine.iOS.Device::get_generation", (void *)icall_ios_generation },
        { "UnityEngine.iOS.Device::get_systemVersion", (void *)icall_ios_system_version },
        { "UnityEngine.UnhandledExceptionHandler::iOSNativeUnhandledExceptionHandler", (void *)icall_noop },
        { GC "Authenticate", (void *)icall_noop }, { GC "GetAuthenticated", (void *)icall_true },
        { GC "Internal_UserName", (void *)icall_player_name }, { GC "Internal_UserID", (void *)icall_player_id },
        { GC "Internal_UserGameID", (void *)icall_player_id }, { GC "Internal_LegacyUserID", (void *)icall_player_id },
        { GC "GetIsUnderage", (void *)icall_false }, { GC "GetUserImage", (void *)icall_null },
        { GC "LoadFriends", (void *)icall_noop }, { GC "InternalLoadAchievementDescriptions", (void *)icall_noop },
        { GC "InternalLoadAchievements", (void *)icall_noop }, { GC "InternalReportProgress", (void *)icall_noop },
        { GC "InternalReportScore", (void *)icall_noop }, { GC "InternalLoadScores", (void *)icall_noop },
        { GC "Internal_ShowAchievementsUI", (void *)icall_noop }, { GC "Internal_ShowLeaderboardUI", (void *)icall_noop },
        { GC "Internal_LoadUsers", (void *)icall_noop }, { GC "ResetAllAchievements", (void *)icall_noop },
        { GC "ShowDefaultAchievementBanner", (void *)icall_noop }, { GC "ShowSpecificLeaderboardUI", (void *)icall_noop },
    };
#undef GC
    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++)
        real_il2cpp_add_internal_call((uintptr_t)calls[i].name, (uintptr_t)mk_host_callback(calls[i].fn, calls[i].name));
    mk_log("registered %zu iOS-only icalls", sizeof(calls) / sizeof(calls[0]));
}

__declspec(dllexport) int il2cpp_init(const char *domain) {
    mk_runtime_start();
    mk_log("il2cpp_init(%s)", domain);
    g_traced = 0;
    int r = real_il2cpp_init((uintptr_t)domain);
    mk_log("il2cpp_init -> %d", r);
    register_ios_icalls();
    // test switch: a file named mk_nogc next to the exe turns the garbage collector off, to tell
    // whether a bug comes from objects that are collected while still in use
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    while (n && path[n - 1] != '\\') n--;
    if (n && n + 8 < MAX_PATH) {
        strcpy(path + n, "mk_nogc");
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            void il2cpp_gc_disable(void);
            il2cpp_gc_disable();
            mk_log("mk_nogc: garbage collector disabled");
        }
    }
    return r;
}
