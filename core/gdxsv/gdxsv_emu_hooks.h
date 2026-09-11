#pragma once
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>
// Functions provided to the emulator by the gdxsv module.

// Signed microseconds added to the main loop's frame period (see get_period in
// core/ui/mainui.cpp). Negative runs the emulator slightly fast, positive
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

void gdxsv_emu_start();

void gdxsv_emu_reset();

// All deserialization paths, including replay seeks and rollback.
void gdxsv_emu_state_restored();

void gdxsv_emu_vblank();

void gdxsv_emu_end_frame();

void gdxsv_emu_next_frame();

void gdxsv_emu_mainui_loop();

void gdxsv_emu_rpc();

void gdxsv_emu_savestate(int slot);

void gdxsv_emu_loadstate(int slot);

bool gdxsv_emu_menu_open();

bool gdxsv_widescreen_hack_enabled();

void gdxsv_emu_gui_display();

void gdxsv_emu_gui_display_replay();

void gdxsv_emu_settings_gdxsv_tab();

void gdxsv_emu_apply_base_settings();

const char* gdxsv_emu_settings_text_for_preparing_font();

void gdxsv_gui_display_osd();

void gdxsv_crash_append_log(FILE* f);

void gdxsv_crash_append_tag(const std::string& logfile, std::vector<http::PostField>& post_fields);

bool gdxsv_is_using_memwatch();

// Headless test mode (gdxsv:headless=yes): no window, no graphics API, the
// null renderer. Meant for automated runs on machines without a display.
bool gdxsv_headless();

// Exit status the process should end with. gdxsv sets it from local test
// results so a harness can tell a finished match from a broken one.
void gdxsv_set_exit_code(int code);
int gdxsv_exit_code();

// Exit the process immediately with the given status, flushing open files but
// skipping teardown. Used to end a finished headless test: there is no window,
// GPU or audio device to release, and the normal SDL teardown path can crash
// on the way out, which would mask the test's real result.
[[noreturn]] void gdxsv_headless_exit(int code);
