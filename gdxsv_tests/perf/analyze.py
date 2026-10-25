#!/usr/bin/env python3
"""Metrics of one perf battle cell from the GDXSV_PERF_TRACE csv of peer 1 and its PresentMon csv.

usage: analyze.py <perf.csv> [--pm pm.csv] [--settle FRAMES] [--frames FRAMES] [--hz HZ]   (prints the metrics JSON)
The measured window starts at GGPO frame --settle and lasts --frames frames. Times in ms.
--hz: the display's refresh rate, for J1 (how many vblanks each frame stays on screen).
"""
import argparse
import bisect
import csv
import json
import statistics

DPAD_BIT = {13: 0x40, 14: 0x80}  # SDL dpad left/right -> kcode bit in the trace's "add" (active high)
RESPONSE_FRAMES = 2  # the game shows a press made in GGPO frame N at frame N+2
FRAME_MS = 1000 / 59.94


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p / 100 * len(v)))] if v else None


def stats(v):
    if not v:
        return {"n": 0}
    return {"n": len(v), "avg": round(statistics.fmean(v), 4), "med": round(statistics.median(v), 4),
            "p95": round(pct(v, 95), 4), "p99": round(pct(v, 99), 4), "max": round(max(v), 4)}


def read_trace(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.reader(f):
            if len(r) == 5 and r[0].isdigit():
                rows.append((int(r[0]), int(r[1]), int(r[2]), r[3], int(r[4])))
    rows.sort(key=lambda x: x[0])
    freq = next(a for q, t, fr, e, a in rows if e == "freq")
    return rows, freq


def read_pm(path):
    # PresentMon builds name the 1.x columns msInPresentAPI, QPCTime as well as MsInPresentAPI, TimeInQPC
    def column(k):
        return "TimeInQPC" if k == "QPCTime" else "M" + k[1:] if k.startswith("ms") else k
    with open(path, newline="", encoding="utf-8-sig") as f:
        rows = [{column(k): v for k, v in r.items()} for r in csv.DictReader(f)]
    return [r for r in rows if r.get("TimeInQPC", "NA") != "NA"]


def judder(P, hz, window_ms, starts):
    """J1: vblanks each frame stays on screen, and per minute how often that is not the steady count (2 at 120 Hz).
    starts: (qpc, is a re-present) of the trace events right before each Present ("frame_present", "represent").
    A re-present adds its time on screen to the frame it repeats."""
    vblank = 1000 / hz
    steady = max(1, round(FRAME_MS / vblank))
    shown = lambda r: float(r["MsBetweenDisplayChange"]) if r["MsBetweenDisplayChange"] not in ("NA", "0.0") else 0.0
    held, repeats = [], 0
    for r in P:
        q = int(r["TimeInQPC"])
        i = bisect.bisect_right(starts, (q, True))
        if held and i > 0 and starts[i - 1][1]:
            held[-1] += shown(r)
            repeats += 1
        else:
            held.append(shown(r))
    held = [round(x / vblank) for x in held]
    hist = {}
    for n in held:
        hist[n] = hist.get(n, 0) + 1
    per_min = 60000 / window_ms
    return {"hz": hz, "steady": steady, "off_per_min": round(sum(n != steady for n in held) * per_min, 1),
            "not_shown_per_min": round(sum(n == 0 for n in held) * per_min, 1),
            "represents_per_min": round(repeats * per_min, 1), "presents_per_s": round(len(P) * per_min / 60, 1),
            "hist": " ".join(f"{k}:{v}" for k, v in sorted(hist.items()))}


def analyze(trace: str, pm: str = None, settle: int = 180, frames: int = 2100, hz: float = None) -> dict:
    rows, freq = read_trace(trace)
    ms = lambda d: d * 1000.0 / freq
    nb = [(q, fr) for q, t, fr, e, a in rows if e == "next_b"]
    t0 = next((q for q, fr in nb if fr >= settle), None)
    t1 = next((q for q, fr in nb if fr >= settle + frames), None)
    if t0 is None or t1 is None:
        return {"error": f"window not reached: last frame {nb[-1][1] if nb else None}"}
    emu_tid = next(t for q, t, fr, e, a in rows if e == "next_b")
    W = [r for r in rows if t0 <= r[0] < t1]
    m = {"window_s": round(ms(t1 - t0) / 1000, 2), "frames": frames}

    # Rollbacks: load_b (current frame) .. load_e (frame loaded) .. reruns until the next next_b.
    rollbacks = []  # (q_begin, q_end, depth, load_ms, rerun_ms list)
    cur = None
    for q, t, fr, e, a in W:
        if e == "load_b":
            cur = {"q": q, "from": fr, "reruns": [], "rb": None}
        elif cur is None:
            continue
        elif e == "load_e":
            cur["to"], cur["load"], cur["end"] = fr, ms(q - cur["q"]), q
        elif e == "rerun_b":
            cur["rb"] = q
        elif e == "rerun_e" and cur["rb"] is not None:
            cur["reruns"].append(ms(q - cur["rb"]))
            cur["end"], cur["rb"] = q, None
        elif e == "next_b":
            if "to" in cur:
                rollbacks.append((cur["q"], cur["end"], cur["from"] - cur["to"], cur["load"], cur["reruns"]))
            cur = None
    by_depth = {}
    for qb, qe, d, ld, rr in rollbacks:
        by_depth.setdefault(d, []).append(ms(qe - qb))
    loads = [r[3] for r in rollbacks]
    reruns = [x for r in rollbacks for x in r[4]]
    m["C1"] = {str(d): stats(v) for d, v in sorted(by_depth.items()) if 1 <= d <= 5}
    if loads and reruns:
        m["C1"]["5_derived"] = {"avg": round(statistics.fmean(loads) + 5 * statistics.fmean(reruns), 4)}
    m["info"] = {"rollbacks": len(rollbacks),
                 "rolled_back_frames_per_min": round(sum(len(r[4]) for r in rollbacks) / (ms(t1 - t0) / 60000), 1),
                 "depth_hist": {str(d): len(v) for d, v in sorted(by_depth.items())}}
    depth_at = [(r[0], r[2]) for r in rollbacks]

    def depth_between(qa, qb):
        i = bisect.bisect_left(depth_at, (qa, -1))
        return max((d for q, d in depth_at[i:] if q < qb), default=0)

    def intervals(name):
        qs = [q for q, t, fr, e, a in W if e == name and t == emu_tid]
        return [(ms(b - a), depth_between(a, b)) for a, b in zip(qs, qs[1:])]

    def by_depth_median(iv):
        g = {}
        for v, d in iv:
            g.setdefault(str(min(d, 5)), []).append(v)
        return {k: round(statistics.median(v), 4) for k, v in sorted(g.items())}

    # B2: input read / send (add) interval
    m["B2"] = {}
    for name in ("input_read", "add"):
        iv = intervals(name)
        v = [x for x, d in iv]
        m["B2"][name] = {**stats(v), "sd": round(statistics.pstdev(v), 4) if v else None, "med_by_depth": by_depth_median(iv)}
    # B3: emulator frame period = limiter wake to wake (rerun frames never pass the limiter)
    wakes = [q for q, t, fr, e, a in W if e == "wake"]
    iv = [(ms(b - a), depth_between(a, b)) for a, b in zip(wakes, wakes[1:])]
    v = [x for x, d in iv]
    m["B3"] = {**stats(v), "sd": round(statistics.pstdev(v), 4) if v else None, "p1": round(pct(v, 1), 4) if v else None,
               "within_1ms_pct": round(100 * sum(abs(x - FRAME_MS) <= 1 for x in v) / len(v), 2) if v else None,
               "drift_ms": round(ms(wakes[-1] - wakes[0]) / (len(wakes) - 1), 5) if len(wakes) > 1 else None,
               "med_by_depth": by_depth_median(iv)}

    # C3: per-frame times
    emul, render, last = [], [], None
    # (q, GGPO frame) of each frame end: the last end_frame before the next frame's next_b. The rollback re-runs end
    # before it, so they never count.
    end_frames = []
    frame = last_end = None
    for q, t, fr, e, a in W:
        if e == "end_frame":
            last_end = q
        elif e == "next_b":
            if frame is not None and last_end is not None:
                end_frames.append((last_end, frame))
            frame, last_end = fr, None
    saves = [ms(q - b) for (b, e1), (q, e2) in zip(*[iter([(q, e) for q, t, fr, e, a in W if e in ("save_b", "save_e")])] * 2)]
    for q, t, fr, e, a in W:
        if t == emu_tid and e in ("next_e", "save_e", "load_e", "rerun_e", "rerun_b"):
            last = (e, q)
        elif e == "end_frame" and last and last[0] != "rerun_b":
            emul.append(ms(q - last[1]))
            last = None
    presents = [q for q, t, fr, e, a in W if e == "present"]
    if not presents:  # builds before the render thread trace a threaded rendering present as ui_rendered
        presents = [q for q, t, fr, e, a in W if e == "ui_rendered"]
    for q, f in end_frames:
        i = bisect.bisect_right(presents, q)
        if i < len(presents):
            render.append(ms(presents[i] - q))
    m["C3"] = {"emulation": stats(emul), "save": stats(saves), "load": stats(loads),
               "rerun": stats(reruns), "render_present": stats(render)}

    # info: prediction-barrier hits (input_read loop count > 0), timesync waits (idle_e error != 0)
    m["info"]["barrier_hits"] = sum(1 for q, t, fr, e, a in W if e == "input_read" and a > 0)
    m["info"]["idle_errors"] = sum(1 for q, t, fr, e, a in W if e == "idle_e" and a != 0)

    if pm:
        P = read_pm(pm)
        P = [r for r in P if t0 <= int(r["TimeInQPC"]) < t1]
        between = [float(r["MsBetweenPresents"]) for r in P if r["MsBetweenPresents"] != "NA"]
        disp = [float(r["MsBetweenDisplayChange"]) for r in P if r["MsBetweenDisplayChange"] not in ("NA", "0.0")]
        refresh = statistics.median(disp) if disp else None
        m["B1"] = {"presents": len(P), "over_20ms_pct": round(100 * sum(x > 20 for x in between) / len(between), 3) if between else None,
                   "not_one_vblank_pct": round(100 * sum(abs(x - refresh) > refresh / 4 for x in disp) / len(disp), 3) if disp else None,
                   "refresh_ms": round(refresh, 4) if refresh else None,
                   "gpu_active_ms_per_s": round(sum(float(r["MsGPUActive"]) for r in P if r.get("MsGPUActive", "NA") != "NA")
                                                / ms(t1 - t0) * 1000, 2)}
        if hz:
            m["J1"] = judder(P, hz, ms(t1 - t0), [(q, e == "represent") for q, t, fr, e, a in W
                                                      if e in ("represent", "frame_present")])
        m["C3"]["in_present"] = stats([float(r["MsInPresentAPI"]) for r in P if r["MsInPresentAPI"] != "NA"])
        # A1/A2: press -> GGPO frame N whose add carries it -> end_frame of N+2 -> the next present (start, display)
        pq = [int(r["TimeInQPC"]) for r in P]
        shown = [float(r["MsUntilDisplayed"]) if r["MsUntilDisplayed"] != "NA" else None for r in P]
        adds = [(q, a) for q, t, fr, e, a in W if e == "add"]
        add_frame = {}
        frame = None
        for q, t, fr, e, a in W:
            if e == "next_b":
                frame = fr
            elif e == "add":
                add_frame[q] = frame
        ef = {f: q for q, f in end_frames}
        a1, a2, lost = [], [], 0
        for q, t, fr, e, a in W:
            if e != "vpress" or a not in DPAD_BIT:
                continue
            i = bisect.bisect_right(adds, (q, 1 << 40))
            hit = next((aq for aq, k in adds[i:i + 30] if k & DPAD_BIT[a]), None)
            n = add_frame.get(hit)
            fq = ef.get(n + RESPONSE_FRAMES) if n is not None else None
            j = bisect.bisect_right(pq, fq) if fq else len(pq)
            if j >= len(pq):
                lost += 1
                continue
            a1.append(ms(pq[j] - q))
            if shown[j] is not None:
                a2.append(ms(pq[j] - q) + shown[j])
        m["A1"] = {**stats(a1), "lost": lost}
        m["A2"] = stats(a2)
    return m


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("trace")
    ap.add_argument("--pm")
    ap.add_argument("--settle", type=int, default=180)
    ap.add_argument("--frames", type=int, default=2100)
    ap.add_argument("--hz", type=float)
    a = ap.parse_args()
    print(json.dumps(analyze(a.trace, a.pm, a.settle, a.frames, a.hz), indent=1))


if __name__ == "__main__":
    main()
