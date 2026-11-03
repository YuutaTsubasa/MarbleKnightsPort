// The handful of out-of-line libc++ (std::__1) functions UnityFramework imports.
//
// Apple arm64 C++ ABI: constructors and destructors return `this` in x0, and
// callers rely on it, so every ctor/dtor below returns its object.
//
// Apple arm64 libc++ uses the "alternate" std::string layout:
//   long:  { char *data; size_t size; size_t cap | 1<<63 }
//   short: { char data[23]; uint8_t size }   (size byte < 0x80)
// Unity overrides operator new/delete inside the image, and libc++'s own
// allocations go through those on iOS, so we call the image's versions too.
// C++ ABI entry points (__cxa_*, typeinfo vtables, personality) come from the
// NDK's libc++abi, which follows the same Itanium ABI.
#include "darwin.h"

#include <android/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "mkcxx", __VA_ARGS__)

static void *(*img_new)(size_t);
static void (*img_delete)(void *);

typedef struct {
    union {
        struct { char *data; size_t size, cap; } l;
        struct { char data[23]; uint8_t size; } s;
    };
} dstring;
_Static_assert(sizeof(dstring) == 24, "std::string");

#define LONG_FLAG ((size_t)1 << 63)
#define SHORT_CAP 22

static int is_long(const dstring *s) { return (s->s.size & 0x80) != 0; }
static size_t str_size(const dstring *s) { return is_long(s) ? s->l.size : s->s.size; }
static size_t str_cap(const dstring *s) { return is_long(s) ? (s->l.cap & ~LONG_FLAG) - 1 : SHORT_CAP; }
static char *str_data(dstring *s) { return is_long(s) ? s->l.data : s->s.data; }
static const char *str_cdata(const dstring *s) { return is_long(s) ? s->l.data : s->s.data; }

static void set_size(dstring *s, size_t n) {
    if (is_long(s)) s->l.size = n;
    else s->s.size = (uint8_t)n;
    str_data(s)[n] = 0;
}

static void __attribute__((noreturn)) length_error(void) { LOG("std::string length_error"); abort(); }
static void __attribute__((noreturn)) out_of_range(void) { LOG("std::string out_of_range"); abort(); }

// Make room for `cap` characters (keeps contents).
static void str_reserve(dstring *s, size_t cap) {
    if (cap <= str_cap(s)) return;
    size_t n = str_size(s);
    size_t alloc = cap + 1;
    if (alloc < 2 * (str_cap(s) + 1)) alloc = 2 * (str_cap(s) + 1);
    alloc = (alloc + 15) & ~(size_t)15;
    char *p = img_new(alloc);
    memcpy(p, str_cdata(s), n + 1);
    if (is_long(s)) img_delete(s->l.data);
    s->l.data = p;
    s->l.size = n;
    s->l.cap = alloc | LONG_FLAG;
}

static void str_init(dstring *s, const char *p, size_t n) {
    memset(s, 0, sizeof(*s));
    if (n > SHORT_CAP) str_reserve(s, n);
    memcpy(str_data(s), p, n);
    set_size(s, n);
}

static dstring *str_replace(dstring *s, size_t pos, size_t del, const char *p, size_t n) {
    size_t sz = str_size(s);
    if (pos > sz) out_of_range();
    if (del > sz - pos) del = sz - pos;
    size_t nsz = sz - del + n;
    // p may point into s; copy it out first if a reallocation or overlap could clobber it.
    char tmpbuf[256], *tmp = NULL;
    const char *src = p;
    if (p >= str_cdata(s) && p < str_cdata(s) + sz + 1) {
        tmp = n <= sizeof(tmpbuf) ? tmpbuf : malloc(n);
        memcpy(tmp, p, n);
        src = tmp;
    }
    str_reserve(s, nsz);
    char *d = str_data(s);
    memmove(d + pos + n, d + pos + del, sz - pos - del);
    memcpy(d + pos, src, n);
    set_size(s, nsz);
    if (tmp && tmp != tmpbuf) free(tmp);
    return s;
}

