"""Shared helpers for the rollback netcode test suite (see README.md).

Each match runs four flycast instances as a local rollback test
(gdxsv:rbk_test=N/4). Every peer gets its own working directory under the
output directory, with a copy of its executable and the pre-battle savestate.
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
from dataclasses import dataclass, field
from typing import Dict, List, Optional

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
IS_WINDOWS = os.name == "nt"
EXE_NAME = "flycast.exe" if IS_WINDOWS else "flycast"
N = 4
STATE_URL = "https://storage.googleapis.com/gdxsv/misc/{name}"

# Log lines that mean something broke, whatever the case.
ERROR_RE = re.compile(r"GGPO error|Exception|\[GPF\]|Assertion|Flycast has stopped")


@dataclass
class Settings:
    exe: str
    rom: str
    out: str
    state_dir: str
    old_exe: Optional[str] = None
    older_exe: Optional[str] = None


@dataclass
class Peer:
    index: int  # 1-based
    dir: str
    exit_code: Optional[int] = None

    @property
    def log(self) -> str:
        try:
            with open(os.path.join(self.dir, "flycast.log"), encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    @property
    def hashlog(self) -> List[int]:
        try:
            with open(os.path.join(self.dir, "data", "hashlog.txt"), encoding="utf-8", errors="replace") as f:
                return [int(l.split()[0]) for l in f if l.split() and l.split()[0].lstrip("-").isdigit()]
        except OSError:
            return []

    @property
    def last_frame(self) -> int:
        frames = self.hashlog
        return frames[-1] if frames else -1

    @property
    def rolled_back_frames(self) -> int:
        seen, dup = set(), set()
        for f in self.hashlog:
            (dup if f in seen else seen).add(f)
        return len(dup)

    @property
    def finished(self) -> bool:
        return "RollbackNet local test finished" in self.log

    @property
    def errors(self) -> List[str]:
        return [l for l in self.log.splitlines() if ERROR_RE.search(l)]

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


def ensure_state(state_dir: str, rom: str) -> None:
    """The local test boots from <rom name>_99.state; fetch it once if missing."""
    os.makedirs(state_dir, exist_ok=True)
    name = os.path.splitext(os.path.basename(rom))[0] + "_99.state"
    path = os.path.join(state_dir, name)
    if not os.path.isfile(path):
        print(f"downloading {name}")
        urllib.request.urlretrieve(STATE_URL.format(name=name), path)


def fetch_release(tag: str, out: str) -> str:
    """Download a gdxsv release build with the GitHub CLI and return its executable."""
    dest = os.path.join(out, "releases", tag)
    exe = os.path.join(dest, "flycast-gdxsv.exe" if IS_WINDOWS else "flycast-gdxsv")
    if not os.path.isfile(exe):
        os.makedirs(dest, exist_ok=True)
        asset = "flycast-gdxsv-windows.zip" if IS_WINDOWS else "flycast-gdxsv-linux-x86_64.zip"
        subprocess.run(["gh", "release", "download", tag, "-R", "inada-s/flycast", "-p", asset, "-D", dest,
                        "--clobber"], check=True)
        shutil.unpack_archive(os.path.join(dest, asset), dest)
    return exe


def run_match(s: Settings, tag: str, exes: List[str], delays: List[int], seed: int = 55,
              env: Optional[Dict[str, str]] = None, headless: bool = True, extra: Optional[List[str]] = None,
              timeout: int = 900, stall_sec: int = 20, on_start=None, keep_exe: bool = False) -> Match:
    """Run one 4-player local match and wait for it to end.

    A peer whose hashlog frame stops moving for stall_sec, before it reports
    the local test finished, counts as stalled and ends the match. on_start is
    called with the Match once every peer has been launched.
    """
    base = os.path.join(s.out, tag)
    shutil.rmtree(base, ignore_errors=True)
    match = Match(tag)
    procs = []
    for i in range(N):
        d = os.path.join(base, f"p{i + 1}")
        os.makedirs(os.path.join(d, "data"), exist_ok=True)
        shutil.copy(exes[i], os.path.join(d, EXE_NAME))
        for f in glob.glob(os.path.join(s.state_dir, "*.state")):
            shutil.copy(f, os.path.join(d, "data"))
        args = [os.path.join(d, EXE_NAME),
                "--config", "config:aica.Volume=0",
                "--config", "gdxsv:hashlog=yes",
                "--config", "gdxsv:ReplayFourScreen=no",
                "--config", f"gdxsv:rbk_test={i + 1}/{N}",
                "--config", f"gdxsv:rand_input={seed}",
                "--config", "log:Verbosity=3",
                "--config", "log:LogToFile=1"]
        if headless:
            args += ["--config", "gdxsv:headless=yes"]
        else:
            args += ["--config", f"window:left={40 + (i % 2) * 660}", "--config", f"window:top={40 + (i // 2) * 520}",
                     "--config", "window:width=640", "--config", "window:height=480"]
        args += (extra or []) + [s.rom]
        penv = os.environ.copy()
        penv.update(env or {})
        penv["GGPO_NETWORK_DELAY"] = str(delays[i])
        procs.append(subprocess.Popen(args, cwd=d, env=penv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        match.peers.append(Peer(i + 1, d))

    if on_start:
        on_start(match)

    start = time.time()
    progress = [(-1, start)] * N
    while any(p.poll() is None for p in procs):
        now = time.time()
        if now - start > timeout:
            match.timed_out = True
            break
        for i, p in enumerate(procs):
            peer = match.peers[i]
            fr = peer.last_frame
            if fr != progress[i][0]:
                progress[i] = (fr, now)
            elif p.poll() is None and fr > 0 and not peer.finished and now - progress[i][1] > stall_sec:
                match.stalled.append(f"p{i + 1}@{fr}")
        if match.stalled:
            break
        time.sleep(1)
    for p in procs:
        if p.poll() is None:
            kill(p.pid)
    match.elapsed = time.time() - start
    for i, p in enumerate(procs):
        p.wait()
        match.peers[i].exit_code = p.returncode
        if not keep_exe:
            try:
                os.remove(os.path.join(match.peers[i].dir, EXE_NAME))
            except OSError:
                pass
    return match


def kill(pid: int) -> None:
    if IS_WINDOWS:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)], capture_output=True)
    else:
        try:
            os.kill(pid, 9)
        except OSError:
            pass


def kill_peer(match: Match, index: int) -> None:
    """Kill one peer, found by the executable path in its working directory."""
    target = os.path.join(match.peers[index - 1].dir, EXE_NAME)
    if IS_WINDOWS:
        ps = (f"Get-CimInstance Win32_Process -Filter \"Name='{EXE_NAME}'\" | "
              f"Where-Object {{ $_.ExecutablePath -eq '{target}' }} | ForEach-Object {{ Stop-Process -Id $_.ProcessId -Force }}")
        subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True)
    else:
        subprocess.run(["pkill", "-9", "-f", target], capture_output=True)


def sample_responding(match: Match, indexes: List[int], seconds: float) -> List[str]:
    """Windows only: sample Process.Responding of the given peers every 0.5s.

    Returns one line per sample, e.g. "5.6s p1=HUNG p2=ok". Windows flags a
    window as not responding after ~5s without processing messages.
    """
    paths = ",".join(f"'{os.path.join(match.peers[i - 1].dir, EXE_NAME)}'" for i in indexes)
    ps = f"""
