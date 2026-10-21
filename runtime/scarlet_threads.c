/* Kernel threads execute the already-lifted SDK entry, not nn::os HLE wrappers. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#include "hostrt.h"
#define THREAD_BASE 0x20000u
#define THREAD_LIMIT 256
#define MAIN_THREAD_HANDLE 0x80u
typedef struct {
    Ctx ctx;
    uint64_t entry, id, mask;
    int priority, core, started, refs;
    void *tls;
    HANDLE host, done;
    jmp_buf exit_target;
} ScarletThread;
static SRWLOCK thread_lock = SRWLOCK_INIT;
static ScarletThread *threads[THREAD_LIMIT];
static __declspec(thread) ScarletThread *current_thread;
static uint64_t next_id = 0x51;
static ScarletThread *lookup(uint32_t h) {
    if (h == 0xffff8000) return current_thread;
    return h >= THREAD_BASE && h < THREAD_BASE + THREAD_LIMIT ? threads[h - THREAD_BASE] : NULL;
}
uint32_t scarlet_main_thread_handle(void) { return MAIN_THREAD_HANDLE; }
static void release_thread(ScarletThread *t) {
    if (--t->refs) return;
    if (t->host) CloseHandle(t->host);
    if (t->done) CloseHandle(t->done);
    if (t->tls) VirtualFree(t->tls, 0, MEM_RELEASE);
    free(t);
}
static DWORD WINAPI run_thread(void *arg) {
    ScarletThread *t = arg;
    current_thread = t;
    trace_printf("Scarlet thread %llx entering %llx\n", (unsigned long long)t->id, (unsigned long long)t->entry);
    if (!setjmp(t->exit_target)) call_guest(&t->ctx, t->entry);
    trace_printf("Scarlet thread %llx exited\n", (unsigned long long)t->id);
    AcquireSRWLockExclusive(&thread_lock);
    SetEvent(t->done);
    current_thread = NULL;
    release_thread(t);
    ReleaseSRWLockExclusive(&thread_lock);
    return 0;
}
/* Return an owned snapshot for WaitSynchronization, safe against CloseHandle. */
HANDLE scarlet_thread_wait_handle(uint32_t h) {
    HANDLE copy = NULL;
    AcquireSRWLockShared(&thread_lock);
    ScarletThread *t = lookup(h);
    if (t) DuplicateHandle(GetCurrentProcess(), t->done, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS);
    ReleaseSRWLockShared(&thread_lock);
    return copy;
}
int scarlet_thread_svc(Ctx *c, uint32_t n) {
    uint64_t *x = c->x;
    if (n == 0x0a && current_thread) longjmp(current_thread->exit_target, 1);
    if (n == 0x10) {
        if (!current_thread) return 0;
        x[0] = current_thread->core; return 1;
    }
    if (n != 8 && n != 9 && n != 0x0c && n != 0x0d && n != 0x0e && n != 0x0f && n != 0x16 && n != 0x25) return 0;
    /* The process main thread is owned by boot.c rather than this table. Its
     * persistent kernel handle is nevertheless a valid CloseHandle target. */
    if (n == 0x16 && (uint32_t)x[0] == MAIN_THREAD_HANDLE) {
        x[0] = 0;
        return 1;
    }
    AcquireSRWLockExclusive(&thread_lock);
    int handled = 1;
    if (n == 8) {
        int priority = (int32_t)x[4], core = (int32_t)x[5];
        if (priority < 0 || priority > 63 || core < -2 || core > 3 || core == -1 || (x[3] & 15) || !x[1] || !x[3]) {
            x[0] = 0xf001; x[1] = 0;
        } else {
            unsigned slot = 0;
            while (slot < THREAD_LIMIT && threads[slot]) ++slot;
            ScarletThread *t = slot < THREAD_LIMIT ? calloc(1, sizeof *t) : NULL;
            if (t) {
                t->refs = 1; t->entry = x[1]; t->ctx.x[0] = x[2]; t->ctx.x[31] = x[3];
                t->priority = priority; t->core = core == -2 ? 0 : core; t->mask = 1ULL << t->core;
                t->id = next_id++;
                t->tls = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                t->done = CreateEventW(NULL, TRUE, FALSE, NULL);
                if (!t->tls || !t->done) { release_thread(t); t = NULL; }
            }
            if (!t) { x[0] = 0xd201; x[1] = 0; }
            else {
                t->ctx.tpidr = (uintptr_t)t->tls;
                memcpy((char *)t->tls + 0x1f0, &t->ctx.tpidr, 8);
                threads[slot] = t; x[0] = 0; x[1] = THREAD_BASE + slot;
                trace_printf("Scarlet CreateThread %x entry=%llx arg=%llx stack=%llx priority=%d core=%d\n",
                    (unsigned)x[1], (unsigned long long)t->entry, (unsigned long long)t->ctx.x[0],
                    (unsigned long long)t->ctx.x[31], priority, t->core);
            }
        }
    } else {
        uint32_t h = (uint32_t)x[(n == 0x0c || n == 0x25) ? 1 : n == 0x0e ? 2 : 0];
        ScarletThread *t = lookup(h);
        if (!t) handled = 0; /* main thread pseudo-handle belongs to hostrt */
        else switch (n) {
        case 9:
            if (t->started) { x[0] = 0xfa01; break; }
            t->host = CreateThread(NULL, (SIZE_T)256 << 20, run_thread, t, CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
            if (!t->host) { x[0] = 0xd201; break; }
            t->started = 1; ++t->refs;
            ResumeThread(t->host); x[0] = 0; break;
        case 0x0c: x[0] = 0; x[1] = t->priority; break;
        case 0x0d:
            if (x[1] > 63) x[0] = 0xf001;
            else { t->priority = (int)x[1]; x[0] = 0; }
            break;
        case 0x0e: x[0] = 0; x[1] = (uint32_t)t->core; x[2] = t->mask; break;
        case 0x0f: {
            int core = (int32_t)x[1]; uint64_t mask = x[2];
            if (core == -2) { core = 0; mask = 1; }
            if (core == -3) core = t->core;
            if (core < -1 || core > 3 || !mask || (mask & ~15ULL) || (core >= 0 && !(mask & (1ULL << core)))) x[0] = 0xf001;
            else { t->core = core; t->mask = mask; x[0] = 0; }
            break;
        }
        case 0x25: x[0] = 0; x[1] = t->id; break;
        case 0x16:
            if (h == 0xffff8000) x[0] = 0xe401;
            else { threads[h - THREAD_BASE] = NULL; release_thread(t); x[0] = 0; }
            break;
        }
    }
    ReleaseSRWLockExclusive(&thread_lock);
    return handled;
}
