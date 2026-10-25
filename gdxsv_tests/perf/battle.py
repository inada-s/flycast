#!/usr/bin/env python3
"""Pre-release perf check: 2-peer local GGPO battle cells with the runtime trace, PresentMon on peer 1.

Peer 1 makes random-time dpad presses (GDXSV_PRESS_SWEEP); peer 2 plays a fixed test-joystick pattern, one input
change every PATTERN_FRAMES vblanks, so peer 1's rollbacks do not overlap. Input delay 2 (TEST_GGPO_DELAY).
Each cell: metrics from analyze.py -> <out>/<cell>_<run>.json; with --baseline, compare.py judges them and a
failing cell is re-run up to 2 more times and judged on the median.

usage: battle.py --exe flycast.exe [--cells all|20_tr1_vs1,...] [--runs N] [--baseline gdxsv_tests/perf/baseline.json]
                 [--out DIR] [--presentmon PresentMon.exe] [--write-baseline FILE] [--config ...] [--p1-config window:left=2600 ...] [--p2-headless]
                 [--hz HZ]
Environment as run.py (GDXSV_ROM_DIR); GDXSV_STATE_DIR: the rollback suite's state dir; PRESENTMON: PresentMon 2.x exe.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import threading
import time

PERF = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(PERF)
sys.path.insert(0, TESTS)
sys.path.insert(0, os.path.join(TESTS, "rollback"))
sys.path.insert(0, PERF)
import analyze  # noqa: E402
import compare  # noqa: E402
import rbk_lib  # noqa: E402

# name -> (send delay p1, p2 in ms, threaded rendering, vsync[, env[, config[, peer 1 config]]])
# 20_* / 30_170_*_vs1: the baseline cells (--cells all), DirectX 11.
# *_tr0_*: threaded rendering off, for builds before it became the only mode; later builds run them threaded.
# The others, by name: run with the display at the rate to check (60 Hz shows a Present that waits for vsync).
# *_dx / *_gl / *_vk: DirectX 11 / OpenGL / Vulkan with vsync; *_fs: peer 1 full screen.
# slow_*: each emulated frame 3 ms slower (GDXSV_TEST_FRAME_COST_US), as on a slower PC; *_60hz / *_5994hz: Present
# waits for the vblank of a simulated 60 / 59.94 Hz display (GDXSV_TEST_PRESENT_HZ), any display.
BASELINE_CELLS = ["20_tr0_vs0", "20_tr0_vs1", "20_tr1_vs0", "20_tr1_vs1", "30_170_tr0_vs1", "30_170_tr1_vs1"]
GL, VK, FS = ["config:pvr.rend=0"], ["config:pvr.rend=4"], ["window:fullscreen=yes"]
SLOW = {"GDXSV_TEST_FRAME_COST_US": "3000"}
CELLS = {
    "20_tr0_vs0": (20, 20, "no", "no"),
    "20_tr0_vs1": (20, 20, "no", "yes"),
    "20_tr1_vs0": (20, 20, "yes", "no"),
    "20_tr1_vs1": (20, 20, "yes", "yes"),
    "30_170_tr0_vs1": (30, 170, "no", "yes"),
    "30_170_tr1_vs1": (30, 170, "yes", "yes"),
    "30_170_tr0_dx_fs": (30, 170, "no", "yes", {}, [], FS),
    "30_170_tr1_dx_fs": (30, 170, "yes", "yes", {}, [], FS),
    "30_170_tr0_gl": (30, 170, "no", "yes", {}, GL),
    "30_170_tr1_gl": (30, 170, "yes", "yes", {}, GL),
    "30_170_tr0_gl_fs": (30, 170, "no", "yes", {}, GL, FS),
    "30_170_tr1_gl_fs": (30, 170, "yes", "yes", {}, GL, FS),
    "30_170_tr0_vk": (30, 170, "no", "yes", {}, VK),
    "30_170_tr1_vk": (30, 170, "yes", "yes", {}, VK),
    "slow_tr0_dx": (30, 170, "no", "yes", SLOW),
    "slow_tr1_dx": (30, 170, "yes", "yes", SLOW),
    "slow_tr0_gl": (30, 170, "no", "yes", SLOW, GL),
    "slow_tr1_gl": (30, 170, "yes", "yes", SLOW, GL),
    "slow_tr0_novs": (30, 170, "no", "no", SLOW),
    "slow_tr1_novs": (30, 170, "yes", "no", SLOW),
    "slow_tr0_60hz": (30, 170, "no", "no", {**SLOW, "GDXSV_TEST_PRESENT_HZ": "60"}),
    "slow_tr1_60hz": (30, 170, "yes", "no", {**SLOW, "GDXSV_TEST_PRESENT_HZ": "60"}),
    "slow_tr0_5994hz": (30, 170, "no", "no", {**SLOW, "GDXSV_TEST_PRESENT_HZ": "59.94"}),
    "slow_tr1_5994hz": (30, 170, "yes", "no", {**SLOW, "GDXSV_TEST_PRESENT_HZ": "59.94"}),
}
SETTLE, FRAMES = 180, 2100  # GGPO frames skipped while timesync converges, then measured (35 s)
RUN_S = 60  # wall time per match: ~5 s to battle frame 0, settle, window, margin
PATTERN_FRAMES = 12  # more than the deepest rollback + input delay
SWEEP = "0,0,100,250,400"  # player index 0, from 0 ms, hold 100 ms, gap 250-400 ms
DPAD_LEFT, DPAD_RIGHT = 13, 14
# SDL hints from the environment: a connected pad would take port 0 from the test joystick
NO_REAL_PADS = {"SDL_XINPUT_ENABLED": "0", "SDL_DIRECTINPUT_ENABLED": "0", "SDL_JOYSTICK_RAWINPUT": "0",
                "SDL_JOYSTICK_HIDAPI": "0", "SDL_JOYSTICK_WGI": "0"}


def pattern(path: str) -> None:
    """Peer 2: press left, release, press right, release, ... one change every PATTERN_FRAMES vblanks."""
    with open(path, "w") as f:
        for n in range(1, 4 * RUN_S * 60 // PATTERN_FRAMES):
            button = DPAD_LEFT if n // 2 % 2 == 0 else DPAD_RIGHT
            f.write(f"{n * PATTERN_FRAMES} b {button} {n % 2}\n")


def cpu_seconds(pid: int):
    """User + kernel CPU time of a process (Windows), None elsewhere or once it ended."""
    if os.name != "nt":
        return None
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None
    t = [wintypes.FILETIME() for _ in range(4)]
    ok = k32.GetProcessTimes(h, *[ctypes.byref(x) for x in t])
    k32.CloseHandle(h)
    ft = lambda f: (f.dwHighDateTime << 32 | f.dwLowDateTime) / 1e7
    return ft(t[2]) + ft(t[3]) if ok else None


def sample_cpu(pid: int, result: dict) -> None:
    """E1: peer 1's CPU use from 10 s to 40 s after its start (the battle), in % of one core."""
    time.sleep(10)
    c0, w0 = cpu_seconds(pid), time.time()
    time.sleep(30)
    c1, w1 = cpu_seconds(pid), time.time()
    if c0 is not None and c1 is not None:
        result["cpu_pct"] = round(100 * (c1 - c0) / (w1 - w0), 1)


