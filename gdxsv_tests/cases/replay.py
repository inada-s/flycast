"""Replays recorded by past releases still play (corpus/replays.json)."""

import concurrent.futures
import os
import re
import time

from lib import assets, emu, image, recheck, replayfile
from lib.case import Skip, case, result, write_lua

STARTS_RE = re.compile(r"start_msg_indexes = ([\d ]*)")
ROUND_RE = re.compile(r"StartMsg key_msg_count (\d+)")
STOP_RE = re.compile(r"replay stopped at key_msg_count_ (\d+) of (\d+)")

# Winner byte and draw flag (core/gdxsv/gdxsv_round_counters.h, gdxsv_backend_rollback.cpp)
OUTCOME = {"dc1": {"winner": 0x0c3364b6, "draw": 0x0c3364c9},
           "dc2": {"winner": 0x0c3d1948, "draw": 0x0c3d1950}}


def play(ctx, entry: dict, pb: str = None, extra: list = (), name: str = None) -> dict:
    """Play one replay headless to its end. It played when the process exited 0 by itself, every
    round in the file started, and playback stopped after the last round's start. Replays that
    record each round's winner (round_data) must also end each round with that winner."""
    rom = ctx.rom(entry["disk"])
    pb = pb or assets.replay(entry)
    p = ctx.run(name or "R_" + entry["id"], rom, [f"gdxsv:replay={pb}", "gdxsv:ReplayPOV=1", "gdxsv:ReplaySeekStates=no", *extra],
                timeout=900, script="outcome.lua", params=OUTCOME[entry["disk"]])
    log = p.log
    m = STARTS_RE.search(log)
    starts = [int(x) for x in m.group(1).split()] if m else []
    round_starts = [int(x) for x in ROUND_RE.findall(log)]
    rounds = len(round_starts)
    if not starts:  # files that list no round starts (log_file_version 20230426): the rounds that ran
        starts = round_starts
    stops = STOP_RE.findall(log)
    stop = int(stops[-1][0]) if stops else None
    inputs = int(stops[-1][1]) if stops else None
    played = (p.clean and bool(starts) and rounds == len(starts) and stop is not None and stop > starts[-1])
    # Recorded in the file (round_data), else as a past build played it (corpus "winners").
    want = replayfile.win_teams(pb) or entry.get("winners", [])
    got = [int(p.e2e[k]) for k in sorted((k for k in p.e2e if k.startswith("round")), key=lambda k: int(k[5:]))]
    outcome_ok = not want or got == want
    return {"id": entry["id"], "disk": entry["disk"], "version": entry["log_file_version"],
            "ok": played and outcome_ok, "played": played,
            "inputs": inputs, "stopped_at": stop, "rounds_in_file": len(starts), "rounds_started": rounds,
            "winners_recorded": want, "winners_seen": got, **p.facts()}


def play_all(ctx, entries: list) -> list:
    if recheck.ACTIVE:
        entries = [e for e in entries if os.path.isdir(os.path.join(ctx.out, "R_" + e["id"]))]
    else:
        entries = [e for e in entries if assets.rom(e["disk"])]
    jobs = int(os.environ.get("GDXSV_REPLAY_JOBS", "4"))
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        return list(pool.map(lambda e: play(ctx, e), entries))


@case("R1", "replays of every format in the corpus play to the end, with the recorded winners", tier="full")
def replay_compat(ctx):
    entries = assets.corpus()
    checks = play_all(ctx, entries)
    skipped = sorted({e["disk"] for e in entries} - {c["disk"] for c in checks})
    return result(checks, skipped_disks=skipped)


@case("R2", "the newest Disc 2 replay in the corpus plays to the end, with the recorded winners", tier="smoke")
def replay_newest(ctx):
    return result([play(ctx, newest_dc2())])


def newest_dc2() -> dict:
    return newest("dc2")


