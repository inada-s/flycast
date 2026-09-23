#!/usr/bin/env python3
import functools
import os
import subprocess
import shutil
import sys
import glob
import random
import time
import threading
import urllib.request
import shutil
from typing import List
from os.path import exists
from pathlib import Path

print = functools.partial(print, flush=True)
os.chdir(os.path.dirname(os.path.abspath(__file__)))

# macOS ships the emulator as an .app bundle and keeps its config under $HOME,
# so several things below have to branch on it: what we launch, whether a copy
# per instance makes sense, and where the log lands.
IS_MAC = sys.platform == "darwin"

N = int(os.getenv("N", 4))
V = int(os.getenv("V", 3))
TIMEOUT = int(os.getenv("TIMEOUT", 3600))
ITERATION = int(os.getenv("ITERATION", 1))
WIDE = int(os.getenv("WIDE", 0))
ROM = os.getenv("ROM", r"C:\rom\gdx-disc2\gdx-disc2.gdi")
FLYCAST = os.getenv("FLYCAST", "/Applications/Flycast-gdxsv.app" if IS_MAC else r"R:\Temp\flycast.exe")
FLYCAST2 = os.getenv("FLYCAST2")
PATCH_FILE = os.getenv("PATCH_FILE", "")
SYNC_LOG = int(os.getenv("SYNC_LOG", 0))
RBK_MS = os.getenv("RBK_MS", "")
PATCH_EVERY_FRAME = int(os.getenv("PATCH_EVERY_FRAME", 0))
FAKE_TIMESYNC = int(os.getenv("FAKE_TIMESYNC", 0))


def q(path: str) -> str:
    """Quote a path for the shell.

    Paths with spaces are the norm on both platforms - macOS keeps replays under
    "Application Support", Windows under "Program Files" - and run() passes one
    joined string to the shell, so an unquoted path is silently truncated at the
    first space.
    """
    return f'"{path}"' if path else path
GDXSV = os.getenv("GDXSV", r"127.0.0.1")
REPLAY = os.getenv("REPLAY", "")
# Live Spectate: a battle code instead of a recorded file.
SPECTATE = os.getenv("SPECTATE", "")
LIVE_BUFFER = int(os.getenv("LIVE_BUFFER", 30))
# Lock the instances to the same frame and start them together.
SYNC = int(os.getenv("SYNC", 1))
BORDERLESS = int(os.getenv("BORDERLESS", 1))
# macOS needs a gap between launches or instances silently fail to come up.
LAUNCH_GAP = float(os.getenv("LAUNCH_GAP", 3 if IS_MAC else 0))
# What actually goes on the command line. Windows runs the copy that
# prepare_workdir puts in the working directory; macOS runs the binary inside
# the bundle, in place, because its config lives under $HOME either way.
if IS_MAC:
    FLYCAST_NAME = f'"{Path(FLYCAST) / "Contents" / "MacOS" / "Flycast"}"'
else:
    FLYCAST_NAME = Path(FLYCAST).name
X_OFFSET = 0
Y_OFFSET = 50
W = 854 if WIDE else 640
H = 480

def download_state():
    os.makedirs(f"work/state", exist_ok=True)
    if not os.path.isfile(f"work/state/gdx-disc1_99.state"):
        urllib.request.urlretrieve("https://storage.googleapis.com/gdxsv/misc/gdx-disc1_99.state", f"work/state/gdx-disc1_99.state")
    if not os.path.isfile(f"work/state/gdx-disc2_99.state"):
        urllib.request.urlretrieve("https://storage.googleapis.com/gdxsv/misc/gdx-disc2_99.state", f"work/state/gdx-disc2_99.state")


def download_flycast(url: str):
    cwd = os.getcwd()
    os.makedirs(f"work", exist_ok=True)
    os.chdir("work")

    if exists("flycast.exe"):
        os.remove("flycast.exe")

    save_name = os.path.basename(url)
    urllib.request.urlretrieve(url, save_name)

    if save_name.endswith("zip"):
        shutil.unpack_archive(save_name, ".")

    if exists("flycast-gdxsv.exe"):
        os.rename("flycast-gdxsv.exe", "flycast.exe")
    
    assert exists("flycast.exe")

    global FLYCAST
    global FLYCAST_NAME
    FLYCAST = "work/flycast.exe"
    FLYCAST_NAME = "flycast.exe"

    os.chdir(cwd)


