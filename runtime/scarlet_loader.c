/* Pokémon Scarlet 1.0.0 static ExeFS loader.
 *
 * The ssbu runtime originally maps one main.elf. Scarlet has three static NSO
 * images with fixed guest bases. The lifted code uses absolute guest addresses,
 * so their data images and relocations must be present at the same addresses:
 *
 *   main     0x7000000000
 *   sdk      0x7006000000
 *   subsdk0  0x7008000000
 *
 * This loader consumes the ELF files written by tools/extract/nso2elf.py.
 * It deliberately keeps main's host-import placeholder numbering unchanged;
 * sdk/subsdk0 resolve against the static modules directly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "runtime.h"
#include "hostrt.h"
#include "scarlet_loader.h"

#define SCARLET_MAIN_BASE    0x7000000000ULL
#define SCARLET_SDK_BASE     0x7006000000ULL
#define SCARLET_SUBSDK0_BASE 0x7008000000ULL
#define SCARLET_WINDOW_SIZE  0x10000000ULL

enum {
    PT_LOAD = 1,
    SHT_DYNSYM = 11,
    DT_NULL = 0,
    DT_PLTRELSZ = 2,
    DT_HASH = 4,
    DT_STRTAB = 5,
    DT_SYMTAB = 6,
    DT_RELA = 7,
    DT_RELASZ = 8,
    DT_INIT = 12,
    DT_FINI = 13,
    DT_JMPREL = 23,
    DT_INIT_ARRAY = 25,
    DT_FINI_ARRAY = 26,
    DT_INIT_ARRAYSZ = 27,
    DT_FINI_ARRAYSZ = 28,
    R_AARCH64_ABS64 = 0x101,
    R_AARCH64_GLOB_DAT = 0x401,
    R_AARCH64_JUMP_SLOT = 0x402,
    R_AARCH64_RELATIVE = 0x403,
};

typedef struct {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
} Elf64Sym;

typedef struct {
    uint64_t off, info;
    int64_t addend;
} Elf64Rela;

ScarletStaticModule g_scarlet_static_modules[SCARLET_STATIC_MODULE_COUNT] = {
    {"main", SCARLET_MAIN_BASE},
    {"sdk", SCARLET_SDK_BASE},
    {"subsdk0", SCARLET_SUBSDK0_BASE},
};

static int read_at(FILE *f, uint64_t off, void *p, size_t n) {
#ifdef _WIN32
    if (_fseeki64(f, (__int64)off, SEEK_SET)) return 0;
#else
    if (fseeko(f, (off_t)off, SEEK_SET)) return 0;
#endif
    return fread(p, 1, n, f) == n;
}

static uint16_t u16(const unsigned char *p) {
    uint16_t v; memcpy(&v, p, sizeof v); return v;
}
static uint32_t u32(const unsigned char *p) {
    uint32_t v; memcpy(&v, p, sizeof v); return v;
}
static uint64_t u64(const unsigned char *p) {
    uint64_t v; memcpy(&v, p, sizeof v); return v;
}

static uint64_t module_limit(int index) {
    if (index + 1 < SCARLET_STATIC_MODULE_COUNT)
        return g_scarlet_static_modules[index + 1].base;
    return SCARLET_MAIN_BASE + SCARLET_WINDOW_SIZE;
}

static int load_one(int index, const char *path) {
    ScarletStaticModule *m = &g_scarlet_static_modules[index];
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Scarlet loader: cannot open %s\n", path);
        return 0;
    }

    unsigned char eh[64];
    if (!read_at(f, 0, eh, sizeof eh) || memcmp(eh, "\x7f" "ELF", 4) ||
        eh[4] != 2 || eh[5] != 1) {
        fprintf(stderr, "Scarlet loader: %s is not a little-endian ELF64 file\n", path);
        fclose(f);
        return 0;
    }

    uint64_t phoff = u64(eh + 0x20);
    uint64_t shoff = u64(eh + 0x28);
    uint16_t phentsize = u16(eh + 0x36), phnum = u16(eh + 0x38);
    uint16_t shentsize = u16(eh + 0x3a), shnum = u16(eh + 0x3c);
    if (phentsize < 56 || shentsize < 64) {
        fprintf(stderr, "Scarlet loader: %s has unexpected ELF header sizes\n", path);
        fclose(f);
        return 0;
    }

    uint64_t max_end = 0, file_image = 0;
    int loads = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        unsigned char ph[64];
        if (!read_at(f, phoff + (uint64_t)i * phentsize, ph, 56)) {
            fprintf(stderr, "Scarlet loader: short program-header read in %s\n", path);
            fclose(f);
            return 0;
        }
        if (u32(ph) != PT_LOAD) continue;
        uint64_t off = u64(ph + 8);
        uint64_t vaddr = u64(ph + 16);
        uint64_t filesz = u64(ph + 32);
        uint64_t memsz = u64(ph + 40);
        if (memsz < filesz || m->base + vaddr + memsz > module_limit(index)) {
            fprintf(stderr, "Scarlet loader: %s PT_LOAD does not fit its guest slot\n", path);
            fclose(f);
            return 0;
        }
        unsigned char *dst = (unsigned char *)(uintptr_t)(m->base + vaddr);
        if (filesz && !read_at(f, off, dst, (size_t)filesz)) {
            fprintf(stderr, "Scarlet loader: short segment read in %s\n", path);
            fclose(f);
            return 0;
        }
        if (memsz > filesz) memset(dst + filesz, 0, (size_t)(memsz - filesz));
        if (loads == 0) m->text_size = filesz;
        else if (loads == 1) m->ro_start = vaddr;
        else if (loads == 2) m->data_start = vaddr;
        if (vaddr + memsz > max_end) max_end = vaddr + memsz;
        if (vaddr + filesz > file_image) file_image = vaddr + filesz;
        loads++;
    }
    if (loads != 3) {
        fprintf(stderr, "Scarlet loader: %s has %d PT_LOAD segments; expected 3\n", path, loads);
        fclose(f);
        return 0;
    }
    m->size = (max_end + 0xfff) & ~0xfffULL;
    m->file_image = file_image;

    /* nso2elf emits normal section headers; use .dynsym's SHT_DYNSYM size
     * rather than assuming STRTAB immediately follows SYMTAB. */
    for (uint16_t i = 0; i < shnum; i++) {
        unsigned char sh[64];
        if (!read_at(f, shoff + (uint64_t)i * shentsize, sh, 64)) break;
        if (u32(sh + 4) == SHT_DYNSYM) {
            uint64_t sz = u64(sh + 32), entsz = u64(sh + 56);
            if (entsz) m->nsyms = sz / entsz;
            break;
        }
    }
    fclose(f);

    uint32_t mod0 = *(uint32_t *)(uintptr_t)(m->base + 4);
    uint64_t mod = m->base + mod0;
    if (memcmp((const void *)(uintptr_t)mod, "MOD0", 4)) {
        fprintf(stderr, "Scarlet loader: %s has no MOD0 header\n", path);
        return 0;
    }
    m->dynamic = mod + (int64_t)*(int32_t *)(uintptr_t)(mod + 4);
    for (uint64_t d = m->dynamic, n = 0; n < 4096; n++, d += 16) {
        int64_t tag = *(int64_t *)(uintptr_t)d;
        uint64_t val = *(uint64_t *)(uintptr_t)(d + 8);
        if (tag == DT_NULL) break;
        switch (tag) {
        case DT_PLTRELSZ: m->pltrelsz = val; break;
        case DT_HASH:
            if (!m->nsyms) m->nsyms = *(uint32_t *)(uintptr_t)(m->base + val + 4);
            break;
        case DT_STRTAB: m->strtab = val; break;
        case DT_SYMTAB: m->symtab = val; break;
        case DT_RELA: m->rela = val; break;
        case DT_RELASZ: m->relasz = val; break;
        case DT_INIT: m->init = val; break;
        case DT_FINI: m->fini = val; break;
        case DT_JMPREL: m->jmprel = val; break;
        case DT_INIT_ARRAY: m->init_array = val; break;
        case DT_FINI_ARRAY: m->fini_array = val; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = val; break;
        case DT_FINI_ARRAYSZ: m->fini_arraysz = val; break;
        }
    }
    if (!m->symtab || !m->strtab || !m->nsyms) {
        fprintf(stderr, "Scarlet loader: %s dynamic symbol metadata is incomplete\n", path);
        return 0;
    }

    printf("Scarlet module %-7s @ 0x%llx: image %#llx text %#llx symbols %llu\n",
           m->name, (unsigned long long)m->base, (unsigned long long)m->size,
           (unsigned long long)m->text_size, (unsigned long long)m->nsyms);
    return 1;
}

