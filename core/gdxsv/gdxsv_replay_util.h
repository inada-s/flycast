#pragma once
#include <string>

// Ensures the shared slot-99 bootstrap savestate exists for this disk,
// downloading it if this install has never needed it before. It is a canned
// "sitting at the network-ready lobby screen" snapshot that file replay,
// Live Spectate and the rollback test harness all resume from, so it must be
// present before any dc_loadstate(99).
bool gdxsv_ensure_replay_savestate(int disk);

void gdxsv_start_replay(const std::string& replay_path, int pov, bool four_screen);
void gdxsv_start_live_spectate(const std::string& battle_code, int pov);

// Viewers watching battle_code right now. Never blocks: returns the last known
// value and refreshes in the background. force_refresh skips the interval.
int gdxsv_live_viewer_count(const std::string& battle_code, bool force_refresh = false);
void gdxsv_end_replay(std::string error = {});
void gdxsv_replay_select_dialog();
