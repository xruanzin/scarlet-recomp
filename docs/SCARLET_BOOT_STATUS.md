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
