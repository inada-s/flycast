# gdxsv options

Every setting, command-line key and environment variable this fork adds.
Upstream options it changes are in [upstream.md](upstream.md).

- Settings live in `emu.cfg`, in the section named by the table heading or
  the key (`section:key`).
- Any of them can be set for one run with `--config <section>:<key>=<value>`
  (not saved). The keys of "Command-line keys" are only read that way or from
  `emu.cfg`; they have no settings page.
- A new `emu.cfg` starts from the gdxsv defaults in
  `gdxsv_apply_base_settings` (`core/gdxsv/gdxsv_gui_settings.cpp`): audio
  sync on, `FixedFrequency` 59.94 Hz, Sega controller + VMU, OpenGL (DirectX
  11 on Windows), 720p, no frame skip, UPnP on.

## Settings, section `gdxsv`

| Key | Default | Effect |
|---|---|---|
| `server` | `zdxsv.net` | lobby server host |
| `loginkey` | empty | lobby account key; generated when empty |
| `language` | -1 | translation patch: 0 Japanese, 1 Cantonese, 2 English, 3 off |
| `TextureResolution2D` | -1 | texture pack 2D layer (menus, briefing, HUD, translated images): 1 `<channel>-2d-1x.json` (only the language images, at the original size), 2 `<channel>-2d-2x.json`, 4 `<channel>-2d-4x.json`; -1 takes 2 from a released build's `UseTexturePack=yes`, else 1 |
| `TextureResolution3D` | -1 | texture pack 3D layer (mobile suits, ships, stages): 1 off (no download), 2 `<channel>-3d-2x.json`, 4 `<channel>-3d-4x.json`; -1 takes 2 from a released build's `UseTexturePack=yes`, else 1 |
| `FontRaster` | no | DC1/DC2: draw the game font glyphs from the embedded GDXGothic Medium instead of the 24 px bitmaps: text with the game's edge, the in-battle pilot names smaller with a see-through dark outline, and the battle-message sheet (the INFORMATION panel's messages) at the names' size; wins over the embedded translation textures and the texture pack, a `data/textures` folder wins over it; Use High-Resolution Font in the gdxsv settings; takes effect at the next game start |
| `TexturePackChannel` | empty | texture pack channel to download |
| `WidescreenHudLayout` | 1 | HUD in widescreen: 0 stock 4:3, 1 up to 16:9, 2 viewport edges |
| `LocalPort` | 0 | UDP port for battles; 0 picks a random one |
| `MinDelay` | 2 | lowest input delay of a rollback battle and of a takeover, in frames |
| `RollbackBudget` | 1.0 | frames of the one-way latency a rollback battle leaves to rollback rather than to input delay (0 to 1.5); not on the settings page: each battle of 1800 frames or more moves it halfway to the depth of rollback this PC fits in a frame, minus 1 |
| `SaveReplay` | yes | save every battle as a replay |
| `UploadReplay` | yes | upload saved replays |
| `ReplayPath` | empty | replay folder; empty is `data/replays` |
| `SkipRenderingHack` | yes | replay seek runs only the game logic of skipped frames |
| `SlowIdleLoopHack` | yes | dynarec hack for the game's idle loop |
| `MaxFrameLatency` | 1 | DirectX 11: frames DXGI may queue for display (1 to 16); 0 keeps the driver default; read when the renderer starts |
| `ReplayHideName` | no | replay: generic player names |
| `ReplayShowAllyHP` | yes | replay: the total HP field shows the ally's HP |
| `ReplayKeyDisplay` | yes | replay: show the inputs |
| `ReplaySkipMsSelection` | yes | replay: fast-forward the mobile suit selection |
| `ReplayGodsEyeClosest` | 150 | replay god's eye view: the closest the camera comes to the players, 150 to 1500 world units |
| `ReplayGodsEyeAngle` | 45 | replay god's eye view: degrees it leans back from straight down, 0 to 70 |
| `ReplayGodsEyeNameplates` | yes | replay god's eye view: a nameplate over each MS (force logo, pilot name, HP gauge) |
| `ReplayGodsEyeBoost` | yes | replay god's eye view: a boost gauge under each nameplate while its MS is off the ground |
| `ReplayGodsEyeTeamColors` | yes | replay god's eye view: nameplate lines and HP gauges in the team colors; off: white lines and the game's gauge colors |
| `ReplayGodsEyeMsImage` | no | replay god's eye view: the MS's image left of each nameplate |
| `ReplayGodsEyeFade` | yes | replay god's eye view: a nameplate fades out while it overlaps another nameplate or MS |
| `ReplayGodsEyeTrail` | yes | replay god's eye view: a trail behind each MS in its team color |
| `ReplayGodsEyeTrailSeconds` | 6 | replay god's eye view: how long a trail lasts, 1 to 20 seconds |
| `ReplayGodsEyeDpad` | yes | replay god's eye view: camera mode, where the d-pad turns and zooms the view instead of controlling the replay, X and Y tilt it, B steps whom it follows, and the 1 to 5 keys (main row or numpad) pick the players it follows, 5 everyone; off, timeline mode. The CAMERA panel's gamepad toggle, or START |
| `LiveAutoNext` | no | spectating: move on to the next live battle |

