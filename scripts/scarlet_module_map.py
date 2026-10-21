#!/usr/bin/env python3
"""Map imports/exports between Scarlet's static ExeFS modules.

Reads the ELF files produced by scarlet_ingest.py and writes a JSON map that
later lifter stages can use to replace PLT calls with direct native calls when
a symbol is uniquely exported by a sibling module.
"""
import argparse
import json
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import scarlet_profile as profile  # noqa: E402

ELF_OFF = 0x1000


def sections(data):
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shnum, shstrndx = struct.unpack_from("<HH", data, 0x3c)
    rows = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * 64)
            for i in range(shnum)]
    shstr = rows[shstrndx]
    names = data[shstr[4]:shstr[4] + shstr[5]]
    out = {}
    for row in rows:
        no = row[0]
        if no >= len(names):
            continue
        end = names.find(b"\0", no)
        if end < 0:
            continue
        out[names[no:end].decode("latin-1")] = row
    return out


def load_module(name, path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        raise ValueError("%s is not an ELF file" % path)
    sec = sections(data)
    sym = sec[".dynsym"]
    st = sec[".dynstr"]
    text = sec[".text"]
    str_data = data[st[4]:st[4] + st[5]]
    symbols = []
    for i in range(sym[5] // 24):
        off = sym[4] + i * 24
        no, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", data, off)
        if no >= len(str_data):
            sname = ""
        else:
            end = str_data.find(b"\0", no)
            sname = str_data[no:end].decode("latin-1") if end >= 0 else ""
        symbols.append({
            "index": i,
            "name": sname,
            "info": info,
            "type": info & 0x0f,
            "bind": info >> 4,
            "defined": bool(shndx),
            "shndx": shndx,
            "value": value,
            "size": size,
        })
    base = profile.MODULE_BASES[name]
    lo, hi = base + text[3], base + text[3] + text[5]
    exports = []
    imports = []
    for symrow in symbols:
        if not symrow["name"]:
            continue
        if symrow["defined"]:
            row = dict(symrow)
            row["address"] = base + symrow["value"]
            row["in_text"] = lo <= row["address"] < hi
            exports.append(row)
        else:
            imports.append(dict(symrow))
    return {
        "name": name,
        "base": base,
        "text_start": lo,
        "text_end": hi,
        "text_size": text[5],
        "symbols": len(symbols),
        "imports": imports,
        "exports": exports,
    }


def build_map(bin_dir):
    modules = {}
    for name in profile.STATIC_MODULES:
        path = os.path.join(bin_dir, name + ".elf")
        if not os.path.isfile(path):
            raise FileNotFoundError(path)
        modules[name] = load_module(name, path)

    by_name = {}
    for name, mod in modules.items():
        for e in mod["exports"]:
            by_name.setdefault(e["name"], []).append({
                "module": name,
                "address": e["address"],
                "value": e["value"],
                "size": e["size"],
                "type": e["type"],
                "bind": e["bind"],
                "in_text": e["in_text"],
            })

    resolutions = {}
    unresolved = {}
    ambiguous = {}
    for name, mod in modules.items():
        r, u, a = {}, [], {}
        for imp in mod["imports"]:
            candidates = [x for x in by_name.get(imp["name"], [])
                          if x["module"] != name]
            unique_addr = {x["address"] for x in candidates}
            if len(unique_addr) == 1:
                target = candidates[0]
                target = dict(target)
                target["resolution"] = "unique"
                r[imp["name"]] = target
            elif not candidates:
                u.append(imp["name"])
            else:
                # Horizon's static NSO lookup, and the proven Scarlet legacy
                # loader, search loaded modules in order. Scarlet loads
                # main -> sdk -> subsdk0, so duplicate global definitions
                # such as operator new/delete bind to the first provider.
                rank = {module: i for i, module in enumerate(profile.STATIC_MODULES)}
                ordered = sorted(candidates, key=lambda x: rank.get(x["module"], 1 << 30))
                target = dict(ordered[0])
                target["resolution"] = "module-load-order"
                target["alternatives"] = ordered[1:]
                r[imp["name"]] = target
        resolutions[name] = r
        unresolved[name] = sorted(set(u))
        ambiguous[name] = a

    return {
        "format": 2,
        "game": profile.GAME_NAME,
        "version": profile.BASE_VERSION,
        "main_build_id": profile.MAIN_BUILD_ID,
        "modules": modules,
        "resolutions": resolutions,
        "unresolved": unresolved,
        "ambiguous": ambiguous,
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bin", default=os.path.join(REPO, "build", "binaries", profile.BASE_VERSION))
    ap.add_argument("--out", default=os.path.join(REPO, "build", "scarlet_module_map.json"))
    a = ap.parse_args(argv)
    result = build_map(os.path.abspath(a.bin))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, sort_keys=True)

    print("Scarlet static module map")
    for name in profile.STATIC_MODULES:
        m = result["modules"][name]
        by_target = {}
        for target in result["resolutions"][name].values():
            by_target[target["module"]] = by_target.get(target["module"], 0) + 1
        route = ", ".join("%s=%d" % x for x in sorted(by_target.items())) or "none"
        print("  %-7s text=%7.2f MiB  exports=%5d imports=%5d  sibling-resolved=%4d (%s)  unresolved=%4d ambiguous=%d" %
              (name, m["text_size"] / 1048576.0, len(m["exports"]), len(m["imports"]),
               len(result["resolutions"][name]), route, len(result["unresolved"][name]),
               len(result["ambiguous"][name])))
    print("  map: %s" % os.path.abspath(a.out))
    for name in profile.STATIC_MODULES:
        unresolved = result["unresolved"][name]
        ambiguous = result["ambiguous"][name]
        if unresolved:
            print("\n  %s unresolved external imports (%d):" % (name, len(unresolved)))
            for symbol in unresolved:
                print("    " + symbol)
        ordered = [(symbol, target) for symbol, target in result["resolutions"][name].items()
                   if target.get("resolution") == "module-load-order"]
        if ordered:
            print("\n  %s duplicate imports resolved by module load order (%d):" % (name, len(ordered)))
            for symbol, target in sorted(ordered):
                alts = ", ".join("%s@%#x" % (t["module"], t["address"])
                                 for t in target.get("alternatives", []))
                print("    %s -> %s@%#x%s" %
                      (symbol, target["module"], target["address"],
                       (" (before " + alts + ")") if alts else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
