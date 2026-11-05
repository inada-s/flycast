"""Main-menu features (no ROM, a window): update check, texture pack download, wireless and VPN warnings.
Judged by log lines: a popup waits behind any other open popup, so it may never be drawn."""

import json
import os

from lib import emu, recheck, servers
from lib.case import Skip, case, result
from lib.xxh64 import hexdigest

WAIT = 30
# The wireless warning would hold back the update popup and the texture download (they need no popup open).
WIRED = "gdxsv:ConnectionMedium=Wired"


def main_menu(ctx, name: str, config: list, until: str, routes=None, clean: bool = True) -> dict:
    """Start in the main menu with a local HTTP server (routes: path -> body); stop once `until`
    is in the log, or after WAIT s."""
    with servers.FakeHttp(routes=routes or {}) as http:
        url = f"http://127.0.0.1:{http.port}"
        cfg = ["log:Verbosity=3", "log:LogToFile=1"] + [c.format(url=url) for c in config]
        inst = emu.launch(ctx.exe, "", os.path.join(ctx.out, name), config=cfg, headless=False, clean=clean)
        try:
            inst.wait_log(until, WAIT)
        finally:
            inst.kill()
            inst.proc.wait(10)
            inst.remove_exe()
        requests = list(http.requests)
    return {"inst": inst, "requests": requests, "log": inst.log}


def lines(log: str, text: str) -> list:
    return [l.split(": ", 1)[-1] for l in log.splitlines() if text in l]


# Update check (gdxsv:UpdateCheckUrl)

NEW_TAG = "gdxsv-99.0.0"
# GdxsvUpdate only offers a release with the zip of this platform.
ZIPS = ["flycast-gdxsv-windows.zip", "flycast-gdxsv-macos-x86_64.zip", "flycast-gdxsv-linux-x86_64.zip"]


def check_update(ctx, name: str, tag: str) -> dict:
    assets = [{"name": z, "browser_download_url": f"http://127.0.0.1:1/{z}", "size": 1} for z in ZIPS]
    r = main_menu(ctx, name, [WIRED, "gdxsv:UpdateCheckUrl={url}/latest"], "update check:",
                  {"/latest": json.dumps({"tag_name": tag, "assets": assets}).encode()})
    return {"requests": r["requests"], "line": "".join(lines(r["log"], "update check:")[:1]),
            "errors": r["inst"].errors[:5]}


@case("U1", f"update check: the release API (local) has {NEW_TAG}, offered as newer with its download")
def update_check(ctx):
    r = check_update(ctx, "U1", NEW_TAG)
    return result([{"what": "asked the release API", "ok": "GET /latest" in r["requests"]},
                   {"what": f"{NEW_TAG} newer, download found",
                    "ok": r["line"] == f"update check: latest {NEW_TAG}, newer 1, download 1"},
                   {"what": "no error in the log", "ok": not r["errors"]}], **r)


# Texture pack layers (TextureResolution2D, TextureResolution3D, TexturePackChannel, gdxsv:TexturePackUrl)

