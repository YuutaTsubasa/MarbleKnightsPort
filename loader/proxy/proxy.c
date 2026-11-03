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
__attribute__((visibility("default"))) int il2cpp_init(const char *domain) {
    LOG("il2cpp_init(%s)", domain);
    int r = real_il2cpp_init(domain);
    LOG("il2cpp_init -> %d", r);
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
