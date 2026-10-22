# Frame pacing probe (investigation, not for merge)

Windows, DX11 only. Needs a build of this branch, the Disc 2 GDI and Python 3.

## Run

Trace on: env `FPTRACE=<dir>` (`drive.py` sets it). Knobs (env, read by flycast):

| knob | effect |
|---|---|
| `FPT_LIM=a1` | limiter A1: absolute schedule, a frame late by a period or more skips whole periods |
| `FPT_LIM=a2` | limiter A2: pays back 1/4 of the lateness per frame |
| `FPT_LIM=b2` | A1, each wake snapped to the host vblank nearest to its ideal time (+0.5 ms) |
| `FPT_HRT=1` | with a1/a2/b2: high-resolution waitable timer instead of sleep + busy wait |
| `PACE=yes` | edge shift (B1), current limiter (`drive.py` sets `gdxsv:PresentPacing`) |
| `FPT_HANG=ms:N` | sleep `ms` every N limiter calls (hang test) |
| `FPT_RR=Hz` | override the refresh rate read at startup (swap interval outside rollback battles) |
| `FPT_PERIOD=us` | fixed limiter period; 0 = no limiter; negative = one frame per host vblank |

```
set EXE=C:\path\flycast.exe
set ROM=C:\rom\gdx-disc2\gdx-disc2.gdi
rem offline, one instance, 75 s (SECS to change):
python drive.py --exe %EXE% --rom %ROM% --out out base_off off no
rem 2-peer rollback battle on this PC (~60 s), threaded rendering on:
set FPT_LIM=a1
python drive.py --exe %EXE% --rom %ROM% --out out a1_tr1 match yes
```

Arguments after `tr` are passed as `--config` (e.g. `window:fullscreen=yes`).
Close other windows over flycast; do not touch the PC during a run.

## Read

Each script takes one or more `out/<tag>/fpt` folders (one CSV per flycast process):

| script | prints |
|---|---|
| `burst.py [--skip S]` | bad display intervals (shown-vblank step != 1), bursts (>= 4 bad, <= 8 presents apart), present phase vs vblank, drift per frame |
| `fps.py` | game fps, GGPO timesync skips, `tsdrop` |
| `per.py` | limiter wake period p50 |
| `over.py` | limiter oversleep p50/p99/max |
| `work.py` | limiter wake to Present call p1/p50/p99 |
| `hang.py` | the 6 wake intervals after each injected hang (a catch-up burst = several far under a period) |
| `lat.py [--b3]` | local input sample to Present call / shown vblank (match mode); `--b3` replays the trace as a once-per-vblank present |

Shown vblank = the first host vblank after `Present` returns. The vblank thread watches output 0 of adapter 0: on a
PC with several displays, put the window on that display (the primary), or the vblank times are another display's.

## Trace format

`fpt_<pid>.csv`: `t_us,thread,event,a,b,c`. Events: `vbl` host vblank; `pres` Present returned (a = call us);
`lim` limiter wake; `ef` frame start (rollback); `inp` local input sampled; `rr`/`load`/`save` rollback;
`skip` timesync skip; `tsdrop` dropped TA context; `hang` injected hang; `pace` B1 shift; `rend` UI loop render.
