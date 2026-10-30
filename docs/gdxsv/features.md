# gdxsv features and their tests

Every feature this fork adds to Flycast, with the tests that cover it.
The game is Mobile Suit Gundam: Federation vs. Zeon DX (Dreamcast, Disc 1 and
Disc 2). Every option is in [options.md](options.md); the changes to
upstream code and the fork point are in [upstream.md](upstream.md).

- A test is a unit test or a rig test: a script that runs one or more
  emulators with the game and checks the result. The game image is not in the
  repository.
- **Control** is the variant of a test that must fail. It shows the test can
  detect the problem. Only agreement and comparison checks have one; "–" in
  the Control column means the check needs none.
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
| Network patch: an SH4 payload (`gdxsv_patch/`) replaces the game's modem stack; the game talks to the host through `gdx_rpc` | always for the game | O5: installed after the state load, the login runs through it | – |
| Lobby connection over TCP (`gdxsv_backend_tcp`) | setting `server` (default `zdxsv.net`) | O1: login and plaza on a local `gdxsv lbs` | – |
| Login key, generated on first use | setting `loginkey` | O1: the lobby finds the account by the key | – |
| Platform info to the lobby (build, OS, CPU, machine id, wireless or not, local and public IPs, UDP port, UPnP and port test results) | always | O1: received by the lobby (CPU, payload `patch_id`, public IP, port test result) | – |
| Wireless warning popup and VPN warning toast (connection medium) | main menu; `gdxsv:ConnectionMedium` overrides the detected adapter | M1: Wireless logs the popup, `VPN: e2e` the toast with the adapter | – |
| P2P connectivity test and public IP / UPnP port mapping | first lobby connection | O1: public IP and P2P test requests reach the local server, its UDP "Hello" makes the port test `Success` (UPnP off); O6: no Hello makes it `Failed (Timeout)` | – |
| HTTPS latency test to the cloud regions | when the game goes online | none | none |
| Ping test to the battle peers, input delay from it | lobby battle | rollback cases A1 to D8 use it (`TEST_GGPO_DELAY` replaces the delay); I1: the delay setting reaches the game's input buffer | I1c |
| Rollback budget per PC: the input delay is max(2, `MinDelay`, ceil(one-way latency in frames − `RollbackBudget`)). At a session end of 1800 frames or more, `RollbackBudget` moves halfway to clamp(median fit − 1, 0, 1.5), where the fit of a frame is its `rollback_slack` over its emulation time (work less rollback): the frames of rollback it has room for, a rollback frame taken to cost as much as an emulated one. Saved in `emu.cfg`; a value from `--config` is not saved. P2PMatchingReport `cfg_rollback_budget`: the value at the battle start, before the clamp | always (rollback battle) | `run_suite.py` E1: a battle from fresh directories uses 1.00, and its end logs and saves the update | – |
| Relay through peers and relay servers, relay RTTs passed on between peers | lobby battle info | `run_suite.py` D2 to D8 (D4 to D8: local `gdxsv relay`); unit: `GdxsvPingPongTest.*` | – |
| Rollback battle sockets read and relayed packets forwarded on a network thread, also during a rollback | always (rollback battle) | `run_suite.py` A1 to D8 (inputs and hashes agree; D2 to D8 relay through it); no test of the read gap during a rollback | – |
| Battle without GGPO over UDP (`gdxsv_backend_udp`) | battle info without rollback | O2: one player, local battle server | – |
| Network status OSD | in battle (setting `network:Stats`) | G1: the stats panel in a window grab (golden label mask) | G1c |

## Rollback netcode (GGPO)

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Rollback battle (`gdxsv_backend_rollback`, `core/network/ggpo.cpp`, `core/deps/ggpo`) | lobby battle | `run_suite.py`: A1 basic, A2 20 rounds (`--long`), B2 windowed; O3: from a local gdxsv P2P lobby, O4: two clients | A1c |
| Compatibility with older releases | always | `run_suite.py` A3, A4 (`--old-release`, `--older-release`); live spectating: S3 (spectator), S4 (uploading peers) on the previous release | A1c (same checks) |
| Timesync skips replayed in a rollback | always | `run_suite.py` B1 (`GGPO_TEST_LOG=1`, `TEST_FAKE_TIMESYNC=20`) | – |
| Peer drop, session start timeout, re-battle cancel | always | `run_suite.py` C1, C2, C3 | – |
| Malformed packets dropped | always | `run_suite.py` D1 (`fuzz.py`) | – |
| Local test match without servers | `gdxsv:rbk_test=N/4`, `gdxsv:rand_input=SEED` | it is the tool of the cases above | none |
| SH4 `fmac` as `fmul` + `fadd` on every CPU and backend (same results across hosts) | CMake `GDXSV_FP_COMPAT` (on) | `gdxsv_tests` D1 (fixture: macos-mini arm64; equal on its x86_64 build and on windows-mini x64) | `gdxsv_tests` D1c |

