#!/usr/bin/env python3
"""WCN7850: run the host AWDL protocol checks copied from ForbiddenItlwm (tests/awdl_*.cpp) under the sanitizers.
Adapted from ForbiddenItlwm tools/test_awdl.py (GPL-2.0): our include layout, Tahoe target only, no Bluetooth tests."""
from pathlib import Path
import subprocess, sys, tempfile
root = Path(__file__).resolve().parents[1]
tests = sorted(p for p in (root / "tests").glob("awdl_*.cpp"))
failed = []
with tempfile.TemporaryDirectory(prefix="wcn7850-awdl-tests-") as directory:
    for source in tests:
        output = Path(directory) / source.stem
        cmd = ["xcrun", "clang++", "-std=c++14", "-D__IO80211_TARGET=__MAC_26_0", "-DAIRPORT_WCL",
               "-I" + str(root / "include"), "-I" + str(root / "airport" / "awdlnative"), "-I" + str(root / "airport" / "WCL"),
               "-fsanitize=address,undefined", str(source), "-o", str(output)]
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode:
            failed.append((source.name, "compile", build.stderr[-1500:]))
            print(f"FAIL compile {source.name}"); continue
        run = subprocess.run([str(output)], capture_output=True, text=True)
        if run.returncode:
            failed.append((source.name, "run", (run.stdout + run.stderr)[-1500:]))
            print(f"FAIL run {source.name}")
        else:
            print(f"ok {source.name}")
for name, stage, text in failed:
    print(f"\n==== {name} ({stage})\n{text}")
sys.exit(1 if failed else 0)