def run_cell(s, name: str, tag: str, out: str, presentmon: str, config: list, p1_config: list, hz: float, p2_headless: bool) -> dict:
    d1, d2, tr, vs, *extra = CELLS[name]
    p1_pad = os.path.join(out, "p1.joystick")
    p2_pad = os.path.join(out, "p2.joystick")
    open(p1_pad, "w").close()
    pattern(p2_pad)
    env = {"TEST_GGPO_DELAY": "2", "VITAL": "600", "GGPO_TEST_LOG": "1", "TEST_PING_MS": "2000",
           "GDXSV_PERF_TRACE": "perf.csv", **NO_REAL_PADS, **(extra[0] if extra else {})}
    peer_env = {1: {"GDXSV_TEST_JOYSTICK": p1_pad, "GDXSV_PRESS_SWEEP": SWEEP},
                2: {"GDXSV_TEST_JOYSTICK": p2_pad}}
    cfg = [f"config:rend.ThreadedRendering={tr}", "input:JoystickPolling=yes", f"config:rend.vsync={vs}",
           "config:rend.DelayFrameSwapping=no", "config:pvr.rend=2"] + (extra[1] if len(extra) > 1 else []) + config
    pm_csv = os.path.join(out, tag + "_pm.csv")
    pm_proc = []
    cpu = {}

    def on_start(m):
        threading.Thread(target=sample_cpu, args=(m.peers[0].inst.proc.pid, cpu), daemon=True).start()
        if presentmon:
            pm_proc.append(subprocess.Popen(
                [presentmon, "--process_id", str(m.peers[0].inst.proc.pid), "--qpc_time", "--v1_metrics", "--no_console_stats",
                 "--stop_existing_session", "--output_file", pm_csv],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))

    # peer 2 without random input: its pad pattern alone drives it. Peer 1's sweep turns its random input off.
    m = rbk_lib.run_match(s, tag, [s.exe] * 2, [d1, d2], headless=False, env=env, config=cfg, timeout=RUN_S,
                          on_start=on_start, peer_env=peer_env, peer_config={1: (extra[2] if len(extra) > 2 else []) + p1_config, 2: ["gdxsv:rand_input=0"] + (["gdxsv:headless=yes"] if p2_headless else [])})
    for p in pm_proc:  # --terminate_on_proc_exit does not fire when flycast is killed
        subprocess.run([presentmon, "--terminate_existing_session"], capture_output=True, timeout=30)
        try:
            p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            p.kill()
    p1, p2 = m.peers
    r = analyze.analyze(os.path.join(p1.dir, "perf.csv"), pm_csv if presentmon else None, SETTLE, FRAMES, hz)
    # D1: the rollback suite's checks that apply to a match cut before its end (state hashes differ per peer
    # by design; round winners and last frames need a finished match, the hash log is cut at the kill).
    if "cpu_pct" in cpu:
        r["E1"] = cpu
    r["D1"] = {"failures": sum(len(p.errors) for p in m.peers) + len(m.stalled)}
    r["checks"] = {"delay2": all("delay=2" in p.log for p in m.peers), "rtt": all("rtt=" in p.log for p in m.peers),
                   "p2_no_rand": "RandomInput" not in p2.log, "sweep": "press sweep: player 0" in p1.log,
                   "errors": [e for p in m.peers for e in p.errors][:5], "stalled": m.stalled}
    with open(os.path.join(out, tag + ".json"), "w") as f:
        json.dump(r, f, indent=1)
    return r