## Replays

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Desync check: a round that starts with another RNG than recorded stops playback with an error dialog; headless runs exit rc 4 | always | no test: needs a replay that does not reproduce from its inputs | none |
| Save a battle as a replay, upload it | settings `SaveReplay`, `UploadReplay`, `ReplayPath` | R3: a local rollback match (`RBK_SAVE_REPLAY`, never uploaded) saves one replay per peer, the same inputs, it plays to the end with the match's winner | – |
| Replay browser: Local, Server, Live tabs | menu "Live & Replays" | G2: each tab lists its fake or local entry (window grabs, golden images) | – |
| Play a replay file | menu, or `gdxsv:replay=FILE` | R1 (every replay format), R2 (smoke tier: the newest Disc 2 replay): plays to the end with the recorded winners | R2c |
| Replays and Live from the online lobby: the lobby connection stays open, lineCheck is answered, the server's messages are held (counts and room states: the latest per key; chat: the last 32 lines; the rest in order) and passed to the game when playback ends and the lobby state is loaded back; playback ends in the lobby game, not the replay browser (which shows a playback error). Four-screen guests share the host's data directory and do not save slot 90 over it. Online (lobby or battle), Eject Disk is disabled: the game would reboot to the BIOS. From the game's match entry (its own `lbsLobbyMatchingEntry`), or from creating or entering a room for a partner (`lbsRoomCreate`, `lbsRoomEntry`), until it opens the battle connection or the lobby again, cancels or leaves (`lbsRoomExit`, a member's `lbsMatchingEntry` 0, `lbsRoomRemove`, `lbsLobbyExit`) or gets an error answer, the menu does not open and fast-forward is off | menu "Live & Replays" in the lobby | O7: two clients in a P2P lobby; a plays the last round of a corpus replay from the plaza (`GDXSV_TEST_START_REPLAY`), its lobby resumes and it joins lobby 2; from the entry to the mobile suit select the start is refused, also between the match and the battle connection; the two-peer battle follows. O8: the same on four screens, the three guests close when it ends. Not checked: Live from the lobby | – |
| Cost 300->295: a lobby game patch (gdxsv `m_patch`), no longer written by the emulator. Replays before `log_file_version` 20261023 and live battles from older lobby servers get 295 from the replay backend. A cached slot-99 savestate published before (cost 295 in RAM) is downloaded again | lobby setting `patch_names` | R5: older replays (Disc 1, Disc 2) and a 20261023 copy with the patch read 295 | – |
| Playback controls: pause (A), step or seek (Left/Right), speed (Up/Down), round jump, control bar | during a replay | unit: `GdxsvReplayInputTest.*`, `GdxsvReplayUi.*`; V2, V3: round jump and seek at start (`replay_target_round`, `replay_target_frame`) | – |
| Mapped Fast-forward: toggles 100%/300%, wakes the control bar after the battle starts; ignored during loading, menus and takeover; replay startup clears Flycast's inherited fast-forward | during replay, Flycast Fast-forward binding (Space by default) | `python gdxsv_tests/run.py --exe EXE --cases V10,V10c` (Windows/macOS): after a round jump and frame seek, a held press selects 300% once, the next restores 100%, and a menu press cannot change the speed | – |
| Takeover: play on from a replay frame, retry with START | pause menu, or `gdxsv:replay_takeover_frame` | V6: the pad plays from the frame, START retries from it | V6c |
| Round results kept across seeks and round jumps | always | S2: a spectator joining a live battle in its last round sees each player's wins, losses and draws of the earlier rounds (`fake_live_lbs.py`); unit: `GdxsvRoundCountersTest.*` | – |
| Key display, Ally HP, hidden names, skip mobile suit selection | settings `ReplayKeyDisplay`, `ReplayShowAllyHP`, `ReplayHideName`, `ReplaySkipMsSelection` | V4: hidden names | – |
| Point of view | `gdxsv:ReplayPOV` | V1: the game plays as the chosen player | – |
| Four-screen replay and live spectating: the original window displays a 2x2 grid, with three hidden processes sending video through Syphon (macOS OpenGL/Vulkan) or Spout (Windows OpenGL/DirectX 11, or a Vulkan host with DirectX 11 guests on the same GPU). macOS guests do not take keyboard focus; their icons may flash briefly at startup before disappearing from the Dock and application switcher. Each macOS guest keeps a silent audio output running, so Game Mode, which a full-screen host turns on, does not throttle it. Hidden guests exit on startup failure or when their host closes. The 1P key display fits its quadrant; other HUD elements and replay controls span the full window. Only the host's FPS counter is shown, when enabled; it measures the main window's drawing rate, not each guest's progress. VMU overlays are hidden on all four screens during playback; the host's saved setting is restored when the session ends. All four screens render at half the host's internal resolution, at least 240 lines (Half); the host's own setting is restored when the session ends. The host keeps its normal resizing, maximize and full-screen controls. Windows Vulkan reception requires Vulkan 1.1 and checks DirectX texture import support and matching GPU selection before loading a four-screen replay or launching guests, and again if the graphics device changes during playback. Its receiving device is released at session end; ordinary playback creates none. These receivers keep the last good picture of a missing or failed sender with a status message, including while reconnecting. A removed DirectX receiving device discards its invalid textures but keeps the grid, shows the cause and retries up to three times. Unsupported renderers disable the four-screen controls. A failed video-sharing check shows its reason and starts nothing from the browser; command-line launches fall back to single-screen playback. A terminal sharing failure during playback, including exhausted device-reset retries or an unsupported Windows renderer fallback, ends the replay with an error and closes its hidden guests. A new four-screen launch retries the check. | Replay/Watch (4 screens), or `gdxsv:ReplayFourScreen=yes` | V5: 3 guests on one sync group, each screen plays as its player at half the configured resolution. V7: the host window grid shows four live viewpoints, each with the battle timer in place. V8: closing the host window ends every guest within 15 s. V9: `replay_target_round=3` starts all four screens at round 3; there, host speed up, seek back and seek forward reach the guests; all four processes play into round 4. Windows only (V7-V9) | V7c |
| Quieter lock-on warning, roughly half amplitude depending on the game's SFX setting; pan, repeat timing and flashing HUD stay stock | Replay/Watch (4 screens) | V5 (Disc 2): all four screens use warning gain selector 20; Disc 1: no test | – |
| Named video output for external viewers | video routing enabled, with `gdxsv:VideoRoutingName` | none | none |
| Four-screen audio: all viewpoints aligned at matching replay positions, delayed by three displayed frames; paused steps track continuous guest audio; stalled or overflowing guests rebuffer with short fades, without changing guest pitch; short freezes retain buffered alignment; resume neither repeats nor stretches a partially consumed packet; offline briefing input resumes together with a bounded wait | four-screen replay with `MultiPovAudioSync` | `flycast --gtest_filter=GdxsvMultiPovAudioTest.*` with `ENABLE_CTEST=ON` (requires POSIX fork): viewpoint impulses align at 1×/2×/3×, uneven paused steps and packet sizes preserve all four tones, stalls and overflow recover without persistent gaps, frame-varying markers remain aligned through short host freezes, partial packets neither repeat nor stretch on resume, gaps introduce no abrupt PCM edges, guest restores invalidate old audio clocks and play a sound once, briefing requests release or time out while audio continues, concurrent playback snapshots stay coherent; `GdxsvReplayInputTest.*` also checks headless paused following and pending seeks | `GdxsvMultiPovAudioTest.RejectsDelayedCopy`: delayed guest impulses must fail the PCM comparison |
| Audio rate control (resample up to 0.5 % to hold the queue half full) | replay and spectating without audio sync; `gdxsv:AudioRateControl` | F3: a windowed replay paced at 60 Hz, the queue never runs dry or full | – |
| Fast seek: output-only game functions return early on seek frames | setting `SkipRenderingHack` | K1: same seek and outcome, under 0.85 x the ms per frame without the hack | K1c |

