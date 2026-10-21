#!/usr/bin/env python3
"""Validate a Pokémon Scarlet 1.0.0 ExeFS and convert its static NSOs to ELF.

This is the first executable Scarlet-specific stage in the ssbu-recomp-derived
pipeline. It deliberately does not lift or link anything yet.

Usage:
    python scripts/scarlet_ingest.py <dump-or-exefs> [--out build/binaries/1.0.0]
"""
import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import scarlet_profile as profile  # noqa: E402


class IngestError(RuntimeError):
    pass


def find_exefs(path):
    path = os.path.abspath(path)
    if os.path.basename(path).lower() == "exefs":
        return path
    candidate = os.path.join(path, "exefs")
    return candidate if os.path.isdir(candidate) else path


def read_nso_build_id(path):
    with open(path, "rb") as f:
        header = f.read(0x60)
    if len(header) < 0x54 or header[:4] != b"NSO0":
        raise IngestError("%s is not a valid NSO0 file" % path)
    # nso2elf.py currently prints the 20-byte value at 0x40. Keep the same
    # representation here so validation and conversion cannot disagree.
    return header[0x40:0x54].hex()


def require_inputs(exefs):
    missing = [name for name in profile.REQUIRED_EXEFS
               if not os.path.isfile(os.path.join(exefs, name))]
    if missing:
        raise IngestError("missing ExeFS file(s): %s" % ", ".join(missing))


def convert_one(name, exefs, out_dir):
    converter = os.path.join(REPO, "tools", "extract", "nso2elf.py")
    src = os.path.join(exefs, name)
    dst = os.path.join(out_dir, name + ".elf")
    p = subprocess.run(
        [sys.executable, converter, src, dst],
        cwd=REPO,
        capture_output=True,
        text=True,
    )
    log = (p.stdout or "") + (p.stderr or "")
    if p.returncode:
        raise IngestError("%s conversion failed (exit %d):\n%s" %
                          (name, p.returncode, log[-3000:]))
    if "MISMATCH" in log:
        raise IngestError("%s segment hash mismatch:\n%s" % (name, log[-3000:]))
    m = re.search(r"build id\s+([0-9a-fA-F]+)", log)
    build_id = m.group(1).lower() if m else read_nso_build_id(src)
    return dst, build_id, log


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("dump", help="dump root containing exefs/, or the exefs directory itself")
    ap.add_argument("--out", default=os.path.join(REPO, "build", "binaries", profile.BASE_VERSION),
                    help="output directory for converted ELF files")
    a = ap.parse_args(argv)

    exefs = find_exefs(a.dump)
    if not os.path.isdir(exefs):
        raise IngestError("ExeFS directory not found: %s" % exefs)
    require_inputs(exefs)

    main_path = os.path.join(exefs, "main")
    main_bid = read_nso_build_id(main_path)
    if not profile.matches_main_build_id(main_bid):
        raise IngestError(
            "unsupported Pokémon Scarlet main Build ID %s; expected %s for %s" %
            (main_bid, profile.MAIN_BUILD_ID, profile.BASE_VERSION)
        )

    out_dir = os.path.abspath(a.out)
    os.makedirs(out_dir, exist_ok=True)

    print("Scarlet Recomp ExeFS ingestion")
    print("  ExeFS:   %s" % exefs)
    print("  title:   %s (%s)" % (profile.GAME_NAME, profile.TITLE_ID_HEX))
    print("  version: %s" % profile.BASE_VERSION)
    print("  main ID: %s" % main_bid)

    converted = []
    for name in profile.STATIC_MODULES:
        dst, build_id, log = convert_one(name, exefs, out_dir)
        converted.append((name, dst, build_id))
        seg = next((line for line in log.splitlines() if line.startswith("seg 0 ")), "")
        print("  %-7s build %-16s  %s" % (name, build_id[:16], seg or "converted"))

    # Keep the non-code ExeFS pieces next to the ELFs for the later loader stage.
    for name in ("main.npdm", "rtld"):
        src = os.path.join(exefs, name)
        dst = os.path.join(out_dir, name)
        with open(src, "rb") as fi, open(dst, "wb") as fo:
            fo.write(fi.read())

    print("  output:  %s" % out_dir)
    print("OK: converted %d Scarlet static module(s)" % len(converted))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except IngestError as e:
        print("ERROR:", e, file=sys.stderr)
        raise SystemExit(2)
