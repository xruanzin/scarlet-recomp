# Scarlet porting plan

This branch turns the ssbu-recomp architecture into a Pokémon Scarlet target
without modifying the historical xruanzin/scarlet_pc repository.

## Proven inputs from scarlet_pc

The legacy Scarlet port has already established enough information that this is
not a restart:

- title ID: `0100A3D008C5C000`
- known base version: `1.0.0`
- known main Build ID:
  `ca44e7dd62e61a2a4e8889b3319ae173375770fd`
- static code modules already handled there: `main`, `sdk`, `subsdk0`
- existing AArch64 lifting, relocation discovery, EH function discovery,
  indirect-call handling, TLS/setjmp work, NVN/Vulkan fixes and profiling/REA
  findings are migration inputs, not reasons to keep the LAS runtime.

## Architectural rule

The base runtime/lifter is ssbu-recomp. Scarlet-specific knowledge is layered
on top. We do not wholesale merge scarlet_pc's runtime tree into this fork.

That gives three conceptual remotes:

- origin: xruanzin/scarlet-recomp
- upstream: Denkishi/ssbu-recomp
- legacy: xruanzin/scarlet_pc

## Phase S0 — repository bootstrap

- preserve GPL-3.0 and upstream history
- add Scarlet target metadata and provenance
- centralize title/build/module constants in `scripts/scarlet_profile.py`
- do not claim the game boots from this fork yet

## Phase S1 — Scarlet ExeFS ingestion

Replace SSBU build detection with NSO metadata/Build-ID based detection.

Required input:

```
<dump>/exefs/main
<dump>/exefs/main.npdm
<dump>/exefs/rtld
<dump>/exefs/sdk
<dump>/exefs/subsdk0
<dump>/romfs/...
```

The convert stage must produce ELF images for all three static code modules:

```
main     -> main.elf
sdk      -> sdk.elf
subsdk0  -> subsdk0.elf
```

No Scarlet address should be accepted unless the Build ID/profile matches.

## Phase S2 — generic multi-module static lifter

Generalize the ssbu main lifter so a module profile supplies:

- image path and guest base
- text/data ranges
- dynamic relocations
- symbol/import tables
- EH frame / LSDA data
- exported function table

Direct calls remain direct native C calls. Indirect calls use a fast per-site
cache before the shared lookup. Preserve ssbu's differential lifter tests.

## Phase S3 — C++ exceptions / unwind

Use ssbu-recomp's EH scheme as the primary implementation:

- parse FDE/LSDA
- identify landing pads
- emit resume entries
- preserve catch/throw behavior across lifted/native boundaries

Compare against the known setjmp/longjmp and EH behavior discovered in
scarlet_pc rather than discarding that evidence.

## Phase S4 — Switch runtime services

Start with ssbu-recomp's runtime and determine Scarlet's actually used surface.
Port Scarlet-specific behavior only where required. Hot tiny SDK functions may
receive host-native fast paths after correctness is proven.

## Phase S5 — NVN / Vulkan

Start from ssbu-recomp's NVN-to-Vulkan backend, then migrate only Scarlet fixes
that represent real API behavior or game-required extensions. Candidate legacy
areas include:

- shader interpolation
- storage buffers
- gathers
- stencil sampling
- transform feedback
- LUT/texture handling
- sampler behavior
- command-buffer/synchronization findings

Each migration needs a regression test before replacing the upstream behavior.

## Phase S6 — performance

Once first-frame/gameplay correctness exists:

- profile native host and lifted guest separately
- specialize hot BLR/vtable sites
- replace proven synchronization hot paths
- prepare shaders/pipelines outside frame-critical paths
- apply PGO only after semantic correctness
- use REA/Ghidra on measured hotspots, not arbitrary functions

## First milestone

The first concrete milestone for this fork is:

1. recognize a Scarlet 1.0.0 dump;
2. convert `main`, `sdk`, and `subsdk0` to ELF;
3. lift and compile them with the ssbu-derived pipeline;
4. link a Scarlet-named host executable/game DLL;
5. enter the game's process startup far enough to produce a deterministic
   missing-service/import report.

That report becomes the runtime implementation queue.
