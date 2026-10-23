"""Launch and stop flycast instances for the gdxsv tests.

Every instance gets its own directory with its own config and data, so any
number can run at once. All differences between operating systems live here.

Where flycast keeps its files:
- Windows: next to the executable, so each instance runs a copy of it.
- Linux: XDG_CONFIG_HOME / XDG_DATA_HOME, set to <dir>/xdg.
- macOS: $HOME/.flycast when it exists, so HOME is set to <dir>.
"""
import ctypes
import hashlib
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.request
from typing import Dict, List, Optional

from . import recheck

IS_WINDOWS = os.name == "nt"
IS_MAC = sys.platform == "darwin"
IS_LINUX = sys.platform.startswith("linux")
EXE_NAME = "flycast.exe" if IS_WINDOWS else "flycast"
LOG_NAME = "flycast.log"
STATE_URL = "https://storage.googleapis.com/gdxsv/misc/{name}"
# Every instance starts with these; a test's own config comes after and can point them at local servers.
OFFLINE = ["gdxsv:server=127.0.0.1",
           "gdxsv:UploadReplay=no",
           "gdxsv:ReplayApiUrl=http://127.0.0.1:1",
           "gdxsv:LiveApiUrl=http://127.0.0.1:1",
           "gdxsv:UpdateCheckUrl=http://127.0.0.1:1",
           "gdxsv:TexturePackUrl=http://127.0.0.1:1/",
           # The online boot's checks (gdxsv_emu_hooks.cpp), and no router port mapping.
           "gdxsv:PublicIpUrl=http://127.0.0.1:1",
           "gdxsv:UdpTestUrl=http://127.0.0.1:1",
           "gdxsv:LatencyHost=127.0.0.1:1",
           "network:EnableUPnP=no"]
