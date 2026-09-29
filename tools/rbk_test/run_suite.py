#!/usr/bin/env python3
"""Rollback netcode test suite. See README.md for what each case covers.

    python tools/rbk_test/run_suite.py --rom D:\\rom\\gdx-disc2\\gdx-disc2.gdi --old-release gdxsv-1.9.2
    python tools/rbk_test/run_suite.py --rom ... --cases A1,C1
    python tools/rbk_test/run_suite.py --list
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import time
from typing import Callable, Dict, List, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzz  # noqa: E402
import rbk_lib as lib  # noqa: E402
from rbk_lib import Match, Settings  # noqa: E402

PASS, FAIL, SKIP, INCONCLUSIVE = "PASS", "FAIL", "SKIP", "INCONCLUSIVE"
Result = Tuple[str, List[str]]

DELAYS = [16, 16, 16, 100]           # one slow sender: rollbacks and timesync on everyone
DELAYS_SPREAD = [50, 16, 80, 100]    # windowed runs: rollbacks on every peer
THREADED_OFF = ["--config", "config:rend.ThreadedRendering=no"]
LAST_FRAME_SLACK = 2


def common(m: Match, peers=None) -> List[str]:
    """Checks every completed match must pass. Returns the failures."""
    peers = peers or m.peers
    bad = []
    if m.timed_out:
        bad.append("timed out")
    if m.stalled:
        bad.append("stalled: " + " ".join(m.stalled))
    for p in peers:
        if not p.finished:
            bad.append(f"p{p.index} did not finish")
        for e in p.errors[:3]:
            bad.append(f"p{p.index}: {e.strip()}")
    # Teardown timing can leave one peer a frame ahead in its last saved state; more than that is suspicious.
    frames = [p.last_frame for p in peers]
    if max(frames) - min(frames) > LAST_FRAME_SLACK:
        bad.append("last frames differ: " + " ".join(f"p{p.index}={p.last_frame}" for p in peers))
    if len({tuple(p.win_teams) for p in peers}) > 1 or not peers[0].win_teams:
        bad.append("round winners differ or missing: " + " ".join(f"p{p.index}={p.win_teams}" for p in peers))
    return bad


def summary(m: Match) -> str:
    return (f"{m.tag}: {m.elapsed:.0f}s, frames={[p.last_frame for p in m.peers]}, "
            f"rounds={len(m.peers[0].win_teams)}, rolled back={[p.rolled_back_frames for p in m.peers]}")


def verdict(bad: List[str], notes: List[str]) -> Result:
    return (FAIL, bad + notes) if bad else (PASS, notes)


# --- A. Regression -----------------------------------------------------------

def a1_basic(s: Settings) -> Result:
    m = lib.run_match(s, "A1", [s.exe] * 4, DELAYS)
    return verdict(common(m), [summary(m)])


def a2_long(s: Settings) -> Result:
    m = lib.run_match(s, "A2", [s.exe] * 4, DELAYS, env={"MAXREBATTLE": "20"}, timeout=3600)
    bad = common(m)
    if len(m.peers[0].win_teams) != 20:
        bad.append(f"expected 20 rounds, got {len(m.peers[0].win_teams)}")
    for name, get in (("used MS", lambda p: p.used_ms), ("join frames", lambda p: p.joins)):
        if len({tuple(get(p)) for p in m.peers}) > 1:
            bad.append(f"{name} differ between peers")
    return verdict(bad, [summary(m)])


def _mixed(s: Settings, other: str, tag: str) -> Result:
    bad, notes = [], []
    for suffix, exes in (("old-first", [other, s.exe, other, s.exe]), ("new-first", [s.exe, other, s.exe, other])):
        m = lib.run_match(s, f"{tag}-{suffix}", exes, DELAYS)
        bad += [f"{suffix}: {b}" for b in common(m)]
        notes.append(summary(m))
    return verdict(bad, notes)


def a3_mixed_old(s: Settings) -> Result:
    if not s.old_exe:
        return SKIP, ["no --old-exe / --old-release"]
    return _mixed(s, s.old_exe, "A3")


def a4_mixed_older(s: Settings) -> Result:
    if not s.older_exe:
        return SKIP, ["no --older-exe / --older-release"]
    return _mixed(s, s.older_exe, "A4")


# --- B. Rollback -------------------------------------------------------------

def b1_skip_replay(s: Settings) -> Result:
    bad, notes, crossings = [], [], []
    for seed in (11, 22, 33, 44, 55, 66):
        m = lib.run_match(s, f"B1-{seed}", [s.exe] * 4, DELAYS, seed=seed, env={"GGPO_TEST_LOG": "1"})
        bad += [f"seed {seed}: {b}" for b in common(m)]
        for p in m.peers:
            for seek, frm, skip, replayed in lib.skip_crossings(p):
                if skip > seek:
                    crossings.append((seed, p.index, seek, frm, skip, replayed))
    notes.append(f"rollbacks over a skip after the seek frame: {len(crossings)}")
    for c in crossings:
        notes.append("  seed=%d p%d seek=%d from=%d skip=%d replayed=%s" % c)
        if not c[5]:
            bad.append("skip at frame %d not replayed (seed %d p%d)" % (c[4], c[0], c[1]))
    if not bad and not crossings:
        return INCONCLUSIVE, notes + ["no rollback crossed a skip; rerun or add seeds"]
    return verdict(bad, notes)


def b2_threaded(s: Settings) -> Result:
    m = lib.run_match(s, "B2", [s.exe] * 4, DELAYS_SPREAD, headless=False, env={"MAXREBATTLE": "3"})
    bad = common(m)
    if not any(p.rolled_back_frames for p in m.peers):
        bad.append("no rollbacks happened")
    return verdict(bad, [summary(m)])


def b3_not_threaded(s: Settings) -> Result:
    m = lib.run_match(s, "B3", [s.exe] * 4, DELAYS_SPREAD, headless=False, extra=THREADED_OFF)
    # This harness produces no rollbacks with threaded rendering off (old builds too), so only completion is checked.
    return verdict(common(m), [summary(m)])


# --- C. Disconnect and session lifecycle --------------------------------------

def c1_peer_drop(s: Settings) -> Result:
    if not lib.IS_WINDOWS:
        return SKIP, ["Windows only (samples Process.Responding)"]
    samples: List[str] = []

    def drop(m: Match):
        def body():
            if not lib.wait_for_log(m, 1, "LoadEndMsg Join", 120):
                return
            time.sleep(10)  # well into the battle
            lib.kill_peer(m, 4)
            samples.extend(lib.sample_responding(m, [1, 2, 3], 40))
        threading.Thread(target=body, daemon=True).start()

    m = lib.run_match(s, "C1", [s.exe] * 4, DELAYS_SPREAD, headless=False, on_start=drop, timeout=240, stall_sec=600)
    hung = [l for l in samples if "HUNG" in l]
    bad = []
    if not samples:
        bad.append("never sampled (battle did not start?)")
    if hung:
        bad.append(f"windows not responding in {len(hung)}/{len(samples)} samples, first: {hung[0]}")
    for p in m.peers[:3]:
        if "GdxsvBackendRollback.Close Done" not in p.log:
            bad.append(f"p{p.index} never closed the session")
        bad += [f"p{p.index}: {e.strip()}" for e in p.errors[:3]]
    return verdict(bad, [f"responding samples: {len(samples) - len(hung)}/{len(samples)}"])


def c2_start_timeout(s: Settings) -> Result:
    bad, notes = [], []
    for run in range(1, 4):  # the old crash depended on timing
        def drop(m: Match):
            def body():
                if lib.wait_for_log(m, 4, "Start UdpPingPong Thread", 60):
                    time.sleep(5)  # mid ping test: the others already have its RTT
                    lib.kill_peer(m, 4)
            threading.Thread(target=body, daemon=True).start()

        m = lib.run_match(s, f"C2-{run}", [s.exe] * 4, DELAYS, on_start=drop, timeout=60, stall_sec=600)
        for p in m.peers[:3]:
            if "StartNetwork timeout" not in p.log:
                bad.append(f"run {run} p{p.index}: no StartNetwork timeout")
            if "GdxsvBackendRollback.Close Done" not in p.log:
                bad.append(f"run {run} p{p.index}: never closed")
            bad += [f"run {run} p{p.index}: {e.strip()}" for e in p.errors[:3]]
        notes.append(f"run {run}: exit codes {[p.exit_code for p in m.peers[:3]]}")
    return verdict(bad, notes)


def _rebattle_cancel(s: Settings, tag: str, headless: bool) -> Tuple[List[str], List[str]]:
    env = {"RAND_MASK": "06F6", "MAXREBATTLE": "3", "GGPO_TEST_LOG": "1"}
    m = lib.run_match(s, tag, [s.exe] * 4, DELAYS_SPREAD, env=env, headless=headless)
    bad = common(m)
    cancel_frames = set()
    for p in m.peers:
        sc = lib.scenes(p)
        at_43 = [f for f, scene, _ in sc if scene == "4/3"]
        at_44 = [st for _, scene, st in sc if scene == "4/4"]
        if not at_43:
            return [], [f"{tag}: p{p.index} never reached the re-battle-cancel scene"]
        cancel_frames.add(at_43[0])
        if not at_44 or at_44[0] != 10:  # State::CloseWait
            bad.append(f"{tag} p{p.index}: not in CloseWait at the friend save scene ({at_44})")
    if len(cancel_frames) > 1:
        bad.append(f"{tag}: peers entered the cancel scene on different frames {sorted(cancel_frames)}")
    return bad, [summary(m) + f", cancel scene at frame {sorted(cancel_frames)}"]


def c3_rebattle_cancel(s: Settings) -> Result:
    bad, notes = [], []
    for tag, headless in (("C3-headless", True), ("C3-threaded", False)):
        b, n = _rebattle_cancel(s, tag, headless)
        bad += b
        notes += n
    if not bad and any("never reached" in n for n in notes):
        return INCONCLUSIVE, notes
    return verdict(bad, notes)


# --- D. Network robustness ---------------------------------------------------

def d1_fuzz(s: Settings) -> Result:
    sent = []

    def attack(m: Match):
        def body():
            if lib.wait_for_log(m, 1, "StartMsg Join", 120):
                sent.append(fuzz.run(45))
        threading.Thread(target=body, daemon=True).start()

    m = lib.run_match(s, "D1", [s.exe] * 4, DELAYS, on_start=attack)
    bad = common(m)
    if not sent:
        bad.append("fuzzer never started")
    return verdict(bad, [summary(m), f"malformed packets sent: {sent[0] if sent else 0}"])


def d2_relay(s: Settings) -> Result:
    m = lib.run_match(s, "D2", [s.exe] * 4, [16] * 4, env={"TEST_RELAY": "single", "GGPO_TEST_LOG": "1"})
    bad = common(m)
    if "Relay:1" not in m.peers[0].log:
        bad.append("p1 did not pick the relay")
    if "RBKTEST relay forward" not in m.peers[1].log:
        bad.append("p2 forwarded nothing")
    return verdict(bad, [summary(m)])


def d3_relay_loop(s: Settings) -> Result:
    env = {"TEST_RELAY": "loop", "GGPO_TEST_LOG": "1"}
    m = lib.run_match(s, "D3", [s.exe] * 4, [16] * 4, env=env, timeout=120, stall_sec=600)
    bad = []
    for p in m.peers[:2]:
        drops = [l for l in p.log.splitlines() if "RBKTEST relay drop-return-to-sender" in l]
        if not drops:
            bad.append(f"p{p.index}: relay loop not cut")
        elif len(drops) > 2:  # logged every 100 drops: more than ~200 means packets kept bouncing
            bad.append(f"p{p.index}: {len(drops)} drop log lines, packets kept bouncing")
    for p in m.peers:
        bad += [f"p{p.index}: {e.strip()}" for e in p.errors[:3]]
    return verdict(bad, ["session start is expected to time out: peers 0 and 1 cannot reach peer 3"])


def d4_relay_server(s: Settings) -> Result:
    if not s.relay_exe:
        return SKIP, ["no gdxsv binary (--relay-exe)"]
    port = 19879
    log_path = os.path.join(s.out, "D4_relay.log")
    penv = os.environ.copy()
    penv["GDXSV_RELAY_ADDR"] = f"127.0.0.1:{port}"
    with open(log_path, "w", encoding="utf-8") as log:
        relay = subprocess.Popen([s.relay_exe, "-pprof=0", "-relay_test_session=12345:1234", "relay"],
                                 env=penv, stdout=log, stderr=subprocess.STDOUT)
        try:
            env = {"TEST_RELAY": "server", "TEST_RELAY_SERVER": f"127.0.0.1:{port}", "TEST_RELAY_TOKEN": "1234",
                   "GGPO_TEST_LOG": "1"}
            m = lib.run_match(s, "D4", [s.exe] * 4, [16] * 4, env=env)
        finally:
            lib.kill(relay.pid)
            relay.wait()
    with open(log_path, encoding="utf-8", errors="replace") as f:
        relay_log = f.read()
    bad = common(m)
    if "Relay:2" not in m.peers[0].log:
        bad.append("p1 did not pick the relay server")
    for p in m.peers:
        if "RBKTEST relay server path" not in p.log:
            bad.append(f"p{p.index} did not send through the relay server")
    bound = relay_log.count("relay peer bound")
    if bound != len(m.peers):
        bad.append(f"relay bound {bound} peers, expected {len(m.peers)}")
    return verdict(bad, [summary(m), f"relay log: {log_path}"])


CASES: Dict[str, Tuple[str, Callable[[Settings], Result], int, bool]] = {
    # id: (title, function, rough minutes, long-running)
    "A1": ("basic 4-player match", a1_basic, 1, False),
    "A2": ("20-round match", a2_long, 21, True),
    "A3": ("mixed with --old-exe, both peer 0 sides", a3_mixed_old, 2, False),
    "A4": ("mixed with --older-exe, both peer 0 sides", a4_mixed_older, 2, False),
    "B1": ("timesync skip replayed across a rollback", b1_skip_replay, 7, False),
    "B2": ("threaded rendering, windowed, 3 rounds", b2_threaded, 4, False),
    "B3": ("threaded rendering off, windowed", b3_not_threaded, 2, False),
    "C1": ("peer drops mid-battle, windows stay responsive", c1_peer_drop, 3, False),
    "C2": ("GGPO session start times out, x3", c2_start_timeout, 3, False),
    "C3": ("re-battle cancel ends the match (4/3 -> 4/4)", c3_rebattle_cancel, 3, False),
    "D1": ("malformed packet fuzzing during a match", d1_fuzz, 1, False),
    "D2": ("relay path in use", d2_relay, 1, False),
    "D3": ("relay loop is cut", d3_relay_loop, 2, False),
    "D4": ("relay server path, peers answer through it", d4_relay_server, 1, False),
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=os.path.join(lib.REPO, "cmake-build-relwithdebinfo", lib.EXE_NAME),
                    help="build under test")
    ap.add_argument("--rom", default=os.environ.get("ROM"), help="Disc 2 GDI (or set ROM)")
    ap.add_argument("--out", default=os.path.join(lib.HERE, "out"), help="work/output directory")
    ap.add_argument("--state-dir", default=os.path.join(lib.REPO, "work", "state"),
                    help="where <rom>_99.state lives; downloaded if missing")
    ap.add_argument("--old-exe", help="previous release executable, for A3")
    ap.add_argument("--old-release", help="previous release tag to download instead, e.g. gdxsv-1.9.2")
    ap.add_argument("--older-exe", help="an older release executable, for A4")
    ap.add_argument("--older-release", help="older release tag to download instead")
    ap.add_argument("--relay-exe", default=os.path.join(lib.REPO, "..", "gdxsv", "bin",
                                                         "gdxsv.exe" if os.name == "nt" else "gdxsv"),
                    help="gdxsv binary serving the relay, for D4")
    ap.add_argument("--cases", help="comma-separated case ids (default: all but long ones)")
    ap.add_argument("--long", action="store_true", help="include long-running cases")
    ap.add_argument("--list", action="store_true", help="list cases and exit")
    a = ap.parse_args()

    if a.list:
        for cid, (title, _, minutes, long_) in CASES.items():
            print(f"{cid}  ~{minutes:>2}min  {title}{'  (long)' if long_ else ''}")
        return 0
    if not a.rom or not os.path.isfile(a.rom):
        ap.error("--rom must point at the Disc 2 GDI")
    if not os.path.isfile(a.exe):
        ap.error(f"no build at {a.exe}")

    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    s = Settings(exe=os.path.abspath(a.exe), rom=os.path.abspath(a.rom), out=out,
                 state_dir=os.path.abspath(a.state_dir), old_exe=a.old_exe, older_exe=a.older_exe,
                 relay_exe=os.path.abspath(a.relay_exe) if os.path.isfile(a.relay_exe) else None)
    if a.old_release:
        s.old_exe = lib.fetch_release(a.old_release, out)
    if a.older_release:
        s.older_exe = lib.fetch_release(a.older_release, out)
    lib.ensure_state(s.state_dir, s.rom)

    ids = a.cases.split(",") if a.cases else [c for c, v in CASES.items() if a.long or not v[3]]
    results = {}
    for cid in ids:
        title, fn, minutes, _ = CASES[cid]
        print(f"=== {cid} {title} (~{minutes} min)", flush=True)
        try:
            status, lines = fn(s)
        except Exception as e:  # keep going; one broken case must not hide the rest
            status, lines = FAIL, [f"harness error: {e!r}"]
        results[cid] = {"title": title, "status": status, "details": lines}
        print(f"    {status}", flush=True)
        for line in lines:
            print(f"      {line}", flush=True)

    print("\n=== summary")
    for cid, r in results.items():
        print(f"{r['status']:<13}{cid}  {r['title']}")
    with open(os.path.join(out, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2, ensure_ascii=False)
    return 1 if any(r["status"] == FAIL for r in results.values()) else 0


if __name__ == "__main__":
    sys.exit(main())
