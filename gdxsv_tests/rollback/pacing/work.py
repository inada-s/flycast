# Time from the limiter wake (lim event) to the next Present call (pres t - duration), us: p1/p50/p99.
import bisect, glob, sys
for d in sys.argv[1:]:
    for path in sorted(glob.glob(d + "/fpt_*.csv")):
        lim, calls = [], []
        for line in open(path):
            p = line.split(",")
            if len(p) != 6: continue
            if p[2] == "lim": lim.append(int(p[0]))
            elif p[2] == "pres": calls.append(int(p[0]) - int(p[3]))
        lim.sort(); xs = []
        for c in sorted(calls):
            i = bisect.bisect_right(lim, c) - 1
            if i >= 0 and c - lim[i] < 16000: xs.append(c - lim[i])
        xs.sort(); n = len(xs)
        print(d.split("/")[-2], path[-9:], f"wake->call p1={xs[n//100]} p50={xs[n//2]} p99={xs[n*99//100]}")
