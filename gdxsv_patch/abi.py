"""The boundary between the game and the gdxsv payload.

Savestates keep every address the game receives from the payload: hooks
written into game memory and copies the game makes of them (HUD objects copy
their renderer). Only the addresses declared here may be handed to the game;
everything else in the payload is free to move between builds.

Rules, checked by gen_abi.py against abi_released.json:
- Never move or remove a released ENTRIES or PINNED item. Add new ones.
- An entry keeps its meaning and calling convention forever.

A hook target is "entry:<name>" (a slot in ENTRIES) or a PINNED symbol.
"""
from typing import List, NamedTuple, Optional, Tuple

# Fixed jump slots in gdx.func.entry: mov.l target,<reg> / jmp @<reg> / nop.
ENTRY_BASE = 0x0C4F0590
ENTRY_LIMIT = 0x0C4F0800
ENTRY_SIZE = 12


class Entry(NamedTuple):
    name: str  # symbol gdx_entry_<name>
    target: str  # payload function the slot jumps to
    reg: str  # scratch register; must be dead at every caller
    addr: int


ENTRIES = [
    # gdxsv-1.8.10..1.9.2 Disc-2 widescreen entries, at their released addresses.
    Entry("ws_transition_matte", "gdx_widescreen_transition_matte", "r0", 0x0C4F0590),
    Entry("ws_fade_submit", "gdx_widescreen_fade_submit", "r0", 0x0C4F059C),
    Entry("ws_hud_render", "gdx_widescreen_hud_render", "r0", 0x0C4F0640),
    # r0 is live at the result-screen hook site.
    Entry("ws_result", "gdx_widescreen_result_black_postproject", "r2", 0x0C4F06F4),
    Entry("player_info32_request", "gdx_player_info32_request", "r0", 0x0C4F05A8),
    Entry("win_lose32_request", "gdx_win_lose32_request", "r0", 0x0C4F05B4),
    Entry("stats_poll", "gdx_stats_poll", "r0", 0x0C4F05C0),
    Entry("player_info32_draw", "gdx_player_info32_draw", "r0", 0x0C4F05CC),
    Entry("win_lose32_draw", "gdx_win_lose32_draw", "r0", 0x0C4F05D8),
]

# Symbols the game or the host reaches at a fixed address.
PINNED = {
    # Networking. gdx_initialize() hands these to the game, and the
    # distributed slot-99 savestates hold them.
    "gdx_sock_create": 0x0C4F0000,
    "gdx_sock_close": 0x0C4F0004,
    "gdx_gethostbyname": 0x0C4F003C,
    "gdx_connect_sock": 0x0C4F0068,
    "gdx_select": 0x0C4F00B4,
    "gdx_lbs_sock_write": 0x0C4F00F4,
    "gdx_lbs_sock_read": 0x0C4F0120,
    "gdx_mcs_sock_read": 0x0C4F015C,
    "gdx_mcs_sock_write": 0x0C4F0168,
    "gdx_softreset_disconnect": 0x0C4F0194,
    "gdx_ppp_get_status": 0x0C4F0218,
    "gdx_dial_start_disk1": 0x0C4F0540,
    "gdx_dial_start_disk2": 0x0C4F0568,
    # A payload waiting to be replaced still talks through its own gdx_rpc,
    # and the host compares patch_id before replacing it.
    "gdx_rpc": 0x0C4E0200,
    "patch_id": 0x0C4E0258,
    # The game holds the HUD table address; HUD objects copy its entries.
    "gdx_widescreen_hud_table_disk1": 0x0C4E0300,
    "gdx_widescreen_hud_table_disk2": 0x0C4E0354,
}


class Hook(NamedTuple):
    """A 32-bit game cell holding a payload address."""
    feature: str
    disk: int
    addr: int
    stock: Optional[int]  # None: always overwritten
    target: str
    legacy: Tuple[int, ...] = ()  # released values relocated to target
    repair_only: bool = False  # only relocate legacy values; the guest installs it


class Code(NamedTuple):
    """16-bit game instructions replaced while a feature is on."""
    feature: str
    disk: int
    addr: int
    stock: Tuple[int, ...]
    patched: Tuple[int, ...]


