"""Online features against a local gdxsv lobby (lib/servers.py)."""

import json
import os
import re
import shutil
import threading
import time

from lib import assets, servers
from lib.case import Skip, case, result

LOGIN_KEY = "E2ETEST1"
USER = ("E2E001", "e2e")  # user id, handle name the lobby lists for LOGIN_KEY
LOGIN_KEY2 = "E2ETEST3"  # the second client of the two-player cases
USER2 = ("E2E002", "e2e2")
BOOT = 300  # vblanks of cold boot before the network menu savestate is loaded
LOGIN_FRAMES = 4400  # boot, the menu path in lua/login.lua, and the lobby's plaza messages
BATTLE_FRAMES = 40000  # upper bound; the script ends 300 vblanks into the battle


def login(ctx, tag: str, client_key: str, battle: bool = False, force_start: bool = True, mcs_region: str = "",
          unpatch: bool = False, **net):
    """Disc 2 logs in from the network menu savestate; returns (probe, lobby log, fake HTTP requests).
    net: session()'s server, udp_hello, public_ip."""
    probes, lbs, reqs = session(ctx, tag, [(tag, client_key, {"battle": battle, "unpatch": unpatch})],
                                force_start=force_start, mcs_region=mcs_region, **net)
    return probes[0], lbs, reqs


def session(ctx, tag: str, clients: list, force_start: bool = True, mcs_region: str = "",
            server: str = "127.0.0.1", udp_hello: bool = False, public_ip: str = "127.0.0.1",
            replay_state: bool = False, during=None):
    """clients = (name, gdxsv:loginkey, lua/login.lua params[, Ctx.probe options]) run at once
    against one lobby; returns ([probe per client], lobby log, fake HTTP requests).
    server: gdxsv:server; udp_hello: the fake P2P test answers over UDP (lib/servers.FakeHttp);
    public_ip: the fake public IP answer; replay_state: the replay boot savestate (slot 99) too;
    during: called in a thread with the clients' directories once they are started."""
    rom = ctx.rom("dc2")
    exe = servers.server_exe()
    if exe is None:
        raise Skip("no gdxsv server build (GDXSV_SERVER_EXE)")
    states = assets.login_state_dir()
    if states is None:
        raise Skip("no network menu savestate (GDXSV_LOGIN_STATE)")
    if replay_state and states:
        both = states + "_replay"
        os.makedirs(both, exist_ok=True)
        for d in (states, assets.state_dir(rom)):
            for name in os.listdir(d):
                shutil.copy(os.path.join(d, name), both)
        states = both
    lobby = servers.Lobby(exe, os.path.join(ctx.out, tag + "_lbs"))
    lobby.init([(servers.server_login_key(LOGIN_KEY),) + USER, (servers.server_login_key(LOGIN_KEY2),) + USER2])
    servers.add_training_lobby(lobby.db, force_start=force_start, mcs_region=mcs_region)
    probes = [None] * len(clients)
    with servers.FakeHttp(body=public_ip.encode(), udp_hello=udp_hello) as http:
        def run(i, name, key, params, opts=None):
            battle = params.get("battle", False)
            cfg = http.config() + [f"gdxsv:server={server}", f"gdxsv:loginkey={key}",
                                   "config:rend.ThreadedRendering=no", "config:aica.Volume=0"]
            kw = {"frames": BATTLE_FRAMES if battle else LOGIN_FRAMES, "until_done": battle, **(opts or {})}
            probes[i] = ctx.probe(name, rom, "login.lua", {"boot": BOOT, "slot": assets.LOGIN_SLOT, **params},
                                  config=cfg, loadstate=False, states=states, timeout=900, **kw)
        lobby.start()
        try:
            threads = [threading.Thread(target=run, args=(i,) + tuple(c)) for i, c in enumerate(clients)]
            for t in threads:
                t.start()
            if during:
                threading.Thread(target=during, args=([os.path.join(ctx.out, c[0]) for c in clients],),
                                 daemon=True).start()
            for t in threads:
                t.join()
        finally:
            lobby.stop()
        return probes, lobby.log(), list(http.requests)


