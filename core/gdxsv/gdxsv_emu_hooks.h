#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
// Functions provided to the emulator by the gdxsv module.

// Signed microseconds added to the main loop's frame period (see
// gdxsv_frame_limiter.cpp). Negative runs the emulator slightly fast, positive
// slightly slow. Replay and Live Spectate steer this to hold a target distance
// behind the live edge without the visible hitch a whole-frame stall causes.
// Zero everywhere else, so normal play and online battle are untouched.
extern std::atomic<int> gdxsv_frame_period_trim_us;

namespace http {
struct PostField;
}

bool gdxsv_enabled();

bool gdxsv_is_ingame();

bool gdxsv_is_online();

bool gdxsv_is_savestate_allowed();

void gdxsv_emu_flycast_init();
void gdxsv_emu_flycast_term();

// macOS: identify hidden guests before AppKit starts and suppress foreground activation.
bool gdxsv_emu_app_init(int argc, const char* const argv[]);

void gdxsv_emu_start();

void gdxsv_emu_reset();

// All deserialization paths, including replay seeks and rollback.
void gdxsv_emu_state_restored();

void gdxsv_emu_vblank();

void gdxsv_emu_end_frame();

// Emulator::run and the emulation thread: after a frame ran (before ggpo::nextFrame).
void gdxsv_emu_next_frame();
// The emulation thread (threaded rendering): after ggpo::nextFrame.
void gdxsv_emu_frame_end();

// mainui_loop: before the first frame, once per frame after the renderer
// switch (the FixedFrequency wait when the frame was rendered), at exit.
void gdxsv_emu_mainui_loop_start();
void gdxsv_emu_mainui_loop(bool rendered);
void gdxsv_emu_mainui_loop_end();

void gdxsv_emu_rpc();

void gdxsv_emu_savestate(int slot);

void gdxsv_emu_loadstate(int slot);

bool gdxsv_emu_menu_open();

// Commands (pause) menu, first column: the Live & Replays button.
void gdxsv_emu_gui_display_commands(float buttonWidth);
void gdxsv_emu_gui_display();

void gdxsv_emu_gui_display_replay();

void gdxsv_emu_settings_gdxsv_tab();
void gdxsv_emu_settings_video();

void gdxsv_emu_apply_base_settings();

// Settings > General > Custom Paths and the gdxsv tab. Not on Android or iOS.
void gdxsv_emu_settings_replay_folder();

// Settings > Advanced: gdxsv debug fields.
void gdxsv_emu_settings_advanced();

// settings.gdxsv.skipRenderingAddr for a rollback frame run: the skip
// rendering hook address, or 0 to render in full.
uint32_t gdxsv_emu_skip_rendering_addr(bool last_frame);

const char* gdxsv_emu_settings_text_for_preparing_font();

void gdxsv_gui_display_osd();

void gdxsv_crash_append_log(FILE* f);

void gdxsv_crash_append_tag(const std::string& logfile, std::vector<http::PostField>& post_fields);

bool gdxsv_is_using_memwatch();

// Headless test mode (gdxsv:headless=yes): no window, no graphics API, the
// null renderer. Meant for automated runs on machines without a display.
bool gdxsv_headless();

// True in a 4-player replay guest (a screen spawned and driven by a host).
bool gdxsv_is_multi_pov_guest();

// Alt+Enter / F11. Hidden replay guests consume the key; the host uses
// the ordinary SDL full-screen controls.
bool gdxsv_emu_toggle_fullscreen();

// Exit status the process should end with. gdxsv sets it from local test
// results so a harness can tell a finished match from a broken one.
void gdxsv_set_exit_code(int code);
int gdxsv_exit_code();

// Exit the process immediately with the given status, flushing open files but
// skipping teardown. Used to end a finished headless test: there is no window,
// GPU or audio device to release, and the normal SDL teardown path can crash
// on the way out, which would mask the test's real result.
[[noreturn]] void gdxsv_headless_exit(int code);
