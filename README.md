# Scarlet Recomp

**Pokémon Scarlet as a native PC program through static recompilation — work in progress.**

This repository is a fork of [Denkishi/ssbu-recomp](https://github.com/Denkishi/ssbu-recomp)
(OpenSmash-Ultimate). The goal is to reuse its static AArch64-to-C recompilation
architecture, native runtime and NVN-to-Vulkan approach for Pokémon Scarlet,
while carrying forward the Scarlet-specific reverse engineering already done in
[xruanzin/scarlet_pc](https://github.com/xruanzin/scarlet_pc).

> **Current status: bootstrap / not playable from this fork yet.**
> The old `scarlet_pc` repository is intentionally left untouched.

## Target

The first supported target is Pokémon Scarlet **1.0.0**:

- title ID: `0100A3D008C5C000`
- known main Build ID: `ca44e7dd62e61a2a4e8889b3319ae173375770fd`
- static modules: `main`, `sdk`, `subsdk0`
- host target: Windows x86-64
- graphics target: Vulkan

The design target is ahead-of-time static recompilation. Game AArch64 code is
translated to C and compiled to native host code; there should be no CPU
interpreter or gameplay JIT in the final path.

## Lineage

This project deliberately keeps the two source lineages separate:

- **Architecture/runtime base:** `Denkishi/ssbu-recomp`
- **Scarlet reverse engineering and proven game-specific behavior:**
  `xruanzin/scarlet_pc`

The new port is not a rename of the LAS-derived Scarlet runtime. Scarlet-specific
knowledge is migrated into the ssbu-derived architecture piece by piece.

See [SCARLET_PORTING.md](SCARLET_PORTING.md) for the engineering plan and
[NOTICE.md](NOTICE.md) for provenance.

## Legal / game data

This repository must contain **no Pokémon Scarlet game code, assets, keys or
firmware**. Generated lifted source and game DLLs are created locally from a
user's own legally obtained dump and remain outside Git.

The repository is GPL-3.0 because it derives from ssbu-recomp. MIT notices for
LAS Recomp-derived code migrated from the legacy project are preserved where
applicable.

## Development branch

Active Scarlet adaptation starts on:

```
scarlet-bootstrap
```

The `main` branch may remain close to upstream while the first Scarlet boot
pipeline is established.
