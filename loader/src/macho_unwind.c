// Makes C++ exceptions work across UnityFramework frames.
//
// Mach-O describes unwinding with compact unwind (__TEXT,__unwind_info); the
// NDK's LLVM libunwind on Android only reads DWARF CFI found via
// dl_iterate_phdr. We translate every compact entry into a DWARF FDE, build an
// .eh_frame_hdr binary-search table, and report a fake loaded module covering
// the image's code so libunwind finds it like any ELF library.
//
// IL2CPP raises managed exceptions as C++ exceptions (Il2CppExceptionWrapper),
// so this is required for any try/catch in game code.
#define _GNU_SOURCE
#include "macho_loader.h"

#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); \
    __android_log_print(ANDROID_LOG_INFO, "mkunwind", __VA_ARGS__); } while (0)

// compact unwind (arm64)
#define UNWIND_HAS_LSDA 0x40000000u
#define UNWIND_PERSONALITY_MASK 0x30000000u
#define UNWIND_ARM64_MODE_MASK 0x0F000000u
#define UNWIND_ARM64_MODE_FRAMELESS 0x02000000u
#define UNWIND_ARM64_MODE_DWARF 0x03000000u
#define UNWIND_ARM64_MODE_FRAME 0x04000000u

typedef struct { uint64_t start, end; uint32_t enc; uint64_t lsda; uint64_t fde; } entry;

typedef struct { uint8_t *p; size_t n, cap; } buf;
static void put(buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->cap + n) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put8(buf *b, uint8_t v) { put(b, &v, 1); }
static void put32(buf *b, uint32_t v) { put(b, &v, 4); }
static void put64(buf *b, uint64_t v) { put(b, &v, 8); }
static void uleb(buf *b, uint64_t v) { do { uint8_t c = v & 0x7f; v >>= 7; if (v) c |= 0x80; put8(b, c); } while (v); }
static void sleb(buf *b, int64_t v) {
    for (;;) {
        uint8_t c = v & 0x7f;
        v >>= 7;
        if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) { put8(b, c); return; }
        put8(b, c | 0x80);
    }
}

static uint8_t *g_hdr, *g_ehframe;
static size_t g_hdr_size;
static uint64_t g_code_lo, g_code_hi;
static ElfW(Phdr) g_phdr[2];

// CIE: version 1, code align 4, data align -8, return register x30.
static size_t emit_cie(buf *b, void *personality) {
    size_t at = b->n;
    buf body = { 0 };
    put32(&body, 0);  // CIE id
    put8(&body, 1);   // version
    if (personality) put(&body, "zPLR", 5); else put(&body, "zR", 3);
    uleb(&body, 4);
    sleb(&body, -8);
    uleb(&body, 30);
    if (personality) {
        uleb(&body, 1 + 8 + 1 + 1);
        put8(&body, 0x00);  // personality: absptr
        put64(&body, (uint64_t)(uintptr_t)personality);
        put8(&body, 0x00);  // LSDA: absptr
        put8(&body, 0x00);  // FDE pointers: absptr
    } else {
        uleb(&body, 1);
        put8(&body, 0x00);
    }
    put8(&body, 0x0c); uleb(&body, 31); uleb(&body, 0);  // DW_CFA_def_cfa sp, 0
    while ((body.n + 4) & 7) put8(&body, 0);              // DW_CFA_nop padding
    put32(b, (uint32_t)body.n);
    put(b, body.p, body.n);
    free(body.p);
    return at;
}

static void offset_rule(buf *b, unsigned reg, unsigned factored) {
    if (reg < 64) { put8(b, 0x80 | reg); uleb(b, factored); }
    else { put8(b, 0x05); uleb(b, reg); uleb(b, factored); }  // DW_CFA_offset_extended
}

