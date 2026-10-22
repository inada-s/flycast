#pragma once

#include <mutex>

#include "lbs_message.h"

// Overlay box at the bottom center while this user waits for a lobby match: a spinner and
// the users waiting. Observe() and ObserveSent() may run on the emulation thread;
// DisplayOSD() on the UI thread.
class GdxsvLobbyStatus {
   public:
	void DisplayOSD();
	void Observe(const LbsMessage& msg);
	void ObserveSent(const LbsMessage& msg);
	void Clear();

	void enabled(bool enabled) { enabled_ = enabled; }
	bool enabled() const { return enabled_; }

   private:
	struct Status {
		bool entered = false;
		int waiting = 0;
	};

	bool enabled_ = false;
	std::mutex mutex_;
	Status status_;
	int team_waiting_[2] = {0, 0};
};
