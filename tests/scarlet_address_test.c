#include <assert.h>
#include <stdio.h>
#include "../runtime/scarlet_sync.h"
static volatile LONG word;
static DWORD WINAPI wait_equal(void *unused) {
    (void)unused;
    assert(scarlet_wait_for_address((uintptr_t)&word, 2, 1, 2000000000LL) == 0);
    return 0;
}
static void registered(unsigned expected) {
    DWORD start = GetTickCount();
    for (;;) {
        unsigned count = 0;
        AcquireSRWLockShared(&scarlet_address_lock);
        for (ScarletAddressWaiter *p = scarlet_address_waiters; p; p = p->next) ++count;
        ReleaseSRWLockShared(&scarlet_address_lock);
        if (count == expected) return;
        assert(GetTickCount() - start < 2000); SwitchToThread();
    }
}
int main(void) {
    uint64_t a = (uintptr_t)&word;
    word = 1;
    assert(scarlet_wait_for_address(a, 2, 0, 0) == 0xfa01);
    assert(scarlet_wait_for_address(a, 2, 1, 0) == SCARLET_R_TIMEOUT);
    assert(scarlet_wait_for_address(a, 3, 1, 0) == 0xf001);
    assert(scarlet_signal_to_address(a, 3, 1, 1) == 0xf001);
    word = -2;
    assert(scarlet_wait_for_address(a, 1, -1, 0) == SCARLET_R_TIMEOUT && word == -3);
    assert(scarlet_wait_for_address(a, 0, -4, 0) == 0xfa01);
    word = 1;
    assert(scarlet_signal_to_address(a, 1, 0, 1) == 0xfa01 && word == 1);
    assert(scarlet_signal_to_address(a, 1, 1, 1) == 0 && word == 2);
    assert(scarlet_signal_to_address(a, 2, 2, 1) == 0 && word == 3);
    for (int i = 0; i < 300; ++i) {
        word = 1;
        HANDLE t = CreateThread(NULL, 0, wait_equal, NULL, 0, NULL);
        assert(t); registered(1);
        assert(scarlet_signal_to_address(a, 2, 1, 1) == 0 && word == 0);
        assert(WaitForSingleObject(t, 3000) == WAIT_OBJECT_0); CloseHandle(t);
    }
    word = 1;
    HANDLE both[2] = {CreateThread(NULL,0,wait_equal,NULL,0,NULL), CreateThread(NULL,0,wait_equal,NULL,0,NULL)};
    assert(both[0] && both[1]); registered(2);
    assert(scarlet_signal_to_address(a,2,1,1) == 0 && word == 1); registered(1);
    assert(scarlet_signal_to_address(a,0,0,0) == 0); /* zero means all for address arbitration */
    assert(WaitForMultipleObjects(2,both,TRUE,3000) == WAIT_OBJECT_0);
    CloseHandle(both[0]); CloseHandle(both[1]); registered(0);
    word = 1;
    both[0] = CreateThread(NULL,0,wait_equal,NULL,0,NULL);
    both[1] = CreateThread(NULL,0,wait_equal,NULL,0,NULL);
    assert(both[0] && both[1]); registered(2);
    assert(scarlet_signal_to_address(a,2,1,0) == 0 && word == -1);
    assert(WaitForMultipleObjects(2,both,TRUE,3000) == WAIT_OBJECT_0);
    CloseHandle(both[0]); CloseHandle(both[1]); registered(0);
    word = 1;
    assert(scarlet_wait_for_address(a,2,1,1000000) == SCARLET_R_TIMEOUT); registered(0);
    puts("Scarlet address arbitration: signed predicates, decrement, compare/modify, bounded wake, broadcast and 300 races passed");
    return 0;
}
