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

	void Reset();
	void OnMainUiLoop();
	void OnEndOfFrame();
	void OnNextFrame();
	bool OnOpenMenu();
	void DisplayOSD();

	bool StartFile(const char* path, int pov);
	bool StartBuffer(const std::vector<u8>& buf, int pov);
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
	void PrintDisconnectionSummary() const;
	void ProcessLbsMessage();
	void ProcessMcsMessage(const McsMessage& msg);
	void ApplyPatch(bool first_time);
	void RestorePatch();
	void RenderPauseMenu();

	bool IsCommandAllowedInDebugMode(int cmd) const;
	void AddCtrlCommand(int cmd, int arg1 = 0, int arg2 = 0);
	void StartSaveState();
	void EndSaveState();

	struct ReplayCtrlCommand {
		enum Command {
			None,
			// System used
			SaveFirstFrame,
			SendStartMsg,
			SeekToBriefing,

			// User control
			TogglePauseMenu,
			TogglePause,
			StepFrame,
			JumpToKeyMsg,
			SeekForward,
			SeekBackward,
			SetSpeed,
			NextSpeed,
			SetRound,
			NextRound,
		};

		ReplayCtrlCommand() = default;
		ReplayCtrlCommand(Command cmd) : cmd(cmd) {}
		ReplayCtrlCommand(Command cmd, int arg1) : cmd(cmd), arg1(arg1) {}
		ReplayCtrlCommand(Command cmd, int arg1, int arg2) : cmd(cmd), arg1(arg1), arg2(arg2) {}

		Command cmd = None;
		int arg1 = 0;
		int arg2 = 0;
	};

	class CommandQueue {
	   public:
		template <class... Args>
		void emplace_back(Args&&... args) {
			std::lock_guard lock(mtx_);
			cmds_.emplace_back(args...);
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
	};

	State state_ = State::None;
	CommandQueue ctrl_commands_;
	LbsMessageReader lbs_tx_reader_;
	proto::BattleLogFile log_file_;
	std::deque<u8> recv_buf_;
	int pov_ = 0;
	int key_msg_count_ = 0;
	int start_msg_count_ = 0;
	int recv_delay_ = 0;
	bool end_of_frame_ = false;
	bool seeking_ = false;
	int target_round_ = 0;
	int target_frame_ = 0;
	bool pause_menu_opend_ = false;
	bool lbs_first_skip_ = false;
	int ctrl_play_speed_ = 0;
	bool ctrl_step_frame_ = false;
	bool ctrl_pause_ = false;
	bool save_converted_log_ = false;
	bool debug_mode_ = false;  // Debug mode: no save-state, no ctrl_commands, no frame-end stop
};