# Changes from upstream Flycast

What this fork changes in Flycast's own code, against the last merge of
upstream ([flyinghead/flycast](https://github.com/flyinghead/flycast)
`master`). The features are in [features.md](features.md), their options in
[options.md](options.md).

## Fork point

- The fork point is the merge base with upstream `master`:

  ```
  git fetch https://github.com/flyinghead/flycast.git master
  git merge-base FETCH_HEAD gdxsv-master
  git diff --stat $(git merge-base FETCH_HEAD gdxsv-master) gdxsv-master -- . ':!core/deps'
  ```

- An upstream merge moves it; this page names files and functions, not
  lines.

## Added, not in upstream

| Path | What |
|---|---|
| `core/gdxsv/` | all gdxsv features: lobby, rollback, replay, spectating, patches, UI; textures of the translation |
| `gdxsv_patch/` | SH4 network payload of the game (`build.sh`, `abi.py` -> `gdxsv_patch_abi.h`, `gdxsv_patch.inc`) |
| `gdxsv_langmod/` | translation sources (`translation.csv`, PSD textures) and their generators |
| `core/deps/protobuf-3.13.0/` | protobuf runtime (`gdxsv.pb.cc`) |
| `resources/i18n-gdxsv/` | UI translations of the gdxsv pages |
| `tests/src/Gdxsv*.cpp`, `tests/src/test_stubs.cpp` | unit tests |
| `gdxsv_tests/` | e2e tests: `lib/` (launching instances, hash logs), `rollback/` (rollback and live spectating rigs) |
| `run.py`, `.github/workflows/run-game.yml` | several local instances; the same on a self-hosted runner |
| `gdxsv-format.sh`, `.github/pull.yml` | clang-format of gdxsv files; upstream sync bot config |

## Changed third-party code

| Path | Change |
|---|---|
| `core/deps/ggpo/` | relays (peers and relay servers), dual-stack (IPv4 and IPv6) sockets, timesync skips replayed in rollbacks, size checks on received packets |
| `core/deps/implot/` | ImPlot 0.18 WIP (upstream: 0.17) |
| submodules `SDL`, `asio`, `luabridge`, `rcheevos`, `oboe`, `libadrenotools`, `Spout`, `Syphon` | pinned to older commits than upstream's |

## Changed upstream files

Hooks into `core/gdxsv` (`gdxsv_emu_*` in `gdxsv_emu_hooks.h`) unless stated.

| Area | Files | Why |
|---|---|---|
| Start, reset, save states | `core/nullDC.cpp`, `core/emulator.cpp/.h`, `core/serialize.cpp` | gdxsv init, start and reset hooks; restore hook after a state load; find content in the working directory; a load-state hotkey without threaded rendering loads after the frame (`core/ui/gui.cpp/.h`, `core/ui/mainui.cpp`) |
| Game RPC | `core/hw/holly/sb_mem.cpp` | the payload's `gdx_rpc` call reaches the host through area 0 |
| Frame hooks | `core/hw/pvr/Renderer_if.cpp/.h`, `core/hw/pvr/ta_ctx.cpp`, `core/rend/norend/norend.cpp` | vblank and end-of-frame hooks; headless skips rendering and presents once per frame; fast-forward counts in frame skip |
| Rollback | `core/network/ggpo.cpp/.h` (session start and stats in `core/gdxsv/gdxsv_ggpo.cpp`, hash log in `core/gdxsv/gdxsv_ggpo_hooks.cpp`), `core/hw/pvr/elan.cpp`, `core/hw/mem/mem_watch.h`, `core/hw/maple/maple_cfg.h` | gdxsv rollback session on top of upstream GGPO; flag bits sent beside the pad input; state hash log; test logs; memory watch shared with gdxsv; a fault on an already-saved page no longer loops |
| Determinism | `core/hw/sh4/interpr/sh4_fpu.cpp`, `core/rec-ARM/rec_arm.cpp`, `core/rec-ARM64/rec_arm64.cpp`, `core/rec-x64/rec_x64.cpp`, `core/hw/sh4/dyna/shil_canonical.h` | `fmac` without fused multiply-add (`GDXSV_FP_COMPAT`) |
| Dynarec | `core/rec-x64/rec_x64.cpp`, `core/hw/sh4/dyna/blockmanager.cpp`, `core/hw/sh4/dyna/decoder.cpp`, `core/hw/arm7/arm7_rec_x64.cpp` | x64 block linking (all games); output-only game functions skipped on seek frames (hook call); slow idle loop hack; ARM7 `cpuid` probed once |
| Memory | `core/windows/win_vmem.cpp`, `core/linux/posix_vmem.cpp` | Windows: fixed RAM base (host address low 28 bits = guest address); Linux: one shared-memory name per process (four-screen replay) |
| Frame pacing | `core/ui/mainui.cpp` (hooks into `core/gdxsv/gdxsv_frame_limiter.cpp`), `core/cfg/option.cpp/.h` | `FixedFrequency` limiter with precise sleep and a replay trim; context recreated when `DupeFrames` changes; `LimitFPS` an option, not a constant |
| Audio | `core/audio/audiostream.cpp/.h` (hooks into `core/gdxsv/gdxsv_audio.cpp`), `audiobackend_coreaudio.cpp`, `audiobackend_directsound.cpp`, `audiobackend_sdl2.cpp`, `core/hw/aica/sgc_if.cpp` | rate control for replay and spectating (backends report their queue level); per-instance volume (four-screen replay) and fade; a muted replay seek or rollback rerun steps the channels without mixing |
| Input | `core/input/gamepad_device.cpp/.h`, `core/sdl/sdl.cpp/.h` | joystick polling from the emulation thread; XInput / DirectInput switches; opposite directions cancel for the game |
| Crash reports | `core/oslib/oslib.cpp/.h` | own crash list file; gdxsv log, tags and `emu.cfg` attached to a report; four-screen replay guests do not upload |
| Window | `core/sdl/sdl.cpp`, `core/wsi/sdl.cpp`, `core/rend/dx11/dx11context.cpp`, `core/wsi/switcher.cpp`, `core/linux-dist/main.cpp` | headless (no graphics API), borderless, hidden rendering guests, window position for several instances, four-screen grid; frames duplicated on a monitor over 60 Hz only with `DupeFrames` (OpenGL, DirectX 11) |
| Video routing | `core/wsi/switcher.cpp`, `core/rend/vulkan/vulkan_context.cpp`, `core/rend/gles/gldraw.cpp`, `core/rend/dx11/dx11_renderer.cpp`, `core/windows/winmain.cpp`, `shell/apple/emulator-osx/emulator-osx/osx-main.mm` | Spout / Syphon sender names; the unified replay scales the final host image before UI drawing, receives guest video in gdxsv, and releases receivers before graphics shutdown |
| Config | `core/cfg/cfg.cpp` | a new `emu.cfg` gets the gdxsv defaults |
| UI | `core/ui/gui.cpp/.h`, `core/ui/settings*.cpp/.h`, `core/ui/gui_font.cpp`, `core/oslib/i18n.cpp` | gdxsv settings page, Live & Replays menu, update popup, gdxsv translations, Japanese fonts always loaded |
| Process exit | `core/windows/winmain.cpp`, `core/linux-dist/main.cpp` | exit code set by gdxsv (`gdxsv_exit_code`); Windows: breakpad include path under MSVC |
| macOS application launch | `shell/apple/emulator-osx/emulator-osx/SDLApplicationDelegate.mm` | `CustomApplicationMain` calls `gdxsv_emu_app_init` before AppKit initialization and skips foreground activation so hidden guests do not take keyboard focus |
| Network | `core/network/miniupnp.cpp/.h` | UPnP mapping details |
| HTTP | `core/oslib/http_client.cpp/.h`, `shell/apple/common/http_client.mm` | thread-local URL buffers on Windows (concurrent requests mixed hosts) |
| Textures | `core/rend/CustomTexture.cpp` | embedded and downloaded texture packs for the game (`T13306M`); its glyph-cell textures built from the glyph replacements |
| Logs | `core/log/LogManager.cpp`, `core/log/InMemoryListener.h`, `core/profiler/fc_profiler.h` | one log file per instance; 100 lines kept in memory (was 20); a missing include |
| Settings struct | `core/types.h` | `settings.gdxsv` runtime state |
| Build | `CMakeLists.txt`, `tests/CMakeLists.txt`, `resources/resources.cmake`, `.gitignore` | `include(core/gdxsv/gdxsv.cmake)` (gdxsv sources, protobuf), build options, gdxsv tests and resources |
| CI | `.github/workflows/c-cpp.yml`, `crowdin_translate.yml` | gdxsv builds and releases from `gdxsv-*` tags |

## Changed spots

Each change in an upstream file is marked where it is: a line that names
gdxsv (`gdxsv_` call or include), else a `// gdxsv: <why>` comment. The list
below is generated from those markers; `tools/upstream_spots.py` checks that
every spot is marked and the list is current (`--write` updates it).

<!-- upstream_spots begin: generated by tools/upstream_spots.py --write -->
| File | Where | What |
|---|---|---|
| `.gitignore` |  | Visual Studio resource editor files |
| `CMakeLists.txt` |  | x86_64 only |
| `CMakeLists.txt` |  | `gdxsv_tests` |
| `CMakeLists.txt` |  | version from the latest gdxsv-* tag, in semver form |
| `CMakeLists.txt` |  | `# Transform git describe to semver format, e.g. gdxsv-1.2.4-3-g89b14873e -> gdxsv-1.2.4-dev.3+89b148` |
| `CMakeLists.txt` |  | version and hash in the configure log |
| `CMakeLists.txt` |  | x86_64 only, no arm64 symbols. |
| `CMakeLists.txt` |  | x86_64 only, no arm64 symbols. |
| `CMakeLists.txt` |  | SDL headers for the MSVC (clang-cl) build |
| `CMakeLists.txt` |  | `if(LUA_FOUND AND (NOT APPLE OR GDXSV_APPLE_LUA))` |
| `CMakeLists.txt` |  | bundle version from the gdxsv-* tag |
| `CMakeLists.txt` | `include` | `include(core/gdxsv/gdxsv.cmake)` |
| `core/audio/audiobackend_coreaudio.cpp` |  | queue level for the replay and spectating audio rate control |
| `core/audio/audiobackend_directsound.cpp` |  | queue level for the replay and spectating audio rate control |
| `core/audio/audiobackend_sdl2.cpp` |  | queue level for the replay and spectating audio rate control |
| `core/audio/audiostream.cpp` |  | include `gdxsv/gdxsv_audio_hooks.h` |
| `core/audio/audiostream.cpp` | `pushFrame` | the gain moved to WriteSample, which may resample before the buffer |
| `core/audio/audiostream.cpp` | `WriteSample` | `gdxsv_audio_pushed` |
| `core/audio/audiostream.cpp` | `WriteSample` | `gdxsv_audio_gain`, `gdxsv_audio_write` |
| `core/audio/audiostream.cpp` | `FlushAudio` | drops queued samples on a replay seek |
| `core/audio/audiostream.h` |  | frames queued for output and the most the queue holds, for audio rate control. |
| `core/audio/audiostream.h` |  | drops queued samples on a replay seek |
| `core/audio/audiostream.h` |  | queue level for the backends' getQueueLevel |
| `core/cfg/cfg.cpp` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/cfg/cfg.cpp` | `open` | `gdxsv_emu_apply_base_settings` |
| `core/cfg/option.cpp` |  | an option, not a constant: replay and rollback turn it off |
| `core/cfg/option.cpp` |  | fixed frame rate limiter (core/gdxsv/gdxsv_frame_limiter.cpp) |
| `core/cfg/option.cpp` |  | XInput and DirectInput switches; joystick polling from the emulation thread |
| `core/cfg/option.h` |  | an option, not a constant |
| `core/cfg/option.h` |  | fixed frame rate limiter |
| `core/cfg/option.h` |  | XInput and DirectInput switches; joystick polling from the emulation thread |
| `core/emulator.cpp` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/emulator.cpp` | `Emulator::dc_reset` | `gdxsv_emu_reset` |
| `core/emulator.cpp` | `Emulator::init` | `gdxsv_emu_reset` |
| `core/emulator.cpp` | `Emulator::loadGame` | `gdxsv_emu_start` |
| `core/emulator.cpp` | `Emulator::unloadGame` | `gdxsv_emu_reset` |
| `core/emulator.cpp` | `Emulator::loadstate` | `gdxsv_emu_reset` |
| `core/emulator.cpp` | `Emulator::run` | `gdxsv_emu_next_frame` |
| `core/emulator.cpp` | `Emulator::start` | `gdxsv_emu_frame_end` after `ggpo::nextFrame` (the frame limiter of a rollback battle with threaded rendering) |
| `core/emulator.cpp` | `Emulator::start` | fix blackout on threaded rendering |
| `core/emulator.cpp` | `Emulator::stop` | stop the GGPO session the rollback backend started, so the emulator thread runs no rollback frame and restarts no CPU after the stop |
| `core/emulator.cpp` | `Emulator::vblank` | `gdxsv_emu_end_frame` |
| `core/emulator.h` |  | a rollback or replay session ended |
| `core/hw/aica/sgc_if.cpp` | `ChannelEx::Step` | muted replay seek or rollback rerun (`ggpo::rollbacking()`, include `network/ggpo.h`) without DSP: step envelopes, stream and LFO, skip the mix nobody hears |
| `core/hw/arm7/arm7_rec_x64.cpp` |  | cpuid is slow: probe the host once, not for every compiled block |
| `core/hw/holly/sb_mem.cpp` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/hw/holly/sb_mem.cpp` | `ReadMem_area0` | `gdxsv_emu_rpc` |
| `core/hw/maple/maple_cfg.h` |  | flag bits sent beside the pad input in rollback |
| `core/hw/mem/mem_watch.h` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/hw/mem/mem_watch.h` |  | already saved, yet it faulted: something protected the page again (gdxsv). |
| `core/hw/mem/mem_watch.h` | `writeAccess` | `gdxsv_is_using_memwatch` |
| `core/hw/mem/mem_watch.h` | `protect` | `gdxsv_is_using_memwatch` |
| `core/hw/mem/mem_watch.h` | `Watcher::unprotectPage` | unprotect one page, for `load_game_state` |
| `core/hw/mem/mem_watch.h` | `Watcher::protectRestored` | protect the restored and the dirty pages after `load_game_state`, not the hot pages; take the hot pages' copies again; include `<unordered_set>` |
| `core/hw/mem/mem_watch.h` | `Watcher` (`hot`, `faultStreak`), `Watcher::save`, `Watcher::restoreHot`, `Watcher::hit`, `Watcher::reset` | hot pages for the rollback save: a page written in 2 consecutive saves stays unprotected and is compared with its copy at each save instead of faulting; protected again after 60 unchanged saves |
| `core/hw/pvr/Renderer_if.cpp` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/hw/pvr/Renderer_if.cpp` | `rend_create_renderer` | `gdxsv_headless` |
| `core/hw/pvr/Renderer_if.cpp` | `rend_start_render` | `gdxsv_emu_end_frame` |
| `core/hw/pvr/Renderer_if.cpp` | `rend_vblank` | `gdxsv_emu_vblank` |
| `core/hw/pvr/Renderer_if.h` |  | caches reloaded after a rollback or replay session |
| `core/hw/pvr/Renderer_if.h` |  | caches reloaded after a rollback or replay session |
| `core/hw/pvr/ta_ctx.cpp` |  | rollback time sync in QueueRender |
| `core/hw/pvr/ta_ctx.cpp` | `QueueRender` | fast-forward counts in frame skip; a rollback time sync skips one render every 30 frames |
| `core/hw/sh4/dyna/blockmanager.cpp` |  | include `gdxsv/gdxsv_sh4_hooks.h` |
| `core/hw/sh4/dyna/blockmanager.cpp` | `bm_GetCodeByVAddr` | `gdxsv_sh4_code_addr` |
| `core/hw/sh4/dyna/blockmanager.cpp` | `bm_AddBlock` | `gdxsv_sh4_block_compiled` when `GDXSV_SH4_BLOCK_MAP` is set (profiling) |
| `core/hw/sh4/dyna/decoder.cpp` |  | slow idle loop hack |
| `core/hw/sh4/dyna/decoder.cpp` |  | slow idle loop hack, the game's idle loop block costs more cycles |
| `core/hw/sh4/dyna/shil_canonical.h` |  | round the product before the add (no fused multiply-add) |
| `core/hw/sh4/interpr/sh4_fpu.cpp` | `sh4op` | round the product before the add (no fused multiply-add) |
| `core/hw/sh4/sh4_mmr.cpp` | `serialize` | doSqWrite is a host pointer: saved as null so the same emulated state saves the same bytes in every run |
| `core/hw/sh4/sh4_mmr.cpp` | `deserialize` | doSqWrite was saved as null |
| `core/input/gamepad_device.cpp` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/input/gamepad_device.cpp` | `GamepadDevice::handleButtonInput` | `gdxsv_is_ingame` |
| `core/input/gamepad_device.cpp` | `GamepadDevice::handleButtonInput` | `gdxsv_is_ingame` |
| `core/input/gamepad_device.cpp` | `GamepadDevice::gamepad_axis_input` | last raw value per axis, the start point of axis detection (triggers that rest away from 0) |
| `core/input/gamepad_device.cpp` | `GamepadDevice::detectInput` | detection starts from the axes' current values |
| `core/input/gamepad_device.cpp` | `GamepadDevice::RampAnalog` | `gdxsv_enabled` |
| `core/input/gamepad_device.h` |  | include `gdxsv/gdxsv_input_hooks.h` |
| `core/input/gamepad_device.h` |  | `gdxsv_input_digital_axis` |
| `core/input/gamepad_device.h` |  | start point of axis detection |
| `core/linux-dist/main.cpp` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/linux-dist/main.cpp` | `main` | without a display server SDL has nothing to open; fall back to its dummy driver so headless runs (gdxsv:headless=yes) get past initialization. |
| `core/linux-dist/main.cpp` | `main` | `gdxsv_exit_code` |
| `core/linux/posix_vmem.cpp` | `allocate_shared_filemem` | one name per process: instances sharing a data dir (4-player replay) could otherwise open the same file before either unlinks it. |
| `core/log/InMemoryListener.h` |  | upstream 20; the log goes into rollback reports |
| `core/log/LogManager.cpp` |  | include `gdxsv/gdxsv_multi_pov.h` |
| `core/log/LogManager.cpp` | `LogManager::UpdateConfig` | `gdxsv_multi_pov_log_file_name` |
| `core/network/ggpo.cpp` |  | include `gdxsv/gdxsv_sleep.h` |
| `core/network/ggpo.cpp` |  | skip frames, rollback seek and extra input of the gdxsv session |
| `core/network/ggpo.cpp` | `getLocalInput` | `gdxsv_enabled` |
| `core/network/ggpo.cpp` |  | 4-player battles |
| `core/network/ggpo.cpp` |  | microseconds for the fps shown in the gdxsv stats |
| `core/network/ggpo.cpp` |  | frames of the last rollback, for the skip frame replay and its test log |
| `core/network/ggpo.cpp` |  | extra input (game state flags) sent with the buttons |
| `core/network/ggpo.cpp` |  | exInput |
| `core/network/ggpo.cpp` | `on_event` | `gdxsv_ggpo_state` |
| `core/network/ggpo.cpp` | `advance_frame` | `settings.gdxsv.skipRenderingAddr`, `settings.gdxsv.skipRenderingBaseAddr` |
| `core/network/ggpo.cpp` | `advance_frame` | `gdxsv_ggpo_state`, `settings.gdxsv.skipRenderingAddr`, `gdxsv_emu_skip_rendering_addr` |
| `core/network/ggpo.cpp` | `advance_frame` | `settings.gdxsv.skipRenderingAddr` |
| `core/network/ggpo.cpp` | `advance_frame` | `gdxsv_ggpo_state` |
| `core/network/ggpo.cpp` | `load_game_state` | test log |
| `core/network/ggpo.cpp` | `load_game_state` | `gdxsv_ggpo_state` |
| `core/network/ggpo.cpp` | `load_game_state` | free_buffer keeps the skip counts of the frames after it |
| `core/network/ggpo.cpp` | `load_game_state` | unprotects each restored page once (`unprotectPage`) instead of the whole RAM, VRAM and AICA RAM, and protects only those and the dirty pages again (`protectRestored`); include `<unordered_set>`; hot pages back to the last save first (`restoreHot`) |
| `core/network/ggpo.cpp` | `save_game_state` | `gdxsv_ggpo_state_saved` |
| `core/network/ggpo.cpp` | `save_game_state`, `MemPages::save` | the delta is taken with hot pages (`Watcher::save`, which also protects) |
| `core/network/ggpo.cpp` | `free_buffer` | during a rollback the freed state is one of the frames about to be re-simulated (frame > lastSavedFrame), whose skip count is still needed by advance_frame. |
| `core/network/ggpo.cpp` | `save_game_state`, `free_buffer`, `stopSession` | freed save state buffers kept in `stateBufferPool` and reused (a fresh 10 MB block page-faults on its first writes each frame); freed at session stop |
| `core/network/ggpo.cpp` | `startSession` | `gdxsv_ggpo_session_start`, `gdxsv_ggpo_state` |
| `core/network/ggpo.cpp` | `startSession` | `gdxsv_ggpo_state` |
| `core/network/ggpo.cpp` | `startSession` | `gdxsv_ggpo_frame_delay`: GGPO frame delay +1 in a gdxsv rollback battle |
| `core/network/ggpo.cpp` | `stopSession` | `gdxsv_ggpo_session_stop`, `gdxsv_ggpo_state` |
| `core/network/ggpo.cpp` | `getInput` | `gdxsv_ggpo_state` |
| `core/network/ggpo.cpp` | `getInput` | `gdxsv_ggpo_state` |
| `core/network/ggpo.cpp` | `notifySkipInput` | skip frames |
| `core/network/ggpo.cpp` | `nextFrame` | `gdxsv_ggpo_state` |
| `core/network/ggpo.cpp` | `nextFrame` | `gdxsv_ggpo_state` |
| `core/network/ggpo.cpp` | `nextFrame` | `gdxsv_ggpo_state`, `gdxsv_input_update_joysticks` |
| `core/network/ggpo.cpp` | `nextFrame` | `gdxsv_ggpo_state`, `gdxsv_enabled` |
| `core/network/ggpo.cpp` | `nextFrame` | `gdxsv_sleep_us` |
| `core/network/ggpo.cpp` | `nextFrame` | `gdxsv_ggpo_state` |
| `core/network/ggpo.cpp` | `nextFrame`, `addLocalInput` | the local input part of `nextFrame` split out as `addLocalInput`, to run it before or after the advance |
| `core/network/ggpo.cpp` | `nextFrame`, `addLocalInput` | gdxsv rollback battle (`gdxsv_ggpo_state.inputBeforeAdvance`): the local input added before `ggpo_advance_frame` (none at frame 0: no input before the first input read), then `gdxsv_ggpo_wait_prediction_barrier`; `addLocalInput` no longer starts the executor (`nextFrame` does) |
| `core/network/ggpo.cpp` | `poll` | rollback check between frames, for the gdxsv session |
| `core/network/ggpo.cpp` | `startNetwork` | `gdxsv_sleep_us` |
| `core/network/ggpo.cpp` | `displayStats` | per-peer stats of the 4-player session |
| `core/network/ggpo.cpp` | `getCurrentFrame` | the gdxsv session works by frame number |
| `core/network/ggpo.cpp` | `gdxsv_ggpo_upstream` | `GdxsvGgpoUpstream`, `gdxsv_ggpo_upstream` |
| `core/network/ggpo.h` |  | timeSyncFrames |
| `core/network/ggpo.h` |  | `bool getCurrentFrame(int* frame); // gdxsv` |
| `core/network/ggpo.h` | `rollbacking` | skip frames and extra input of the gdxsv session |
| `core/network/miniupnp.cpp` | `MiniUPnP::Init` | `snprintf(lastError, sizeof(lastError), "UPnP discover failed: error %d", error); // gdxsv` |
| `core/network/miniupnp.cpp` | `MiniUPnP::Init` | `snprintf(lastError, sizeof(lastError), "Internet Gateway not found: error %d", error); // gdxsv` |
| `core/network/miniupnp.cpp` | `MiniUPnP::AddPortMapping` | `snprintf(lastError, sizeof(lastError), "Port %d redirection failed: error %d", port, error); // gdxs` |
| `core/network/miniupnp.cpp` | `MiniUPnP::CheckPortMapping` | P2P status, true if the router maps the port to this host |
| `core/network/miniupnp.h` |  | error text for the P2P status |
| `core/network/miniupnp.h` |  | port-mapping check, addresses and last error for the P2P status |
| `core/network/miniupnp.h` |  | `char lastError[256]; // gdxsv` |
| `core/network/miniupnp.h` |  | P2P status |
| `core/nullDC.cpp` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/nullDC.cpp` | `flycast_init` | find content in the working directory |
| `core/nullDC.cpp` | `flycast_init` | `gdxsv_emu_flycast_init` |
| `core/nullDC.cpp` | `flycast_term` | `gdxsv_emu_flycast_term`: ends the boot network tests before static destruction |
| `core/nullDC.cpp` | `dc_savestateAllowed` | `gdxsv_is_savestate_allowed` |
| `core/nullDC.cpp` | `dc_savestate` | `gdxsv_emu_savestate` |
| `core/nullDC.cpp` | `dc_loadstate` | `gdxsv_emu_loadstate` |
| `core/oslib/http_client.cpp` | `crackUrl` | thread_local, concurrent requests (update check, texture pack, boxart...) swapped hosts/paths |
| `core/oslib/http_client.cpp` | `get` | return the HTTP status, not always 200 |
| `core/oslib/http_client.cpp` | `get` | HTTP status |
| `core/oslib/i18n.cpp` | `reloadLanguage` | `dictMgr->add_directory("i18n-gdxsv");` |
| `core/oslib/oslib.cpp` | `os_UpdateInputState` | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/oslib/oslib.cpp` | `registerCrash` | `gdxsv_crash_append_log` |
| `core/oslib/oslib.cpp` | `uploadCrashes` | `gdxsv_is_multi_pov_guest` |
| `core/oslib/oslib.cpp` | `uploadCrashes` | `gdxsv_crash_append_tag` |
| `core/profiler/fc_profiler.h` |  | memset; no longer included through implot 0.18 |
| `core/rec-ARM/rec_arm.cpp` | `Arm32Assembler::compileOp` | round the product before the add (no fused multiply-add) |
| `core/rec-ARM64/rec_arm64.cpp` |  | round the product before the add (no fused multiply-add) |
| `core/rec-x64/rec_x64.cpp` |  | include `gdxsv/gdxsv_sh4_hooks.h` |
| `core/rec-x64/rec_x64.cpp` |  | `gdxsv_sh4_linkable` |
| `core/rec-x64/rec_x64.cpp` |  | ftrv and fipr inline, bit-exact with the interpreter, so netplay peers agree |
| `core/rec-x64/rec_x64.cpp` |  | fused multiply-add breaks sync of replays and netplay |
| `core/rec-x64/rec_x64.cpp` |  | `#endif // GDXSV_FP_COMPAT` |
| `core/rec-x64/rec_x64.cpp` |  | block linking |
| `core/rec-x64/rec_x64.cpp` |  | block linking, the link code is long |
| `core/rec-x64/rec_x64.cpp` |  | block linking |
| `core/rec-x64/rec_x64.cpp` |  | `gdxsv_sh4_render_return_ptr`, `gdxsv_sh4_render_only_mask_ptr`, `settings.gdxsv.skipRenderingBaseAddr` |
| `core/rec-x64/rec_x64.cpp` |  | cpuid is slow: probe the host once, not for every compiled block |
| `core/rec-x64/rec_x64.cpp` |  | block linking |
| `core/rend/CustomTexture.cpp` |  | include `gdxsv/gdxsv_custom_texture_source.h` |
| `core/rend/CustomTexture.cpp` | `CustomTexture::loadTexture` | `gdxsv_custom_texture_load_cells`: a texture made of 32x32 glyph cells without a replacement of its own is built from the glyph replacements |
| `core/rend/CustomTexture.cpp` | `CustomTexture::init` | DC2 embedded textures and the downloaded texture pack |
| `core/rend/CustomTexture.cpp` | `CustomTexture::loadCustomTextureAsync` | `gdxsv_custom_texture_on_queue`: gdxsv copies the cells from VRAM when the load is queued and keeps them for the loader thread |
| `core/rend/dx11/dx11context.cpp` |  | include `gdxsv/gdxsv_video_hooks.h` |
| `core/rend/dx11/dx11context.cpp` | `DX11Context::init` | `gdxsv_video_max_frame_latency`: `SetMaximumFrameLatency` (`gdxsv:MaxFrameLatency`) |
| `core/rend/dx11/dx11context.cpp` | `DX11Context::Present` | only duplicate frames on high refresh rate monitors when asked to |
| `core/rend/dx11/dx11_renderer.cpp` |  | include `gdxsv/gdxsv_video_hooks.h` |
| `core/rend/dx11/dx11_renderer.cpp` | `DX11Renderer::displayFramebuffer` | `gdxsv_video_grid_active`, `gdxsv_video_viewport` |
| `core/rend/gles/gldraw.cpp` |  | include `gdxsv/gdxsv_video_hooks.h` |
| `core/rend/gles/gldraw.cpp` | `OpenGLRenderer::renderLastFrame` | scale the final game image into the host's quadrant, before UI. |
| `core/rend/gles/gldraw.cpp` | `OpenGLRenderer::renderLastFrame` | final presentation rectangle |
| `core/rend/gles/gldraw.cpp` | `OpenGLRenderer::renderLastFrame` | final presentation rectangle |
| `core/rend/norend/norend.cpp` |  | present once per rendered frame, as the real renderers do; a second Present (frame swap after an immediate one) would pace headless runs at half speed. |
| `core/rend/norend/norend.cpp` |  | a framebuffer frame is presented once too |
| `core/rend/norend/norend.cpp` |  | Render or RenderFramebuffer since the last Present |
| `core/rend/vulkan/vulkan_context.cpp` |  | include `gdxsv/gdxsv_video_hooks.h` |
| `core/rend/vulkan/vulkan_context.cpp` | `VulkanContext::InitDevice` | `gdxsv_video_vulkan_extensions` |
| `core/rend/vulkan/vulkan_context.cpp` | `VulkanContext::DrawFrame` | scale the final game image into the host's quadrant, before UI. |
| `core/sdl/sdl.cpp` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/sdl/sdl.cpp` | `isWindowFullscreen` | native window controls can change SDL's flags directly |
| `core/sdl/sdl.cpp` | `captureMouse` | `settings.gdxsv.replayModeActive` |
| `core/sdl/sdl.cpp` | `emuEventCallback` | `settings.gdxsv.replayModeActive` |
| `core/sdl/sdl.cpp` | `input_sdl_init` | XInput and DirectInput switches; joystick events on their own thread |
| `core/sdl/sdl.cpp` | `input_sdl_init` | `gdxsv_input_attach_test_joystick` |
| `core/sdl/sdl.cpp` | `input_sdl_handle` | hidden replay guests must not enter full screen |
| `core/sdl/sdl.cpp` | `input_sdl_handle` | `settings.gdxsv.replayModeActive` |
| `core/sdl/sdl.cpp` | `input_sdl_handle` | handle outside window for network stats hovering |
| `core/sdl/sdl.cpp` | `input_sdl_handle` | `settings.gdxsv.replayModeActive` |
| `core/sdl/sdl.cpp` | `input_sdl_handle` | actual flag |
| `core/sdl/sdl.cpp` | `input_sdl_handle` | polled in gdxsv_input_poll_joysticks |
| `core/sdl/sdl.cpp` | `input_sdl_handle` | polled in gdxsv_input_poll_joysticks |
| `core/sdl/sdl.cpp` | `input_sdl_handle` | polled in gdxsv_input_poll_joysticks |
| `core/sdl/sdl.cpp` | `input_sdl_handle` | when polling mode is enabled, poll joystick state here (main thread) |
| `core/sdl/sdl.cpp` | `get_window_state` | actual flag |
| `core/sdl/sdl.cpp` | `sdl_recreate_window` | a capture rig tiles instances edge to edge, so it wants no border. |
| `core/sdl/sdl.cpp` | `sdl_get_window` | hidden replay guests follow the host window |
| `core/sdl/sdl.cpp` | `sdl_window_create` | a fullscreen window is not minimized on focus loss. |
| `core/sdl/sdl.cpp` | `sdl_window_destroy` | hidden replay guests must not save their render-target geometry. |
| `core/sdl/sdl.h` |  | nullptr before the window is created or in a headless run. |
| `core/serialize.cpp` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/serialize.cpp` | `dc_deserialize` | `gdxsv_emu_state_restored` |
| `core/types.h` |  | replay fade, 0.0 = silent, 1.0 = full volume (for fade-in effects) |
| `core/types.h` |  | `} gdxsv;` |
| `core/ui/gui.cpp` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/ui/gui.cpp` | `delayedKeysUp` | not static, the replay UI calls it |
| `core/ui/gui.cpp` | `gui_open_settings` | gdxsv_emu_menu_open() can take the key (replay pause menu, in-game stats) |
| `core/ui/gui.cpp` | `gui_display_commands` | `gdxsv_enabled` |
| `core/ui/gui.cpp` | `gui_display_commands` | `gdxsv_enabled` |
| `core/ui/gui.cpp` | `gui_display_commands` | `gdxsv_emu_gui_display_commands` |
| `core/ui/gui.cpp` | `drawBoxartBackground` | not static, the gdxsv menu calls it |
| `core/ui/gui.cpp` | `gui_network_start` | the handshake can be gone while this dialog is still drawn |
| `core/ui/gui.cpp` | `gui_display_ui` | `GdxsvReplay`, `GdxsvLatencyCheck` |
| `core/ui/gui.cpp` | `gui_display_ui` | `gdxsv_emu_gui_display` |
| `core/ui/gui.cpp` | `gui_draw_osd` | `gdxsv_gui_display_osd` |
| `core/ui/gui.cpp` | `gui_loadDeferredState` | new: loads a state requested by a hotkey while the sh4 code ran |
| `core/ui/gui.cpp` | `gui_loadState` | without threaded rendering a hotkey polled from the running sh4 code (maple DMA) defers the load to the frame end: loading there freed the dynarec block being executed (upstream bug) |
| `core/ui/gui.h` |  | `gui_loadDeferredState` |
| `core/ui/gui.h` |  | used by the gdxsv menu |
| `core/ui/gui.h` |  | used by the replay UI |
| `core/ui/gui.h` |  | `GdxsvReplay`, `GdxsvLatencyCheck` |
| `core/ui/gui_font.cpp` | `gui_loadFonts` | Japanese text is always needed. |
| `core/ui/mainui.cpp` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/ui/mainui.cpp` | `mainui_rend_frame` | `gui_loadDeferredState` after the frame |
| `core/ui/mainui.cpp` | `mainui_loop` | `gdxsv_emu_mainui_loop_start` |
| `core/ui/mainui.cpp` | `mainui_loop` | gdxsv_emu_mainui_loop gets whether a frame was rendered |
| `core/ui/mainui.cpp` | `mainui_loop` | `gdxsv_emu_mainui_reinit_needed` |
| `core/ui/mainui.cpp` | `mainui_loop` | `gdxsv_emu_mainui_loop` |
| `core/ui/mainui.cpp` | `mainui_loop` | `gdxsv_emu_mainui_loop_end` |
| `core/ui/settings.cpp` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/ui/settings.cpp` | `gui_settings_advanced` | `gdxsv_emu_settings_advanced` |
| `core/ui/settings.cpp` | `gui_display_settings` | `Gdxsv`, `gdxsv_emu_settings_gdxsv_tab` |
| `core/ui/settings_controls.cpp` | `gui_settings_controls` | XInput and DirectInput switches, joystick polling |
| `core/ui/settings_general.cpp` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/ui/settings_general.cpp` | `manageSinglePath` | not static, the gdxsv replay folder setting uses it |
| `core/ui/settings_general.cpp` | `gui_settings_general` | `gdxsv_emu_settings_replay_folder` |
| `core/ui/settings_video.cpp` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/ui/settings_video.cpp` | `gui_settings_video` | `gdxsv_emu_settings_video` |
| `core/windows/win_vmem.cpp` | `LPVOID` | fixed base address for deterministic memory layout (useful for Cheat Engine). |
| `core/windows/win_vmem.cpp` | `init` | place base_alloc so that ram_base (base_alloc + sizeof(Sh4RCB)) lands on a 0x10000000 boundary. |
| `core/windows/winmain.cpp` |  | breakpad include path of the MSVC (clang-cl) build |
| `core/windows/winmain.cpp` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `core/windows/winmain.cpp` | `main` | `gdxsv_exit_code` |
| `core/windows/winmain.cpp` | `getThreadName` | include `gdxsv/gdxsv_video_hooks.h` |
| `core/windows/winmain.cpp` | `os_VideoRoutingPublishFrameTexture` | `gdxsv_video_sender_name` |
| `core/windows/winmain.cpp` | `os_VideoRoutingPublishFrameTexture` | `gdxsv_video_sender_name` |
| `core/wsi/sdl.cpp` | `SDLGLGraphicsContext::init` | only duplicate frames on high refresh rate monitors when asked to |
| `core/wsi/sdl.cpp` | `SDLGLGraphicsContext::changeGLSwapInterval` | only duplicate frames on high refresh rate monitors when asked to |
| `core/wsi/switcher.cpp` |  | include `gdxsv/gdxsv_null_imgui_driver.h` |
| `core/wsi/switcher.cpp` | `initRenderApi` | `gdxsv_headless`, `GdxsvNullImGuiDriver` |
| `core/wsi/switcher.cpp` | `termRenderApi` | `gdxsv_headless`, `gdxsv_video_term` |
| `shell/apple/common/http_client.mm` |  | delegate of the asynchronous NSURLSession request in http::get |
| `shell/apple/common/http_client.mm` | `get` | use the asynchronous NSURLSession API and pump the run loop when called from the main thread |
| `shell/apple/common/http_client.mm` | `get` | content was filled by the handler |
| `shell/apple/emulator-osx/emulator-osx/SDLApplicationDelegate.mm` |  | include `gdxsv/gdxsv_emu_hooks.h` |
| `shell/apple/emulator-osx/emulator-osx/SDLApplicationDelegate.mm` | `CustomApplicationMain` | `gdxsv_emu_app_init` |
| `shell/apple/emulator-osx/emulator-osx/SDLApplicationDelegate.mm` | `CustomApplicationMain` | hidden workers must not take keyboard focus. |
| `shell/apple/emulator-osx/emulator-osx/osx-main.mm` | `os_RunInstance` | include `gdxsv/gdxsv_video_hooks.h` |
| `shell/apple/emulator-osx/emulator-osx/osx-main.mm` | `os_VideoRoutingPublishFrameTexture` | `gdxsv_video_sender_name` |
| `shell/apple/emulator-osx/emulator-osx/osx-main.mm` | `os_VideoRoutingPublishFrameTexture` | `gdxsv_video_sender_name` |
<!-- upstream_spots end -->