static Elf64Sym *module_sym(ScarletStaticModule *m, uint64_t i) {
    if (i >= m->nsyms) return NULL;
    return (Elf64Sym *)(uintptr_t)(m->base + m->symtab + i * sizeof(Elf64Sym));
}

static const char *module_sym_name(ScarletStaticModule *m, const Elf64Sym *s) {
    return (const char *)(uintptr_t)(m->base + m->strtab + s->name);
}

uint64_t scarlet_find_static_symbol(const char *name) {
    if (!name || !*name) return 0;
    for (int mi = 0; mi < SCARLET_STATIC_MODULE_COUNT; mi++) {
        ScarletStaticModule *m = &g_scarlet_static_modules[mi];
        for (uint64_t i = 1; i < m->nsyms; i++) {
            Elf64Sym *s = module_sym(m, i);
            if (s && s->shndx && !strcmp(module_sym_name(m, s), name))
                return m->base + s->value;
        }
    }
    return 0;
}

const ScarletStaticModule *scarlet_module_for_address(uint64_t addr) {
    for (int i = 0; i < SCARLET_STATIC_MODULE_COUNT; i++) {
        ScarletStaticModule *m = &g_scarlet_static_modules[i];
        if (addr >= m->base && addr < m->base + m->size) return m;
    }
    return NULL;
}

