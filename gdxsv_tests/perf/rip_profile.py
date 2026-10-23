#!/usr/bin/env python3
"""Profile from sampler.cpp output: the busiest thread's functions and their category shares.

usage: rip_profile.py --syms flycast.exe [--top N] rip.txt [rip.txt ...]
--syms: an executable with symbols, the same build as the one sampled (a MinGW RelWithDebInfo build, or a Release
build relinked without -s). Its symbols are read with `nm -n -C` (nm from PATH or --nm).
Addresses outside every module are JIT code: the SH4 and ARM7 blocks and their dispatchers.
"""
import argparse
import bisect
import collections
import struct
import subprocess
import sys

SYSTEM = ("ntdll.dll", "win32u.dll", "kernelbase.dll")  # threads waiting in the OS


def image_base(exe: str) -> int:
    with open(exe, "rb") as f:
        head = f.read(4096)
    pe = struct.unpack_from("<I", head, 0x3C)[0]
    magic = struct.unpack_from("<H", head, pe + 24)[0]
    return struct.unpack_from("<Q", head, pe + 48)[0] if magic == 0x20B else struct.unpack_from("<I", head, pe + 52)[0]


def symbols(exe: str, nm: str) -> list:
    out = subprocess.run([nm, "-n", "-C", exe], capture_output=True, text=True, errors="replace").stdout
    syms = []
    for line in out.splitlines():
        p = line.split(maxsplit=2)
        if len(p) == 3 and p[1] in "tT":
            syms.append((int(p[0], 16), p[2]))
    return syms


def category(f: str) -> str:
    if f == "<JIT>":
        return "jit"
    if "aica::arm" in f:
        return "aica arm7"
    if "aica" in f.lower():
        return "aica sound"
    if "sched" in f or "INTC" in f or "spg" in f:
        return "scheduler/interrupts"
    if "bm_" in f or "gdxsv_sh4_code_addr" in f:
        return "block manager"
    if f.lower().endswith(".dll"):
        return "dll"
    if "Mem" in f or "addrspace" in f or "_area" in f:
        return "memory handlers"
    return "other"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--syms", required=True)
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("samples", nargs="+")
    a = ap.parse_args()
    syms = symbols(a.syms, a.nm)
    if not syms:
        print(f"no symbols in {a.syms}")
        return 2
    addrs = [s for s, _ in syms]
    pref = image_base(a.syms)
    fn, cat = collections.Counter(), collections.Counter()
    for path in a.samples:
        mods, rows = [], []
        with open(path) as f:
            for line in f:
                p = line.split()
                if p[0] == "M":
                    mods.append((int(p[1], 16), int(p[2], 16), p[3]))
                else:
                    rows.append((int(p[0]), int(p[1], 16), int(p[2])))
        exe = next(m for m in mods if m[2].lower().endswith(".exe"))

        def name(rip: int) -> str:
            if exe[0] <= rip < exe[0] + exe[1]:
                i = bisect.bisect_right(addrs, rip - exe[0] + pref) - 1
                return syms[i][1] if i >= 0 else "?exe"
            for b, s, n in mods:
                if b <= rip < b + s:
                    return n
            return "<JIT>"

        busy = collections.Counter()
        for t, r, c in rows:
            if name(r).lower() not in SYSTEM:
                busy[t] += c
        tid = busy.most_common(1)[0][0]  # the emulation thread
        for t, r, c in rows:
            if t == tid:
                f = name(r)
                fn[f] += c
                cat[category(f)] += c
    n = sum(fn.values())
    print(f"busiest thread: {n} samples in {len(a.samples)} file(s)")
    for f, c in fn.most_common(a.top):
        print(f"{100 * c / n:5.1f}% {f[:110]}")
    for k, c in cat.most_common():
        print(f"CAT {100 * c / n:5.1f}% {k}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
