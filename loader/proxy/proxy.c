// Proxy libil2cpp.so for the official Unity 2019.4.17f1 Android player.
//
// libunity.so dlopen()s libil2cpp.so and dlsym()s the il2cpp_* API. We export
// the same names (trampolines generated from il2cpp_api.txt) and forward them
// into the iOS UnityFramework, which we load from libmkframework.so in the
// app's native library directory when this library is loaded.
#define _GNU_SOURCE
#include <android/log.h>
#include <dlfcn.h>
#include <libgen.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

#include "darwin.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "mkproxy", __VA_ARGS__)

extern void *mk_api_slots[];
extern const char *mk_api_names[];

static mk_image g_img;
static void patch_image(void);
static char g_external_files[512];
static struct sigaction g_prev_segv, g_prev_bus, g_prev_ill, g_prev_abrt;

static void log_addr(const char *what, uintptr_t a) {
    if (a >= (uintptr_t)g_img.base && a < (uintptr_t)g_img.base + g_img.size)
        LOG("  %s = UnityFramework+%#lx", what, (unsigned long)(a - (uintptr_t)g_img.base));
    else {
        Dl_info di;
        if (dladdr((void *)a, &di) && di.dli_fname)
            LOG("  %s = %s+%#lx (%s)", what, di.dli_fname, (unsigned long)(a - (uintptr_t)di.dli_fbase),
                di.dli_sname ? di.dli_sname : "?");
        else
            LOG("  %s = %#lx", what, (unsigned long)a);
    }
}

// Log image-relative addresses, then hand the signal to whoever was installed before us.
static void crash_handler(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    LOG("*** signal %d, fault addr %p", sig, si->si_addr);
    log_addr("pc", uc->uc_mcontext.pc);
    log_addr("lr", uc->uc_mcontext.regs[30]);
    uintptr_t fp = uc->uc_mcontext.regs[29];
    for (int i = 0; i < 32 && fp && !(fp & 7); i++) {
        uintptr_t *f = (uintptr_t *)fp;
        char tag[16];
        snprintf(tag, sizeof(tag), "frame%d", i);
        log_addr(tag, f[1]);
        if (f[0] <= fp) break;
        fp = f[0];
    }
    struct sigaction *prev = sig == SIGSEGV ? &g_prev_segv : sig == SIGBUS ? &g_prev_bus
                           : sig == SIGILL ? &g_prev_ill : &g_prev_abrt;
    sigaction(sig, prev, NULL);
    raise(sig);
}

static void install_crash_logger(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    sigaction(SIGBUS, &sa, &g_prev_bus);
    sigaction(SIGILL, &sa, &g_prev_ill);
    sigaction(SIGABRT, &sa, &g_prev_abrt);
}

// ---- in-memory patches to the official libunity.so (2019.4.17f1, arm64 Release) ----
// Offsets come from Unity's own symbol file (libunity.sym.so); each patch checks
// the original instruction first and is skipped if it does not match.
typedef struct { uint32_t off, expect, replace; const char *what; } code_patch;
static const code_patch g_unity_patches[] = {
    // SerializedFile::ReadMetadata<false/true>: `ldr w1, [sp, #0xd8]` (file's target platform) is
    // compared with 13 (Android). Load 13 instead so iOS-built files and AssetBundles are accepted.
    { 0x6207e4, 0xb940dbe1, 0x528001a1, "ReadMetadata<false> build target" },
    { 0x621560, 0xb940dbe1, 0x528001a1, "ReadMetadata<true> build target" },
    // InitializeGfxDevice: a forced renderer (-force-gles30) must appear in BuildSettings' graphics
    // API list, which for the iOS build only holds Metal. `b.ne ok` -> `b ok`.
    { 0x88ba00, 0x54000841, 0x14000042, "InitializeGfxDevice graphics API list" },
};

static int find_unity(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    if (info->dlpi_name && strstr(info->dlpi_name, "/libunity.so")) { *(uintptr_t *)data = info->dlpi_addr; return 1; }
    return 0;
}

static void patch_unity(void) {
    uintptr_t base = 0;
    dl_iterate_phdr(find_unity, &base);
    if (!base) { LOG("libunity.so not loaded yet; no patches"); return; }
    long pg = sysconf(_SC_PAGESIZE);
    for (size_t i = 0; i < sizeof(g_unity_patches) / sizeof(g_unity_patches[0]); i++) {
        const code_patch *p = &g_unity_patches[i];
        uint32_t *at = (uint32_t *)(base + p->off);
        if (*at != p->expect) { LOG("patch %s: found %#x, expected %#x; skipped", p->what, *at, p->expect); continue; }
        void *page = (void *)((uintptr_t)at & ~(uintptr_t)(pg - 1));
        mprotect(page, pg, PROT_READ | PROT_WRITE | PROT_EXEC);
        *at = p->replace;
        mprotect(page, pg, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)at, (char *)(at + 1));
        LOG("patched libunity: %s", p->what);
    }
}

