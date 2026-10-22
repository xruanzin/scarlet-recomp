/* Native ABI fixture; no ROM required. Build with -Iruntime. */
#include <assert.h>
#include <stdio.h>
#include "../runtime/scarlet_tls.h"
static unsigned char tlr[0x200], libc_tls[0x200], thread[0x400], other_thread[0x400];
static uint64_t current_tlr;
uint64_t guest_tpidrro(Ctx *c) { (void)c; return current_tlr; }
int main(void) {
    Ctx c = {0};
    c.tpidr = (uintptr_t)libc_tls;
    current_tlr = (uintptr_t)tlr;
    wr64(current_tlr + 0x1f8, (uintptr_t)thread);
    wr64((uintptr_t)libc_tls + 0x1f8, 0xdeadbeef);
    wr64((uintptr_t)thread + 0x80, 0x12345678);
    assert(scarlet_sdk_current_thread(&c) == (uintptr_t)thread);
    assert(scarlet_sdk_tls_value(&c, 0) == 0x12345678);
    scarlet_sdk_set_tls_value(&c, 5, 0xaabbccdd);
    assert(rd64((uintptr_t)thread + 0x80 + 5 * 8) == 0xaabbccdd);
    /* SDK code and the host import must see the same slot storage. */
    wr64((uintptr_t)thread + 0x80 + 5 * 8, 0x99887766);
    assert(scarlet_sdk_tls_value(&c, 5) == 0x99887766);
    wr64(current_tlr + 0x1f8, (uintptr_t)other_thread);
    assert(scarlet_sdk_current_thread(&c) == (uintptr_t)other_thread);
    assert(scarlet_sdk_tls_value(&c, 5) == 0);
    puts("Scarlet TLS: split TLR/libc TLS, ThreadType and shared slot storage passed");
    return 0;
}
