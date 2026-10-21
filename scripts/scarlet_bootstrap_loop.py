#!/usr/bin/env python3
"""Iteratively push Scarlet boot through missing static indirect targets.

This is a bring-up helper, not a permanent runtime mode. It preserves the
existing build/lift cache: only a module with newly discovered late targets is
relifted, and setup.py recompiles only changed generated buckets/runtime files.
The loop stops as soon as the blocker is anything other than an unknown static
indirect-call target. If the game stays open, the loop naturally waits for the
user to close it.
"""
import argparse
import glob
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import scarlet_profile as profile  # noqa: E402

UNKNOWN_RE = re.compile(r"indirect call to an address that is not a known function \(0x([0-9a-fA-F]+)\)")
FRAME_RE = re.compile(r"display:\s+(\d+) frames presented")


def project_python():
    private = os.path.join(REPO, "deps", "python", "python.exe")
    return private if os.path.isfile(private) else sys.executable


def module_for(addr):
    if profile.MODULE_BASES["main"] <= addr < profile.MODULE_BASES["sdk"]:
        return "main"
    if profile.MODULE_BASES["sdk"] <= addr < profile.MODULE_BASES["subsdk0"]:
        return "sdk"
    if profile.MODULE_BASES["subsdk0"] <= addr < profile.MODULE_BASES["subsdk0"] + 0x10000000:
        return "subsdk0"
    return None


def target_file(build):
    return os.path.join(build, "scarlet_late_targets.txt")


def read_targets(build):
    out = {m: set(getattr(profile, "LATE_INDIRECT_STARTS", {}).get(m, ()))
           for m in profile.STATIC_MODULES}
    try:
        with open(target_file(build), encoding="utf-8", errors="replace") as f:
            for line in f:
                part = line.split()
                if len(part) != 2 or part[0] not in out:
                    continue
                try:
                    out[part[0]].add(int(part[1], 0))
                except ValueError:
                    pass
    except OSError:
        pass
    return out


def append_target(build, module, addr):
    os.makedirs(build, exist_ok=True)
    have = read_targets(build).get(module, set())
    if addr in have:
        return False
    with open(target_file(build), "a", encoding="utf-8") as f:
        f.write("%s 0x%x\n" % (module, addr))
    print("[scarlet-loop] learned %s late target 0x%x" % (module, addr), flush=True)
    return True


def table_has(build, module, addr):
    path = os.path.join(build, "lift", module, "table.c")
    try:
        text = open(path, encoding="latin-1").read().lower()
    except OSError:
        return False
    return ("{ 0x%xull," % addr).lower() in text


def run(cmd, env=None):
    print("\n[scarlet-loop] >", " ".join('"%s"' % x if " " in x else x for x in cmd), flush=True)
    return subprocess.call(cmd, cwd=REPO, env=env)


def refresh_missing(build, dump, jobs):
    targets = read_targets(build)
    dirty = []
    expected_rev = getattr(profile, "LIFT_ABI_REV", "")
    for module, addrs in targets.items():
        rev_ok = True
        if expected_rev:
            try:
                got_rev = open(os.path.join(build, "lift", module, ".scarlet_lift_rev"),
                               encoding="utf-8").read().strip()
            except OSError:
                got_rev = ""
            rev_ok = got_rev == expected_rev
        if not rev_ok or any(not table_has(build, module, a) for a in addrs):
            dirty.append(module)
    if not dirty:
        return True
    for module in dirty:
        code = run([project_python(), os.path.join(REPO, "scripts", "scarlet_lift_probe.py"),
                    "--module", module, "--jobs", str(jobs)])
        if code:
            return False
    code = run([project_python(), os.path.join(REPO, "scripts", "setup.py"),
                "--dump", dump, "--from", "lift", "--reuse-probe-lift", "--jobs", str(jobs)])
    return code == 0


def latest_log(build):
    logs = glob.glob(os.path.join(build, "logs", "game_*.log"))
    return max(logs, key=os.path.getmtime) if logs else None


