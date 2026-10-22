/* Build like scarlet_ipc_test.c. Exercises real Windows page protection. */
#include <assert.h>
#include "../runtime/scarlet_memory.c"
void trace_printf(const char *fmt, ...) { (void)fmt; }
int main(void) {
    /* Match boot.c: reserve the guest stack arena before exercising MapMemory. */
    unsigned char *boot = scarlet_bootstrap_stack_reserve(0x120000);
    assert(boot);
    /* Guest-free pages inside a larger host reservation can hold shared memory. */
    void *arena = VirtualAlloc((void *)(uintptr_t)0x7060000000ULL, 0x20000, MEM_RESERVE, PAGE_NOACCESS);
    assert(arena);
    uint64_t shared_at = (uintptr_t)arena + 0x3000;
    assert(shared_vm_map(shared_at, 0x1000));
    uint64_t guard_info[5];
    assert(scarlet_query_free_region((uintptr_t)arena - 0x1000, guard_info));
    assert(guard_info[0] + guard_info[1] == shared_at);
    assert(scarlet_query_free_region((uintptr_t)arena + 0x1000, guard_info));
    assert(guard_info[0] + guard_info[1] == shared_at);
    *(volatile unsigned char *)(uintptr_t)shared_at = 42;
    shared_vm_unmap(shared_at, 0x1000);
    MEMORY_BASIC_INFORMATION reserved_info;
    VirtualQuery(arena, &reserved_info, sizeof reserved_info);
    assert(reserved_info.State == MEM_RESERVE);
    assert(VirtualFree(arena, 0, MEM_RELEASE));
    unsigned char *src = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(src); memset(src, 0x5a, 0x10000);
    uint64_t from = (uintptr_t)src, dst = STACK_BASE + 0x5000;
    Ctx c = {0}; c.x[0] = dst; c.x[1] = from; c.x[2] = 0x1000;
    assert(scarlet_memory_svc(&c, 4) && c.x[0] == 0);
    MEMORY_BASIC_INFORMATION info;
    VirtualQuery(src, &info, sizeof info); assert(info.Protect == PAGE_NOACCESS);
    assert(*(unsigned char *)(uintptr_t)dst == 0x5a);
    uint64_t mi[5] = {0}; c.x[0] = (uintptr_t)mi; c.x[2] = dst;
    assert(scarlet_memory_svc(&c, 6) && mi[0] == dst && mi[1] == 0x1000 && mi[2] == 0xb && mi[3] == 3);
    *(unsigned char *)(uintptr_t)dst = 0xa5;
    /* A second 4KiB map sharing the same 64KiB reservation must also succeed. */
    c.x[0] = dst + 0x1000; c.x[1] = from + 0x1000; c.x[2] = 0x1000;
    scarlet_memory_svc(&c, 4); assert(c.x[0] == 0);
    c.x[0] = dst; c.x[1] = from + 0x2000; c.x[2] = 0x1000;
    scarlet_memory_svc(&c, 4); assert(c.x[0] != 0); /* destination overlap */
    c.x[0] = dst; c.x[1] = from; c.x[2] = 0x1000;
    scarlet_memory_svc(&c, 5); assert(c.x[0] == 0 && src[0] == 0xa5);
    VirtualQuery((void *)(uintptr_t)dst, &info, sizeof info); assert(info.State == MEM_RESERVE);
    memset(mi, 0xff, sizeof mi);
    assert(scarlet_query_free_region(dst, mi));
    assert(mi[0] <= dst && mi[0] + mi[1] > dst && mi[2] == 0 && mi[3] == 0);
    c.x[0] = dst + 0x1000; c.x[1] = from + 0x1000; c.x[2] = 0x1000;
    scarlet_memory_svc(&c, 5); assert(c.x[0] == 0);
    assert(scarlet_query_free_region(STACK_BASE + 0x100000, mi));
    assert(mi[1] > 0x10000); /* large SDK stack searches need the full gap */
    c.x[0] = dst + 1; c.x[1] = from; c.x[2] = 0x1000;
    scarlet_memory_svc(&c, 4); assert(c.x[0] == 0xcc01);
    c.x[0] = dst; c.x[1] = from; c.x[2] = 0;
    scarlet_memory_svc(&c, 4); assert(c.x[0] == 0xca01);
    VirtualFree(src, 0, MEM_RELEASE);
    /* Time/HID can choose a 4KiB page inside a shared 64KiB reservation. */
    uint32_t time = scarlet_sharedmem_create(0x1000, "time", 5);
    uint32_t hid = scarlet_sharedmem_create(0x2000, "hid", 4);
    assert(time && hid);
    uint64_t shared = 0x7066ea5000ULL;
    assert(shared_map(time, shared, 0x1000, 0) == 0);
    assert(shared_map(hid, shared + 0x1000, 0x2000, 0) == 0);
    uint64_t shared_info[5] = {0};
    c.x[0] = (uintptr_t)shared_info; c.x[2] = shared + 0x38;
    assert(scarlet_memory_svc(&c, 6) && c.x[0] == 0);
    assert(shared_info[0] == shared && shared_info[1] == 0x1000 && shared_info[2] == 6);
    assert(!strcmp((char *)(uintptr_t)shared, "time"));
    assert(!strcmp((char *)(uintptr_t)(shared + 0x1000), "hid"));
    assert(shared_map(hid, shared + 0x1000, 0x1000, 1) != 0);
    *(char *)(uintptr_t)shared = 'T';
    assert(shared_map(time, shared, 0x1000, 1) == 0);
    assert(!strcmp((char *)(uintptr_t)(shared + 0x1000), "hid"));
    assert(shared_map(time, shared, 0x1000, 0) == 0);
    assert(*(char *)(uintptr_t)shared == 'T');
    assert(shared_close(time) && shared_close(hid));
    VirtualQuery((void *)(uintptr_t)shared, &info, sizeof info); assert(info.State == MEM_FREE);
    /* A map crossing a reservation boundary must unwind all its pages. */
    uint32_t wide = scarlet_sharedmem_create(0x40000, NULL, 0);
    assert(shared_map(wide, shared, 0x40000, 0) == 0);
    memset((void *)(uintptr_t)shared, 0x42, 0x40000);
    assert(shared_close(wide));
    VirtualQuery((void *)(uintptr_t)(shared + 0x30000), &info, sizeof info); assert(info.State == MEM_FREE);
    unsigned char *tls = scarlet_guest_alloc(0x10000);
    assert(tls && (uintptr_t)tls < (1ULL << 39));
    uint64_t address = (uintptr_t)tls;
    uint32_t d1 = (uint32_t)address;
    uint32_t d2 = (uint32_t)(((address >> 32) & 15) << 28) | (uint32_t)(((address >> 36) & 7) << 2);
    assert((d1 | ((uint64_t)(d2 >> 28) << 32) | ((uint64_t)((d2 >> 2) & 7) << 36)) == address);
    VirtualFree(tls, 0, MEM_RELEASE);
    assert(boot && (uintptr_t)boot >= STACK_BASE && (uintptr_t)boot + 0x120000 <= STACK_BASE + STACK_SIZE);
    assert(VirtualAlloc(boot + 0x10000, 0x100000, MEM_COMMIT, PAGE_READWRITE) == boot + 0x10000);
    VirtualQuery(boot, &info, sizeof info); assert(info.State == MEM_RESERVE);
    VirtualQuery(boot + 0x110000, &info, sizeof info); assert(info.State == MEM_RESERVE);
    assert(scarlet_query_free_region(STACK_BASE + 0x100000, mi) && mi[0] + mi[1] <= (uintptr_t)boot);
    /* Regression: source straddles two independent Windows reservations, as
     * 0x727bfff000 + 0x20000 did immediately before ThreadManager aborted. */
    uint64_t split = 0x7062000000ULL, alias = STACK_BASE + 0x200000;
    assert(VirtualAlloc((void *)(uintptr_t)split, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(map_memory(alias, split + 0xf000, 0x2000, 0) == 0xdc01);
    /* A later protection failure must unwind the already-protected first page. */
    assert(!stack_protect_range(split + 0xf000, 0x2000, PAGE_NOACCESS, PAGE_READWRITE));
    VirtualQuery((void *)(uintptr_t)(split + 0xf000), &info, sizeof info);
    assert(info.Protect == PAGE_READWRITE);
    assert(VirtualAlloc((void *)(uintptr_t)(split + 0x10000), 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    uint64_t spanning = split + 0xf000;
    memset((void *)(uintptr_t)spanning, 0x6b, 0x20000);
    assert(map_memory(alias, spanning, 0x20000, 0) == 0);
    VirtualQuery((void *)(uintptr_t)spanning, &info, sizeof info); assert(info.Protect == PAGE_NOACCESS);
    VirtualQuery((void *)(uintptr_t)(split + 0x10000), &info, sizeof info); assert(info.Protect == PAGE_NOACCESS);
    assert(*(unsigned char *)(uintptr_t)(alias + 0x1ffff) == 0x6b);
    memset((void *)(uintptr_t)alias, 0x7c, 0x20000);
    assert(map_memory(alias, spanning, 0x20000, 1) == 0);
    assert(*(unsigned char *)(uintptr_t)spanning == 0x7c);
    assert(*(unsigned char *)(uintptr_t)(spanning + 0x1ffff) == 0x7c);
    VirtualQuery((void *)(uintptr_t)spanning, &info, sizeof info); assert(info.Protect == PAGE_READWRITE);
    VirtualQuery((void *)(uintptr_t)(split + 0x10000), &info, sizeof info); assert(info.Protect == PAGE_READWRITE);
    VirtualFree((void *)(uintptr_t)split, 0, MEM_RELEASE);
    VirtualFree((void *)(uintptr_t)(split + 0x10000), 0, MEM_RELEASE);
    puts("Scarlet memory: 4KiB map, shared reservation, protection, copyback, query and validation passed");
    return 0;
}