$paths = @({paths})
$pids = @{{}}
foreach ($p in (Get-CimInstance Win32_Process -Filter "Name='{EXE_NAME}'")) {{
  $i = [Array]::IndexOf($paths, $p.ExecutablePath); if ($i -ge 0) {{ $pids["p$($i)"] = $p.ProcessId }} }}
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt {seconds}) {{
  $row = @()
  foreach ($k in ($pids.Keys | Sort-Object)) {{
    $gp = Get-Process -Id $pids[$k] -ErrorAction SilentlyContinue
    if ($gp) {{ $gp.Refresh(); $row += "$k=" + ($(if ($gp.Responding) {{'ok'}} else {{'HUNG'}})) }} else {{ $row += "$k=exited" }}
  }}
  "{{0,5:N1}}s {{1}}" -f $sw.Elapsed.TotalSeconds, ($row -join ' ')
  Start-Sleep -Milliseconds 500
}}
"""
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    # The script numbers peers by list position; map back to peer numbers.
    names = {f"p{j}": f"p{i}" for j, i in enumerate(indexes)}
    return [re.sub(r"\bp\d+(?==)", lambda m: names.get(m.group(0), m.group(0)), l) for l in out.strip().splitlines()]


def wait_for_log(match: Match, index: int, text: str, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if text in match.peers[index - 1].log:
            return True
        time.sleep(0.2)
    return False


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