# macOS: while the display sleeps, a window's vsync blocks the emulator.
if IS_MAC:
    OFFLINE.append("config:rend.vsync=no")

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
           clean: bool = True, emu_cfg: Optional[str] = None,
           files: Optional[Dict[str, str]] = None, suspended: bool = False) -> Instance:
    """Start one instance in dir.

    config: "section:key=value" entries, passed as --config.
    state_dir: its *.state files are copied to the data dir (the pre-battle savestates).
    lua: a script copied to the config dir and loaded with config:LuaFileName.
    emu_cfg: the text of an emu.cfg written before the start; None: no file, flycast creates it.
    files: {path relative to the config dir: text}, written before the start (e.g. mappings/*.cfg).
    suspended: started frozen (Windows: before its first instruction); suspend(pid, False) runs it.
    """
    if recheck.ACTIVE:  # the saved instance; Ctx.probe/run read back its exit
        inst = Instance(os.path.abspath(dir), os.path.abspath(exe))
        inst.proc = recheck.Done(0)
        return inst
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
    if emu_cfg is not None:
        with open(os.path.join(inst.config_dir, "emu.cfg"), "w", encoding="utf-8") as f:
            f.write(emu_cfg)
    for rel, text in (files or {}).items():
        path = os.path.join(inst.config_dir, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
    # Never the production lobby or APIs: a session that ends returns to the lobby (gdxsv:server).
    cfg = list(OFFLINE) + list(config or [])
    if lua:
        shutil.copy(lua, os.path.join(inst.config_dir, os.path.basename(lua)))
        cfg.append(f"config:LuaFileName={os.path.basename(lua)}")
    if headless:
        cfg.append("gdxsv:headless=yes")
    args = [inst.run_exe]
    for c in cfg:
        args += ["--config", c]
    if rom:  # none: the main menu (GuiState::Main)
        args.append(rom)
    penv = os.environ.copy()
    penv.update(inst.env())
    penv.update(env or {})
    flags = 0x4 if suspended and IS_WINDOWS else 0  # CREATE_SUSPENDED
    inst.proc = subprocess.Popen(args, cwd=inst.dir, env=penv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 creationflags=flags)
    if suspended and not IS_WINDOWS:
        suspend(inst.proc.pid, True)
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


def window_rect(pid: int) -> Optional[tuple]:
    """The screen box (left, top, right, bottom) of pid's window client area, raised to
    the top so a screen grab of it shows the emulator; None if it has no visible window.
    Windows only (NotImplementedError elsewhere): a grab of the window itself is black,
    its surface is OpenGL/DirectX."""
    if not IS_WINDOWS:
        raise NotImplementedError("window_rect: Windows only")
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    user32.SetProcessDPIAware()  # screen pixels, the same as the grab's
    hwnds: List[int] = []
    proc_t = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def visit(hwnd, _):
        p = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
            hwnds.append(hwnd)
        return True

    user32.EnumWindows(proc_t(visit), None)
    if not hwnds:
        return None
    hwnd = ctypes.c_void_p(hwnds[0])
    # HWND_TOPMOST, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE
    user32.SetWindowPos(hwnd, ctypes.c_void_p(-1), 0, 0, 0, 0, 0x1 | 0x2 | 0x10)
    r, pt = wintypes.RECT(), wintypes.POINT(0, 0)
    user32.GetClientRect(hwnd, ctypes.byref(r))
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return (pt.x, pt.y, pt.x + r.right, pt.y + r.bottom)


def click(pid: int, x: int, y: int) -> bool:
    """A left click at (x, y) of pid's window client area, with the real mouse (the window
    is raised first; the click also gives it the keyboard). False if it has no window."""
    box = window_rect(pid)
    if box is None or box[2] <= box[0]:
        return False
    user32 = ctypes.windll.user32
    user32.SetCursorPos(box[0] + x, box[1] + y)
    time.sleep(0.2)  # ImGui sees the hover before the press
    user32.mouse_event(0x2, 0, 0, 0, 0)  # MOUSEEVENTF_LEFTDOWN
    time.sleep(0.1)
    user32.mouse_event(0x4, 0, 0, 0, 0)  # MOUSEEVENTF_LEFTUP
    return True


def press_key(vk: int, pid: Optional[int] = None, hold: float = 0.1) -> None:
    """Press and release a Windows virtual-key code; pid selects the target window on macOS."""
    if IS_MAC:
        if pid is None:
            raise ValueError("macOS key injection needs a target pid")
        codes = {0x20: 49, 0x09: 48, 0x1b: 53}  # Space, Tab, Escape
        code = codes[vk]
        cg = ctypes.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
        cf = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
        ctypes.CDLL("/System/Library/Frameworks/AppKit.framework/AppKit")
        objc = ctypes.CDLL("/usr/lib/libobjc.A.dylib")
        objc.objc_getClass.argtypes = [ctypes.c_char_p]
        objc.objc_getClass.restype = ctypes.c_void_p
        objc.sel_registerName.argtypes = [ctypes.c_char_p]
        objc.sel_registerName.restype = ctypes.c_void_p
        running = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int)(("objc_msgSend", objc))
        activate = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong)(("objc_msgSend", objc))
        app = running(objc.objc_getClass(b"NSRunningApplication"),
                      objc.sel_registerName(b"runningApplicationWithProcessIdentifier:"), pid)
        if not activate(app, objc.sel_registerName(b"activateWithOptions:"), 2):
            raise RuntimeError("Could not focus the test emulator")
        cg.CGEventCreateKeyboardEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_bool]
        cg.CGEventCreateKeyboardEvent.restype = ctypes.c_void_p
        cg.CGEventPostToPid.argtypes = [ctypes.c_int, ctypes.c_void_p]
        cf.CFRelease.argtypes = [ctypes.c_void_p]
        for down in (True, False):
            event = cg.CGEventCreateKeyboardEvent(None, code, down)
            if not event:
                raise RuntimeError("Could not create keyboard event")
            cg.CGEventPostToPid(pid, event)
            cf.CFRelease(event)
            if down:
                time.sleep(hold)
        return
    if not IS_WINDOWS:
        raise NotImplementedError("press_key: Windows and macOS only")
    if pid is not None:
        window_rect(pid)  # raises the target window
    user32 = ctypes.windll.user32
    scan = user32.MapVirtualKeyW(vk, 0)
    user32.keybd_event(vk, scan, 0, 0)
    time.sleep(hold)
    user32.keybd_event(vk, scan, 0x2, 0)  # KEYEVENTF_KEYUP


def suspend(pid: int, on: bool) -> None:
    """Freeze (on=True) or resume a process, every thread of it."""
    if not IS_WINDOWS:
        os.kill(pid, signal.SIGSTOP if on else signal.SIGCONT)
        return
    ntdll, kernel32 = ctypes.windll.ntdll, ctypes.windll.kernel32
    kernel32.OpenProcess.restype = ctypes.c_void_p
    handle = kernel32.OpenProcess(0x0800, False, pid)  # PROCESS_SUSPEND_RESUME
    if not handle:
        raise OSError(f"OpenProcess({pid}) failed")
    try:
        (ntdll.NtSuspendProcess if on else ntdll.NtResumeProcess)(ctypes.c_void_p(handle))
    finally:
        kernel32.CloseHandle(ctypes.c_void_p(handle))


