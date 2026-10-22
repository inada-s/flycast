# Limiter behaviour after each injected hang: wake intervals (lim event times) of the next 6 frames, ms.
# A catch-up burst shows as several intervals well under a period.
# usage: hang.py <fpt dir>...
import glob, sys
for d in sys.argv[1:]:
    for path in sorted(glob.glob(d + "/fpt_*.csv")):
        ev = []
        for line in open(path):
            p = line.split(",")
            if len(p) == 6 and p[2] in ("lim", "hang"):
                ev.append((int(p[0]), p[2], int(p[3])))
        ev.sort()
        short, rows = 0, []
        for i, (t, k, a) in enumerate(ev):
            if k != "hang":
                continue
            lims = [x[0] for x in ev[i + 1:i + 12] if x[1] == "lim"][:7]
            iv = [(b - a) / 1000 for a, b in zip(lims, lims[1:])]
            short += sum(1 for x in iv[1:] if x < 8)
            rows.append(" ".join(f"{x:.1f}" for x in iv))
        print(d.split("/")[-2], path[-9:], f"hangs={len(rows)} short(<8ms, after the first)={short}")
        for r in rows[:3]:
            print("   ", r)
