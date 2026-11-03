// Just enough CoreFoundation for libil2cpp's locale/culture queries (Windows build; the Android
// version is loader/src/darwin_cf.c).
//
// CFStrings use the constant-string layout the compiler emits for @"..." literals in __cfstring:
// { isa = &__CFConstantStringClassReference, flags, char *, long length }, so literals from the
// image and strings we create are handled the same way.
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_RAND_S
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "win_rt.h"

#define SHIM(name) static void s_##name(cpu *c)
#define P(i) ((void *)(uintptr_t)X(i))

uint8_t mk_cf_constant_string_class[64];
typedef struct { void *isa; int64_t flags; const char *str; int64_t len; } cfstr;

static cfstr *mkstr(const char *s) {
    cfstr *r = mk_malloc(sizeof(*r));
    r->isa = mk_cf_constant_string_class;
    r->flags = 0x7c8;
    r->str = mk_strdup(s);
    r->len = (int64_t)strlen(s);
    return r;
}
// locale keys are CFStringRef globals compared by identity: each key is its own object
static cfstr k_country = { mk_cf_constant_string_class, 0x7c8, "kCFLocaleCountryCode", 20 };
static cfstr k_language = { mk_cf_constant_string_class, 0x7c8, "kCFLocaleLanguageCode", 21 };
static cfstr k_script = { mk_cf_constant_string_class, 0x7c8, "kCFLocaleScriptCode", 19 };
static cfstr k_bundle_version = { mk_cf_constant_string_class, 0x7c8, "CFBundleVersion", 15 };
static void *g_k_country = &k_country, *g_k_language = &k_language, *g_k_script = &k_script,
    *g_k_bundle_version = &k_bundle_version;
static void *g_alloc_default, *g_alloc_null;
static uint8_t g_locale[16], g_type_array_callbacks[64];

