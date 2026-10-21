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
| `gdxsv_tests/` | `run.py` e2e cases by id (P1, R2, ...), tiers `smoke` and `full`; see its `README.md` |
| `gdxsv_tests/rollback/` | `run_suite.py` (rollback cases A1 to D8), `live_rig.py`, `fake_live_lbs.py`, `fuzz.py`; setup and pass criteria in its `README.md` |
| repository root | `run.py` (several local instances side by side) |

## Online play

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Network patch: an SH4 payload (`gdxsv_patch/`) replaces the game's modem stack; the game talks to the host through `gdx_rpc` | always for the game | O5: installed after the state load, the login runs through it | O5c (`patch_id` never current: no login) |
| Lobby connection over TCP (`gdxsv_backend_tcp`) | setting `server` (default `zdxsv.net`) | O1: login and plaza on a local `gdxsv lbs` | O1t (`server` with no lobby: no connection) |
| Login key, generated on first use | setting `loginkey` | O1: the lobby finds the account by the key | O1c (unknown key) |
| Platform info to the lobby (build, OS, CPU, machine id, wireless or not, local and public IPs, UDP port, UPnP and port test results) | always | O1: received by the lobby (CPU, payload `patch_id`, public IP, port test result) | O1i (another public IP answer shows up in it) |
| Wireless warning popup and VPN warning toast (connection medium) | main menu; `gdxsv:ConnectionMedium` overrides the detected adapter | M1: Wireless logs the popup, `VPN: e2e` the toast with the adapter | M1c |
| P2P connectivity test and public IP / UPnP port mapping | first lobby connection | O1: public IP and P2P test requests reach the local server, its UDP "Hello" makes the port test `Success` (UPnP off) | O1p (no Hello: `Failed (Timeout)`) |
| HTTPS latency test to the cloud regions | when the game goes online | none | none |
| Ping test to the battle peers, input delay from it | lobby battle | rollback cases A1 to D8 use it (`TEST_GGPO_DELAY` replaces the delay); I1: the delay setting reaches the game's input buffer | I1c |
| Relay through peers and relay servers, relay RTTs passed on between peers | lobby battle info | `run_suite.py` D2 to D8 (D4 to D8: local `gdxsv relay`); unit: `GdxsvPingPongTest.*` | D2c to D8c |
| Battle without GGPO over UDP (`gdxsv_backend_udp`) | battle info without rollback | O2: one player, local battle server | O2c |
| Network status OSD | in battle (setting `network:Stats`) | G1: the stats panel in a window grab (golden label mask) | G1c |

## Rollback netcode (GGPO)

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Rollback battle (`gdxsv_backend_rollback`, `core/network/ggpo.cpp`, `core/deps/ggpo`) | lobby battle | `run_suite.py`: A1 basic, A2 20 rounds (`--long`), B2/B3 threaded rendering on/off; O3: from a local gdxsv P2P lobby, O4: two clients | A1c, B2c, B3c, O3c, O4c |
| Compatibility with older releases | always | `run_suite.py` A3, A4 (`--old-release`, `--older-release`); live spectating: S3 (spectator), S4 (uploading peers) on the previous release | A3c, A4c, S3c, S4c |
| Timesync skips replayed in a rollback | always | `run_suite.py` B1 (`GGPO_TEST_LOG=1`) | B1c |
| Peer drop, session start timeout, re-battle cancel | always | `run_suite.py` C1, C2, C3 | C1c, C2c, C3c |
| Malformed packets dropped | always | `run_suite.py` D1 (`fuzz.py`) | D1c |
| Local test match without servers | `gdxsv:rbk_test=N/4`, `gdxsv:rand_input=SEED` | it is the tool of the cases above | none |
| SH4 `fmac` as `fmul` + `fadd` on every CPU and backend (same results across hosts) | CMake `GDXSV_FP_COMPAT` (on) | `gdxsv_tests` D1 (fixture: macos-mini arm64; equal on its x86_64 build and on windows-mini x64) | `gdxsv_tests` D1c |

