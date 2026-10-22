/* Build like scarlet_ipc_test.c. Exercises real Windows page protection. */
#include <assert.h>
#include "../runtime/scarlet_memory.c"
void trace_printf(const char *fmt, ...) { (void)fmt; }
int main(void) {
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
    unsigned char *boot = scarlet_bootstrap_stack_reserve(0x120000);
    assert(boot && (uintptr_t)boot >= STACK_BASE && (uintptr_t)boot + 0x120000 <= STACK_BASE + STACK_SIZE);
    assert(VirtualAlloc(boot + 0x10000, 0x100000, MEM_COMMIT, PAGE_READWRITE) == boot + 0x10000);
    VirtualQuery(boot, &info, sizeof info); assert(info.State == MEM_RESERVE);
    VirtualQuery(boot + 0x110000, &info, sizeof info); assert(info.State == MEM_RESERVE);
    assert(scarlet_query_free_region(STACK_BASE + 0x100000, mi) && mi[0] + mi[1] <= (uintptr_t)boot);
    puts("Scarlet memory: 4KiB map, shared reservation, protection, copyback, query and validation passed");
    return 0;
}
