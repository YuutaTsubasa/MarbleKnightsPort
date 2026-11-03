// Milestone 1 probe: load the iOS UnityFramework on Android, run its static
// initializers, then bring up IL2CPP through the exported il2cpp_* API and
// list the assemblies from global-metadata.dat. No Unity Android player yet.
//
// usage: il2cpp_probe <root>   (root contains UnityFramework and Data/)
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include "darwin.h"

static mk_image g_img;
static sigjmp_buf g_init_jmp;
static volatile int g_in_init = -1;

static void report_addr(const char *what, uintptr_t a) {
    if (a >= (uintptr_t)g_img.base && a < (uintptr_t)g_img.base + g_img.size) {
        uintptr_t off = 0;
        const char *n = mk_symbolicate(&g_img, a, &off);
        fprintf(stderr, "  %s = image+%#lx (%s+%#lx)\n", what, (unsigned long)(a - (uintptr_t)g_img.base),
                n ? n : "?", (unsigned long)off);
    } else {
        fprintf(stderr, "  %s = %#lx\n", what, (unsigned long)a);
    }
}

static void crash_handler(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    fprintf(stderr, "*** signal %d at fault addr %p (initializer %d)\n", sig, si->si_addr, g_in_init);
    report_addr("pc", uc->uc_mcontext.pc);
    report_addr("lr", uc->uc_mcontext.regs[30]);
    for (int i = 0; i < 8; i++) fprintf(stderr, "  x%d = %#llx\n", i, (unsigned long long)uc->uc_mcontext.regs[i]);
    // walk the frame-pointer chain
    uintptr_t fp = uc->uc_mcontext.regs[29];
    for (int i = 0; i < 24 && fp; i++) {
        uintptr_t *f = (uintptr_t *)fp;
        if ((fp & 7) || fp < 0x1000) break;
        char tag[16];
        snprintf(tag, sizeof(tag), "frame%d", i);
        report_addr(tag, f[1]);
        fp = f[0];
    }
    if (getenv("MK_DUMP_GAM")) {
        // libil2cpp's s_GenericArrayMethods: vector<{std::string name; MethodInfo* m; MethodInfo* iface}>
        uintptr_t *vec = (uintptr_t *)(g_img.base + strtoull(getenv("MK_DUMP_GAM"), NULL, 16));
        for (uintptr_t e = vec[0]; e < vec[1]; e += 40) {
            const uint8_t *s = (const uint8_t *)e;
            const char *name = (s[23] & 0x80) ? *(const char **)s : (const char *)s;
            uintptr_t m = *(uintptr_t *)(e + 24), im = *(uintptr_t *)(e + 32);
            fprintf(stderr, "  GAM %-70s m=%s iface=%s\n", name, m ? *(const char **)(m + 16) : "-",
                    im ? *(const char **)(im + 16) : "(null)");
        }
    }
    if (g_in_init >= 0) siglongjmp(g_init_jmp, 1);
    _exit(128 + sig);
}

static int init_filter(mk_image *img, uint64_t vm, unsigned index) {
    (void)img;
    static const char *skip;
    if (!skip) skip = getenv("MK_INIT_SKIP") ? getenv("MK_INIT_SKIP") : "";
    char key[32];
    snprintf(key, sizeof(key), ",%u,", index);
    char padded[1024];
    snprintf(padded, sizeof(padded), ",%s,", skip);
    if (strstr(padded, key)) { fprintf(stderr, "init[%u] %#llx skipped (MK_INIT_SKIP)\n", index, (unsigned long long)vm); return 0; }
    return 1;
}

typedef void (*init_fn)(int, const char **, const char **, const char **, void *);

static void run_initializers(void) {
    uint64_t *fns = (uint64_t *)(g_img.base + g_img.init_addr);
    static const char *argv[] = { "reprovisioned", 0 }, *envp[] = { 0 }, *apple[] = { 0 };
    unsigned failed = 0;
    for (unsigned i = 0; i < g_img.init_count; i++) {
        uint64_t vm = fns[i] - (uint64_t)g_img.slide;
        if (!init_filter(&g_img, vm, i)) continue;
        g_in_init = (int)i;
        if (sigsetjmp(g_init_jmp, 1) == 0) {
            ((init_fn)(uintptr_t)fns[i])(1, argv, envp, apple, NULL);
        } else {
            fprintf(stderr, "init[%u] at image+%#llx crashed, skipped\n", i, (unsigned long long)vm);
            failed++;
        }
    }
    g_in_init = -1;
    fprintf(stderr, "ran %llu initializers, %u crashed\n", (unsigned long long)g_img.init_count, failed);
}

#define API(ret, name, args) typedef ret(*name##_t) args; static name##_t name;
API(int, il2cpp_init, (const char *))
API(void, il2cpp_set_data_dir, (const char *))
API(void, il2cpp_set_config_dir, (const char *))
API(void, il2cpp_set_commandline_arguments, (int, const char *const[], const char *))
API(void *, il2cpp_domain_get, (void))
API(const void **, il2cpp_domain_get_assemblies, (const void *, size_t *))
API(const void *, il2cpp_assembly_get_image, (const void *))
API(const char *, il2cpp_image_get_name, (const void *))
API(size_t, il2cpp_image_get_class_count, (const void *))
API(void *, il2cpp_class_from_name, (const void *, const char *, const char *))
API(const void *, il2cpp_class_get_method_from_name, (void *, const char *, int))
API(void *, il2cpp_runtime_invoke, (const void *, void *, void **, void **))
API(void *, il2cpp_string_new, (const char *))
API(void *, il2cpp_object_unbox, (void *))
API(void *, il2cpp_object_get_class, (void *))
API(const char *, il2cpp_class_get_name, (void *))
API(void, il2cpp_gc_collect, (int))
API(int64_t, il2cpp_gc_get_used_size, (void))

