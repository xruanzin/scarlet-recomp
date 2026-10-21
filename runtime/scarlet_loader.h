#ifndef SCARLET_LOADER_H
#define SCARLET_LOADER_H

#include <stdint.h>

enum { SCARLET_STATIC_MODULE_COUNT = 3 };

typedef struct {
    const char *name;
    uint64_t base;
    uint64_t size;
    uint64_t text_size;
    uint64_t ro_start;
    uint64_t data_start;
    uint64_t file_image;
    uint64_t dynamic;
    uint64_t rela, relasz;
    uint64_t jmprel, pltrelsz;
    uint64_t symtab, strtab, nsyms;
    uint64_t init, fini;
    uint64_t init_array, init_arraysz;
    uint64_t fini_array, fini_arraysz;
} ScarletStaticModule;

extern ScarletStaticModule g_scarlet_static_modules[SCARLET_STATIC_MODULE_COUNT];

int scarlet_load_static_modules(const char *main_elf);
uint64_t scarlet_find_static_symbol(const char *name);
const ScarletStaticModule *scarlet_module_for_address(uint64_t addr);
/* Nearest defined dynamic symbol at/below addr. Returns NULL if none; when
 * sym_addr is non-NULL, receives the guest address of that symbol. */
const char *scarlet_symbolize_static_address(uint64_t addr, uint64_t *sym_addr);

#endif
