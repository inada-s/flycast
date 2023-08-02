#pragma once

#include "gdxsv.pb.h"
#include "gdxsv_save_state.h"
#include "lbs_message.h"
#include "mcs_message.h"
#include "types.h"

// Mock network implementation to replay local battle log
class GdxsvBackendReplay {
   public:
	enum class State {
		None,
		Start,
		LbsStartBattleFlow,
		McsWaitJoin,
		McsSessionExchange,
		McsInBattle,
		McsWaitStartMsg,
		End,
	};

	struct ReplayCtrlCommand {
		enum Command {
			None,
			// System used
			SaveFirstFrame,
			SetMaxLag,
			SeekToBriefing,

			// User control
			TogglePauseMenu,
			Pause,
			Resume,
			TogglePause,
			StepFrame,
			SomeFrameForward,
			SomeFrameBackward,
			SetSpeed,
			ChangeSpeed,
			SetRound,
			ChangeRound,
		};

		Command cmd;
		int arg1;
		int arg2;
	};

	void Reset();
	void OnMainUiLoop();
	void OnEndOfFrame();
	void OnNextFrame();
	bool OnOpenMenu();
	void DisplayOSD();

	bool StartFile(const char *path, int pov);
	bool StartBuffer(const std::vector<u8> &buf, int pov);
	void Stop();
	bool ChangeRoundAvailable() const;

	// Network Backend Interface
	void Open();
	void Close();
	u32 OnSockWrite(u32 addr, u32 size);
	u32 OnSockRead(u32 addr, u32 size);
	u32 OnSockPoll();

private:
	bool Start();
	void PrintDisconnectionSummary();
	void ProcessLbsMessage();
	void ProcessMcsMessage(const McsMessage &msg);
	void ApplyPatch(bool first_time);
	void RestorePatch();
	void RenderPauseMenu();

	// Replay control (need mutex lock)
	void CtrlSpeedUp();
	void CtrlSpeedDown();
	void CtrlSetSpeed(int speed);
	void CtrlTogglePause();
	void CtrlStepFrame();
	void CtrlSomeFrameBackward();
	void CtrlSomeFrameForward();
	void CtrlSetRound(int round);
	void CtrlNextRound();
	void CtrlPrevRound();

	State state_;
	class {
	public:
		void push_back(const ReplayCtrlCommand& cmd) {
			std::lock_guard lock(mtx_);
			cmds_.push_back(cmd);
		}
		void pop_front() {
			std::lock_guard lock(mtx_);
			if (!cmds_.empty()) cmds_.pop_front();
		}
		bool try_get_front(ReplayCtrlCommand& cmd) {
			std::lock_guard lock(mtx_);
			if (!cmds_.empty()) {
				cmd = cmds_.front();
				return true;
			}
			return false;
		}
		size_t size() {
			std::lock_guard lock(mtx_);
			return cmds_.size();
		}
		bool empty() {
			std::lock_guard lock(mtx_);
			return cmds_.empty();
		}
		bool contains(ReplayCtrlCommand::Command c) {
			std::lock_guard lock(mtx_);
			for (const auto& cmd : cmds_) {
				if (cmd.cmd == c) {
					return true;
				}
			}
			return false;
		}
		void clear() {
			std::lock_guard lock(mtx_);
			cmds_.clear();
		}
	private:
		std::recursive_mutex mtx_;
		std::deque<ReplayCtrlCommand> cmds_;
	} ctrl_commands_;
	bool end_of_frame_;
	bool seeking_;
	bool pause_menu_opend_;
	LbsMessageReader lbs_tx_reader_;
	proto::BattleLogFile log_file_;
	std::deque<u8> recv_buf_;
	int pov_;
	int recv_delay_;
	int start_msg_count_;
	int key_msg_count_;
	bool ctrl_pause_;
	int ctrl_play_speed_;
	int ctrl_step_frame_;
	bool save_converted_log_;
};