def checks(p, lbs: str, requests: list) -> list:
    lines = lbs.splitlines()
    return [
        {"what": "probe ran", "ok": p.clean, **p.facts()},
        {"what": "lobby got the platform info",
         "ok": "Command: lbsPlatformInfo, Direction: ClientToServer" in lbs and '"cpu": "x86/64"' in lbs},
        # The account is found by the hash of the login key: the seeded user is listed and chosen.
        {"what": "login key's user logged in",
         "ok": any("login ok" in l and f'"user_id": "{USER[0]}"' in l for l in lines)},
        {"what": "plaza joined", "ok": "Command: lbsPlazaJoin, Direction: ClientToServer" in lbs},
        {"what": "public IP asked of the local server", "ok": "GET /ip" in requests},
        {"what": "P2P test asked of the local server", "ok": "POST /udptest" in requests},
    ]


@case("O1", "lobby connection, login key, platform info, plaza, P2P port test (local gdxsv lbs)")
def online_login(ctx):
    p, lbs, reqs = login(ctx, "O1", LOGIN_KEY, udp_hello=True)
    return result(checks(p, lbs, reqs) + net_checks(p, lbs), requests=sorted(set(reqs)))


@case("O1c", "O1 with a login key the lobby does not know: no user to log in", control_of="O1")
def online_login_control(ctx):
    p, lbs, reqs = login(ctx, "O1c", "E2ETEST2")
    c = checks(p, lbs, reqs)
    return result([{"what": "probe ran", "ok": c[0]["ok"]},
                   {"what": "platform info still sent", "ok": c[1]["ok"]},
                   {"what": "login check fails", "ok": not c[2]["ok"]}],
                  detail=c)


NO_LOBBY = "192.0.2.1"  # TEST-NET-1: nothing answers, the connect times out (5 s)


@case("O1t", "O1 with gdxsv:server at an address without a lobby: no TCP connection, nothing logged in",
      control_of="O1")
def online_tcp_control(ctx):
    p, lbs, reqs = login(ctx, "O1t", LOGIN_KEY, server=NO_LOBBY)
    c = checks(p, lbs, reqs)
    return result([{"what": "probe ran", "ok": c[0]["ok"]},
                   {"what": f"client connected to {NO_LOBBY}", "ok": f"TCP Connect: {NO_LOBBY}:" in p.log},
                   {"what": "no client in the lobby log", "ok": "update session id" not in lbs},
                   {"what": "platform info check fails", "ok": not c[1]["ok"]},
                   {"what": "login check fails", "ok": not c[2]["ok"]}],
                  detail=c)


OTHER_IP = "203.0.113.7"  # TEST-NET-3


@case("O1i", "O1 with another public IP answer: the platform info carries it, O1's IP check fails",
      control_of="O1")
def online_platform_control(ctx):
    p, lbs, reqs = login(ctx, "O1i", LOGIN_KEY, public_ip=OTHER_IP)
    c = net_checks(p, lbs)
    return result([{"what": "probe ran", "ok": p.clean, **p.facts()},
                   {"what": "public IP check fails", "ok": not c[1]["ok"]},
                   {"what": f"platform info public_ipv4 = {OTHER_IP}", "ok": c[1]["got"] == OTHER_IP}],
                  detail=c)


@case("O1p", "O1 with a P2P test server that sends no UDP Hello: port test failed", control_of="O1")
def online_p2p_control(ctx):
    p, lbs, reqs = login(ctx, "O1p", LOGIN_KEY)
    c = checks(p, lbs, reqs) + net_checks(p, lbs)
    return result([{"what": "probe ran", "ok": c[0]["ok"]},
                   {"what": "logged in", "ok": c[2]["ok"]},
                   {"what": "P2P test still asked", "ok": c[5]["ok"]},
                   {"what": "port test check fails", "ok": not c[-1]["ok"]},
                   {"what": "port_test_v4 = Failed (Timeout)", "ok": c[-1]["got"] == "Failed (Timeout)"}],
                  detail=c)


