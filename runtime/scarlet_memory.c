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
static uint64_t bootstrap_stack_base, bootstrap_stack_size;

/* HIPC descriptors carry only 39 address bits. ASLR can place VirtualAlloc(NULL)
 * above that limit, so guest-visible bootstrap pointers need low reservations. */
static SRWLOCK guest_alloc_lock = SRWLOCK_INIT;
static uint64_t guest_alloc_next = 0x7080000000ULL;
void *scarlet_guest_alloc(size_t size) {
    uint64_t rounded = ((uint64_t)size + 0xffff) & ~0xffffULL;
    if (!size || rounded < size || rounded > 0x10000000) return NULL;
    AcquireSRWLockExclusive(&guest_alloc_lock);
    void *p = NULL;
    while (guest_alloc_next <= 0x7090000000ULL - rounded) {
        uint64_t addr = guest_alloc_next;
        guest_alloc_next += rounded;
        p = VirtualAlloc((void *)(uintptr_t)addr, (SIZE_T)rounded, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) break;
    }
    ReleaseSRWLockExclusive(&guest_alloc_lock);
    return p;
}
void *scarlet_bootstrap_stack_reserve(size_t size) {
    if (!size || size > STACK_SIZE || (size & 0xfff)) return NULL;
    size = (size + 0xffff) & ~(size_t)0xffff;
    AcquireSRWLockExclusive(&map_lock);
    if (!stack_reservation)
        stack_reservation = VirtualAlloc((void *)(uintptr_t)STACK_BASE, STACK_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    void *p = NULL;
    if (stack_reservation && !bootstrap_stack_size) {
        bootstrap_stack_size = size;
        bootstrap_stack_base = STACK_BASE + STACK_SIZE - size;
        p = (void *)(uintptr_t)bootstrap_stack_base;
    }
    ReleaseSRWLockExclusive(&map_lock);
    return p;
}
typedef struct { uint64_t dst, src, size; DWORD protection; } StackMap;
static StackMap stack_maps[256];

/* Horizon's virtual-address allocator asks for a contiguous free span before
 * issuing MapMemory. A Windows reservation without committed pages is still
 * guest-free; returning a fabricated 64KiB chunk rejects larger thread stacks. */
int scarlet_query_free_region(uint64_t addr, uint64_t info[5]) {
    MEMORY_BASIC_INFORMATION mi;
    const uint64_t address_limit = 1ULL << 39;
    if (addr >= address_limit || !VirtualQuery((void *)(uintptr_t)addr, &mi, sizeof mi) || mi.State == MEM_COMMIT) return 0;
    uint64_t base = (uint64_t)(uintptr_t)mi.BaseAddress;
    uint64_t end = base + mi.RegionSize;
    /* Windows splits MEM_FREE and MEM_RESERVE at allocation boundaries. Both
     * are guest-free until committed; Horizon's guard-range check must see a
     * single gap even when its 16KiB guard straddles a 64KiB reservation edge. */
    MEMORY_BASIC_INFORMATION adjacent;
    if (end > address_limit) end = address_limit;
    while (end < address_limit && VirtualQuery((void *)(uintptr_t)end, &adjacent, sizeof adjacent) && adjacent.State != MEM_COMMIT) {
        uint64_t next = (uint64_t)(uintptr_t)adjacent.BaseAddress + adjacent.RegionSize;
        if (next <= end) break;
        end = next > address_limit ? address_limit : next;
    }
    uint64_t size = end - base;
    if (bootstrap_stack_size && addr < bootstrap_stack_base && base + size > bootstrap_stack_base)
        size = bootstrap_stack_base - base;
    memset(info, 0, 5 * sizeof *info);
    info[0] = base; info[1] = size;
    return 1;
}

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

/* Horizon transfer-memory objects. Unlike SharedMemory, the backing pages are
 * owned by the guest process itself; the kernel object only lends that range to
 * another service/process. Scarlet creates one 8 MiB Perm_None object for
 * audren:u during boot. Keep the source address and expose optional alias
 * mapping for SVC 0x51/0x52 so later services can use the same object model. */
#define TMEM_BASE 0x40000u
#define TMEM_LIMIT 64
typedef struct {
    uint64_t src, size, mapped_addr;
    uint32_t owner_perm, mapped_perm;
} TransferMem;
static TransferMem tmem[TMEM_LIMIT];
static SRWLOCK tmem_lock = SRWLOCK_INIT;
static void shared_vm_unmap(uint64_t addr, uint64_t size);
static int shared_vm_map(uint64_t addr, uint64_t size);

static TransferMem *transfer_lookup(uint32_t h) {
    if (h < TMEM_BASE || h >= TMEM_BASE + TMEM_LIMIT) return NULL;
    TransferMem *m = &tmem[h - TMEM_BASE];
    return m->size ? m : NULL;
}
static uint32_t transfer_create(uint64_t src, uint64_t size, uint32_t perm, uint32_t *out_h) {
    if (!out_h) return 0xd201;

    /* Match the proven scarlet_pc kernel model: CreateTransferMemory itself
     * does not reject the guest backing range. Horizon creates a kernel object
     * that refers to the caller-owned pages; any later mapping operation is
     * where address/size/permission validation matters. The stricter host-VM
     * checks previously used here were artificial and made Scarlet's valid
     * 8 MiB audren work buffer fail with Result 0xdc01. */
    unsigned slot = 0;
    while (slot < TMEM_LIMIT && tmem[slot].size) ++slot;
    if (slot == TMEM_LIMIT) return 0xd201;

    tmem[slot] = (TransferMem){src, size, 0, perm, 0};
    *out_h = TMEM_BASE + slot;
    return 0;
}

static int transfer_close(uint32_t h) {
    TransferMem *m = transfer_lookup(h);
    if (!m) return 0;
    if (m->mapped_addr) {
        if (m->mapped_perm & 2)
            memcpy((void *)(uintptr_t)m->src, (void *)(uintptr_t)m->mapped_addr, (size_t)m->size);
        shared_vm_unmap(m->mapped_addr, m->size);
    }
    memset(m, 0, sizeof *m);
    return 1;
}

/* VirtualAlloc reservations require 64KiB alignment, while Horizon accepts
 * any 4KiB-aligned shared-memory address. Several guest objects may occupy
 * different pages of one Windows reservation. Track page ownership so closing
 * one mapping cannot release an adjacent object's reservation. */
typedef struct { uint64_t base; uint32_t pages; int owned; } SharedReservation;
static SharedReservation shared_reservations[256];
static void shared_vm_unmap(uint64_t addr, uint64_t size) {
    while (size) {
        uint64_t base = addr & ~0xffffULL;
        uint64_t chunk = 0x10000 - (addr - base);
        if (chunk > size) chunk = size;
        unsigned first = (unsigned)((addr - base) >> 12), count = (unsigned)(chunk >> 12);
        uint32_t mask = ((1u << count) - 1u) << first;
        for (unsigned i = 0; i < 256; ++i) if (shared_reservations[i].base == base) {
            VirtualFree((void *)(uintptr_t)addr, (SIZE_T)chunk, MEM_DECOMMIT);
            shared_reservations[i].pages &= ~mask;
            if (!shared_reservations[i].pages) {
                if (shared_reservations[i].owned) VirtualFree((void *)(uintptr_t)base, 0, MEM_RELEASE);
                memset(&shared_reservations[i], 0, sizeof shared_reservations[i]);
            }
            break;
        }
        addr += chunk; size -= chunk;
    }
}
static int shared_vm_map(uint64_t addr, uint64_t size) {
    uint64_t begin = addr, mapped = 0;
    while (size) {
        uint64_t base = addr & ~0xffffULL;
        uint64_t chunk = 0x10000 - (addr - base);
        if (chunk > size) chunk = size;
        unsigned first = (unsigned)((addr - base) >> 12), count = (unsigned)(chunk >> 12);
        uint32_t mask = ((1u << count) - 1u) << first;
        SharedReservation *slot = NULL, *empty = NULL;
        for (unsigned i = 0; i < 256; ++i) {
            if (shared_reservations[i].base == base) { slot = &shared_reservations[i]; break; }
            if (!shared_reservations[i].base && !empty) empty = &shared_reservations[i];
        }
        int reserved = 0;
        if (!slot && empty) {
            void *p = VirtualAlloc((void *)(uintptr_t)base, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
            if (p == (void *)(uintptr_t)base) { slot = empty; slot->base = base; slot->owned = 1; reserved = 1; }
            else {
                MEMORY_BASIC_INFORMATION mi;
                if (VirtualQuery((void *)(uintptr_t)addr, &mi, sizeof mi) && mi.State == MEM_RESERVE &&
                    (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize >= addr + chunk) {
                    slot = empty; slot->base = base; slot->owned = 0;
                }
            }
        }
        if (!slot || (slot->pages & mask) ||
            !VirtualAlloc((void *)(uintptr_t)addr, (SIZE_T)chunk, MEM_COMMIT, PAGE_READWRITE)) {
            if (reserved) { VirtualFree((void *)(uintptr_t)base, 0, MEM_RELEASE); slot->base = 0; }
            shared_vm_unmap(begin, mapped);
            return 0;
        }
        slot->pages |= mask;
        addr += chunk; size -= chunk; mapped += chunk;
    }
    return 1;
}

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
    if (!m || !addr || (addr & 0xfff) || !size || (size & 0xfff) || size != m->size || addr + size < addr) return 0xe401;
    if (unmap) {
        if (m->mapped_addr != addr) return 0xdc01;
        memcpy(m->initial, (void *)(uintptr_t)addr, (size_t)size);
        shared_vm_unmap(addr, size);
        m->mapped_addr = 0;
        return 0;
    }
    if (m->mapped_addr && m->mapped_addr != addr) return 0xdc01;
    if (!m->mapped_addr) {
        if (!shared_vm_map(addr, size)) return 0xd001;
        memcpy((void *)(uintptr_t)addr, m->initial, (size_t)size);
        m->mapped_addr = addr;
    }
    return 0;
}
static int shared_close(uint32_t h) {
    SharedMem *m = shared_lookup(h);
    if (!m) return 0;
    if (m->mapped_addr) shared_vm_unmap(m->mapped_addr, m->size);
    free(m->initial);
    memset(m, 0, sizeof *m);
    return 1;
}

static uint32_t map_memory(uint64_t dst, uint64_t src, uint64_t size, int unmap) {
    if ((dst | src) & 0xfff) return 0xcc01;
    if (!size || (size & 0xfff)) return 0xca01;
    if (dst < STACK_BASE || size > STACK_SIZE || dst - STACK_BASE > STACK_SIZE - size || src + size < src) return 0xdc01;
    if (bootstrap_stack_size && dst < bootstrap_stack_base + bootstrap_stack_size && dst + size > bootstrap_stack_base) return 0xdc01;
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
    if (n == 0x15) { /* CreateTransferMemory(_, source, size, permission) -> result, handle */
        uint64_t src = c->x[1], size = c->x[2];
        uint32_t perm = (uint32_t)c->x[3], h = 0;
        AcquireSRWLockExclusive(&tmem_lock);
        uint32_t r = transfer_create(src, size, perm, &h);
        ReleaseSRWLockExclusive(&tmem_lock);
        c->x[0] = r; c->x[1] = r ? 0 : h;
        trace_printf("Scarlet CreateTransferMemory src=%llx size=%llx perm=%u -> %x result=%x\n",
                     (unsigned long long)src, (unsigned long long)size, perm, h, r);
        printf("Scarlet CreateTransferMemory src=%llx size=%llx perm=%u -> handle=%x result=%x\n",
               (unsigned long long)src, (unsigned long long)size, perm, h, r);
        return 1;
    }
    if (n == 0x51 || n == 0x52) { /* Map/UnmapTransferMemory(handle, addr, size[, perm]) */
        uint32_t h = (uint32_t)c->x[0];
        uint64_t addr = c->x[1], size = c->x[2];
        uint32_t perm = (uint32_t)c->x[3], r = 0;
        AcquireSRWLockExclusive(&tmem_lock);
        TransferMem *m = transfer_lookup(h);
        if (!m || !addr || (addr & 0xfff) || !size || (size & 0xfff) || size != (m ? m->size : 0)) {
            r = 0xe401;
        } else if (n == 0x51) {
            if (perm != 0 && perm != 1 && perm != 3) r = 0xd801;
            else if (m->mapped_addr && m->mapped_addr != addr) r = 0xdc01;
            else if (!m->mapped_addr) {
                if (!shared_vm_map(addr, size)) r = 0xd001;
                else {
                    memcpy((void *)(uintptr_t)addr, (void *)(uintptr_t)m->src, (size_t)size);
                    m->mapped_addr = addr; m->mapped_perm = perm;
                }
            }
        } else {
            if (m->mapped_addr != addr) r = 0xdc01;
            else {
                if (m->mapped_perm & 2)
                    memcpy((void *)(uintptr_t)m->src, (void *)(uintptr_t)addr, (size_t)size);
                shared_vm_unmap(addr, size);
                m->mapped_addr = 0; m->mapped_perm = 0;
            }
        }
        ReleaseSRWLockExclusive(&tmem_lock);
        c->x[0] = r;
        trace_printf("Scarlet %sTransferMemory h=%x addr=%llx size=%llx perm=%u -> %x\n",
                     n == 0x51 ? "Map" : "Unmap", h, (unsigned long long)addr,
                     (unsigned long long)size, perm, r);
        return 1;
    }
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
        AcquireSRWLockExclusive(&tmem_lock);
        int handled = transfer_close(h);
        ReleaseSRWLockExclusive(&tmem_lock);
        if (handled) { c->x[0] = 0; return 1; }
        AcquireSRWLockExclusive(&shmem_lock);
        handled = shared_close(h);
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
        AcquireSRWLockShared(&tmem_lock);
        for (unsigned i = 0; i < TMEM_LIMIT; ++i) {
            TransferMem *m = &tmem[i];
            if (m->size && addr >= m->src && addr - m->src < m->size) {
                /* Source remains its normal heap mapping but is marked Borrowed,
                 * with the permission requested by CreateTransferMemory. */
                uint64_t info[5] = {m->src, m->size, 5 | (1ULL << 32), m->owner_perm, 0};
                memcpy((void *)(uintptr_t)c->x[0], info, sizeof info);
                c->x[0] = 0; c->x[1] = 0; handled = 1; break;
            }
            if (m->mapped_addr && addr >= m->mapped_addr && addr - m->mapped_addr < m->size) {
                uint64_t info[5] = {m->mapped_addr, m->size,
                                    m->owner_perm ? 0x0e : 0x0d, m->mapped_perm, 0};
                memcpy((void *)(uintptr_t)c->x[0], info, sizeof info);
                c->x[0] = 0; c->x[1] = 0; handled = 1; break;
            }
        }
        ReleaseSRWLockShared(&tmem_lock);
        AcquireSRWLockShared(&shmem_lock);
        for (unsigned i = 0; !handled && i < SHMEM_LIMIT; ++i) {
            SharedMem *m = &shmem[i];
            if (m->mapped_addr && addr >= m->mapped_addr && addr - m->mapped_addr < m->size) {
                uint64_t info[5] = {m->mapped_addr, m->size, 6, 3, 0};
                memcpy((void *)(uintptr_t)c->x[0], info, sizeof info);
                c->x[0] = 0; c->x[1] = 0; handled = 1; break;
            }
        }
        ReleaseSRWLockShared(&shmem_lock);
        for (unsigned i = 0; !handled && i < 256; ++i) {
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