static int emit_fde(buf *b, size_t cie_at, int with_lsda, const entry *e) {
    uint32_t enc = e->enc, mode = enc & UNWIND_ARM64_MODE_MASK;
    buf body = { 0 };
    size_t me = b->n;
    put32(&body, (uint32_t)(me + 4 - cie_at));  // CIE pointer: distance back to the CIE
    put64(&body, e->start);
    put64(&body, e->end - e->start);
    if (with_lsda) { uleb(&body, 8); put64(&body, e->lsda); } else uleb(&body, 0);
    static const unsigned pairs[] = { 19, 21, 23, 25, 27 };
    if (mode == UNWIND_ARM64_MODE_FRAME) {
        put8(&body, 0x0c); uleb(&body, 29); uleb(&body, 16);  // CFA = fp + 16
        offset_rule(&body, 30, 1);                             // lr at CFA-8
        offset_rule(&body, 29, 2);                             // fp at CFA-16
        unsigned slot = 3;
        for (int i = 0; i < 5; i++)
            if (enc & (1u << i)) { offset_rule(&body, pairs[i], slot++); offset_rule(&body, pairs[i] + 1, slot++); }
        for (int i = 0; i < 4; i++)
            if (enc & (0x100u << i)) { offset_rule(&body, 72 + 2 * i, slot++); offset_rule(&body, 73 + 2 * i, slot++); }
    } else if (mode == UNWIND_ARM64_MODE_FRAMELESS || enc == 0) {
        uint32_t size = 16 * ((enc >> 12) & 0xfff);
        put8(&body, 0x0c); uleb(&body, 31); uleb(&body, size);  // CFA = sp + size, return address stays in lr
        unsigned slot = 1;
        for (int i = 0; i < 5; i++)
            if (enc & (1u << i)) { offset_rule(&body, pairs[i], slot++); offset_rule(&body, pairs[i] + 1, slot++); }
        for (int i = 0; i < 4; i++)
            if (enc & (0x100u << i)) { offset_rule(&body, 72 + 2 * i, slot++); offset_rule(&body, 73 + 2 * i, slot++); }
    } else {
        free(body.p);
        return 0;
    }
    while ((body.n + 4) & 7) put8(&body, 0);
    put32(b, (uint32_t)body.n);
    put(b, body.p, body.n);
    free(body.p);
    return 1;
}

static int cmp_entry(const void *a, const void *b) {
    const entry *x = a, *y = b;
    return (x->start > y->start) - (x->start < y->start);
}

extern int __gxx_personality_v0();