__attribute__((constructor)) static void proxy_init(void) {
    patch_unity();
    Dl_info di;
    dladdr((void *)proxy_init, &di);
    char dir[512], path[600];
    snprintf(dir, sizeof(dir), "%s", di.dli_fname);
    snprintf(path, sizeof(path), "%s/libmkframework.so", dirname(dir));
    LOG("loading %s", path);
    g_darwin_exe_path = "/private/var/containers/Bundle/Application/reprovisioned.app/reprovisioned";
    g_darwin_image = &g_img;
    if (mk_load(path, darwin_resolve, &g_img)) { LOG("failed to load UnityFramework"); abort(); }
    darwin_init(&g_img);
    darwin_cxx_init(&g_img);
    mk_register_unwind(&g_img);
    install_crash_logger();
    LOG("UnityFramework at %p; running %llu initializers", g_img.base, (unsigned long long)g_img.init_count);
    mk_run_initializers(&g_img, NULL);
    patch_image();
    unsigned n = 0, missing = 0;
    for (; mk_api_names[n]; n++) {
        mk_api_slots[n] = mk_export(&g_img, mk_api_names[n]);
        if (!mk_api_slots[n]) { LOG("missing export %s", mk_api_names[n]); missing++; }
    }
    LOG("forwarding %u il2cpp_* functions (%u missing)", n, missing);
}

extern void real_il2cpp_set_data_dir(const char *);
extern void real_il2cpp_set_config_dir(const char *);
extern int real_il2cpp_init(const char *);

__attribute__((visibility("default"))) void il2cpp_set_data_dir(const char *d) {
    LOG("il2cpp_set_data_dir(%s)", d);
    // <external files>/il2cpp -> <external files>
    snprintf(g_external_files, sizeof(g_external_files), "%s", d);
    char *slash = strrchr(g_external_files, '/');
    if (slash) *slash = 0;
    real_il2cpp_set_data_dir(d);
}
__attribute__((visibility("default"))) void il2cpp_set_config_dir(const char *d) {
    LOG("il2cpp_set_config_dir(%s)", d);
    real_il2cpp_set_config_dir(d);
}
static void on_throw(void *obj, void *tinfo);
static void register_ios_icalls(void);
static void adapt_icalls(void);
static void record_icall(const char *, const void *);
static void dump_class(const char *, const char *);
static void patch_image(void);
extern void (*mk_throw_hook)(void *, void *);
__attribute__((visibility("default"))) int il2cpp_init(const char *domain) {
    LOG("il2cpp_init(%s)", domain);
    int r = real_il2cpp_init(domain);
    LOG("il2cpp_init -> %d", r);
    mk_throw_hook = on_throw;
    register_ios_icalls();
    adapt_icalls();
    if (getenv("MK_DUMP")) dump_class("GameCenter", "GameCenter");
    return r;
}

// --- diagnostics for calls the engine makes with missing (stripped) methods ---
static _Unwind_Reason_Code bt_cb(struct _Unwind_Context *ctx, void *arg) {
    int *n = arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    char tag[16];
    snprintf(tag, sizeof(tag), "bt%d", (*n)++);
    log_addr(tag, pc);
    return *n < 24 ? _URC_NO_REASON : _URC_END_OF_STACK;
}
void mk_log_backtrace(const char *why) {
    LOG("backtrace: %s", why);
    int n = 0;
    _Unwind_Backtrace(bt_cb, &n);
}

extern uint32_t real_il2cpp_method_get_param_count(const void *);
__attribute__((visibility("default"))) uint32_t il2cpp_method_get_param_count(const void *m) {
    if (!m) {
        static int logged;
        if (logged++ < 1) mk_log_backtrace("il2cpp_method_get_param_count(NULL)");
        return 0;
    }
    return real_il2cpp_method_get_param_count(m);
}

// The iOS build strips UnityAnalyticsModule, but the Android engine still
// calls into it (AnalyticsSessionInfo.CallSessionStateChanged...). Treat
// invokes of missing methods as no-ops.
extern void *real_il2cpp_runtime_invoke(const void *, void *, void **, void **);
extern void *real_il2cpp_runtime_invoke_convert_args(const void *, void *, void **, int, void **);
static void trace_invoke(const void *m, void **params);
static void watch_object(const void *m, void *obj);
__attribute__((visibility("default"))) void *il2cpp_runtime_invoke(const void *m, void *obj, void **params, void **exc) {
    if (!m) { if (exc) *exc = NULL; return NULL; }
    trace_invoke(m, params);
    if (obj) watch_object(m, obj);
    return real_il2cpp_runtime_invoke(m, obj, params, exc);
}
__attribute__((visibility("default"))) void *il2cpp_runtime_invoke_convert_args(const void *m, void *obj, void **params,
                                                                               int n, void **exc) {
    if (!m) { if (exc) *exc = NULL; return NULL; }
    return real_il2cpp_runtime_invoke_convert_args(m, obj, params, n, exc);
}

// ---- icall overrides ----
// The engine registers its icalls through il2cpp_add_internal_call; we swap a
// few for our own versions.
// g_external_files: /storage/emulated/0/Android/data/<pkg>/files
extern void *il2cpp_string_new(const char *);
extern void real_il2cpp_add_internal_call(const char *, const void *);

// StreamingAssets live on external storage as plain files (3.3 GB of
// AssetBundles + FMOD banks), so FMOD's own file I/O can open them too.
static void *icall_get_streamingAssetsPath(void) {
    char p[600];
    snprintf(p, sizeof(p), "%s/Raw", g_external_files);
    return il2cpp_string_new(p);
}

static const struct { const char *name; const void *fn; } g_icall_overrides[] = {
    { "UnityEngine.Application::get_streamingAssetsPath", icall_get_streamingAssetsPath },
    { "UnityEngine.Application::get_streamingAssetsPath()", icall_get_streamingAssetsPath },
};

__attribute__((visibility("default"))) void il2cpp_add_internal_call(const char *name, const void *fn) {
    for (size_t i = 0; i < sizeof(g_icall_overrides) / sizeof(g_icall_overrides[0]); i++)
        if (!strcmp(name, g_icall_overrides[i].name)) { LOG("icall override: %s", name); fn = g_icall_overrides[i].fn; }
    real_il2cpp_add_internal_call(name, fn);
    record_icall(name, fn);
}

