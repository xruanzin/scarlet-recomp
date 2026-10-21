#!/usr/bin/env python3
"""Audit guest_unimpl sites produced by the Scarlet lifter.

Scans build/lift/<module>/c_*.c, decodes the original AArch64 words with
Capstone, and ranks missing instruction mnemonics. This separates genuine
lifter gaps from invalid/data words reached by over-conservative function
discovery.
"""
import argparse
import collections
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import scarlet_profile as profile  # noqa: E402

SITE_RE = re.compile(
    r'guest_unimpl\((0x[0-9a-fA-F]+)ULL,\s*(0x[0-9a-fA-F]+)U,\s*"([^"]+)"\);'
)


def _ensure_private_python_for_capstone():
    try:
        import capstone  # noqa: F401
        return
    except ModuleNotFoundError:
        pass
    private = os.path.join(REPO, "deps", "python", "python.exe")
    same = (os.path.normcase(os.path.abspath(sys.executable)) ==
            os.path.normcase(os.path.abspath(private)))
    if os.path.isfile(private) and not same:
        print("Capstone is not installed in %s; relaunching with project Python: %s" %
              (sys.executable, private), flush=True)
        os.execv(private, [private, os.path.abspath(__file__)] + sys.argv[1:])
    raise RuntimeError(
        "Python dependency 'capstone' is not available. Run once:\n"
        "  powershell.exe -NoProfile -ExecutionPolicy Bypass "
        "-File scripts\\bootstrap.ps1 -NoLaunch"
    )


def decoder():
    _ensure_private_python_for_capstone()
    import capstone
    arch = getattr(capstone, "CS_ARCH_AARCH64",
                   getattr(capstone, "CS_ARCH_ARM64", None))
    if arch is None:
        raise RuntimeError("this Capstone build has no AArch64 decoder")
    mode = getattr(capstone, "CS_MODE_LITTLE_ENDIAN", 0)
    md = capstone.Cs(arch, mode)
    md.detail = False
    return md


def decode(md, word, address):
    raw = int(word).to_bytes(4, "little")
    ins = next(iter(md.disasm(raw, address, count=1)), None)
    if ins is None:
        return "<invalid>", ""
    return ins.mnemonic, ins.op_str


def scan_module(root, module, md):
    path = os.path.join(root, module)
    reasons = collections.Counter()
    mnemonics = collections.Counter()
    pairs = collections.Counter()
    words = collections.Counter()
    examples = {}
    files = sites = 0
    if not os.path.isdir(path):
        raise FileNotFoundError(path)
    for name in sorted(os.listdir(path)):
        if not (name.startswith("c_") and name.endswith(".c")):
            continue
        files += 1
        with open(os.path.join(path, name), encoding="latin-1") as f:
            text = f.read()
        for m in SITE_RE.finditer(text):
            addr = int(m.group(1), 16)
            word = int(m.group(2), 16)
            reason = m.group(3)
            mnemonic, op_str = decode(md, word, addr)
            sites += 1
            reasons[reason] += 1
            mnemonics[mnemonic] += 1
            pairs[(reason, mnemonic)] += 1
            words[(reason, mnemonic, word)] += 1
            examples.setdefault((reason, mnemonic), {
                "address": addr,
                "word": word,
                "op_str": op_str,
                "file": name,
            })
    return {
        "module": module,
        "files": files,
        "sites": sites,
        "reasons": reasons,
        "mnemonics": mnemonics,
        "pairs": pairs,
        "words": words,
        "examples": examples,
    }


def serialise(result):
    return {
        "module": result["module"],
        "files": result["files"],
        "sites": result["sites"],
        "reasons": dict(result["reasons"]),
        "mnemonics": dict(result["mnemonics"]),
        "pairs": [
            {
                "reason": reason,
                "mnemonic": mnemonic,
                "count": count,
                "example": result["examples"][(reason, mnemonic)],
            }
            for (reason, mnemonic), count in result["pairs"].most_common()
        ],
        "top_words": [
            {
                "reason": reason,
                "mnemonic": mnemonic,
                "word": "0x%08x" % word,
                "count": count,
            }
            for (reason, mnemonic, word), count in result["words"].most_common(100)
        ],
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--lift", default=os.path.join(REPO, "build", "lift"))
    ap.add_argument("--module", choices=("all",) + profile.STATIC_MODULES, default="all")
    ap.add_argument("--out", default=os.path.join(REPO, "build", "scarlet_untranslated.json"))
    ap.add_argument("--top", type=int, default=25)
    a = ap.parse_args(argv)
    mods = profile.STATIC_MODULES if a.module == "all" else (a.module,)
    md = decoder()
    results = [scan_module(os.path.abspath(a.lift), m, md) for m in mods]

    print("Scarlet untranslated AArch64 audit")
    total = 0
    combined = collections.Counter()
    combined_pairs = collections.Counter()
    for r in results:
        total += r["sites"]
        combined.update(r["mnemonics"])
        combined_pairs.update(r["pairs"])
        invalid = r["mnemonics"].get("<invalid>", 0)
        pct = 100.0 * invalid / r["sites"] if r["sites"] else 0.0
        print("\n  %s: %d sites in %d generated files; invalid/data=%d (%.1f%%)" %
              (r["module"], r["sites"], r["files"], invalid, pct))
        for (reason, mnemonic), count in r["pairs"].most_common(a.top):
            ex = r["examples"][(reason, mnemonic)]
            asm = mnemonic + ((" " + ex["op_str"]) if ex["op_str"] else "")
            print("    %-12s %-10s %7d  example %#x: %08x  %s" %
                  (reason, mnemonic, count, ex["address"], ex["word"], asm))

    print("\n  combined: %d untranslated sites" % total)
    for mnemonic, count in combined.most_common(a.top):
        print("    %-12s %8d" % (mnemonic, count))

    payload = {
        "format": 1,
        "game": profile.GAME_NAME,
        "version": profile.BASE_VERSION,
        "total_sites": total,
        "combined_mnemonics": dict(combined),
        "combined_pairs": [
            {"reason": reason, "mnemonic": mnemonic, "count": count}
            for (reason, mnemonic), count in combined_pairs.most_common()
        ],
        "modules": {r["module"]: serialise(r) for r in results},
    }
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2, sort_keys=True)
    print("\n  report: %s" % os.path.abspath(a.out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
