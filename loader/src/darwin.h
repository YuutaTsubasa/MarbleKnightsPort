// Darwin (iOS) compatibility layer shared declarations.
#pragma once
#include "macho_loader.h"

typedef struct { const char *name; void *addr; } darwin_sym;

void *darwin_lookup(const darwin_sym *table, const char *name);
void *darwin_libsystem(const char *name);
void *darwin_libcxx(const char *name);
void *darwin_corefoundation(const char *name);
// Resolver handed to mk_load: picks the table by the importing dylib.
void *darwin_resolve(const char *name, const char *dylib, int weak);

void darwin_init(mk_image *img);
void darwin_cxx_init(mk_image *img);

extern mk_image *g_darwin_image;
extern const char *g_darwin_exe_path;
