// C++ exceptions of the recompiled UnityFramework on Windows (libc++abi replacement).
//
// The guest throws with __cxa_throw; we raise a Windows exception (MK_CXX_EXC) whose argument is
// the exception header. Translated call sites that have a landing pad run under __try / __except
// (tools/a64toc.py, MK_EH_CALL); mk_eh_filter plays the Itanium personality routine for the
// call site: catch clauses are matched against the thrown typeinfo (with the base classes of
// __si_class_type_info / __vmi_class_type_info), cleanups always run. The landing pad gets
// x0 = header ("_Unwind_Exception *") and x1 = selector, as on arm64; it then calls
// __cxa_begin_catch(x0) or, after a cleanup, _Unwind_Resume(x0), which raises again.
#define _CRT_SECURE_NO_WARNINGS
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "win_rt.h"

#define MK_CXX_EXC 0xE04D4B01u
#define HDR 128  // header in front of the thrown object

typedef struct exc_hdr {
    uint64_t ti, dtor;
    int32_t handlers, rethrown;
    struct exc_hdr *next_caught;
} exc_hdr;
#define OBJ(h) ((uint8_t *)(h) + HDR)

// typeinfo vtables (the guest's typeinfo objects point 16 bytes into them)
uint64_t mk_vt_class_ti[8], mk_vt_si_class_ti[8], mk_vt_vmi_class_ti[8], mk_vt_pointer_ti[8], mk_vt_fundamental_ti[8];
// std:: typeinfo objects the image refers to: { vtable+16, name }
typedef struct { uint64_t vt, name; } ti0;
static ti0 g_ti_exception, g_ti_bad_alloc, g_ti_bad_cast, g_ti_runtime_error, g_ti_logic_error, g_ti_length_error,
    g_ti_out_of_range, g_ti_invalid_argument, g_ti_bad_typeid, g_ti_overflow_error, g_ti_range_error;

static __declspec(thread) exc_hdr *t_caught;  // innermost caught exception (begin_catch .. end_catch)

static int ti_match(uint64_t thrown, uint64_t catch_ti, int depth) {
    if (!thrown || depth > 32) return 0;
    if (thrown == catch_ti) return 1;
    const char *tn = *(const char **)(uintptr_t)(thrown + 8), *cn = *(const char **)(uintptr_t)(catch_ti + 8);
    if (tn && cn && !strcmp(tn, cn)) return 1;
    uint64_t vt = *(uint64_t *)(uintptr_t)thrown;
    if (vt == (uint64_t)(uintptr_t)&mk_vt_si_class_ti[2]) return ti_match(*(uint64_t *)(uintptr_t)(thrown + 16), catch_ti, depth + 1);
    if (vt == (uint64_t)(uintptr_t)&mk_vt_vmi_class_ti[2]) {
        uint32_t nbases = *(uint32_t *)(uintptr_t)(thrown + 20);
        for (uint32_t i = 0; i < nbases && i < 64; i++) {
            uint64_t base = *(uint64_t *)(uintptr_t)(thrown + 24 + 16 * i);
            int64_t flags = *(int64_t *)(uintptr_t)(thrown + 32 + 16 * i);
            if ((flags & 2) && ti_match(base, catch_ti, depth + 1)) return 1;  // __public_mask
        }
    }
    return 0;
}

static uint64_t ti_ref(uint64_t r) {
    if (!r) return 0;
    if (r >> 63) return *(uint64_t *)(uintptr_t)(IMG + (r & ~(1ull << 63)));  // through the pointer slot
    return IMG + r;
}