def platform_info(lbs: str) -> dict:
    """The platform info the lobby logged with the login (LoginUser line)."""
    for l in lbs.splitlines():
        m = re.search(r'"platform_info": (\{.*?\})', l)
        if "LoginUser" in l and m:
            return json.loads(m.group(1))
    return {}


def net_checks(p, lbs: str) -> list:
    """Platform info fields against what this run measured or served."""
    info = platform_info(lbs)
    patch = PATCH_RE.findall(p.log)
    return [
        {"what": "platform info: payload patch_id", "got": info.get("patch_id"), "want": patch[-1:],
         "ok": bool(patch) and info.get("patch_id") == patch[-1]},
        {"what": "platform info: public IP from the local server", "got": info.get("public_ipv4"),
         "ok": info.get("public_ipv4") == "127.0.0.1"},
        {"what": "P2P port test: UDP Hello received", "got": info.get("port_test_v4"),
         "ok": info.get("port_test_v4") == "Success"},
    ]

PATCH_RE = re.compile(r"N\[COMMON\]: patch -?\d+ (\d+)$", re.M)


def payload_checks(p, lbs: str, requests: list) -> list:
    """The network payload (gdxsv_patch/) is installed after the state load and is what the login runs on."""
    ids = PATCH_RE.findall(p.log)
    want = ids[-1] if ids else None
    return [
        {"what": "payload installed (log: patch <in RAM> <current id>)", "ok": want is not None, "ids": ids},
        {"what": "current patch_id in RAM at the network menu", "got": p.e2e.get("patch_id_menu"), "want": want,
         "ok": want is not None and p.e2e.get("patch_id_menu") == want},
        {"what": "lobby connection not refused", "ok": "Lobby connection refused" not in p.log},
    ] + checks(p, lbs, requests)


@case("O5", "network patch: the SH4 payload is installed and the login runs through it (local gdxsv lbs)")
def online_payload(ctx):
    p, lbs, reqs = login(ctx, "O5", LOGIN_KEY)
    return result(payload_checks(p, lbs, reqs))


@case("O5c", "O5 with the payload's patch_id zeroed every vblank: no current payload, no login", control_of="O5")
def online_payload_control(ctx):
    p, lbs, reqs = login(ctx, "O5c", LOGIN_KEY, unpatch=True)
    c = payload_checks(p, lbs, reqs)
    return result([{"what": "probe ran", "ok": c[3]["ok"]},
                   {"what": "patch_id check fails", "ok": not c[1]["ok"]},
                   {"what": "lobby connection refused", "ok": not c[2]["ok"]},
                   {"what": "login check fails", "ok": not c[5]["ok"]}],
                  detail=c)


@case("O2", "lobby to battle: training lobby entry, force start, mobile suit select, battle (local gdxsv)")
def online_battle(ctx):
    p, lbs, _ = login(ctx, "O2", LOGIN_KEY, battle=True)
    return result(battle_checks(p, lbs, rollback=False))


@case("O2c", "O2 in a lobby without force start: no battle", control_of="O2")
def online_battle_control(ctx):
    p, lbs, _ = login(ctx, "O2c", LOGIN_KEY, battle=True, force_start=False)
    c = battle_checks(p, lbs, rollback=False)
    return result([{"what": "probe ran", "ok": c[0]["ok"]},
                   {"what": "battle checks fail", "ok": not any(x["ok"] for x in c[1:])}], detail=c)


