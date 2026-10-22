# Scarlet boot investigation — 2026-10-22

Target: Scarlet 1.0.0, main Build ID ca44e7dd62e61a2a4e8889b3319ae173375770fd.
Branch: scarlet-bootstrap. No extracted game content is committed.

## Confirmed fixes

- Windows host link uses Synchronization.lib for WaitOnAddress/WakeByAddress*.
- Static SDK data wins over unrelated partial-SDK host data; this keeps libc++ locale facet IDs consistent.
- Host TLS wrappers read ThreadType through the Horizon TLR, rather than confusing the TLR with libc TLS.
- Shared memory accepts 4 KiB pages in 64 KiB Windows reservations, preserves neighboring pages, and can commit guest-free pages inside existing reservations.
- Main stack and thread TLS/TLR stay inside Horizon's 39-bit IPC descriptor address width. QueryMemory describes full free spans and protects the bootstrap stack guards.
- Horizon condition waits register before releasing their mutex and retain early signals. Mutex reacquisition sets the waiter bit before sleeping so the SDK calls the kernel unlock path.
- QueryMemory reports shared mappings with Horizon state 6 and no longer reports the entire static-image window, including holes, as code. Adjacent uncommitted Windows regions are coalesced forward for Horizon guard checks.
- Timed diagnostics no longer dereference a fixed SSBU std::map address, and guest-thread counts include Scarlet kernel threads.
- Shader container scanning uses portable container fields for Scarlet and never dereferences the SSBU engine tables at 0x71052a32d8/0x7104470aa8.
- VI/NV graphics entry points route to the existing host renderer. Scarlet texture storage uses its Tegra CPU layout instead of the SSBU SDK oracle's incompatible fixed addresses. Sparse textures still stop explicitly.
- Bounded indirect jump-table destinations encountered during graphics initialization and asset decoding are retained in the Scarlet profile. Incremental compile refreshes only modules with missing targets or changed host wrappers and hashes each source to retain unchanged objects.

## Evidence and validation

Native fixtures: scarlet_loader_test, scarlet_tls_test, scarlet_memory_test, scarlet_sync_test (500 racing waits/signals plus bounded wakes, broadcast, timeout and mutex contention), scarlet_texture_test (tiled, linear, buffer, array and BC1). Python profile tests: 3 passed.

The graphics boundary update compiled 6/1221 game sources. Subsequent late-target updates compiled 2/1221, retaining SDK/subsdk objects. Module lifts report zero untranslated instructions; this is a translation coverage count, not evidence of complete gameplay correctness.

Real runtime reached NVN device, memory pools, sampler pools, two textures, window and queue initialization, and background asset work. A subsequent run reached 5,975 program/shader registrations and created 1,283 Vulkan shader modules before exposing another SDK formatting target (now added with all cases of its bounded table). No frame has yet been presented. Local diagnostic logs are in build/logs/boot-*.log; boot_trace.txt is rewritten on every run in build/work.

REA inspected the generated native PE earlier in this investigation. Ghidra was not configured and native objdump was unavailable; no REA decompilation result is claimed. Follow-up fixes use available source, lifted instruction sequences, ELF jump-table bytes and actual runtime traces. Legacy reference: xruanzin/scarlet_pc at 55437e0d9b7fead5b281c00c852272d7cbffed0e.

## Final 60-second smoke test

`build/logs/boot-final.log`: process exited 0 after the configured 60-second limit (65 seconds including shutdown), 15 guest kernel threads started, 11,872 program/shader registrations, 56 containers / 3,822 variations scanned, 3,480 Vulkan shader modules created. No guest trap, host exception or Vulkan call failure in this run. Zero presented frames: the title screen is not validated. The main thread was waiting on a Horizon condition variable while workers prepared assets/shaders.

A following `setup.py --dump <Base> --from compile` completed in 9.5 seconds: 0 of 1,221 sources compiled; structural checks passed. This confirms that repeating the supplied incremental command preserves the existing game objects.

## Longer warm-cache run: remaining blockers

`build/logs/boot-warm.log` (requested 180-second limit) failed before the limit with native access violation 0xc0000005 in ntdll, reading 0xffffffffffffffff; no guest stack was reported. The root cause of this native failure has not been isolated. Before it, the renderer reported untranslated Maxwell SHFL and SURED, SUST with a register image handle, and GLSL rejection because texel offsets were not compile-time constants. No presented frame. Thus the passing 60-second smoke test does not establish a stable full boot or playable game. Further work must investigate the renderer/shader translator and the native failure, in addition to any subsequently reached Horizon services.

## User log follow-up: game_20261022_005508.log

The supplied run stopped on unimplemented SVC 0x34 (WaitForAddress, type 2) and SVC 0x35 (SignalToAddress, type 2), both on the same address. Ported all three wait/signal variants from the legacy Scarlet kernel to Windows: predicate checking, decrement-if-less, equality/increment/conditional modification, atomic waiter registration, count-limited wakes and broadcast for nonpositive counts. A separate address queue avoids condition-variable key-flag writes corrupting the arbitration word.

`scarlet_address_test` passes 300 signal/registration races, signed comparison, decrement, timeout cleanup, conditional word updates and bounded/broadcast wakes; the existing 500-round condition-variable stress test also passes. Runtime-only link preserves the 1,221 game objects.

`boot-address-no-cscan.log` records SignalToAddress returning 0 and WaitForAddress returning the expected InvalidState (0xfa01) when the userspace value changed first. The original unimplemented-SVC traps are removed. This diagnostic run disabled speculative container scanning, but still hit the independent native ntdll access violation while shaders were being prepared (0 frames). Disabling the scan alone does not resolve the native failure; it is not committed as a workaround.

## Incremental build repair: game_20261022_010133.log

The build log contained LNK1104 for ssbu_glslang.dll. A stale ScarletRecomp process (PID 8412, executable path verified) also held scarlet_game.dll open; it was stopped before retrying. setup.py now keeps the existing glslang DLL when its shim, build script, headers and libraries are unchanged. A fixture checks missing/unchanged/changed inputs. The game's 14-case byte table at main rodata 0x389c584 (dispatch base 0x7001507078) now retains every destination, including the reported 0x70015070b8.

The exact incremental setup command was retried successfully: 10.9 seconds, 0/1221 source compilations on retry, glslang DLL reused, structural check passed. Initial target regeneration compiled only two sources. Native run fix-010133-run.log exited after 35 seconds with 0xc0000374 (STATUS_HEAP_CORRUPTION), immediately after reporting compute program #11278; timing alone does not prove that program caused the corruption. No first frame is confirmed. The earlier missing target is fixed; renderer heap corruption remains unresolved. REA objdump inspection was attempted and returned ERR_REA_WINDOWS_NATIVE, so no REA disassembly is claimed.
