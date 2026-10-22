# Bursts of bad display intervals (shown-vblank step != 1), per fpt_*.csv.
# Shown vblank = first host vbl after the Present returns. A burst = >= MINBAD bad
# intervals, each <= GAP presents from the previous one.
# usage: burst.py [--skip S] <fpt dir>...
import bisect, glob, statistics, sys
GAP, MINBAD = 8, 4
args = sys.argv[1:]
skip = 0
if args and args[0] == "--skip":
    skip = float(args[1]) * 1e6
    args = args[2:]
for d in args:
    for path in sorted(p.replace(chr(92), "/") for p in glob.glob(d + "/fpt_*.csv")):
        ev = []
        for line in open(path):
            p = line.split(",")
            if len(p) == 6:
                ev.append((int(p[0]), p[2], int(p[3])))
        ev.sort()
        vbl = [t for t, k, a in ev if k == "vbl"]
        efs = [t for t, k, a in ev if k == "ef"]
        pres = [(t, a) for t, k, a in ev if k == "pres"]
        if len(pres) < 300 or len(vbl) < 300:
            print(path, "too short", len(pres), len(vbl)); continue
        lo = (efs[0] if efs and efs[0] > pres[0][0] + 5e6 else pres[0][0]) + skip
        hi = min(efs[-1] if efs else pres[-1][0], pres[-1][0], vbl[-1])
        pres = [x for x in pres if lo <= x[0] <= hi]
        js, ph = [], []
        for t, a in pres:
            js.append(bisect.bisect_left(vbl, t))
            i = bisect.bisect_right(vbl, t - a) - 1
            ph.append(t - a - vbl[i] if i >= 0 else -1)
        bad = [k for k in range(1, len(js)) if js[k] - js[k - 1] != 1]
        bursts, cur = [], []
        for k in bad:
            if cur and k - cur[-1] > GAP:
                if len(cur) >= MINBAD: bursts.append(cur)
                cur = []
            cur.append(k)
        if len(cur) >= MINBAD: bursts.append(cur)
        inb = sum(len(b) for b in bursts)
        T = (pres[-1][0] - pres[0][0]) / 1e6
        dur = [(pres[b[-1]][0] - pres[b[0]][0]) / 1e6 for b in bursts]
        sp = [(pres[b[0]][0] - pres[a[0]][0]) / 1e6 for a, b in zip(bursts, bursts[1:])]
        bph = [statistics.median(ph[b[0]:b[-1] + 1]) / 1000 for b in bursts]
        # call-phase drift: median step of call time mod vblank period over non-bad frames
        period = statistics.median([b - a for a, b in zip(vbl, vbl[1:])])
        steps = [((b - a + period / 2) % period) - period / 2 for a, b in zip(ph, ph[1:]) if a >= 0 and b >= 0]
        drift = statistics.median(steps)
        fmt = lambda xs: " ".join(f"{x:.1f}" for x in xs)
        print(f"{path.replace(chr(92), "/").split("/")[-3]} {path[-14:]} T={T:.0f}s n={len(pres)} bad={len(bad)} ({100*len(bad)/len(pres):.2f}%) "
              f"bursts={len(bursts)} in_burst={inb} burst_s={sum(dur):.1f} drift={drift:+.1f}us/f step_p1/p99={sorted(steps)[len(steps)//100]:.0f}/{sorted(steps)[-len(steps)//100]:.0f}us")
        if bursts:
            print(f"   dur_s: {fmt(dur)}\n   spacing_s: {fmt(sp)}\n   call_phase_ms: {fmt(bph)}")
