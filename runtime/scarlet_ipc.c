/* Scarlet bootstrap HIPC/CMIF services. Protocol layout cross-checked against
 * xruanzin/scarlet_pc runtime/ipc.c at 55437e0d9b7fead5b281c00c852272d7cbffed0e.
 * Unknown services/commands remain explicit boot blockers, never success stubs. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "hostrt.h"
HANDLE scarlet_thread_wait_handle(uint32_t h);

#define HANDLE_BASE 0x10000u
#define SESSION_COUNT 128
#define R_INVALID_HANDLE 0xe401u
#define R_INVALID_SIZE 0xca01u
static SRWLOCK sessions_lock = SRWLOCK_INIT;
static SRWLOCK ipc_lock = SRWLOCK_INIT; /* serialize session/domain lifetime */
static unsigned char sessions[SESSION_COUNT];
typedef struct { unsigned refs; unsigned char objects[128]; } Domain;
static Domain *domains[SESSION_COUNT];
static HANDLE events[SESSION_COUNT];
#define OBJECT_EVENT 255
static HANDLE applet_message_event;
static HANDLE suspended_tick_event;
static HANDLE gpu_error_event, friend_invitation_event;
static HANDLE aoc_changed_event;
static int focus_message_pending = 1;
static int request_exit_on_next_program;
static unsigned char self_settings[32];
enum { SERVICE_SM = 1, SERVICE_LM, SERVICE_LOGGER, SERVICE_APM, SERVICE_APM_SESSION,
       SERVICE_APPLET, SERVICE_PROXY, SERVICE_COMMON, SERVICE_SELF, SERVICE_WINDOW,
       SERVICE_AUDIO, SERVICE_DISPLAY, SERVICE_WINDING, SERVICE_LIBRARY, SERVICE_APPLICATION, SERVICE_DEBUG,
       SERVICE_AOC, SERVICE_ECTX_AW, SERVICE_ECTX_REGISTRAR, SERVICE_PCTL, SERVICE_PCTL_CONTROL,
       SERVICE_FSP, SERVICE_TIME, SERVICE_TIME_SYSTEM, SERVICE_TIME_STEADY, SERVICE_TIME_TZ,
       SERVICE_HID, SERVICE_HID_APPLET, SERVICE_HID_SYS };
static uint32_t performance_config[2] = {0x92220008, 0x92220008};
static const unsigned char scarlet_clock_uuid[16] =
    {0x4c,0x41,0x52,0x65,0x63,0x6f,0x6d,0x70,1,2,3,4,5,6,7,8};
static uint32_t time_shmem_handle, hid_shmem_handle;
static uint32_t hid_supported_styles = 0x1f;

static int64_t scarlet_steady_seconds(void) {
    extern uint64_t guest_cntpct(void);
    return (int64_t)(guest_cntpct() / 19200000ULL);
}
static int64_t scarlet_unix_now(void) { return (int64_t)time(NULL); }
static void scarlet_write_clock_context(unsigned char *p) {
    int64_t steady = scarlet_steady_seconds();
    int64_t off = scarlet_unix_now() - steady;
    memcpy(p, &off, 8);
    memcpy(p + 8, &steady, 8);
    memcpy(p + 0x10, scarlet_clock_uuid, 16);
}
static uint32_t scarlet_time_sharedmem(void) {
    if (time_shmem_handle) return time_shmem_handle;
    unsigned char block[0x1000] = {0};
    memcpy(block + 0x10, scarlet_clock_uuid, 16);
    scarlet_write_clock_context(block + 0x40);
    scarlet_write_clock_context(block + 0x88);
    extern uint32_t scarlet_sharedmem_create(uint64_t, const void *, size_t);
    time_shmem_handle = scarlet_sharedmem_create(sizeof block, block, sizeof block);
    return time_shmem_handle;
}
static uint32_t scarlet_hid_sharedmem(void) {
    if (hid_shmem_handle) return hid_shmem_handle;
    extern uint32_t scarlet_sharedmem_create(uint64_t, const void *, size_t);
    hid_shmem_handle = scarlet_sharedmem_create(0x40000, NULL, 0);
    return hid_shmem_handle;
}
static volatile uint32_t g_scarlet_terminate_result;
uint32_t scarlet_last_terminate_result(void) { return g_scarlet_terminate_result; }