const char *scarlet_symbolize_static_address(uint64_t addr, uint64_t *sym_addr) {
    ScarletStaticModule *m = NULL;
    for (int i = 0; i < SCARLET_STATIC_MODULE_COUNT; i++) {
        ScarletStaticModule *q = &g_scarlet_static_modules[i];
        if (addr >= q->base && addr < q->base + q->size) { m = q; break; }
    }
    if (!m) return NULL;

    uint64_t off = addr - m->base, best_value = 0;
    const char *best = NULL;
    for (uint64_t i = 1; i < m->nsyms; i++) {
        Elf64Sym *s = module_sym(m, i);
        if (!s || !s->shndx || !s->name || s->value > off) continue;
        if (!best || s->value >= best_value) {
            best_value = s->value;
            best = module_sym_name(m, s);
        }
    }
    if (best && sym_addr) *sym_addr = m->base + best_value;
    return best;
}

static int main_host_override(const char *name) {
    static const char *const names[] = {
        "_ZN2nn2os16GetCurrentThreadEv",
        "_ZN2nn2os11GetTlsValueENS0_7TlsSlotE",
        "memcpy",
        "strcmp",
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(name, names[i])) return 1;
    return 0;
}

static uint64_t resolve_symbol(ScarletStaticModule *m, uint32_t symidx) {
    Elf64Sym *s = module_sym(m, symidx);
    if (!s) return 0;
    if (s->shndx) return m->base + s->value;
    const char *name = module_sym_name(m, s);

    /* main's existing host-data overrides (sdkdata.c) and the four
     * profile-validated host fast paths keep their original dynsym index. */
    if (m == &g_scarlet_static_modules[0]) {
        uint64_t override = import_address_override((int)symidx);
        if (override) return override;
        if (main_host_override(name))
            return IMPORT_BASE + (uint64_t)symidx * IMPORT_SLOT;
    }

    uint64_t target = scarlet_find_static_symbol(name);
    if (target) return target;

    if ((s->info >> 4) == 2) return 0; /* STB_WEAK */

    /* The measured Scarlet 1.0.0 module map has no strong unresolved import.
     * Keep main's old placeholder behavior as a diagnostic fallback, but never
     * invent a module-local index for sdk/subsdk0. */
    if (m == &g_scarlet_static_modules[0])
        return IMPORT_BASE + (uint64_t)symidx * IMPORT_SLOT;

    fprintf(stderr, "Scarlet loader: strong unresolved import %s in %s\n",
            name, m->name);
    return 0;
}