class Value(NamedTuple):
    """A 32-bit game literal the host sets per aspect ratio."""
    feature: str
    disk: int
    kind: str
    addr: int
    stock: int


# Features installed with the payload. The host installs "widescreen".
PAYLOAD_FEATURES = ("dial", "stats")

STATS_1_8_12 = {"player_info32_request": 0x0C4F1080, "win_lose32_request": 0x0C4F1090,
                "stats_poll": 0x0C4F10A0, "player_info32_draw": 0x0C4F0C88,
                "win_lose32_draw": 0x0C4F0BE8}
STATS_1_8_13 = {"player_info32_request": 0x0C4F1184, "win_lose32_request": 0x0C4F1194,
                "stats_poll": 0x0C4F11A4, "player_info32_draw": 0x0C4F0FF4,
                "win_lose32_draw": 0x0C4F0BF0}  # through 1.9.2


def _stats(addr, stock, name, repair_only=False):
    return Hook("stats", 2, addr, stock, "entry:" + name,
                (STATS_1_8_12[name], STATS_1_8_13[name]), repair_only)


HOOKS: List[Hook] = [
    Hook("dial", 1, 0x8C181BB4, None, "gdx_dial_start_disk1"),
    Hook("dial", 2, 0x8C1E0274, None, "gdx_dial_start_disk2"),
    # Installed together, only on the known ROM layout. The guest stats
    # initialization installs the draw hooks over the stock text renderer.
    _stats(0x0C030B04, 0x0C036094, "player_info32_request"),
    _stats(0x0C02CF44, 0x0C034E9C, "win_lose32_request"),
    _stats(0x0C03224C, 0x0C034E9C, "win_lose32_request"),
    _stats(0x0C030AEC, 0x0C033E20, "stats_poll"),
    _stats(0x0C02CCBC, 0x0C033E20, "stats_poll"),
    _stats(0x0C03221C, 0x0C033E20, "stats_poll"),
    _stats(0x0C03E454, 0x0C02404C, "player_info32_draw", repair_only=True),
    _stats(0x0C03EC40, 0x0C02404C, "player_info32_draw", repair_only=True),
    _stats(0x0C041F8C, 0x0C02404C, "win_lose32_draw", repair_only=True),
    Hook("widescreen", 1, 0x0C019FAC, 0x0C17EA78, "gdx_widescreen_hud_table_disk1"),
    Hook("widescreen", 1, 0x0C135E18, 0x0C135E20, "entry:ws_transition_matte"),
    Hook("widescreen", 1, 0x0C05CB50, 0x0C13EE50, "entry:ws_fade_submit"),
    Hook("widescreen", 1, 0x0C15EA04, 0x2602FAE2, "entry:ws_result"),
    Hook("widescreen", 2, 0x0C1196F0, 0x0C2403A4, "gdx_widescreen_hud_table_disk2", (0x0C4F06B0,)),
    Hook("widescreen", 2, 0x0C1955AC, 0x0C1955B4, "entry:ws_transition_matte"),
    Hook("widescreen", 2, 0x0C049F70, 0x0C19E630, "entry:ws_fade_submit"),
    Hook("widescreen", 2, 0x0C1BE1E4, 0x2602FAE2, "entry:ws_result"),
]

# Both discs share the code at every widescreen site.
_CULL = ((0x8FC9,), (0x8DBC,), (0x8FAF,), (0x8DA2,))
_RESULT_STOCK = (0xF572, 0xF66B, 0x7540, 0xF64B)
_RESULT_HOOK = (0xD201, 0x422B, 0x0009, 0x0009)  # mov.l @(8,pc),r2 / jmp @r2 / nop / pad