typedef struct { uint32_t service, cmd, type, result; } ScarletIpcRecent;
static ScarletIpcRecent g_ipc_recent[32];
static volatile LONG g_ipc_recent_seq;
static void remember_ipc(uint32_t service, uint32_t cmd, uint32_t type, uint32_t result) {
    LONG n = InterlockedIncrement(&g_ipc_recent_seq);
    g_ipc_recent[(unsigned)n & 31] = (ScarletIpcRecent){service, cmd, type, result};
}
static void dump_recent_ipc(void) {
    LONG n = g_ipc_recent_seq;
    fprintf(stderr, "[Scarlet diagnostic] recent IPCs (oldest to newest):\n");
    LONG first = n > 31 ? n - 31 : 1;
    for (LONG i = first; i <= n; ++i) {
        ScarletIpcRecent e = g_ipc_recent[(unsigned)i & 31];
        fprintf(stderr, "  service=%u cmd=%u type=%u result=0x%08x\n",
                e.service, e.cmd, e.type, e.result);
    }
}

static uint32_t new_session(unsigned char service) {
    uint32_t h = 0;
    AcquireSRWLockExclusive(&sessions_lock);
    for (unsigned i = 0; i < SESSION_COUNT; ++i) if (!sessions[i]) {
        sessions[i] = service; h = HANDLE_BASE + i; break;
    }
    ReleaseSRWLockExclusive(&sessions_lock);
    return h;
}
static unsigned char get_session(uint32_t h) {
    if (h < HANDLE_BASE || h >= HANDLE_BASE + SESSION_COUNT) return 0;
    AcquireSRWLockShared(&sessions_lock);
    unsigned char s = sessions[h - HANDLE_BASE];
    ReleaseSRWLockShared(&sessions_lock);
    return s;
}
static uint32_t close_session(uint32_t h) {
    if (h < HANDLE_BASE || h >= HANDLE_BASE + SESSION_COUNT) return R_INVALID_HANDLE;
    AcquireSRWLockExclusive(&sessions_lock);
    uint32_t r = sessions[h - HANDLE_BASE] ? 0 : R_INVALID_HANDLE;
    sessions[h - HANDLE_BASE] = 0;
    if (events[h - HANDLE_BASE]) CloseHandle(events[h - HANDLE_BASE]);
    events[h - HANDLE_BASE] = NULL;
    Domain *d = domains[h - HANDLE_BASE];
    domains[h - HANDLE_BASE] = NULL;
    if (d && --d->refs == 0) free(d);
    ReleaseSRWLockExclusive(&sessions_lock);
    return r;
}

static uint32_t copy_event(HANDLE event) {
    HANDLE copy = NULL;
    if (!event || !DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) return 0;
    uint32_t h = new_session(OBJECT_EVENT);
    if (!h) { CloseHandle(copy); return 0; }
    events[h - HANDLE_BASE] = copy;
    return h;
}