static int relocate_one(ScarletStaticModule *m) {
    uint64_t rel = 0, own = 0, ext = 0, weak = 0, other = 0;
    const uint64_t table[2][2] = {
        {m->rela, m->relasz},
        {m->jmprel, m->pltrelsz},
    };
    for (int t = 0; t < 2; t++) {
        if (!table[t][0] || !table[t][1]) continue;
        Elf64Rela *r = (Elf64Rela *)(uintptr_t)(m->base + table[t][0]);
        uint64_t n = table[t][1] / sizeof *r;
        for (uint64_t i = 0; i < n; i++) {
            uint32_t type = (uint32_t)r[i].info;
            uint32_t symidx = (uint32_t)(r[i].info >> 32);
            uint64_t *where = (uint64_t *)(uintptr_t)(m->base + r[i].off);
            if (type == R_AARCH64_RELATIVE) {
                *where = m->base + (uint64_t)r[i].addend;
                rel++;
                continue;
            }
            if (type == R_AARCH64_ABS64 || type == R_AARCH64_GLOB_DAT ||
                type == R_AARCH64_JUMP_SLOT) {
                Elf64Sym *s = module_sym(m, symidx);
                uint64_t target = resolve_symbol(m, symidx);
                if (s && s->shndx) own++;
                else if (!target && s && (s->info >> 4) == 2) weak++;
                else ext++;
                *where = target + (type == R_AARCH64_ABS64 ? (uint64_t)r[i].addend : 0);
                continue;
            }
            other++;
        }
    }
    printf("Scarlet relocate %-7s: relative=%llu own=%llu resolved/import=%llu weak-null=%llu other=%llu\n",
           m->name, (unsigned long long)rel, (unsigned long long)own,
           (unsigned long long)ext, (unsigned long long)weak,
           (unsigned long long)other);
    if (other) {
        fprintf(stderr, "Scarlet loader: %s has unsupported relocation types\n", m->name);
        return 0;
    }
    return 1;
}

static int module_paths(const char *main_elf, char out[3][1024]) {
    size_t n = strlen(main_elf);
    if (!n || n >= 1000) return 0;
    char dir[1024];
    memcpy(dir, main_elf, n + 1);
    char *a = strrchr(dir, '/'), *b = strrchr(dir, '\\');
    char *slash = a && (!b || a > b) ? a : b;
    if (!slash) strcpy(dir, ".");
    else *slash = 0;
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    snprintf(out[0], 1024, "%s%cmain.elf", dir, sep);
    snprintf(out[1], 1024, "%s%csdk.elf", dir, sep);
    snprintf(out[2], 1024, "%s%csubsdk0.elf", dir, sep);
    return 1;
}

int scarlet_load_static_modules(const char *main_elf) {
    void *mem = VirtualAlloc((void *)(uintptr_t)SCARLET_MAIN_BASE,
                             (SIZE_T)SCARLET_WINDOW_SIZE,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (mem != (void *)(uintptr_t)SCARLET_MAIN_BASE) {
        fprintf(stderr, "Scarlet loader: cannot map static guest window at 0x%llx\n",
                (unsigned long long)SCARLET_MAIN_BASE);
        return 0;
    }

    char paths[3][1024];
    if (!module_paths(main_elf, paths)) {
        fprintf(stderr, "Scarlet loader: bad main ELF path\n");
        return 0;
    }
    for (int i = 0; i < SCARLET_STATIC_MODULE_COUNT; i++)
        if (!load_one(i, paths[i])) return 0;

    for (int i = 0; i < SCARLET_STATIC_MODULE_COUNT; i++)
        if (!relocate_one(&g_scarlet_static_modules[i])) return 0;

    ScarletStaticModule *main = &g_scarlet_static_modules[0];
    g_main.text_size = main->text_size;
    g_main.ro_start = main->ro_start;
    g_main.data_start = main->data_start;
    g_main.file_image = main->file_image;
    g_main.rela = main->rela;
    g_main.relasz = main->relasz;
    g_main.jmprel = main->jmprel;
    g_main.pltrelsz = main->pltrelsz;
    g_main.symtab = main->symtab;
    g_main.strtab = main->strtab;
    g_main.init_array = main->init_array;
    g_main.init_arraysz = main->init_arraysz;
    g_game_build = GAME_13_0_4; /* ga() is identity for Scarlet's one supported build */

    if (!getenv("SCARLET_NO_PROTECT") && !getenv("SSBU_NO_PROTECT")) {
        for (int i = 0; i < SCARLET_STATIC_MODULE_COUNT; i++) {
            ScarletStaticModule *m = &g_scarlet_static_modules[i];
            DWORD oldp;
            if (m->data_start && !VirtualProtect((void *)(uintptr_t)m->base,
                                                  (SIZE_T)m->data_start,
                                                  PAGE_READONLY, &oldp)) {
                fprintf(stderr, "Scarlet loader: cannot protect %s read-only segments\n", m->name);
                return 0;
            }
        }
    }
    return 1;
}
