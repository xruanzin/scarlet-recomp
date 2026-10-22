/* Scarlet SDK 16 ThreadType accessors, matching its lifted GetCurrentThread
 * (sdk+0x376d00) and Get/SetTlsValue (sdk+0x3799a0/0x3799c0).
 * TPIDRRO_EL0 points to the kernel TLR; TPIDR_EL0 is separate libc TLS. */
#ifndef SCARLET_TLS_H
#define SCARLET_TLS_H
#include "runtime.h"
uint64_t guest_tpidrro(Ctx *c);
static inline uint64_t scarlet_sdk_current_thread(Ctx *c) {
    uint64_t tlr = guest_tpidrro(c);
    uint64_t thread = rd64(tlr + 0x1f8);
#ifdef SCARLET_RECOMP
    /* Scarlet's main thread must always expose a guest ThreadType through the
     * kernel TLR. If original SDK bootstrap code overwrites that slot with a
     * NULL/low module-relative value, GetTlsValue immediately dereferences
     * ThreadType+0x80 and crashes at a low address. Repair only the process
     * main context; worker threads keep their own published ThreadType. */
    extern uint64_t g_main_stack_base, g_main_stack_size;
    extern uint64_t os_main_thread_type(void);
    uint64_t sp = c->x[31];
    uint64_t main_end = g_main_stack_base + g_main_stack_size;
    if (thread < 0x100000000ULL &&
        g_main_stack_size &&
        sp >= g_main_stack_base && sp <= main_end) {
        uint64_t repaired = os_main_thread_type();
        trace_printf("Scarlet TLS: repairing main ThreadType %llx -> %llx (tlr=%llx sp=%llx)\n",
                     (unsigned long long)thread,
                     (unsigned long long)repaired,
                     (unsigned long long)tlr,
                     (unsigned long long)sp);
        wr64(tlr + 0x1f8, repaired);
        thread = repaired;
    }
#endif
    return thread;
}
static inline uint64_t scarlet_sdk_tls_value(Ctx *c, uint32_t slot) {
    return rd64(scarlet_sdk_current_thread(c) + 0x80 + (uint64_t)slot * 8);
}
static inline void scarlet_sdk_set_tls_value(Ctx *c, uint32_t slot, uint64_t value) {
    wr64(scarlet_sdk_current_thread(c) + 0x80 + (uint64_t)slot * 8, value);
}
#endif
