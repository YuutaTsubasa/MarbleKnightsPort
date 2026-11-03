// Minimal runtime Mach-O loader for arm64 Android.
//
// Maps the segments of a thin arm64 MH_DYLIB, applies rebases, binds every
// import (eager and lazy) through a resolver, and exposes the exports trie.
// Imports without an implementation are bound to numbered trap stubs so a
// call logs the symbol name instead of jumping to NULL.
#include "macho_loader.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOGI(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); \
    __android_log_print(ANDROID_LOG_INFO, "mkload", __VA_ARGS__); } while (0)

#define MH_MAGIC_64 0xfeedfacf
#define LC_SEGMENT_64 0x19
#define LC_SYMTAB 0x2
#define LC_DYLD_INFO_ONLY 0x80000022
#define LC_LOAD_DYLIB 0xc
#define LC_LOAD_WEAK_DYLIB 0x80000018
#define LC_REEXPORT_DYLIB 0x8000001f
#define LC_LOAD_UPWARD_DYLIB 0x80000023

typedef struct { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; } mach_header_64;
typedef struct { uint32_t cmd, cmdsize; } load_command;
typedef struct {
    uint32_t cmd, cmdsize; char segname[16];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    int32_t maxprot, initprot; uint32_t nsects, flags;
} segment_command_64;
typedef struct {
    char sectname[16], segname[16];
    uint64_t addr, size; uint32_t offset, align, reloff, nreloc, flags, r1, r2, r3;
} section_64;
typedef struct {
    uint32_t cmd, cmdsize, rebase_off, rebase_size, bind_off, bind_size, weak_bind_off, weak_bind_size,
             lazy_bind_off, lazy_bind_size, export_off, export_size;
} dyld_info_command;

#define MAX_SEGS 16
#define MAX_DYLIBS 64

static uint64_t uleb(const uint8_t **p) {
    uint64_t r = 0; int s = 0; uint8_t b;
    do { b = *(*p)++; r |= (uint64_t)(b & 0x7f) << s; s += 7; } while (b & 0x80);
    return r;
}
static int64_t sleb(const uint8_t **p) {
    int64_t r = 0; int s = 0; uint8_t b;
    do { b = *(*p)++; r |= (int64_t)(b & 0x7f) << s; s += 7; } while (b & 0x80);
    if (s < 64 && (b & 0x40)) r |= -((int64_t)1 << s);
    return r;
}

// ---- trap stubs (see trap.S) ---------------------------------------------
extern uint8_t mk_trap_stubs[];
#define MK_TRAP_COUNT 4096
static const char *g_trap_names[MK_TRAP_COUNT];
static unsigned g_trap_used;
int mk_trap_fatal;

const char *mk_trap_name(unsigned index) {
    return index < g_trap_used ? g_trap_names[index] : "?";
}

void mk_trap_hit(unsigned index, uint64_t lr) {
    static unsigned char seen[MK_TRAP_COUNT];
    if (index < MK_TRAP_COUNT && !seen[index]) {
        seen[index] = 1;
        LOGI("[trap] missing import %s called (lr=%#llx)", mk_trap_name(index), (unsigned long long)lr);
    }
    if (mk_trap_fatal) abort();
}

static void *trap_for(const char *name) {
    for (unsigned i = 0; i < g_trap_used; i++)
        if (strcmp(g_trap_names[i], name) == 0) return mk_trap_stubs + 8 * i;
    if (g_trap_used == MK_TRAP_COUNT) return NULL;
    g_trap_names[g_trap_used] = strdup(name);
    return mk_trap_stubs + 8 * g_trap_used++;
}

static uint8_t g_dummy_data[1 << 16] __attribute__((aligned(4096)));

// ---- binding ---------------------------------------------------------------
typedef struct {
    mk_image *img; mk_resolver resolve;
    const char *dylibs[MAX_DYLIBS]; int ndylibs;
    segment_command_64 *segs[MAX_SEGS]; int nsegs;
    unsigned missing_func, missing_data;
} bind_ctx;

