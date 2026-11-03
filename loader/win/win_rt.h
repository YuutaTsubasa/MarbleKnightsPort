// Windows runtime for the recompiled UnityFramework (win_rt.c) and its Darwin shims (darwin_win.c).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "recomp_rt.h"

typedef void (*mk_import_fn)(cpu *c);
typedef struct { mk_import_fn fn; void *data; } mk_import;  // one of them, or neither (not provided)

mk_import mk_resolve_import(const char *sym, const char *lib);  // darwin_win.c
void mk_shims_init(void);
mk_import_fn mk_sys_lookup(const char *sym);  // darwin_win_sys.c
mk_import mk_cxx_lookup(const char *sym);     // darwin_win_cxx.c
void mk_cxx_init(void);
mk_import_fn mk_libcxx_lookup(const char *sym);  // darwin_win_libcxx.c
void mk_libcxx_init(void);
const char *mk_exception_type_name(void *exception_pointers);
void mk_sys_init(void);
void mk_imp(cpu *c, int k);  // generated imp_N(c) call this
// one per host thread that runs guest code; pthread_t and the mach thread port point at it
typedef struct mk_thread {
    uint32_t tid, port;
    void *handle;              // HANDLE with suspend / get-context rights
    uint8_t *gstack;           // guest stack (GUEST_STACK bytes)
    size_t gsize;
    cpu *volatile cur;         // innermost active guest register file, NULL outside guest code
    uint64_t start_fn, start_arg, result;  // pthread_create / pthread_join
    int detached, suspended;
    uint64_t tsd[1024];        // pthread TSD slots (mrs tpidrro_el0); [0] = pthread_self
} mk_thread;
mk_thread *mk_self(void);
void mk_thread_bind(mk_thread *t);
mk_thread *mk_thread_by_port(uint32_t port);
int mk_thread_count(void);
mk_thread *mk_thread_at(int i);
mk_thread *mk_thread_new(void);  // registered, for a thread about to be created

uint64_t mk_guest_enter(uint64_t target, const uint64_t *x, int nx, const double *v, int nv, double *fret);
uint64_t mk_host_callback(void *fn, const char *what);
uint64_t mk_export(const char *name);
cpu *mk_current_cpu(void);
void mk_log(const char *fmt, ...);
void mk_fatal(const char *fmt, ...);

// the allocator for memory the guest may free (darwin_win.c)
void *mk_malloc(size_t n);
void *mk_malloc_aligned(size_t n, size_t align);
void mk_free(void *p);
char *mk_strdup(const char *s);

// shim helpers: arguments and results in guest registers
#define X(i) (c->x[i])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define GSP ((uint8_t *)(uintptr_t)c->x[31])  // stack-passed (variadic) arguments start here