static uint32_t request(Ctx *c, uint32_t h, uint32_t *msg, size_t size) {
    unsigned char service = get_session(h);
    if (!service || service == OBJECT_EVENT) return R_INVALID_HANDLE;
    if (!msg || size < 32) return R_INVALID_SIZE;
    uint32_t type = msg[0] & 0xffff, words = msg[1] & 0x3ff;
    size_t off = 2;
    /* IPC close releases the server endpoint; CloseHandle releases the handle. */
    if (type == 2 || type == 15) return 0;
    if (msg[1] & 0x80000000u) {
        if (size < 12) return R_INVALID_SIZE;
        uint32_t hd = msg[off++];
        off += (hd & 1 ? 2 : 0) + ((hd >> 1) & 15) + ((hd >> 5) & 15);
    }
    off += ((msg[0] >> 16) & 15) * 2;
    off += (((msg[0] >> 20) & 15) + ((msg[0] >> 24) & 15) + (msg[0] >> 28)) * 3;
    size_t raw_end = off + words, aligned = (off + 3) & ~(size_t)3;
    int tipc = type >= 16;
    if (raw_end > size / 4 || (!tipc && aligned + 4 > raw_end)) return R_INVALID_SIZE;
    Domain *domain = domains[h - HANDLE_BASE];
    int domain_request = domain && (type == 4 || type == 6);
    int domain_close = 0;
    uint32_t object_id = 0;
    uint32_t *in = msg + aligned;
    if (domain_request) {
        uint32_t operation = in[0] & 255, payload_size = in[0] >> 16;
        object_id = in[1];
        if (!object_id || object_id >= 128 || !domain->objects[object_id]) return R_INVALID_HANDLE;
        service = domain->objects[object_id];
        if (operation == 2) { domain->objects[object_id] = 0; domain_close = 1; }
        else if (operation != 1 || payload_size < 16 || aligned + 4 + (payload_size + 3) / 4 > raw_end)
            return R_INVALID_SIZE;
        aligned += 4;
        in = msg + aligned;
    }
    if (!tipc && !domain_close && in[0] != 0x49434653) return 0xf601; /* malformed CMIF */
    uint32_t cmd = domain_close ? 0 : tipc ? type - 16 : in[2], result = 0, moved = 0, copied = 0;
    uint64_t value = 0;
    unsigned char out_data[64] = {0};
    const void *out_ptr = &value;
    uint32_t *data = tipc ? msg + off : in + 4;
    size_t input_size = domain_close ? 0 : tipc ? words * 4 : (raw_end - aligned - 4) * 4;
    size_t data_size = 0;
    trace_printf("Scarlet IPC handle=%x service=%u object=%u type=%u cmd=%u raw=%u\n", h, service, object_id, type, cmd, words);
    if (getenv("SCARLET_IPC_BOOT_TRACE"))
        printf("Scarlet IPC ok? handle=%x service=%u object=%u type=%u cmd=%u raw=%u\n",
               h, service, object_id, type, cmd, words);
    if (domain_close) {
        /* Domain close has a header-only response. */
    } else if (type == 5 || type == 7) {
        switch (cmd) {
        case 0:
            if (domain) { result = 0xf601; break; }
            domain = (Domain *)calloc(1, sizeof *domain);
            if (!domain) { result = 0xd201; break; }
            domain->refs = 1; domain->objects[1] = service;
            domains[h - HANDLE_BASE] = domain;
            value = 1; data_size = 4; break;
        case 2: case 4:
            moved = new_session(service);
            if (!moved) result = 0xd201;
            else if (domain) { domains[moved - HANDLE_BASE] = domain; ++domain->refs; }
            break;
        case 3: value = 0x8000; data_size = 4; break;
        default: goto unsupported;
        }
    } else if (type == 4 || type == 6 || tipc) {
        if (service == SERVICE_SM && cmd == 0) {
            /* RegisterClient: the host process is the sole service-manager client. */
        } else if (service == SERVICE_SM && cmd == 1) {
            if (input_size < 8) return R_INVALID_SIZE;
            char name[9] = {0}; memcpy(name, data, 8);
            trace_printf("Scarlet SM GetService '%s'\n", name);
            if (!strcmp(name, "lm")) moved = new_session(SERVICE_LM);
            else if (!strcmp(name, "apm")) moved = new_session(SERVICE_APM);
            else if (!strcmp(name, "appletOE")) moved = new_session(SERVICE_APPLET);
            else if (!strcmp(name, "aoc:u")) moved = new_session(SERVICE_AOC);
            else if (!strcmp(name, "ectx:aw")) moved = new_session(SERVICE_ECTX_AW);
            else if (!strcmp(name, "fsp-srv")) moved = new_session(SERVICE_FSP);
            else if (!strcmp(name, "time:u") || !strcmp(name, "time:a") || !strcmp(name, "time:s"))
                moved = new_session(SERVICE_TIME);
            else if (!strcmp(name, "hid")) moved = new_session(SERVICE_HID);
            else if (!strcmp(name, "hid:sys")) moved = new_session(SERVICE_HID_SYS);
            else if (!strcmp(name, "pctl") || !strcmp(name, "pctl:a") ||
                     !strcmp(name, "pctl:r") || !strcmp(name, "pctl:s"))
                moved = new_session(SERVICE_PCTL);
            else {
                printf("Scarlet SM GetService '%s' is not implemented\n", name);
                goto unsupported;
            }
            if (!moved) result = 0xd201;
        } else if (service == SERVICE_APPLET && cmd == 0) {
            moved = new_session(SERVICE_PROXY);
            if (!moved) result = 0xd201;
        } else if (service == SERVICE_PROXY) {
            unsigned char child = 0;
            switch (cmd) {
            case 0: child = SERVICE_COMMON; break;
            case 1: child = SERVICE_SELF; break;
            case 2: child = SERVICE_WINDOW; break;
            case 3: child = SERVICE_AUDIO; break;
            case 4: child = SERVICE_DISPLAY; break;
            case 10: child = SERVICE_WINDING; break;
            case 11: child = SERVICE_LIBRARY; break;
            case 20: child = SERVICE_APPLICATION; break;
            case 1000: child = SERVICE_DEBUG; break;
            default: goto unsupported;
            }
            moved = new_session(child);
            if (!moved) result = 0xd201;
        } else if (service == SERVICE_APPLICATION && (cmd == 130 || cmd == 140)) {
            HANDLE *event = cmd == 130 ? &gpu_error_event : &friend_invitation_event;
            if (!*event) *event = CreateEventW(NULL, TRUE, FALSE, NULL);
            copied = copy_event(*event);
            if (!copied) result = 0xd201;
        } else if (service == SERVICE_APPLICATION && cmd == 20) {
            /* EnsureSaveData -> required size. Saves live in SSBU_USERDATA, so
             * no additional system-save allocation is required here. */
            value = 0; data_size = 8;
        } else if (service == SERVICE_APPLICATION && cmd == 21) {
            /* GetDesiredLanguage: 8-byte packed nn::settings language code. */
            value = 0;
            memcpy(&value, "pt-BR", 5);
            data_size = 8;
        } else if (service == SERVICE_APPLICATION && cmd == 22) {
            /* SetTerminateResult(Result) is immediately followed by svcBreak on
             * fatal bootstrap paths. Preserve the value so Break diagnostics
             * can recover it even if the SDK's temporary Result object is
             * clobbered before svcBreak. */
            uint32_t terminate_result = input_size >= 4 ? data[0] : 0;
            g_scarlet_terminate_result = terminate_result;
            trace_printf("Scarlet SetTerminateResult(%08x)\n", terminate_result);
            printf("Scarlet SetTerminateResult = 0x%08x\n", terminate_result);
            if (terminate_result) {
                extern void scarlet_dump_guest_stack_now(Ctx *, const char *);
                dump_recent_ipc();
                scarlet_dump_guest_stack_now(c, "SetTerminateResult(nonzero)");
            }
        } else if (service == SERVICE_APPLICATION && cmd == 23) {
            /* GetDisplayVersion returns exactly 0x10 bytes. */
            memcpy(out_data, "1.0.0", 5);
            out_ptr = out_data;
            data_size = 0x10;
        } else if (service == SERVICE_APPLICATION && cmd == 40) {
            /* NotifyRunning -> bool/u8 restart requested. */
            out_data[0] = 0;
            out_ptr = out_data;
            data_size = 1;
        } else if (service == SERVICE_COMMON && cmd == 900) {
            request_exit_on_next_program = 1;
        } else if (service == SERVICE_COMMON && cmd == 0) {
            if (!applet_message_event) applet_message_event = CreateEventW(NULL, TRUE, focus_message_pending, NULL);
            copied = copy_event(applet_message_event);
            if (!copied) result = 0xd201;
        } else if (service == SERVICE_COMMON && cmd == 1) {
            if (focus_message_pending) {
                value = 15; data_size = 4; focus_message_pending = 0;
                if (applet_message_event) ResetEvent(applet_message_event);
            } else result = 0x680; /* AM: no messages */
        } else if (service == SERVICE_SELF && cmd == 90) {
            value = 0; data_size = 8; /* application has never been suspended */
        } else if (service == SERVICE_SELF && cmd == 91) {
            if (!suspended_tick_event) suspended_tick_event = CreateEventW(NULL, TRUE, FALSE, NULL);
            copied = copy_event(suspended_tick_event);
            if (!copied) result = 0xd201;
        } else if (service == SERVICE_SELF && (cmd == 11 || cmd == 12 || cmd == 13 || cmd == 14 || cmd == 16)) {
            if (input_size < (cmd == 13 ? 3 : 1)) return R_INVALID_SIZE;
            self_settings[cmd] = ((unsigned char *)data)[0];
            if (cmd == 13) { self_settings[30] = ((unsigned char *)data)[1]; self_settings[31] = ((unsigned char *)data)[2]; }
        } else if (service == SERVICE_WINDOW && cmd == 1) {
            value = 0x1234; data_size = 8; /* stable AppletResourceUserId */
        } else if (service == SERVICE_WINDOW && cmd == 10) {
            /* Single foreground application owns this host window. */
        } else if (service == SERVICE_COMMON && (cmd == 5 || cmd == 6 || cmd == 9)) {
            value = 1; data_size = 4; /* docked, boost, foreground */
        } else if (service == SERVICE_COMMON && cmd == 8) {
            value = 0; data_size = 4; /* normal boot */
        } else if (service == SERVICE_AOC && (cmd == 0 || cmd == 2)) {
            /* CountAddOnContent[ByApplicationId]. The baseline dump has no DLC
             * mounted, so the installed add-on-content count is zero. */
            value = 0; data_size = 4;
        } else if (service == SERVICE_AOC && (cmd == 1 || cmd == 3)) {
            /* ListAddOnContent[ByApplicationId]. With a zero installed count,
             * no output-buffer entries are written; return out_count=0. */
            value = 0; data_size = 4;
        } else if (service == SERVICE_AOC && (cmd == 4 || cmd == 5)) {
            /* GetAddOnContentBaseId[ByApplicationId]. Scarlet's application
             * id is 0100A3D008C5C000 and its AOC base is application+0x1000. */
            value = 0x0100A3D008C5D000ULL; data_size = 8;
        } else if (service == SERVICE_AOC && (cmd == 8 || cmd == 10)) {
            /* DLC list-change event. There is no host DLC installer yet, so
             * keep a stable manual-reset event permanently unsignaled. */
            if (!aoc_changed_event) aoc_changed_event = CreateEventW(NULL, TRUE, FALSE, NULL);
            copied = copy_event(aoc_changed_event);
            if (!copied) result = 0xd201;
        } else if (service == SERVICE_AOC && cmd == 9) {
            value = 0; data_size = 4; /* no lost add-on-content error */
        } else if (service == SERVICE_ECTX_AW && cmd == 0) {
            /* IWriterForApplication::CreateContextRegistrar. Error-context
             * collection is optional for gameplay; return a live registrar
             * object so the SDK can complete/commit its bootstrap context. */
            moved = new_session(SERVICE_ECTX_REGISTRAR);
            if (!moved) result = 0xd201;
        } else if (service == SERVICE_ECTX_AW && cmd == 1) {
            /* CommitContext: there is no host error-report database yet. The
             * contract is satisfied once the context was accepted. */
        } else if (service == SERVICE_ECTX_REGISTRAR && cmd == 0) {
            /* IContextRegistrar::Complete. The guest has finished populating
             * its context descriptors; nothing needs to be persisted here. */
        } else if (service == SERVICE_PCTL && (cmd == 0 || cmd == 1)) {
            /* pctl root -> IParentalControlService. */
            moved = new_session(SERVICE_PCTL_CONTROL);
            if (!moved) result = 0xd201;
        } else if (service == SERVICE_PCTL_CONTROL &&
                   (cmd == 1018 || cmd == 1031 || cmd == 1035)) {
            /* Stereo/free-communication/etc restrictions: disabled. */
            value = 0; data_size = 1;
        } else if (service == SERVICE_PCTL_CONTROL && cmd == 1032) {
            value = 0; data_size = 4;
        } else if (service == SERVICE_PCTL_CONTROL) {
            /* Match the proven Scarlet runtime exactly: unknown parental
             * control queries succeed with a 16-byte zeroed payload. Some SDK
             * callers inspect that structure even when Result is success. */
            memset(out_data, 0, 0x10);
            out_ptr = out_data;
            data_size = 0x10;
            trace_printf("Scarlet pctl cmd %u -> unrestricted/16-byte zero payload\n", cmd);
        } else if (service == SERVICE_FSP) {
            /* Proven Scarlet bootstrap contract from scarlet_pc. The early
             * startup path only needs SetCurrentProcess plus CMIF control
             * messages before rtld_init; keep the other harmless bootstrap
             * controls ready so we do not stop one command later. */
            if (cmd == 1 || cmd == 22 || cmd == 1003 || cmd == 1004 || cmd == 1006) {
                /* success, no payload */
            } else if (cmd == 1005) {
                value = 0; data_size = 4; /* GetGlobalAccessLogMode */
            } else if (cmd == 1011) {
                memset(out_data, 0, 0x10); out_ptr = out_data; data_size = 0x10;
            } else {
                goto unsupported;
            }
        } else if (service == SERVICE_TIME) {
            switch (cmd) {
            case 0: case 1: case 4: case 5:
                moved = new_session(SERVICE_TIME_SYSTEM); break;
            case 2:
                moved = new_session(SERVICE_TIME_STEADY); break;
            case 3:
                moved = new_session(SERVICE_TIME_TZ); break;
            case 20:
                copied = scarlet_time_sharedmem();
                if (!copied) result = 0xd201;
                break;
            case 100: case 101:
                out_data[0] = 0; out_ptr = out_data; data_size = 1; break;
            default:
                memset(out_data, 0, 0x10); out_ptr = out_data; data_size = 0x10; break;
            }
        } else if (service == SERVICE_TIME_SYSTEM) {
            if (cmd == 0) {
                int64_t now = scarlet_unix_now();
                memcpy(out_data, &now, 8); out_ptr = out_data; data_size = 8;
            } else if (cmd == 2) {
                scarlet_write_clock_context(out_data); out_ptr = out_data; data_size = 0x20;
            } else goto unsupported;
        } else if (service == SERVICE_TIME_STEADY) {
            if (cmd == 0) {
                int64_t now = scarlet_steady_seconds();
                memcpy(out_data, &now, 8);
                memcpy(out_data + 8, scarlet_clock_uuid, 16);
                out_ptr = out_data; data_size = 0x18;
            } else {
                value = 0; data_size = 8;
            }
        } else if (service == SERVICE_TIME_TZ) {
            if (cmd == 0) {
                memcpy(out_data, "UTC", 4); out_ptr = out_data; data_size = 0x24;
            } else {
                memset(out_data, 0, 0x10); out_ptr = out_data; data_size = 0x10;
            }
        } else if (service == SERVICE_HID && cmd == 0) {
            moved = new_session(SERVICE_HID_APPLET);
            if (!moved) result = 0xd201;
        } else if (service == SERVICE_HID_APPLET && cmd == 0) {
            copied = scarlet_hid_sharedmem();
            if (!copied) result = 0xd201;
        } else if (service == SERVICE_HID && (cmd == 40 || cmd == 106)) {
            HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
            copied = copy_event(ev);
            if (ev) CloseHandle(ev);
            if (!copied) result = 0xd201;
        } else if (service == SERVICE_HID && cmd == 100) {
            if (input_size >= 4) hid_supported_styles = data[0];
        } else if (service == SERVICE_HID && cmd == 101) {
            value = hid_supported_styles; data_size = 4;
        } else if (service == SERVICE_HID && (cmd == 121 || cmd == 129)) {
            value = 0; data_size = 8;
        } else if (service == SERVICE_HID && cmd == 200) {
            memset(out_data, 0, 0x10);
            *(uint32_t *)out_data = 1;
            out_ptr = out_data; data_size = 0x10;
        } else if (service == SERVICE_HID) {
            /* Activate/Set/vibration commands are no-ops on a host with
             * input bridged outside Horizon HID, matching scarlet_pc. */
        } else if (service == SERVICE_HID_SYS) {
            memset(out_data, 0, 0x10); out_ptr = out_data; data_size = 0x10;
        } else if (service == SERVICE_APM && cmd == 0) {
            moved = new_session(SERVICE_APM_SESSION);
            if (!moved) result = 0xd201;
        } else if (service == SERVICE_APM && cmd == 1) {
            value = 1; data_size = 4; /* docked performance mode */
        } else if (service == SERVICE_APM_SESSION && cmd <= 1) {
            if (input_size < (cmd == 0 ? 8 : 4)) return R_INVALID_SIZE;
            if (data[0] > 1) result = 0xf001;
            else if (cmd == 0) performance_config[data[0]] = data[1];
            else { value = performance_config[data[0]]; data_size = 4; }
        } else if (service == SERVICE_LM && cmd == 0) {
            moved = new_session(SERVICE_LOGGER);
            if (!moved) result = 0xd201;
        } else if (service == SERVICE_LOGGER && cmd == 0) {
            /* ILogger::Log forwards its binary packet to the host trace. */
            size_t desc = 2;
            if (msg[1] & 0x80000000u) {
                uint32_t hd = msg[desc++];
                desc += (hd & 1 ? 2 : 0) + ((hd >> 1) & 15) + ((hd >> 5) & 15);
            }
            if (!((msg[0] >> 16) & 15) || desc + 2 > size / 4) return R_INVALID_SIZE;
            uint32_t a = msg[desc], b = msg[desc + 1];
            uint64_t address = b | ((uint64_t)((a >> 12) & 15) << 32) | ((uint64_t)((a >> 6) & 7) << 36);
            size_t length = a >> 16;
            const unsigned char *packet = (const unsigned char *)(uintptr_t)address;
            trace_printf("Scarlet ILogger packet (%u bytes):", (unsigned)length);
            for (size_t i = 0; i < length; ++i) trace_printf(" %02x", packet[i]);
            trace_printf("\n");
        } else if (service == SERVICE_LOGGER && cmd == 1) {
            /* SetDestination: host diagnostics are the available log destination. */
            if (input_size < 4) return R_INVALID_SIZE;
            trace_printf("Scarlet ILogger destination=%x (host trace)\n", data[0]);
        } else goto unsupported;
    } else goto unsupported;
    remember_ipc(service, cmd, type, result);
    const size_t data_words = (data_size + 3) / 4;
    const int has_handles = moved || copied;

    /* TIPC has no CMIF magic or 16-byte alignment. Its raw response is
     * Result followed immediately by service output. Round byte-sized outputs
     * up to whole HIPC words exactly as libnx/scarlet_pc does. */
    if (tipc) {
        size_t handle_words = has_handles ? 1 + (copied ? 1 : 0) + (moved ? 1 : 0) : 0;
        size_t total_words = 2 + handle_words + 1 + data_words;
        if (size < total_words * 4) return R_INVALID_SIZE;
        memset(msg, 0, total_words * 4);
        uint32_t *q = msg + 2;
        msg[0] = type;
        msg[1] = 1 + (uint32_t)data_words;
        if (has_handles) {
            msg[1] |= 0x80000000u;
            *q++ = (copied ? (1u << 1) : 0) | (moved ? (1u << 5) : 0);
            if (copied) *q++ = copied;
            if (moved) *q++ = moved;
        }
        q[0] = result;
        if (data_size) memcpy(q + 1, out_ptr, data_size);
        return 0;
    }

    uint32_t domain_out = 0;
    if (domain_request && moved) {
        unsigned id = 1;
        while (id < 128 && domain->objects[id]) ++id;
        if (id == 128) result = 0xd201;
        else { domain->objects[id] = get_session(moved); domain_out = id; }
        close_session(moved); moved = 0;
    }

    /* CMIF response layout proven by scarlet_pc:
     *   HIPC header
     *   optional special-header + copy/move handles
     *   16-byte alignment
     *   optional 16-byte domain response header
     *   SFCO, padding, Result, padding, service data, domain object ids
     *
     * Do not assume SFCO lives at msg[4]: with enough returned handles the
     * aligned raw section moves forward. */
    const int cmif_handles = moved || copied;
    size_t handle_words = cmif_handles ? 1 + (copied ? 1 : 0) + (moved ? 1 : 0) : 0;
    uint32_t *q0 = msg + 2 + handle_words;
    uint32_t *aligned_out = (uint32_t *)(((uintptr_t)q0 + 15) & ~(uintptr_t)15);
    size_t domain_words = domain_request ? 4 + (domain_out ? 1 : 0) : 0;
    uint32_t *end = aligned_out + domain_words + 4 + data_words;
    if ((uintptr_t)end > (uintptr_t)msg + size) return R_INVALID_SIZE;

    size_t payload_words = 4 + data_words + domain_words;
    size_t raw_words = payload_words + 4; /* conservative alignment padding, as scarlet_pc */
    memset(msg, 0, (size_t)(end - msg) * 4);
    msg[0] = 0;
    msg[1] = (uint32_t)raw_words | (cmif_handles ? 0x80000000u : 0);

    uint32_t *q = msg + 2;
    if (cmif_handles) {
        *q++ = (copied ? (1u << 1) : 0) | (moved ? (1u << 5) : 0);
        if (copied) *q++ = copied;
        if (moved) *q++ = moved;
    }
    uint32_t *out = (uint32_t *)(((uintptr_t)q + 15) & ~(uintptr_t)15);
    if (domain_request) {
        out[0] = domain_out ? 1 : 0;
        out += 4;
    }
    out[0] = 0x4f434653; /* SFCO */
    out[1] = 0;
    out[2] = result;
    out[3] = 0;
    if (data_size) memcpy(out + 4, out_ptr, data_size);
    if (domain_out) out[4 + data_words] = domain_out;
    return 0;
unsupported:
    printf("Scarlet IPC unsupported: handle=%x service=%u type=%u cmd=%u guest=%llx\n",
           h, service, type, cmd, (unsigned long long)c->fn);
    guest_trap("Scarlet IPC command", cmd);
    return 0xf601;
}

