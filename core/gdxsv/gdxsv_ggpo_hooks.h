// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <mutex>
#include <random>

#include "types.h"

// Functions provided to core/network/ggpo.cpp by the gdxsv module.

void gdxsv_ggpo_session_start();
void gdxsv_ggpo_session_stop();
// GGPO saved the state of a frame, rollbacks included.
void gdxsv_ggpo_state_saved(int frame, const unsigned char* buffer, int len);
// Draws the network stats of the gdxsv session (one block per peer). False when not a gdxsv session.
bool gdxsv_ggpo_display_stats();
// GGPO frame delay for the frame delay setting: +1 in a gdxsv rollback battle, whose input is added
// before ggpo_advance_frame increments the frame.
int gdxsv_ggpo_frame_delay(int delay);
// gdxsv rollback battle, after ggpo_advance_frame: waits (pumping GGPO) while the prediction barrier
// would reject the next local input, so that input never waits before the advance, where a rollback would load a
// state older than the emulated frame. False on a GGPO error.
bool gdxsv_ggpo_wait_prediction_barrier();

// gdxsv state of a GGPO session, shared by core/network/ggpo.cpp and gdxsv_ggpo.cpp.
struct GdxsvGgpoState {
	int playerHandles[4];  // GGPOPlayerHandle per player
	int disconnectFlags;   // bit per disconnected player, from ggpo_synchronize_input
	std::array<int, 3> inputBlockCount;
	bool useExInput;
	bool testLog;  // GGPO_TEST_LOG: log timesync skips and rollback loads for gdxsv_tests/rollback
	bool testNoSkipReplay;  // GGPO_TEST_NO_SKIP_REPLAY: skips after the seek frame are not replayed (control of rollback case B1)
	int totalRollbackFrames;
	int totalTimeSync;
	float fpsAvg;
	bool useRandInput;
	u32 randInputMask;
	std::mt19937 randSource;
	bool inputBeforeAdvance;  // gdxsv rollback battle: the local input is added before ggpo_advance_frame
};
extern GdxsvGgpoState gdxsv_ggpo_state;

struct GGPOSession;
// Upstream state of core/network/ggpo.cpp that gdxsv_ggpo.cpp starts a session with.
struct GdxsvGgpoUpstream {
	GGPOSession** session;
	std::recursive_mutex* mutex;
	int* localPlayerNum;
	int* localPlayer;  // GGPOPlayerHandle
	int* remotePlayer;
	bool* synchronized;
	int* analogAxes;
	int* inputSize;
	int* timesyncOccurred;  // frames the "Behind" bar of the stats stays red
};
// Defined in core/network/ggpo.cpp. Fills cb (a GGPOSessionCallbacks) with upstream's callbacks.
GdxsvGgpoUpstream gdxsv_ggpo_upstream(void* cb);
