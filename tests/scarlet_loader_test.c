/* Native regression: full-SDK data must not resolve to partial-SDK host data.
 * deps/llvm/bin/clang.exe -DSCARLET_RECOMP -Iruntime -Ibuild/work
 *     tests/scarlet_loader_test.c -o build/probe/scarlet_loader_test.exe
 * No game files or lifted objects are used by this synthetic module fixture. */
#include <assert.h>
#include "../runtime/scarlet_loader.c"

MainLayout g_main;
int g_game_build;
static uint64_t host_overrides[8];
uint64_t import_address_override(int sym) { return host_overrides[sym]; }

static unsigned char main_image[2048], sdk_image[2048];
static void fixture(ScarletStaticModule *m, unsigned char *image) {
    m->base = (uintptr_t)image;
    m->symtab = 64;
    m->strtab = 512;
    m->nsyms = 8;
}
static void symbol(ScarletStaticModule *m, unsigned i, unsigned name_offset,
                   const char *name, uint16_t section, unsigned binding, uint64_t value) {
    Elf64Sym *s = module_sym(m, i);
    *s = (Elf64Sym){name_offset, (uint8_t)(binding << 4), 0, section, value, 16};
    strcpy((char *)(uintptr_t)(m->base + m->strtab + name_offset), name);
}
int main(void) {
    ScarletStaticModule *main = &g_scarlet_static_modules[0], *sdk = &g_scarlet_static_modules[1];
    fixture(main, main_image); fixture(sdk, sdk_image);
    symbol(main, 1, 1, "_ZNSt3__15ctypeIcE2idE", 0, 1, 0);
    symbol(sdk, 1, 1, "_ZNSt3__15ctypeIcE2idE", 1, 1, 1024);
    host_overrides[1] = 0x12340000;
    assert(resolve_symbol(main, 1) == sdk->base + 1024);
    symbol(main, 2, 64, "memcpy", 0, 1, 0);
    assert(resolve_symbol(main, 2) == IMPORT_BASE + 2 * IMPORT_SLOT);
    symbol(main, 3, 96, "host_only_data", 0, 1, 0);
    host_overrides[3] = 0x56780000;
    assert(resolve_symbol(main, 3) == host_overrides[3]);
    symbol(main, 4, 128, "own_data", 1, 1, 1200);
    host_overrides[4] = 0xdeadbeef;
    assert(resolve_symbol(main, 4) == main->base + 1200);
    symbol(main, 5, 160, "missing_weak", 0, 2, 0);
    assert(resolve_symbol(main, 5) == 0);
    symbol(main, 6, 192, "_ZNSt3__18numpunctIcE2idE", 0, 1, 0);
    symbol(sdk, 2, 64, "_ZNSt3__18numpunctIcE2idE", 1, 1, 1100);
    host_overrides[6] = 0x99990000;
    assert(resolve_symbol(main, 6) == sdk->base + 1100);
    symbol(main, 7, 240, "_ZN2nn2vi10InitializeEv", 0, 1, 0);
    symbol(sdk, 3, 128, "_ZN2nn2vi10InitializeEv", 1, 1, 1300);
    assert(resolve_symbol(main, 7) == IMPORT_BASE + 7 * IMPORT_SLOT);
    puts("Scarlet loader: SDK facet identity, host function/fallback, own and weak symbols passed");
    return 0;
}