## Replays

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Desync check: a round that starts with another RNG than recorded stops playback with an error dialog; headless runs exit rc 4 | always | no test: needs a replay that does not reproduce from its inputs | none |
| Save a battle as a replay, upload it | settings `SaveReplay`, `UploadReplay`, `ReplayPath` | R3: a local rollback match (`RBK_SAVE_REPLAY`, never uploaded) saves one replay per peer, the same inputs, it plays to the end with the match's winner | R3c (`SaveReplay=no`) |
| Replay browser: Local, Server, Live tabs | menu "Live & Replays" | G2: each tab lists its fake or local entry (window grabs, golden images) | G2c |
| Play a replay file | menu, or `gdxsv:replay=FILE` | R1 (every replay format), R2: plays to the end with the recorded winners | R2c |
| Playback controls: pause (A), step or seek (Left/Right), speed (Up/Down), round jump, control bar | during a replay | unit: `GdxsvReplayInputTest.*`, `GdxsvReplayUi.*`; V2, V3: round jump and seek at start (`replay_target_round`, `replay_target_frame`) | V2c, V3c |
| Takeover: play on from a replay frame, retry with START | pause menu, or `gdxsv:replay_takeover_frame` | V6: the pad plays from the frame, START retries from it | V6c |
| Round results kept across seeks and round jumps | always | S2: a spectator joining a live battle in its last round sees each player's wins, losses and draws of the earlier rounds (`fake_live_lbs.py`); unit: `GdxsvRoundCountersTest.*` | S2c |
| Key display, Ally HP, hidden names, skip mobile suit selection | settings `ReplayKeyDisplay`, `ReplayShowAllyHP`, `ReplayHideName`, `ReplaySkipMsSelection` | V4: hidden names | V4c |
| Point of view | `gdxsv:ReplayPOV` | V1: the game plays as the chosen player | V1c |
| Four-screen replay and live spectating: the original window displays a 2x2 grid, with three hidden processes sending video through Syphon (macOS OpenGL/Vulkan) or Spout (Windows OpenGL/DirectX 11, or a Vulkan host with DirectX 11 guests on the same GPU). macOS guests do not take keyboard focus; their icons may flash briefly at startup before disappearing from the Dock and application switcher. Hidden guests exit on startup failure or when their host closes. The 1P key display fits its quadrant; other HUD elements and replay controls span the full window. Only the host's FPS counter is shown, when enabled; it measures the main window's drawing rate, not each guest's progress. VMU overlays are hidden on all four screens during playback; the host's saved setting is restored when the session ends. The host keeps its normal resizing, maximize and full-screen controls. Windows Vulkan reception requires Vulkan 1.1 and checks DirectX texture import support and matching GPU selection before loading a four-screen replay or launching guests, and again if the graphics device changes during playback. Its receiving device is released at session end; ordinary playback creates none. These receivers keep the last good picture of a missing or failed sender with a status message, including while reconnecting. A removed DirectX receiving device discards its invalid textures but keeps the grid, shows the cause and retries up to three times. Unsupported renderers disable the four-screen controls. A failed video-sharing check shows its reason and starts nothing from the browser; command-line launches fall back to single-screen playback. A terminal sharing failure during playback, including exhausted device-reset retries or an unsupported Windows renderer fallback, ends the replay with an error and closes its hidden guests. A new four-screen launch retries the check. | Replay/Watch (4 screens), or `gdxsv:ReplayFourScreen=yes` | V5: 3 guests on one sync group, each screen plays as its player. V7: the host window grid shows four live viewpoints, each with the battle timer in place. V8: closing the host window ends every guest within 15 s. V9: `replay_target_round=3` starts all four screens at round 3; there, host speed up, seek back and seek forward reach the guests; all four processes play into round 4. Windows only (V7-V9) | V5c, V7c, V8c, V9c |
| Quieter lock-on warning, roughly half amplitude depending on the game's SFX setting; pan, repeat timing and flashing HUD stay stock | Replay/Watch (4 screens) | V5 (Disc 2): all four screens use warning gain selector 20; Disc 1: no test | V5c: single-screen replay keeps stock selector 32 and fails the quiet-warning checks |
| Named video output for external viewers | video routing enabled, with `gdxsv:VideoRoutingName` | none | none |
| Four-screen audio: all viewpoints mixed at matching replay positions, delayed by three displayed frames; offline briefing input resumes together with a bounded wait | four-screen replay with `MultiPovAudioSync` | `flycast --gtest_filter=GdxsvMultiPovAudioTest.*` with `ENABLE_CTEST=ON` (requires POSIX fork): viewpoint impulses align at 1×/2×/3×, a sound tied to frames since restore plays once while paused, briefing requests release or time out while audio continues, queues recover | `GdxsvMultiPovAudioTest.RejectsDelayedCopy`: delayed guest impulses must fail the PCM comparison |
| Audio rate control (resample up to 0.5 % to hold the queue half full) | replay and spectating without audio sync; `gdxsv:AudioRateControl` | F3: a windowed replay paced at 60 Hz, the queue never runs dry or full | F3c |
| Fast seek: output-only game functions return early on seek frames | setting `SkipRenderingHack` | K1: same seek and outcome, under 0.85 x the ms per frame without the hack | K1c |

After a paused seek, guest audio that arrives outside the mixer's delay is
omitted while paused. After resuming, music from guests that restored late can
remain behind even when their replay inputs match. The mixer does not run extra
emulated frames to catch up that sound playback.

