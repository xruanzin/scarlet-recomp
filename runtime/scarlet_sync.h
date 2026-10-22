#ifndef SCARLET_SYNC_H
#define SCARLET_SYNC_H
#include <windows.h>
#include <stdint.h>
/* Horizon mutex words are a thread handle plus bit 30 when kernel waiters
 * exist. Windows WaitOnAddress gives us the same sleep/wake primitive without
 * spinning a host CPU while the full Scarlet kernel is being brought over. */
#define SCARLET_WAITERS_BIT 0x40000000u
#define SCARLET_R_TIMEOUT   0xEA01u

static uint32_t scarlet_mutex_load(uint64_t addr) {
    return (uint32_t)InterlockedCompareExchange((volatile LONG *)(uintptr_t)addr, 0, 0);
}
static void scarlet_mutex_unlock(uint64_t addr) {
    volatile LONG *p = (volatile LONG *)(uintptr_t)addr;
    uint32_t old = (uint32_t)InterlockedExchange(p, 0);
    if (old & SCARLET_WAITERS_BIT) WakeByAddressSingle((PVOID)p);
    else WakeByAddressSingle((PVOID)p); /* harmless, and covers conservative waiter ownership */
}
static void scarlet_mutex_acquire_after_wait(uint64_t addr, uint32_t tag) {
    volatile LONG *p = (volatile LONG *)(uintptr_t)addr;
    for (;;) {
        uint32_t v = scarlet_mutex_load(addr);
        if (v == 0) {
            /* Preserve the waiter bit conservatively: if another waiter exists,
             * the eventual owner will enter SVC ArbitrateUnlock and wake it. */
            LONG want = (LONG)(tag | SCARLET_WAITERS_BIT);
            if ((uint32_t)InterlockedCompareExchange(p, want, 0) == 0) return;
            continue;
        }
        /* A userspace owner only enters ArbitrateUnlock when this bit is set.
         * Mark it before sleeping, including a woken condition-variable waiter. */
        if (!(v & SCARLET_WAITERS_BIT)) {
            if ((uint32_t)InterlockedCompareExchange(p, (LONG)(v | SCARLET_WAITERS_BIT), (LONG)v) != v) continue;
            v |= SCARLET_WAITERS_BIT;
        }
        uint32_t observed = v;
        WaitOnAddress((volatile VOID *)p, &observed, sizeof observed, INFINITE);
    }
}
static uint32_t scarlet_arbitrate_lock(uint32_t owner, uint64_t addr, uint32_t tag) {
    volatile LONG *p = (volatile LONG *)(uintptr_t)addr;
    uint32_t expected_owner = owner | SCARLET_WAITERS_BIT;
    uint32_t v = scarlet_mutex_load(addr);
    /* Horizon returns immediately when userspace changed the word before the
     * syscall entered. This race is normal. */
    if (v != expected_owner) return 0;
    for (;;) {
        v = scarlet_mutex_load(addr);
        if (v == 0) {
            LONG want = (LONG)(tag | SCARLET_WAITERS_BIT);
            if ((uint32_t)InterlockedCompareExchange(p, want, 0) == 0) return 0;
            continue;
        }
        /* A userspace owner only enters ArbitrateUnlock when this bit is set.
         * Mark it before sleeping, including a woken condition-variable waiter. */
        if (!(v & SCARLET_WAITERS_BIT)) {
            if ((uint32_t)InterlockedCompareExchange(p, (LONG)(v | SCARLET_WAITERS_BIT), (LONG)v) != v) continue;
            v |= SCARLET_WAITERS_BIT;
        }
        uint32_t observed = v;
        WaitOnAddress((volatile VOID *)p, &observed, sizeof observed, INFINITE);
    }
}
typedef struct ScarletKeyWaiter {
    uint64_t key;
    HANDLE event;
    int signaled;
    struct ScarletKeyWaiter *next;
} ScarletKeyWaiter;
static SRWLOCK scarlet_key_lock = SRWLOCK_INIT;
static ScarletKeyWaiter *scarlet_key_waiters;
static void scarlet_key_refresh(uint64_t key) {
    int waiting = 0;
    for (ScarletKeyWaiter *p = scarlet_key_waiters; p; p = p->next)
        if (p->key == key) { waiting = 1; break; }
    InterlockedExchange((volatile LONG *)(uintptr_t)key, waiting);
}
static uint32_t scarlet_wait_process_wide_key(uint64_t mutex, uint64_t key, uint32_t tag, int64_t timeout_ns) {
    ScarletKeyWaiter waiter = {key, CreateEventW(NULL, FALSE, FALSE, NULL), 0, NULL};
    if (!waiter.event) return 0xd201;
    /* Registration and mutex release are atomic with respect to signaling.
     * An event retains a signal issued before WaitForSingleObject begins. */
    AcquireSRWLockExclusive(&scarlet_key_lock);
    ScarletKeyWaiter **tail = &scarlet_key_waiters;
    while (*tail) tail = &(*tail)->next;
    *tail = &waiter;
    InterlockedExchange((volatile LONG *)(uintptr_t)key, 1);
    scarlet_mutex_unlock(mutex);
    ReleaseSRWLockExclusive(&scarlet_key_lock);
    DWORD ms = INFINITE;
    if (timeout_ns >= 0) {
        uint64_t m = ((uint64_t)timeout_ns + 999999ULL) / 1000000ULL;
        ms = m >= 0xfffffffeULL ? 0xfffffffeUL : (DWORD)m;
    }
    WaitForSingleObject(waiter.event, ms);
    AcquireSRWLockExclusive(&scarlet_key_lock);
    ScarletKeyWaiter **link = &scarlet_key_waiters;
    while (*link && *link != &waiter) link = &(*link)->next;
    if (*link) *link = waiter.next;
    int signaled = waiter.signaled;
    scarlet_key_refresh(key);
    ReleaseSRWLockExclusive(&scarlet_key_lock);
    CloseHandle(waiter.event);
    /* The SDK reacquires after timeout. Only a signaled waiter owns its mutex
     * on return from the SVC; reacquiring on timeout deadlocks the SDK caller. */
    if (!signaled) return SCARLET_R_TIMEOUT;
    scarlet_mutex_acquire_after_wait(mutex, tag);
    return 0;
}
static void scarlet_signal_process_wide_key(uint64_t key, int32_t count) {
    if (!count) return;
    AcquireSRWLockExclusive(&scarlet_key_lock);
    ScarletKeyWaiter **link = &scarlet_key_waiters;
    while (*link && count) {
        ScarletKeyWaiter *waiter = *link;
        if (waiter->key != key) { link = &waiter->next; continue; }
        *link = waiter->next;
        waiter->signaled = 1;
        SetEvent(waiter->event);
        if (count > 0) --count;
    }
    scarlet_key_refresh(key);
    ReleaseSRWLockExclusive(&scarlet_key_lock);
}

#endif