After a paused seek, guest audio that arrives outside the mixer's delay is
omitted while paused. Late restores, and host freezes or guest stalls while
paused, leave the independently emulated music at different positions. Paused
buffering can keep a short host freeze aligned until resume; longer freezes
rebuffer without synchronizing the music. An offset can be audible for the
rest of the hold and after resume, even when replay inputs match. The mixer
does not run extra emulated frames to catch up that sound playback.

## Live spectating

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Uplink: one battle peer streams its confirmed inputs to the lobby | lobby battle | S1 (`live_rig.py`): lobby's recording equals that peer's replay | – |
| Spectator: download, catch up, follow live | Live tab, or `gdxsv:spectate=CODE` | S1 (`live_rig.py`): each spectator's state per step equals the replay played offline; unit: `GdxsvSpectator.*` | – |
| Live auto-next (watch the next live battle) | setting `LiveAutoNext` | S5: `fake_live_lbs.py` with two battles, the spectator moves on to the second and joins it | – |

## Game and presentation

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Translation patch (Japanese, Cantonese, English) | setting `language` | L1 | – |
| Game patches (`gdxsv_patch.inc`), rewritten every vblank | always for the game | P1 (Disc 1, Disc 2) | – |
| NAOMI version (`gundmxgd`) boots | the NAOMI game files | N1 | – |
| Texture pack, downloaded and updated | settings `UseTexturePack`, `TexturePackChannel` | X1: a local channel, 3 chunks checked by hash and installed; the next start finds it current (`gdxsv:TexturePackUrl`); the pack is not loaded in a game (a fake pack); X2: one chunk with a wrong hash, the pack is not installed | – |
| Glyph-cell textures (DC1/DC2 battle name sheet) | on for `T13306M` when `FontRaster`; only `FontRaster` draws the cells: the texture pack's and the embedded textures' 32x32 glyph replacements do not apply to cells, nor does a `data/textures/T13306M` folder | T1 (cells drawn by `FontRaster`) | T1c |
| Game font glyphs drawn from GDXGothic (DC1/DC2): text with the game's edge, in-battle pilot names (cells) smaller with a dark outline, the battle-message sheet's characters at the names' size over the sheet of the embedded textures or the texture pack when one has it, else over the game's own sheet read from VRAM (its emblems and labels) | setting `FontRaster` (Use High-Resolution Font) | T1 (Disc 2): the font loads and the battle HUD pilot name matches the outline-font golden; table from `gdxsv_langmod/gen_font_glyphs.py`; the battle-message sheet: no committed test, manual: a replay's INFORMATION panel (命中, 撃破) in GDXGothic, with and without a texture pack; Disc 1: no committed test, manual: menus, in-battle pilot names and the INFORMATION panel in GDXGothic | T1c: `FontRaster=no`: no font loaded, the name check fails |
| Widescreen HUD layout | setting `WidescreenHudLayout` | W1 (under the default `rend.ThreadedRendering=yes`: a change stops the SH4 once so that the patch goes in between SH4 runs) | – |
| Slow idle loop hack | setting `SlowIdleLoopHack` | H1: unthrottled, over 2 x the fps without the hack | H1c |

