#!/usr/bin/env python3
"""Replay seek bench: the fast-seek case's seek (newest Disc 2 replay to input 6000, no rendering), timed by flycast.

Each run is killed after its JumpToKeyMsg line and reports that line's ms per frame. Several executables are run
in turn within each round, so they share the machine's drift over time; compare them only within one invocation.

usage: seek_bench.py --exe A.exe [--exe B.exe ...] [--runs N] [--hack on|off|both] [--config section:key=value ...]
                     [--sampler sampler.exe [--sample-after S]] [--out DIR]
--sampler: sample each run's threads with sampler.exe (Windows, see sampler.cpp) into <run dir>/rip.txt; read it
with rip_profile.py. Sampling slows the seek: its ms per frame are not comparable with unsampled runs.
Environment as run.py (GDXSV_ROM_DIR, GDXSV_REPLAY_DIR). Exit 0 when every run reported a seek.
"""
import argparse
import os
import statistics
import subprocess
import sys
import time

TESTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TESTS)
from lib import assets, emu  # noqa: E402
from cases import replay  # noqa: E402

TIMEOUT = 120  # s per run; a seek takes about 15 s


def run_one(exe: str, rom: str, pb: str, out: str, hack: str, extra: list, sampler: str, sample_after: float):
    cfg = ["log:Verbosity=3", "log:LogToFile=1", f"gdxsv:replay={pb}", "gdxsv:ReplayPOV=1",
           "gdxsv:ReplaySeekStates=no", f"gdxsv:replay_target_frame={replay.SEEK_FRAME}",
           f"gdxsv:SkipRenderingHack={'yes' if hack == 'on' else 'no'}", *extra]
    inst = emu.launch(exe, rom, out, config=cfg)
    samp = None
    log = os.path.join(out, "flycast.log")
    t0, hit = time.time(), None
    try:
        while time.time() - t0 < TIMEOUT and hit is None:
            if sampler and samp is None and time.time() - t0 >= sample_after:
                # the sampler stops when flycast exits
                samp = subprocess.Popen([sampler, str(inst.proc.pid), os.path.join(out, "rip.txt")])
            time.sleep(0.5)
            if os.path.exists(log):
                with open(log, encoding="utf-8", errors="replace") as f:
                    hit = replay.SEEK_RE.search(f.read())
    finally:
        inst.proc.kill()
        inst.proc.wait()
        if samp:
            samp.wait(30)
    return float(hit.group(3)) if hit and int(hit.group(1)) == replay.SEEK_FRAME else None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exe", action="append", required=True)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--hack", choices=["on", "off", "both"], default="both")
    ap.add_argument("--config", action="append", default=[])
    ap.add_argument("--sampler")
    ap.add_argument("--sample-after", type=float, default=4.0, help="s after launch; the seek starts about 3.6 s in")
    ap.add_argument("--out", default=os.path.join(TESTS, "out", "seek_bench"))
    a = ap.parse_args()
    entry = replay.newest_dc2()
    rom, pb = assets.rom(entry["disk"]), assets.replay(entry)
    if not rom:
        print("no ROM: set GDXSV_ROM_DIR")
        return 2
    modes = ["on", "off"] if a.hack == "both" else [a.hack]
    res = {(i, m): [] for i in range(len(a.exe)) for m in modes}
    for r in range(a.runs):
        for i, exe in enumerate(a.exe):
            for m in modes:
                v = run_one(exe, rom, pb, os.path.join(a.out, f"exe{i}_{m}_{r}"), m, a.config, a.sampler,
                            a.sample_after)
                res[(i, m)].append(v)
                print(f"round {r} exe{i} hack {m}: {v if v is not None else 'NO SEEK'} ms/fr", flush=True)
    for (i, m), v in res.items():
        ok = [x for x in v if x is not None]
        mean = f"mean {statistics.mean(ok):.3f}" if ok else "no seek"
        print(f"SUMMARY exe{i} {a.exe[i]} hack {m}: {mean} runs {v}")
    return 0 if all(None not in v for v in res.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
