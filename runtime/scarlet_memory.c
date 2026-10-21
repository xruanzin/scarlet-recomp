/* Horizon MapMemory stack aliases. While mapped, the original is inaccessible;
 * copying on map/unmap therefore preserves the accessible contents. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "hostrt.h"
#define STACK_BASE 0x7200000000ULL
#define STACK_SIZE 0x40000000ULL
static SRWLOCK map_lock = SRWLOCK_INIT;
static void *stack_reservation;
typedef struct { uint64_t dst, src, size; DWORD protection; } StackMap;
static StackMap stack_maps[256];

/* Minimal Horizon shared-memory objects for time:u / hid. */
#define SHMEM_BASE 0x30000u
#define SHMEM_LIMIT 64
typedef struct {
    uint64_t size;
    unsigned char *initial;
    uint64_t mapped_addr;
} SharedMem;
static SharedMem shmem[SHMEM_LIMIT];
static SRWLOCK shmem_lock = SRWLOCK_INIT;

uint32_t scarlet_sharedmem_create(uint64_t size, const void *initial, size_t initial_size) {
    if (!size || (size & 0xfff)) return 0;
    AcquireSRWLockExclusive(&shmem_lock);
    unsigned slot = 0;
    while (slot < SHMEM_LIMIT && shmem[slot].size) ++slot;
    if (slot == SHMEM_LIMIT) { ReleaseSRWLockExclusive(&shmem_lock); return 0; }
    unsigned char *buf = (unsigned char *)calloc(1, (size_t)size);
    if (!buf) { ReleaseSRWLockExclusive(&shmem_lock); return 0; }
    if (initial && initial_size) {
        if (initial_size > size) initial_size = (size_t)size;
        memcpy(buf, initial, initial_size);
    }
    shmem[slot].size = size;
    shmem[slot].initial = buf;
    shmem[slot].mapped_addr = 0;
    ReleaseSRWLockExclusive(&shmem_lock);
    return SHMEM_BASE + slot;
}
static SharedMem *shared_lookup(uint32_t h) {
    if (h < SHMEM_BASE || h >= SHMEM_BASE + SHMEM_LIMIT) return NULL;
    SharedMem *m = &shmem[h - SHMEM_BASE];
    return m->size ? m : NULL;
}
static uint32_t shared_map(uint32_t h, uint64_t addr, uint64_t size, int unmap) {
    SharedMem *m = shared_lookup(h);
    if (!m || !addr || (addr & 0xfff) || !size || (size & 0xfff) || size > m->size) return 0xe401;
    if (unmap) {
        if (m->mapped_addr != addr) return 0xdc01;
        VirtualFree((void *)(uintptr_t)addr, 0, MEM_RELEASE);
        m->mapped_addr = 0;
        return 0;
    }
    if (m->mapped_addr && m->mapped_addr != addr) return 0xdc01;
    if (!m->mapped_addr) {
        void *p = VirtualAlloc((void *)(uintptr_t)addr, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p != (void *)(uintptr_t)addr) {
            if (p) VirtualFree(p, 0, MEM_RELEASE);
            return 0xd001;
        }
        memcpy(p, m->initial, (size_t)size);
        m->mapped_addr = addr;
    }
    return 0;
}
static int shared_close(uint32_t h) {
    SharedMem *m = shared_lookup(h);
    if (!m) return 0;
    if (m->mapped_addr) VirtualFree((void *)(uintptr_t)m->mapped_addr, 0, MEM_RELEASE);
    free(m->initial);
    memset(m, 0, sizeof *m);
    return 1;
}

