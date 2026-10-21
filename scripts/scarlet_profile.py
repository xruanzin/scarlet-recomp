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