@case("O3", "lobby to a rollback battle: P2P lobby, one player (local gdxsv)")
def online_rollback(ctx):
    p, lbs, _ = login(ctx, "O3", LOGIN_KEY, battle=True, mcs_region="p2p")
    return result(battle_checks(p, lbs, rollback=True))


@case("O3c", "O3's checks on a battle-server lobby: battle, but no rollback", control_of="O3")
def online_rollback_control(ctx):
    p, lbs, _ = login(ctx, "O3c", LOGIN_KEY, battle=True)
    c = battle_checks(p, lbs, rollback=True)
    return result([{"what": "battle reached", "ok": c[0]["ok"] and c[1]["ok"]},
                   {"what": "rollback checks fail", "ok": not any(x["ok"] for x in c[2:])}], detail=c)


def battle_checks(p, lbs: str, rollback: bool) -> list:
    log = p.log
    c = [{"what": "probe ran", "ok": p.clean, **p.facts()},
         {"what": "battle scene reached", "ok": p.e2e.get("battle") == "1"}]
    if rollback:
        return c + [
            {"what": "lobby sent the P2P matching", "ok": "Command: lbsP2PMatching, Direction: ServerToClient" in lbs},
            {"what": "GGPO session started", "ok": "StartGGPOSession" in log and "Peer count 1" in log},
            {"what": "battle loaded on the rollback backend", "ok": "LoadEndMsg Join" in log},
        ]
    return c + [
        {"what": "lobby gave the battle server", "ok": "Command: lbsAskMcsAddress, Direction: ClientToServer" in lbs},
        {"what": "battle server room joined over UDP", "ok": "new McsRoom" in lbs and "join udp peer" in lbs},
        {"what": "UDP backend logged in to the battle",
         "ok": "gdxsv_backend_udp" in log and f"user_id:{USER[0]}" in log},
    ]


def pair(ctx, tag: str, second_enters: bool = True):
    """Two clients, two users, one P2P lobby: the first one waits for the second's entry, then
    force-starts. second_enters=False: the second only logs in."""
    first = {"battle": True, "chat_delay": 900, "in_battle": 600}
    second = {"battle": True, "chat": False, "in_battle": 600} if second_enters else {}
    return session(ctx, tag, [(tag + "a", LOGIN_KEY, first), (tag + "b", LOGIN_KEY2, second)], mcs_region="p2p")


def pair_checks(probes, lbs: str) -> list:
    c = [{"what": "lobby sent the P2P matching to both",
          "ok": lbs.count("Command: lbsP2PMatching, Direction: ServerToClient") >= 2}]
    for name, p in zip("ab", probes):
        log = p.log
        c += [
            {"what": f"{name}: probe ran", "ok": p.clean, **p.facts()},
            {"what": f"{name}: battle scene reached", "ok": p.e2e.get("battle") == "1"},
            {"what": f"{name}: GGPO session of two peers", "ok": "StartGGPOSession" in log and "Peer count 2" in log},
            {"what": f"{name}: the other peer answered", "ok": re.search(r"Peer\d [\d.]+ms IP:", log) is not None},
            {"what": f"{name}: battle loaded on the rollback backend", "ok": "LoadEndMsg" in log},
        ]
    return c


@case("O4", "two clients, P2P lobby: a rollback battle between two real peers (local gdxsv)")
def online_rollback_pair(ctx):
    probes, lbs, _ = pair(ctx, "O4")
    return result(pair_checks(probes, lbs))


@case("O4c", "O4 with the second client not entering: a one-peer battle, the pair checks fail", control_of="O4")
def online_rollback_pair_control(ctx):
    probes, lbs, _ = pair(ctx, "O4c", second_enters=False)
    c = pair_checks([probes[0], probes[0]], lbs)
    a = {x["what"]: x["ok"] for x in c}
    return result([{"what": "a: battle reached", "ok": a["a: probe ran"] and a["a: battle scene reached"]},
                   {"what": "two-peer checks fail", "ok": not a["lobby sent the P2P matching to both"]
                    and not a["a: GGPO session of two peers"] and not a["a: the other peer answered"]}], detail=c)


