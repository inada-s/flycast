"""Translation patch, widescreen HUD (judged by RAM)."""

import os
import re
import struct

from lib.case import case, result

# gdxsv:language (core/gdxsv/gdxsv_translation.h GdxsvLanguage) and the first translated
# string (0x8c010000 + game offset 0x1ba264) as each language leaves it. Japanese is the
# game's own text, the same as disabled.
TEXT = 0x8c1ca264
LANG_TEXT = {
    "japanese": (0, "8349815b8367838d815b8368928682c5"),
    "cantonese": (1, "90b38ddde6a48ee691b69e8381458145"),
    "english": (2, "4c6f6164696e6720736176652066696c"),  # "Loading save fil"
    "disabled": (3, "8349815b8367838d815b8368928682c5"),
}


def lang_text(ctx, lang: str, want: str) -> list:
    p = ctx.probe(lang, ctx.rom("dc2"), "memread.lua", {"frame": 30, "reads": [[TEXT, 16]]}, frames=50,
                  config=[f"gdxsv:language={LANG_TEXT[lang][0]}"])
    got = p.e2e.get(f"0x{TEXT:08x}:16")
    return [{"language": lang, "what": "probe ran", "ok": p.clean, **p.facts()},
            {"language": lang, "want": want, "got": got, "ok": got == LANG_TEXT[want][1]}]


@case("L1", "translation patch: Disc 2 text per language", tier="smoke")
def translation(ctx):
    return result(sum((lang_text(ctx, lang, lang) for lang in LANG_TEXT), []))


# Game offset 0x1ba128: the game's セーブを行うには, which every language, Japanese included, patches to "Flycast > Settings".
PATCHED_TEXT = 0x8c1ca128
FLYCAST_SETTINGS = "466c7963617374203e2053657474696e"  # "Flycast > Settin"


@case("L2", "language: an old language=3 (off) is read as Japanese, so the game gets the Japanese patch")
def language_off_migrated(ctx):
    p = ctx.probe("L2", ctx.rom("dc2"), "memread.lua", {"frame": 30, "reads": [[PATCHED_TEXT, 16]]}, frames=50,
                  config=["gdxsv:language=3"])
    got = p.e2e.get(f"0x{PATCHED_TEXT:08x}:16")
    return result([{"what": "probe ran", "ok": p.clean, **p.facts()},
                   {"what": "Japanese patch applied", "want": FLYCAST_SETTINGS, "got": got, "ok": got == FLYCAST_SETTINGS}])


SYMBOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                       "core", "gdxsv", "gdxsv_patch.inc")
WS_SYMBOLS = ["gdx_widescreen_transition_left_x", "gdx_widescreen_transition_right_x",
              "gdx_widescreen_hud_right_offset"]


def symbol(name: str) -> int:
    with open(SYMBOLS, encoding="utf-8") as f:
        m = re.search(r'symbols_\["%s"\] = (0x[0-9a-f]+);' % re.escape(name), f.read())
    return int(m.group(1), 16)


def ws_expected(wide: bool, super_wide: bool, layout: int, w: int, h: int) -> list:
    """Gdxsv::WriteWidescreenPatch: left/right edge of the transition matte, HUD offset."""
    f32 = lambda x: struct.unpack("<f", struct.pack("<f", x))[0]  # noqa: E731
    stock, view = 4 / 3, f32(w / h)
    aspect = hud = stock
    if wide:
        aspect = view if super_wide else 16 / 9
        if layout == 1:
            hud = max(stock, min(view, 16 / 9))
        elif layout == 2:
            hud = max(stock, min(view, aspect))
    scale = aspect / stock
    return [320 - 325 * scale if wide else 0.0, 320 + 325 * scale if wide else 640.0, 0.075 * (hud - stock)]


# (name, widescreen, super widescreen, HUD layout, viewport)
WS_CASES = [
    ("off", False, False, 1, 854, 480),
    ("layout0_16x9", True, False, 0, 854, 480),
    ("layout1_16x9", True, False, 1, 854, 480),
    ("layout2_16x9", True, False, 2, 854, 480),
    ("layout1_4x3", True, False, 1, 640, 480),
    ("super_layout2_21x9", True, True, 2, 1120, 480),
]


def ws_run(ctx, name, wide, super_wide, layout, w, h, expect) -> list:
    yn = lambda b: "yes" if b else "no"  # noqa: E731
    addrs = [symbol(s) for s in WS_SYMBOLS]
    p = ctx.probe(name, ctx.rom("dc2"), "memread.lua", {"frame": 30, "reads": [[a, 4] for a in addrs]}, frames=50,
                  config=[f"config:rend.WideScreen={yn(wide)}", f"config:rend.WidescreenGameHacks={yn(wide)}",
                          f"config:rend.SuperWideScreen={yn(super_wide)}", f"gdxsv:WidescreenHudLayout={layout}",
                          f"gdxsv:headless_width={w}", f"gdxsv:headless_height={h}"])
    got = []
    for a in addrs:
        v = p.e2e.get(f"0x{a:08x}:4")
        got.append(struct.unpack("<f", struct.pack("<I", int(v)))[0] if v else None)
    ok = all(g is not None and abs(g - e) < 1e-3 for g, e in zip(got, expect))
    return [{"case": name, "what": "probe ran", "ok": p.clean, **p.facts()},
            {"case": name, "want": [round(x, 4) for x in expect],
             "got": [g if g is None else round(g, 4) for g in got], "ok": ok}]


@case("W1", "widescreen HUD layout: matte edges and HUD offset per layout and viewport")
def widescreen(ctx):
    return result(sum((ws_run(ctx, *c, expect=ws_expected(*c[1:])) for c in WS_CASES), []))