static int ipc_svc(Ctx *c, uint32_t n) {
    uint64_t *x = c->x;
    switch (n) {
    case 0x11: case 0x12: case 0x17: {
        uint32_t h = (uint32_t)x[0];
        if (get_session(h) != OBJECT_EVENT) { x[0] = R_INVALID_HANDLE; return 1; }
        HANDLE event = events[h - HANDLE_BASE];
        if (n == 0x17 && WaitForSingleObject(event, 0) != WAIT_OBJECT_0) x[0] = 0xfa01;
        else x[0] = (n == 0x11 ? SetEvent(event) : ResetEvent(event)) ? 0 : 0xf001;
        return 1;
    }
    case 0x16:
        /* Only claim handles owned here; future kernel object classes dispatch elsewhere. */
        if (!get_session((uint32_t)x[0])) return 0;
        x[0] = close_session((uint32_t)x[0]); return 1;
    case 0x1f: {
        const char *name = (const char *)(uintptr_t)x[1];
        uint32_t h = name && !strncmp(name, "sm:", 4) ? new_session(SERVICE_SM) : 0;
        trace_printf("Scarlet ConnectToNamedPort '%.*s' -> %x\n", 12, name ? name : "", h);
        x[0] = h ? 0 : 0xf201; x[1] = h; return 1;
    }
    case 0x21: {
        extern uint64_t guest_tpidrro(Ctx *);
        x[0] = request(c, (uint32_t)x[0], (uint32_t *)(uintptr_t)guest_tpidrro(c), 0x100);
        return 1;
    }
    case 0x22: x[0] = request(c, (uint32_t)x[2], (uint32_t *)(uintptr_t)x[0], (size_t)x[1]); return 1;
    default: return 0;
    }
}

