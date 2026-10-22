# Latency per trace: local input sample (inp) -> end of that frame (ef) -> next Present call / shown vblank.
# Shown vblank = first host vbl after the Present returns. Also a B3 replay (--b3): each frame shown at the
# first vbl after its Present call starts (render done), newest frame wins a vblank; prints drops and bad steps.
# usage: lat.py [--b3] <fpt dir>...
import bisect, glob, statistics, sys
args = sys.argv[1:]
b3 = args and args[0] == "--b3"
if b3:
    args = args[1:]

def q(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p * len(xs)))] / 1000

for d in args:
    for path in sorted(p.replace(chr(92), "/") for p in glob.glob(d + "/fpt_*.csv")):
        ev = []
        for line in open(path):
            p = line.split(",")
            if len(p) == 6:
                ev.append((int(p[0]), p[2], int(p[3]), p[1]))
        ev.sort()
        vbl = [t for t, k, a, h in ev if k == "vbl"]
        efs = [t for t, k, a, h in ev if k == "ef"]
        pres = [(t - a, t) for t, k, a, h in ev if k == "pres"]
        inps = [t for t, k, a, h in ev if k == "inp"]
        same = {h for t, k, a, h in ev if k == "pres"} == {h for t, k, a, h in ev if k == "inp"}
        if len(efs) < 300 or len(pres) < 300:
            print(path, "too short"); continue
        lo, hi = efs[0] + 5e6, min(efs[-1], vbl[-1] - 1e5)
        ps = [s for s, e in pres]
        lp, ls = [], []
        for t in inps:
            if not lo <= t <= hi:
                continue
            i = bisect.bisect_right(efs, t)
            if i >= len(efs):
                continue
            j = bisect.bisect_right(ps, t if same else efs[i])
            if j >= len(pres):
                continue
            s, e = pres[j]
            v = vbl[bisect.bisect_right(vbl, e)]
            lp.append(s - t); ls.append(v - t)
        name = path.split("/")[-3] + "/" + path.split("_")[-1]
        line = (f"{name} n={len(lp)} inp->present p50/p99={q(lp,.5):.1f}/{q(lp,.99):.1f}ms "
                f"inp->shown p50/p99={q(ls,.5):.1f}/{q(ls,.99):.1f}ms")
        if b3:
            win = [(s, e) for s, e in pres if lo <= s <= hi]
            real = [bisect.bisect_right(vbl, e) for s, e in win]
            sim = [bisect.bisect_right(vbl, s) for s, e in win]
            shown = sorted(set(sim))
            drops = len(sim) - len(shown)
            bad_sim = sum(1 for a, b in zip(shown, shown[1:]) if b - a != 1) + drops
            bad_real = sum(1 for a, b in zip(real, real[1:]) if b - a != 1)
            gain = statistics.median(vbl[r] - vbl[m] for r, m in zip(real, sim)) / 1000
            line += f" | real bad={bad_real} B3 bad={bad_sim} (drops {drops}) shown earlier by p50 {gain:.1f}ms"
        print(line)