// ---- managed exception logging ----
// Il2CppExceptionWrapper { Il2CppException *ex }; System.Exception: _className @0x10, _message @0x18.
extern void (*mk_throw_hook)(void *, void *);
extern void *il2cpp_domain_get(void);
extern const void **il2cpp_domain_get_assemblies(const void *, size_t *);
extern const void *il2cpp_assembly_get_image(const void *);
extern size_t il2cpp_image_get_class_count(const void *);
extern void *il2cpp_image_get_class(const void *, size_t);
extern const void *il2cpp_class_get_methods(void *, void **);
extern const char *il2cpp_method_get_name(const void *);
extern const char *il2cpp_class_get_name(void *);
extern const char *il2cpp_class_get_namespace(void *);
extern void *il2cpp_object_get_class(void *);

typedef struct { uintptr_t addr; void *klass; const char *name; } method_ent;
static method_ent *g_methods;
static size_t g_nmethods;

static int cmp_ent(const void *a, const void *b) {
    uintptr_t x = ((const method_ent *)a)->addr, y = ((const method_ent *)b)->addr;
    return (x > y) - (x < y);
}

extern void *il2cpp_class_get_nested_types(void *, void **);
static size_t g_mcap;
static void add_class_methods(void *k, int depth) {
    void *it = NULL;
    const void *m;
    while ((m = il2cpp_class_get_methods(k, &it))) {
        uintptr_t p = *(const uintptr_t *)m;  // MethodInfo::methodPointer
        if (!p) continue;
        if (g_nmethods == g_mcap) g_methods = realloc(g_methods, (g_mcap *= 2) * sizeof(method_ent));
        g_methods[g_nmethods++] = (method_ent){ p, k, il2cpp_method_get_name(m) };
    }
    void *nit = NULL, *n;
    while (depth < 8 && (n = il2cpp_class_get_nested_types(k, &nit))) add_class_methods(n, depth + 1);
}

static void build_method_table(void) {
    size_t na = 0;
    const void **as = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &na);
    g_mcap = 1 << 16;
    g_methods = malloc(g_mcap * sizeof(method_ent));
    for (size_t i = 0; i < na; i++) {
        const void *img = il2cpp_assembly_get_image(as[i]);
        size_t nc = il2cpp_image_get_class_count(img);
        for (size_t c = 0; c < nc; c++) {
            void *k = il2cpp_image_get_class(img, c);
            if (k) add_class_methods(k, 0);
        }
    }
    qsort(g_methods, g_nmethods, sizeof(method_ent), cmp_ent);
    LOG("method table: %zu methods", g_nmethods);
    char path[600];
    snprintf(path, sizeof(path), "%s/methods.txt", g_external_files);
    FILE *f = fopen(path, "w");
    if (f) {
        for (size_t i = 0; i < g_nmethods; i++)
            fprintf(f, "%lx %s.%s::%s\n", (unsigned long)(g_methods[i].addr - (uintptr_t)g_img.base),
                    il2cpp_class_get_namespace(g_methods[i].klass), il2cpp_class_get_name(g_methods[i].klass),
                    g_methods[i].name);
        fclose(f);
    }
}

static void managed_name(uintptr_t pc, char *out, size_t n) {
    size_t lo = 0, hi = g_nmethods;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (g_methods[mid].addr <= pc) lo = mid + 1; else hi = mid; }
    if (!lo) { snprintf(out, n, "?"); return; }
    const method_ent *e = &g_methods[lo - 1];
    snprintf(out, n, "%s.%s::%s+%#lx", il2cpp_class_get_namespace(e->klass), il2cpp_class_get_name(e->klass), e->name,
             (unsigned long)(pc - e->addr));
}

static void utf16_to_ascii(const void *str, char *out, size_t n) {
    if (!str) { snprintf(out, n, "(null)"); return; }
    int len = *(const int *)((const char *)str + 16);
    const uint16_t *ch = (const uint16_t *)((const char *)str + 20);
    size_t k = 0;
    for (; k + 1 < n && (int)k < len; k++) out[k] = ch[k] < 128 ? (char)ch[k] : '?';
    out[k] = 0;
}

#include <sys/uio.h>
static int safe_read(uintptr_t addr, void *out, size_t n) {
    struct iovec local = { out, n }, remote = { (void *)addr, n };
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
}
// If v points at an Il2CppObject, write "Namespace.Class" into out.
static int object_class_name(uintptr_t v, char *out, size_t n) {
    uintptr_t klass, name, ns;
    char a[64] = {0}, b[64] = {0};
    if (!v || (v & 7) || !safe_read(v, &klass, 8) || !klass || (klass & 7)) return 0;
    if (!safe_read(klass + 0x10, &name, 8) || !safe_read(klass + 0x18, &ns, 8)) return 0;
    if (!safe_read(name, a, 63) || !safe_read(ns, b, 63)) return 0;
    for (int i = 0; a[i]; i++) if (a[i] < 32 || a[i] > 126) return 0;
    for (int i = 0; b[i]; i++) if (b[i] < 32 || b[i] > 126) return 0;
    if (!a[0]) return 0;
    snprintf(out, n, "%s%s%s", b, b[0] ? "." : "", a);
    return 1;
}

