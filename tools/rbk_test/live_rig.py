"""Live spectator rig: a 4-player local rollback match streamed through a local
lbs to live spectators, checked against the peers' own replays.

    python tools/rbk_test/live_rig.py --rom D:\\rom\\gdx-disc2\\gdx-disc2.gdi --header <dc2 replay of a local test>

The peers run as in run_suite.py (gdxsv:rbk_test, random input) with
TEST_SPECTATOR_LBS pointing their uplink at a local `gdxsv lbs` started with
-spectator_test_session, so lbs assembles the live recording exactly as in
production. Spectators join with gdxsv:spectate. Every instance runs
live_det.lua, which logs the battle state once per sim step.

Checks:
- lbs's recording (<header>.live.pb) is exactly one peer's replay: lbs follows the first uploader.
- each spectator's per-step battle state against that replay played offline.

--header must be a Disc 2 replay of a local test with the same VITAL, so lbs
hands spectators the same rule the peers used. Make one with --make-header.
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rbk_lib as lib  # noqa: E402

LUA = os.path.join(lib.HERE, "live_det.lua")
# LuaFileName is resolved against each instance's directory, out/<tag>/<instance>.
LUA_REL = "../../live_det.lua"
LBS_PORT = 9876


def proto_module(out, gdxsv_repo):
    """Python bindings for gdxsv.proto, generated with protoc from the gdxsv repo."""
    d = os.path.join(out, "pb")
    if not os.path.isfile(os.path.join(d, "gdxsv_pb2.py")):
        os.makedirs(d, exist_ok=True)
        proto_dir = os.path.join(gdxsv_repo, "gdxsv", "proto")
        subprocess.run(["protoc", "-I", proto_dir, f"--python_out={d}", os.path.join(proto_dir, "gdxsv.proto")],
                       check=True)
    sys.path.insert(0, d)
    # Older protoc output only loads with the pure Python implementation.
    os.environ.setdefault("PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION", "python")


def load_log(path):
    import gdxsv_pb2 as pb
    f = pb.BattleLogFile()
    with open(path, "rb") as fp:
        f.ParseFromString(fp.read())
    return f


def read_det(path):
    steps = {}
    if not os.path.isfile(path):
        return steps
    with open(path) as fp:
        for line in fp:
            if not line.endswith("\n"):
                break  # cut off when the instance was killed
            e = line.split()
            if len(e) == 9 and e[0] == "S":
                # e[4] is the second RNG, which also advances per vblank, so it is not compared.
                steps[(int(e[1]), int(e[2]))] = " ".join([e[3]] + e[5:])
    return steps


def start_lbs(gdxsv_exe, header, log_path):
    env = os.environ.copy()
    env.update({"GDXSV_LOBBY_ADDR": f":{LBS_PORT}", "GDXSV_LOBBY_PUBLIC_ADDR": f"127.0.0.1:{LBS_PORT}",
                "GDXSV_LOBBY_HTTP_ADDR": "", "GDXSV_BATTLE_ADDR": ":3334", "GDXSV_RELAY_ADDR": ""})
    cwd = os.path.dirname(os.path.dirname(os.path.abspath(gdxsv_exe)))
    out = open(log_path, "w")
    return subprocess.Popen([gdxsv_exe, "-v=3", f"-spectator_test_session={header}", "lbs"], cwd=cwd, env=env,
                            stdout=out, stderr=subprocess.STDOUT)


def start_spectator(s, base, index, pov, extra):
    d = os.path.join(base, f"s{index}")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "data"), exist_ok=True)
    shutil.copy(s.exe, os.path.join(d, lib.EXE_NAME))
    for f in glob.glob(os.path.join(s.state_dir, "*.state")):
        shutil.copy(f, os.path.join(d, "data"))
    args = [os.path.join(d, lib.EXE_NAME),
            "--config", "config:aica.Volume=0",
            "--config", "gdxsv:ReplayFourScreen=no",
            "--config", "gdxsv:server=127.0.0.1",
            "--config", "gdxsv:spectate=0123456",
            "--config", f"gdxsv:ReplayPOV={pov}",
            "--config", f"config:LuaFileName={LUA_REL}",
            "--config", "log:Verbosity=3",
            "--config", "log:LogToFile=1",
            "--config", f"window:left={40 + (index % 2) * 660}", "--config", f"window:top={40 + (index // 2) * 520}",
            "--config", "window:width=640", "--config", "window:height=480"] + extra + [s.rom]
    return d, subprocess.Popen(args, cwd=d, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def play_replay(s, base, name, replay, pov, timeout=900):
    """Play a replay headless with the probe; its det.txt is the reference state."""
    d = os.path.join(base, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "data"), exist_ok=True)
    shutil.copy(s.exe, os.path.join(d, lib.EXE_NAME))
    for f in glob.glob(os.path.join(s.state_dir, "*.state")):
        shutil.copy(f, os.path.join(d, "data"))
    args = [os.path.join(d, lib.EXE_NAME),
            "--config", "config:aica.Volume=0",
            "--config", "gdxsv:ReplayFourScreen=no",
            "--config", "gdxsv:headless=yes",
            "--config", f"gdxsv:replay={os.path.abspath(replay)}",
            "--config", f"gdxsv:ReplayPOV={pov}",
            "--config", f"config:LuaFileName={LUA_REL}",
            "--config", "log:Verbosity=3",
            "--config", "log:LogToFile=1", s.rom]
    p = subprocess.Popen(args, cwd=d, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        lib.kill(p.pid)
    try:
        os.remove(os.path.join(d, lib.EXE_NAME))
    except OSError:
        pass
    return d


def lbs_warnings(log_path):
    lines = []
    with open(log_path, encoding="utf-8", errors="replace") as fp:
        for line in fp:
            if "legacy spectator" in line:
                lines.append(line.rstrip())
    return lines


def compare_inputs(live, peers):
    """lbs records one peer, the first whose upload arrives. Its recording must be that peer's replay
    (up to the replay's end; the replay may hold a few more inputs from after the uplink stopped).
    Returns the matching peer's name, or None."""
    print(f"lbs recording: {len(live.inputs)} inputs, starts {list(live.start_msg_indexes)}")
    match = None
    for name, rep in peers:
        same_starts = list(rep.start_msg_indexes) == list(live.start_msg_indexes)
        same_seeds = list(rep.start_msg_randoms) == list(live.start_msg_randoms)
        n = len(live.inputs)
        same_inputs = n <= len(rep.inputs) and list(rep.inputs[:n]) == list(live.inputs)
        exact = same_starts and same_seeds and same_inputs
        print(f"  {name}: starts {list(rep.start_msg_indexes)}, {len(rep.inputs)} inputs"
              + (" <- recorded" if exact and match is None else "" if exact else
                 f" (starts {'same' if same_starts else 'differ'}, inputs {'same' if same_inputs else 'differ'})"))
        if exact and match is None:
            match = name
    if match is None:
        print("  no peer's replay matches the lbs recording")
    return match


def compare_det(ref, other, name):
    common = sorted(set(ref) & set(other))
    bad = [k for k in common if ref[k] != other[k]]
    print(f"  {name}: {len(other)} steps, {len(common)} shared with the replay, {len(bad)} differ"
          + (f", first {bad[0]}" if bad else ""))
    for k in bad[:2]:
        print(f"     rep  {k}: {ref[k]}")
        print(f"     {name:4} {k}: {other[k]}")
    return not bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--exe", default=os.path.join(lib.REPO, "cmake-build-relwithdebinfo", lib.EXE_NAME))
    ap.add_argument("--gdxsv", default=os.path.join(lib.REPO, "..", "gdxsv", "bin", "gdxsv.exe"))
    ap.add_argument("--state-dir", default=os.path.join(lib.REPO, "work", "state"))
    ap.add_argument("--out", default=os.path.join(lib.HERE, "out"))
    ap.add_argument("--tag", default="live")
    ap.add_argument("--header", help="Disc 2 replay of a local test (users/rule/patches for lbs)")
    ap.add_argument("--make-header", action="store_true", help="run a match without spectators and save its replay")
    ap.add_argument("--seed", type=int, default=55)
    ap.add_argument("--vital", default="60")
    ap.add_argument("--rounds", default="3", help="MAXREBATTLE")
    ap.add_argument("--delays", default="16,16,16,16", help="GGPO_NETWORK_DELAY per peer, ms")
    ap.add_argument("--input-delay", default="", help="TEST_GGPO_DELAY: fixed input delay frames, so latency means prediction")
    ap.add_argument("--spectators", default="1:0", help="comma separated pov:join_delay_sec")
    ap.add_argument("--spectator-extra", default="", help="extra flycast args for spectators, space separated")
    ap.add_argument("--timeout", type=int, default=1500)
    a = ap.parse_args()

    s = lib.Settings(exe=a.exe, rom=a.rom, out=a.out, state_dir=a.state_dir)
    proto_module(a.out, os.path.dirname(os.path.dirname(os.path.abspath(a.gdxsv))))
    lib.ensure_state(s.state_dir, s.rom)
    delays = [int(x) for x in a.delays.split(",")]
    env = {"VITAL": a.vital, "MAXREBATTLE": a.rounds, "RBK_SAVE_REPLAY": "1"}
    if a.input_delay:
        env["TEST_GGPO_DELAY"] = a.input_delay
    shutil.copy(LUA, os.path.join(a.out, "live_det.lua"))
    extra = ["--config", f"config:LuaFileName={LUA_REL}"]
    base = os.path.join(a.out, a.tag)

    if a.make_header:
        m = lib.run_match(s, a.tag, [s.exe] * lib.N, delays, seed=a.seed, env=env, extra=extra, timeout=a.timeout)
        print("elapsed", round(m.elapsed), "timed_out", m.timed_out, "stalled", m.stalled)
        print("header:", os.path.join(base, "p1", "data", "replays", "0123456.pb"))
        return 0

    header = os.path.abspath(os.path.join(a.out, f"{a.tag}_header.pb"))
    shutil.copy(a.header, header)
    for f in glob.glob(header + ".live.pb"):
        os.remove(f)
    lbs = start_lbs(os.path.abspath(a.gdxsv), header, os.path.join(a.out, f"{a.tag}_lbs.log"))
    time.sleep(2)

    env["TEST_SPECTATOR_LBS"] = f"127.0.0.1:{LBS_PORT}"
    specs = []
    plan = [tuple(int(x) for x in p.split(":")) for p in a.spectators.split(",") if p]
    spec_extra = a.spectator_extra.split() if a.spectator_extra else []

    def on_start(_match):
        def launch():
            t0 = time.time()
            for i, (pov, delay) in enumerate(sorted(plan, key=lambda p: p[1])):
                time.sleep(max(0, delay - (time.time() - t0)))
                specs.append(start_spectator(s, base, i + 1, pov, spec_extra))
        threading.Thread(target=launch, daemon=True).start()

    try:
        m = lib.run_match(s, a.tag, [s.exe] * lib.N, delays, seed=a.seed, env=env, extra=extra,
                          timeout=a.timeout, on_start=on_start)
        print("match elapsed", round(m.elapsed), "timed_out", m.timed_out, "stalled", m.stalled)
        for p in m.peers:
            print(f"  p{p.index} finished={p.finished} win={p.win_teams} errors={p.errors[:3]}")
        # Let the spectators play out the buffered tail; lbs never closes a test session.
        time.sleep(15)
    finally:
        for _, p in specs:
            lib.kill(p.pid)
        lbs.terminate()

    uplinks = []
    for p in range(1, lib.N + 1):
        with open(os.path.join(base, f"p{p}", "flycast.log"), encoding="utf-8", errors="replace") as fp:
            if any("Start GdxsvSpectatorUplink Thread" in line for line in fp):
                uplinks.append(f"p{p}")
    print(f"uplink started on: {uplinks}")

    ok = True
    live_path = header + ".live.pb"
    peers = []
    for p in range(1, lib.N + 1):
        path = os.path.join(base, f"p{p}", "data", "replays", "0123456.pb")
        if os.path.isfile(path):
            peers.append((f"p{p}", load_log(path)))
    recorded = None
    if os.path.isfile(live_path) and peers:
        recorded = compare_inputs(load_log(live_path), peers)
    else:
        print("missing lbs recording or peer replays")
    ok &= recorded is not None

    warns = lbs_warnings(os.path.join(a.out, f"{a.tag}_lbs.log"))
    print(f"lbs legacy warnings: {len(warns)}")
    for w in warns[:10]:
        print("  " + w[w.find("legacy"):][:220])

    # Reference: the recorded peer's replay played back offline. The peers' own det.txt is
    # only a rough check - a rollback need not rewrite every step it reran.
    replay = os.path.join(base, recorded or "p1", "data", "replays", "0123456.pb")
    ref = read_det(os.path.join(play_replay(s, base, "r1", replay, 1), "det.txt")) if os.path.isfile(replay) else {}
    print(f"battle state: replay {len(ref)} steps")
    for p in range(1, lib.N + 1):
        compare_det(ref, read_det(os.path.join(base, f"p{p}", "det.txt")), f"p{p}")
    for d, _ in specs:
        ok &= compare_det(ref, read_det(os.path.join(d, "det.txt")), os.path.basename(d))
        with open(os.path.join(d, "flycast.log"), encoding="utf-8", errors="replace") as fp:
            held = sum(1 for line in fp if "StartMsg held" in line)
        print(f"  {os.path.basename(d)}: StartMsg held for a round start {held} time(s)")
    print("RESULT", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
