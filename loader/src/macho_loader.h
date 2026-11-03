// Loads a thin arm64 Mach-O dylib (UnityFramework) into an Android process.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct mk_image {
    uint8_t *base;        // address of the mach header (vmaddr 0 of __TEXT)
    intptr_t slide;       // base - preferred __TEXT vmaddr
    size_t size;          // total mapped bytes
    const uint8_t *file;  // the whole file, mmap'ed read-only
    size_t file_size;
    // __DATA*,__mod_init_func
    uint64_t init_addr, init_count;
    // __TEXT,__unwind_info / __eh_frame / __text / il2cpp, for the unwinder and diagnostics
    uint64_t unwind_info, unwind_info_size;
    uint64_t eh_frame, eh_frame_size;
    uint64_t text_lo, text_hi;
    uint32_t export_off, export_size;
    uint32_t symoff, nsyms, stroff, strsize;
} mk_image;

// Resolves an imported symbol (with its leading underscore stripped) to an address.
// Returns NULL when there is no implementation; the loader then binds a trap stub.
typedef void *(*mk_resolver)(const char *name, const char *dylib, int weak);

int mk_load(const char *path, mk_resolver resolve, mk_image *out);
// Translates __unwind_info to DWARF and exposes it to libunwind (needed for C++ exceptions).
int mk_register_unwind(mk_image *img);
void mk_run_initializers(mk_image *img, int (*filter)(mk_image *, uint64_t vmaddr, unsigned index));
// Looks up an exported symbol (Mach-O spelling, e.g. "_il2cpp_init").
void *mk_export(const mk_image *img, const char *name);
// Nearest symbol at or before addr (exports + local symtab), for crash logs.
const char *mk_symbolicate(const mk_image *img, uintptr_t addr, uintptr_t *off);

// Missing-import bookkeeping (trap stubs).
const char *mk_trap_name(unsigned index);
extern int mk_trap_fatal;