static _Unwind_Reason_Code exc_bt_cb(struct _Unwind_Context *ctx, void *arg) {
    int *n = arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if ((*n)++ == 0) return _URC_NO_REASON;  // the hook itself
    char buf[300];
    if (pc >= (uintptr_t)g_img.base && pc < (uintptr_t)g_img.base + g_img.size) {
        managed_name(pc - 4, buf, sizeof(buf));
        LOG("    at %s  (UnityFramework+%#lx)", buf, (unsigned long)(pc - (uintptr_t)g_img.base));
        char objs[512] = "", cn[160];
        for (int r = 19; r <= 28; r++) {
            uintptr_t v = _Unwind_GetGR(ctx, r);
            if (!object_class_name(v, cn, sizeof(cn))) continue;
            if (!strcmp(cn, "System.String")) {  // show the string's value instead
                int len = 0;
                uint16_t ch[48];
                char sv[52];
                if (safe_read(v + 16, &len, 4) && len > 0) {
                    int k = len < 48 ? len : 48;
                    if (safe_read(v + 20, ch, k * 2)) {
                        for (int q = 0; q < k; q++) sv[q] = ch[q] < 128 && ch[q] >= 32 ? (char)ch[q] : '?';
                        sv[k] = 0;
                        snprintf(cn, sizeof(cn), "\"%s\"", sv);
                    }
                }
            }
            if (strlen(objs) + strlen(cn) + 8 < sizeof(objs)) {
                char t[180];
                snprintf(t, sizeof(t), " x%d=%s", r, cn);
                strcat(objs, t);
            }
        }
        if (objs[0]) LOG("       regs:%s", objs);
    } else {
        Dl_info di;
        if (dladdr((void *)pc, &di) && di.dli_fname)
            LOG("    at %s+%#lx", strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname,
                (unsigned long)(pc - (uintptr_t)di.dli_fbase));
    }
    return *n < 18 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

static void on_throw(void *obj, void *tinfo) {
    (void)tinfo;
    static int count;
    if (count >= 300) return;
    void *ex = *(void **)obj;
    if (!ex || (uintptr_t)ex < 0x10000) return;
    if (!g_methods) build_method_table();
    void *k = il2cpp_object_get_class(ex);
    char msg[200];
    utf16_to_ascii(*(void **)((char *)ex + 0x18), msg, sizeof(msg));
    LOG("managed throw #%d: %s.%s: %s", ++count, il2cpp_class_get_namespace(k), il2cpp_class_get_name(k), msg);
    int n = 0;
    _Unwind_Backtrace(exc_bt_cb, &n);
}

// ---- patches inside UnityFramework ----
// Replace a 16-byte exported stub with `ldr x16, #8; br x16; .quad target`.
static void redirect_image_function(uintptr_t off, void *target, const char *what) {
    uint32_t *at = (uint32_t *)(g_img.base + off);
    long pg = sysconf(_SC_PAGESIZE);
    void *page = (void *)((uintptr_t)at & ~(uintptr_t)(pg - 1));
    mprotect(page, 2 * pg, PROT_READ | PROT_WRITE | PROT_EXEC);
    at[0] = 0x58000050;  // ldr x16, #8
    at[1] = 0xd61f0200;  // br x16
    *(uint64_t *)(at + 2) = (uint64_t)(uintptr_t)target;
    mprotect(page, 2 * pg, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)at, (char *)(at + 4));
    LOG("redirected %s", what);
}

// FMOD: iOS output goes through AudioUnit/AVAudioSession, which we don't provide yet.
// Start FMOD Studio with the no-sound output so the game's audio calls succeed.
typedef int (*studio_init_fn)(void *, int, unsigned, unsigned, void *);
static studio_init_fn g_studio_init_impl;
static int studio_initialize_hook(void *studio, int maxch, unsigned sflags, unsigned flags, void *extra) {
    int (*get_core)(void *, void **) = mk_export(&g_img, "_FMOD_Studio_System_GetCoreSystem");
    int (*set_output)(void *, int) = mk_export(&g_img, "_FMOD5_System_SetOutput");
    void *core = NULL;
    int r1 = get_core(studio, &core), r2 = core ? set_output(core, 2 /* FMOD_OUTPUTTYPE_NOSOUND */) : -1;
    int r = g_studio_init_impl(studio, maxch, sflags, flags, extra);
    LOG("FMOD Studio initialize with NOSOUND output: core=%d setOutput=%d init=%d", r1, r2, r);
    return r;
}

static void patch_cloudkit(void);
static void patch_gamecenter(void);
static void patch_image(void) {
    patch_cloudkit();
    patch_gamecenter();
    // FMOD_Studio_System_Initialize: stp/mov/ldp/b <impl> (UnityFramework 2.3.1)
    uintptr_t off = (uintptr_t)mk_export(&g_img, "_FMOD_Studio_System_Initialize") - (uintptr_t)g_img.base;
    uint32_t *p = (uint32_t *)(g_img.base + off);
    if (p[0] == 0xa9bf7bfd && (p[3] & 0xfc000000) == 0x14000000) {
        int32_t imm = (int32_t)(p[3] << 6) >> 6;
        g_studio_init_impl = (studio_init_fn)(uintptr_t)((uintptr_t)(p + 3) + (intptr_t)imm * 4);
        redirect_image_function(off, studio_initialize_hook, "FMOD_Studio_System_Initialize");
    } else {
        LOG("FMOD_Studio_System_Initialize has an unexpected prologue; not patched");
    }
}