static int is_data_symbol(const char *name) {
    // Imports read as data (not called). Unresolved ones get a zeroed block instead of a trap.
    static const char *prefixes[] = { "OBJC_CLASS_$_", "OBJC_METACLASS_$_", "OBJC_EHTYPE_$_", "_ZTV", "_ZTI", "_ZTS",
                                      "_NSConcrete", 0 };
    for (int i = 0; prefixes[i]; i++)
        if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0) return 1;
    if (name[0] == 'k' && name[1] >= 'A' && name[1] <= 'Z') return 1;  // kCFLocaleCountryCode, kSecClass...
    if (strstr(name, "Notification") || strstr(name, "Key") || strstr(name, "Category") || strstr(name, "Domain"))
        return 1;  // AVAudioSession*/UIKit string constants
    return 0;
}

static void do_bind(bind_ctx *c, uint64_t vmaddr, int ordinal, const char *sym, int64_t addend, int weak) {
    const char *lib = (ordinal >= 1 && ordinal <= c->ndylibs) ? c->dylibs[ordinal - 1] : "(flat)";
    const char *name = sym[0] == '_' ? sym + 1 : sym;
    void *target = c->resolve(name, lib, weak);
    if (!target) {
        if (is_data_symbol(name)) {
            target = g_dummy_data;
            c->missing_data++;
        } else {
            target = trap_for(name);
            c->missing_func++;
        }
    }
    *(uint64_t *)(c->img->base + vmaddr) = (uint64_t)(uintptr_t)target + addend;
}

static void run_bind_opcodes(bind_ctx *c, const uint8_t *p, const uint8_t *end, int lazy) {
    int ordinal = 0, seg = 0, weak = 0;
    int64_t addend = 0;
    uint64_t off = 0;
    const char *sym = "";
    while (p < end) {
        uint8_t b = *p++, op = b & 0xf0, imm = b & 0x0f;
        switch (op) {
        case 0x00: if (!lazy) return; break;
        case 0x10: ordinal = imm; break;
        case 0x20: ordinal = (int)uleb(&p); break;
        case 0x30: ordinal = imm ? (int)(int8_t)(0xf0 | imm) : 0; break;
        case 0x40: weak = imm & 1; sym = (const char *)p; p += strlen(sym) + 1; break;
        case 0x50: break;
        case 0x60: addend = sleb(&p); break;
        case 0x70: seg = imm; off = uleb(&p); break;
        case 0x80: off += uleb(&p); break;
        case 0x90: do_bind(c, c->segs[seg]->vmaddr + off, ordinal, sym, addend, weak); off += 8; break;
        case 0xa0: do_bind(c, c->segs[seg]->vmaddr + off, ordinal, sym, addend, weak); off += 8 + uleb(&p); break;
        case 0xb0: do_bind(c, c->segs[seg]->vmaddr + off, ordinal, sym, addend, weak); off += 8 + imm * 8; break;
        case 0xc0: {
            uint64_t n = uleb(&p), skip = uleb(&p);
            while (n--) { do_bind(c, c->segs[seg]->vmaddr + off, ordinal, sym, addend, weak); off += 8 + skip; }
            break;
        }
        default: LOGI("bad bind opcode %#x", op); return;
        }
    }
}

static void run_rebase_opcodes(bind_ctx *c, const uint8_t *p, const uint8_t *end) {
    int seg = 0;
    uint64_t off = 0, n, skip;
    uint8_t *base = c->img->base;
    intptr_t slide = c->img->slide;
#define REBASE() (*(uint64_t *)(base + c->segs[seg]->vmaddr + off) += slide)
    while (p < end) {
        uint8_t b = *p++, op = b & 0xf0, imm = b & 0x0f;
        switch (op) {
        case 0x00: return;
        case 0x10: break;
        case 0x20: seg = imm; off = uleb(&p); break;
        case 0x30: off += uleb(&p); break;
        case 0x40: off += imm * 8; break;
        case 0x50: for (n = imm; n; n--) { REBASE(); off += 8; } break;
        case 0x60: for (n = uleb(&p); n; n--) { REBASE(); off += 8; } break;
        case 0x70: REBASE(); off += 8 + uleb(&p); break;
        case 0x80: n = uleb(&p); skip = uleb(&p); while (n--) { REBASE(); off += 8 + skip; } break;
        default: LOGI("bad rebase opcode %#x", op); return;
        }
    }
#undef REBASE
}

