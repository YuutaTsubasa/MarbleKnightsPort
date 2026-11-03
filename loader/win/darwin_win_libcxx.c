// The out-of-line libc++ (std::__1) functions UnityFramework imports, for the Windows build.
// Same logic as the Android shims (loader/src/darwin_cxx.c), reading arguments from guest registers.
//
// Apple arm64 C++ ABI: constructors and destructors return `this` in x0.
// Apple's libc++ uses the "alternate" std::string layout:
//   long:  { char *data; size_t size; size_t cap | 1<<63 }
//   short: { char data[23]; uint8_t size }   (size byte < 0x80)
// Unity overrides operator new/delete inside the image; string buffers go through those, like on iOS.
#define _CRT_SECURE_NO_WARNINGS
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "win_rt.h"

#define SHIM(name) static void s_##name(cpu *c)
#define P(i) ((void *)(uintptr_t)X(i))
#define S(i) ((const char *)(uintptr_t)X(i))

static uint64_t g_new, g_delete;  // the image's operator new / delete
static char *img_new(size_t n) {
    if (!g_new) return mk_malloc(n);
    uint64_t x[1] = { n };
    return (char *)(uintptr_t)mk_guest_enter(g_new, x, 1, NULL, 0, NULL);
}
static void img_delete(void *p) {
    if (!g_delete) { mk_free(p); return; }
    uint64_t x[1] = { (uint64_t)(uintptr_t)p };
    mk_guest_enter(g_delete, x, 1, NULL, 0, NULL);
}

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
static void str_reserve(dstring *s, size_t cap) {
    if (cap <= str_cap(s)) return;
    size_t n = str_size(s), alloc = cap + 1;
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
    if (pos > sz) mk_fatal("std::string out_of_range");
    if (del > sz - pos) del = sz - pos;
    size_t nsz = sz - del + n;
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

SHIM(find_c) {
    const dstring *s = P(0);
    size_t n = str_size(s), pos = X(2);
    const char *d = str_cdata(s);
    for (size_t i = pos; i < n; i++) if (d[i] == (char)X(1)) { RET(i); return; }
    RET(-1);
}
SHIM(find_s) {  // find(const char *, pos, n)
    const dstring *s = P(0);
    size_t sz = str_size(s), pos = X(2), n = X(3);
    const char *d = str_cdata(s);
    if (n == 0) { RET(pos <= sz ? pos : (size_t)-1); return; }
    for (size_t i = pos; i + n <= sz; i++) if (!memcmp(d + i, S(1), n)) { RET(i); return; }
    RET(-1);
}
SHIM(rfind_c) {
    const dstring *s = P(0);
    size_t n = str_size(s), pos = X(2);
    const char *d = str_cdata(s);
    if (!n) { RET(-1); return; }
    if (pos >= n) pos = n - 1;
    for (size_t i = pos + 1; i-- > 0;) if (d[i] == (char)X(1)) { RET(i); return; }
    RET(-1);
}
SHIM(rfind_s) {
    const dstring *s = P(0);
    size_t sz = str_size(s), pos = X(2), n = X(3);
    const char *d = str_cdata(s);
    if (n > sz) { RET(-1); return; }
    if (pos > sz - n) pos = sz - n;
    for (size_t i = pos + 1; i-- > 0;) if (!memcmp(d + i, S(1), n)) { RET(i); return; }
    RET(-1);
}
SHIM(compare_cstr) {
    const dstring *s = P(0);
    size_t a = str_size(s), b = strlen(S(1)), m = a < b ? a : b;
    int r = memcmp(str_cdata(s), S(1), m);
    RET((int64_t)(r ? r : (a < b ? -1 : a > b)));
}
SHIM(compare_sub) {
    const dstring *s = P(0);
    size_t sz = str_size(s), pos = X(1), n1 = X(2), n2 = X(4);
    if (pos > sz) mk_fatal("std::string out_of_range");
    if (n1 > sz - pos) n1 = sz - pos;
    size_t m = n1 < n2 ? n1 : n2;
    int r = memcmp(str_cdata(s) + pos, S(3), m);
    RET((int64_t)(r ? r : (n1 < n2 ? -1 : n1 > n2)));
}
SHIM(erase) { RET(str_replace(P(0), X(1), X(2), "", 0)); }
SHIM(append_cstr) { dstring *s = P(0); RET(str_replace(s, str_size(s), 0, S(1), strlen(S(1)))); }
SHIM(append_n) { dstring *s = P(0); RET(str_replace(s, str_size(s), 0, S(1), X(2))); }
static void append_fill(dstring *s, size_t n, char ch) {
    size_t sz = str_size(s);
    str_reserve(s, sz + n);
    memset(str_data(s) + sz, ch, n);
    set_size(s, sz + n);
}
SHIM(append_fill) { append_fill(P(0), X(1), (char)X(2)); RET(P(0)); }
SHIM(assign_cstr) { dstring *s = P(0); RET(str_replace(s, 0, str_size(s), S(1), strlen(S(1)))); }
SHIM(assign_n) { dstring *s = P(0); RET(str_replace(s, 0, str_size(s), S(1), X(2))); }
SHIM(insert_cstr) { RET(str_replace(P(0), X(1), 0, S(2), strlen(S(2)))); }
SHIM(insert_n) { RET(str_replace(P(0), X(1), 0, S(2), X(3))); }
SHIM(resize) {
    dstring *s = P(0);
    size_t sz = str_size(s), n = X(1);
    if (n > sz) append_fill(s, n - sz, (char)X(2));
    else set_size(s, n);
}
SHIM(reserve) { str_reserve(P(0), X(1)); }
SHIM(grow_by) {  // (old_cap, delta_cap, old_sz, n_copy, n_del, n_add): keeps [0,n_copy) and the tail, gap of n_add
    dstring *s = P(0);
    size_t old_cap = X(1), delta = X(2), old_sz = X(3), n_copy = X(4), n_del = X(5), n_add = X(6);
    size_t alloc = old_cap + delta + 1;
    if (alloc < 2 * (old_cap + 1)) alloc = 2 * (old_cap + 1);
    alloc = (alloc + 15) & ~(size_t)15;
    char *p = img_new(alloc);
    const char *old = str_cdata(s);
    memcpy(p, old, n_copy);
    memcpy(p + n_copy + n_add, old + n_copy + n_del, old_sz - n_del - n_copy);
    if (is_long(s)) img_delete(s->l.data);
    s->l.data = p;
    s->l.cap = alloc | LONG_FLAG;
}
SHIM(push_back) { append_fill(P(0), 1, (char)X(1)); }
SHIM(copy_ctor) { const dstring *o = P(1); str_init(P(0), str_cdata(o), str_size(o)); RET(P(0)); }
SHIM(sub_ctor) {
    const dstring *o = P(1);
    size_t sz = str_size(o), pos = X(2), n = X(3);
    if (pos > sz) mk_fatal("std::string out_of_range");
    if (n > sz - pos) n = sz - pos;
    str_init(P(0), str_cdata(o) + pos, n);
    RET(P(0));
}
SHIM(init_n) { str_init(P(0), S(1), X(2)); }  // __init(const char *, size_t)
SHIM(dtor) { dstring *s = P(0); if (is_long(s)) img_delete(s->l.data); RET(s); }
SHIM(assign_copy) { dstring *s = P(0); const dstring *o = P(1); if (s != o) str_replace(s, 0, str_size(s), str_cdata(o), str_size(o)); RET(s); }
SHIM(assign_char) { dstring *s = P(0); char ch = (char)X(1); RET(str_replace(s, 0, str_size(s), &ch, 1)); }
SHIM(str_plus) {  // operator+(const char *, const string &): the result comes back through x8
    dstring *ret = (dstring *)(uintptr_t)c->x[8];
    const dstring *b = P(1);
    size_t na = strlen(S(0));
    str_init(ret, S(0), na);
    str_replace(ret, na, 0, str_cdata(b), str_size(b));
}
SHIM(throw_length) { mk_fatal("std::length_error in guest code"); }
SHIM(throw_range) { mk_fatal("std::out_of_range in guest code"); }
SHIM(ret_this) { (void)c; }  // destructors of objects with nothing to free: this stays in x0
SHIM(release_weak) {  // __shared_weak_count::__release_weak()
    uint8_t *cnt = P(0);
    volatile LONG64 *weak = (volatile LONG64 *)(cnt + 16);
    if (*weak == 0 || InterlockedDecrement64(weak) == -1) {
        uint64_t fn = (*(uint64_t **)cnt)[4];  // __on_zero_shared_weak
        uint64_t x[1] = { (uint64_t)(uintptr_t)cnt };
        mk_guest_enter(fn, x, 1, NULL, 0, NULL);
    }
}
static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
static int cmp_uint(const void *a, const void *b) { unsigned x = *(const unsigned *)a, y = *(const unsigned *)b; return (x > y) - (x < y); }
SHIM(sort_int) { qsort(P(0), ((int *)P(1)) - ((int *)P(0)), sizeof(int), cmp_int); }
SHIM(sort_uint) { qsort(P(0), ((unsigned *)P(1)) - ((unsigned *)P(0)), sizeof(unsigned), cmp_uint); }
SHIM(memset_pattern16) {
    uint8_t *d = P(0);
    const uint8_t *pat = P(1);
    for (size_t i = 0; i < X(2); i++) d[i] = pat[i & 15];
}

#define STR "St3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE"
typedef struct { const char *name; mk_import_fn fn; } cxx_fn;
static const cxx_fn g_libcxx[] = {
    { "__ZNK" STR "4findEcm", s_find_c }, { "__ZNK" STR "4findEPKcmm", s_find_s },
    { "__ZNK" STR "5rfindEPKcmm", s_rfind_s }, { "__ZNK" STR "5rfindEcm", s_rfind_c },
    { "__ZNK" STR "7compareEPKc", s_compare_cstr }, { "__ZNK" STR "7compareEmmPKcm", s_compare_sub },
    { "__ZN" STR "5eraseEmm", s_erase }, { "__ZN" STR "6appendEPKc", s_append_cstr },
    { "__ZN" STR "6appendEPKcm", s_append_n }, { "__ZN" STR "6appendEmc", s_append_fill },
    { "__ZN" STR "6assignEPKc", s_assign_cstr }, { "__ZN" STR "6assignEPKcm", s_assign_n },
    { "__ZN" STR "6insertEmPKc", s_insert_cstr }, { "__ZN" STR "6insertEmPKcm", s_insert_n },
    { "__ZN" STR "6resizeEmc", s_resize }, { "__ZN" STR "7reserveEm", s_reserve },
    { "__ZN" STR "9__grow_byEmmmmmm", s_grow_by }, { "__ZN" STR "9push_backEc", s_push_back },
    { "__ZN" STR "C1ERKS5_", s_copy_ctor }, { "__ZN" STR "C2ERKS5_", s_copy_ctor },
    { "__ZN" STR "C1ERKS5_mmRKS4_", s_sub_ctor }, { "__ZN" STR "6__initEPKcm", s_init_n },
    { "__ZN" STR "D1Ev", s_dtor }, { "__ZN" STR "D2Ev", s_dtor },
    { "__ZN" STR "aSERKS5_", s_assign_copy }, { "__ZN" STR "aSEc", s_assign_char },
    { "__ZNSt3__1plIcNS_11char_traitsIcEENS_9allocatorIcEEEENS_12basic_stringIT_T0_T1_EEPKS6_RKS9_", s_str_plus },
    { "__ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv", s_throw_length },
    { "__ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv", s_throw_length },
    { "__ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv", s_throw_range },
    { "__ZNSt3__15mutexD1Ev", s_ret_this }, { "__ZNSt3__118condition_variableD1Ev", s_ret_this },
    { "__ZNSt3__119__shared_weak_count14__release_weakEv", s_release_weak },
    { "__ZNSt3__119__shared_weak_countD2Ev", s_ret_this },
    { "__ZNSt3__16__sortIRNS_6__lessIiiEEPiEEvT0_S5_T_", s_sort_int },
    { "__ZNSt3__16__sortIRNS_6__lessIjjEEPjEEvT0_S5_T_", s_sort_uint },
    { "_memset_pattern16", s_memset_pattern16 },
    { 0, 0 } };

mk_import_fn mk_libcxx_lookup(const char *sym) {
    for (const cxx_fn *e = g_libcxx; e->name; e++)
        if (!strcmp(e->name, sym)) return e->fn;
    return NULL;
}
void mk_libcxx_init(void) {
    g_new = mk_export("__Znwm");
    g_delete = mk_export("__ZdlPv");
    mk_log("operator new/delete in the image: %s", g_new && g_delete ? "yes" : "no (malloc/free)");
}
