# gdxsv_tests

End-to-end tests of the gdxsv features: each case runs the emulator with the
game and checks RAM values, logs or hash logs. One case covers one row of
[`docs/gdxsv/features.md`](../docs/gdxsv/features.md), whose Test and Control
columns name the case ids. A control (`<id>c`) runs the case's check on input
where the feature is absent and passes when the check catches it. A case has a
control only when its check is an agreement or comparison (hashes, round
winners, a golden, a recorded result), which a broken check would pass
silently.

Python 3 standard library and Lua only (Pillow for image compares). Not run in
GitHub CI.

## Run

```
python gdxsv_tests/run.py --list
python gdxsv_tests/run.py --exe build/flycast.exe                 # smoke tier
python gdxsv_tests/run.py --exe build/flycast.exe --tier full --jobs 4
python gdxsv_tests/run.py --exe build/flycast.exe --cases R2,R2c
python gdxsv_tests/run.py --recheck gdxsv_tests/out --cases K1      # checks only, no flycast
```

- `--exe` or `GDXSV_EXE`: the flycast build to test.
- Results: `gdxsv_tests/out/` (`--out`), `summary.json` with the commit, one
  directory per case with the instances' logs. Exit code 0 when every case
  passed or was skipped.
- A case whose ROM is missing is skipped.
- `--recheck <out dir>`: each case saved there (with `--cases`: those and their
  controls) runs its checks on a copy of its files (in `<out dir>.recheck`) and
  prints each check with its values, in seconds. Instances, the lobby and the
  HTTP stub are not started: their exit, log and requests are read back from
  `probe.json`, `lbs.log`, `http_<n>.json`, a rollback match's end from
  `match_<n>.json` and its peers' files; what that run did not save is listed
  and defaults to exit 0 and no requests. Try a new or changed check here on an
  earlier run's directory before a run with flycast.

## Pre-release check

One command, about an hour: the full tier and the rollback suite, then a
verdict per failed case.

```
python gdxsv_tests/precheck.py --exe build/flycast.exe --old-exe <last release>/flycast-gdxsv.exe \
    --release gdxsv-1.10.0 --rom <Disc 2 GDI> --state-dir work/state --relay-exe <gdxsv.exe>
```

- Idle box: no build, rig, bench or other job while it runs. It refuses to
  start while flycast, ninja or a compiler runs.
- Parallel first: `run.py --tier full --jobs 4`, then `run_suite.py --jobs 3`.
  Most of a case is real-time play (battles at 59.94 fps, boot, ping test), not
  CPU, so cases share the box well. Tune `--jobs` / `--suite-jobs` per machine
  from measured runs.
- Then each failed case alone. Passes alone: failed under load, not a failure.
- Still fails: 3 runs interleaved with the last release (`--old-exe`).
  - Fails on both: a broken test. It does not block the release; fix its
    setup, timing assumption or check in a test PR. There is no list of
    tolerated flaky cases.
  - Fails on this build only: a regression, printed with the PRs merged since
    `--release`. Only this A/B makes a failure a regression.
- No bisect without the maintainer's approval.
- Time box: no new step starts after `--minutes` (default 60); cases not
  reached show as `not run`.
