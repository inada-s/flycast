# gdxsv features and their tests

Every feature this fork adds to Flycast, with the tests that cover it.
The game is Mobile Suit Gundam: Federation vs. Zeon DX (Dreamcast, Disc 1 and
Disc 2). Every option is in [options.md](options.md); the changes to
upstream code and the fork point are in [upstream.md](upstream.md).

- A test is a unit test or a rig test: a script that runs one or more
  emulators with the game and checks the result. The game image is not in the
  repository.
- **Control** is the variant of a test that must fail. It shows the test can
  detect the problem.
- "none" means the feature has no automated test.
- This file does not record results. A result belongs to the commit it ran on.
- A feature's PR updates its row; "none" is stated, not left out.

## Where the tests are

| Location | Tests |
|---|---|
| `tests/src/Gdxsv*.cpp` | unit tests (`flycast_tests`, built with `-DBUILD_TESTING=ON`; CI does not run them) |
| `tools/rbk_test/` | `run_suite.py` (rollback cases A1 to D8), `live_rig.py`, `fake_live_lbs.py`, `fuzz.py`; setup and pass criteria in its `README.md` |
| repository root | `run.py` (several local instances side by side) |

## Online play

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Network patch: an SH4 payload (`gdxsv_patch/`) replaces the game's modem stack; the game talks to the host through `gdx_rpc` | always for the game | none | none |
| Lobby connection over TCP (`gdxsv_backend_tcp`) | setting `server` (default `zdxsv.net`) | none | none |
| Login key, generated on first use | setting `loginkey` | none | none |
| Platform info to the lobby (build, OS, CPU, machine id, wireless or not, local and public IPs, UDP port, UPnP and port test results) | always | none | none |
| VPN warning (connection medium) | always online | none | none |
| P2P connectivity test and public IP / UPnP port mapping | first lobby connection | none | none |
| HTTPS latency test to the cloud regions | when the game goes online | none | none |
| Ping test to the battle peers, input delay from it | lobby battle | rollback cases A1 to D8 use it (`TEST_GGPO_DELAY` replaces the delay) | none |
| Relay through peers and relay servers, relay RTTs passed on between peers | lobby battle info | `run_suite.py` D2 to D8 (D4 to D8: local `gdxsv relay`); unit: `GdxsvPingPongTest.*` | none |
| Battle without GGPO over UDP (`gdxsv_backend_udp`) | battle info without rollback | none | none |
| Network status OSD | in battle | none | none |

## Rollback netcode (GGPO)

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Rollback battle (`gdxsv_backend_rollback`, `core/network/ggpo.cpp`, `core/deps/ggpo`) | lobby battle | `run_suite.py`: A1 basic, A2 20 rounds (`--long`), B2/B3 threaded rendering on/off | none |
| Compatibility with older releases | always | `run_suite.py` A3, A4 (`--old-release`, `--older-release`) | none |
| Timesync skips replayed in a rollback | always | `run_suite.py` B1 (`GGPO_TEST_LOG=1`) | none |
| Peer drop, session start timeout, re-battle cancel | always | `run_suite.py` C1, C2, C3 | none |
| Malformed packets dropped | always | `run_suite.py` D1 (`fuzz.py`) | none |
| Local test match without servers | `gdxsv:rbk_test=N/4`, `gdxsv:rand_input=SEED` | it is the tool of the cases above | none |
| SH4 `fmac` as `fmul` + `fadd` on every CPU and backend (same results across hosts) | CMake `GDXSV_FP_COMPAT` (on) | none | none |

## Replays

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Save a battle as a replay, upload it | settings `SaveReplay`, `UploadReplay`, `ReplayPath` | `RBK_SAVE_REPLAY` saves a local match (never uploaded) | none |
| Replay browser: Local, Server, Live tabs | menu "Live & Replays" | none | none |
| Play a replay file | menu, or `gdxsv:replay=FILE` | none | none |
| Playback controls: pause (A), step or seek (Left/Right), speed (Up/Down), round jump, control bar | during a replay | unit: `GdxsvReplayInputTest.*`, `GdxsvReplayUi.*` | none |
| Takeover: play on from a replay frame, retry with START | pause menu | none | none |
| Round results kept across seeks and round jumps | always | unit: `GdxsvRoundCountersTest.*` | none |
| Key display, Ally HP, hidden names, skip mobile suit selection | settings `ReplayKeyDisplay`, `ReplayShowAllyHP`, `ReplayHideName`, `ReplaySkipMsSelection` | none | none |
| Point of view | `gdxsv:ReplayPOV` | none | none |
| Four-screen replay: 4 processes in a 2x2 grid, kept on one frame | `gdxsv:ReplayFourScreen=yes` | none | none |
| Audio rate control (resample up to 0.5 % to hold the queue half full) | replay and spectating without audio sync | none | none |
| Fast seek: output-only game functions return early on seek frames | setting `SkipRenderingHack` | none | none |

## Live spectating

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Uplink: one battle peer streams its confirmed inputs to the lobby | lobby battle | `live_rig.py`: lobby's recording equals that peer's replay | none |
| Spectator: download, catch up, follow live | Live tab, or `gdxsv:spectate=CODE` | `live_rig.py`: each spectator's state per step equals the replay played offline; unit: `GdxsvSpectator.*` | none |
| Live auto-next (watch the next live battle) | setting `LiveAutoNext` | `fake_live_lbs.py` (by hand, see `tools/rbk_test/README.md`) | none |

## Game and presentation

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Translation patch (Japanese, Cantonese, English) | setting `language` | none | none |
| Texture pack, downloaded and updated | settings `UseTexturePack`, `TexturePackChannel` | none | none |
| Widescreen HUD layout | setting `WidescreenHudLayout` | none | none |
| Slow idle loop hack | setting `SlowIdleLoopHack` | none | none |
| Projectile view (debug overlay, Disc 2) | setting `ProjectileView` (ini only) | none | none |
| Arcade slowdown (Disc 2) | compiled off (`GdxsvSlowdown::kEnabled`) | none | none |

## Frame pacing and input

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Fixed frame rate with a precise sleep (59.94, 60, 50, 30 Hz, or by cable) | setting `config:rend.FixedFrequency` | none | none |
| Audio sync as a setting | setting `config:aica.LimitFPS` | none | none |
| Joystick polling on the emulation thread; XInput / DirectInput switches | settings `input:JoystickPolling`, `input:UseXInput`, `input:UseDirectInput` | none | none |
| Opposite directions cancel (SOCD) for the game | always for the game | none | none |
| gdxsv defaults on a new config file | first start | none | none |

## Releases and tools

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Update check against the releases of `inada-s/flycast`, a popup offers the new version | on start | none | none |
| Release build from a `gdxsv-*` tag | push a tag | the workflow itself | none |
| CI build (macOS, Linux, Windows MinGW) | every push and PR | the workflow itself | none |
| Headless mode (no window, no graphics API) | `gdxsv:headless=yes` | most rollback cases run headless | none |
| Boot probe for patch checks | `gdxsv:headless_loadstate=yes`, `gdxsv:headless_probe_frames=N` | none | none |
| Per-frame state hash log | `gdxsv:hashlog=yes` | read by `tools/rbk_test` (`rbk_lib.py`) | none |
| `run.py` (several instances, replay, spectate, rollback test) and the `RunGame` workflow | `python run.py <function>` | none | none |
| Fixed host address of guest RAM on Windows (lower 28 bits equal the guest address) | always | none | none |
