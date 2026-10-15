"""Shared helpers for the rollback netcode test suite (see README.md).

Each match runs four flycast instances as a local rollback test
(gdxsv:rbk_test=N/4), each in its own directory under the output directory
(tests/gdxsv/lib/emu.py).
"""
import os
import shutil
import re
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(HERE, ".."))
from lib import emu, hashlog  # noqa: E402
from lib.emu import IS_WINDOWS, EXE_NAME, ERROR_RE, kill, ensure_state, fetch_release  # noqa: E402,F401

N = 4
DEFAULT_PORT_BASE = 20010  # the peers' ports: base + peer. Release builds always use this one.

_local = threading.local()


def port_base() -> int:
    """Port base of the matches this thread runs (TEST_PORT_BASE), so cases can run in parallel."""
    return getattr(_local, "port_base", DEFAULT_PORT_BASE)


def set_port_base(base: int) -> None:
    _local.port_base = base


def relay_port_base() -> int:
    """First port for local relay servers, apart from every other thread's."""
    return 19879 + (port_base() - DEFAULT_PORT_BASE) // 10


@dataclass
class Settings:
    exe: str
    rom: str
    out: str
    state_dir: str
    old_exe: Optional[str] = None
    older_exe: Optional[str] = None
    relay_exe: Optional[str] = None  # gdxsv binary, for the relay server case


@dataclass
class Peer:
    index: int  # 1-based
    inst: emu.Instance
    exit_code: Optional[int] = None

    @property
    def dir(self) -> str:
        return self.inst.dir

    @property
    def log(self) -> str:
        return self.inst.log

    @property
    def hashlog(self) -> List[int]:
        return hashlog.frames(self.inst.hashlog_path)

    @property
    def last_frame(self) -> int:
        return hashlog.last_frame(self.inst.hashlog_path)

    @property
    def rolled_back_frames(self) -> int:
        return hashlog.rolled_back(self.inst.hashlog_path)

    @property
    def finished(self) -> bool:
        return "RollbackNet local test finished" in self.log

    @property
    def errors(self) -> List[str]:
        return self.inst.errors

    @property
    def win_teams(self) -> List[str]:
        return re.findall(r"WIN_TEAM = (\d+)", self.log)

    @property
    def used_ms(self) -> List[str]:
        return re.findall(r"USED MS = (\d+)", self.log)

    @property
    def joins(self) -> List[str]:
        # A rollback re-simulation can log the same join twice; keep first occurrences.
        return list(dict.fromkeys(re.findall(r"(?:StartMsg|LoadEndMsg) Join:(\d+)", self.log)))


@dataclass
class Match:
    tag: str
    peers: List[Peer] = field(default_factory=list)
    stalled: List[str] = field(default_factory=list)
    timed_out: bool = False
    elapsed: float = 0.0


