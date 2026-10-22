/* Host-backed nn::mem::StandardAllocator for Scarlet bootstrap.
 *
 * Scarlet's SDK initializes StandardAllocator with addr=NULL and a 12 GiB
 * virtual arena. The console implementation expects Horizon virtual-memory
 * plumbing and aborts in this native runtime. Keep returned pointers inside
 * Scarlet's guest heap window and commit pages on demand.
 *
 * This deliberately favors correctness/bring-up over allocator sophistication.
 * Free spans are reused with a first-fit list; a later performance pass can
 * replace this with the optimized caches from scarlet_pc without changing the
 * lifted HLE ABI.
 */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "runtime.h"
#include "hostrt.h"

#define STD_BASE  0x7240000000ULL
#define STD_LIMIT (STD_BASE + 0x00D0000000ULL)
#define COMMIT_GRANULE 0x01000000ULL

enum {
    STD_INIT = 0,
    STD_INIT_CACHED,
    STD_ALLOC,
    STD_ALLOC_ALIGNED,
    STD_FREE,
    STD_REALLOC,
    STD_SIZEOF,
    STD_FREE_SIZE,
    STD_FINALIZE
};

typedef struct Rec {
    uint64_t addr, size, capacity, align;
    struct Rec *next;
} Rec;
typedef struct FreeSpan {
    uint64_t addr, size;
    struct FreeSpan *next;
} FreeSpan;

static SRWLOCK g_lock = SRWLOCK_INIT;
static Rec *g_live;
static FreeSpan *g_free;
static uint64_t g_next = STD_BASE;
static uint64_t g_committed = STD_BASE;
static uint64_t g_declared = 0x10000000ULL;
static LONG g_initialized;

