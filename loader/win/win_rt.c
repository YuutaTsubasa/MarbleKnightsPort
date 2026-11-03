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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
typedef struct { void *fn; const char *what; int logged; } host_cb;
static host_cb g_cbs[1 << 16];
static volatile LONG g_ncbs;

uint64_t mk_host_callback(void *fn, const char *what) {
    if (!fn) return 0;
    for (LONG i = 0; i < g_ncbs; i++)
        if (g_cbs[i].fn == fn) return (uint64_t)(uintptr_t)(g_markers + MARK_CB + 16u * (uint32_t)i);
    LONG i = InterlockedIncrement(&g_ncbs) - 1;
    g_cbs[i].fn = fn;
    g_cbs[i].what = what;
    return (uint64_t)(uintptr_t)(g_markers + MARK_CB + 16u * (uint32_t)i);
}

// Engine callbacks and icalls: integer/pointer arguments only for now (x0-x7 -> rcx, rdx, r8, r9, stack).
typedef uint64_t (*host8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static void call_host_cb(cpu *c, uint32_t i) {
    host_cb *h = &g_cbs[i];
    if (!h->logged) { h->logged = 1; mk_log("host callback %s (first call)", h->what); }
    c->x[0] = ((host8)h->fn)(c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], c->x[5], c->x[6], c->x[7]);
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
    t->gsize = GUEST_STACK;
    t->gstack = VirtualAlloc(NULL, GUEST_STACK, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!t->gstack) mk_fatal("no memory for a guest stack");
    return t;
}
void mk_thread_bind(mk_thread *t) {  // called on the thread itself
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
static int log_escape(void *ep) {
    const char *n = mk_exception_type_name(ep);
    if (n) mk_log("C++ exception %s leaves the guest (uncaught)", n);
    return EXCEPTION_CONTINUE_SEARCH;
}
cpu *mk_current_cpu(void) { mk_thread *t = TlsGetValue(g_tls_self); return t ? t->cur : NULL; }

uint64_t mk_guest_enter(uint64_t target, const uint64_t *x, int nx, const double *v, int nv, double *fret) {
    mk_thread *t = mk_self();
    cpu *outer = t->cur;
    cpu c;
    memset(&c, 0, sizeof(c));
    for (int i = 0; i < nx && i < 8; i++) c.x[i] = x[i];
    for (int i = 0; i < nv && i < 8; i++) c.v[i].d[0] = v[i];
    uint64_t sp = outer ? outer->x[31] - 512 : (uint64_t)(uintptr_t)(t->gstack + t->gsize - 256);
    sp &= ~15ull;
    if (sp < (uint64_t)(uintptr_t)t->gstack + 65536) mk_fatal("guest stack overflow");
    c.x[31] = sp;
    t->cur = &c;
    __try {
        __try { dispatch(&c, target); }
        __except (log_escape(GetExceptionInformation())) {}
    } __finally { t->cur = outer; }
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

static uint64_t g_exports_n;
static struct { uint64_t off; char *name; } *g_exports;
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

static int g_started;
void mk_runtime_start(void) {
    if (g_started) return;
    g_started = 1;
    load_image();
    mk_shims_init();
    char wd[16];
    if (GetEnvironmentVariableA("MK_WATCHDOG", wd, sizeof(wd))) CreateThread(NULL, 0, watchdog, (LPVOID)(uintptr_t)atoi(wd), 0, NULL);
    run_initializers();
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
    }
    return TRUE;
}

// ---- hand-written API entries ------------------------------------------------------------------
void real_il2cpp_add_internal_call(uintptr_t name, uintptr_t method);
void real_il2cpp_set_data_dir(uintptr_t path);
int32_t real_il2cpp_init(uintptr_t domain);

__declspec(dllexport) void il2cpp_add_internal_call(const char *name, void *method) {
    mk_runtime_start();
    real_il2cpp_add_internal_call((uintptr_t)name, (uintptr_t)mk_host_callback(method, _strdup(name)));
}
__declspec(dllexport) void il2cpp_set_data_dir(const char *path) {
    mk_runtime_start();
    mk_log("il2cpp_set_data_dir(%s)", path);
    real_il2cpp_set_data_dir((uintptr_t)path);
}
__declspec(dllexport) int il2cpp_init(const char *domain) {
    mk_runtime_start();
    mk_log("il2cpp_init(%s)", domain);
    g_traced = 0;
    int r = real_il2cpp_init((uintptr_t)domain);
    mk_log("il2cpp_init -> %d", r);
    return r;
}
