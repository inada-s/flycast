"""Launch and stop flycast instances for the gdxsv tests.

Every instance gets its own directory with its own config and data, so any
number can run at once. All differences between operating systems live here.

Where flycast keeps its files:
- Windows: next to the executable, so each instance runs a copy of it.
- Linux: XDG_CONFIG_HOME / XDG_DATA_HOME, set to <dir>/xdg.
- macOS: $HOME/.flycast when it exists, so HOME is set to <dir>.
"""
import ctypes
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
from typing import Dict, List, Optional

IS_WINDOWS = os.name == "nt"
IS_MAC = sys.platform == "darwin"
IS_LINUX = sys.platform.startswith("linux")
EXE_NAME = "flycast.exe" if IS_WINDOWS else "flycast"
LOG_NAME = "flycast.log"
STATE_URL = "https://storage.googleapis.com/gdxsv/misc/{name}"

# Log lines that mean something broke, whatever the test.
ERROR_RE = re.compile(r"GGPO error|Exception|\[GPF\]|Assertion|Flycast has stopped")


class Instance:
    """One flycast process and the files it writes."""

    def __init__(self, dir: str, exe: str):
        self.dir = dir
        self.exe = exe  # the source executable; on Windows a copy runs from dir
        self.proc: Optional[subprocess.Popen] = None

    @property
    def run_exe(self) -> str:
        return os.path.join(self.dir, EXE_NAME) if IS_WINDOWS else self.exe

    @property
    def config_dir(self) -> str:
        if IS_LINUX:
            return os.path.join(self.dir, "xdg", "flycast")
        if IS_MAC:
            return os.path.join(self.dir, ".flycast")
        return self.dir

    @property
    def data_dir(self) -> str:
        if IS_LINUX:
            return os.path.join(self.dir, "xdg", "flycast")
        return os.path.join(self.config_dir, "data")

    @property
    def log_path(self) -> str:
        # LogToFile writes to the working directory, except on macOS (data dir).
        return os.path.join(self.data_dir if IS_MAC else self.dir, LOG_NAME)

    @property
    def hashlog_path(self) -> str:
        return os.path.join(self.data_dir, "hashlog.txt")

    @property
    def log(self) -> str:
        try:
            with open(self.log_path, encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    @property
    def errors(self) -> List[str]:
        return [l for l in self.log.splitlines() if ERROR_RE.search(l)]

    def env(self) -> Dict[str, str]:
        if IS_LINUX:
            xdg = os.path.join(self.dir, "xdg")
            return {"XDG_CONFIG_HOME": xdg, "XDG_DATA_HOME": xdg}
        if IS_MAC:
            return {"HOME": self.dir}
        return {}

    def poll(self) -> Optional[int]:
        return self.proc.poll() if self.proc else None

    def kill(self) -> None:
        if self.proc is not None and self.proc.poll() is None:
            kill(self.proc.pid)

    def wait_log(self, text: str, timeout: float) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if text in self.log:
                return True
            time.sleep(0.2)
        return False

    def remove_exe(self) -> None:
        """Delete the per-instance copy of the executable (Windows)."""
        if IS_WINDOWS:
            try:
                os.remove(self.run_exe)
            except OSError:
                pass


def launch(exe: str, rom: str, dir: str, config: Optional[List[str]] = None, env: Optional[Dict[str, str]] = None,
           headless: bool = True, state_dir: Optional[str] = None, lua: Optional[str] = None,
           clean: bool = True) -> Instance:
    """Start one instance in dir.

    config: "section:key=value" entries, passed as --config.
    state_dir: its *.state files are copied to the data dir (the pre-battle savestates).
    lua: a script copied to the config dir and loaded with config:LuaFileName.
    """
    if clean:
        shutil.rmtree(dir, ignore_errors=True)
    inst = Instance(os.path.abspath(dir), os.path.abspath(exe))
    os.makedirs(inst.data_dir, exist_ok=True)
    if IS_WINDOWS:
        shutil.copy(inst.exe, inst.run_exe)
    if state_dir:
        for name in os.listdir(state_dir):
            if name.endswith(".state"):
                shutil.copy(os.path.join(state_dir, name), inst.data_dir)
    cfg = list(config or [])
    if lua:
        shutil.copy(lua, os.path.join(inst.config_dir, os.path.basename(lua)))
        cfg.append(f"config:LuaFileName={os.path.basename(lua)}")
    if headless:
        cfg.append("gdxsv:headless=yes")
    args = [inst.run_exe]
    for c in cfg:
        args += ["--config", c]
    args.append(rom)
    penv = os.environ.copy()
    penv.update(inst.env())
    penv.update(env or {})
    inst.proc = subprocess.Popen(args, cwd=inst.dir, env=penv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return inst


def kill(pid: int) -> None:
    if IS_WINDOWS:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)], capture_output=True)
    else:
        try:
            os.kill(pid, 9)
        except OSError:
            pass


def hung_windows(pids: List[int]) -> Dict[int, Optional[bool]]:
    """Windows only: for each pid, whether its window is not responding (no
    messages processed for about 5 s); None if it has no visible window."""
    user32 = ctypes.windll.user32
    found: Dict[int, Optional[bool]] = {p: None for p in pids}
    proc_t = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def visit(hwnd, _):
        pid = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(pid))
        if pid.value in found and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
            found[pid.value] = bool(found[pid.value]) or bool(user32.IsHungAppWindow(ctypes.c_void_p(hwnd)))
        return True

    user32.EnumWindows(proc_t(visit), None)
    return found


def ensure_state(state_dir: str, rom: str) -> None:
    """The game boots from <rom name>_99.state; fetch it once if missing."""
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
