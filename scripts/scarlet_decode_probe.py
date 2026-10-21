#!/usr/bin/env python3
"""Fast regression probe for Scarlet AArch64 decoder additions.

Uses representative instruction words observed in the user's real 1.0.0 lift.
It imports the lifter against main.elf and decodes the words directly without
running function discovery or generating 1,000+ C files.
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIFTER = os.path.join(REPO, "tools", "lifter")
BIN = os.path.join(REPO, "build", "binaries", "1.0.0")
if not os.path.isfile(os.path.join(BIN, "main.elf")):
    raise SystemExit("missing build/binaries/1.0.0/main.elf; run scarlet_ingest.py first")

os.environ["RECOMP_BIN"] = BIN
os.environ["RECOMP_MODULE"] = "main"
os.environ["RECOMP_BASE"] = "0x7000000000"
sys.path.insert(0, LIFTER)
import recomp  # noqa: E402

CASES = [
    ("udf",       0x00000000),
    ("urhadd",    0x2e211400),
    ("bic-imm",   0x2f077604),
    ("fmaxp",     0x2e35f6b6),
    ("uaddw",     0x2e2010e7),
    ("sqxtun",    0x2e2128e7),
    ("sqxtun2",   0x6e212807),
    ("fminp",     0x2ea6f4c7),
    ("shll",      0x2e613801),
    ("sqrshrun",  0x2f0c8c00),
    ("uaddw2",    0x6e201260),
    ("mls-elem",  0x6f4040c3),
    ("usubw2",    0x6e213064),
    ("sqdmulh",   0x4e62b4d1),
    ("clz",       0x2ea04800),
    ("srhadd",    0x4e611400),
    ("mla",       0x4eb29480),
    ("mul-elem",  0x4f7080c6),
    ("mla-elem",  0x6f6000a4),
    ("smlal",     0x0e6280a7),
    ("sqxtn",     0x0e614825),
    ("sqxtn2",    0x4e614845),
    ("sqshrun",   0x2f0d84c6),
    ("st4-elem",  0x0d20a184),
    ("ld2-elem",  0x0de742a3),
    ("frintz",    0x4ea19b3a),
    ("facge",     0x6e31ee10),
    ("umlal",     0x2e228065),
    ("orr-imm",   0x4f001420),
    ("sqadd",     0x4e610ce1),
    ("sqrdmulh",  0x6ea0b467),
    ("sqshl",     0x4ea34c84),
    ("sqrshl",    0x4ebe5f1a),
    ("shadd",     0x0e720454),
    ("shsub",     0x0e722456),
    ("sqrd-elem", 0x4f84d0a6),
    ("aese",      0x4e284820),
    ("aesimc",    0x4e287802),
    ("aesmc",     0x4e286800),
    ("aesd",      0x4e285820),
    ("smull",     0x0ea1c000),
    ("smlal-sdk", 0x0e78821c),
    ("sqrshrn",   0x5f119f9c),
    ("sha256h",   0x5e0442b6),
    ("sha256h2",  0x5e045295),
    ("sha256su0", 0x5e282820),
    ("sha256su1", 0x5e036040),
    ("rev32",     0x6e200821),
    ("ld4-elem",  0x0dff603c),
    ("pmull",     0x0ee1e002),
    ("pmull2",    0x4ee1e000),
    ("cntpct",    0xd53be020),
    ("msr-nzcv",  0xd51b4204),
    ("dc-civac",  0xd50b7e29),
    ("dc-cvau",   0xd50b7b2a),
    ("ic-ivau",   0xd50b7529),
    ("rev16-v",   0x4e201800),
    ("fcvtas-v",  0x4e21cbfd),
    ("fcvtzs-fx", 0x4f37fc63),
    ("fcvtzs-d",  0x4f7cfc00),
    ("sqxtn-h",   0x5e614a11),
    ("sqxtn-s",   0x5ea14a32),
    ("smlal-elem-long", 0x0f862060),
    ("unallocated-a57", 0x009ed9a4),
]

env = recomp.Env()
fn = recomp.Fn(recomp.BASE + 0x1000, recomp.BASE + 0x1004, env)
failed = []
print("Scarlet decoder smoke probe")
for name, word in CASES:
    try:
        stmt = fn.lift(word, recomp.BASE + 0x1000)
        print("  %-12s %08x  OK  %s" % (name, word, stmt[:90]))
    except recomp.Unimpl as e:
        failed.append((name, word, str(e)))
        print("  %-12s %08x  FAIL %s" % (name, word, e))
    except Exception as e:
        failed.append((name, word, type(e).__name__ + ": " + str(e)))
        print("  %-12s %08x  ERROR %s: %s" % (name, word, type(e).__name__, e))

print("\n  supported: %d/%d" % (len(CASES) - len(failed), len(CASES)))
if failed:
    print("FAIL: %d representative Scarlet instructions still miss the intended decoder path" % len(failed))
    return_code = 1
else:
    print("OK: all representative Scarlet decoder additions are recognized")
    return_code = 0
raise SystemExit(return_code)