## Frame pacing and input

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Fixed frame rate with a precise sleep (59.94, 60, 50, 30 Hz, or by cable): each frame waits a period from the last wake, so a late frame is not paid back by shorter ones | setting `config:rend.FixedFrequency` | F1: 59.94, 50, 30 fps by the wall clock | – |
| Audio sync as a setting | setting `config:aica.LimitFPS` | F2: audio paces the game at 59.94 fps | – |
| XInput / DirectInput switches. Joysticks are read from SDL events on the main thread; the former `input:JoystickPolling` (polling from the emulation thread) is removed and ignored in a config file | settings `input:UseXInput`, `input:UseDirectInput` | J1: SDL virtual gamepad; J4: the two switches (Windows) | – |
| Rollback battle frame order: the local input is read and added before `ggpo_advance_frame` (save, rollback), not after it; same target frame (GGPO frame delay +1) and prediction limit (the barrier is waited for after the advance). Upstream netplay keeps upstream's order | always in a gdxsv rollback battle, no setting | `run_suite.py` A1, B2: the match completes and agrees; the input timing is not checked | – |
| Rollback battle: the `FixedFrequency` frame limiter runs on the emulation thread after `ggpo::nextFrame`; the render thread presents each queued frame at once, without the limiter | always in a gdxsv rollback battle, no setting | `run_suite.py` B2 (the frame times are not checked) | none |
| Threaded rendering renders from snapshots: each frame is rendered from a copy of VRAM, palette and fog table taken when it is queued, so the emulation thread never waits for the render thread (render end, rollback VRAM load, full queue); a new frame replaces a queued one; outside a rollback battle the frame limiter paces the emulation thread at each vblank, whether the game renders that frame or not (with no fixed rate and no audio sync it runs at the native rate, unthrottled when headless). Full framebuffer emulation and render-to-texture copy to VRAM are off with threaded rendering | `config:rend.ThreadedRendering` | `run_suite.py` A1, B2 with `GDXSV_VRAM_TRACK_CHECK=1` (the copy equals VRAM at every capture) | none |
| Threaded rendering, render thread: while the game runs, frames are rendered and presented on a render thread and the main thread only reads the window events (`SDL_WaitEventTimeout`), so a Present that waits for the display does not delay the input; the GUI, renderer changes and exit stop the render thread and run on the main thread as before | `config:rend.ThreadedRendering` | `run_suite.py` A1, B2; replays R1, R2, V3, E1. Not checked: macOS, Linux, opening the menu, resizing, renderer changes during a game | none |
| Threaded rendering presents the newest frame: a frame that waits for the vblank does not hold back the next one. DirectX 11: a frame latency waitable swap chain (3 buffers, latency 1), the render thread waits for it before it takes a frame, Present with sync interval 0 (no tearing without `DXGI_PRESENT_ALLOW_TEARING`); a system that rejects that swap chain (Windows 7) presents without it; Vulkan: mailbox as before, else FIFO latest ready when the driver has it. OpenGL follows the VSync option (no guarantee against tearing without vsync): with vsync off, a composited window (Windows 8 or later) shows the newest frame without tearing. Chosen when the renderer starts: a change of the setting during a session takes effect at the next renderer start | `config:rend.ThreadedRendering` with vsync | `gdxsv_tests/perf/battle.py` cells `30_170_tr*_vs1`, `*_dx_fs`, `*_gl`, `*_gl_fs`, `*_vk`, `slow_*_dx`, `slow_*_gl` at 60 Hz (press to display, frame periods; manual). Not checked: macOS, Linux | none |
| Threaded rendering only: `rend.ThreadedRendering` is overridden on after each settings load (`gdxsv_emu_settings_loaded`), so the config file keeps its value for an older version and `--config config:rend.ThreadedRendering=no` has no effect; its checkboxes and the Recommended Settings no longer change it. Full Framebuffer Emulation and Copy Rendered Textures to VRAM are hidden (off with threaded rendering). The gdxsv code for threaded rendering off (early present, UI-thread limiter) is gone; upstream's stays | always | C4: off in emu.cfg and on the command line, `ThreadedRendering` reads on and `RenderToTextureBuffer` off, VSync off on the same command line reads off, emu.cfg keeps `no` (builds before this fail it); `run_suite.py` A1, B2 | none |
| OpenGL and DirectX 11: frames duplicated on a monitor over 60 Hz only when asked to; otherwise vsync swaps every `gameSwapInterval` vblanks (1 for the game) | setting `config:rend.DupeFrames` | no test: needs a display above 60 Hz | none |
| DirectX 11: at most 1 frame queued for display (default changed from the driver's) | setting `gdxsv:MaxFrameLatency` (0: driver default) | no test | none |
| Opposite directions cancel (SOCD) for the game | always for the game | J3: SDL virtual gamepad, d-pad bound to the left stick | – |
| gdxsv defaults on a new config file | first start | C1: the defaults hold after the game start | – |
| Settings from an older release's `emu.cfg` (gdxsv-1.9.6 layout) | always | C2: flycast and gdxsv values hold after the game start | – |
| Gdx UDP Port out of 1-65535 replaced by a random port | game start, settings screen | C3: 70000 and -5 from `emu.cfg` | – |

## Releases and tools

| Feature | How to turn on | Test | Control |
|---|---|---|---|
| Update check against the releases of `inada-s/flycast`, a popup offers the new version | on start (`gdxsv:UpdateCheckUrl`) | U1: a newer release on a local release API is offered with its download | – |
| Release build from a `gdxsv-*` tag | push a tag | the workflow itself | none |
| CI build (macOS, Linux, Windows MinGW) | every push and PR | the workflow itself | none |
| Headless mode (no window, no graphics API) | `gdxsv:headless=yes` | E1; most rollback cases run headless | – |
| Boot probe for patch checks | `gdxsv:headless_loadstate=yes`, `gdxsv:headless_probe_frames=N` | every headless case runs on it; E1: exits rc 0 after N vblanks | – |
| Per-frame state hash log | `gdxsv:hashlog=yes` | Q1: a local rollback match logs every frame per peer, and each rollback's frames again (read by `gdxsv_tests/lib/hashlog.py`, rollback suite) | – |
| `run.py` (several instances, replay, spectate, rollback test) and the `RunGame` workflow | `python run.py <function>` | none | none |
| Fixed host address of guest RAM on Windows (lower 28 bits equal the guest address) | always | Y1: guest code read from outside at host 0x21c010000 | Y1c |