## Settings in other sections

| Key | Default | Effect |
|---|---|---|
| `config:aica.LimitFPS` | yes | audio sync (a constant in upstream) |
| `config:rend.FixedFrequency` | 0 | frame limiter: 0 off, 1 by cable and broadcast, 2 59.94 Hz, 3 60 Hz, 4 50 Hz, 5 30 Hz |
| `input:UseXInput` | yes | Windows: use XInput devices |
| `input:UseDirectInput` | yes | Windows: use DirectInput devices |

## Command-line keys, section `gdxsv`

| Key | Default | Effect |
|---|---|---|
| `replay` | empty | play this replay file on start |
| `spectate` | empty | watch this live battle code on start |
| `ReplayPOV` | 1 | point of view of `replay` / `spectate`, player 1 to 4 |
| `ReplayFourScreen` | no | `replay` as a four-screen replay (four processes) |
| `ReplayPOVMode` | empty | single-screen `replay` / `spectate` on Disc 2: `godseye` opens the god's eye view, `player` (or empty) the player's camera; one shot |
| `replay_target_round` | 0 | start `replay` at this round |
| `replay_target_frame` | 0 | start `replay` at this input index |
| `replay_takeover_frame` | 0 | take over `replay` (pause menu's Take Over) once it plays this input index |
| `TexturePackUrl` | gdxsv storage | base URL of the texture pack channel JSON and its chunks |
| `ReplayApiUrl` | lobby API | where replays are listed and fetched |
| `LiveApiUrl` | lobby API | where live battles are listed (`/status`) |
| `ConnectionMedium` | empty (detected) | `Wired`, `Wireless`, `VPN: <name>`: used instead of the detected adapter (warnings, lobby report) |
| `UdpTestUrl` | gdxsv cloud function | P2P test: asked to send a UDP packet to this client's public address |
| `PublicIpUrl` | empty (ipify, seeip) | returns this client's public IP as text; replaces the IPv4 and IPv6 lists |
| `LatencyHost` | empty (Cloud Run regions) | `host[:port]` every region's HTTPS latency test uses instead |
| `UpdateCheckUrl` | GitHub latest release | release JSON the startup update check reads |
| `LiveBufferFrames` | 30 | spectating: frames kept behind the live edge (10 to 3600) |
| `AudioRateControl` | yes | replay and spectating: audio rate control, including the combined four-screen output (`no`: the queue can overflow or run dry) |
| `ReplayFixedFrequency` | 2 | windowed replay and spectating: `rend.FixedFrequency` mode (3: 60 Hz, for tests) |
| `VideoRoutingName` | empty | override the full Syphon/Spout sender name (up to 255 UTF-8 bytes); empty preserves the ordinary sender name; unified replay assigns its guests unique names using the session and player |
| `MultiPovAudioSync` | yes | four-screen replay: combine matching audio positions through the host's output device; instances unable to join use separate outputs |
| `MultiPovMixVolume` | 100 | synchronized four-screen mix level, 0 to 100 percent; Flycast's master volume still applies |
| `MultiPovVolume` | 50 | volume of each instance's separate output when audio routing is off or unavailable, 0 to 100 |
| `SpectateSyncGroup` | empty | instances with the same name play the same frame (four-screen replay sets it) |
| `SyncMaxWaitMs` | 2 | longest such a sync stalls a frame, 0 to 200 ms |
| `SyncStartTime` | 0 | Unix time to wait for before loading the start state |
| `rbk_test` | empty | local rollback test match, `<peer>/<peers>` (`gdxsv_tests/rollback`) |
| `rand_input` | 0 | seed of random input in `rbk_test` |
| `hashlog` | no | per-frame GGPO state hash log, `data/hashlog.txt` |
| `headless` | no | no window, no graphics API |
| `headless_width`, `headless_height` | 640, 480 | headless display size |
| `headless_loadstate` | no | headless: start from the lobby save state |
| `headless_probe_frames` | 0 | headless: exit after N vblanks |
| `borderless` | no | borderless window |

## Environment variables

Tests only; they do nothing unless set. Effects are in
`gdxsv_tests/rollback/README.md`.

| Variable | Used by |
|---|---|
| `GGPO_NETWORK_DELAY`, `GGPO_TEST_LOG`, `GGPO_TEST_NO_SKIP_REPLAY` | any GGPO session |
| `GGPO_DELAY` | GGPO input delay 0 to 64, over the setting |
| `GGPO_TEST_NO_REBATTLE_CANCEL` | `rbk_test` match (control of rollback case C3) |
| `VITAL`, `MAXREBATTLE`, `RAND_MASK`, `RBK_SAVE_REPLAY`, `TEST_PORT_BASE`, `TEST_GGPO_DELAY`, `TEST_FAKE_TIMESYNC` | `rbk_test` match |
| `TEST_PING_MS` | `rbk_test` match: P2P ping test length, ms (default 7500) |
| `GDXSV_VRAM_TRACK_CHECK` | any game: checks the render snapshot copy against VRAM at each capture, logs `RENDSNAP` |
| `GDXSV_PERF_TRACE=<csv path>` | any run: event trace for the perf check (`gdxsv_perf_trace.h`), off when unset |
| `GDXSV_TEST_PRESENT_HZ=<hz>` | any game: each Present returns at the next vblank of a simulated display at that rate, as a Present that waits for vsync (perf cells `slow_*_60hz`, `slow_*_5994hz`) |
| `GDXSV_TEST_FRAME_COST_US=<us>` | any game: each emulated frame, rollback re-runs included, that much slower, as on a slower PC (perf cells `slow_*`) |
| `GDXSV_PRESS_SWEEP=<player>,<start ms>,<hold ms>,<gap min ms>,<gap max ms>` | `rbk_test` match with `GDXSV_TEST_JOYSTICK`: random-time dpad presses on that player |
| `TEST_RELAY`, `TEST_RELAY_SERVER`, `TEST_RELAY_TOKEN`, `TEST_RELAY_IPV6` | `rbk_test` relays |
| `TEST_SPECTATOR_LBS`, `TEST_SPECTATOR_UPLINK`, `TEST_UPLINK_ROUND_DELAY` | `rbk_test` live uplink |
| `GDXSV_SPECTATOR_TEST_REPLAY` | unit test `GdxsvSpectator` |
| `GDXSV_TEST_START_REPLAY` | online: a file name; once the file appears, the replay whose path it holds (`four_screen <path>`: on four screens) starts as from the menu and the replay browser, or is refused (logged) where the menu would not open (`gdxsv_tests` O7, O8) |

## Build options (CMake)

| Option | Default | Effect |
|---|---|---|
| `GDXSV_FP_COMPAT` | ON | SH4 `fmac` as `fmul` + `fadd` on every CPU and backend, so replays and netplay agree across hosts |
| `ENABLE_SLEEP_BENCHMARK` | OFF | log the precision of the frame limiter's sleep |
| `MSVC_BREAKPAD` | OFF | Windows MSVC: build the breakpad client |