CODE: List[Code] = [
    # MS and building horizontal culling branches.
    *(Code("widescreen", 1, a, s, (0x0009,)) for a, s in zip((0x0C14F40A, 0x0C14F424, 0x0C14BA46, 0x0C14BA60), _CULL)),
    *(Code("widescreen", 2, a, s, (0x0009,)) for a, s in zip((0x0C1AEBEA, 0x0C1AEC04, 0x0C1AB226, 0x0C1AB240), _CULL)),
    # Result-screen post-projection; the literal after it is a widescreen Hook.
    Code("widescreen", 1, 0x0C15E9FC, _RESULT_STOCK, _RESULT_HOOK),
    Code("widescreen", 2, 0x0C1BE1DC, _RESULT_STOCK, _RESULT_HOOK),
]

_F0, _F640, _FM5, _F645 = 0x00000000, 0x44200000, 0xC0A00000, 0x44214000
# Transition and fade quads: two left and two right X coordinates each.
_MATTES = {
    1: [((0x0C16E1D4, 0x0C16E1E4, 0x0C16E1F4, 0x0C16E204), 0),  # menu background gray fade
        ((0x0C16E214, 0x0C16E224, 0x0C16E234, 0x0C16E244), 0),  # full-screen fade
        ((0x0C173AC4, 0x0C173AD4, 0x0C173AE4, 0x0C173AF4), 5),  # opening fade
        ((0x0C173B04, 0x0C173B14, 0x0C173B24, 0x0C173B34), 5),  # centre-field fade
        ((0x0C173D48, 0x0C173D58, 0x0C173D68, 0x0C173D78), 0),  # title-logo mask
        ((0x0C173B44, 0x0C173B54, 0x0C173B64, 0x0C173B74), 5),  # upper cinema bar
        ((0x0C173B84, 0x0C173B94, 0x0C173BA4, 0x0C173BB4), 5),  # lower cinema bar
        ((0x0C1811FC, 0x0C18121C, 0x0C18120C, 0x0C18122C), 0),  # pause overlay
        ((0x0C181AF0, 0x0C181B10, 0x0C181B00, 0x0C181B20), 0)],  # network return-to-lobby mask
    2: [((0x0C1CE334, 0x0C1CE344, 0x0C1CE354, 0x0C1CE364), 0),
        ((0x0C1CE374, 0x0C1CE384, 0x0C1CE394, 0x0C1CE3A4), 0),
        ((0x0C1D3B88, 0x0C1D3B98, 0x0C1D3BA8, 0x0C1D3BB8), 5),
        ((0x0C1D3BC8, 0x0C1D3BD8, 0x0C1D3BE8, 0x0C1D3BF8), 5),
        ((0x0C1D3E78, 0x0C1D3E88, 0x0C1D3E98, 0x0C1D3EA8), 0),
        ((0x0C1D3C60, 0x0C1D3C70, 0x0C1D3C80, 0x0C1D3C90), 5),
        ((0x0C1D3CA0, 0x0C1D3CB0, 0x0C1D3CC0, 0x0C1D3CD0), 5),
        ((0x0C1DEBA8, 0x0C1DEBC8, 0x0C1DEBB8, 0x0C1DEBD8), 0),
        ((0x0C1E01B0, 0x0C1E01D0, 0x0C1E01C0, 0x0C1E01E0), 0)],
}


def _mattes():
    for disk, quads in _MATTES.items():
        for (l0, l1, r0, r1), margin in quads:
            left, right = (_FM5, _F645) if margin else (_F0, _F640)
            for a in (l0, l1):
                yield Value("widescreen", disk, "matte_left", a, left)
            for a in (r0, r1):
                yield Value("widescreen", disk, "matte_right", a, right)


VALUES: List[Value] = [
    *_mattes(),
    Value("widescreen", 1, "frustum_left", 0x0C067DBC, 0xBDCCCCCD),
    Value("widescreen", 1, "frustum_right", 0x0C067DC0, 0x3DCCCCCD),
    Value("widescreen", 1, "info_panel_x", 0x0C020378, 0xBD3B2FEC),
    Value("widescreen", 2, "frustum_left", 0x0C055280, 0xBDCCCCCD),
    Value("widescreen", 2, "frustum_right", 0x0C055284, 0x3DCCCCCD),
    Value("widescreen", 2, "info_panel_x", 0x0C1213CC, 0xBD3B2FEC),
]
