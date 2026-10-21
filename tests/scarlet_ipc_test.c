/* Native protocol regression test: no ROM, lifted objects or game DLL needed.
 * deps/llvm/bin/clang.exe -DSCARLET_RECOMP -Iruntime -Ibuild/work
 *     tests/scarlet_ipc_test.c -o build/probe/scarlet_ipc_test.exe
 */
#include <assert.h>
#include <stdlib.h>
#include "../runtime/scarlet_ipc.c"
void trace_printf(const char *fmt, ...) { (void)fmt; }
void guest_trap(const char *what, uint64_t addr) {
    fprintf(stderr, "unexpected trap %s %llu\n", what, (unsigned long long)addr);
    abort();
}
static Ctx ctx;
static uint32_t msg[64];
static void clear(void) { memset(msg, 0, sizeof msg); }
static void send_request(uint32_t h) {
    ctx.x[0] = h; ctx.tpidr = (uintptr_t)msg;
    assert(scarlet_ipc_svc(&ctx, 0x21)); assert(ctx.x[0] == 0);
}
static uint32_t service(uint32_t sm, const char *name) {
    clear(); msg[0] = 17; msg[1] = 2; memcpy(msg + 2, name, strlen(name));
    send_request(sm);
    assert(msg[1] == 0x80000001 && msg[2] == 0x20 && msg[4] == 0);
    return msg[3];
}
static void cmif(uint32_t type, uint32_t cmd) {
    clear(); msg[0] = type; msg[1] = 8; msg[4] = 0x49434653; msg[6] = cmd;
}
static void domain_cmd(uint32_t object, uint32_t cmd) {
    clear(); msg[0] = 6; msg[1] = 12;
    msg[4] = 0x00100001; msg[5] = object; msg[8] = 0x49434653; msg[10] = cmd;
}
int main(void) {
    ctx.x[1] = (uintptr_t)"sm:";
    assert(scarlet_ipc_svc(&ctx, 0x1f) && ctx.x[0] == 0);
    uint32_t sm = (uint32_t)ctx.x[1];
    clear(); msg[0] = 16; msg[1] = 0x80000000; msg[2] = 1; /* PID only, no CMIF */
    send_request(sm); assert(msg[1] == 1 && msg[2] == 0);
    uint32_t lm = service(sm, "lm");
    cmif(7, 3); send_request(lm); assert(msg[4] == 0x4f434653 && msg[6] == 0 && msg[8] == 0x8000);
    cmif(7, 0); send_request(lm); assert(msg[8] == 1);
    domain_cmd(1, 0); send_request(lm);
    assert(!(msg[1] & 0x80000000) && msg[4] == 1 && msg[8] == 0x4f434653 && msg[10] == 0);
    uint32_t logger = msg[12]; assert(logger == 2);
    cmif(7, 2); send_request(lm); uint32_t clone = msg[3]; assert(clone != lm);
    ctx.x[0] = lm; assert(scarlet_ipc_svc(&ctx, 0x16) && ctx.x[0] == 0);
    domain_cmd(logger, 1); msg[1] = 13; msg[4] = 0x00140001; msg[12] = 1;
    send_request(clone); assert(msg[10] == 0); /* domain survives closing original */
    clear(); msg[0] = 6; msg[1] = 8; msg[4] = 2; msg[5] = logger; send_request(clone);
    domain_cmd(logger, 1); ctx.x[0] = clone; scarlet_ipc_svc(&ctx, 0x21); assert(ctx.x[0] == R_INVALID_HANDLE);
    /* Truncated descriptor/raw section must not be interpreted as a service request. */
    clear(); msg[0] = 6; msg[1] = 0x3ff;
    ctx.x[0] = clone; scarlet_ipc_svc(&ctx, 0x21); assert(ctx.x[0] == R_INVALID_SIZE);
    /* User-buffer variant is bounded by x1, including the debug/header read. */
    ctx.x[0] = (uintptr_t)msg; ctx.x[1] = 4; ctx.x[2] = clone;
    scarlet_ipc_svc(&ctx, 0x22); assert(ctx.x[0] == R_INVALID_SIZE);
    uint32_t aoc = service(sm, "aoc:u");
    cmif(4, 2); send_request(aoc); assert(msg[8] == 0); /* CountAddOnContent */
    cmif(4, 5); send_request(aoc);
    assert(((uint64_t)msg[8] | ((uint64_t)msg[9] << 32)) == 0x0100A3D008C5D000ULL);
    cmif(4, 8); send_request(aoc); assert(msg[1] & 0x80000000u); /* change event */
    uint32_t aoc_event = msg[3];
    ctx.x[1] = (uintptr_t)&aoc_event; ctx.x[2] = 1; ctx.x[3] = 0;
    scarlet_ipc_svc(&ctx, 0x18); assert(ctx.x[0] == 0xea01);
    ctx.x[0] = aoc_event; scarlet_ipc_svc(&ctx, 0x16); assert(ctx.x[0] == 0);

    uint32_t ectx = service(sm, "ectx:aw");
    cmif(4, 0); send_request(ectx); uint32_t registrar = msg[3]; assert(registrar);
    cmif(4, 0); send_request(registrar); assert(msg[6] == 0); /* Complete */
    cmif(4, 1); send_request(ectx); assert(msg[6] == 0);      /* CommitContext */
    ctx.x[0] = registrar; assert(scarlet_ipc_svc(&ctx, 0x16) && ctx.x[0] == 0);
    ctx.x[0] = ectx; assert(scarlet_ipc_svc(&ctx, 0x16) && ctx.x[0] == 0);

    uint32_t pctl = service(sm, "pctl");
    cmif(4, 0); send_request(pctl); uint32_t pctl_control = msg[3]; assert(pctl_control);
    cmif(4, 1031); send_request(pctl_control); assert((msg[8] & 0xffu) == 0);
    cmif(4, 1032); send_request(pctl_control); assert(msg[8] == 0);
    ctx.x[0] = pctl_control; assert(scarlet_ipc_svc(&ctx, 0x16) && ctx.x[0] == 0);
    ctx.x[0] = pctl; assert(scarlet_ipc_svc(&ctx, 0x16) && ctx.x[0] == 0);

    uint32_t apm = service(sm, "apm");
    cmif(4, 0); send_request(apm); uint32_t apm_session = msg[3];
    cmif(4, 0); msg[1] = 10; msg[8] = 1; msg[9] = 0x92220008; send_request(apm_session);
    cmif(4, 1); msg[1] = 9; msg[8] = 1; send_request(apm_session); assert(msg[8] == 0x92220008);
    /* Close message and CloseHandle are distinct operations. */
    clear(); msg[0] = 15; send_request(sm);
    ctx.x[0] = sm; assert(scarlet_ipc_svc(&ctx, 0x16) && ctx.x[0] == 0);
    clear(); msg[0] = 16; ctx.x[0] = sm; scarlet_ipc_svc(&ctx, 0x21); assert(ctx.x[0] == R_INVALID_HANDLE);
    /* A copied event must keep its object alive independently, and waits must
     * observe real signaled/timeout states rather than unconditional success. */
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    uint32_t e = copy_event(ev); assert(e); CloseHandle(ev);
    ctx.x[1] = (uintptr_t)&e; ctx.x[2] = 1; ctx.x[3] = 0;
    scarlet_ipc_svc(&ctx, 0x18); assert(ctx.x[0] == 0xea01 && (uint32_t)ctx.x[1] == 0xffffffff);
    ctx.x[0] = e; scarlet_ipc_svc(&ctx, 0x11); assert(ctx.x[0] == 0);
    ctx.x[1] = (uintptr_t)&e; ctx.x[2] = 1; ctx.x[3] = 1000000;
    scarlet_ipc_svc(&ctx, 0x18); assert(ctx.x[0] == 0 && ctx.x[1] == 0);
    ctx.x[0] = e; scarlet_ipc_svc(&ctx, 0x17); assert(ctx.x[0] == 0);
    ctx.x[0] = e; scarlet_ipc_svc(&ctx, 0x17); assert(ctx.x[0] == 0xfa01);
    ctx.x[0] = e; scarlet_ipc_svc(&ctx, 0x16); assert(ctx.x[0] == 0);
    ctx.x[1] = (uintptr_t)&e; ctx.x[2] = 1; ctx.x[3] = 0;
    scarlet_ipc_svc(&ctx, 0x18); assert(ctx.x[0] == R_INVALID_HANDLE);
    puts("Scarlet IPC: TIPC, CMIF, domains, bounds, AOC, ECTX, PCTL, APM, close and event SVC tests passed");
    return 0;
}