// --- exported entry points (named after the Itanium mangling, minus the leading underscore) ---
static size_t s_find_c(const dstring *s, char c, size_t pos) {
    size_t n = str_size(s);
    const char *d = str_cdata(s);
    for (size_t i = pos; i < n; i++) if (d[i] == c) return i;
    return (size_t)-1;
}
static size_t s_rfind_c(const dstring *s, char c, size_t pos) {
    size_t n = str_size(s);
    const char *d = str_cdata(s);
    if (!n) return (size_t)-1;
    if (pos >= n) pos = n - 1;
    for (size_t i = pos + 1; i-- > 0;) if (d[i] == c) return i;
    return (size_t)-1;
}
static size_t s_rfind_s(const dstring *s, const char *p, size_t pos, size_t n) {
    size_t sz = str_size(s);
    const char *d = str_cdata(s);
    if (n > sz) return (size_t)-1;
    if (pos > sz - n) pos = sz - n;
    for (size_t i = pos + 1; i-- > 0;) if (!memcmp(d + i, p, n)) return i;
    return (size_t)-1;
}
static int s_compare_cstr(const dstring *s, const char *p) {
    size_t a = str_size(s), b = strlen(p), m = a < b ? a : b;
    int r = memcmp(str_cdata(s), p, m);
    return r ? r : (a < b ? -1 : a > b);
}
static int s_compare_sub(const dstring *s, size_t pos, size_t n1, const char *p, size_t n2) {
    size_t sz = str_size(s);
    if (pos > sz) out_of_range();
    if (n1 > sz - pos) n1 = sz - pos;
    size_t m = n1 < n2 ? n1 : n2;
    int r = memcmp(str_cdata(s) + pos, p, m);
    return r ? r : (n1 < n2 ? -1 : n1 > n2);
}
static dstring *s_erase(dstring *s, size_t pos, size_t n) { return str_replace(s, pos, n, "", 0); }
static dstring *s_append_cstr(dstring *s, const char *p) { return str_replace(s, str_size(s), 0, p, strlen(p)); }
static dstring *s_append_n(dstring *s, const char *p, size_t n) { return str_replace(s, str_size(s), 0, p, n); }
static dstring *s_append_fill(dstring *s, size_t n, char c) {
    size_t sz = str_size(s);
    str_reserve(s, sz + n);
    memset(str_data(s) + sz, c, n);
    set_size(s, sz + n);
    return s;
}
static dstring *s_assign_cstr(dstring *s, const char *p) { return str_replace(s, 0, str_size(s), p, strlen(p)); }
static dstring *s_insert_cstr(dstring *s, size_t pos, const char *p) { return str_replace(s, pos, 0, p, strlen(p)); }
static void s_resize(dstring *s, size_t n, char c) {
    size_t sz = str_size(s);
    if (n > sz) s_append_fill(s, n - sz, c);
    else set_size(s, n);
}
static void s_reserve(dstring *s, size_t n) { str_reserve(s, n); }
static void s_grow_by(dstring *s, size_t old_cap, size_t delta_cap, size_t old_sz, size_t n_copy, size_t n_del,
                      size_t n_add) {
    // Grows the buffer, keeping [0,n_copy) and [n_copy+n_del, old_sz) with a gap of n_add after n_copy.
    // The caller writes the new characters and the size afterwards.
    size_t cap = old_cap + delta_cap;
    size_t alloc = cap + 1;
    if (alloc < 2 * (old_cap + 1)) alloc = 2 * (old_cap + 1);
    alloc = (alloc + 15) & ~(size_t)15;
    char *p = img_new(alloc);
    const char *old = str_cdata(s);
    memcpy(p, old, n_copy);
    size_t tail = old_sz - n_del - n_copy;
    memcpy(p + n_copy + n_add, old + n_copy + n_del, tail);
    if (is_long(s)) img_delete(s->l.data);
    s->l.data = p;
    s->l.cap = alloc | LONG_FLAG;
}
static void s_push_back(dstring *s, char c) { s_append_fill(s, 1, c); }
static dstring *s_copy_ctor(dstring *s, const dstring *o) { str_init(s, str_cdata(o), str_size(o)); return s; }
static dstring *s_sub_ctor(dstring *s, const dstring *o, size_t pos, size_t n, const void *alloc) {
    (void)alloc;
    size_t sz = str_size(o);
    if (pos > sz) out_of_range();
    if (n > sz - pos) n = sz - pos;
    str_init(s, str_cdata(o) + pos, n);
    return s;
}
static dstring *s_dtor(dstring *s) { if (is_long(s)) img_delete(s->l.data); return s; }
static dstring *s_assign_copy(dstring *s, const dstring *o) {
    if (s != o) str_replace(s, 0, str_size(s), str_cdata(o), str_size(o));
    return s;
}
static dstring *s_assign_char(dstring *s, char c) { return str_replace(s, 0, str_size(s), &c, 1); }
// operator+(const char*, const string&) returns the string through x8 (sret);
// darwin_str_plus in trap.S moves x8 into the third argument.
void shim_str_plus(const char *a, const dstring *b, dstring *ret) {
    size_t na = strlen(a);
    str_init(ret, a, na);
    str_replace(ret, na, 0, str_cdata(b), str_size(b));
}