CHANNEL = "e2e"
PACK_VERSION = "e2e-1"
PACK_NAMES = ("ex", "2d-1x", "2d-2x", "2d-4x", "3d-2x", "3d-4x")
# Stand-ins for the packs, different from each other: only hashes are checked
PACKS = {name: bytes((i * (7 + n) + i // 251) & 0xff for i in range(100_000)) for n, name in enumerate(PACK_NAMES)}
CHUNKS = 3


def installed_name(pack: str) -> str:
    return f"T13306M-{pack.split('-')[0]}.texp"


def pack_routes(bad_chunk_in: str = "") -> dict:
    """Every pack of the channel; bad_chunk_in: the pack whose second chunk lists a wrong hash."""
    routes = {}
    for name, data in PACKS.items():
        step = -(-len(data) // CHUNKS)
        parts = [data[i:i + step] for i in range(0, len(data), step)]
        chunks = [{"path": f"{name}/c{i}", "size": len(part), "hash": hexdigest(part)} for i, part in enumerate(parts)]
        if name == bad_chunk_in:
            chunks[1]["hash"] = "0" * 16
        info = {"name": f"T13306M-{name}.zip", "version": PACK_VERSION, "xxhash": hexdigest(data), "size": len(data),
                "chunks": chunks}
        routes[f"/{CHANNEL}-{name}.json"] = json.dumps(info).encode()
        routes.update({f"/{name}/c{i}": part for i, part in enumerate(parts)})
    return routes


def asked(requests: list) -> list:
    """The channel files asked, in order."""
    return [name for q in requests for name in PACK_NAMES if f"/{CHANNEL}-{name}.json" in q]


def installed(inst, pack: str) -> bool:
    try:
        with open(os.path.join(inst.data_dir, installed_name(pack)), "rb") as f:
            return f.read() == PACKS[pack]
    except OSError:
        return False


LAYERED = ("ex", "2d-4x", "3d-2x")


def texture_runs(ctx, name: str, bad_chunk_in: str) -> dict:
    """Run 1 from an empty data dir: download. Run 2, same dir: the packs are current, nothing fetched."""
    if recheck.ACTIVE:
        raise Skip("not recheckable: run 2 rewrites run 1's log in the same dir")
    cfg = [WIRED, "gdxsv:TextureResolution2D=4", "gdxsv:TextureResolution3D=2", "gdxsv:language=0",
           f"gdxsv:TexturePackChannel={CHANNEL}", "gdxsv:TexturePackUrl={url}/"]
    routes = pack_routes(bad_chunk_in)
    until = "chunk hash missmatch" if bad_chunk_in else f"texture pack updated to {PACK_VERSION}: {CHANNEL}-3d-2x.json"
    first = main_menu(ctx, name, cfg, until, routes)
    packs = {pack: installed(first["inst"], pack) for pack in LAYERED}
    os.remove(first["inst"].log_path)  # else run 2 matches run 1's lines before flycast rewrites the file
    second = main_menu(ctx, name, cfg, f"texture pack check: {CHANNEL}-3d-2x.json", routes, clean=False)
    return {"first": {"requests": first["requests"], "check": lines(first["log"], "texture pack"),
                      "errors": first["inst"].errors[:5]},
            "installed": packs,
            "second": {"requests": second["requests"], "check": lines(second["log"], "texture pack")}}


def chunks_fetched(requests: list, pack: str) -> int:
    return sum(q.startswith(f"GET /{pack}/c") for q in requests)


def texture_checks(r: dict) -> list:
    first, second = r["first"], r["second"]
    return ([{"what": "asked the ex, 2D 4x and 3D 2x channel files", "ok": asked(first["requests"]) == list(LAYERED)}] +
            [{"what": f"{pack}: {CHUNKS} chunks, updated to {PACK_VERSION}, installed as {installed_name(pack)}",
              "ok": chunks_fetched(first["requests"], pack) == CHUNKS
              and f"texture pack updated to {PACK_VERSION}: {CHANNEL}-{pack}.json" in first["check"]
              and r["installed"][pack]} for pack in LAYERED] +
            [{"what": "second start: every pack current (new 0), no chunk fetched",
              "ok": all(f"texture pack check: {CHANNEL}-{pack}.json, new 0" in second["check"] for pack in LAYERED)
              and not any(chunks_fetched(second["requests"], pack) for pack in LAYERED)}])


@case("X1", f"texture pack: 2D 4x and 3D 2x on a local channel: the ex, 2D and 3D packs downloaded in "
            f"{CHUNKS} chunks each, checked and installed; the next start finds them current")
def texture_pack(ctx):
    r = texture_runs(ctx, "X1", "")
    return result(texture_checks(r) + [{"what": "no error in the log", "ok": not r["first"]["errors"]}], **r)


@case("X2", "texture pack: X1 with one chunk of the 2D pack listing a wrong hash: the 2D pack is not installed")
def texture_pack_bad_hash(ctx):
    r = texture_runs(ctx, "X2", "2d-4x")
    return result([{"what": "asked the 2D 4x channel file", "ok": "2d-4x" in asked(r["first"]["requests"])},
                   {"what": "2D pack not installed", "ok": not r["installed"]["2d-4x"]
                    and f"texture pack updated to {PACK_VERSION}: {CHANNEL}-2d-4x.json" not in r["first"]["check"]}],
                  **r)


def pack_channel_run(ctx, name: str, settings: list, packs: list) -> dict:
    """One start with these settings: the channel files asked must be `packs`, each installed."""
    cfg = [WIRED, f"gdxsv:TexturePackChannel={CHANNEL}", "gdxsv:TexturePackUrl={url}/"] + settings
    r = main_menu(ctx, name, cfg, f"texture pack updated to {PACK_VERSION}: {CHANNEL}-{packs[-1]}.json", pack_routes())
    return result([{"what": "asked only " + ", ".join(packs), "ok": asked(r["requests"]) == packs}] +
                  [{"what": f"{pack} installed as {installed_name(pack)}", "ok": installed(r["inst"], pack)} for pack in packs],
                  requests=r["requests"], check=lines(r["log"], "texture pack"))


@case("X3", "texture pack: 2D 1x and 3D off ask the ex and 2D 1x packs and install both")
def texture_pack_1x(ctx):
    return pack_channel_run(ctx, "X3", ["gdxsv:TextureResolution2D=1", "gdxsv:TextureResolution3D=1", "gdxsv:language=2"],
                            ["ex", "2d-1x"])


@case("X4", "texture pack: 2D 2x and 3D 4x ask the ex, 2D 2x and 3D 4x packs and install them")
def texture_pack_2x_4x(ctx):
    return pack_channel_run(ctx, "X4", ["gdxsv:TextureResolution2D=2", "gdxsv:TextureResolution3D=4", "gdxsv:language=1"],
                            ["ex", "2d-2x", "3d-4x"])


@case("X5", "texture pack: an old language=3 (off) is read as Japanese, so 2D 1x asks the ex and 2D 1x packs")
def texture_pack_language_off_migrated(ctx):
    return pack_channel_run(ctx, "X5", ["gdxsv:TextureResolution2D=1", "gdxsv:TextureResolution3D=1", "gdxsv:language=3"],
                            ["ex", "2d-1x"])


# Wireless warning and VPN toast (gdxsv:ConnectionMedium stands in for the detected adapter)

def warning(ctx, name: str, medium: str) -> dict:
    r = main_menu(ctx, name, [f"gdxsv:ConnectionMedium={medium}"], "warning shown")
    return {"lines": lines(r["log"], "warning shown")}


@case("M1", "connection warnings: Wireless opens the LAN cable popup, 'VPN: e2e' the VPN toast with the adapter")
def connection_warnings(ctx):
    w = warning(ctx, "M1w", "Wireless")
    v = warning(ctx, "M1v", "VPN: e2e")
    return result([{"what": "wireless warning", "ok": w["lines"] == ["wireless warning shown"]},
                   {"what": "VPN toast", "ok": v["lines"] == ["VPN warning shown: VPN: e2e"]}], wireless=w, vpn=v)