static void *need(const char *n) {
    void *p = mk_export(&g_img, n);
    if (!p) { fprintf(stderr, "missing export %s\n", n); exit(2); }
    return p;
}
#define LOAD(name) name = (name##_t)need("_" #name)

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : "/data/local/tmp/mk";
    setvbuf(stderr, NULL, _IONBF, 0);
    mk_trap_fatal = getenv("MK_TRAP_FATAL") != NULL;

    stack_t ss = { .ss_sp = malloc(1 << 16), .ss_size = 1 << 16 };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    char path[512], exe[512];
    snprintf(path, sizeof(path), "%s/UnityFramework", root);
    snprintf(exe, sizeof(exe), "%s/reprovisioned", root);
    g_darwin_exe_path = strdup(exe);
    g_darwin_image = &g_img;
    if (mk_load(path, darwin_resolve, &g_img)) return 1;
    darwin_init(&g_img);
    darwin_cxx_init(&g_img);
    mk_register_unwind(&g_img);
    fprintf(stderr, "loaded at %p, %llu initializers\n", g_img.base, (unsigned long long)g_img.init_count);

    if (getenv("MK_NO_INIT")) fprintf(stderr, "skipping initializers\n");
    else run_initializers();

    LOAD(il2cpp_init); LOAD(il2cpp_set_data_dir); LOAD(il2cpp_set_config_dir);
    LOAD(il2cpp_set_commandline_arguments); LOAD(il2cpp_domain_get); LOAD(il2cpp_domain_get_assemblies);
    LOAD(il2cpp_assembly_get_image); LOAD(il2cpp_image_get_name); LOAD(il2cpp_image_get_class_count);
    LOAD(il2cpp_class_from_name); LOAD(il2cpp_class_get_method_from_name); LOAD(il2cpp_runtime_invoke);
    LOAD(il2cpp_string_new); LOAD(il2cpp_object_unbox); LOAD(il2cpp_object_get_class); LOAD(il2cpp_class_get_name);
    LOAD(il2cpp_gc_collect); LOAD(il2cpp_gc_get_used_size);

    char data[512], cfg[512];
    snprintf(data, sizeof(data), "%s/Data/Managed", root);
    snprintf(cfg, sizeof(cfg), "%s/Data/Managed", root);
    const char *args[] = { exe };
    il2cpp_set_commandline_arguments(1, args, NULL);
    il2cpp_set_config_dir(cfg);
    il2cpp_set_data_dir(data);
    fprintf(stderr, "calling il2cpp_init...\n");
    int ok = il2cpp_init("IL2CPP Root Domain");
    fprintf(stderr, "il2cpp_init returned %d\n", ok);

    size_t n = 0;
    const void **as = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
    fprintf(stderr, "%zu assemblies:\n", n);
    const void *corlib = NULL;
    for (size_t i = 0; i < n; i++) {
        const void *img = il2cpp_assembly_get_image(as[i]);
        const char *name = il2cpp_image_get_name(img);
        fprintf(stderr, "  %-48s %zu classes\n", name, il2cpp_image_get_class_count(img));
        if (!strcmp(name, "mscorlib.dll")) corlib = img;
    }

    if (corlib) {
        // Run real generated code: String.Concat(string, string) and Int32.ToString().
        void *str = il2cpp_class_from_name(corlib, "System", "String");
        const void *concat = il2cpp_class_get_method_from_name(str, "Concat", 2);
        void *exc = NULL;
        void *params[2] = { il2cpp_string_new("Marble "), il2cpp_string_new("Knights") };
        void *r = concat ? il2cpp_runtime_invoke(concat, NULL, params, &exc) : NULL;
        if (r) {
            // Il2CppString: { klass, monitor, int32 length, uint16 chars[] }
            int len = *(int *)((char *)r + 16);
            const uint16_t *ch = (const uint16_t *)((char *)r + 20);
            char buf[128];
            int k = 0;
            for (; k < len && k < 127; k++) buf[k] = (char)ch[k];
            buf[k] = 0;
            fprintf(stderr, "String.Concat -> \"%s\"\n", buf);
        } else {
            fprintf(stderr, "String.Concat failed (exc=%p)\n", exc);
        }
        il2cpp_gc_collect(0);
        fprintf(stderr, "GC collect ok, used %lld bytes\n", (long long)il2cpp_gc_get_used_size());

        if (getenv("MK_TEST_EXCEPTION")) {
            // Int32.Parse("x") throws FormatException inside generated code; runtime_invoke must catch it.
            void *i32 = il2cpp_class_from_name(corlib, "System", "Int32");
            const void *parse = il2cpp_class_get_method_from_name(i32, "Parse", 1);
            void *p1[1] = { il2cpp_string_new("not a number") };
            exc = NULL;
            il2cpp_runtime_invoke(parse, NULL, p1, &exc);
            fprintf(stderr, "Int32.Parse threw: %s\n", exc ? il2cpp_class_get_name(il2cpp_object_get_class(exc)) : "(nothing)");
        }
    }
    fprintf(stderr, "probe done\n");
    _exit(0);
}