static void s_throw_length(void) { length_error(); }
static void s_throw_range(void) { out_of_range(); }
static void s_terminate(void) { LOG("std::terminate"); abort(); }
static void s_pure_virtual(void) { LOG("pure virtual call"); abort(); }

static void *s_mutex_dtor(void *m) { pthread_mutex_destroy(m); return m; }
static void *s_condvar_dtor(void *c) { pthread_cond_destroy(c); return c; }

typedef struct { void **vtbl; _Atomic long shared, weak; } shared_weak_count;
static void s_release_weak(shared_weak_count *c) {
    void (*on_zero_weak)(shared_weak_count *) = (void (*)(shared_weak_count *))c->vtbl[4];
    if (atomic_load_explicit(&c->weak, memory_order_acquire) == 0 || atomic_fetch_sub(&c->weak, 1) - 1 == -1)
        on_zero_weak(c);
}
static void *s_shared_weak_dtor(void *c) { return c; }

static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
static int cmp_uint(const void *a, const void *b) { unsigned x = *(const unsigned *)a, y = *(const unsigned *)b; return (x > y) - (x < y); }
static void s_sort_int(int *f, int *l, void *cmp) { (void)cmp; qsort(f, l - f, sizeof(int), cmp_int); }
static void s_sort_uint(unsigned *f, unsigned *l, void *cmp) { (void)cmp; qsort(f, l - f, sizeof(unsigned), cmp_uint); }

extern void darwin_str_plus(void);

// C++ ABI from the NDK's libc++abi / libunwind.
extern void *__cxa_allocate_exception(size_t);
extern void __cxa_free_exception(void *);
extern void *__cxa_begin_catch(void *);
extern void __cxa_end_catch(void);
extern void __cxa_rethrow(void);
extern void __cxa_throw(void *, void *, void (*)(void *));
extern void __cxa_call_unexpected(void *);
extern int __cxa_guard_acquire(void *);
extern void __cxa_guard_release(void *);
extern void __cxa_guard_abort(void *);
extern int __gxx_personality_v0();
extern void _Unwind_Resume(void *);
extern char _ZTVN10__cxxabiv117__class_type_infoE[], _ZTVN10__cxxabiv120__si_class_type_infoE[],
    _ZTVN10__cxxabiv121__vmi_class_type_infoE[], _ZTISt8bad_cast[], _ZTVSt12length_error[];

