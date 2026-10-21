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
    c.x[0] = dst + 0x1000; c.x[1] = from + 0x1000; c.x[2] = 0x1000;
    scarlet_memory_svc(&c, 5); assert(c.x[0] == 0);
    c.x[0] = dst + 1; c.x[1] = from; c.x[2] = 0x1000;
    scarlet_memory_svc(&c, 4); assert(c.x[0] == 0xcc01);
    c.x[0] = dst; c.x[1] = from; c.x[2] = 0;
    scarlet_memory_svc(&c, 4); assert(c.x[0] == 0xca01);
    VirtualFree(src, 0, MEM_RELEASE);
    puts("Scarlet memory: 4KiB map, shared reservation, protection, copyback, query and validation passed");
    return 0;
}