# O6: a's flycast.log lines (gdxsv.cpp, gdxsv_backend_replay.cpp, gdxsv_replay_util.cpp)
TEST_STARTED = "test: starting replay"
SUSPENDED = "lobby: suspended for replay playback"
BACK = "lobby: back to the lobby from replay playback"
RESUMED = re.compile(r"lobby: resumed with (\d+) held messages")
REPLAY_SECONDS = 15  # a plays this long before b starts the match


def read_log(d: str) -> str:
    try:
        with open(os.path.join(d, "flycast.log"), encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def lobby_replay(ctx, tag: str, second_enters: bool = True):
    """a enters the P2P lobby's match, then starts a corpus replay from the lobby (GDXSV_TEST_START_REPLAY,
    the browser's start). b enters REPLAY_SECONDS into the playback and force-starts; a's playback
    ends, its lobby resumes and both play the battle. second_enters=False: b only logs in."""
    from cases.replay import newest_dc2
    replay = assets.replay(newest_dc2())

    def watch(dirs):
        a, b = dirs
        deadline = time.time() + 900
        while time.time() < deadline and SUSPENDED not in read_log(a):
            time.sleep(0.5)
        time.sleep(REPLAY_SECONDS)
        open(os.path.join(b, "go_chat.flag"), "w").close()
        while time.time() < deadline and not RESUMED.search(read_log(a)):
            time.sleep(0.5)
        open(os.path.join(a, "lobby_back.flag"), "w").close()

    first = {"battle": True, "chat": False, "in_battle": 600, "start_replay": ["start_replay.txt", replay],
             "wait_file": "lobby_back.flag", "wait_file_max": 12000 if second_enters else 3600}
    first_opts = {"window": True, "frames": 80000, "env": {"GDXSV_TEST_START_REPLAY": "start_replay.txt"}}
    second = {"battle": True, "in_battle": 600, "wait_file": "go_chat.flag", "wait_file_max": 12000} \
        if second_enters else {}
    return session(ctx, tag, [(tag + "a", LOGIN_KEY, first, first_opts), (tag + "b", LOGIN_KEY2, second)],
                   mcs_region="p2p", replay_state=True, during=watch)


def lobby_replay_checks(probes, lbs: str) -> list:
    a = probes[0].log
    m = RESUMED.search(a)
    started = a.find(SUSPENDED)
    return [
        {"what": "a: replay started from the lobby",
         "ok": TEST_STARTED in a and started >= 0 and "Loaded state" in a[started:]},
        {"what": "a: the match ended the replay", "ok": BACK in a[max(started, 0):]},
        {"what": "a: lobby resumed with the held messages", "held": int(m.group(1)) if m else None,
         "ok": bool(m) and int(m.group(1)) > 0},
    ] + pair_checks(probes, lbs)


@case("O6", "replay from the online lobby while waiting for a match: the match ends it, the lobby resumes "
            "and the battle starts (local gdxsv, two clients)")
def online_lobby_replay(ctx):
    probes, lbs, _ = lobby_replay(ctx, "O6")
    return result(lobby_replay_checks(probes, lbs))


@case("O6c", "O6 with the second client not entering: no match, a's replay is not ended", control_of="O6")
def online_lobby_replay_control(ctx):
    probes, lbs, _ = lobby_replay(ctx, "O6c", second_enters=False)
    c = lobby_replay_checks(probes, lbs)
    ok = {x["what"]: x["ok"] for x in c}
    return result([{"what": "a: replay started from the lobby", "ok": ok["a: replay started from the lobby"]},
                   {"what": "match checks fail", "ok": not ok["a: the match ended the replay"]
                    and not ok["a: lobby resumed with the held messages"] and not ok["a: battle scene reached"]}],
                  detail=c)