// ---- iOS-only icalls the Android engine does not register ----
static int icall_ios_generation(void) { return 10002; /* DeviceGeneration.iPadUnknown */ }
static void *icall_ios_system_version(void) { return il2cpp_string_new("14.4"); }
static void icall_noop(void) {}
static int icall_false(void) { return 0; }
static int icall_true(void) { return 1; }
static void *icall_null(void) { return NULL; }
static void *icall_player_name(void) { return il2cpp_string_new("Player"); }
static void *icall_player_id(void) { return il2cpp_string_new("G:0000000000"); }

static void register_ios_icalls(void) {
    static const struct { const char *name; const void *fn; } calls[] = {
        { "UnityEngine.iOS.Device::get_generation", icall_ios_generation },
        { "UnityEngine.iOS.Device::get_systemVersion", icall_ios_system_version },
        { "UnityEngine.UnhandledExceptionHandler::iOSNativeUnhandledExceptionHandler", icall_noop },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::Authenticate", icall_noop },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::GetAuthenticated", icall_true },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::Internal_UserName", icall_player_name },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::Internal_UserID", icall_player_id },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::Internal_UserGameID", icall_player_id },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::Internal_LegacyUserID", icall_player_id },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::GetIsUnderage", icall_false },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::GetUserImage", icall_null },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::LoadFriends", icall_noop },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::InternalLoadAchievementDescriptions", icall_noop },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::InternalLoadAchievements", icall_noop },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::InternalReportProgress", icall_noop },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::InternalReportScore", icall_noop },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::InternalLoadScores", icall_noop },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::Internal_ShowAchievementsUI", icall_noop },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::Internal_ShowLeaderboardUI", icall_noop },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::Internal_LoadUsers", icall_noop },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::ResetAllAchievements", icall_noop },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::ShowDefaultAchievementBanner", icall_noop },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::ShowSpecificLeaderboardUI", icall_noop },
    };
    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) real_il2cpp_add_internal_call(calls[i].name, calls[i].fn);
    LOG("registered %zu iOS-only icalls", sizeof(calls) / sizeof(calls[0]));
}

// ---- reflection helpers for replacing game P/Invoke wrappers ----
extern void *il2cpp_class_from_name(const void *, const char *, const char *);
extern uint32_t real_il2cpp_method_get_param_count(const void *);
extern const void *il2cpp_method_get_param(const void *, uint32_t);
extern const void *il2cpp_method_get_return_type(const void *);
extern char *il2cpp_type_get_name(const void *);
extern uint32_t il2cpp_method_get_flags(const void *, uint32_t *);

static void *find_class(const char *ns, const char *name) {
    size_t na = 0;
    const void **as = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &na);
    for (size_t i = 0; i < na; i++) {
        void *k = il2cpp_class_from_name(il2cpp_assembly_get_image(as[i]), ns, name);
        if (k) return k;
    }
    return NULL;
}

static void dump_class(const char *ns, const char *name) {
    void *k = find_class(ns, name), *it = NULL;
    const void *m;
    if (!k) { LOG("class %s.%s not found", ns, name); return; }
    while ((m = il2cpp_class_get_methods(k, &it))) {
        char sig[400];
        uint32_t iflags = 0, flags = il2cpp_method_get_flags(m, &iflags);
        int n = snprintf(sig, sizeof(sig), "%s%s %s(", (flags & 0x10) ? "static " : "",
                         il2cpp_type_get_name(il2cpp_method_get_return_type(m)), il2cpp_method_get_name(m));
        uint32_t pc = real_il2cpp_method_get_param_count(m);
        for (uint32_t i = 0; i < pc && n < (int)sizeof(sig) - 60; i++)
            n += snprintf(sig + n, sizeof(sig) - n, "%s%s", i ? ", " : "", il2cpp_type_get_name(il2cpp_method_get_param(m, i)));
        LOG("  %s.%s: %s)  ptr=UnityFramework+%#lx flags=%#x", ns, name, sig,
            (unsigned long)(*(const uintptr_t *)m - (uintptr_t)g_img.base), flags);
    }
}

// ---- CloudKit plugin replacement: "iCloud" records as local files ----
// Native side of CloudKit.CloudKitInterface (DllImport "__Internal"), offsets in UnityFramework 2.3.1.
// Callback table order follows the C# EventHandlers struct.
typedef struct {
    void (*account_status_changed)(void);
    void (*fetch_succeeded)(const void *data, int len, const char *record, const char *tag);
    void (*fetch_failed)(const char *record, int code, const char *desc, const char *tag);
    void (*save_succeeded)(const char *record, const char *tag);
    void (*save_requires_merge)(const void *data, int len, const char *record, const char *tag);
    void (*save_failed)(const char *record, int code, const char *desc, const char *tag);
} ck_handlers;
static ck_handlers g_ck;

static void ck_path(const char *record, char *out, size_t n) {
    char safe[256];
    size_t k = 0;
    for (; record && record[k] && k < sizeof(safe) - 1; k++)
        safe[k] = (record[k] == '/' || record[k] == 0x5c || record[k] == ':') ? '_' : record[k];
    safe[k] = 0;
    snprintf(out, n, "%s/icloud/%s.bin", g_external_files, safe);
}

