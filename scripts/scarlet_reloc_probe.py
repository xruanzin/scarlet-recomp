#!/usr/bin/env python3
"""Validate the Scarlet C loader's multi-module ELF/relocation assumptions.

This is intentionally cheap: it does not lift or compile code. It reads the
three ELF files, classifies every AArch64 relocation with the same policy as
runtime/scarlet_loader.c, and fails if a strong unresolved symbol, unsupported
relocation type, malformed table, or module-slot overlap is found.
"""
import argparse
import json
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import scarlet_profile as profile  # noqa: E402
import scarlet_module_map as smap  # noqa: E402

R_ABS64 = 0x101
R_GLOB_DAT = 0x401
R_JUMP_SLOT = 0x402
R_RELATIVE = 0x403
SUPPORTED = {R_ABS64, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE}


def dyn_symbols(data, sec):
    sym = sec[".dynsym"]
    st = sec[".dynstr"]
    strings = data[st[4]:st[4] + st[5]]
    out = []
    for i in range(sym[5] // 24):
        no, info, other, shndx, value, size = struct.unpack_from(
            "<IBBHQQ", data, sym[4] + i * 24)
        if no < len(strings):
            end = strings.find(b"\0", no)
            name = strings[no:end].decode("latin-1") if end >= 0 else ""
        else:
            name = ""
        out.append({
            "name": name,
            "bind": info >> 4,
            "type": info & 0x0f,
            "defined": bool(shndx),
            "value": value,
            "size": size,
        })
    return out


def load_segments(data):
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    loads = []
    for i in range(phnum):
        p = phoff + i * phentsize
        typ, flags = struct.unpack_from("<II", data, p)
        if typ != 1:
            continue
        off, vaddr, paddr, filesz, memsz, align = struct.unpack_from(
            "<QQQQQQ", data, p + 8)
        loads.append({
            "flags": flags, "offset": off, "vaddr": vaddr,
            "filesz": filesz, "memsz": memsz, "align": align,
        })
    return loads


def dynamic_tags(data, sec):
    dyn = sec[".dynamic"]
    tags = {}
    for off in range(dyn[4], dyn[4] + dyn[5], 16):
        tag, value = struct.unpack_from("<qQ", data, off)
        if tag == 0:
            break
        tags.setdefault(tag, value)
    return tags


def relocations(data, sec):
    """Read the exact relocation tables runtime/scarlet_loader.c will use.

    nso2elf intentionally does not create SHT_RELA sections: relocation bytes
    remain inside the flat NSO memory image. DT_RELA/DT_JMPREL in .dynamic are
    therefore the authoritative table locations.
    """
    tags = dynamic_tags(data, sec)
    rows = []
    tables = (
        ("rela", tags.get(7, 0), tags.get(8, 0)),       # DT_RELA / DT_RELASZ
        ("jmprel", tags.get(23, 0), tags.get(2, 0)),   # DT_JMPREL / DT_PLTRELSZ
    )
    ent = tags.get(9, 24)  # DT_RELAENT
    if ent != 24:
        raise RuntimeError("DT_RELAENT is %d, expected 24" % ent)
    for name, addr, size in tables:
        if not addr and not size:
            continue
        if not addr or size % ent:
            raise RuntimeError("%s has malformed address/size %#x/%#x" %
                               (name, addr, size))
        for guest_off in range(addr, addr + size, ent):
            # nso2elf stores the complete NSO memory image at ELF file +0x1000.
            off = 0x1000 + guest_off
            if off + 24 > len(data):
                raise RuntimeError("%s relocation %#x falls outside ELF image" %
                                   (name, guest_off))
            where, info, addend = struct.unpack_from("<QQq", data, off)
            rows.append({
                "section": name,
                "table_address": guest_off,
                "offset": where,
                "type": info & 0xffffffff,
                "sym": info >> 32,
                "addend": addend,
            })
    return rows


def classify(module, row, syms, module_map):
    typ = row["type"]
    if typ not in SUPPORTED:
        return "unsupported"
    if typ == R_RELATIVE:
        return "relative"
    if row["sym"] >= len(syms):
        return "bad-symbol-index"
    sym = syms[row["sym"]]
    if sym["defined"]:
        return "own"
    name = sym["name"]
    target = module_map["resolutions"].get(module, {}).get(name)
    if module == "main" and name in profile.HLE_OVERRIDE_SYMBOLS:
        return "host-override"
    if target:
        return "sibling-" + target["module"]
    if sym["bind"] == 2:
        return "weak-null"
    return "strong-unresolved"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bin", default=os.path.join(
        REPO, "build", "binaries", profile.BASE_VERSION))
    ap.add_argument("--map", dest="map_path", default=os.path.join(
        REPO, "build", "scarlet_module_map.json"))
    a = ap.parse_args(argv)

    bin_dir = os.path.abspath(a.bin)
    # Always rebuild the map from the same ELFs in memory so this probe cannot
    # accidentally validate a stale JSON file.
    module_map = smap.build_map(bin_dir)
    if a.map_path:
        os.makedirs(os.path.dirname(os.path.abspath(a.map_path)), exist_ok=True)
        with open(a.map_path, "w", encoding="utf-8") as f:
            json.dump(module_map, f, indent=2, sort_keys=True)

    print("Scarlet relocation probe")
    failures = 0
    totals = {}
    for i, module in enumerate(profile.STATIC_MODULES):
        path = os.path.join(bin_dir, module + ".elf")
        data = open(path, "rb").read()
        sec = smap.sections(data)
        syms = dyn_symbols(data, sec)
        loads = load_segments(data)
        if len(loads) != 3:
            print("  %-7s FAIL: PT_LOAD count %d (expected 3)" %
                  (module, len(loads)))
            failures += 1
            continue
        base = profile.MODULE_BASES[module]
        limit = (profile.MODULE_BASES[profile.STATIC_MODULES[i + 1]]
                 if i + 1 < len(profile.STATIC_MODULES)
                 else profile.MODULE_BASES["main"] + 0x10000000)
        image_end = max(x["vaddr"] + x["memsz"] for x in loads)
        slot_ok = base + image_end <= limit
        rows = relocations(data, sec)
        if not rows:
            print("  %-7s FAIL: dynamic relocation tables are empty" % module)
            failures += 1
        counts = {}
        examples = {}
        for row in rows:
            kind = classify(module, row, syms, module_map)
            counts[kind] = counts.get(kind, 0) + 1
            examples.setdefault(kind, row)
        totals[module] = counts
        bad = sum(counts.get(k, 0) for k in (
            "unsupported", "bad-symbol-index", "strong-unresolved"))
        if not slot_ok:
            bad += 1
        failures += bad
        print("  %-7s image=%7.2f MiB slot=%s relocs=%7d" %
              (module, image_end / 1048576.0,
               "OK" if slot_ok else "OVERLAP", len(rows)))
        for kind in ("relative", "own", "sibling-main", "sibling-sdk",
                     "sibling-subsdk0", "host-override", "weak-null",
                     "strong-unresolved", "unsupported", "bad-symbol-index"):
            n = counts.get(kind, 0)
            if n:
                print("           %-20s %7d" % (kind, n))
        for kind in ("strong-unresolved", "unsupported", "bad-symbol-index"):
            if counts.get(kind):
                row = examples[kind]
                name = ""
                if row["sym"] < len(syms):
                    name = syms[row["sym"]]["name"]
                print("           FIRST %-16s off=%#x type=%#x sym=%d %s" %
                      (kind, row["offset"], row["type"], row["sym"], name))

    strong = sum(v.get("strong-unresolved", 0) for v in totals.values())
    unsupported = sum(v.get("unsupported", 0) for v in totals.values())
    print("\n  strong unresolved relocations: %d" % strong)
    print("  unsupported relocation types: %d" % unsupported)
    if failures:
        print("FAIL: %d loader assumption problem(s)" % failures)
        return 1
    print("OK: Scarlet static ELF layout and relocations match the C loader")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