def median_runs(runs: list) -> dict:
    """Per-leaf median over runs of the same cell."""
    if len(runs) == 1:
        return runs[0]
    out = {}
    for k, v in runs[0].items():
        if isinstance(v, dict):
            out[k] = median_runs([r.get(k, {}) for r in runs])
        elif isinstance(v, (int, float)) and not isinstance(v, bool):
            vals = [r[k] for r in runs if isinstance(r.get(k), (int, float))]
            out[k] = statistics.median(vals) if vals else None
        else:
            out[k] = v
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--cells", default="all")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--baseline")
    ap.add_argument("--write-baseline")
    ap.add_argument("--presentmon", default=os.environ.get("PRESENTMON"))
    ap.add_argument("--out", default=os.path.join(TESTS, "out", "perf"))
    ap.add_argument("--config", nargs="*", default=[], help="more config for both peers, after the cell's")
    ap.add_argument("--p2-headless", action="store_true", help="no window for peer 2: only peer 1 presents to the desktop")
    ap.add_argument("--p1-config", nargs="*", default=[], help="more peer 1 config, e.g. window:left=2600 to open on a second display")
    ap.add_argument("--hz", type=float, help="peer 1's display refresh rate, for J1")
    a = ap.parse_args()
    rom = os.path.join(os.environ.get("GDXSV_ROM_DIR", ""), "gdx-disc2", "gdx-disc2.gdi")
    state = os.environ.get("GDXSV_STATE_DIR", os.path.join(TESTS, "out", "state"))
    os.makedirs(a.out, exist_ok=True)
    rbk_lib.N = 2
    s = rbk_lib.Settings(exe=a.exe, rom=rom, out=a.out, state_dir=state)
    pm = a.presentmon if a.presentmon and os.path.exists(a.presentmon) else None
    if not pm:
        print("no PresentMon: A1, A2, B1 not measured")
    base = json.load(open(a.baseline)) if a.baseline else None
    cells = BASELINE_CELLS if a.cells == "all" else a.cells.split(",")
    result, ok = {}, True
    for name in cells:
        runs = []
        while True:
            t = time.time()
            runs.append(run_cell(s, name, f"{name}_{len(runs) + 1}", a.out, pm, a.config, a.p1_config, a.hz, a.p2_headless))
            print(f"{name} run {len(runs)}: {time.time() - t:.0f} s, checks {runs[-1]['checks']}", flush=True)
            if len(runs) < a.runs:
                continue
            med = median_runs(runs)
            if not base or name not in base:
                break
            fails = compare.judge(name, base[name], med)
            hard = [f for f in fails if f.startswith("D1")]
            if not fails or hard or len(runs) >= 3:
                break
            print(f"{name}: outside tolerance ({'; '.join(fails)}), re-run", flush=True)
        med["runs"] = len(runs)
        if a.runs > 1:
            med["spread"] = compare.spread(runs)
        result[name] = med
        if base and name in base:
            ok &= not compare.judge(name, base[name], med)
        ok &= med.get("D1", {}).get("failures", 1) == 0
    with open(os.path.join(a.out, "result.json"), "w") as f:
        json.dump(result, f, indent=1)
    if a.write_baseline:
        with open(a.write_baseline, "w") as f:
            json.dump(result, f, indent=1)
    print(compare.table(base, result))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
