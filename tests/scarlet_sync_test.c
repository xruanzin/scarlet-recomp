/* Native Horizon synchronization stress test; link Synchronization.lib. */
#include <assert.h>
#include <stdio.h>
#include "../runtime/scarlet_sync.h"
static volatile LONG mutex_word, key_word;
static volatile LONG finished;
static DWORD WINAPI waiter_thread(void *arg) {
    uint32_t tag = (uint32_t)(uintptr_t)arg;
    scarlet_mutex_acquire_after_wait((uintptr_t)&mutex_word, tag);
    uint32_t r = scarlet_wait_process_wide_key((uintptr_t)&mutex_word, (uintptr_t)&key_word, tag, 2000000000LL);
    assert(r == 0);
    assert(((uint32_t)mutex_word & ~SCARLET_WAITERS_BIT) == tag);
    scarlet_mutex_unlock((uintptr_t)&mutex_word);
    InterlockedIncrement(&finished);
    return 0;
}
static void wait_registered(unsigned count) {
    ULONGLONG deadline = GetTickCount64() + 2000;
    for (;;) {
        unsigned n = 0;
        AcquireSRWLockShared(&scarlet_key_lock);
        for (ScarletKeyWaiter *p = scarlet_key_waiters; p; p = p->next) ++n;
        ReleaseSRWLockShared(&scarlet_key_lock);
        if (n == count) return;
        assert(GetTickCount64() < deadline);
        SwitchToThread();
    }
}
static DWORD WINAPI owner_thread(void *unused) {
    (void)unused;
    while (!((uint32_t)mutex_word & SCARLET_WAITERS_BIT)) SwitchToThread();
    scarlet_mutex_unlock((uintptr_t)&mutex_word);
    return 0;
}
int main(void) {
    mutex_word = 0x123;
    HANDLE owner = CreateThread(NULL, 0, owner_thread, NULL, 0, NULL);
    assert(owner);
    scarlet_mutex_acquire_after_wait((uintptr_t)&mutex_word, 0x124);
    assert(((uint32_t)mutex_word & ~SCARLET_WAITERS_BIT) == 0x124);
    assert(WaitForSingleObject(owner, 2000) == WAIT_OBJECT_0);
    CloseHandle(owner);
    scarlet_mutex_unlock((uintptr_t)&mutex_word);

    /* Race a producer against the waiter's release-to-sleep boundary. */
    for (int i = 0; i < 500; ++i) {
        HANDLE t = CreateThread(NULL, 0, waiter_thread, (void *)(uintptr_t)0x81, 0, NULL);
        assert(t); wait_registered(1);
        scarlet_signal_process_wide_key((uintptr_t)&key_word, 1);
        assert(WaitForSingleObject(t, 3000) == WAIT_OBJECT_0);
        CloseHandle(t);
        assert(key_word == 0 && mutex_word == 0);
    }
    finished = 0;
    HANDLE a = CreateThread(NULL, 0, waiter_thread, (void *)(uintptr_t)0x81, 0, NULL);
    HANDLE b = CreateThread(NULL, 0, waiter_thread, (void *)(uintptr_t)0x82, 0, NULL);
    assert(a && b); wait_registered(2);
    scarlet_signal_process_wide_key((uintptr_t)&key_word, 0); wait_registered(2);
    scarlet_signal_process_wide_key((uintptr_t)&key_word, 1); wait_registered(1);
    assert(key_word == 1);
    scarlet_signal_process_wide_key((uintptr_t)&key_word, -1);
    HANDLE both[] = {a, b};
    assert(WaitForMultipleObjects(2, both, TRUE, 3000) == WAIT_OBJECT_0);
    assert(finished == 2 && key_word == 0);
    CloseHandle(a); CloseHandle(b);
    mutex_word = 0x80;
    assert(scarlet_wait_process_wide_key((uintptr_t)&mutex_word, (uintptr_t)&key_word, 0x80, 0) == SCARLET_R_TIMEOUT);
    assert(mutex_word == 0 && key_word == 0); /* SDK owns timeout reacquisition */
    puts("Scarlet sync: 500 signal/wait races, bounded wake, broadcast, zero signal and timeout passed");
    return 0;
}
