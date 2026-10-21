"""Game font glyphs drawn from the bundled outline font (gdxsv:FontRaster), judged by a cropped golden
image of the battle HUD's pilot name in a frame-exact grab."""

import os
import re

from lib import assets, emu, image, recheck
from lib.case import Skip, case, result, write_lua
from cases.replay import e2e_values, wait_for

REPLAY = "1788627530784"
HOLD = 3600  # a battle vblank of REPLAY: the HUD shows the pilot name "DRIQ"
SIZE = (960, 720)
NAME_BOX = (78, 98, 200, 128)  # client coordinates in a 960x720 window
NAME_GOLDEN = "fontraster_hud_name.png"
# Measured on windows-mini at vblanks 3600 and 4800: 1.00 with FontRaster, 0.11 with the game's glyphs.
DRAWN = 0.8
LOADED_RE = re.compile(r"FontRaster: .+ loaded in \d+ us, (\d+) glyph textures")


def held_grab(ctx, name: str, font_raster: bool) -> dict:
    """REPLAY in a SIZE window, its client area grabbed while lua/hold.lua stalls it at vblank HOLD."""
    d = os.path.join(ctx.out, name)
    shot = os.path.join(d, "shot.png")
    os.makedirs(ctx.out, exist_ok=True)
    lua = write_lua(os.path.join(ctx.out, name + ".lua"), "hold.lua", {"frame": HOLD})
    entry = next(e for e in assets.corpus() if e["id"] == REPLAY)
    cfg = [f"gdxsv:replay={assets.replay(entry)}", "gdxsv:ReplaySeekStates=no", "gdxsv:ConnectionMedium=Wired",
           f"gdxsv:FontRaster={'yes' if font_raster else 'no'}", f"window:width={SIZE[0]}", f"window:height={SIZE[1]}"]
    inst = emu.launch(ctx.exe, ctx.rom("dc2"), d, config=cfg, headless=False, lua=lua,
                      emu_cfg="[log]\nLogToFile = 1\nVerbosity = 3\n")
    if not recheck.ACTIVE:
        try:
            if wait_for(lambda: "hold" in e2e_values(inst), 150):
                with image.SCREEN_LOCK:
                    image.grab(inst.proc.pid, shot)
                open(os.path.join(d, "go"), "w").close()
        except NotImplementedError as e:
            raise Skip(str(e))
        finally:
            inst.kill()
            inst.proc.wait(10)
            inst.remove_exe()
    loaded = LOADED_RE.search(inst.log)
    score = round(image.recall(NAME_GOLDEN, image.white_mask(shot, NAME_BOX, SIZE)), 2) if os.path.isfile(shot) else None
    return {"held": e2e_values(inst).get("hold"), "glyphs": int(loaded.group(1)) if loaded else None, "score": score}


def checks(r: dict) -> list:
    return [{"what": f"replay held at vblank {HOLD} and grabbed", "held": r["held"], "ok": r["held"] == str(HOLD)},
            {"what": "the font is loaded with its glyph table", "glyphs": r["glyphs"], "ok": (r["glyphs"] or 0) > 0},
            {"what": f"the HUD pilot name matches {NAME_GOLDEN}", "recall": r["score"], "ok": (r["score"] or 0) >= DRAWN}]


@case("T1", "outline font glyphs: the battle HUD pilot name is drawn from the bundled font (FontRaster)")
def font_raster(ctx):
    return result(checks(held_grab(ctx, "T1", True)))


@case("T1c", "T1 with gdxsv:FontRaster=no: no font loaded, the name keeps the game's glyphs", control_of="T1")
def font_raster_control(ctx):
    r = held_grab(ctx, "T1c", False)
    held, loaded, drawn = checks(r)
    return result([held, {"what": "font load check fails", "ok": not loaded["ok"]},
                   {"what": "name check fails", "ok": not drawn["ok"]}], detail=r)
