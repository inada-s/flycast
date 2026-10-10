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
| `ReplayApiUrl` | lobby API | where replays are listed and fetched |
| `LiveApiUrl` | lobby API | where live battles are listed (`/status`) |
| `LiveBufferFrames` | 30 | spectating: frames kept behind the live edge (10 to 3600) |
| `MultiPovVolume` | 50 | four-screen replay: volume of each instance, 0 to 100 |
| `SpectateSyncGroup` | empty | instances with the same name play the same frame (four-screen replay sets it) |
| `SyncMaxWaitMs` | 2 | longest such a sync stalls a frame, 0 to 200 ms |
| `SyncStartTime` | 0 | Unix time to wait for before loading the start state |
| `rbk_test` | empty | local rollback test match, `<peer>/<peers>` (`tools/rbk_test`) |
| `rand_input` | 0 | seed of random input in `rbk_test` |
| `hashlog` | no | per-frame GGPO state hash log, `data/hashlog.txt` |
| `headless` | no | no window, no graphics API |
| `headless_width`, `headless_height` | 640, 480 | headless display size |
| `headless_loadstate` | no | headless: start from the lobby save state |
| `headless_probe_frames` | 0 | headless: exit after N vblanks |
| `borderless` | no | borderless window |

## Environment variables

Tests only; they do nothing unless set. Effects are in
`tools/rbk_test/README.md`.

| Variable | Used by |
|---|---|
| `GGPO_NETWORK_DELAY`, `GGPO_TEST_LOG` | any GGPO session |
| `GGPO_DELAY` | GGPO input delay 0 to 64, over the setting |
| `VITAL`, `MAXREBATTLE`, `RAND_MASK`, `RBK_SAVE_REPLAY`, `TEST_PORT_BASE`, `TEST_GGPO_DELAY`, `TEST_FAKE_TIMESYNC` | `rbk_test` match |
| `TEST_RELAY`, `TEST_RELAY_SERVER`, `TEST_RELAY_TOKEN`, `TEST_RELAY_IPV6` | `rbk_test` relays |
| `TEST_SPECTATOR_LBS`, `TEST_SPECTATOR_UPLINK`, `TEST_UPLINK_ROUND_DELAY` | `rbk_test` live uplink |
| `GDXSV_SPECTATOR_TEST_REPLAY` | unit test `GdxsvSpectator` |

## Build options (CMake)

| Option | Default | Effect |
|---|---|---|
| `GDXSV_FP_COMPAT` | ON | SH4 `fmac` as `fmul` + `fadd` on every CPU and backend, so replays and netplay agree across hosts |
| `ENABLE_SLEEP_BENCHMARK` | OFF | log the precision of the frame limiter's sleep |
| `MSVC_BREAKPAD` | OFF | Windows MSVC: build the breakpad client |
