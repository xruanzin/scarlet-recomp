# Scarlet CreateThread 0x2a2: focused investigation

Baseline: 6737edede256d9eb2c34c39ee52cf6495789f948, branch fix/scarlet-pre-tls-regression.
Input: build/logs/game_20261022_045417.log and its boot_trace.txt. No ThreadType/TLR HLE was added or restored.

## Cause and exact SDK branch

REA/Ghidra analysis of the original ARM64 instruction bytes confirms:

- 0x70063769d0 forwards public CreateThread arguments to the SDK ThreadManager.
- 0x7006377e74 supplies processor -2 (default); it does not validate/reject the ThreadType.
- 0x7006377b78 clears/initializes the original SDK ThreadType (0x1c0 bytes), stores priority, entry, argument and stack fields, then calls the stack preparer through 0x70065c9d80 (0x700637e158).
- At 0x7006377c6c, TBZ w0,#0 branches to 0x7006377d88 if stack preparation returned false. 0x7006377d9c invokes the SDK assertion/abort helper. This precedes kernel thread creation at 0x7006377c90; neither SVC CreateThread priority/core checks nor the new thread's TLS/TLR entry has run yet.
- 0x700637e158 returns false when the alias allocator returns 2. The direct lifted implementation connects this to the exhausted alias-map retries.
- The direct caller 0x7002fc0a98 allocates a 0x1000-aligned stack and passes its pointer/size to original nn::os::CreateThread for ThreadType 0x70043d1f88, entry 0x7002fc0fa4. No broad nn::os override is needed.

The old trace repeatedly rejected MapMemory(src=0x727bfff000,size=0x20000) with 0xdc01. That span crosses the host heap region boundary at 0x727c000000. Runtime map_memory incorrectly required the entire source to fit one VirtualQuery result. Writable committed guest memory may span adjacent Windows reservations/regions. Moreover, one VirtualProtect cannot cross separate MEM_RESERVE allocations, so removing only the validation would leave mapping/unmapping broken.

## Minimal runtime change

Only runtime/scarlet_memory.c changes: validate the entire source as committed, writable, unguarded memory with consistent protection; apply/revert protection in 64KiB-bounded chunks. Preserve source protection, inaccessible source while aliased, alias content, unmap copyback and rollback after partial protection failure. Keep all alignment, overlap, bounds and invalid-source failures.

SVC CreateThread, its kernel-thread table, SDK entry trampoline, TLS, TPIDRRO, GetCurrentThread and GetTlsValue are unchanged. Priority/core validation remains in the original runtime path, reached after successful stack preparation.

## REA evidence and limits

Windows REA rejected native ARM64 ELF input (PE-only provider admission). To avoid whole-game analysis, 22KiB of code slices were copied byte-for-byte into an analysis-only ARM64 PE with original virtual addresses and explicit analysis entry exports. Four requested functions and one direct helper were queried. The PE is never executed or shipped with the port; extracted game bytes are not committed. External code/data outside the slices is intentionally absent. Ghidra's inferred Windows ABI signatures and fallthrough after the unresolved abort helper are not treated as original SDK source; the exact branch instructions and lifted code were cross-checked.

Final slice container SHA-256: 560fcfa13480e16a25d3259c4f7717c3e05e4cf9ba7424df86683478b4fe7308.
REA Evidence IDs (full local snapshot retained in the task workspace):
- 0x70063769d0: ev_2d90b9ea4ae2d6e06870c2405d5f165f48acdaafa18601cd7aee9fb914cfa247
- 0x7006377e74: ev_0a5defe20e41a4780e0c7cb8cf8f31769100ba18f83821911dcab9566ecda08d
- 0x7006377b78: ev_e0bcf229c68dacb119acbe60f9c6ae6914c6db741a7d2dd92f5fff37d140b271
- 0x7002fc0a98: ev_2600e155c6188803b7b07570473ef035e0ab8b4424a7f301addf0e4987e8d7cd
- 0x700637e158: ev_2ce7ce23e1d143abec7ee5f42a1c6b6a99c0169d1f13fd7b160bf8ac5ab71f72

## Verification and remaining boot

Native tests/scarlet_memory_test.c passed: a 0x20000 stack starting 0xf000 into a 64KiB reservation spans the next independent reservation, maps successfully, protects both pieces, copies modifications back and restores both protections. A missing next region is still rejected; partial protection failure restores earlier pages. The fixture now reserves the stack arena first, matching boot.c.

Runtime-only setup --only link completed successfully (2.5s link); no generated game sources were rebuilt. Real boot create-thread-stack-boot.log advanced past the old assertion and created kernel threads 0x20015 and 0x20016, including ThreadType 0x70043d1f88 and the next one at 0x70043d1938 (priority 43, core 0). audren:u requests continued. No low-address 0x60ade0 crash.

The next independent failure is IPC service 35 / command 5 returning 0xf601, reported by nn::codec::GetHardwareOpusDecoderWorkBufferSize, after GetService 'hwopus'. This is not a ThreadType/TLR failure and is not silently bypassed. It is outside the stack-map repair. The run exited 4 after 37 seconds, before command submission/draw/present; zero frames were presented. First image/gameplay are not achieved by this patch.
