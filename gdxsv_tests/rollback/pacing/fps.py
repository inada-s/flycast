# Game fps (ef count / ef span) and timesync events (skip = inserted frame, tsdrop) per peer.
# usage: fps.py <fpt dir>...
import glob, sys
for d in sys.argv[1:]:
    out = []
    for path in sorted(glob.glob(d + "/fpt_*.csv")):
        ef, skip, drop = [], 0, 0
        for line in open(path):
            p = line.split(",")
            if len(p) != 6: continue
            if p[2] == "ef": ef.append(int(p[0]))
            elif p[2] == "skip": skip += 1
            elif p[2] == "tsdrop": drop += 1
        ef.sort()
        out.append(f"{(len(ef) - 1) * 1e6 / (ef[-1] - ef[0]):.3f}/{skip}/{drop}")
    print(d, "fps/skip/tsdrop:", " ".join(out))
