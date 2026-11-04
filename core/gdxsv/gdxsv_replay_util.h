#pragma once
#include <string>

#include "gdxsv_camera.h"

// Ensures the shared slot-99 bootstrap savestate exists for this disk,
// downloading it if this install has never needed it before. It is a canned
// "sitting at the network-ready lobby screen" snapshot that file replay,
// Live Spectate and the rollback test harness all resume from, so it must be
// present before any dc_loadstate(99).
bool gdxsv_ensure_replay_savestate(int disk);

namespace proto { class BattleLogFile; }

// gdxsv:ReplayPath when the folder exists, else data/replays. A folder on an
// unplugged drive falls back instead of failing.
std::string gdxsv_replay_dir();
// Writes log to filename in dir (from gdxsv_replay_dir()), or in data/replays
// if dir cannot take it, so a finished match is not lost. Safe on any thread.
// Returns the path written, empty on failure.
std::string gdxsv_save_replay_file(const proto::BattleLogFile& log, const std::string& dir, const std::string& filename);

// pov_mode applies to a single-screen replay; four screens show the players' own views.
void gdxsv_start_replay(const std::string& replay_path, int pov, bool four_screen, GdxsvPovMode pov_mode);
// Live autoplay (gdxsv:LiveAutoNext) carries on from this battle to the next. pov_mode applies to a single-screen battle
// and to those autoplay moves on to.
void gdxsv_start_live_spectate(const std::string& battle_code, int pov, bool four_screen, GdxsvPovMode pov_mode);
// For a live battle started elsewhere (the command line).
void gdxsv_live_autoplay_begin(const std::string& battle_code, int pov, bool four_screen);

// Viewers watching battle_code right now. Never blocks: returns the last known
// value and refreshes in the background. force_refresh skips the interval.
int gdxsv_live_viewer_count(const std::string& battle_code, bool force_refresh = false);
// user_exit: the viewer left from the pause menu, which also ends Live autoplay.
void gdxsv_end_replay(std::string error = {}, bool user_exit = false);
// Test switch for gdxsv_tests O7, O8: GDXSV_TEST_START_REPLAY names a file. Once it appears while the
// game is online, the replay whose path it holds ("four_screen <path>": on four screens) starts as
// from the menu and the replay browser, or is refused where the menu would not open or the button is
// disabled.
void gdxsv_test_start_replay_tick();
void gdxsv_replay_select_dialog();
