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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
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
static void patch_image(void);
extern void (*mk_throw_hook)(void *, void *);
__attribute__((visibility("default"))) int il2cpp_init(const char *domain) {
    LOG("il2cpp_init(%s)", domain);
    int r = real_il2cpp_init(domain);
    LOG("il2cpp_init -> %d", r);
    mk_throw_hook = on_throw;
    register_ios_icalls();
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
__attribute__((visibility("default"))) void *il2cpp_runtime_invoke(const void *m, void *obj, void **params, void **exc) {
    if (!m) { if (exc) *exc = NULL; return NULL; }
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

static void patch_image(void) {
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
static void *icall_null(void) { return NULL; }
static void *icall_player_name(void) { return il2cpp_string_new("Player"); }
static void *icall_player_id(void) { return il2cpp_string_new("G:0000000000"); }

static void register_ios_icalls(void) {
    static const struct { const char *name; const void *fn; } calls[] = {
        { "UnityEngine.iOS.Device::get_generation", icall_ios_generation },
        { "UnityEngine.iOS.Device::get_systemVersion", icall_ios_system_version },
        { "UnityEngine.UnhandledExceptionHandler::iOSNativeUnhandledExceptionHandler", icall_noop },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::Authenticate", icall_noop },
        { "UnityEngine.SocialPlatforms.GameCenter.GameCenterPlatform::GetAuthenticated", icall_false },
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