static int prot_of(int vmprot) {
    return ((vmprot & 1) ? PROT_READ : 0) | ((vmprot & 2) ? PROT_WRITE : 0) | ((vmprot & 4) ? PROT_EXEC : 0);
}

int mk_load(const char *path, mk_resolver resolve, mk_image *img) {
    memset(img, 0, sizeof(*img));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { LOGI("open %s: %s", path, strerror(errno)); return -1; }
    struct stat st;
    fstat(fd, &st);
    img->file_size = st.st_size;
    img->file = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (img->file == MAP_FAILED) { LOGI("mmap file: %s", strerror(errno)); return -1; }
    const mach_header_64 *mh = (const mach_header_64 *)img->file;
    if (mh->magic != MH_MAGIC_64 || mh->cputype != 0x0100000c) { LOGI("not an arm64 Mach-O"); return -1; }

    bind_ctx c = { .img = img, .resolve = resolve };
    dyld_info_command *di = NULL;
    uint64_t lo = UINT64_MAX, hi = 0;
    const uint8_t *lc = img->file + sizeof(*mh);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const load_command *l = (const load_command *)lc;
        if (l->cmd == LC_SEGMENT_64) {
            segment_command_64 *s = (segment_command_64 *)lc;
            if (strcmp(s->segname, "__PAGEZERO") && c.nsegs < MAX_SEGS) {
                c.segs[c.nsegs++] = s;
                if (s->vmaddr < lo) lo = s->vmaddr;
                if (s->vmaddr + s->vmsize > hi) hi = s->vmaddr + s->vmsize;
            } else if (c.nsegs < MAX_SEGS) {
                c.segs[c.nsegs++] = s;  // keep ordinals aligned with segment indexes
            }
        } else if (l->cmd == LC_DYLD_INFO_ONLY) {
            di = (dyld_info_command *)lc;
        } else if (l->cmd == LC_SYMTAB) {
            const uint32_t *v = (const uint32_t *)lc;
            img->symoff = v[2]; img->nsyms = v[3]; img->stroff = v[4]; img->strsize = v[5];
        } else if (l->cmd == LC_LOAD_DYLIB || l->cmd == LC_LOAD_WEAK_DYLIB || l->cmd == LC_REEXPORT_DYLIB ||
                   l->cmd == LC_LOAD_UPWARD_DYLIB) {
            uint32_t noff = ((const uint32_t *)lc)[2];
            const char *n = (const char *)lc + noff, *slash = strrchr(n, '/');
            if (c.ndylibs < MAX_DYLIBS) c.dylibs[c.ndylibs++] = slash ? slash + 1 : n;
        }
        lc += l->cmdsize;
    }
    if (!di) { LOGI("no LC_DYLD_INFO_ONLY (chained fixups are not supported)"); return -1; }

    img->size = hi - lo;
    uint8_t *base = mmap(NULL, img->size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { LOGI("reserve: %s", strerror(errno)); return -1; }
    img->base = base - lo;
    img->slide = (intptr_t)img->base;

    long pg = sysconf(_SC_PAGESIZE);
    for (int i = 0; i < c.nsegs; i++) {
        segment_command_64 *s = c.segs[i];
        if (!strcmp(s->segname, "__PAGEZERO") || !s->vmsize) continue;
        uint8_t *at = img->base + s->vmaddr;
        if (mprotect(at, s->vmsize, PROT_READ | PROT_WRITE)) { LOGI("mprotect: %s", strerror(errno)); return -1; }
        if (s->filesize) {
            int text = (s->initprot & 4) != 0;
            // __TEXT is mapped straight from the file (keeps it shareable and lets shell exec it);
            // writable segments get a private copy so rebases/binds can be written.
            if (text) {
                size_t len = (s->filesize + pg - 1) & ~(pg - 1);
                if (mmap(at, len, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd, s->fileoff) == MAP_FAILED) {
                    LOGI("map %s: %s", s->segname, strerror(errno));
                    return -1;
                }
            } else {
                memcpy(at, img->file + s->fileoff, s->filesize);
            }
        }
        LOGI("segment %-14s %p-%p file %#llx+%#llx", s->segname, at, at + s->vmsize,
             (unsigned long long)s->fileoff, (unsigned long long)s->filesize);
        const section_64 *sec = (const section_64 *)(s + 1);
        for (uint32_t k = 0; k < s->nsects; k++, sec++) {
            if (!strncmp(sec->sectname, "__mod_init_func", 16)) { img->init_addr = sec->addr; img->init_count = sec->size / 8; }
            else if (!strncmp(sec->sectname, "__unwind_info", 16)) { img->unwind_info = sec->addr; img->unwind_info_size = sec->size; }
            else if (!strncmp(sec->sectname, "__eh_frame", 16)) { img->eh_frame = sec->addr; img->eh_frame_size = sec->size; }
            else if (!strncmp(sec->sectname, "__text", 16)) { img->text_lo = sec->addr; if (!img->text_hi) img->text_hi = sec->addr + sec->size; }
            else if (!strncmp(sec->sectname, "il2cpp", 16)) { img->text_hi = sec->addr + sec->size; }
        }
    }
    img->export_off = di->export_off;
    img->export_size = di->export_size;

    run_rebase_opcodes(&c, img->file + di->rebase_off, img->file + di->rebase_off + di->rebase_size);
    run_bind_opcodes(&c, img->file + di->bind_off, img->file + di->bind_off + di->bind_size, 0);
    run_bind_opcodes(&c, img->file + di->lazy_bind_off, img->file + di->lazy_bind_off + di->lazy_bind_size, 1);
    LOGI("bound imports: %u functions and %u data symbols have no implementation yet",
         c.missing_func, c.missing_data);

    for (int i = 0; i < c.nsegs; i++) {
        segment_command_64 *s = c.segs[i];
        if (!strcmp(s->segname, "__PAGEZERO") || !s->vmsize || (s->initprot & 4)) continue;
        mprotect(img->base + s->vmaddr, s->vmsize, prot_of(s->initprot) | PROT_READ);
    }
    close(fd);
    return 0;
}