## Live spectating

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Uplink: one battle peer streams its confirmed inputs to the lobby | lobby battle | S1 (`live_rig.py`): lobby's recording equals that peer's replay | S1c |
| Spectator: download, catch up, follow live | Live tab, or `gdxsv:spectate=CODE` | S1 (`live_rig.py`): each spectator's state per step equals the replay played offline; unit: `GdxsvSpectator.*` | S1c |
| Live auto-next (watch the next live battle) | setting `LiveAutoNext` | S5: `fake_live_lbs.py` with two battles, the spectator moves on to the second and joins it | S5c (`LiveAutoNext=no`) |

## Game and presentation

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Translation patch (Japanese, Cantonese, English) | setting `language` | L1 | L1c |
| Game patches (`gdxsv_patch.inc`), rewritten every vblank | always for the game | P1 (Disc 1, Disc 2) | P1c |
| NAOMI version (`gundmxgd`) boots | the NAOMI game files | N1 | N1c |
| Texture pack, downloaded and updated | settings `UseTexturePack`, `TexturePackChannel` | X1: a local channel, 3 chunks checked by hash and installed; the next start finds it current (`gdxsv:TexturePackUrl`); the pack is not loaded in a game (a fake pack) | X1c |
| Glyph-cell textures (DC2 battle name sheet) | on for `T13306M` when the downloaded texture pack (Use Texture Pack) or the embedded textures replace 32x32 glyphs; a `data/textures/T13306M` folder does not apply to cells | no committed test; manual: a replay with and without a glyph pack, the in-battle pilot names change | the same replay without the pack |
| Game font glyphs drawn from an outline font (DC2) | setting `FontRaster` | no test: a rig run checks the log line `FontRaster: ... loaded` and the pilot name on screen; table from `gdxsv_langmod/gen_font_glyphs.py` | `FontRaster=no` shows the bitmap glyph |
| Widescreen HUD layout | setting `WidescreenHudLayout` | W1 (known failure: not written under `rend.ThreadedRendering=yes`, the default) | W1c (known failure) |
| Slow idle loop hack | setting `SlowIdleLoopHack` | H1: unthrottled, over 2 x the fps without the hack | H1c |
| Projectile view (debug overlay, Disc 2) | setting `ProjectileView` (ini only) | none | none |
| Arcade slowdown (Disc 2) | compiled off (`GdxsvSlowdown::kEnabled`) | none | none |

## Frame pacing and input

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Fixed frame rate with a precise sleep (59.94, 60, 50, 30 Hz, or by cable) | setting `config:rend.FixedFrequency` | F1: 59.94, 50, 30 fps by the wall clock | F1c |
| Audio sync as a setting | setting `config:aica.LimitFPS` | F2: audio paces the game at 59.94 fps | F2c |
| Joystick polling on the emulation thread; XInput / DirectInput switches | settings `input:JoystickPolling`, `input:UseXInput`, `input:UseDirectInput` | J1 (polling on), J2 (off): SDL virtual gamepad; J4: the two switches (Windows) | J1c, J4c |
| Opposite directions cancel (SOCD) for the game | always for the game | J3: SDL virtual gamepad, d-pad bound to the left stick | J3c |
| gdxsv defaults on a new config file | first start | C1: the defaults hold after the game start | C1c (an `emu.cfg` already there) |
| Settings from an older release's `emu.cfg` (gdxsv-1.9.6 layout) | always | C2: flycast and gdxsv values hold after the game start | C2c (sections renamed) |
| Gdx UDP Port out of 1-65535 replaced by a random port | game start, settings screen | C3: 70000 and -5 from `emu.cfg` | C3c (65535 kept) |

## Releases and tools

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Update check against the releases of `inada-s/flycast`, a popup offers the new version | on start (`gdxsv:UpdateCheckUrl`) | U1: a newer release on a local release API is offered with its download | U1c |
| Release build from a `gdxsv-*` tag | push a tag | the workflow itself | none |
| CI build (macOS, Linux, Windows MinGW) | every push and PR | the workflow itself | none |
| Headless mode (no window, no graphics API) | `gdxsv:headless=yes` | E1; most rollback cases run headless | E1c |
| Boot probe for patch checks | `gdxsv:headless_loadstate=yes`, `gdxsv:headless_probe_frames=N` | B1: resumes the lobby state, exits rc 0 after N vblanks | B1c |
| Per-frame state hash log | `gdxsv:hashlog=yes` | Q1: a local rollback match logs every frame per peer, and each rollback's frames again (read by `gdxsv_tests/lib/hashlog.py`, rollback suite) | Q1c (`hashlog=no`) |
| `run.py` (several instances, replay, spectate, rollback test) and the `RunGame` workflow | `python run.py <function>` | none | none |
| Fixed host address of guest RAM on Windows (lower 28 bits equal the guest address) | always | Y1: guest code read from outside at host 0x21c010000 | Y1c |
