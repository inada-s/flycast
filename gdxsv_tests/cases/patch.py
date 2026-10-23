"""Game memory patches (core/gdxsv/gdxsv.cpp WritePatchDisk1/2), applied every vblank."""

from lib.case import case, result

# (address, width, value before the patch, value written by the patch, what it is)
ANCHORS = {
    "dc1": [
        (0x0c310450, 1, 0, 1, "send key every frame"),
    ],
    "dc2": [
        (0x0c3abb90, 1, 0, 1, "send key every frame"),
    ],
}


def repatched(ctx, disk: str, after: int) -> list:
    """The lobby savestate already holds the patched values: write the original ones back
    and read `after` vblanks later; the emulator must have written the patch again."""
    anchors = ANCHORS[disk]
    p = ctx.probe(disk, ctx.rom(disk), "memread.lua",
                  {"frame": 10, "after": after, "writes": [[a, w, orig] for a, w, orig, _, _ in anchors],
                   "reads": [[a, w] for a, w, _, _, _ in anchors]}, frames=40)
    checks = [{"disk": disk, "what": "probe ran", "ok": p.clean, **p.facts()}]
    for addr, width, _, want, what in anchors:
        got = p.e2e.get(f"0x{addr:08x}:{width}")
        checks.append({"disk": disk, "what": what, "addr": f"0x{addr:08x}", "want": want, "got": got,
                       "ok": got == str(want)})
    return checks


@case("P1", "game patches written every vblank (Disc 1, Disc 2)", tier="smoke")
def patch_applied(ctx):
    return result(repatched(ctx, "dc1", 2) + repatched(ctx, "dc2", 2))


@case("P1c", "P1 read before a vblank passed: the check sees the unpatched values", control_of="P1")
def patch_applied_control(ctx):
    checks = repatched(ctx, "dc2", 0)
    ran = checks[0]["ok"]
    caught = [c["what"] for c in checks[1:] if not c["ok"]]
    return result([{"what": "probe ran", "ok": ran},
                   {"what": "every anchor fails the check", "ok": len(caught) == len(checks) - 1}],
                  detail=checks)
