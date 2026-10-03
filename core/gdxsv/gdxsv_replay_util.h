#pragma once
#include <string>

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

void gdxsv_start_replay(const std::string& replay_path, int pov, bool four_screen);
// Live autoplay (gdxsv:LiveAutoNext) carries on from this battle to the next.
void gdxsv_start_live_spectate(const std::string& battle_code, int pov, bool four_screen = false);
// For a live battle started elsewhere (the command line).
void gdxsv_live_autoplay_begin(const std::string& battle_code, int pov, bool four_screen);

// Viewers watching battle_code right now. Never blocks: returns the last known
// value and refreshes in the background. force_refresh skips the interval.
int gdxsv_live_viewer_count(const std::string& battle_code, bool force_refresh = false);
// user_exit: the viewer left from the pause menu, which also ends Live autoplay.
void gdxsv_end_replay(std::string error = {}, bool user_exit = false);
void gdxsv_replay_select_dialog();
