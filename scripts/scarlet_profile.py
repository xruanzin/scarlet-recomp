"""Pokémon Scarlet target profile for scarlet-recomp.

Keep game-specific constants here instead of scattering them through the SSBU
runtime/lifter. The initial values come from the already-working scarlet_pc
reverse-engineering branch and are intentionally scoped to Scarlet 1.0.0.
"""

GAME_NAME = "Pokémon Scarlet"
PROJECT_NAME = "Scarlet Recomp"

TITLE_ID = 0x0100A3D008C5C000
TITLE_ID_HEX = "0100A3D008C5C000"
BASE_VERSION = "1.0.0"

# Full main NSO Build ID observed by scarlet_pc for the base 1.0.0 executable.
MAIN_BUILD_ID = "ca44e7dd62e61a2a4e8889b3319ae173375770fd"
MAIN_BUILD_ID_PREFIX = MAIN_BUILD_ID[:16]

# Scarlet's static ExeFS code modules that scarlet_pc already recompiles.
STATIC_MODULES = ("main", "sdk", "subsdk0")
LIFT_ABI_REV = "scarlet-tpidrro-v1"
LIFT_ABI_REV_BY_MODULE = {
    "main": "scarlet-host-graphics-v2",
    "sdk": "scarlet-stdalloc-v1",
    "subsdk0": "scarlet-tpidrro-v1",
}
# Guest entry points discovered only through indirect calls during real boot.
# Keep these sparse: recomp.py splits the containing function at each address,
# so regenerating a module rewrites only the affected source bucket(s).
LATE_INDIRECT_STARTS = {
    # Bounded byte jump tables verified in main.elf during boot: asset decoding
    # at rodata 0x37a0744 (6 cases), graphics variants at 0x389c276 (20),
    # 0x389c2da (19), 0x389c301 (19), 0x389c2c6 (20), 0x3870e82 (4).
    "main": (
        0x70000511bc, 0x70000514c4, 0x7000051528,
        0x7000051554, 0x7000051580, 0x7000a7a1f4,
        0x7000a7a20c, 0x7000a7a26c, 0x7000a7d824,
        0x7000a7d83c, 0x7000a7d848, 0x7000a7d958,
        0x7000a7d988, 0x7000a7da98, 0x7000a7daec,
        0x7000a7db24, 0x7000a7db34, 0x7000a7dbbc,
    ),
    # SDK printf/va_arg tables at rodata 0xb2916c (56 u16) and 0xb291ec (18 u8).
    "sdk": (
        0x700657c484, 0x700657c4dc, 0x700657c500,
        0x700657c518, 0x700657c558, 0x700657c59c,
        0x700657c5f8, 0x700657c614, 0x700657c684,
        0x700657c6a8, 0x700657c6bc, 0x700657c724,
        0x700657c9b0, 0x700657ceac, 0x700657ced8,
        0x700657cefc, 0x700657cf20, 0x700657cf44,
        0x700657cf68, 0x700657cf8c, 0x700657cfb0,
        0x700657cff4,
    ),
    "subsdk0": (),
}

MODULE_BASES = {
    "main": 0x7000000000,
    "sdk": 0x7006000000,
    "subsdk0": 0x7008000000,
}
REQUIRED_EXEFS = ("main", "main.npdm", "rtld", "sdk", "subsdk0")

WINDOWS_EXE = "ScarletRecomp.exe"
GAME_DLL = "scarlet_game.dll"

UPSTREAM_REPO = "Denkishi/ssbu-recomp"
LEGACY_REPO = "xruanzin/scarlet_pc"


def matches_main_build_id(value):
    """Accept bytes or hex text and match only the known Scarlet 1.0.0 main."""
    if isinstance(value, bytes):
        value = value.hex()
    value = str(value).lower().replace("0x", "")
    return value == MAIN_BUILD_ID or value.startswith(MAIN_BUILD_ID_PREFIX)


# Scarlet 1.0.0 overrides already validated in the legacy native port. Keep
# these on the host fast path instead of converting their PLT imports to direct
# calls into sdk. Additions must be profile/gameplay validated first.
HLE_OVERRIDE_SYMBOLS = (
    "_ZN2nn2os16GetCurrentThreadEv",
    "_ZN2nn2os11GetTlsValueENS0_7TlsSlotE",
    "memcpy",
    "strcmp",
    # The host renderer owns the native display and NVN device. These public
    # entry points also form the renderer boundary in scarlet_pc/runtime/hle.
    "_ZN2nn2vi10InitializeEv",
    "_ZN2nn2vi8FinalizeEv",
    "_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",
    "_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE",
    "_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE",
    "_ZN2nn2vi12DestroyLayerEPNS0_5LayerE",
    "_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",
    "_ZN2nv18InitializeGraphicsEPvm",
    "_ZN2nv16FinalizeGraphicsEv",
    "_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_",
    "nvnBootstrapLoader",
)
