#!/usr/bin/env python3
"""Profile from sampler.cpp output: the busiest thread's functions and their category shares.

usage: rip_profile.py --syms flycast.exe [--top N] [--block-map [--functions FILE]] rip.txt [rip.txt ...]
--syms: an executable with symbols, the same build as the one sampled (a MinGW RelWithDebInfo build, or a Release
build relinked without -s). Its symbols are read with `nm -n -C` (nm from PATH or --nm).
Addresses outside every module are JIT code: the SH4 and ARM7 blocks and their dispatchers.
--block-map: read the SH4 block map (GDXSV_SH4_BLOCK_MAP, seek_bench.py --block-map) as blocks.txt next to each
samples file. JIT samples inside a block are attributed to its guest address, the SH4 main loop and memory handlers
get their names, the rest of the JIT (ARM7) stays <JIT other>. Prints the top guest blocks or functions with their
op mix per guest instruction (static, from the block: reads, writes, FPU ops, interpreter fallbacks).
--functions: "<guest addr hex> <name>" lines; blocks are grouped by the nearest function start at or below them.
"""
import argparse
import bisect
import collections
import os
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


def block_map(path: str) -> tuple:
    """Host start -> (host size, guest addr, guest instructions, reads, writes, fpu, fallbacks) or (host size, name)
    for the main loop and memory handlers; a later line wins."""
    blocks = {}
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) >= 9:
                blocks[int(p[0], 16)] = (int(p[1]), int(p[2], 16), int(p[3]), int(p[5]), int(p[6]), int(p[7]),
                                         int(p[8]))
            elif len(p) == 3:
                blocks[int(p[0], 16)] = (int(p[1]), p[2])
    starts = sorted(blocks)
    return starts, blocks


def category(f: str) -> str:
    if f.startswith("sh4 "):
        return "jit sh4 blocks"
    if f.startswith("<JIT"):
        if f.startswith("<JIT sh4 mem_"):
            return "jit sh4 memory handlers"
        if f == "<JIT sh4 mainloop>":
            return "jit sh4 main loop"
        return "jit" if f == "<JIT>" else "jit other (arm7)"
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
    ap.add_argument("--block-map", action="store_true")
    ap.add_argument("--functions")
    ap.add_argument("samples", nargs="+")
    a = ap.parse_args()
    syms = symbols(a.syms, a.nm)
    if not syms:
        print(f"no symbols in {a.syms}")
        return 2
    addrs = [s for s, _ in syms]
    pref = image_base(a.syms)
    funcs = []
    if a.functions:
        with open(a.functions) as f:
            funcs = sorted((int(p[0], 16), p[1] if len(p) > 1 else "") for p in (line.split(maxsplit=1) for line in f)
                           if p and not p[0].startswith("#"))
    fstarts = [s for s, _ in funcs]
    fn, cat = collections.Counter(), collections.Counter()
    guest = collections.Counter()  # guest block or function -> samples
    mix = collections.defaultdict(lambda: [0.0] * 5)  # samples-weighted: instructions, reads, writes, fpu, fallbacks
    nblocks = collections.defaultdict(set)
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
        starts, blocks = block_map(os.path.join(os.path.dirname(path), "blocks.txt")) if a.block_map else ([], {})

        def name(rip: int) -> str:
            if exe[0] <= rip < exe[0] + exe[1]:
                i = bisect.bisect_right(addrs, rip - exe[0] + pref) - 1
                return syms[i][1] if i >= 0 else "?exe"
            for b, s, n in mods:
                if b <= rip < b + s:
                    return n
            if not a.block_map:
                return "<JIT>"
            i = bisect.bisect_right(starts, rip) - 1
            if i >= 0 and rip < starts[i] + blocks[starts[i]][0]:
                b = blocks[starts[i]]
                return f"<JIT sh4 {b[1]}>" if isinstance(b[1], str) else f"sh4 {b[1]:08X} {starts[i]:x}"
            return "<JIT other>"

        busy = collections.Counter()
        for t, r, c in rows:
            if name(r).lower() not in SYSTEM:
                busy[t] += c
        tid = busy.most_common(1)[0][0]  # the emulation thread
        for t, r, c in rows:
            if t != tid:
                continue
            f = name(r)
            cat[category(f)] += c
            if not f.startswith("sh4 "):
                fn[f] += c
                continue
            _, ga, host = f.split()
            blk = blocks[int(host, 16)]
            g = int(ga, 16)
            fn["<JIT sh4 blocks>"] += c
            if funcs:
                i = bisect.bisect_right(fstarts, g & 0x1FFFFFFF | 0x0C000000 if g >= 0x0C000000 else g) - 1
                key = f"{funcs[i][0]:08X} {funcs[i][1]}".strip() if i >= 0 else f"? {g:08X}"
            else:
                key = f"{g:08X}"
            guest[key] += c
            nblocks[key].add(g)
            m = mix[key]
            for k, v in enumerate((blk[2], blk[3], blk[4], blk[5], blk[6])):
                m[k] += v * c
    n = sum(cat.values())
    print(f"busiest thread: {n} samples in {len(a.samples)} file(s)")
    for f, c in fn.most_common(a.top):
        print(f"{100 * c / n:5.1f}% {f[:110]}")
    for k, c in cat.most_common():
        print(f"CAT {100 * c / n:5.1f}% {k}")
    if guest:
        sh4 = sum(guest.values())
        print(f"--- guest {'functions' if funcs else 'blocks'}: % of thread, % of sh4 blocks, blocks, "
              "per guest instruction: reads writes fpu fallbacks; guest instructions per block")
        for k, c in guest.most_common(a.top):
            m = mix[k]
            ins = max(m[0], 1)
            print(f"{100 * c / n:5.1f}% {100 * c / sh4:5.1f}% {len(nblocks[k]):4d}  {m[1] / ins:.2f} {m[2] / ins:.2f} "
                  f"{m[3] / ins:.2f} {m[4] / ins:.2f}  {m[0] / c:5.1f}  {k}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