def run_match(s: Settings, tag: str, exes: List[str], delays: List[int], seed: int = 55,
              env: Optional[Dict[str, str]] = None, headless: bool = True, config: Optional[List[str]] = None,
              lua: Optional[str] = None, timeout: int = 900, stall_sec: int = 20, on_start=None,
              keep_exe: bool = False) -> Match:
    """Run one 4-player local match and wait for it to end.

    A peer whose hashlog frame stops moving for stall_sec, before it reports
    the local test finished, counts as stalled and ends the match. on_start is
    called with the Match once every peer has been launched. config: more
    "section:key=value" entries; lua: a script every peer loads.
    """
    base = os.path.join(s.out, tag)
    match = Match(tag)
    if os.path.isdir(base):
        shutil.rmtree(base, ignore_errors=True)
    for i in range(N):
        cfg = ["config:aica.Volume=0",
               "gdxsv:hashlog=yes",
               "gdxsv:ReplayFourScreen=no",
               f"gdxsv:rbk_test={i + 1}/{N}",
               f"gdxsv:rand_input={seed}",
               "log:Verbosity=3",
               "log:LogToFile=1"]
        if not headless:
            cfg += [f"window:left={40 + (i % 2) * 660}", f"window:top={40 + (i // 2) * 520}",
                    "window:width=640", "window:height=480"]
        cfg += config or []
        penv = dict(env or {})
        penv["GGPO_NETWORK_DELAY"] = str(delays[i])
        if port_base() != DEFAULT_PORT_BASE:
            penv["TEST_PORT_BASE"] = str(port_base())
        inst = emu.launch(exes[i], s.rom, os.path.join(base, f"p{i + 1}"), config=cfg, env=penv,
                          headless=headless, state_dir=s.state_dir, lua=lua)
        match.peers.append(Peer(i + 1, inst))

    if on_start:
        on_start(match)

    start = time.time()
    progress = [(-1, start)] * N
    while any(p.inst.poll() is None for p in match.peers):
        now = time.time()
        if now - start > timeout:
            match.timed_out = True
            break
        for i, peer in enumerate(match.peers):
            fr = peer.last_frame
            if fr != progress[i][0]:
                progress[i] = (fr, now)
            elif peer.inst.poll() is None and fr > 0 and not peer.finished and now - progress[i][1] > stall_sec:
                match.stalled.append(f"p{i + 1}@{fr}")
        if match.stalled:
            break
        time.sleep(1)
    for peer in match.peers:
        peer.inst.kill()
    match.elapsed = time.time() - start
    for peer in match.peers:
        peer.inst.proc.wait()
        peer.exit_code = peer.inst.proc.returncode
        if not keep_exe:
            peer.inst.remove_exe()
    return match


def kill_peer(match: Match, index: int) -> None:
    match.peers[index - 1].inst.kill()


def sample_responding(match: Match, indexes: List[int], seconds: float) -> List[str]:
    """Windows only: sample every 0.5 s whether the given peers' windows respond.

    Returns one line per sample, e.g. "5.6 s p1=HUNG p2=ok". Windows flags a
    window as not responding after ~5 s without processing messages.
    """
    pids = {i: match.peers[i - 1].inst.proc.pid for i in indexes}
    out = []
    start = time.time()
    while time.time() - start < seconds:
        hung = emu.hung_windows(list(pids.values()))
        row = []
        for i, pid in pids.items():
            alive = match.peers[i - 1].inst.poll() is None
            state = "exited" if not alive else "HUNG" if hung[pid] else "ok"
            row.append(f"p{i}={state}")
        out.append(f"{time.time() - start:5.1f}s {' '.join(row)}")
        time.sleep(0.5)
    return out


def wait_for_log(match: Match, index: int, text: str, timeout: float) -> bool:
    return match.peers[index - 1].inst.wait_log(text, timeout)


def skip_crossings(peer: Peer):
    """Rollbacks whose re-simulated range contains a timesync skip (needs GGPO_TEST_LOG=1).

    Returns a list of (seek, from, skip_frame, replayed). A skip at the seek
    frame itself was always replayed; one after the seek frame is what the
    skippedFrames fix is about.
    """
    records, loads, replays = [], [], []
    for line in peer.log.splitlines():
        m = re.search(r"RBKTEST skip record frame=(\d+)", line)
        if m:
            records.append(int(m.group(1)))
        m = re.search(r"RBKTEST load seek=(\d+) from=(\d+)", line)
        if m:
            loads.append((int(m.group(1)), int(m.group(2)), len(replays)))
        m = re.search(r"RBKTEST skip replay frame=(\d+)", line)
        if m:
            replays.append(int(m.group(1)))
    out = []
    for li, (seek, frm, ridx) in enumerate(loads):
        nxt = loads[li + 1][2] if li + 1 < len(loads) else len(replays)
        replayed = set(replays[ridx:nxt])
        for s in records:
            if seek <= s < frm:
                out.append((seek, frm, s, s in replayed))
    return out


def scenes(peer: Peer):
    """Scene changes logged with GGPO_TEST_LOG=1: list of (frame, scene, state)."""
    return [(int(m.group(1)), m.group(2), int(m.group(3)))
            for m in re.finditer(r"RBKTEST scene frame=(\d+) scene=(\d+/\d+) confirmed=-?\d+ state=(\d+)", peer.log)]
