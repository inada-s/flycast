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
| `UseTexturePack` | no | download and use the texture pack |
| `TexturePackChannel` | empty | texture pack channel to download |
| `WidescreenHudLayout` | 1 | HUD in widescreen: 0 stock 4:3, 1 up to 16:9, 2 viewport edges |
| `LocalPort` | 0 | UDP port for battles; 0 picks a random one |
| `MinDelay` | 2 | lowest input delay of a rollback battle and of a takeover, in frames |
| `SaveReplay` | yes | save every battle as a replay |
| `UploadReplay` | yes | upload saved replays |
| `ReplayPath` | empty | replay folder; empty is `data/replays` |
| `SkipRenderingHack` | yes | replay seek runs only the game logic of skipped frames |
| `SlowIdleLoopHack` | yes | dynarec hack for the game's idle loop |
| `MaxFrameLatency` | 1 | DirectX 11: frames DXGI may queue for display (1 to 16); 0 keeps the driver default; read when the renderer starts |
| `RepeatPresent` | yes | DirectX 11 window, vsync on: present the last frame again at every vsync while waiting for the next one |
| `ReplayHideName` | no | replay: generic player names |
| `ReplayShowAllyHP` | yes | replay: the total HP field shows the ally's HP |
| `ReplayKeyDisplay` | yes | replay: show the inputs |
| `ReplaySkipMsSelection` | yes | replay: fast-forward the mobile suit selection |
| `ProjectileView` | no | debug overlay of projectiles (Disc 2); not on the settings page |
| `LiveAutoNext` | no | spectating: move on to the next live battle |

## Settings in other sections

| Key | Default | Effect |
|---|---|---|
| `config:aica.LimitFPS` | yes | audio sync (a constant in upstream) |
| `config:rend.FixedFrequency` | 0 | frame limiter: 0 off, 1 by cable and broadcast, 2 59.94 Hz, 3 60 Hz, 4 50 Hz, 5 30 Hz |
| `input:UseXInput` | yes | Windows: use XInput devices |
| `input:UseDirectInput` | yes | Windows: use DirectInput devices |
| `input:JoystickPolling` | no | poll the joystick from the emulation thread |

## Command-line keys, section `gdxsv`

| Key | Default | Effect |
|---|---|---|
| `replay` | empty | play this replay file on start |
| `spectate` | empty | watch this live battle code on start |
| `ReplayPOV` | 1 | point of view of `replay` / `spectate`, player 1 to 4 |
| `ReplayFourScreen` | no | `replay` as a four-screen replay (four processes) |
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
| `GDXSV_PERF_TRACE=<csv path>` | any run: event trace for the perf check (`gdxsv_perf_trace.h`), off when unset |
| `GDXSV_PRESS_SWEEP=<player>,<start ms>,<hold ms>,<gap min ms>,<gap max ms>` | `rbk_test` match with `GDXSV_TEST_JOYSTICK`: random-time dpad presses on that player |
| `TEST_RELAY`, `TEST_RELAY_SERVER`, `TEST_RELAY_TOKEN`, `TEST_RELAY_IPV6` | `rbk_test` relays |
| `TEST_SPECTATOR_LBS`, `TEST_SPECTATOR_UPLINK`, `TEST_UPLINK_ROUND_DELAY` | `rbk_test` live uplink |
| `GDXSV_SPECTATOR_TEST_REPLAY` | unit test `GdxsvSpectator` |

## Build options (CMake)

| Option | Default | Effect |
|---|---|---|
| `GDXSV_FP_COMPAT` | ON | SH4 `fmac` as `fmul` + `fadd` on every CPU and backend, so replays and netplay agree across hosts |
| `ENABLE_SLEEP_BENCHMARK` | OFF | log the precision of the frame limiter's sleep |
| `MSVC_BREAKPAD` | OFF | Windows MSVC: build the breakpad client |