def inspect_log(path):
    if not path:
        return None, 0, False
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return None, 0, False
    hits = UNKNOWN_RE.findall(text)
    frames = [int(x) for x in FRAME_RE.findall(text)]
    app_aborted = ("SetTerminateResult = 0x000002a2" in text or
                   "result=0x000002a2" in text)
    return (int(hits[-1], 16) if hits else None), (frames[-1] if frames else 0), app_aborted


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dump", default=r"D:\Pokemon Scarlet\Extracted\Base")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--max-rounds", type=int, default=30)
    ap.add_argument("--force-fallback", action="store_true",
                    help="retry ApplicationAborted with SCARLET_FORCE_BOOT (diagnostic only)")
    ap.add_argument("--build", default=os.path.join(REPO, "build"))
    a = ap.parse_args(argv)
    build = os.path.abspath(a.build)
    dump = os.path.abspath(a.dump)

    # First make sure the executable contains every target already learned.
    if not refresh_missing(build, dump, max(1, a.jobs)):
        return 2

    # Runtime/source commits may have changed even when lift tables were current.
    if run([project_python(), os.path.join(REPO, "scripts", "setup.py"),
            "--dump", dump, "--from", "stage", "--reuse-probe-lift",
            "--jobs", str(max(1, a.jobs))]):
        return 3

    for round_no in range(1, max(1, a.max_rounds) + 1):
        print("\n[scarlet-loop] ===== boot round %d =====" % round_no, flush=True)
        env = dict(os.environ)
        env.pop("SCARLET_FORCE_BOOT", None)
        env.pop("SCARLET_IPC_BOOT_TRACE", None)
        print("[scarlet-loop] trying normal nn::init::Start first", flush=True)
        run([project_python(), os.path.join(REPO, "scripts", "run.py"), "--play"], env=env)

        log = latest_log(build)
        target, frames, app_aborted = inspect_log(log)
        if not frames and target is None and app_aborted and a.force_fallback:
            print("[scarlet-loop] normal boot returned ApplicationAborted; --force-fallback requested", flush=True)
            env["SCARLET_FORCE_BOOT"] = "1"
            run([project_python(), os.path.join(REPO, "scripts", "run.py"), "--play"], env=env)
            log = latest_log(build)
            target, frames, app_aborted = inspect_log(log)
        elif not frames and target is None and app_aborted:
            print("[scarlet-loop] stopped on original ApplicationAborted; force boot disabled by default.", flush=True)
            print("[scarlet-loop] latest log:", log, flush=True)
            return 0

        if frames:
            print("[scarlet-loop] SUCCESS/PROGRESS: %d frame(s) presented. Latest log: %s" %
                  (frames, log), flush=True)
            return 0
        if target is None:
            print("[scarlet-loop] stopped: blocker is no longer a missing indirect target.", flush=True)
            print("[scarlet-loop] latest log:", log, flush=True)
            return 0

        module = module_for(target)
        if not module:
            print("[scarlet-loop] stopped: target 0x%x is outside known static modules." % target, flush=True)
            print("[scarlet-loop] latest log:", log, flush=True)
            return 4
        append_target(build, module, target)

        # Relift only the module that owns the newly reached target, then
        # combine/recompile incrementally. setup.py refuses stale probe tables.
        if run([project_python(), os.path.join(REPO, "scripts", "scarlet_lift_probe.py"),
                "--module", module, "--jobs", str(max(1, a.jobs))]):
            return 5
        if run([project_python(), os.path.join(REPO, "scripts", "setup.py"),
                "--dump", dump, "--from", "lift", "--reuse-probe-lift",
                "--jobs", str(max(1, a.jobs))]):
            return 6

    print("[scarlet-loop] stopped after %d rounds." % a.max_rounds, flush=True)
    return 7


if __name__ == "__main__":
    raise SystemExit(main())
