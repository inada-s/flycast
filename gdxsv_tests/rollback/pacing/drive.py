# One windowed run with the frame pacing probe (DX11, vsync on). The trace goes to <out>/<tag>/fpt/fpt_<pid>.csv.
# mode off: one offline instance for SECS s (default 75). mode match: a 2-peer rollback battle (send delay 50/16 ms).
# usage: drive.py --exe EXE --rom GDI --out DIR [--state-dir DIR] <tag> <off|match> <tr yes|no> [extra --config ...]
# env passed to flycast: FPT_LIM, FPT_HRT, FPT_HANG, FPT_RR; PACE=yes turns on the edge shift (B1).
import argparse, os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", ".."))
import rbk_lib as lib
from lib import emu

ap = argparse.ArgumentParser()
ap.add_argument("--exe", required=True)
ap.add_argument("--rom", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--state-dir", help="where <rom>_99.state lives (match mode); downloaded if missing")
ap.add_argument("tag")
ap.add_argument("mode", choices=["off", "match"])
ap.add_argument("tr", choices=["yes", "no"])
ap.add_argument("extra", nargs="*")
a = ap.parse_args()

out = os.path.abspath(a.out)
tdir = f"{out}/{a.tag}/fpt"
os.makedirs(tdir, exist_ok=True)
for f in os.listdir(tdir):
    os.remove(os.path.join(tdir, f))
cfg = ["config:pvr.rend=2", "gdxsv:PresentPacing=" + os.environ.get("PACE", "no"),
       f"config:rend.ThreadedRendering={a.tr}", "config:rend.vsync=yes"] + a.extra
env = {"FPTRACE": tdir, **{k: os.environ[k] for k in ("FPT_LIM", "FPT_HRT", "FPT_HANG", "FPT_RR") if k in os.environ}}
if a.mode == "off":
    inst = emu.launch(a.exe, a.rom, f"{out}/{a.tag}/p1",
                      config=cfg + ["config:aica.Volume=0", "window:width=640", "window:height=480"],
                      env=env, headless=False)
    time.sleep(int(os.environ.get("SECS", "75")))
    inst.kill()
    inst.proc.wait()
    print("offline done", a.tag)
else:
    state_dir = os.path.abspath(a.state_dir or os.path.join(out, "state"))
    lib.ensure_state(state_dir, a.rom)
    lib.N = 2
    s = lib.Settings(exe=a.exe, rom=a.rom, out=out, state_dir=state_dir)
    m = lib.run_match(s, a.tag + "m", [a.exe] * 2, [50, 16], headless=False,
                      env={"MAXREBATTLE": "1", **env}, config=cfg, timeout=600)
    print(a.tag, "finished", [p.finished for p in m.peers], "timed_out", m.timed_out, "stalled", m.stalled,
          "elapsed", int(m.elapsed))