def close(pid: int) -> None:
    """Ask pid to quit as a user would: WM_CLOSE to its windows (Windows), else SIGTERM."""
    if IS_WINDOWS:
        subprocess.run(["taskkill", "/PID", str(pid)], capture_output=True)
    else:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass


def processes(exe: str) -> List[int]:
    """The pids of the running processes started from exe (Windows only: an instance's
    run_exe, so the processes it started from itself too)."""
    if not IS_WINDOWS:
        raise NotImplementedError("processes: Windows only")
    kernel32, psapi = ctypes.windll.kernel32, ctypes.windll.psapi
    kernel32.OpenProcess.restype = ctypes.c_void_p
    pids = (ctypes.c_ulong * 8192)()
    got = ctypes.c_ulong()
    psapi.EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(got))
    want = os.path.normcase(os.path.abspath(exe))
    found = []
    for pid in pids[:got.value // ctypes.sizeof(ctypes.c_ulong)]:
        handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            continue
        buf, size = ctypes.create_unicode_buffer(1024), ctypes.c_ulong(1024)
        if (kernel32.QueryFullProcessImageNameW(ctypes.c_void_p(handle), 0, buf, ctypes.byref(size))
                and os.path.normcase(buf.value) == want):
            found.append(pid)
        kernel32.CloseHandle(ctypes.c_void_p(handle))
    return found


def _process_memory(pid: int, fn):
    """fn(kernel32, handle) on a handle with VM read and operation rights (Windows only)."""
    kernel32 = ctypes.windll.kernel32
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.VirtualAllocEx.restype = ctypes.c_void_p
    kernel32.VirtualAllocEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong,
                                        ctypes.c_ulong]
    kernel32.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                           ctypes.POINTER(ctypes.c_size_t)]
    handle = kernel32.OpenProcess(0x0010 | 0x0008 | 0x0400, False, pid)  # VM_READ, VM_OPERATION, QUERY_INFORMATION
    if not handle:
        raise OSError(f"OpenProcess({pid}) failed")
    try:
        return fn(kernel32, ctypes.c_void_p(handle))
    finally:
        kernel32.CloseHandle(ctypes.c_void_p(handle))


def read_memory(pid: int, address: int, size: int) -> Optional[bytes]:
    """size bytes at a host address of the process; None if not readable (Windows only)."""
    def read(kernel32, handle):
        buf, n = ctypes.create_string_buffer(size), ctypes.c_size_t()
        ok = kernel32.ReadProcessMemory(handle, ctypes.c_void_p(address), buf, size, ctypes.byref(n))
        return buf.raw[:n.value] if ok and n.value == size else None
    return _process_memory(pid, read)


def reserve_memory(pid: int, address: int, size: int) -> bool:
    """Reserve host address space in the process, so its own allocation there fails (Windows only)."""
    def reserve(kernel32, handle):
        mem_reserve, page_noaccess = 0x2000, 0x01
        return kernel32.VirtualAllocEx(handle, ctypes.c_void_p(address), size, mem_reserve, page_noaccess) == address
    return _process_memory(pid, reserve)


# sha256 of the savestates published until 2026-10 (cost 295 in RAM); flycast fetches them again too.
OUTDATED_STATES = {"f2385a508a9960777bc275101b29a6f9a3bac0e36fc13f0df9fb325e3cacd786",
                   "6a2da41d1b3224c97970e53dc81289724b5bd2ce4ff636c15b54e29f7461904b"}
_state_lock = threading.Lock()
_states_checked = set()


def ensure_state(state_dir: str, rom: str) -> None:
    """The game boots from <rom name>_99.state; fetch it once if missing or outdated. Cases run
    in parallel: the first one checks and fetches before any instance copies the file."""
    os.makedirs(state_dir, exist_ok=True)
    name = os.path.splitext(os.path.basename(rom))[0] + "_99.state"
    path = os.path.join(state_dir, name)
    with _state_lock:
        if path in _states_checked:
            return
        if os.path.isfile(path):
            with open(path, "rb") as f:
                outdated = hashlib.sha256(f.read()).hexdigest() in OUTDATED_STATES
            if outdated:
                os.remove(path)
        if not os.path.isfile(path):
            print(f"downloading {name}")
            urllib.request.urlretrieve(STATE_URL.format(name=name), path)
        _states_checked.add(path)


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
