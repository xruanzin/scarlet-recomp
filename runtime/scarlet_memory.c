/* Horizon MapMemory stack aliases. While mapped, the original is inaccessible;
 * copying on map/unmap therefore preserves the accessible contents. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "hostrt.h"
#define STACK_BASE 0x7200000000ULL
#define STACK_SIZE 0x40000000ULL
static SRWLOCK map_lock = SRWLOCK_INIT;
static void *stack_reservation;
typedef struct { uint64_t dst, src, size; DWORD protection; } StackMap;
static StackMap stack_maps[256];

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