@case("R2c", "R2 on a copy with the inputs cut in the third round: the check fails it", control_of="R2")
def replay_newest_control(ctx):
    entry = newest_dc2()
    pb = assets.replay(entry)
    starts = replayfile.start_indexes(pb)
    os.makedirs(ctx.out, exist_ok=True)
    cut = os.path.join(ctx.out, entry["id"] + "_cut.pb")
    replayfile.truncate_inputs(pb, cut, (starts[2] + starts[3]) // 2)
    c = play(ctx, entry, pb=cut)
    return result([{"what": "the cut replay fails the check", "ok": not c["ok"]}], detail=c)


# Bytes that follow the replay's point of view: the game stores the player side the replay
# backend answers (lbsAskPlayerSide = ReplayPOV). Found by RAM diffs of POV 1-4 runs of two
# corpus replays; stable from the battle on.
POV_SIDE = 0x0c3d19d9  # POV - 1
POV_ID = 0x0c391d92  # POV
POV_FRAME = 4000  # vblanks after boot: in the first round


def pov_run(ctx, name: str, entry: dict, pov) -> dict:
    """Read the POV bytes at POV_FRAME of a replay played with gdxsv:ReplayPOV=pov (None: unset)."""
    cfg = [f"gdxsv:replay={assets.replay(entry)}", "gdxsv:ReplaySeekStates=no"]
    if pov is not None:
        cfg.append(f"gdxsv:ReplayPOV={pov}")
    p = ctx.probe(name, ctx.rom(entry["disk"]), "memread.lua",
                  {"frame": POV_FRAME, "reads": [[POV_SIDE, 1], [POV_ID, 1]]},
                  frames=POV_FRAME + 20, config=cfg, loadstate=False, until_done=True, timeout=300)
    return {"pov": pov, "side": p.e2e.get(f"0x{POV_SIDE:08x}:1"), "id": p.e2e.get(f"0x{POV_ID:08x}:1"),
            "clean": p.clean, **p.facts()}


def pov_checks(runs: list, want: list) -> list:
    return [{"what": f"ReplayPOV {w}: the game plays as player {w}",
             "ok": r["clean"] and r["side"] == str(w - 1) and r["id"] == str(w), **r} for r, w in zip(runs, want)]


@case("V1", "replay point of view: the game plays the replay as the chosen player (ReplayPOV 2 and 4)")
def replay_pov(ctx):
    e = newest_dc2()
    with concurrent.futures.ThreadPoolExecutor(2) as ex:
        runs = list(ex.map(lambda pov: pov_run(ctx, f"V1_pov{pov}", e, pov), (2, 4)))
    return result(pov_checks(runs, [2, 4]))


@case("V1c", "V1's check for POV 2 on a replay played without ReplayPOV: it fails", control_of="V1")
def replay_pov_control(ctx):
    r = pov_run(ctx, "V1c", newest_dc2(), None)
    c = pov_checks([r], [2])[0]
    return result([{"what": "probe ran", "ok": r["clean"]},
                   {"what": "POV 2 check fails (player 1 by default)", "ok": not c["ok"] and r["id"] == "1"}],
                  detail=c)


# Cost 300->295 (core/gdxsv/gdxsv_backend_replay.cpp WriteLegacyCostPatch). Files before
# log_file_version 20261023 were recorded while the emulator forced it; newer files get it only
# from their game patches, as the lobby sends it (gdxsv m_patch).
COST = {"dc1": [0x0c1b0fd0], "dc2": [0x0c21bfec, 0x0c21bff4, 0x0c21c034]}
COST_VERSION = 20261023


def newest(disk: str) -> dict:
    return max((e for e in assets.corpus() if e["disk"] == disk), key=lambda e: e["date"])


def cost_run(ctx, name: str, disk: str, version: int = None, patch: bool = False) -> dict:
    """Read the cost values at POV_FRAME of the newest replay of `disk`; with `version`, a copy
    of it with that log_file_version, plus the cost game patch when `patch`."""
    entry = newest(disk)
    pb = assets.replay(entry)
    if version is not None:
        os.makedirs(ctx.out, exist_ok=True)
        dst = os.path.join(ctx.out, f"{name}.pb")
        if not recheck.ACTIVE:
            codes = [(16, a, 300, 295) for a in COST[disk]]
            replayfile.rewrite_header(pb, dst, version, [replayfile.game_patch(disk, "cost295", codes)] if patch else [])
        pb = dst
    p = ctx.probe(name, ctx.rom(disk), "memread.lua",
                  {"frame": POV_FRAME, "reads": [[a, 2] for a in COST[disk]]},
                  frames=POV_FRAME + 20, config=[f"gdxsv:replay={pb}", "gdxsv:ReplaySeekStates=no"],
                  loadstate=False, until_done=True, timeout=300)
    return {"disk": disk, "version": version or entry["log_file_version"], "patch": patch,
            "cost": [p.e2e.get(f"0x{a:08x}:2") for a in COST[disk]], "clean": p.clean, **p.facts()}


def cost_check(what: str, r: dict, want: int) -> dict:
    return {"what": what, "ok": r["clean"] and r["cost"] == [str(want)] * len(r["cost"]), **r}


@case("R5", "cost 300->295 in replays: older files keep it, newer ones take it from their game patch (Disc 1, Disc 2)")
def replay_cost(ctx):
    runs = [(f"{d}: recorded file (version before {COST_VERSION}): 295", (f"R5_{d}_old", d), 295) for d in COST]
    runs += [(f"dc2: version {COST_VERSION} with the cost295 game patch: 295", ("R5_dc2_patch", "dc2", COST_VERSION, True), 295)]
    with concurrent.futures.ThreadPoolExecutor(3) as ex:
        got = list(ex.map(lambda r: cost_run(ctx, *r[1]), runs))
    return result([cost_check(what, g, want) for (what, _, want), g in zip(runs, got)])


@case("R5c", f"R5's check on a copy with version {COST_VERSION} and no cost game patch: it fails (300)", control_of="R5")
def replay_cost_control(ctx):
    r = cost_run(ctx, "R5c", "dc2", COST_VERSION)
    c = cost_check("295 check", r, 295)
    return result([{"what": "probe ran", "ok": r["clean"]},
                   {"what": "295 check fails, the stock cost 300", "ok": not c["ok"] and r["cost"] == ["300"] * 3}],
                  detail=c)


@case("V2", "replay round jump at start: gdxsv:replay_target_round=4 plays the last two rounds of five")
def replay_round_jump(ctx):
    r, log = jump_run(ctx, "V2", 4)
    return result(jump_checks(r, log, 4), **r)


@case("V2c", "V2's checks on the replay played from round 1: they fail", control_of="V2")
def replay_round_jump_control(ctx):
    r, log = jump_run(ctx, "V2c", None)
    c = jump_checks(r, log, 4)
    return result([{"what": "replay played to its end", "ok": c[0]["ok"] and c[3]["ok"]},
                   {"what": "jump checks fail", "ok": not c[1]["ok"] and not c[2]["ok"]}], detail=c)


def jump_run(ctx, name: str, round):
    e = newest_dc2()
    extra = [f"gdxsv:replay_target_round={round}"] if round else []
    r = play(ctx, e, extra=extra, name=name)
    with open(os.path.join(ctx.out, name, "flycast.log"), encoding="utf-8", errors="replace") as f:
        return r, f.read()


def jump_checks(r: dict, log: str, round: int) -> list:
    starts = [int(x) for x in STARTS_RE.search(log).group(1).split()] if STARTS_RE.search(log) else []
    played = [int(x) for x in ROUND_RE.findall(log)]
    # The game's first round starts at frame 0, then the jump lands on round `round`.
    return [
        {"what": "process ended by itself, no errors", "ok": r["rc"] == 0 and not r["timed_out"] and not r["errors"]},
        {"what": f"one jump to round {round}", "ok": log.count(f"ctrl_change_round_:{round} ") == 1},
        {"what": "rounds 2 to the jump skipped, the later ones played",
         "ok": bool(starts) and played == [0] + starts[round:]
         and len(r["winners_seen"]) == len(starts) - round + 1},
        {"what": "playback stopped at the last input", "ok": r["inputs"] is not None and r["stopped_at"] == r["inputs"]},
    ]


FRAME_JUMP_RE = re.compile(r"JumpToKeyMsg \d+->(\d+) target=(\d+)")


@case("V3", "replay seek at start: gdxsv:replay_target_frame=4000 lands on input 4000, then plays to the end")
def replay_frame_jump(ctx):
    r = play(ctx, newest_dc2(), extra=["gdxsv:replay_target_frame=4000"], name="V3")
    return result(frame_jump_checks(r, ctx.out, "V3", 4000), **r)


@case("V3c", "V3's checks on the replay played without a target frame: the seek check fails", control_of="V3")
def replay_frame_jump_control(ctx):
    r = play(ctx, newest_dc2(), name="V3c")
    c = frame_jump_checks(r, ctx.out, "V3c", 4000)
    return result([{"what": "replay played", "ok": c[1]["ok"]}, {"what": "seek check fails", "ok": not c[0]["ok"]}],
                  detail=c)


def frame_jump_checks(r: dict, out: str, name: str, frame: int) -> list:
    with open(os.path.join(out, name, "flycast.log"), encoding="utf-8", errors="replace") as f:
        jumps = FRAME_JUMP_RE.findall(f.read())
    return [{"what": f"one seek, landed on input {frame}", "ok": jumps == [(str(frame), str(frame))], "jumps": jumps},
            {"what": "every round played to the end with the recorded winners", "ok": r["ok"]}]


SPEED_RE = re.compile(r"Replay speed: (\d+%)")
FAST_FORWARD_TARGETS = [("round-jump", "gdxsv:replay_target_round=4", "ctrl_change_round_:4"),
                        ("frame-seek", "gdxsv:replay_target_frame=4000", "target=4000")]


def fast_forward_run(ctx, name: str, target: str, ready: str, press: bool) -> dict:
    """Use the mapped keyboard key after a round jump or frame seek; save each observed speed
    sequence so --recheck can apply the same checks without sending input."""
    saved = os.path.join(ctx.out, name + ".json")
    if recheck.ACTIVE:
        return recheck.load(saved, {})
    if not (emu.IS_WINDOWS or emu.IS_MAC):
        raise Skip("mapped keyboard input requires Windows or macOS")
    rom = ctx.rom("dc2")
    cfg = [f"gdxsv:replay={assets.replay(newest_dc2())}", target,
           "gdxsv:ReplayFourScreen=no", "gdxsv:ReplaySeekStates=no", "gdxsv:ReplaySkipMsSelection=yes",
           "config:rend.Resolution=480", "config:rend.vsync=no", "log:LogToFile=yes", "log:Verbosity=3"]
    with image.SCREEN_LOCK:
        inst = emu.launch(ctx.exe, rom, os.path.join(ctx.out, name), config=cfg,
                          state_dir=assets.state_dir(rom), headless=False)
        r = {}

        def speeds(expected):
            wait_for(lambda: inst.poll() is not None or SPEED_RE.findall(inst.log) == expected, 3)
            return SPEED_RE.findall(inst.log)

        try:
            r["ready"] = wait_for(lambda: inst.poll() is not None or ready in inst.log, 120) and inst.poll() is None
            if r["ready"]:
                time.sleep(3)  # allow Loading and the control bar to disappear
                pid = inst.proc.pid
                if press:
                    emu.press_key(0x20, pid, hold=0.8)
                r["first"] = speeds(["300%"])
                if press:
                    time.sleep(1)
                    r["released"] = SPEED_RE.findall(inst.log)
                    emu.press_key(0x20, pid)
                    r["second"] = speeds(["300%", "100%"])
                    emu.press_key(0x09, pid)  # Tab: replay pause menu
                    r["menu_open"] = wait_for(lambda: "Save Menu Opened frame" in inst.log, 3)
                    emu.press_key(0x20, pid)
                    time.sleep(0.5)
                    emu.press_key(0x09, pid)
                    time.sleep(0.5)
                    r["menu"] = SPEED_RE.findall(inst.log)
                    emu.press_key(0x20, pid)
                    r["after_menu"] = speeds(["300%", "100%", "300%"])
                    emu.press_key(0x20, pid)
                    r["final"] = speeds(["300%", "100%", "300%", "100%"])
            r.update(errors=inst.errors, running=inst.poll() is None)
        except NotImplementedError as e:
            raise Skip(str(e))
        finally:
            inst.kill()
            inst.proc.wait(10)
            inst.remove_exe()
        recheck.save(saved, r)
    return r


def fast_forward_checks(r: dict) -> list:
    return [{"what": "replay reached its target and stayed running without errors",
             "ok": r.get("ready") and r.get("running") and not r.get("errors")},
            {"what": "held Fast-forward selects 300% once", "ok": r.get("first") == ["300%"]},
            {"what": "release does not toggle; the next press restores 100%",
             "ok": r.get("released") == ["300%"] and r.get("second") == ["300%", "100%"]},
            {"what": "menu press neither changes speed nor leaks after closing",
             "ok": r.get("menu_open") and r.get("menu") == ["300%", "100%"]},
            {"what": "fresh presses after the menu toggle 300% then 100%",
             "ok": r.get("after_menu") == ["300%", "100%", "300%"]
             and r.get("final") == ["300%", "100%", "300%", "100%"]}]


@case("V10", "mapped Fast-forward toggles 100%/300% after a round jump and frame seek; pause menu ignores it")
def replay_fast_forward(ctx):
    runs = {name: fast_forward_run(ctx, name, target, ready, True) for name, target, ready in FAST_FORWARD_TARGETS}
    return result([{**c, "target": name} for name, r in runs.items() for c in fast_forward_checks(r)], runs=runs)


@case("V10c", "V10 without the first key press: the 300% check fails", control_of="V10")
def replay_fast_forward_control(ctx):
    runs = {name: fast_forward_run(ctx, name, target, ready, False) for name, target, ready in FAST_FORWARD_TARGETS}
    checks = []
    for name, r in runs.items():
        c = fast_forward_checks(r)
        checks += [{**c[0], "target": name},
                   {"what": "missing press fails the 300% check", "target": name,
                    "ok": not c[1]["ok"] and r.get("first") == []}]
    return result(checks, runs=runs)


SEEK_RE = re.compile(r"JumpToKeyMsg \d+->(\d+) target=\d+ load=-?\d+ ran=(\d+) frames in \d+\[ms\] \(([\d.]+)\[ms/fr\]\)")
SEEK_FRAME = 6000  # in round 1 (target 8000 landed on round 2's start, 6376)
# Measured on windows-mini (newest Disc 2 replay, 6400 frames run): hack on 2.0-2.1 ms/fr, off 3.1-3.3.
SEEK_SPEEDUP = 0.85


def seek_run(ctx, name: str, hack: bool) -> dict:
    r = play(ctx, newest_dc2(), name=name,
             extra=[f"gdxsv:replay_target_frame={SEEK_FRAME}", f"gdxsv:SkipRenderingHack={'yes' if hack else 'no'}"])
    with open(os.path.join(ctx.out, name, "flycast.log"), encoding="utf-8", errors="replace") as f:
        seeks = SEEK_RE.findall(f.read())
    r["seek"] = [{"landed": int(k), "ran": int(n), "ms_per_frame": float(ms)} for k, n, ms in seeks]
    return r


def seek_checks(fast: dict, slow: dict) -> list:
    one = all(len(r["seek"]) == 1 and r["seek"][0]["landed"] == SEEK_FRAME for r in (fast, slow))
    return [
        {"what": f"each run: one seek, landed on input {SEEK_FRAME}", "ok": one},
        {"what": "each run: every round played to the end with the recorded winners", "ok": fast["ok"] and slow["ok"]},
        {"what": f"seek with the hack under {SEEK_SPEEDUP} x the ms per frame without it",
         "ok": one and fast["seek"][0]["ms_per_frame"] < SEEK_SPEEDUP * slow["seek"][0]["ms_per_frame"],
         "ms_per_frame": [r["seek"][0]["ms_per_frame"] if r["seek"] else None for r in (fast, slow)]},
    ]


@case("K1", "fast seek: SkipRenderingHack seeks faster per frame, same outcome as without it")
def replay_fast_seek(ctx):
    fast, slow = seek_run(ctx, "K1_on", True), seek_run(ctx, "K1_off", False)
    return result(seek_checks(fast, slow), on=fast, off=slow)


@case("K1c", "K1's checks with the hack off in both runs: the faster check fails", control_of="K1")
def replay_fast_seek_control(ctx):
    a, b = seek_run(ctx, "K1c_a", False), seek_run(ctx, "K1c_b", False)
    c = seek_checks(a, b)
    return result([{"what": "both runs seeked and played", "ok": c[0]["ok"] and c[1]["ok"]},
                   {"what": "faster check fails", "ok": not c[2]["ok"]}], detail=c)


NAMES = 0x0c3d1b04  # pilot names in the battle, 16 bytes per player (SJIS)
PILOT = "826f8268826b826e8273824f"  # ＰＩＬＯＴ０


def scenes_run(ctx, name: str, extra: list) -> dict:
    e = newest_dc2()
    p = ctx.probe(name, ctx.rom("dc2"), "replay_scenes.lua",
                  {"max": 12000, "after": 60, "reads": [[NAMES + 16 * i, 14] for i in range(4)]}, frames=12100,
                  config=[f"gdxsv:replay={assets.replay(e)}", "gdxsv:ReplaySeekStates=no", *extra],
                  loadstate=False, until_done=True, timeout=300)
    names = [p.e2e.get(f"0x{NAMES + 16 * i:08x}", "") for i in range(4)]
    return {"clean": p.clean, "battle": p.e2e.get("battle"), "names": names,
            **p.facts()}


def hide_name_check(r: dict) -> dict:
    want = [PILOT + f"82{0x50 + i:02x}" for i in range(4)]
    return {"what": "players are named ＰＩＬＯＴ０1 to ０4 in the battle",
            "ok": r["clean"] and [n[:len(want[0])] for n in r["names"]] == want, "names": r["names"]}


@case("V4", "replay hidden names: ReplayHideName shows the players as ＰＩＬＯＴ０1-4 in the battle")
def replay_hide_name(ctx):
    return result([hide_name_check(scenes_run(ctx, "V4", ["gdxsv:ReplayHideName=yes"]))])


@case("V4c", "V4's check without ReplayHideName: the recorded names show", control_of="V4")
def replay_hide_name_control(ctx):
    r = scenes_run(ctx, "V4c", [])
    return result([{"what": "battle reached", "ok": r["clean"] and r["battle"] is not None},
                   {"what": "name check fails", "ok": not hide_name_check(r)["ok"]}], detail=r)


def four_screen_run(ctx, name: str, four: bool) -> dict:
    """Newest Disc 2 replay in a window with ReplayFourScreen; logging and the Lua script go in
    emu.cfg, since the host starts its guests without its --config values."""
    e = newest_dc2()
    rom = ctx.rom("dc2")
    cfg = [f"gdxsv:replay={assets.replay(e)}", "gdxsv:ReplaySeekStates=no", "window:width=480", "window:height=360"]
    if four:
        cfg.append("gdxsv:ReplayFourScreen=yes")
    p = ctx.probe(name, rom, "fourscreen.lua", {"frame": POV_FRAME, "after": 300, "side": POV_SIDE, "id": POV_ID},
                  frames=POV_FRAME + 400, config=cfg, loadstate=False, window=True, timeout=300,
                  states=assets.state_dir(rom),
                  emu_cfg=f"[config]\nLuaFileName = {name}.lua\n\n[log]\nLogToFile = 1\nVerbosity = 3\n")
    d = os.path.join(ctx.out, name)
    log_dir = os.path.dirname(p.inst.log_path)
    povs = {1: (p.e2e.get("side"), p.e2e.get("id"))}
    warning_gains = {1: p.e2e.get("warning_gain")}
    logs = {}
    for n in (2, 3, 4):
        vals = {}
        try:
            with open(os.path.join(d, f"e2e_pov{n}.log"), encoding="utf-8") as f:
                vals = dict(l[4:].strip().split("=", 1) for l in f if l.startswith("E2E "))
        except OSError:
            pass
        povs[n] = (vals.get("side"), vals.get("id"))
        warning_gains[n] = vals.get("warning_gain")
        try:
            with open(os.path.join(log_dir, f"flycast-{n}P.log"), encoding="utf-8", errors="replace") as f:
                logs[n] = f.read()
        except OSError:
            logs[n] = ""
    return {"clean": p.clean, "host_log": p.log, "povs": povs, "warning_gains": warning_gains, "logs": logs,
            **p.facts()}


def four_screen_checks(r: dict) -> list:
    host = re.search(r"multi-pov: host session (\S+) with (\d+) guests", r["host_log"])
    session = host.group(1) if host else None
    checks = [{"what": "host started a session with 3 guests", "ok": bool(host) and host.group(2) == "3"}]
    for n, (side, id) in r["povs"].items():
        checks.append({"what": f"screen {n}: the game plays as player {n}", "side": side, "id": id,
                       "ok": side == str(n - 1) and id == str(n)})
        gain = r["warning_gains"][n]
        checks.append({"what": f"screen {n}: warning gain selector is 20", "gain": gain, "ok": gain == "20"})
    for n, log in r["logs"].items():
        checks.append({"what": f"guest {n}P joined the host's sync group and plays the host's replay",
                       "ok": session is not None and f"joined group {session}" in log
                       and f"guest {n}P playing from the host's replay" in log
                       and "start barrier released" in log})
    return checks


@case("V5", "four-screen replay: three guests on one sync group, each plays as its player with a quieter warning")
def replay_four_screen(ctx):
    r = four_screen_run(ctx, "V5", True)
    return result([{"what": "probe ran", "ok": r["clean"]}] + four_screen_checks(r))


@case("V5c", "V5 without ReplayFourScreen: no guests, stock warning gain; the four-screen checks fail",
      control_of="V5")
def replay_four_screen_control(ctx):
    r = four_screen_run(ctx, "V5c", False)
    c = four_screen_checks(r)
    must_fail = [x for x in c if "guest" in x["what"] or "warning gain" in x["what"]
                 or x["what"].startswith(("screen 2", "screen 3", "screen 4"))]
    return result([{"what": "probe ran", "ok": r["clean"]},
                   {"what": "single screen keeps the stock warning gain", "ok": r["warning_gains"][1] == "32"},
                   {"what": "every four-screen check fails", "failed": [x["what"] for x in must_fail if not x["ok"]],
                    "ok": all(not x["ok"] for x in must_fail)}], detail=c)


RECEIVING_RE = re.compile(r"multi-pov viewer: receiving .* (\d)P")
GUEST_GONE = "host is gone, closing this screen"


def four_screen_window(ctx, name: str, four: bool, params: dict, extra: list = ()):
    """Newest Disc 2 replay in a 960x720 window, driven by fourscreen_host.lua. The script
    goes in the host's --config (its guests run without it); logging in emu.cfg reaches all."""
    rom = ctx.rom("dc2")
    os.makedirs(ctx.out, exist_ok=True)
    lua = write_lua(os.path.join(ctx.out, name + ".lua"), "fourscreen_host.lua", {"max": 30000, **params})
    cfg = [f"gdxsv:replay={assets.replay(newest_dc2())}", "window:width=960", "window:height=720", *extra]
    if four:
        cfg.append("gdxsv:ReplayFourScreen=yes")
    return emu.launch(ctx.exe, rom, os.path.join(ctx.out, name), config=cfg, headless=False, lua=lua,
                      state_dir=assets.state_dir(rom), emu_cfg="[log]\nLogToFile = 1\nVerbosity = 3\n")


def e2e_values(inst) -> dict:
    try:
        with open(os.path.join(inst.dir, "e2e.log"), encoding="utf-8", errors="replace") as f:
            return dict(l[4:].strip().split("=", 1) for l in f if l.startswith("E2E ") and "=" in l)
    except OSError:
        return {}


def wait_for(fn, timeout: float) -> bool:
    end = time.time() + timeout
    while time.time() < end:
        if fn():
            return True
        time.sleep(0.5)
    return False


def guest_logs(inst) -> dict:
    logs = {}
    for n in (2, 3, 4):
        try:
            with open(os.path.join(inst.dir, f"flycast-{n}P.log"), encoding="utf-8", errors="replace") as f:
                logs[n] = f.read()
        except OSError:
            logs[n] = ""
    return logs


def stop_all(inst) -> None:
    for pid in emu.processes(inst.run_exe):
        emu.kill(pid)
    inst.proc.wait(10)


def receiving(log: str) -> list:
    return sorted({int(n) for n in RECEIVING_RE.findall(log)})


def display_run(ctx, name: str, four: bool) -> dict:
    """Two grabs of the host window 1 s apart, once the battle is on and (four-screen) the host
    receives every guest's video."""
    inst = four_screen_window(ctx, name, four, {})
    shots = [os.path.join(inst.dir, f"shot{i}.png") for i in (0, 1)]
    if not recheck.ACTIVE:
        try:
            ready = wait_for(lambda: "battle" in e2e_values(inst) and (not four or len(receiving(inst.log)) == 3), 120)
            time.sleep(3)
            with image.SCREEN_LOCK:
                for i, path in enumerate(shots):
                    if i:
                        time.sleep(1)
                    image.grab(inst.proc.pid, path)
        finally:
            stop_all(inst)
    return {"host_log": inst.log, "shots": shots}


def quadrants(path: str) -> list:
    """Top left, top right, bottom left, bottom right quarter of the image at path, in RGB."""
    from PIL import Image
    im = Image.open(path).convert("RGB")
    w, h = im.size
    return [im.crop((x, y, x + w // 2, y + h // 2)) for y in (0, h // 2) for x in (0, w // 2)]


def mean_diff(a, b) -> float:
    from PIL import ImageChops, ImageStat
    return round(sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3, 1)


def timer_mask(quadrant) -> list:
    """Near-white pixels (all channels over 200) in the battle timer's box, scaled to 64x32."""
    w, h = quadrant.size
    box = quadrant.crop(tuple(int(f * s) for f, s in zip(TIMER_BOX, (w, h, w, h)))).resize((64, 32))
    return [min(px) > 200 for px in box.getdata()]


def display_checks(r: dict) -> list:
    if not all(os.path.isfile(p) for p in r["shots"]):
        return [{"what": "host window grabbed twice", "ok": False}]
    a, b = quadrants(r["shots"][0]), quadrants(r["shots"][1])
    moved = [mean_diff(x, y) for x, y in zip(a, b)]
    apart = [mean_diff(a[i], a[j]) for i in range(4) for j in range(i + 1, 4)]
    # Every viewpoint shows the battle timer at the same place in its frame: in a 2x2 grid,
    # each quadrant's timer box has the top left one's white digits.
    masks = [timer_mask(q) for q in a]
    white = [i for i, v in enumerate(masks[0]) if v]
    recall = [round(sum(1 for i in white if m[i]) / len(white), 2) if white else 0.0 for m in masks[1:]]
    return [{"what": "host receives the video of 2P, 3P and 4P", "ok": receiving(r["host_log"]) == [2, 3, 4]},
            {"what": "every quadrant changes between the grabs (live)", "mean_diff": moved,
             "ok": all(m >= LIVE_DIFF for m in moved)},
            {"what": "the four quadrants differ (four viewpoints)", "mean_diff": apart,
             "ok": all(m >= LIVE_DIFF for m in apart)},
            {"what": "each quadrant has the battle timer at the same place (a 2x2 grid of game screens)",
             "white_pixels": len(white), "recall": recall,
             "ok": len(white) >= TIMER_MIN_WHITE and all(x >= TIMER_RECALL for x in recall)}]


# Measured on windows-mini, 960x720 window: quadrants move by 6.9 or more in 1 s.
LIVE_DIFF = 2.0
TIMER_BOX = (0.60, 0.03, 0.82, 0.15)  # of a game screen: the TIME digits, top right
TIMER_MIN_WHITE = 50
TIMER_RECALL = 0.8


@case("V7", "four-screen replay: the host window shows four live viewpoints in a 2x2 grid")
def replay_four_screen_display(ctx):
    return result(display_checks(display_run(ctx, "V7", True)))


@case("V7c", "V7's checks on a replay played without ReplayFourScreen: the grid check fails", control_of="V7")
def replay_four_screen_display_control(ctx):
    c = display_checks(display_run(ctx, "V7c", False))
    return result([{"what": "window grabbed, live", "ok": len(c) == 4 and c[1]["ok"]},
                   {"what": "the grid check fails", "ok": len(c) == 4 and not c[3]["ok"]}], detail=c)


CLOSE_WAIT = 15  # seconds after the close; guests exit within 1 s on windows-mini


def close_run(ctx, name: str, close: bool) -> dict:
    """Four-screen replay in battle with every guest's video arriving; then the host window is
    closed (WM_CLOSE) or left open, and the guests still running are counted for CLOSE_WAIT s."""
    inst = four_screen_window(ctx, name, True, {})
    saved = os.path.join(inst.dir, "close.json")
    if recheck.ACTIVE:
        rec = recheck.load(saved, {})
    else:
        try:
            ready = wait_for(lambda: "battle" in e2e_values(inst) and len(receiving(inst.log)) == 3, 120)
            before = len(emu.processes(inst.run_exe)) - 1
            if close:
                emu.close(inst.proc.pid)
            t0, left = time.time(), before
            while time.time() - t0 < CLOSE_WAIT:
                left = len(emu.processes(inst.run_exe)) - (1 if inst.poll() is None else 0)
                if left == 0:
                    break
                time.sleep(0.5)
            rec = {"ready": ready, "guests_before": before, "guests_left": left,
                   "seconds": round(time.time() - t0, 1), "host_rc": inst.poll()}
        finally:
            stop_all(inst)
        recheck.save(saved, rec)
    return {**rec, "logs": guest_logs(inst)}


def close_checks(r: dict) -> list:
    return [{"what": "three guests ran, every video arriving", "ok": r.get("ready") and r.get("guests_before") == 3},
            {"what": f"every guest exited within {CLOSE_WAIT} s", "left": r.get("guests_left"),
             "seconds": r.get("seconds"), "ok": r.get("guests_left") == 0},
            {"what": "each guest logged that its host is gone",
             "ok": all(GUEST_GONE in log for log in r["logs"].values())},
            {"what": "the host exited with 0", "ok": r.get("host_rc") == 0}]


@case("V8", "four-screen replay: closing the host window ends its three guests")
def replay_four_screen_close(ctx):
    return result(close_checks(close_run(ctx, "V8", True)))


@case("V8c", "V8's checks with the host window left open: the exit checks fail", control_of="V8")
def replay_four_screen_close_control(ctx):
    c = close_checks(close_run(ctx, "V8c", False))
    return result([{"what": "three guests ran", "ok": c[0]["ok"]},
                   {"what": "the exit checks fail", "ok": not any(x["ok"] for x in c[1:])}], detail=c)


CONTROL_STEPS = ("speed_up", "seek_back", "seek_forward", "speed_up2")
HOST_SEEK_RE = re.compile(r"multi-pov: seek (\d+) -> (\d+), generation")
SYNC_RE = re.compile(r"^(\d+):(\d\d):(\d\d\d) .*spectate sync: frame (\d+)", re.M)
# Frames per second of play between two "spectate sync" lines; 1x is 60, sped up measured 180.
SPED_UP_FPS = 90
# gdxsv:replay_target_round of V9: the screens start there, skipping round 2.
JUMP_ROUND = 3


def controls_run(ctx, name: str, four: bool) -> dict:
    """The host starts at round JUMP_ROUND. In its battle the host speeds up, seeks back, seeks
    forward and speeds up again, then plays into the next round's battle; the processes still
    running are counted there."""
    inst = four_screen_window(ctx, name, four, {"controls": True, "gap": 120, "after": 300},
                              [f"gdxsv:replay_target_round={JUMP_ROUND}"])
    saved = os.path.join(inst.dir, "controls.json")
    if recheck.ACTIVE:
        rec = recheck.load(saved, {})
    else:
        try:
            wait_for(lambda: inst.poll() is not None or any(k in e2e_values(inst) for k in ("done", "error")), 300)
            rec = {"running": len(emu.processes(inst.run_exe)), "host_exited": inst.poll()}
        finally:
            stop_all(inst)
        recheck.save(saved, rec)
    starts = replayfile.start_indexes(assets.replay(newest_dc2()))
    return {**rec, "e2e": e2e_values(inst), "host_log": inst.log, "logs": guest_logs(inst), "starts": starts}


def fastest_fps(log: str) -> float:
    marks = [((int(h) * 60 + int(m)) * 1000 + int(ms), int(f)) for h, m, ms, f in SYNC_RE.findall(log)]
    rates = [(f1 - f0) * 1000 / (t1 - t0) for (t0, f0), (t1, f1) in zip(marks, marks[1:]) if t1 > t0]
    return round(max(rates), 1) if rates else 0.0


def controls_checks(r: dict) -> list:
    e, starts = r["e2e"], r["starts"]
    jump = f"ctrl_change_round_:{JUMP_ROUND} "
    next_round = JUMP_ROUND + 1
    after = f"StartMsg key_msg_count {starts[JUMP_ROUND]}" if len(starts) > JUMP_ROUND else None
    round2 = f"StartMsg key_msg_count {starts[1]}" if len(starts) > 1 else None
    # A seek inside the jumped-to round: both ends at or past its start.
    floor = starts[JUMP_ROUND - 1] if len(starts) >= JUMP_ROUND else None
    seeks = [(int(a), int(b)) for a, b in HOST_SEEK_RE.findall(r["host_log"])]
    inside = [(a, b) for a, b in seeks if floor is not None and a >= floor and b >= floor]
    fps = fastest_fps(r["host_log"])
    checks = [{"what": "host: speed up, seek back, seek forward, speed up, then the next battle",
               "ok": all(k in e for k in CONTROL_STEPS) and e.get("next", "none") != "none" and "done" in e,
               "e2e": e},
              {"what": f"host jumped once to round {JUMP_ROUND}, round 2 not played",
               "ok": r["host_log"].count(jump) == 1 and round2 is not None and round2 not in r["host_log"]},
              {"what": f"host played faster than {SPED_UP_FPS} frames per second", "fps": fps,
               "ok": fps > SPED_UP_FPS},
              {"what": f"host published a seek back and a seek forward in round {JUMP_ROUND} to its guests",
               "seeks": seeks, "ok": any(b < a for a, b in inside) and any(b > a for a, b in inside)},
              {"what": f"all four processes ran into round {next_round}", "running": r.get("running"),
               "ok": r.get("running") == 4 and r.get("host_exited") is None}]
    for n, log in r["logs"].items():
        checks.append({"what": f"guest {n}P jumped to round {JUMP_ROUND}, followed both seeks, then started round "
                       f"{next_round}",
                       "ok": jump in log and len(FRAME_JUMP_RE.findall(log)) >= 2 and after is not None
                       and after in log and GUEST_GONE not in log})
    return checks


@case("V9", "four-screen replay: gdxsv:replay_target_round=3 starts all four screens at round 3; host speed "
      "up, seek back and seek forward keep them playing into the next round")
def replay_four_screen_controls(ctx):
    return result(controls_checks(controls_run(ctx, "V9", True)))


@case("V9c", "V9 on a replay played without ReplayFourScreen: the seek, process and guest checks fail",
      control_of="V9")
def replay_four_screen_controls_control(ctx):
    c = controls_checks(controls_run(ctx, "V9c", False))
    return result([{"what": f"host jumped to round {JUMP_ROUND}, controls ran into the next round",
                    "ok": c[0]["ok"] and c[1]["ok"]},
                   {"what": "the seek, process and guest checks fail", "ok": not any(x["ok"] for x in c[3:])}],
                  detail=c)



TAKEOVER_FRAME = 3000
TAKEOVER_RE = re.compile(r"(StartTakeover|RetryTakeover) at key_msg_count_:(\d+)")
# B X Y C Z on the pad: MCS B X Y LT RT (0xb003), held together in no corpus replay
TAKEOVER_HOLD = 0x0703
SAVE_INTERVAL = 60  # ReplaySeekFrames: Take Over resumes from the newest state saved before the frame


def takeover_run(ctx, name: str, takeover: bool) -> dict:
    e = newest_dc2()
    extra = [f"gdxsv:replay_takeover_frame={TAKEOVER_FRAME}"] if takeover else []
    p = ctx.probe(name, ctx.rom("dc2"), "takeover.lua",
                  {"max": 12000, "window": 3600, "hold": TAKEOVER_HOLD, "mcs": 0xb003, "run": 60, "inetbuf": 0x0c3ab984},
                  frames=16000, config=[f"gdxsv:replay={assets.replay(e)}", "gdxsv:ReplaySeekStates=no",
                                        f"gdxsv:replay_target_frame={TAKEOVER_FRAME}", *extra],
                  loadstate=False, window=True, timeout=400)
    return {"clean": p.clean, "e2e": dict(p.e2e), "events": TAKEOVER_RE.findall(p.log), **p.facts()}


def takeover_checks(r: dict) -> list:
    at = [int(n) for _, n in r["events"]]
    kinds = [k for k, _ in r["events"]]
    return [{"what": f"takeover started at the last saved state before input {TAKEOVER_FRAME} (states every "
                     f"{SAVE_INTERVAL})", "events": r["events"],
             "ok": kinds[:1] == ["StartTakeover"] and TAKEOVER_FRAME - SAVE_INTERVAL <= at[0] < TAKEOVER_FRAME},
            {"what": "the game takes this pad's input, not the recording", "held": r["e2e"].get("held"),
             "ok": r["e2e"].get("held", "none") != "none"},
            {"what": "START retries from the same input, then plays on", "held2": r["e2e"].get("held2"),
             "ok": kinds[:3] == ["StartTakeover", "RetryTakeover", "StartTakeover"] and len(set(at[:3])) == 1
             and r["e2e"].get("held2", "none") != "none"}]


@case("V6", f"replay takeover: gdxsv:replay_takeover_frame={TAKEOVER_FRAME}, the pad plays from that input, "
            "START retries from it")
def replay_takeover(ctx):
    r = takeover_run(ctx, "V6", True)
    return result([{"what": "battle shown", "ok": "battle" in r["e2e"]}] + takeover_checks(r), **r)


@case("V6c", "V6's checks on the replay played without a takeover: they fail", control_of="V6")
def replay_takeover_control(ctx):
    r = takeover_run(ctx, "V6c", False)
    c = takeover_checks(r)
    return result([{"what": "battle shown", "ok": "battle" in r["e2e"]},
                   {"what": "every takeover check fails", "ok": all(not x["ok"] for x in c)}], detail=c, **r)
