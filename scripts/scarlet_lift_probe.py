#!/usr/bin/env python3
"""Run the ssbu-recomp EH-aware lifter against Scarlet static modules.

This does not compile or link the game. It proves that the lifter can parse and
translate each real Scarlet module and leaves generated C under build/lift/.
"""
import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import scarlet_profile as profile  # noqa: E402


def project_python():
    private = os.path.join(REPO, "deps", "python", "python.exe")
    return private if os.path.isfile(private) else sys.executable


def jobs_default():
    cpu = os.cpu_count() or 4
    return max(1, min(6, cpu))


def run_module(name, bin_dir, out_root, jobs):
    lifter_dir = os.path.join(REPO, "tools", "lifter")
    out = os.path.join(out_root, name)
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ)
    env["RECOMP_BIN"] = bin_dir
    env["RECOMP_MODULE"] = name
    env["RECOMP_BASE"] = hex(profile.MODULE_BASES[name])
    late = getattr(profile, "LATE_INDIRECT_STARTS", {}).get(name, ())
    if late:
        env["RECOMP_EXTRA_STARTS"] = ",".join(hex(x) for x in late)
        print("late indirect starts:", env["RECOMP_EXTRA_STARTS"], flush=True)
    module_map = os.path.join(REPO, "build", "scarlet_module_map.json")
    if os.path.isfile(module_map):
        env["RECOMP_MODULE_MAP"] = module_map
    env["PYTHONUNBUFFERED"] = "1"
    cmd = [project_python(), os.path.join(lifter_dir, "recomp_eh.py"), out, str(jobs)]
    print("\n== %s @ %#x ==" % (name, profile.MODULE_BASES[name]), flush=True)
    p = subprocess.Popen(
        cmd, cwd=lifter_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=1)
    lines = []
    assert p.stdout is not None
    for line in p.stdout:
        print(line, end="", flush=True)
        lines.append(line)
    code = p.wait()
    output = "".join(lines)
    if code:
        return {"module": name, "ok": False, "code": code}

    late = getattr(profile, "LATE_INDIRECT_STARTS", {}).get(name, ())
    if late:
        table_path = os.path.join(out, "table.c")
        try:
            table_text = open(table_path, encoding="latin-1").read().lower()
        except OSError:
            print("FATAL: generated table missing:", table_path, flush=True)
            return {"module": name, "ok": False, "code": 97}
        missing_late = [x for x in late if ("0x%xull" % x).lower() not in table_text]
        if missing_late:
            print("FATAL: late target(s) absent from generated %s table: %s" %
                  (name, ", ".join(hex(x) for x in missing_late)), flush=True)
            return {"module": name, "ok": False, "code": 98}
        print("verified late targets in %s/table.c: %s" %
              (name, ", ".join(hex(x) for x in late)), flush=True)
    m = re.search(r"functions:\s+(\d+).*instructions:\s+(\d+)\s+not translated:\s+(\d+)", output)
    return {
        "module": name,
        "ok": True,
        "functions": int(m.group(1)) if m else None,
        "instructions": int(m.group(2)) if m else None,
        "untranslated": int(m.group(3)) if m else None,
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--module", choices=("all",) + profile.STATIC_MODULES, default="all")
    ap.add_argument("--jobs", type=int, default=jobs_default())
    ap.add_argument("--bin", default=os.path.join(REPO, "build", "binaries", profile.BASE_VERSION))
    ap.add_argument("--out", default=os.path.join(REPO, "build", "lift"))
    a = ap.parse_args(argv)
    modules = profile.STATIC_MODULES if a.module == "all" else (a.module,)
    results = [run_module(n, os.path.abspath(a.bin), os.path.abspath(a.out), max(1, a.jobs))
               for n in modules]
    failed = [r for r in results if not r["ok"]]
    print("\nScarlet lifter probe summary")
    for r in results:
        if not r["ok"]:
            print("  %-7s FAILED exit=%d" % (r["module"], r["code"]))
        else:
            print("  %-7s functions=%s instructions=%s untranslated=%s" %
                  (r["module"], r["functions"], r["instructions"], r["untranslated"]))
    print("  generated: %s" % os.path.abspath(a.out))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
