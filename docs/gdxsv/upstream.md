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

- An upstream merge moves it; this page lists the files, not the lines, so it
  stays right until a file is added or dropped.

## Added, not in upstream

| Path | What |
|---|---|
| `core/gdxsv/` | all gdxsv features: lobby, rollback, replay, spectating, patches, UI; textures of the translation |
| `gdxsv_patch/` | SH4 network payload of the game (`build.sh`, `abi.py` -> `gdxsv_patch_abi.h`, `gdxsv_patch.inc`) |
| `gdxsv_langmod/` | translation sources (`translation.csv`, PSD textures) and their generators |
| `core/deps/protobuf-3.13.0/` | protobuf runtime (`gdxsv.pb.cc`) |
| `resources/i18n-gdxsv/` | UI translations of the gdxsv pages |
| `core/sleep.cpp`, `core/sleep.h` | precise sleep for the frame limiter |
| `core/ui/null_imgui_driver.h` | UI driver that draws nothing (headless) |
| `tests/src/Gdxsv*.cpp`, `tests/src/test_stubs.cpp` | unit tests |
| `tools/rbk_test/` | rollback and live spectating rigs |
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
| Start, reset, save states | `core/nullDC.cpp`, `core/emulator.cpp/.h`, `core/serialize.cpp` | gdxsv init, start and reset hooks; restore hook after a state load; find content in the working directory |
| Game RPC | `core/hw/holly/sb_mem.cpp` | the payload's `gdx_rpc` call reaches the host through area 0 |
| Frame hooks | `core/hw/pvr/Renderer_if.cpp/.h`, `core/hw/pvr/ta_ctx.cpp` | vblank and end-of-frame hooks; headless skips rendering; fast-forward counts in frame skip |
| Rollback | `core/network/ggpo.cpp/.h`, `core/hw/pvr/elan.cpp`, `core/hw/mem/mem_watch.h`, `core/hw/maple/maple_cfg.h` | gdxsv rollback session on top of upstream GGPO; flag bits sent beside the pad input; state hash log; test logs; memory watch shared with gdxsv; a fault on an already-saved page no longer loops |
| Determinism | `core/hw/sh4/interpr/sh4_fpu.cpp`, `core/rec-ARM/rec_arm.cpp`, `core/rec-ARM64/rec_arm64.cpp`, `core/rec-x64/rec_x64.cpp`, `core/hw/sh4/dyna/shil_canonical.h` | `fmac` without fused multiply-add (`GDXSV_FP_COMPAT`) |
| Dynarec | `core/rec-x64/rec_x64.cpp`, `core/hw/sh4/dyna/blockmanager.cpp/.h`, `core/hw/sh4/dyna/decoder.cpp`, `core/hw/arm7/arm7_rec_x64.cpp` | x64 block linking (all games); output-only game functions skipped on seek frames; slow idle loop hack; ARM7 `cpuid` probed once |
| Memory | `core/windows/win_vmem.cpp`, `core/linux/posix_vmem.cpp` | Windows: fixed RAM base (host address low 28 bits = guest address); Linux: one shared-memory name per process (four-screen replay) |
| Frame pacing | `core/ui/mainui.cpp`, `core/cfg/option.cpp/.h` | `FixedFrequency` limiter with precise sleep and a replay trim; `LimitFPS` an option, not a constant |
| Audio | `core/audio/audiostream.cpp/.h`, `audiobackend_coreaudio.cpp`, `audiobackend_directsound.cpp`, `audiobackend_sdl2.cpp` | rate control for replay and spectating (backends report their queue level); per-instance volume (four-screen replay) and fade |
| Input | `core/input/gamepad_device.cpp/.h`, `core/sdl/sdl_gamepad.cpp/.h`, `core/sdl/sdl.cpp/.h`, `core/oslib/oslib.cpp/.h` | joystick polling from the emulation thread; XInput / DirectInput switches; opposite directions cancel for the game |
| Window | `core/sdl/sdl.cpp`, `core/wsi/sdl.cpp`, `core/wsi/switcher.cpp`, `core/linux-dist/main.cpp` | headless (no graphics API), borderless, window position for several instances, four-screen grid; frames duplicated on a monitor over 60 Hz only with `DupeFrames` |
| Video routing | `core/wsi/gl_context.cpp`, `core/rend/dx11/dx11context.cpp`, `core/rend/vulkan/vulkan_context.cpp`, `core/rend/gles/gles.cpp`, `core/rend/gl4/gles.cpp` | Spout / Syphon output drawn and torn down with the graphics context |
| Config | `core/cfg/cfg.cpp` | a new `emu.cfg` gets the gdxsv defaults |
| UI | `core/ui/gui.cpp/.h`, `core/ui/settings*.cpp/.h`, `core/ui/gui_font.cpp`, `core/oslib/i18n.cpp` | gdxsv settings page, Live & Replays menu, update popup, gdxsv translations, Japanese fonts always loaded |
| Platform info | `core/windows/winmain.cpp`, `shell/apple/emulator-osx/emulator-osx/osx-main.mm`, `SDLApplicationDelegate.mm`, `core/network/net_platform.h`, `core/network/miniupnp.cpp/.h` | machine id, connection medium (VPN), open URLs (also Linux); UDP sockets ignore ICMP port unreachable on Windows; UPnP mapping details |
| HTTP | `core/oslib/http_client.cpp/.h`, `shell/apple/common/http_client.mm` | per-call URL buffers (concurrent requests mixed hosts) |
| Textures | `core/rend/CustomTexture.cpp`, `core/rend/TexCache.cpp` | embedded and downloaded texture packs for the game (`T13306M`) |
| Logs | `core/log/LogManager.cpp`, `core/log/InMemoryListener.h`, `core/profiler/fc_profiler.h` | one log file per instance; 100 lines kept in memory (was 20); a missing include |
| Settings struct | `core/types.h` | `settings.gdxsv` runtime state |
| Build | `CMakeLists.txt`, `CMakePresets.json` (removed), `tests/CMakeLists.txt`, `resources/resources.cmake`, `.gitignore` | gdxsv sources, protobuf, build options, gdxsv tests and resources |
| CI | `.github/workflows/c-cpp.yml`, `crowdin_translate.yml` | gdxsv builds and releases from `gdxsv-*` tags |
