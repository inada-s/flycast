# core/gdxsv

All gdxsv code of this fork. Rules for adding to it: `AGENTS.md`. Features,
options and changed upstream files: `docs/gdxsv/`.

## Glossary

| Term | Meaning |
|---|---|
| gdxsv | the online service of Mobile Suit Gundam: Federation vs. Zeon (DX); also the global `Gdxsv gdxsv` object |
| disk | which game is running: 1 = Federation vs. Zeon, 2 = Federation vs. Zeon DX (`Gdxsv::disk_`) |
| LBS | lobby server: login, lobby, match making; also relays live spectating |
| MCS | match server: the battle connection of the UDP backend |
| patch | SH4 code written into guest RAM (`gdxsv_patch/`, `gdxsv_patch.inc`) that hands the game's network calls to the emulator |
| gdx RPC | a request the patched game writes to guest RAM for the emulator to serve (socket open, read, write, poll) |
| backend | where the game's network calls go: TCP (lobby), UDP (match), rollback (GGPO) or replay |
| relay | a peer or server that forwards rollback packets between peers that cannot reach each other |
| replay | a recorded battle played back from its inputs |
| live | spectating a battle while it is played: uplink from a player, downlink to a spectator |
| POV | point of view: which player's screen a replay shows |
| hook header | `gdxsv_*_hooks.h`: what an upstream file calls into this directory |

## Files

| Files | What |
|---|---|
| `gdxsv.cpp/.h` | the `Gdxsv` object: game detection, patching, RPC, settings, backend selection |
| `gdxsv.cmake` | gdxsv sources and build settings, included from `CMakeLists.txt` |
| `gdxsv_config.cpp/.h` | gdxsv options (`config::Gdx*`, `[gdxsv]` in emu.cfg) |
| `gdxsv_sh4_hooks.cpp/.h` | calls from the SH4 dynarec (slow idle loop hack) |
| `gdxsv_emu_hooks.cpp/.h` | calls from the emulator (start, stop, frame, input, UI) into gdxsv |
| `gdx_rpc.h` | gdx RPC request layout and codes |
| `gdxsv_patch.inc`, `gdxsv_patch_abi.h` | SH4 patch bytes and the addresses it hooks; generated from `gdxsv_patch/` |
| `gdxsv.pb.cc/.h` | protobuf messages shared with the server (generated) |
| `lbs_message.h`, `mcs_message.h` | LBS and MCS message formats and readers |
| `gdxsv_network.cpp/.h` | sockets, UDP remotes, relay ping and selection |
| `gdxsv_upnp.cpp/.h` | `MiniUPnP` plus the port-mapping check and addresses the P2P status uses |
| `juice_helper.c/.h` | STUN binding request and response (from libjuice) |
| `gdxsv_backend_tcp.cpp/.h` | TCP backend: the lobby (LBS) connection |
| `gdxsv_backend_udp.cpp/.h` | UDP backend: MCS over UDP |
| `gdxsv_backend_rollback.cpp/.h` | rollback backend: GGPO session for a battle |
| `gdxsv_ggpo_hooks.cpp/.h` | called from `core/network/ggpo.cpp`: per-frame state hash log (`gdxsv:hashlog`) |
| `gdxsv_backend_replay.cpp/.h` | replay backend: plays a recorded battle |
| `gdxsv_replay_util.cpp/.h`, `gdxsv_replay_ui.h` | replay files, bootstrap save state, replay UI data |
| `gdxsv_save_state.cpp/.h` | in-memory save states for rollback |
| `gdxsv_spectator_uplink.cpp/.h` | sends a player's confirmed inputs to LBS for live spectating |
| `gdxsv_spectator_downlink.cpp/.h` | receives live inputs from LBS |
| `gdxsv_spectate_sync.cpp/.h` | keeps spectator instances on one machine on the same frame |
| `gdxsv_multi_pov*.cpp/.h/.mm` | 4-player replay grid: launch, window placement per platform (`_win`, `_sdl`, `_apple`, `_none`) |
| `gdxsv_sdl.cpp/.h` | SDL hints, window flags and headless display size called from `core/sdl/sdl.cpp` |
| `gdxsv_round_counters.h` | round and win counts of a battle |
| `gdxsv_slowdown.cpp/.h` | experimental arcade slowdown in DX battles |
| `gdxsv_projectile_view.cpp/.h` | debug overlay of DX projectiles |
| `gdxsv_key_display.cpp/.h` | on-screen input display |
| `gdxsv_gui_settings.cpp/.h` | gdxsv settings tab and base settings |
| `gdxsv_translation.cpp/.h`, `gdxsv_translation_patch.inc` | game translation; the `.inc` is generated from `gdxsv_langmod/` |
| `gdxsv_custom_texture_source.cpp/.h`, `gdxsv_custom_texture_update.cpp/.h`, `Textures/`, `textures.rc` | bundled custom textures and their download |
| `gdxsv_update.cpp/.h`, `gdxsv_update_apple.mm` | client self-update |
| `gdxsv_https_latency*.cpp/.h/.mm` | HTTPS latency to each server region, per platform (`_win`, `_curl`, `_apple`) |
| `gdxsv_prof.cpp/.h` | named timers for profiling |
| `gdxsv_frame_limiter.cpp/.h` | FixedFrequency frame limiter of the main UI loop (period, replay trim) |
| `gdxsv_sleep.cpp/.h` | precise sleep for the frame limiter and network threads |
| `gdxsv_null_imgui_driver.h` | UI driver that draws nothing (headless runs) |
| `libs.h` | guest memory access helpers |