static uint32_t map_memory(uint64_t dst, uint64_t src, uint64_t size, int unmap) {
    if ((dst | src) & 0xfff) return 0xcc01;
    if (!size || (size & 0xfff)) return 0xca01;
    if (dst < STACK_BASE || size > STACK_SIZE || dst - STACK_BASE > STACK_SIZE - size || src + size < src) return 0xdc01;
    StackMap *slot = NULL;
    if (unmap) {
        for (unsigned i = 0; i < 256; ++i) if (stack_maps[i].dst == dst && stack_maps[i].src == src && stack_maps[i].size == size) {
            slot = &stack_maps[i]; break;
        }
        if (!slot) return 0xdc01;
        DWORD old;
        if (!VirtualProtect((void *)(uintptr_t)src, (SIZE_T)size, PAGE_READWRITE, &old)) return 0xd001;
        memcpy((void *)(uintptr_t)src, (void *)(uintptr_t)dst, (size_t)size);
        VirtualProtect((void *)(uintptr_t)src, (SIZE_T)size, slot->protection, &old);
        VirtualFree((void *)(uintptr_t)dst, (SIZE_T)size, MEM_DECOMMIT);
        memset(slot, 0, sizeof *slot);
        return 0;
    }
    for (unsigned i = 0; i < 256; ++i) {
        StackMap *m = &stack_maps[i];
        if (!m->size) { if (!slot) slot = m; continue; }
        if ((dst < m->dst + m->size && m->dst < dst + size) || (src < m->src + m->size && m->src < src + size)) return 0xdc01;
    }
    if (!slot) return 0xd201;
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((void *)(uintptr_t)src, &info, sizeof info) || info.State != MEM_COMMIT ||
        !(info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (info.Protect & PAGE_GUARD) ||
        src + size > (uint64_t)(uintptr_t)info.BaseAddress + info.RegionSize) return 0xdc01;
    if (!stack_reservation) {
        stack_reservation = VirtualAlloc((void *)(uintptr_t)STACK_BASE, STACK_SIZE, MEM_RESERVE, PAGE_NOACCESS);
        if (!stack_reservation) return 0xd001;
    }
    void *mapped = VirtualAlloc((void *)(uintptr_t)dst, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE);
    if (!mapped) return 0xd001;
    memcpy(mapped, (void *)(uintptr_t)src, (size_t)size);
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)src, (SIZE_T)size, PAGE_NOACCESS, &old)) {
        VirtualFree(mapped, (SIZE_T)size, MEM_DECOMMIT); return 0xd001;
    }
    *slot = (StackMap){dst, src, size, old};
    return 0;
}

int scarlet_memory_svc(Ctx *c, uint32_t n) {
    if (n == 0x13 || n == 0x14) { /* Map/UnmapSharedMemory */
        AcquireSRWLockExclusive(&shmem_lock);
        uint32_t h = (uint32_t)c->x[0];
        uint32_t r = shared_map(h, c->x[1], c->x[2], n == 0x14);
        ReleaseSRWLockExclusive(&shmem_lock);
        c->x[0] = r;
        trace_printf("Scarlet %sSharedMemory h=%x addr=%llx size=%llx -> %x\n",
                     n == 0x14 ? "Unmap" : "Map", h,
                     (unsigned long long)c->x[1], (unsigned long long)c->x[2], r);
        return 1;
    }
    if (n == 0x16) {
        uint32_t h = (uint32_t)c->x[0];
        AcquireSRWLockExclusive(&shmem_lock);
        int handled = shared_close(h);
        ReleaseSRWLockExclusive(&shmem_lock);
        if (!handled) return 0;
        c->x[0] = 0;
        return 1;
    }
    if (n != 4 && n != 5 && n != 6) return 0;
    AcquireSRWLockExclusive(&map_lock);
    int handled = 1;
    if (n == 6) {
        handled = 0;
        uint64_t addr = c->x[2];
        for (unsigned i = 0; i < 256; ++i) {
            StackMap *m = &stack_maps[i];
            if (!m->size) continue;
            int source = addr >= m->src && addr < m->src + m->size;
            if (source || (addr >= m->dst && addr < m->dst + m->size)) {
                /* MemoryInfo: base,size,state,attribute,permission,refcounts,padding. */
                uint64_t info[5] = {source ? m->src : m->dst, m->size, source ? 5 : 0xb, source ? 0 : 3, 0};
                if (source) info[2] |= 1ULL << 32; /* locked */
                memcpy((void *)(uintptr_t)c->x[0], info, sizeof info);
                c->x[0] = 0; c->x[1] = 0; handled = 1; break;
            }
        }
    } else {
        uint64_t dst = c->x[0], src = c->x[1], size = c->x[2];
        c->x[0] = map_memory(dst, src, size, n == 5);
        trace_printf("Scarlet %sMemory dst=%llx src=%llx size=%llx -> %x\n", n == 5 ? "Unmap" : "Map",
                     (unsigned long long)dst, (unsigned long long)src, (unsigned long long)size, (unsigned)c->x[0]);
    }
    ReleaseSRWLockExclusive(&map_lock);
    return handled;
}