SHIM(CFLocaleCopyCurrent) { RET(g_locale); }
SHIM(CFLocaleGetIdentifier) { static cfstr *id; if (!id) id = mkstr("en_US"); RET(id); }
SHIM(CFLocaleGetValue) {
    static cfstr *cc, *lc;
    void *key = P(1);
    if (key == &k_country) { RET(cc ? cc : (cc = mkstr("US"))); return; }
    if (key == &k_language) { RET(lc ? lc : (lc = mkstr("en"))); return; }
    RET(0);
}
SHIM(CFRelease) { (void)c; }
SHIM(CFRetain) { (void)c; }  // returns its argument: x0 unchanged
SHIM(CFStringGetLength) { const cfstr *s = P(0); RET(s ? s->len : 0); }
SHIM(CFStringGetMaximumSizeForEncoding) { RET(X(0) * 4); }
SHIM(CFStringGetCString) {
    const cfstr *s = P(0);
    int64_t size = (int64_t)X(2);
    if (!s || size <= s->len) { RET(0); return; }
    memcpy(P(1), s->str, (size_t)s->len + 1);
    RET(1);
}
// CFStringGetBytes(str, CFRange{loc,len} (x1, x2), enc, lossByte, isExternal, buffer, maxLen, *usedLen)
SHIM(CFStringGetBytes) {
    const cfstr *s = P(0);
    int64_t loc = (int64_t)X(1), len = (int64_t)X(2), max = (int64_t)X(7);
    uint8_t *buf = (uint8_t *)(uintptr_t)X(6);
    int64_t *used = *(int64_t **)(GSP);  // 9th argument: on the stack
    int64_t n = len;
    if (buf && n > max) n = max;
    if (buf) memcpy(buf, s->str + loc, (size_t)n);
    if (used) *used = n;
    RET(n);
}
SHIM(CFStringCreateWithCharacters) {  // (alloc, const UniChar *, count): ASCII subset
    const uint16_t *u = P(1);
    int64_t n = (int64_t)X(2);
    char *b = malloc((size_t)n + 1);
    for (int64_t i = 0; i < n; i++) b[i] = u[i] < 128 ? (char)u[i] : '?';
    b[n] = 0;
    cfstr *r = mkstr(b);
    free(b);
    RET(r);
}
// CFUUID: 16 bytes
SHIM(CFUUIDCreate) { uint8_t *u = mk_malloc(16); for (int i = 0; i < 16; i++) { unsigned v; rand_s(&v); u[i] = (uint8_t)v; } u[6] = (u[6] & 0x0f) | 0x40; u[8] = (u[8] & 0x3f) | 0x80; RET(u); }
SHIM(CFUUIDCreateFromUUIDBytes) { uint8_t *u = mk_malloc(16); memcpy(u, &c->x[1], 16); RET(u); }  // CFUUIDBytes by value (x1, x2)
SHIM(CFUUIDGetUUIDBytes) { const uint8_t *u = P(0); memcpy(&c->x[0], u, 16); }  // returned in x0, x1
SHIM(CFUUIDCreateString) {
    const uint8_t *u = P(1);
    char b[40];
    snprintf(b, sizeof(b), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", u[0], u[1], u[2], u[3], u[4],
             u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    RET(mkstr(b));
}
// CFData { bytes, length }
typedef struct { const uint8_t *p; int64_t n; } cfdata;
SHIM(CFDataCreateWithBytesNoCopy) { cfdata *d = mk_malloc(sizeof(*d)); d->p = P(1); d->n = (int64_t)X(2); RET(d); }
SHIM(CFDataGetBytePtr) { RET(((cfdata *)P(0))->p); }
SHIM(CFDataGetLength) { RET(((cfdata *)P(0))->n); }
SHIM(CFDictionaryGetValue) { RET(0); }
SHIM(CFNumberGetValue) { RET(0); }
SHIM(CFURLGetFileSystemRepresentation) { RET(0); }
SHIM(CFArrayGetCount) { RET(0); }

typedef struct { const char *name; mk_import_fn fn; void *data; } cf_entry;
#define F_(n) { "_" #n, s_##n, NULL }
#define D_(n, p) { n, NULL, (void *)(p) }
static const cf_entry g_cf[] = {
    F_(CFLocaleCopyCurrent), F_(CFLocaleGetIdentifier), F_(CFLocaleGetValue), F_(CFRelease), F_(CFRetain),
    F_(CFStringGetLength), F_(CFStringGetMaximumSizeForEncoding), F_(CFStringGetCString), F_(CFStringGetBytes),
    F_(CFStringCreateWithCharacters), F_(CFUUIDCreate), F_(CFUUIDCreateFromUUIDBytes), F_(CFUUIDGetUUIDBytes),
    F_(CFUUIDCreateString), F_(CFDataCreateWithBytesNoCopy), F_(CFDataGetBytePtr), F_(CFDataGetLength),
    F_(CFDictionaryGetValue), F_(CFNumberGetValue), F_(CFURLGetFileSystemRepresentation), F_(CFArrayGetCount),
    D_("_kCFLocaleCountryCode", &g_k_country), D_("_kCFLocaleLanguageCode", &g_k_language),
    D_("_kCFLocaleScriptCode", &g_k_script), D_("_kCFBundleVersionKey", &g_k_bundle_version),
    D_("_kCFAllocatorDefault", &g_alloc_default), D_("_kCFAllocatorNull", &g_alloc_null),
    D_("_kCFTypeArrayCallBacks", g_type_array_callbacks),
    D_("___CFConstantStringClassReference", mk_cf_constant_string_class),
    { 0, 0, 0 } };

mk_import mk_cf_lookup(const char *sym) {
    mk_import r = { NULL, NULL };
    for (const cf_entry *e = g_cf; e->name; e++)
        if (!strcmp(e->name, sym)) { r.fn = e->fn; r.data = e->data; break; }
    return r;
}