int mk_eh_filter(void *exception_pointers, cpu *c, const mk_eh_site *site) {
    EXCEPTION_POINTERS *ep = exception_pointers;
    if (ep->ExceptionRecord->ExceptionCode != MK_CXX_EXC) return EXCEPTION_CONTINUE_SEARCH;
    exc_hdr *h = (exc_hdr *)ep->ExceptionRecord->ExceptionInformation[0];
    int cleanup = 0;
    for (int i = 0; i < site->n; i++) {
        int64_t sel = site->sel[i];
        if (sel == 0) { cleanup = 1; continue; }
        uint64_t ti = ti_ref(site->ti[i]);
        if (!ti || ti_match(h->ti, ti, 0)) {
            c->x[0] = (uint64_t)(uintptr_t)h;
            c->x[1] = (uint64_t)sel;
            return EXCEPTION_EXECUTE_HANDLER;
        }
    }
    if (!cleanup) return EXCEPTION_CONTINUE_SEARCH;
    c->x[0] = (uint64_t)(uintptr_t)h;
    c->x[1] = 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

static void raise_hdr(exc_hdr *h) {
    ULONG_PTR arg = (ULONG_PTR)h;
    RaiseException(MK_CXX_EXC, 0, 1, &arg);
}

const char *mk_exception_type_name(void *exception_pointers) {
    EXCEPTION_POINTERS *ep = exception_pointers;
    if (ep->ExceptionRecord->ExceptionCode != MK_CXX_EXC) return NULL;
    exc_hdr *h = (exc_hdr *)ep->ExceptionRecord->ExceptionInformation[0];
    return h->ti ? *(const char **)(uintptr_t)(h->ti + 8) : "?";
}

#define SHIM(name) static void s_##name(cpu *c)
#define P(i) ((void *)(uintptr_t)X(i))
SHIM(__cxa_allocate_exception) { RET(OBJ(calloc(1, HDR + X(0)))); }
SHIM(__cxa_free_exception) { free((uint8_t *)P(0) - HDR); }
SHIM(__cxa_throw) {
    exc_hdr *h = (exc_hdr *)((uint8_t *)P(0) - HDR);
    h->ti = X(1);
    h->dtor = X(2);
    raise_hdr(h);
}
SHIM(_Unwind_Resume) { raise_hdr(P(0)); }
SHIM(__cxa_begin_catch) {
    exc_hdr *h = P(0);
    if (!h) { RET(0); return; }
    if (h->handlers++ == 0) { h->next_caught = t_caught; t_caught = h; }
    h->rethrown = 0;
    RET(OBJ(h));
}
SHIM(__cxa_end_catch) {
    exc_hdr *h = t_caught;
    if (!h) return;
    if (--h->handlers > 0) return;
    t_caught = h->next_caught;
    if (h->rethrown) return;  // still in flight
    if (h->dtor) { uint64_t x[1] = { (uint64_t)(uintptr_t)OBJ(h) }; mk_guest_enter(h->dtor, x, 1, NULL, 0, NULL); }
    free(h);
}
SHIM(__cxa_rethrow) {
    exc_hdr *h = t_caught;
    if (!h) mk_fatal("__cxa_rethrow with no exception");
    h->rethrown = 1;
    h->handlers = 0;
    t_caught = h->next_caught;
    raise_hdr(h);
}
SHIM(__cxa_get_exception_ptr) { RET(OBJ(P(0))); }
SHIM(__cxa_current_exception_type) { RET(t_caught ? t_caught->ti : 0); }
SHIM(__cxa_uncaught_exceptions) { RET(0); }
SHIM(_ZSt18uncaught_exceptionv) { RET(0); }
SHIM(_ZSt9terminatev) { mk_fatal("std::terminate() in guest code"); }
SHIM(__cxa_call_unexpected) { mk_fatal("__cxa_call_unexpected in guest code"); }
SHIM(__cxa_bad_cast) { mk_fatal("std::bad_cast thrown in guest code"); }
SHIM(__cxa_bad_typeid) { mk_fatal("std::bad_typeid thrown in guest code"); }
SHIM(__cxa_throw_bad_array_new_length) { mk_fatal("bad_array_new_length in guest code"); }
SHIM(__gxx_personality_v0) { mk_fatal("personality routine called directly"); }

typedef struct { const char *name; mk_import_fn fn; void *data; } cxx_entry;
#define F_(n) { "_" #n, s_##n, NULL }
#define D_(n, p) { n, NULL, (void *)(p) }
static const cxx_entry g_cxx[] = {
    F_(__cxa_allocate_exception), F_(__cxa_free_exception), F_(__cxa_throw), F_(_Unwind_Resume),
    F_(__cxa_begin_catch), F_(__cxa_end_catch), F_(__cxa_rethrow), F_(__cxa_get_exception_ptr),
    F_(__cxa_current_exception_type), F_(__cxa_uncaught_exceptions), F_(_ZSt18uncaught_exceptionv),
    F_(_ZSt9terminatev), F_(__cxa_call_unexpected), F_(__cxa_bad_cast), F_(__cxa_bad_typeid),
    F_(__cxa_throw_bad_array_new_length), F_(__gxx_personality_v0),
    D_("__ZTVN10__cxxabiv117__class_type_infoE", mk_vt_class_ti),
    D_("__ZTVN10__cxxabiv120__si_class_type_infoE", mk_vt_si_class_ti),
    D_("__ZTVN10__cxxabiv121__vmi_class_type_infoE", mk_vt_vmi_class_ti),
    D_("__ZTVN10__cxxabiv119__pointer_type_infoE", mk_vt_pointer_ti),
    D_("__ZTVN10__cxxabiv123__fundamental_type_infoE", mk_vt_fundamental_ti),
    D_("__ZTISt9exception", &g_ti_exception), D_("__ZTISt9bad_alloc", &g_ti_bad_alloc),
    D_("__ZTISt8bad_cast", &g_ti_bad_cast), D_("__ZTISt13runtime_error", &g_ti_runtime_error),
    D_("__ZTISt11logic_error", &g_ti_logic_error), D_("__ZTISt12length_error", &g_ti_length_error),
    D_("__ZTISt12out_of_range", &g_ti_out_of_range), D_("__ZTISt16invalid_argument", &g_ti_invalid_argument),
    D_("__ZTISt10bad_typeid", &g_ti_bad_typeid), D_("__ZTISt14overflow_error", &g_ti_overflow_error),
    D_("__ZTISt11range_error", &g_ti_range_error),
    { 0, 0, 0 } };

mk_import mk_cxx_lookup(const char *sym) {
    mk_import r = { NULL, NULL };
    for (const cxx_entry *e = g_cxx; e->name; e++)
        if (!strcmp(e->name, sym)) { r.fn = e->fn; r.data = e->data; break; }
    return r;
}

static void set_ti(ti0 *t, const char *name) { t->vt = (uint64_t)(uintptr_t)&mk_vt_class_ti[2]; t->name = (uint64_t)(uintptr_t)name; }
void mk_cxx_init(void) {
    set_ti(&g_ti_exception, "St9exception");
    set_ti(&g_ti_bad_alloc, "St9bad_alloc");
    set_ti(&g_ti_bad_cast, "St8bad_cast");
    set_ti(&g_ti_runtime_error, "St13runtime_error");
    set_ti(&g_ti_logic_error, "St11logic_error");
    set_ti(&g_ti_length_error, "St12length_error");
    set_ti(&g_ti_out_of_range, "St12out_of_range");
    set_ti(&g_ti_invalid_argument, "St16invalid_argument");
    set_ti(&g_ti_bad_typeid, "St10bad_typeid");
    set_ti(&g_ti_overflow_error, "St14overflow_error");
    set_ti(&g_ti_range_error, "St11range_error");
}