int scarlet_ipc_svc(Ctx *c, uint32_t n) {
    if (n == 0x18) {
        int32_t count = (int32_t)c->x[2];
        int64_t ns = (int64_t)c->x[3];
        uint32_t *handles = (uint32_t *)(uintptr_t)c->x[1];
        HANDLE copies[64] = {0};
        uint32_t result = 0;
        if (count < 0 || count > 64 || (count && !handles)) result = R_INVALID_SIZE;
        AcquireSRWLockExclusive(&ipc_lock);
        for (int i = 0; !result && i < count; ++i) {
            uint32_t h = handles[i];
            if ((copies[i] = scarlet_thread_wait_handle(h)) != NULL) continue;
            if (get_session(h) != OBJECT_EVENT || !DuplicateHandle(GetCurrentProcess(), events[h - HANDLE_BASE],
                    GetCurrentProcess(), &copies[i], 0, FALSE, DUPLICATE_SAME_ACCESS)) result = R_INVALID_HANDLE;
        }
        ReleaseSRWLockExclusive(&ipc_lock);
        int index = -1;
        if (!result) {
            uint64_t rounded = ns > 0 ? ((uint64_t)ns + 999999) / 1000000 : 0;
            DWORD ms = ns < 0 ? INFINITE : rounded >= INFINITE ? INFINITE - 1 : (DWORD)rounded;
            DWORD wait;
            if (count) wait = WaitForMultipleObjects((DWORD)count, copies, FALSE, ms);
            else { Sleep(ms); wait = WAIT_TIMEOUT; }
            if (wait < (DWORD)count) index = (int)wait;
            else result = wait == WAIT_TIMEOUT ? 0xea01 : R_INVALID_HANDLE;
        }
        for (int i = 0; i < 64; ++i) if (copies[i]) CloseHandle(copies[i]);
        c->x[0] = result; c->x[1] = (uint32_t)index; return 1;
    }
    if (n != 0x11 && n != 0x12 && n != 0x17 && n != 0x16 && n != 0x1f && n != 0x21 && n != 0x22) return 0;
    AcquireSRWLockExclusive(&ipc_lock);
    int handled = ipc_svc(c, n);
    ReleaseSRWLockExclusive(&ipc_lock);
    return handled;
}