static uint64_t align_pow2(uint64_t a) {
    if (a < 16) return 16;
    if (!(a & (a - 1))) return a;
    uint64_t p = 16;
    while (p < a && p < (1ULL << 62)) p <<= 1;
    return p;
}
static uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}
static int ensure_committed(uint64_t end) {
    if (end <= g_committed) return 1;
    uint64_t want = align_up(end, COMMIT_GRANULE);
    if (want > STD_LIMIT) want = STD_LIMIT;
    if (want <= g_committed) return 1;

    /* SetHeapSize in this runtime may already have reserved/committed part of
     * the Scarlet heap. Do not try to MEM_RESERVE the same VA range again:
     * that made the first real StandardAllocator allocation fail and libc++
     * terminate during main DT_INIT. Walk the range and only perform the
     * operation appropriate for each Windows VM state. */
    uint64_t cur = g_committed;
    while (cur < want) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)cur, &mi, sizeof mi)) {
            fprintf(stderr, "Scarlet StandardAllocator: VirtualQuery failed at %#llx (err=%lu)\n",
                    (unsigned long long)cur, GetLastError());
            return 0;
        }
        uint64_t region_end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        uint64_t chunk_end = region_end < want ? region_end : want;
        SIZE_T bytes = (SIZE_T)(chunk_end - cur);
        void *at = (void *)(uintptr_t)cur;

        if (mi.State == MEM_COMMIT) {
            /* Already backed by SetHeapSize or an earlier allocation. */
            DWORD prot = mi.Protect & 0xff;
            if (!(prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE ||
                  prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_WRITECOPY)) {
                DWORD oldp;
                if (!VirtualProtect(at, bytes, PAGE_READWRITE, &oldp)) {
                    fprintf(stderr, "Scarlet StandardAllocator: cannot make committed heap writable at %#llx (err=%lu)\n",
                            (unsigned long long)cur, GetLastError());
                    return 0;
                }
            }
        } else if (mi.State == MEM_RESERVE) {
            void *p = VirtualAlloc(at, bytes, MEM_COMMIT, PAGE_READWRITE);
            if (p != at) {
                fprintf(stderr, "Scarlet StandardAllocator: cannot commit reserved heap %#llx..%#llx (err=%lu)\n",
                        (unsigned long long)cur, (unsigned long long)chunk_end, GetLastError());
                return 0;
            }
        } else { /* MEM_FREE */
            void *p = VirtualAlloc(at, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p != at) {
                if (p) VirtualFree(p, 0, MEM_RELEASE);
                fprintf(stderr, "Scarlet StandardAllocator: cannot reserve guest heap %#llx..%#llx (err=%lu)\n",
                        (unsigned long long)cur, (unsigned long long)chunk_end, GetLastError());
                return 0;
            }
        }
        cur = chunk_end;
    }

    g_committed = want;
    { extern void scarlet_heap_note_committed(uint64_t);
      scarlet_heap_note_committed(g_committed); }
    return 1;
}
static Rec *find_rec(uint64_t addr, Rec ***pp_out) {
    Rec **pp = &g_live;
    while (*pp) {
        if ((*pp)->addr == addr) {
            if (pp_out) *pp_out = pp;
            return *pp;
        }
        pp = &(*pp)->next;
    }
    if (pp_out) *pp_out = NULL;
    return NULL;
}
static void free_insert(uint64_t addr, uint64_t size) {
    if (!size) return;
    FreeSpan *n = (FreeSpan *)malloc(sizeof *n);
    if (!n) return;
    n->addr = addr; n->size = size; n->next = g_free; g_free = n;
}
static uint64_t alloc_locked(uint64_t size, uint64_t align) {
    if (!size) size = 1;
    if (size > UINT64_MAX - 15) return 0;
    align = align_pow2(align);
    uint64_t cap = align_up(size, 16);

    uint64_t addr = 0;
    FreeSpan **fpp = &g_free;
    while (*fpp) {
        FreeSpan *f = *fpp;
        uint64_t a = align_up(f->addr, align);
        uint64_t prefix = a - f->addr;
        if (prefix <= f->size && cap <= f->size - prefix) {
            uint64_t old_addr = f->addr, old_end = f->addr + f->size;
            *fpp = f->next;
            free(f);
            if (prefix) free_insert(old_addr, prefix);
            if (a + cap < old_end) free_insert(a + cap, old_end - (a + cap));
            addr = a;
            break;
        }
        fpp = &(*fpp)->next;
    }
    if (!addr) {
        addr = align_up(g_next, align);
        if (addr < STD_BASE || addr > STD_LIMIT || cap > STD_LIMIT - addr) return 0;
        g_next = addr + cap;
    }
    if (!ensure_committed(addr + cap)) {
        free_insert(addr, cap);
        return 0;
    }
    if (size >= 0x8000 || align >= 0x1000) {
        MEMORY_BASIC_INFORMATION mi = {0};
        VirtualQuery((void *)(uintptr_t)addr, &mi, sizeof mi);
        trace_printf("Scarlet StandardAllocator alloc addr=%llx size=%llx cap=%llx align=%llx committed_to=%llx host_state=%lx protect=%lx\n",
                     (unsigned long long)addr, (unsigned long long)size,
                     (unsigned long long)cap, (unsigned long long)align,
                     (unsigned long long)g_committed,
                     (unsigned long)mi.State, (unsigned long)mi.Protect);
        printf("Scarlet StandardAllocator alloc addr=%llx size=%llx align=%llx committed_to=%llx\n",
               (unsigned long long)addr, (unsigned long long)size,
               (unsigned long long)align, (unsigned long long)g_committed);
    }
    Rec *r = (Rec *)malloc(sizeof *r);
    if (!r) { free_insert(addr, cap); return 0; }
    r->addr = addr; r->size = size; r->capacity = cap; r->align = align;
    r->next = g_live; g_live = r;
    return addr;
}
static uint64_t alloc_guest(uint64_t size, uint64_t align) {
    AcquireSRWLockExclusive(&g_lock);
    uint64_t p = alloc_locked(size, align);
    ReleaseSRWLockExclusive(&g_lock);
    return p;
}
static void free_guest(uint64_t addr) {
    if (!addr) return;
    AcquireSRWLockExclusive(&g_lock);
    Rec **pp = NULL;
    Rec *r = find_rec(addr, &pp);
    if (r && pp) {
        *pp = r->next;
        free_insert(r->addr, r->capacity);
        free(r);
    }
    ReleaseSRWLockExclusive(&g_lock);
}
static uint64_t size_guest(uint64_t addr) {
    uint64_t n = 0;
    AcquireSRWLockShared(&g_lock);
    Rec *r = find_rec(addr, NULL);
    if (r) n = r->size;
    ReleaseSRWLockShared(&g_lock);
    return n;
}
static uint64_t realloc_guest(uint64_t addr, uint64_t size) {
    if (!addr) return alloc_guest(size, 16);
    if (!size) size = 1;

    uint64_t old_size = 0, old_align = 16;
    AcquireSRWLockExclusive(&g_lock);
    Rec *r = find_rec(addr, NULL);
    if (r) {
        old_size = r->size;
        old_align = r->align;
        if (size <= r->capacity) {
            r->size = size;
            ReleaseSRWLockExclusive(&g_lock);
            return addr;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!r) return 0;

    uint64_t n = alloc_guest(size, old_align);
    if (!n) return 0;
    memcpy((void *)(uintptr_t)n, (const void *)(uintptr_t)addr,
           (size_t)(old_size < size ? old_size : size));
    free_guest(addr);
    return n;
}
static uint64_t total_free(void) {
    uint64_t free_bytes = 0;
    AcquireSRWLockShared(&g_lock);
    for (FreeSpan *f = g_free; f; f = f->next) free_bytes += f->size;
    uint64_t tail = STD_LIMIT > g_next ? STD_LIMIT - g_next : 0;
    ReleaseSRWLockShared(&g_lock);
    return tail + free_bytes;
}

void scarlet_stdalloc_dispatch(Ctx *c, int op) {
    switch (op) {
    case STD_INIT:
    case STD_INIT_CACHED:
        g_declared = c->x[2] ? c->x[2] : 0x10000000ULL;
        InterlockedExchange(&g_initialized, 1);
        trace_printf("Scarlet StandardAllocator HLE init addr=%llx declared=%llx cached=%d pool=%llx..%llx\n",
                     (unsigned long long)c->x[1], (unsigned long long)g_declared,
                     op == STD_INIT_CACHED ? (int)(c->x[3] != 0) : 0,
                     (unsigned long long)STD_BASE, (unsigned long long)STD_LIMIT);
        return;
    case STD_ALLOC:
        c->x[0] = alloc_guest(c->x[1], 16);
        return;
    case STD_ALLOC_ALIGNED:
        c->x[0] = alloc_guest(c->x[1], c->x[2]);
        return;
    case STD_FREE:
        free_guest(c->x[1]);
        return;
    case STD_REALLOC:
        c->x[0] = realloc_guest(c->x[1], c->x[2]);
        return;
    case STD_SIZEOF:
        c->x[0] = size_guest(c->x[1]);
        return;
    case STD_FREE_SIZE:
        c->x[0] = total_free();
        return;
    case STD_FINALIZE:
        return;
    default:
        guest_trap("Scarlet StandardAllocator HLE op", (uint64_t)op);
        return;
    }
}
