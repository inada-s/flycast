# Median / mean intervals of vbl, lim, pres, ef events (us) in the ef window.
# usage: per.py <fpt dir>...
import glob, statistics, sys
for d in sys.argv[1:]:
    for path in sorted(glob.glob(d + "/fpt_*.csv")):
        ev = {}
        for line in open(path):
            p = line.split(",")
            if len(p) == 6: ev.setdefault(p[2], []).append(int(p[0]))
        lo, hi = min(ev["ef"]), max(ev["ef"])
        out = []
        for k in ("vbl", "lim", "pres", "ef"):
            xs = sorted(t for t in ev.get(k, []) if lo <= t <= hi)
            iv = [b - a for a, b in zip(xs, xs[1:])]
            if iv: out.append(f"{k} med={statistics.median(iv):.0f} mean={(xs[-1]-xs[0])/len(iv):.1f}")
        print(d.split("/")[-2], path[-9:], " | ".join(out))
