// Just enough CoreFoundation for libil2cpp's locale/culture queries.
//
// CFStrings use the constant-string layout the compiler emits for @"..."
// literals in __cfstring: { isa = &__CFConstantStringClassReference, flags, char *, long length },
// so literals from the image and strings we create are handled the same way.
#include "darwin.h"

#include <android/log.h>
#include <stdlib.h>
#include <string.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "mkcf", __VA_ARGS__)

uint8_t d___CFConstantStringClassReference[64];
typedef struct { void *isa; long flags; const char *str; long len; } cfstr;

static cfstr *mkstr(const char *s) {
    cfstr *c = calloc(1, sizeof(*c));
    c->isa = d___CFConstantStringClassReference;
    c->flags = 0x7c8;
    c->str = strdup(s);
    c->len = (long)strlen(s);
    return c;
}

// Locale keys are compared by address.
void *d_kCFLocaleCountryCode, *d_kCFLocaleLanguageCode, *d_kCFLocaleScriptCode;
void *d_kCFAllocatorDefault, *d_kCFAllocatorNull;
static uint8_t g_locale[16];

static void *d_CFLocaleCopyCurrent(void) { return g_locale; }
static void *d_CFLocaleGetIdentifier(void *l) { (void)l; static cfstr *id; if (!id) id = mkstr("en_US"); return id; }
static void *d_CFLocaleGetValue(void *l, void *key) {
    (void)l;
    static cfstr *cc, *lc;
    if (key == d_kCFLocaleCountryCode) return cc ? cc : (cc = mkstr("US"));
    if (key == d_kCFLocaleLanguageCode) return lc ? lc : (lc = mkstr("en"));
    return NULL;
}
static void d_CFRelease(void *o) { (void)o; }
static void *d_CFRetain(void *o) { return o; }
static long d_CFStringGetLength(const cfstr *s) { return s ? s->len : 0; }
static long d_CFStringGetMaximumSizeForEncoding(long len, uint32_t enc) { (void)enc; return len * 4; }
static int d_CFStringGetCString(const cfstr *s, char *buf, long size, uint32_t enc) {
    (void)enc;
    if (!s || size <= s->len) return 0;
    memcpy(buf, s->str, s->len + 1);
    return 1;
}
// CFStringGetBytes(str, CFRange{loc,len}, enc, lossByte, isExternal, buffer, maxLen, *usedLen)
static long d_CFStringGetBytes(const cfstr *s, long loc, long len, uint32_t enc, uint8_t loss, uint8_t ext,
                               uint8_t *buf, long max, long *used) {
    (void)enc; (void)loss; (void)ext;
    long n = len;
    if (buf && n > max) n = max;
    if (buf) memcpy(buf, s->str + loc, n);
    if (used) *used = n;
    return n;
}

#define E(n) { #n, (void *)d_##n }
#define V(n) { #n, (void *)&d_##n }
static const darwin_sym g_cf[] = {
    E(CFLocaleCopyCurrent), E(CFLocaleGetIdentifier), E(CFLocaleGetValue), E(CFRelease), E(CFRetain),
    E(CFStringGetLength), E(CFStringGetMaximumSizeForEncoding), E(CFStringGetCString), E(CFStringGetBytes),
    V(kCFLocaleCountryCode), V(kCFLocaleLanguageCode), V(kCFLocaleScriptCode), V(kCFAllocatorDefault),
    V(kCFAllocatorNull), { "__CFConstantStringClassReference", d___CFConstantStringClassReference },
    { 0, 0 } };

void *darwin_corefoundation(const char *name) { return darwin_lookup(g_cf, name); }

void *darwin_resolve(const char *name, const char *dylib, int weak) {
    (void)weak;
    void *p = NULL;
    if (!strncmp(dylib, "libSystem", 9)) p = darwin_libsystem(name);
    else if (!strncmp(dylib, "libc++", 6)) p = darwin_libcxx(name);
    else if (!strcmp(dylib, "CoreFoundation")) p = darwin_corefoundation(name);
    else if (!strcmp(dylib, "dlsym")) {
        p = darwin_libsystem(name);
        if (!p) p = darwin_libcxx(name);
        if (!p) p = darwin_corefoundation(name);
    }
    return p;
}
