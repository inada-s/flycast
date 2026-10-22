# Limiter oversleep (lim event field b, us): p50/p99/max per run.
import glob, sys
for d in sys.argv[1:]:
    xs = []
    for path in glob.glob(d + "/fpt_*.csv"):
        for line in open(path):
            p = line.split(",")
            if len(p) == 6 and p[2] == "lim" and int(p[3]) > 0: xs.append(int(p[4]))
    xs.sort()
    print(d.split("/")[-2], f"n={len(xs)} oversleep p50={xs[len(xs)//2]} p99={xs[len(xs)*99//100]} max={xs[-1]}")