#define S "St3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE"
static const darwin_sym g_cxx[] = {
    { "_ZNK" S "4findEcm", s_find_c },
    { "_ZNK" S "5rfindEPKcmm", s_rfind_s },
    { "_ZNK" S "5rfindEcm", s_rfind_c },
    { "_ZNK" S "7compareEPKc", s_compare_cstr },
    { "_ZNK" S "7compareEmmPKcm", s_compare_sub },
    { "_ZN" S "5eraseEmm", s_erase },
    { "_ZN" S "6appendEPKc", s_append_cstr },
    { "_ZN" S "6appendEPKcm", s_append_n },
    { "_ZN" S "6appendEmc", s_append_fill },
    { "_ZN" S "6assignEPKc", s_assign_cstr },
    { "_ZN" S "6insertEmPKc", s_insert_cstr },
    { "_ZN" S "6resizeEmc", s_resize },
    { "_ZN" S "7reserveEm", s_reserve },
    { "_ZN" S "9__grow_byEmmmmmm", s_grow_by },
    { "_ZN" S "9push_backEc", s_push_back },
    { "_ZN" S "C1ERKS5_", s_copy_ctor },
    { "_ZN" S "C1ERKS5_mmRKS4_", s_sub_ctor },
    { "_ZN" S "D1Ev", s_dtor },
    { "_ZN" S "aSERKS5_", s_assign_copy },
    { "_ZN" S "aSEc", s_assign_char },
    { "_ZNSt3__1plIcNS_11char_traitsIcEENS_9allocatorIcEEEENS_12basic_stringIT_T0_T1_EEPKS6_RKS9_", darwin_str_plus },
    { "_ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv", s_throw_length },
    { "_ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv", s_throw_length },
    { "_ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv", s_throw_range },
    { "_ZSt9terminatev", s_terminate },
    { "__cxa_pure_virtual", s_pure_virtual },
    { "_ZNSt3__15mutexD1Ev", s_mutex_dtor },
    { "_ZNSt3__118condition_variableD1Ev", s_condvar_dtor },
    { "_ZNSt3__119__shared_weak_count14__release_weakEv", s_release_weak },
    { "_ZNSt3__119__shared_weak_countD2Ev", s_shared_weak_dtor },
    { "_ZNSt3__16__sortIRNS_6__lessIiiEEPiEEvT0_S5_T_", s_sort_int },
    { "_ZNSt3__16__sortIRNS_6__lessIjjEEPjEEvT0_S5_T_", s_sort_uint },
    { "__cxa_allocate_exception", __cxa_allocate_exception },
    { "__cxa_free_exception", __cxa_free_exception },
    { "__cxa_begin_catch", __cxa_begin_catch },
    { "__cxa_end_catch", __cxa_end_catch },
    { "__cxa_rethrow", __cxa_rethrow },
    { "__cxa_throw", __cxa_throw },
    { "__cxa_call_unexpected", __cxa_call_unexpected },
    { "__cxa_guard_acquire", __cxa_guard_acquire },
    { "__cxa_guard_release", __cxa_guard_release },
    { "__cxa_guard_abort", __cxa_guard_abort },
    { "__gxx_personality_v0", __gxx_personality_v0 },
    { "_Unwind_Resume", _Unwind_Resume },
    { "_ZTVN10__cxxabiv117__class_type_infoE", _ZTVN10__cxxabiv117__class_type_infoE },
    { "_ZTVN10__cxxabiv120__si_class_type_infoE", _ZTVN10__cxxabiv120__si_class_type_infoE },
    { "_ZTVN10__cxxabiv121__vmi_class_type_infoE", _ZTVN10__cxxabiv121__vmi_class_type_infoE },
    { "_ZTISt8bad_cast", _ZTISt8bad_cast },
    { "_ZTVSt12length_error", _ZTVSt12length_error },
    { 0, 0 } };

void *darwin_libcxx(const char *name) { return darwin_lookup(g_cxx, name); }

void darwin_cxx_init(mk_image *img) {
    img_new = (void *(*)(size_t))mk_export(img, "__Znwm");
    img_delete = (void (*)(void *))mk_export(img, "__ZdlPv");
    if (!img_new || !img_delete) { LOG("image has no operator new/delete; using malloc"); img_new = malloc; img_delete = free; }
}