def mac_config_dirs(n: int) -> List[Path]:
    """The config directory macOS hands each instance.

    osx-main.mm numbers instances by how many copies are already running, and
    only the second onward get a subdirectory, so instance 1 is the base path.
    """
    base = Path.home() / "Library" / "Application Support" / "Flycast"
    return [base] + [base / str(i) for i in range(2, n + 1)]


def seed_mac_savestates(n: int):
    """Put the pre-battle savestate in every instance's data directory.

    Each instance would otherwise download its own copy on first use - the
    emulator handles that itself - but that is the same 27MB fetched n times.
    Seeding from whichever instance already has it costs nothing. The name is
    derived from the ROM, the same way hostfs::getSavestatePath does it.
    """
    name = Path(ROM).stem + "_99.state"
    dirs = [d / "data" for d in mac_config_dirs(n)]
    src = next((d / name for d in dirs if (d / name).is_file()), None)
    if src is None:
        print(f"no {name} to seed; each instance will download its own")
        return
    for d in dirs:
        d.mkdir(parents=True, exist_ok=True)
        if not (d / name).is_file():
            shutil.copy(src, d / name)
            print(f"seeded {d / name}")


def prepare_workdir(idx: int):
    os.makedirs(f"work/flycast{idx}/data", exist_ok=True)
    if IS_MAC:
        # Nothing to copy: the bundle runs in place and macOS gives each
        # instance its own config directory by instance number anyway.
        return f"work/flycast{idx}"
    if 2 < idx and FLYCAST2:
        shutil.copy(Path(FLYCAST2), f"work/flycast{idx}/{FLYCAST_NAME}")
    else:
        shutil.copy(Path(FLYCAST), f"work/flycast{idx}/")

    for file in glob.glob(os.path.join("work/state", "*.state")):
        if os.path.isfile(file):
            shutil.copy(file, f"work/flycast{idx}/data")
    return f"work/flycast{idx}"


def conf_log(idx: int):
    if CAPTURE:
        return f"--config log:Verbosity={min(V, 2)} --config log:LogToFile=1"
    return f"--config log:Verbosity={V} --config log:LogToFile=1"


def conf_volume(idx: int):
    return f"--config config:aica.Volume=20"


def conf_gdxsv(idx: int):
    return f"--config gdxsv:server={GDXSV}"


# Set once per run so every instance loads at the same instant. Launch time
# varies by seconds, and that offset would otherwise look like drift.
SYNC_START = 0
SYNC_GROUP = ""
# The sync modes are a capture rig, not a debug run: no injected GGPO delay, no
# verbose logging, and rendering pinned so every instance draws the same work.
CAPTURE = False


def conf_capture(idx: int):
    """Pin rendering so the instances are comparable frame for frame.

    Internal resolution especially: the group runs at its slowest member, so
    leaving it at whatever emu.cfg says makes four emulators on one GPU drift
    apart for reasons that have nothing to do with the sync.
    """
    # ShowFPS off: the counter would be baked into the recording.
    return "--config config:rend.Resolution=480 --config config:rend.SuperWideScreen=no --config config:rend.ShowFPS=no"


def conf_sync(idx: int):
    if not SYNC:
        return ""
    return f"--config gdxsv:SpectateSyncGroup={SYNC_GROUP} --config gdxsv:SyncStartTime={SYNC_START}"


def conf_borderless(idx: int):
    return "--config gdxsv:borderless=yes" if BORDERLESS else ""