void mk_run_initializers(mk_image *img, int (*filter)(mk_image *, uint64_t, unsigned)) {
    typedef void (*init_fn)(int, const char **, const char **, const char **, void *);
    static const char *argv[] = { "UnityFramework", 0 }, *envp[] = { 0 }, *apple[] = { 0 };
    uint64_t *fns = (uint64_t *)(img->base + img->init_addr);
    for (unsigned i = 0; i < img->init_count; i++) {
        uint64_t vm = fns[i] - (uint64_t)img->slide;
        if (filter && !filter(img, vm, i)) continue;
        ((init_fn)(uintptr_t)fns[i])(1, argv, envp, apple, NULL);
    }
}

void *mk_export(const mk_image *img, const char *name) {
    const uint8_t *start = img->file + img->export_off, *end = start + img->export_size, *p = start;
    const char *s = name;
    for (;;) {
        const uint8_t *q = p;
        uint64_t term = uleb(&q);
        if (*s == 0 && term) {
            uint64_t flags = uleb(&q);
            if (flags & 8) return NULL;  // re-export
            return img->base + uleb(&q);
        }
        q += term;
        uint8_t n = *q++;
        const uint8_t *next = NULL;
        for (uint8_t i = 0; i < n; i++) {
            const char *edge = (const char *)q;
            size_t len = strlen(edge);
            q += len + 1;
            uint64_t child = uleb(&q);
            if (!next && strncmp(s, edge, len) == 0) { next = start + child; s += len; }
        }
        if (!next || next >= end) return NULL;
        p = next;
    }
}

const char *mk_symbolicate(const mk_image *img, uintptr_t addr, uintptr_t *off) {
    uint64_t vm = addr - (uintptr_t)img->slide, best = 0;
    const char *bn = NULL;
    const uint8_t *syms = img->file + img->symoff;
    const char *strs = (const char *)img->file + img->stroff;
    for (uint32_t i = 0; i < img->nsyms; i++) {
        const uint8_t *e = syms + 16 * i;
        uint32_t strx = *(const uint32_t *)e;
        uint8_t type = e[4];
        uint64_t value = *(const uint64_t *)(e + 8);
        if ((type & 0x0e) != 0x0e || type & 0xe0) continue;  // defined in a section, not a stab
        if (value <= vm && value >= best) { best = value; bn = strs + strx; }
    }
    if (off) *off = (uintptr_t)(vm - best);
    return bn;
}
