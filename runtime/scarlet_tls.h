/* Scarlet SDK 16 ThreadType accessors, matching its lifted GetCurrentThread
 * (sdk+0x376d00) and Get/SetTlsValue (sdk+0x3799a0/0x3799c0).
 * TPIDRRO_EL0 points to the kernel TLR; TPIDR_EL0 is separate libc TLS. */
#ifndef SCARLET_TLS_H
#define SCARLET_TLS_H
#include "runtime.h"
uint64_t guest_tpidrro(Ctx *c);
static inline uint64_t scarlet_sdk_current_thread(Ctx *c) {
    return rd64(guest_tpidrro(c) + 0x1f8);
}
static inline uint64_t scarlet_sdk_tls_value(Ctx *c, uint32_t slot) {
    return rd64(scarlet_sdk_current_thread(c) + 0x80 + (uint64_t)slot * 8);
}
static inline void scarlet_sdk_set_tls_value(Ctx *c, uint32_t slot, uint64_t value) {
    wr64(scarlet_sdk_current_thread(c) + 0x80 + (uint64_t)slot * 8, value);
}
#endif
