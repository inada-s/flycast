#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include "gdxsv.pb.h"

// Display data only: never copy or retain pointers into the recording's inputs.
struct GdxsvReplayUiState {
	enum class State {
		None, Start, LbsStartBattleFlow, McsWaitJoin, McsSessionExchange,
		McsInBattle, McsWaitStartMsg, End,
	};

	static constexpr int MaxRounds = 10;
	static constexpr int MaxPlayers = 4;
	struct Round {
		int startFrame = 0;
		int endFrame = 0;
		bool hasResult = false;
		int winTeam = 0;
		int usedMsCount = 0;
		std::array<int, MaxPlayers> usedMs{};
	};

	std::string battleCode;
	int inputCount = 0;
	int roundCount = 0;
	bool canChangeRound = false;
	int64_t startAt = 0;
	double inputSeconds = 0.01668335002;
	std::array<Round, MaxRounds> rounds{};
	int userCount = 0;
	std::array<int, MaxPlayers> userTeams{};

	State state = State::None;
	int playbackFrame = 0;
	int currentRound = 0;
	int timelineStart = 0;
	int timelineEnd = 0;
	uint64_t timelineRevision = 0;
	int bootSeekFrames = 180;
	int playSpeed = 0;
	bool inGame = false;
	bool seeking = false;
	bool paused = false;
	bool pauseMenuOpen = false;
	bool loading = false;
	bool takeover = false;
	int takeoverCountdown = 0;
	uint16_t takeoverTargetInput = 0;
	bool liveMode = false;
	bool liveAtEdge = false;
	int liveBufferFrames = 30;

	bool IsLiveSeekTarget(int frame) const {
		// An older round's right edge is not the latest broadcast position.
		return liveMode && inputCount > 0 && timelineEnd == inputCount && frame == inputCount;
	}

	int FrameAtProgress(float progress) const {
		if (timelineEnd <= timelineStart || !std::isfinite(progress))
			return timelineStart;
		const double fraction = std::clamp(static_cast<double>(progress), 0.0, 1.0);
		const int64_t length = static_cast<int64_t>(timelineEnd) - timelineStart;
		return static_cast<int>(timelineStart + std::llround(fraction * length));
	}

	// Called only by the recording's owner. Work is bounded by ten rounds and
	// four players, regardless of the number of recorded input frames.
	static GdxsvReplayUiState FromLog(const proto::BattleLogFile& log) {
		GdxsvReplayUiState ui;
		ui.battleCode = log.battle_code();
		ui.inputCount = log.inputs_size();
		ui.roundCount = std::min(log.start_msg_indexes_size(), MaxRounds);
		ui.canChangeRound = log.start_msg_indexes_size() > 0 &&
			log.start_msg_indexes_size() == log.start_msg_randoms_size();
		ui.startAt = log.start_at();
		if (ui.startAt != 0 && log.end_at() > ui.startAt && ui.inputCount > 0) {
			const double seconds = static_cast<double>(log.end_at() - ui.startAt) / ui.inputCount;
			if (0.01 <= seconds && seconds <= 0.2)
				ui.inputSeconds = seconds;
		}
		ui.userCount = std::min(log.users_size(), MaxPlayers);
		for (int i = 0; i < ui.userCount; ++i)
			ui.userTeams[i] = log.users(i).team();
		for (int i = 0; i < ui.roundCount; ++i) {
			auto& round = ui.rounds[i];
			round.startFrame = log.start_msg_indexes(i);
			round.endFrame = i + 1 < log.start_msg_indexes_size() ? log.start_msg_indexes(i + 1) : ui.inputCount;
			if (i < log.round_data_size()) {
				const auto& result = log.round_data(i);
				round.hasResult = true;
				round.winTeam = result.win_team();
				round.usedMsCount = std::min(result.used_ms_size(), MaxPlayers);
				for (int j = 0; j < round.usedMsCount; ++j)
					round.usedMs[j] = result.used_ms(j);
			}
		}
		return ui;
	}
};

// Publish/copy one coherent value. Neither emulation nor drawing runs under
// this mutex; the UI owns the returned copy for the duration of its frame.
class GdxsvReplayUiSnapshot {
public:
	void Publish(GdxsvReplayUiState state) {
		std::lock_guard<std::mutex> lock(mutex_);
		state_ = std::move(state);
	}
	GdxsvReplayUiState Read() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return state_;
	}
private:
	mutable std::mutex mutex_;
	GdxsvReplayUiState state_;
};