def conf_window_layout(idx: int):
    x = X_OFFSET + W * (idx % 2)
    y = Y_OFFSET + H * (idx // 2)
    wide = "yes" if WIDE else "no"
    return f"--config window:top={y} --config window:left={x} --config window:width={W} --config window:height={H} --config config:rend.WideScreen={wide} --config config:rend.WidescreenGameHacks={wide}"


def local_exe(name: str) -> str:
    """Point at the copy in the working directory explicitly.

    cmd.exe skips the current directory when resolving a bare command name if
    NoDefaultCurrentDirectoryInExePath is set (it is on some dev machines), so
    "flycast.exe" alone fails with "not recognized". Prefixing ".\\" always
    works. macOS already passes a full path.
    """
    if IS_MAC or os.path.isabs(name) or os.sep in name or "/" in name:
        return name
    return os.path.join(".", name)


def run(idx, *arg_list) -> subprocess.Popen:
    exe, *rest = arg_list
    cmd = " ".join([local_exe(exe), *rest])
    print(cmd)
    new_env = os.environ.copy()
    if not CAPTURE:
        # Debug aid for rollback testing; it has no place in a capture rig.
        new_env["GGPO_NETWORK_DELAY"] = os.getenv("GGPO_NETWORK_DELAY", "16")
    # new_env["GGPO_OOP_PERCENT"] = "1"
    if idx == 0:
        # new_env["GGPO_NETWORK_JAM_DELAY"] = "500"
        pass
    # else: new_env["GGPO_NETWORK_DELAY"] = "100"
    return subprocess.Popen(cmd, shell=True, env=new_env)


def run_rom(idx: int) -> subprocess.Popen:
    return run(idx,
        FLYCAST_NAME,
        conf_gdxsv(idx),
        conf_volume(idx),
        conf_log(idx),
        conf_window_layout(idx),
        q(ROM),
    )


def run_replay(idx: int) -> subprocess.Popen:
    return run(idx,
        FLYCAST_NAME,
        conf_gdxsv(idx),
        conf_volume(idx),
        conf_window_layout(idx),
        conf_log(idx),
        f"--config gdxsv:replay={q(REPLAY)}",
        q(ROM),
    )


def run_replay_sync(idx: int) -> subprocess.Popen:
    return run(idx,
        FLYCAST_NAME,
        conf_gdxsv(idx),
        conf_volume(idx),
        conf_window_layout(idx),
        conf_capture(idx),
        conf_borderless(idx),
        conf_sync(idx),
        conf_log(idx),
        f"--config gdxsv:replay={q(REPLAY)}",
        f"--config gdxsv:ReplayPOV={idx + 1}",
        q(ROM),
    )


def run_spectate(idx: int) -> subprocess.Popen:
    return run(idx,
        FLYCAST_NAME,
        conf_gdxsv(idx),
        conf_volume(idx),
        conf_window_layout(idx),
        conf_capture(idx),
        conf_borderless(idx),
        conf_sync(idx),
        conf_log(idx),
        f"--config gdxsv:spectate={q(SPECTATE)}",
        f"--config gdxsv:ReplayPOV={idx + 1}",
        f"--config gdxsv:LiveBufferFrames={LIVE_BUFFER}",
        q(ROM),
    )


def conf_rbk_debug(idx: int):
    # PATCH_FILE: gdxsv-reveng patch sheet (e.g. patches/dc2.txt) applied as if delivered by LBS.
    # SYNC_LOG=1: each instance writes work/flycastN/sync_log.txt for tools/sync_check.py.
    args = []
    if PATCH_FILE:
        args.append(f"--config gdxsv:patch_file={q(os.path.abspath(PATCH_FILE))}")
    if SYNC_LOG:
        args.append("--config gdxsv:sync_log=yes")
    if RBK_MS:
        # RBK_MS=a,b,c,d: Disk 2 MS id per slot (e.g. 17 Guntank, 11 Gyan, 12 Gogg, 14 Z'Gok)
        args.append(f"--config gdxsv:rbk_ms={RBK_MS}")
    if FAKE_TIMESYNC:
        # FAKE_TIMESYNC=K: peer 1 slows down (timesync skip) every K frames, like a peer running ahead
        args.append(f"--config gdxsv:rbk_fake_timesync={FAKE_TIMESYNC}")
    if PATCH_EVERY_FRAME:
        args.append("--config gdxsv:patch_write_once=no")
    return " ".join(args)


def run_rbk_test(idx: int) -> subprocess.Popen:
    return run(idx,
        FLYCAST_NAME,
        conf_gdxsv(idx),
        conf_volume(idx),
        conf_window_layout(idx),
        conf_log(idx),
        f"--config gdxsv:rbk_test={idx+1}/{N}",
        conf_rbk_debug(idx),
        q(ROM),
    )


def run_rbk_test_random(idx: int) -> subprocess.Popen:
    seed = int(os.getenv("SEED", 0)) or random.randint(1, 99999)  # SEED fixes the random inputs for a rerun
    return run(idx,
        FLYCAST_NAME,
        conf_gdxsv(idx),
        conf_volume(idx),
        conf_window_layout(idx),
        conf_log(idx),
        f"--config gdxsv:rbk_test={idx+1}/{N} --config gdxsv:rand_input={seed}",
        conf_rbk_debug(idx),
        q(ROM),
    )


EMU_BENCHMARK_FRAMES = int(os.getenv("EMU_BENCHMARK_FRAMES", 1800))  # Default: 30 seconds at 60fps
EMU_BENCHMARK_REPLAY_ID = os.getenv("EMU_BENCHMARK_REPLAY_ID", "1769530875357")  # Default replay ID


def download_replay(replay_id: str, dest_dir: str) -> str:
    """Download replay file from gdxsv storage and return local path"""
    url = f"https://storage.googleapis.com/gdxsv/replays/{replay_id}.pb"
    os.makedirs(dest_dir, exist_ok=True)
    dest_path = os.path.join(dest_dir, f"{replay_id}.pb")

    if os.path.isfile(dest_path):
        print(f"Replay already exists: {dest_path}")
        return dest_path

    print(f"Downloading replay: {url}")
    urllib.request.urlretrieve(url, dest_path)
    print(f"Downloaded to: {dest_path}")
    return dest_path


def run_emu_benchmark(idx: int) -> subprocess.Popen:
    """Run emulation benchmark: replay with frame skip to measure CPU emulation speed"""
    replays_dir = f"work/flycast{idx+1}/data/replays"
    replay_path = download_replay(EMU_BENCHMARK_REPLAY_ID, replays_dir)

    new_env = os.environ.copy()
    new_env["FLYCAST_EMU_BENCHMARK_FRAMES"] = str(EMU_BENCHMARK_FRAMES)
    cmd = " ".join([
        local_exe(FLYCAST_NAME),
        conf_gdxsv(idx),
        conf_volume(idx),
        conf_window_layout(idx),
        conf_log(idx),
        f"--config gdxsv:replay={replay_path}",
        q(ROM),
    ])
    print(cmd)
    return subprocess.Popen(cmd, shell=True, env=new_env)


def truncate(path: str):
    with open(path, "w") as f:
        f.truncate(0)


def tail(parent: subprocess.Popen, path: str):
    with open(path, "r", encoding='utf-8') as f:
        f.seek(0, 2)
        while parent.poll() == None:
            print(f.readline(), end="")


def exec_func(func_name: str):
    start_time = time.time()
    cwd = os.getcwd()
    print(cwd)

    global SYNC_START, SYNC_GROUP, CAPTURE
    CAPTURE = func_name in ("replay_sync", "spectate")
    SYNC_GROUP = f"runpy{os.getpid()}"
    # Every instance must be past ROM load and JIT warmup before the gate opens.
    SYNC_START = int(time.time()) + 15 + int(N * (LAUNCH_GAP + 1))
    if SYNC:
        print(f"sync group={SYNC_GROUP} loads at {time.strftime('%H:%M:%S', time.localtime(SYNC_START))}")
    if IS_MAC:
        seed_mac_savestates(N)

    func = {f.__name__[4:]: f for f in [
        run_rom,
        run_replay,
        run_replay_sync,
        run_spectate,
        run_rbk_test,
        run_rbk_test_random,
        run_emu_benchmark,
    ]}[func_name]

    popens: List[subprocess.Popen] = []

    try:
        for i in reversed(range(N)):
            wdir = prepare_workdir(i+1)
            os.chdir(wdir)
            truncate("flycast.log")
            for stale in ("sync_log.txt", "rng_trace.txt"):
                if os.path.exists(stale):
                    os.remove(stale)
            p = func(i)
            if i == 0:
                threading.Thread(target=tail, args=(p, "flycast.log"), name="tail", daemon=True).start()
            popens.append(p)
            os.chdir(cwd)
            if LAUNCH_GAP:
                time.sleep(LAUNCH_GAP)

        while popens[-1].poll() == None:
            time.sleep(1)
            if TIMEOUT < time.time() - start_time:
                print("timeout")
                break
    finally:
        for p in popens:
            if os.name == 'nt':
                subprocess.run(['taskkill', '/F', '/T', '/PID', str(p.pid)])
            else:
                p.kill()
        for p in popens:
            print(f"exit {p.poll()} {p.args}")


def main():
    if FLYCAST.startswith("http"):
        download_flycast(FLYCAST)
    
    download_state()

    for t in range(ITERATION):
        print(f"===== ITERATION {t + 1}/{ITERATION} START =====")
        exec_func(sys.argv[1])
        print(f"===== ITERATION {t + 1}/{ITERATION} END   =====")


if __name__ == "__main__":
    main()