static void ck_initialize(const ck_handlers *h, const char *container) {
    g_ck = *h;
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/icloud", g_external_files);
    mkdir(dir, 0700);
    LOG("CloudKit(local) initialize container=%s", container ? container : "(null)");
    if (g_ck.account_status_changed) g_ck.account_status_changed();
}
static int ck_account_status(void) { return 1; /* CKAccountStatusAvailable */ }
static const char *ck_get_auth_error(void) { return strdup(""); }
static void ck_shutdown(void) {}
static void ck_fetch_record(const char *record) {
    char path[700];
    ck_path(record, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG("CloudKit(local) fetch %s: no record", record);
        if (g_ck.fetch_failed) g_ck.fetch_failed(record, 11 /* CKErrorUnknownItem */, "Record not found", "");
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(n > 0 ? n : 1);
    n = (long)fread(buf, 1, n, f);
    fclose(f);
    LOG("CloudKit(local) fetch %s: %ld bytes", record, n);
    if (g_ck.fetch_succeeded) g_ck.fetch_succeeded(buf, (int)n, record, "local");
    free(buf);
}
static void ck_save_record(const void *data, int len, const char *record) {
    char path[700], tmp[720];
    ck_path(record, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(data, 1, len, f) == (size_t)len;
    if (f) fclose(f);
    ok = ok && rename(tmp, path) == 0;
    LOG("CloudKit(local) save %s: %d bytes %s", record, len, ok ? "ok" : "FAILED");
    if (ok && g_ck.save_succeeded) g_ck.save_succeeded(record, "local");
    else if (!ok && g_ck.save_failed) g_ck.save_failed(record, 1, "local write failed", "");
}
static void ck_reverse_save_record(const char *record) { LOG("CloudKit(local) reverse save %s ignored", record); }
static void ck_show_alert(const char *a, const char *b, const char *c) {
    LOG("CloudKit(local) alert: %s / %s / %s", a ? a : "", b ? b : "", c ? c : "");
}

static void patch_cloudkit(void) {
    static const struct { uintptr_t off; void *fn; const char *name; } fns[] = {
        { 0x1fea9c, ck_initialize, "_CKInitialize" }, { 0x1fea08, ck_get_auth_error, "_GetAuthError" },
        { 0x1feb38, ck_shutdown, "_CKShutdown" }, { 0x1feb80, ck_account_status, "_CKAccountStatus" },
        { 0x1febd4, ck_fetch_record, "_CKFetchRecord" }, { 0x1fec44, ck_save_record, "_CKSaveRecord" },
        { 0x1fecf4, ck_reverse_save_record, "_CKReverseSaveRecord" }, { 0x1fed64, ck_show_alert, "_CKShowAlert" },
    };
    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        // every one of these starts with a frame setup; refuse to patch anything unexpected
        uint32_t w = *(uint32_t *)(g_img.base + fns[i].off);
        if ((w & 0xffc003ff) != 0xa98003fd && (w & 0xff0003ff) != 0xd10003ff && (w & 0xffc07fff) != 0xa9807bfd &&
            (w & 0xffc003e0) != 0xa98003e0) {
            LOG("CloudKit %s: unexpected first instruction %#x; not patched", fns[i].name, w);
            continue;
        }
        redirect_image_function(fns[i].off, fns[i].fn, fns[i].name);
    }
}

// ---- trace: first time the engine calls each managed method / steps each coroutine type ----
extern void *il2cpp_method_get_class(const void *);
static void trace_invoke(const void *m, void **params) {
    static uintptr_t seen[4096];
    static int nseen;
    static struct timespec t0;
    if (!t0.tv_sec) clock_gettime(CLOCK_MONOTONIC, &t0);
    const char *mn = il2cpp_method_get_name(m);
    void *k = il2cpp_method_get_class(m);
    uintptr_t key = (uintptr_t)m;
    char extra[160] = "";
    if (mn && !strcmp(mn, "InvokeMoveNext") && params && params[0]) {
        void *ek = il2cpp_object_get_class(params[0]);
        key = (uintptr_t)ek;
        snprintf(extra, sizeof(extra), " enumerator=%s.%s", il2cpp_class_get_namespace(ek), il2cpp_class_get_name(ek));
    }
    for (int i = 0; i < nseen; i++) if (seen[i] == key) return;
    if (nseen < 4096) seen[nseen++] = key;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    LOG("invoke +%.1fs %s.%s::%s%s", (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9,
        il2cpp_class_get_namespace(k), il2cpp_class_get_name(k), mn, extra);
}

// ---- periodically dump the fields of objects whose class is listed in MK_WATCH-like table ----
extern void *il2cpp_class_get_fields(void *, void **);
extern const char *il2cpp_field_get_name(void *);
extern const void *il2cpp_field_get_type(void *);
extern size_t il2cpp_field_get_offset(void *);
extern int il2cpp_type_get_type(const void *);
extern int il2cpp_field_get_flags(void *);
extern void *il2cpp_class_get_parent(void *);

static void dump_fields(void *obj) {
    char line[900];
    int n = 0;
    for (void *k = il2cpp_object_get_class(obj); k; k = il2cpp_class_get_parent(k)) {
        const char *cn = il2cpp_class_get_name(k);
        if (!strcmp(cn, "MonoBehaviour") || !strcmp(cn, "Object")) break;
        void *it = NULL, *f;
        while ((f = il2cpp_class_get_fields(k, &it))) {
            if (il2cpp_field_get_flags(f) & 0x10) continue;  // static
            const void *t = il2cpp_field_get_type(f);
            int tt = il2cpp_type_get_type(t);
            char *p = (char *)obj + il2cpp_field_get_offset(f);
            char v[100];
            switch (tt) {
            case 2: snprintf(v, sizeof(v), "%s", *(uint8_t *)p ? "true" : "false"); break;   // bool
            case 8: case 9: case 0x55: snprintf(v, sizeof(v), "%d", *(int *)p); break;         // int/uint/enum
            case 0xc: snprintf(v, sizeof(v), "%g", *(float *)p); break;                      // float
            case 0x12: case 0x1d: case 0x15: case 0xe: {                                    // class/array/generic/string
                void *o = *(void **)p;
                char cn2[160];
                if (!o) snprintf(v, sizeof(v), "null");
                else if (object_class_name((uintptr_t)o, cn2, sizeof(cn2))) snprintf(v, sizeof(v), "%s", cn2);
                else snprintf(v, sizeof(v), "%p", o);
                break;
            }
            default: continue;
            }
            n += snprintf(line + n, sizeof(line) - n, "%s=%s ", il2cpp_field_get_name(f), v);
            if (n > 300) { LOG("    %s", line); n = 0; }
        }
    }
    if (n) LOG("    %s", line);
}

extern const void *il2cpp_class_get_method_from_name(void *, const char *, int);
extern void *il2cpp_object_unbox(void *);
static int invoke_int(const char *ns, const char *cls, const char *meth) {
    void *k = find_class(ns, cls);
    const void *mm = k ? il2cpp_class_get_method_from_name(k, meth, 0) : NULL;
    if (!mm) return -999;
    void *exc = NULL;
    void *r = real_il2cpp_runtime_invoke(mm, NULL, NULL, &exc);
    if (exc || !r) return -998;
    return *(int *)il2cpp_object_unbox(r);
}

static void watch_object(const void *m, void *obj) {
    static const char *watched[] = { "BootstrapTitleScreen", "Bootstrap", 0 };
    static unsigned calls;
    const char *mn = il2cpp_method_get_name(m);
    if (!mn || strcmp(mn, "Update")) return;
    void *k = il2cpp_method_get_class(m);
    const char *cn = il2cpp_class_get_name(k);
    for (int i = 0; watched[i]; i++) {
        if (!strcmp(cn, watched[i])) {
            int tc = invoke_int("UnityEngine", "Input", "get_touchCount");
            int ak = 0;
            if (tc || ak) LOG("input: touchCount=%d anyKey=%d (call %u)", tc, ak, calls);
        }
        if (!strcmp(cn, watched[i]) && (calls++ % 600) == 0) {
            LOG("watch %s (call %u): isFocused=%d", cn, calls, invoke_int("UnityEngine", "Application", "get_isFocused") & 0xff);
            dump_fields(obj);
        }
    }
}

// ---- GameCenter plugin replacement: an offline, signed-in local player ----
// Native side of GameCenter.GameCenter (game's own plugin, DllImport "__Internal").
// _GCInitialize copies a table of 19 callbacks; the first is OnAuthentication(bool ok, const char *error).
typedef struct { const char *id, *name; } gc_player;  // returned in x0/x1
static void (*g_gc_on_auth)(int ok, const char *error);

static void gc_initialize(void *const *handlers) {
    g_gc_on_auth = (void (*)(int, const char *))handlers[0];
    LOG("GameCenter(local) initialize: reporting an authenticated player");
    if (g_gc_on_auth) g_gc_on_auth(1, NULL);
}
static void gc_noop(void) {}
static gc_player gc_local_player(void) { gc_player p = { strdup("G:0000000001"), strdup("Player") }; return p; }
static int gc_true(void) { return 1; }
static int gc_false(void) { return 0; }

static void patch_gamecenter(void) {
    static const struct { uintptr_t off; void *fn; const char *name; } fns[] = {
        { 0x209a68, gc_initialize, "_GCInitialize" }, { 0x209af4, gc_noop, "_GCTimeoutInitialize" },
        { 0x209b3c, gc_noop, "_GCShutdown" }, { 0x209b84, gc_local_player, "_GCLocalPlayer" },
        { 0x209be8, gc_true, "_GCIsAuthenticated" }, { 0x209c3c, gc_true, "_GCIsMultiplayerRestricted" },
        { 0x209ce0, gc_false, "_GCIsUnderage" }, { 0x209f64, gc_false, "_GCCanShowAccessPoint" },
        { 0x20a0c0, gc_false, "_GCIsShowingDashboard" }, { 0x20a020, gc_noop, "_GCShowAccessPoint" },
        { 0x20a070, gc_noop, "_GCSetAccessPointFocus" }, { 0x20a658, gc_noop, "_GCReportAchievement" },
    };
    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) redirect_image_function(fns[i].off, fns[i].fn, fns[i].name);
}

// ---- icall ABI adapters (Apple caller -> Android libunity callee) ----
// Apple arm64 packs stack arguments by natural size and expects small return values
// extended by the callee; AAPCS64 uses 8-byte stack slots and leaves upper bits undefined.
// Registrations arrive before il2cpp_init, so they are recorded and fixed up afterwards.
extern void mk_icall_adapter(void);
extern int il2cpp_type_is_byref(const void *);
extern void *il2cpp_class_from_type(const void *);
extern int il2cpp_class_is_enum(void *);
extern const void *il2cpp_class_enum_basetype(void *);

typedef struct { char *name; const void *fn; } icall_rec;
static icall_rec *g_icalls;
static size_t g_nicalls, g_icap;

static void record_icall(const char *name, const void *fn) {
    if (g_nicalls == g_icap) g_icalls = realloc(g_icalls, (g_icap = g_icap ? g_icap * 2 : 1024) * sizeof(icall_rec));
    g_icalls[g_nicalls++] = (icall_rec){ strdup(name), fn };
}

static void *find_class_path(const char *full) {  // "Ns.Outer/Inner"
    char buf[300];
    snprintf(buf, sizeof(buf), "%s", full);
    char *slash = strchr(buf, '/');
    if (slash) *slash = 0;
    char *dot = strrchr(buf, '.');
    void *k;
    if (dot) { *dot = 0; k = find_class(buf, dot + 1); } else k = find_class("", buf);
    while (k && slash) {
        char *name = slash + 1;
        slash = strchr(name, '/');
        if (slash) *slash = 0;
        void *it = NULL, *n, *found = NULL;
        while ((n = il2cpp_class_get_nested_types(k, &it)))
            if (!strcmp(il2cpp_class_get_name(n), name)) { found = n; break; }
        k = found;
    }
    return k;
}

// Size of one argument (1, 2, 4, 8); *fp = 1 for float/double; 0 = unsupported (by-value struct).
static int arg_kind(const void *t, int *fp) {
    *fp = 0;
    if (il2cpp_type_is_byref(t)) return 8;
    switch (il2cpp_type_get_type(t)) {
    case 2: case 4: case 5: return 1;
    case 3: case 6: case 7: return 2;
    case 8: case 9: return 4;
    case 0xa: case 0xb: case 0xe: case 0xf: case 0x12: case 0x14: case 0x15: case 0x18: case 0x19:
    case 0x1b: case 0x1c: case 0x1d: return 8;
    case 0xc: *fp = 1; return 4;
    case 0xd: *fp = 1; return 8;
    case 0x11: {
        void *k = il2cpp_class_from_type(t);
        if (k && il2cpp_class_is_enum(k)) return arg_kind(il2cpp_class_enum_basetype(k), fp);
        return 0;
    }
    default: return 0;
    }
}

static void *make_adapter(const char *name, const void *fn, unsigned *n_stack) {
    char cls[300], meth[200];
    const char *sep = strstr(name, "::");
    if (!sep || sep - name >= (long)sizeof(cls)) return NULL;
    memcpy(cls, name, sep - name);
    cls[sep - name] = 0;
    snprintf(meth, sizeof(meth), "%s", sep + 2);
    char *paren = strchr(meth, '(');
    if (paren) *paren = 0;
    void *k = find_class_path(cls);
    const void *m = k ? il2cpp_class_get_method_from_name(k, meth, -1) : NULL;
    if (!m) return NULL;
    uint32_t iflags = 0, flags = il2cpp_method_get_flags(m, &iflags);
    int ngr = (flags & 0x10) ? 0 : 1, nsr = 0, apple_off = 0, aapcs_off = 0, n = 0;
    uint16_t ent[64][4];
    uint32_t pc = real_il2cpp_method_get_param_count(m);
    for (uint32_t i = 0; i < pc; i++) {
        int fp, sz = arg_kind(il2cpp_method_get_param(m, i), &fp);
        if (!sz) return NULL;
        if (fp ? nsr < 8 : ngr < 8) { if (fp) nsr++; else ngr++; continue; }
        if (n == 64) return NULL;
        apple_off = (apple_off + sz - 1) & ~(sz - 1);
        ent[n][0] = (uint16_t)apple_off; ent[n][1] = (uint16_t)aapcs_off; ent[n][2] = (uint16_t)sz; ent[n][3] = 0;
        n++;
        apple_off += sz;
        aapcs_off += 8;
    }
    int rfp, rsz = arg_kind(il2cpp_method_get_return_type(m), &rfp);
    int rt = il2cpp_type_get_type(il2cpp_method_get_return_type(m)), ret_ext = 0;
    if (!rfp && rsz == 1) ret_ext = (rt == 4) ? 2 : 1;
    if (!rfp && rsz == 2) ret_ext = (rt == 6) ? 4 : 3;
    int differs = 0;
    for (int i = 0; i < n; i++) if (ent[i][0] != ent[i][1]) differs = 1;
    if (!differs && !ret_ext) return NULL;
    uint64_t *d = calloc(1, 24 + 8 * n + 8);
    d[0] = (uint64_t)(uintptr_t)fn;
    d[1] = (uint64_t)((aapcs_off + 15) & ~15);
    ((uint32_t *)d)[4] = (uint32_t)n;
    ((uint32_t *)d)[5] = (uint32_t)ret_ext;
    memcpy((char *)d + 24, ent, 8 * n);
    // stub: ldr x16, #16 ; ldr x17, #20 ; br x17 ; nop ; .quad desc ; .quad mk_icall_adapter
    static uint32_t *pool;
    static size_t used, cap;
    if (!pool || used + 8 > cap) {
        cap = 4096;
        pool = mmap(NULL, cap * 4, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        used = 0;
    }
    uint32_t *s = pool + used;
    used += 8;
    s[0] = 0x58000090;  // ldr x16, #16
    s[1] = 0x580000b1;  // ldr x17, #20
    s[2] = 0xd61f0220;  // br x17
    s[3] = 0xd503201f;  // nop
    *(uint64_t *)(s + 4) = (uint64_t)(uintptr_t)d;
    *(uint64_t *)(s + 6) = (uint64_t)(uintptr_t)mk_icall_adapter;
    __builtin___clear_cache((char *)s, (char *)(s + 8));
    if (differs && (*n_stack)++ < 40) LOG("icall adapter: %s (%d stack args, ret_ext %d)", name, n, ret_ext);
    return s;
}

static void adapt_icalls(void) {
    unsigned stack = 0, made = 0;
    for (size_t i = 0; i < g_nicalls; i++) {
        void *stub = make_adapter(g_icalls[i].name, g_icalls[i].fn, &stack);
        if (stub) { real_il2cpp_add_internal_call(g_icalls[i].name, stub); made++; }
    }
    LOG("icall adapters: %u of %zu engine icalls wrapped (%u with stack arguments)", made, g_nicalls, stack);
}