- Output: `gdxsv_tests/out/precheck/` (`parallel/`, `alone/`, `ab/`, each
  runner's log, `precheck.json`).

## Environment

| Variable | Content |
|---|---|
| `GDXSV_ROM_DIR` | `gdx-disc1/gdx-disc1.gdi`, `gdx-disc2/gdx-disc2.gdi`, NAOMI `naomi.zip`, `gundmxgd.zip`, `gundmxgd/gdl-0006.chd` |
| `GDXSV_DISC1`, `GDXSV_DISC2`, `GDXSV_NAOMI` | single files instead of `GDXSV_ROM_DIR` |
| `GDXSV_STATE_DIR` | boot savestates (`<rom>_99.state`), made on first use (default `out/state`) |
| `GDXSV_REPLAY_DIR` | corpus replays, downloaded on first use and checked against `corpus/replays.json` (default `out/replay`) |
| `GDXSV_REPLAY_JOBS` | replays R1 plays in parallel (default 4) |
| `GDXSV_RELEASE_ROOT` | R4, S3, S4: releases downloaded with `gh` to `<dir>/releases/<tag>` (default `out`) |
| `GDXSV_SERVER_EXE` | O1-O8, S1: `gdxsv` from inada-s/gdxsv, built with `-ldflags "-X main.requiredFlycastVersion="`; S1 needs `-spectator_test_session` (gdxsv `master`) |
| `GDXSV_LOGIN_STATE` | O1-O8: a Disc 2 savestate at the network game menu (通信ゲームシステム) with a registered VMU |
| `GDXSV_FP_OFF_EXE` | D1c: a build with `-DGDXSV_FP_COMPAT=OFF` |

The ROM, BIOS, savestates and replays are not in the repository.
Every instance gets `gdxsv:server=127.0.0.1`, `UploadReplay=no` and local
replay/live API URLs: no case reaches a production server. The online cases also
send the public IP, P2P and latency checks to a local fake server and turn UPnP off.

## macOS

- Lua is off in Apple builds unless `-DGDXSV_APPLE_LUA=ON` (Lua 5.4, e.g.
  `-DLUA_INCLUDE_DIR=<lua>/include -DLUA_LIBRARY=<lua>/lib/liblua.a`); without it
  every case that reads RAM through Lua fails.
- `--exe <build>/Flycast.app/Contents/MacOS/Flycast`, signed (`codesign -s - --force --deep`).
- python.org Python has no CA bundle: `SSL_CERT_FILE=/etc/ssl/cert.pem` for the
  replay and savestate downloads.
- Every instance gets `rend.vsync=no`: a window's vsync blocks while the display sleeps.
- x86_64 build on Apple silicon (runs under Rosetta): `-DCMAKE_OSX_ARCHITECTURES=x86_64`,
  an x86_64 `liblua.a` (archive it with `/usr/bin/libtool -static`, GNU `ar` output
  does not link), `-DFT_DISABLE_PNG=ON` when Homebrew's arm64 libpng is found.

## Layout

| Path | Content |
|---|---|
| `run.py` | runner |
| `lib/emu.py` | launch and stop an instance (own data dir, config, ports); OS differences live here |
| `lib/case.py` | case registry, `Ctx.probe` (one instance with a Lua script), `result` |
| `lib/assets.py` | ROM, savestate and replay paths |
| `lib/hashlog.py`, `lib/replayfile.py` | per-frame hash logs; replay protobuf (round results, input cut, field encoding) |
| `lib/image.py`, `golden/` | screen grabs of a window, white-text masks compared with small 1-bit golden images |
| `lua/e2e.lua` | helpers joined in front of every case script: frame waits, input, RAM reads, `E2E key=value` lines to `e2e.log` |
| `cases/*.py` | the cases |
| `corpus/replays.json` | replays of every format, with sha256 and the winners per round |
| `rollback/` | rollback suite `run_suite.py` (A1 to D8), see its README |
| `perf/` | measurement tools, not cases: `seek_bench.py` (replay seek ms per frame, builds alternated per round), `sampler.cpp` + `rip_profile.py` (Windows sampling profile of the seek); usage in each file |

## Cases

| Id | Tier | Checks | Time (windows-mini) |
|---|---|---|---|
| P1 | smoke | game patches rewritten within 2 vblanks after the original values are written back (Disc 1, 2) | 3 s |
| L1 | smoke | translation patch: Disc 2 text per language | 6 s |
| L2 | full | `language=3` (Disabled): Disc 2's `0x8c1ca128` is read and is not "Flycast > Settin", the text every language patches in (L1 is its control) | 2 s |
| N1 | smoke | NAOMI version boots: game id, code at `0x0c020000` | 21 s |
| C1 | smoke | no `emu.cfg`: the gdxsv defaults (`rend.Resolution` 720, `aica.Volume` 50, ...) still hold after the game start, read through Lua | 2 s |
| C2 | smoke | an `emu.cfg` with gdxsv-1.9.6's sections and keys (`[config]`, `[gdxsv]`): `rend.Resolution`, `aica.Volume` read through Lua, `loginkey`, `LocalPort`, `language` from the log | 2 s |
| C3 | smoke | an `emu.cfg` `LocalPort` no UDP socket can bind (70000, -5): the logged `udp_port` is a port in 1-65535 | 3 s |
| R2 / R2c | smoke only (R2; part of R1 in full), full (R2c) | newest Disc 2 replay plays to the end with the recorded winners | 110-150 s |
| R5 | full | cost 300->295 at vblank 4000 of the newest corpus replays: Disc 1 and Disc 2 as recorded (before `log_file_version` 20261023) and a Disc 2 copy at 20261023 with a `cost295` game patch read 295 | 7 s |
| R1 | full | all corpus replays play to the end with the recorded winners | 470-520 s (4-5 replays in parallel) |
| W1 | full | widescreen matte edges and HUD offset per layout and viewport (formula of `WriteWidescreenPatch`) | 10 s |
| I1 / I1c | full | 4-peer local rollback match at GGPO delay 2 and 4: peer 1's B press reaches the confirmed input buffer 2 vblanks later at delay 4 | 130-260 s |
| Q1 | full | 4-peer local rollback match (network delay 0/4/0/4 ms, `GGPO_TEST_LOG`): each peer's hash log holds frames 1..last, one rewind per logged rollback load, to the loaded frame + 1 from the load's frame | 67 s |
| O1, O6 | full | Disc 2 logs in to a local `gdxsv lbs` (fresh SQLite, seeded user for the login key): platform info (payload `patch_id`, public IP, port test result), user login, plaza join; public IP and P2P test go to a local fake HTTP server, which sends the UDP "Hello" back (O6: no UDP Hello, `Failed (Timeout)`) | 85 s each |
| O2 | full | O1, then a training lobby entry, chat force start, mobile suit select and battle: one player on the UDP backend through the local battle server | 170 s |
| O3 | full | as O2 in a P2P lobby (`mcs_region` `p2p`): lobby sends the P2P matching, one-peer GGPO session, battle loads on the rollback backend | 170 s |
| O4 | full | two clients (two login keys, same savestate) in one P2P lobby: both enter, one force-starts; P2P matching to both, two-peer GGPO session, each peer answers the other, both reach the battle | 190 s |
| O7 | full | two clients in one P2P lobby: a, on the plaza map, plays the last round of the newest Disc 2 corpus replay from the lobby (`GDXSV_TEST_START_REPLAY`, `replay_target_round`; a in a window: a headless replay end exits the process). a's log: suspended, state loaded, resumed after it; then a joins lobby 2 and enters; from the entry to the mobile suit select the start is refused (`test: menu refused`), also between the match (`GdxsvBackendRollback.Prepare`) and the battle connection (`.Open`), and no replay starts; b joins, enters and force-starts: the O4 pair checks | not measured |
| O8 | full | O7 with the replay on four screens (`four_screen <path>` in the trigger file): a's session has 3 guests, each plays the host's replay and closes when it ends (`flycast-<n>P.log`); then O7's checks | not measured |
| O5 | full | O1, and the network payload (`gdxsv_patch/`) is in RAM: its install logged, current `patch_id` at the network menu, connection not refused | 85 s |
| R3 | full | 4-peer local rollback match with `RBK_SAVE_REPLAY=1`: each peer saves one replay, the four hold the same inputs but the last (the battle-end frame each peer disconnects on: a remote disconnect can arrive before its input there), peer 1's plays to the end with the match's winner | 80 s |
| R4 / R4c | full | 4-peer local rollback match with `RBK_SAVE_REPLAY=1`; the previous release (`cases/compat.py`) plays peer 1's replay: exit 0, every round started, the recorded winners (releases log no stop line) (R4c: inputs cut halfway through the last round, no winner) | 77 s |
| V1 | full | newest Disc 2 replay with `ReplayPOV` 2 and 4: the game stores player side POV-1 (`0x0c3d19d9`) and id POV (`0x0c391d92`) at vblank 4000 | 15 s |
| V2 | full | `replay_target_round=4` on a 5-round replay: one jump, rounds 4 and 5 play to the last input | 50 s |
| V3 | full | `replay_target_frame=4000`: one seek landing on input 4000, then every round with the recorded winners | 110 s |
| V4 | full | `ReplayHideName`: the battle names the players ＰＩＬＯＴ０1-4 (`0x0c3d1b04`, 16 bytes each) | 5 s |
| V5 | full | `ReplayFourScreen=yes` in a window: the host starts 3 guests (one sync group); POV bytes per screen (guests write `e2e_pov<N>.log` via `lua/fourscreen.lua` in `emu.cfg`) are players 1-4; each screen's `RenderResolution` is half of `emu.cfg`'s 960; each guest log joined the host's group and plays its replay | 57 s |
| V6 / V6c | full | `replay_target_frame=3000 replay_takeover_frame=3000` in a window (the countdown runs from the pause-menu render): `StartTakeover` at the newest saved state before 3000 (within 60); `lua/takeover.lua` holds B X Y C Z (MCS 0xb003, in no corpus replay) and InetBuf shows it 60 vblanks in a row; START: `RetryTakeover` at the same input, then the mask again (V6c: no option, every takeover check fails) | 13 s / 28 s |
| V11 | full | `ReplayPOVMode=godseye` on the newest Disc 1 and Disc 2 replays: both play to the end with the recorded winners, the god's eye view installed and shown (log) | not measured |
| G1 / G1c | full | `cases/osd.py`: the 4-peer local match in 640x480 windows; peer 1's client area is grabbed from the screen every 2 s (Windows only, raised to the top; elsewhere `SKIP`). The near-white pixels of the panel's label column (Delay, Roll, player names, Ping, Lag) cover >= 0.5 of `golden/net_osd_labels.png`'s in at least 5 grabs (measured: 0.64-1.00 in battle, 0.00-0.04 without the panel) (G1c: `network:Stats=no`, in no grab) | 68 s / 61 s |
| G2 | full | `cases/browser.py`: Disc 2 in a 640x480 window (100 % display scale); Tab opens the pause menu, mouse clicks open "Live & Replays" and each tab (Windows only, else `SKIP`). Local fakes list one live battle (`/status`) and one server replay (`/replay`), `data/replays` holds a corpus replay; the tab bar and each tab's players line match `golden/browser_*.png` (recall >= 0.9; measured 1.00, 0.00 without rows) | 21 s |
| K1 / K1c | full | `replay_target_frame=6000` with and without `SkipRenderingHack`: both land on input 6000 and play every round with the recorded winners; the hack seeks under 0.85 x the ms per frame (`JumpToKeyMsg` log line; windows-mini 2.0 vs 3.2) (K1c: off in both runs) | 410 s |
| F1 | full | `config:rend.FixedFrequency` 2, 4, 5 from the lobby savestate: 600 vblanks timed by the wall clock (`lua/rate.lua` marks, 10 ms poll) at 59.94, 50, 30 fps within 2 % (measured 59.91, 50.01, 30.00) | 53 s |
| H1 / H1c | full | `SlowIdleLoopHack` yes vs no, `LimitFPS=no` from the lobby savestate, 6000 vblanks timed like F1: the hack over 2 x the fps (measured 931-981 vs 318-333) (H1c: off in both runs) | 27 s / 39 s |
| F2 | full | `FixedFrequency=0 LimitFPS=yes`: the audio output paces the game, 59.94 fps within 2 % (measured 59.98) | 13 s |
| F3 | full | `cases/audio.py`: a window and an audio device; the newest Disc 2 replay for 100 s with `ReplayFixedFrequency=3` (60 Hz, 0.1 % more audio than the device plays); log `Audio queue: ... min ... max ...` every ~10 s, from the third: the queue never 0 (dry) or 1 (full, samples dropped). Measured 0.18-0.91 | 101 s |
| J1 | smoke, full | `cases/joystick.py`: an SDL virtual gamepad (env `GDXSV_TEST_JOYSTICK`, a script of vblank-timed button/axis/hat changes) on the headless lobby savestate; `lua/joystick.lua` logs player 1's `kcode` and stick X. A pressed at vblank 300, released at 360: seen by 303 / 363 | 11 s |
| J3 | smoke (J3), full | a mapping file binds d-pad left/right to the left stick: left -32768, left + right 0, right 32767, none 0, each within 3 vblanks | 11 s |
| J4 | full | Windows: `input:UseXInput=no`, `input:UseDirectInput=no` log `Disabling XInput, using DirectInput` and `Disabling DirectInput` (SDL hints set) | 2 s |
| E1 | smoke | `cases/headless.py`: `gdxsv:headless=yes` on the lobby savestate: Lua reads the game id, rc 0 after 300 vblanks, no renderer start line (DX11, DX9, Vulkan, OpenGL), on Windows no visible window | 6 s |
| D1 / D1c | full | `cases/fpcompat.py`: newest Disc 2 replay with `gdxsv:hashlog=yes`; the replay backend logs a hash of the four player work blocks (`0x0c3d1cd4` + `0x2000` x player) per input to `replay_hashlog.txt`; every 100th line equals `golden/D1_<replay>.hashlog`, made on another host (committed: macos-mini arm64; no fixture: `SKIP`, the run's `D1/D1.hashlog` is the candidate). D1c: the same with `GDXSV_FP_OFF_EXE`, the hashes differ (arm64 and windows-mini x64: from input 600; arm64 stops at 8231 of 30966); needs a CPU that fuses: arm64, or x86 with FMA3 (Rosetta has none: an x86_64 OFF build equals the fixture, D1c fails) | 33 s arm64, 102 s x86_64 Rosetta / 12 s (macos-mini); 104 s / 30 s windows-mini |
| U1 | full | `cases/menu.py`: no ROM, a window (these run in the main menu), `ConnectionMedium=Wired` (the wireless popup would block the others); `gdxsv:UpdateCheckUrl` = a local release JSON with every platform zip: log `update check: latest gdxsv-99.0.0, newer 1, download 1`. Judged by logs: a popup waits behind any other | 1 s |
| X3, X4, X5 | full | `TexturePackChannel=e2e TexturePackUrl=<local>`: X3 `language=2` with no 2D or 3D setting asks only `/e2e-ex.json` and `/e2e-2d-1x.json` and installs both; X4 `TextureResolution2D=2 TextureResolution3D=4 language=1` asks only `/e2e-ex.json`, `/e2e-2d-2x.json` and `/e2e-3d-4x.json` and installs them; X5 `UseTexturePack=yes` (a released build's) with a file `T13306M.texp` in the data dir asks only `/e2e-ex.json`, `/e2e-2d-2x.json` and `/e2e-3d-2x.json`, installs them, and the old file is gone | 1 s |
| X1, X2 | full | `TextureResolution2D=4 TextureResolution3D=2 language=0 TexturePackChannel=e2e TexturePackUrl=<local>`: `/e2e-ex.json`, `/e2e-2d-4x.json` and `/e2e-3d-2x.json` each list a fake pack of its own in 3 chunks (XXH64 in `lib/xxh64.py`); all fetched, `texture pack updated to e2e-1: <channel file>` for each, `T13306M-<layer>.texp` = the bytes served; second start, same dir: `new 0` for each, no chunk fetched (X2: one 2D chunk hash wrong, the 2D pack not installed) | 3 s / 3 s |
| T1 / T1c | full | `cases/font.py`: corpus replay 1788627530784 in a 960x720 window with `FontRaster=yes`; `lua/hold.lua` stalls it at vblank 3600 while the client area is grabbed (Windows only, else `SKIP`). The log has `FontRaster: ... loaded ... N glyph textures`, and the near-white pixels of the HUD pilot name cover >= 0.8 of `golden/fontraster_hud_name.png`'s (measured 1.00; 0.18 with the game's glyphs) (T1c: `FontRaster=no`, both fail) | 48 s / 48 s |
| Y1 / Y1c | full | `cases/rambase.py`, Windows only: headless lobby savestate; `memread.lua` logs 64 bytes at guest 0x8c010000, the runner reads host 0x21c010000 (`emu.read_memory`): equal, and the log `RAM(16 MB) 000000021c000000`. Y1c: the host range is reserved in the suspended process first (`emu.reserve_memory`), flycast falls back to a dynamic base, the read differs | 2 s / 2 s |
| M1 | full | `ConnectionMedium=Wireless`: `wireless warning shown`; `VPN: e2e`: `VPN warning shown: VPN: e2e` | 3 s |
| S1 | full | `rollback/live_rig.py`: 4 peers (30-80 ms, input delay 3, 2 rounds) upload to a local `gdxsv lbs -spectator_test_session`; its recording is one peer's replay, a spectator's state per step equals that replay played offline. Header replay made once per run | 410 s + 115 s |
| S3, S4 | full | S1 (same `GDXSV_SERVER_EXE`) with the previous release (`cases/live.py`, downloaded to `GDXSV_RELEASE_ROOT`, default `out`) as the spectator and its reference play (S3) or as the 4 uploading peers (S4) | 170-240 s each |
| S2 | full | `rollback/fake_live_lbs.py` streams the newest Disc 2 corpus replay as a live battle 5 s into its last round; the spectator jumps to that round, every player's wins, losses and draws (`lua/counters.lua`) equal the earlier rounds' results, then the game's own count after it | 117 s |
| S5 | full | `rollback/fake_live_lbs.py` serves two live battles (newest Disc 2 corpus replay, 5 s into the last round, 30 s live, 3 s apart) to a windowed spectator with `LiveAutoNext=yes`: it logs `live autoplay: moving on to 9000001` and restores battle 2's round counters | 39 s |

W1 is a known failure (`known_failure=`, reported `KNOWN`, exit code 0): on
the default `rend.ThreadedRendering=yes` the widescreen patch is written only when the
emulator stops between frames, so W1 reads stock values. With
`ThreadedRendering=no` it passes, and the runner reports `known failure passed: drop
the mark`. The fix is postponed: it may move the other patches' timing too.

## Adding a case

- A function in `cases/*.py` with `@case("<id>", "<what it checks>", tier=...)`,
  returning `result([{"what": ..., "ok": ...}, ...], **details)`.
- Lua: `lua/<name>.lua` defines `main()`, a coroutine resumed once per vblank;
  parameters arrive as `E2E_PARAMS`. Standard flycast Lua API only.
- Read what the check judges from files in `ctx.out` (logs, `e2e.log` values),
  so `--recheck` can replay it.
- Judge by RAM values, logs and hash logs; an image only for what is visual by
  nature, cropped and downscaled into `golden/`.
- Name the case in `docs/gdxsv/features.md`. Add a control only for an
  agreement or comparison check, and name it there too; else `–` in the
  Control column.
- A failure a human chose not to fix yet: `known_failure="<why>"` on the case (and its control).