int mk_register_unwind(mk_image *img) {
    if (!img->unwind_info) { LOG("no __unwind_info"); return -1; }
    const uint8_t *ui = img->base + img->unwind_info;
    const uint32_t *h = (const uint32_t *)ui;
    uint32_t common_off = h[1], common_n = h[2], pers_off = h[3], pers_n = h[4], idx_off = h[5], idx_n = h[6];
    const uint32_t *common = (const uint32_t *)(ui + common_off);
    const uint32_t *pers = (const uint32_t *)(ui + pers_off);
    (void)pers; (void)pers_n;

    size_t cap = 1 << 18, n = 0;
    entry *es = malloc(cap * sizeof(entry));
    const uint32_t *idx = (const uint32_t *)(ui + idx_off);
    unsigned dwarf_mode = 0;
    for (uint32_t i = 0; i + 1 < idx_n; i++) {
        uint32_t fn_off = idx[i * 3], page_off = idx[i * 3 + 1], lsda_off = idx[i * 3 + 2];
        uint32_t lsda_end = idx[(i + 1) * 3 + 2];
        const uint32_t *lsda = (const uint32_t *)(ui + lsda_off);
        size_t lsda_n = (lsda_end - lsda_off) / 8, li = 0;
        const uint8_t *page = ui + page_off;
        uint32_t kind = *(const uint32_t *)page;
        uint16_t eoff = *(const uint16_t *)(page + 4), en = *(const uint16_t *)(page + 6);
        for (uint16_t k = 0; k < en; k++) {
            uint32_t fo, enc;
            if (kind == 2) {
                fo = *(const uint32_t *)(page + eoff + 8 * k);
                enc = *(const uint32_t *)(page + eoff + 8 * k + 4);
            } else if (kind == 3) {
                uint16_t peo = *(const uint16_t *)(page + 8), pen = *(const uint16_t *)(page + 10);
                (void)pen;
                uint32_t w = *(const uint32_t *)(page + eoff + 4 * k);
                fo = fn_off + (w & 0xffffff);
                uint32_t ei = w >> 24;
                enc = ei < common_n ? common[ei] : ((const uint32_t *)(page + peo))[ei - common_n];
            } else {
                LOG("unknown second-level page kind %u", kind);
                break;
            }
            if (n == cap) { cap *= 2; es = realloc(es, cap * sizeof(entry)); }
            entry *e = &es[n++];
            e->start = (uint64_t)(uintptr_t)(img->base + fo);
            e->enc = enc;
            e->lsda = 0;
            e->fde = 0;
            if (enc & UNWIND_HAS_LSDA) {
                while (li < lsda_n && lsda[li * 2] < fo) li++;
                if (li < lsda_n && lsda[li * 2] == fo) e->lsda = (uint64_t)(uintptr_t)(img->base + lsda[li * 2 + 1]);
            }
            if ((enc & UNWIND_ARM64_MODE_MASK) == UNWIND_ARM64_MODE_DWARF) {
                e->fde = (uint64_t)(uintptr_t)(img->base + img->eh_frame + (enc & 0xffffff));
                dwarf_mode++;
            }
        }
    }
    qsort(es, n, sizeof(entry), cmp_entry);
    uint64_t text_end = (uint64_t)(uintptr_t)(img->base + idx[(idx_n - 1) * 3]);
    for (size_t i = 0; i < n; i++) es[i].end = i + 1 < n ? es[i + 1].start : text_end;

    buf eh = { 0 };
    size_t cie_plain = emit_cie(&eh, NULL);
    size_t cie_pers = emit_cie(&eh, (void *)__gxx_personality_v0);
    uint64_t *fde_at = malloc(n * sizeof(uint64_t));
    size_t emitted = 0;
    for (size_t i = 0; i < n; i++) {
        entry *e = &es[i];
        if (e->fde) { fde_at[i] = ~0ull; continue; }  // DWARF mode: use the image's own FDE
        size_t at = eh.n;
        int lsda = (e->enc & UNWIND_HAS_LSDA) && e->lsda;
        // Every LSDA in this binary belongs to ___gxx_personality_v0, so one personality CIE is enough.
        if (emit_fde(&eh, lsda || (e->enc & UNWIND_PERSONALITY_MASK) ? cie_pers : cie_plain, lsda, e)) {
            fde_at[i] = at;
            emitted++;
        } else {
            fde_at[i] = ~1ull;
        }
    }
    put32(&eh, 0);  // terminator
    g_ehframe = mmap(NULL, eh.n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memcpy(g_ehframe, eh.p, eh.n);
    free(eh.p);

    // .eh_frame_hdr: version, eh_frame_ptr enc (absptr), fde_count enc (udata4), table enc (datarel|sdata8)
    size_t rows = 0;
    for (size_t i = 0; i < n; i++) if (fde_at[i] != ~1ull) rows++;
    g_hdr_size = 4 + 8 + 4 + rows * 16;
    g_hdr = mmap(NULL, g_hdr_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *p = g_hdr;
    p[0] = 1; p[1] = 0x00; p[2] = 0x03; p[3] = 0x3c;
    *(uint64_t *)(p + 4) = (uint64_t)(uintptr_t)g_ehframe;
    *(uint32_t *)(p + 12) = (uint32_t)rows;
    int64_t *t = (int64_t *)(p + 16);
    for (size_t i = 0; i < n; i++) {
        if (fde_at[i] == ~1ull) continue;
        uint64_t fde = fde_at[i] == ~0ull ? es[i].fde : (uint64_t)(uintptr_t)(g_ehframe + fde_at[i]);
        *t++ = (int64_t)(es[i].start - (uint64_t)(uintptr_t)g_hdr);
        *t++ = (int64_t)(fde - (uint64_t)(uintptr_t)g_hdr);
    }
    mprotect(g_ehframe, eh.n, PROT_READ);
    mprotect(g_hdr, g_hdr_size, PROT_READ);
    g_code_lo = n ? es[0].start : 0;
    g_code_hi = text_end;
    g_phdr[0].p_type = PT_LOAD;
    g_phdr[0].p_flags = PF_R | PF_X;
    g_phdr[0].p_vaddr = g_code_lo;
    g_phdr[0].p_memsz = g_phdr[0].p_filesz = g_code_hi - g_code_lo;
    g_phdr[1].p_type = PT_GNU_EH_FRAME;
    g_phdr[1].p_flags = PF_R;
    g_phdr[1].p_vaddr = (uint64_t)(uintptr_t)g_hdr;
    g_phdr[1].p_memsz = g_phdr[1].p_filesz = g_hdr_size;
    LOG("unwind: %zu functions, %zu FDEs generated, %u DWARF-mode, eh_frame %zu KB", n, emitted, dwarf_mode,
        (size_t)(eh.n >> 10));
    free(fde_at);
    free(es);
    return 0;
}

// libunwind (statically linked from the NDK) finds unwind tables through
// dl_iterate_phdr; this definition takes precedence over bionic's at link time.
typedef int (*iter_cb)(struct dl_phdr_info *, size_t, void *);
int dl_iterate_phdr(iter_cb cb, void *data) {
    static int (*real)(iter_cb, void *);
    if (!real) real = (int (*)(iter_cb, void *))dlsym(RTLD_NEXT, "dl_iterate_phdr");
    if (g_hdr) {
        struct dl_phdr_info info;
        memset(&info, 0, sizeof(info));
        info.dlpi_addr = 0;
        info.dlpi_name = "UnityFramework";
        info.dlpi_phdr = g_phdr;
        info.dlpi_phnum = 2;
        int r = cb(&info, sizeof(info), data);
        if (r) return r;
    }
    return real ? real(cb, data) : 0;
}
