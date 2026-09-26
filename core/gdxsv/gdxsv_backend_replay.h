#pragma once
#include <chrono>

#include "gdxsv_spectate_sync.h"

#include "gdxsv.pb.h"
#include "gdxsv_replay_ui.h"
#include "gdxsv_save_state.h"
#include "gdxsv_spectator_downlink.h"
#include "lbs_message.h"
#include "mcs_message.h"
#include "types.h"

// LBS's lobby port, shared by TCP lobby traffic and the spectator UDP
// channel (see serveUDP in lbs.go). A normal client never needs it spelled
// out: the game itself supplies the port via gdx_rpc's SOCK_OPEN request
// (see Gdxsv::HandleRPC), and only the host comes from config. A spectator
// never runs that lobby-connect flow, so it has nothing to read it from.
constexpr int kGdxsvLbsPort = 9876;

// Mock network implementation to replay local battle log
class GdxsvBackendReplay {
   friend class GdxsvReplayInputTest;

   public:
	using State = GdxsvReplayUiState::State;

	void Reset();
	void OnMainUiLoop();
	void OnEndOfFrame();
	void OnNextFrame();
	bool OnOpenMenu();
	void DisplayOSD();

	bool StartFile(const char* path, int pov);
	bool StartBuffer(const std::vector<u8>& buf, int pov);

	// Starts Live Spectate: subscribes to LBS's spectator UDP channel,
	// bootstraps from the header it sends, and then plays back like a normal
	// buffer replay - except it holds at the live edge for more data instead
	// of stopping when it catches up.
	bool StartLive(const std::string& host, const std::string& battle_code, int pov);
	void Stop();

	// Network Backend Interface
	void Open();
	void Close();
	u32 OnSockWrite(u32 addr, u32 size);
	u32 OnSockRead(u32 addr, u32 size);
	u32 OnSockPoll();

   private:
	using UiState = GdxsvReplayUiState;
	static constexpr int ReplaySeekFrames = 60;
	void PublishUiState();
	void ProcessUiCommands();
	void UpdateReplayFlow();
	void OnNextFrameInternal();
	bool Start();
	bool IsInBriefing() const;
	bool IsInGame() const;
	void PrintDisconnectionSummary() const;
	void ProcessLbsMessage();
	void ProcessMcsMessage(const McsMessage& msg);
	void DeliverKeyMsgBatch();
	void ApplyPatch(bool first_time);
	void RestorePatch();
	void BeginSilentSeek();
	void RunSilentSeekFrame(bool skip_rendering);
	void EndSilentSeek();
	void BeginSilentSeekWithAudioReset();
	void EndSilentSeekWithAudioReset();
	void SendStartMsgs();
	void PrepareRoundStartReplayState();
	void RebuildKeyDisplay() const;
	void BeginLoadingHud();
	void BeginTakeoverCountdown(u16 target_input);
	void RenderPauseMenu(const UiState& ui);
	void RenderTakeoverCountdown(const UiState& ui, u16 current_input);
	void UpdateControlBarVisibility(const UiState& ui);

	// 4-player replay: the host publishes its playback state every frame and
	// the guests follow it with the same control commands the user would give.
	void PublishMultiPovPlayback();
	void FollowMultiPovHost();
	// Guest: blocks while more than a couple of frames ahead of the host.
	void WaitForMultiPovHost();
	void RenderControlBar(const UiState& ui);
	void RenderLoadingHud(const UiState& ui);
	void GetRoundReplayBounds(int& roundStart, int& roundEnd, int& totalRounds) const;
	void GetControlTimelineBounds(int& timelineStart, int& timelineEnd, int& totalRounds) const;
	static const char* SpeedText(int speed);

	// The UDP worker stages deltas. OnNextFrame folds them into log_file_ on
	// the emulation thread; the UI reads only the published display snapshot.
	void CheckLiveUpdate();
	bool InitialLiveCatchUpReady(std::chrono::steady_clock::duration quiet) const;

	// Steers the main loop's frame period so playback holds live_buffer_frames_
	// behind the edge. Small, continuous corrections instead of whole-frame
	// stalls - see gdxsv_frame_period_trim_us.
	void UpdateFramePacing();

	// Live Spectate buffer readout, drawn next to the FPS counter.
	void DisplayLivePacingOSD(const UiState& ui);

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
			StepFrameBackward,
			JumpToKeyMsg,
			SeekForward,
			SeekBackward,
			SetSpeed,
			NextSpeed,
			SetRound,
			NextRound,
			TakeOver,
			StartTakeover,
			RetryTakeover,
			ReturnToReplay,
			FollowLive,
			ResumePlayback,
			ExitReplay,
			TakeoverInput,
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
	GdxsvReplayUiSnapshot ui_snapshot_;
	// UI requests are applied on the emulation thread before catch-up decides
	// what to queue. ctrl_commands_ also holds internal seek/start work.
	CommandQueue ui_commands_;
	CommandQueue ctrl_commands_;
	LbsMessageReader lbs_tx_reader_;
	// Emulation-thread owned after Start; initialization/reset require a
	// stopped emulator. No UI function may read the protobuf directly.
	proto::BattleLogFile log_file_;
	// Written on the emulation thread, read on the UI thread after emu.stop().
	std::string replay_error_;
	std::deque<u8> recv_buf_;
	int pov_ = 0;
	int key_msg_count_ = 0;
	int start_msg_count_ = 0;
	int briefing_start_frame_ = 0;
	int briefing_start_frame_round_ = 0;
	int recv_delay_ = 0;
	bool end_of_frame_ = false;
	bool seeking_ = false;
	int target_round_ = 0;
	int target_frame_ = 0;
	bool pause_menu_opend_ = false;
	int ctrl_play_speed_ = 0;
	bool ctrl_step_frame_ = false;
	bool ctrl_pause_ = false;
	bool ctrl_loading_ = false;
	int ctrl_loading_wait_frames_ = 0;
	bool save_converted_log_ = false;
	uint64_t timeline_revision_ = 0;
	int audio_fade_frames_ = 0;

	// UI-thread owned presentation/input state, reset before playback starts.
	bool lbs_first_skip_ = false;
	uint64_t displayed_timeline_revision_ = 0;
	float ctrl_bar_visibility_ = 0.0f;
	float ctrl_bar_idle_timer_ = 0.0f;
	u32 ctrl_bar_prev_kcode_ = ~0u;
	float step_hold_timer_ = 0.0f;
	float flash_left_ = 0.0f;
	float flash_right_ = 0.0f;
	float flash_up_ = 0.0f;
	float flash_down_ = 0.0f;
	float ctrl_bar_prev_mouse_x_ = -1.0f;
	float ctrl_bar_prev_mouse_y_ = -1.0f;
	bool ctrl_bar_dragging_ = false;
	int ctrl_bar_drag_target_frame_ = -1;
	bool ctrl_input_release_pending_ = false;

	// ---- Live Spectate ----
	// Replays a match that is still being played: live_downlink_ feeds log_file_
	// as frames arrive, instead of it being read whole from a file up front.
	bool live_mode_ = false;
	// Enabled only by a live round jump, retained while that stream drains.
	bool live_counter_reconstruction_ = false;
	GdxsvSpectatorDownlink live_downlink_;

	// True while playback is far enough behind live to warrant a skip-render
	// seek. Measured from the gap each frame, not latched.
	bool live_catching_up_ = true;

	// Whether playback is chasing the live edge. Cleared when the viewer moves
	// somewhere deliberately, so the catch-up does not drag them straight back;
	// the Live button sets it again. Same idea as YouTube's live indicator.
	bool live_following_ = true;

	// The Live pill is active only while following and near the live edge.
	// A deliberate pause/seek leaves it clickable until FollowLive is requested.
	bool live_at_edge_ = false;

	// UI-thread viewer count, refreshed by gdxsv_live_viewer_count.
	int live_viewers_ = 0;

	// Set when the control bar appears, so the count is fetched fresh for the
	// few seconds it is on screen rather than shown stale from last time.
	bool live_viewers_stale_ = true;
	bool ctrl_bar_was_visible_ = false;

	// True while the initial jump to the live match's current round is still
	// in flight (queued SeekToBriefing -> SetRound). Live catch-up must not
	// run during that window - see the catch-up gate in OnNextFrame.
	bool live_round_jump_pending_ = false;

	// Armed only by StartLive. The first catch-up keeps waiting for downloaded
	// inputs instead of ending at each temporary local edge. Completion or a
	// manual playback action clears it; later Live clicks never rearm it.
	bool live_initial_catchup_ = false;
	bool live_initial_backlog_ = true;

	// How far behind the newest available frame playback aims to sit, in
	// frames. Loaded from gdxsv:LiveBufferFrames, default kLiveDefaultBuffer.
	// Sized purely for smoothness: frames arrive in clumps, so 1 leaves no
	// cushion and stutters on every gap.
	int live_buffer_frames_ = 30;

	// Frame-period pacing, which holds the buffer at live_buffer_frames_.
	int pacing_last_recv_ = -1;
	int pacing_stall_frames_ = 0;
	double pacing_rate_hz_ = 59.94;
	int pacing_rate_recv_ = 0;
	std::chrono::steady_clock::time_point pacing_rate_time_{};
	double pacing_integral_ = 0.0;

	// Intake cap, which keeps consumption on the frame clock.
	u32 deliver_last_mainui_ = 0xffffffffu;
	int deliver_this_frame_ = 0;

	// Local multi-instance sync, which holds several spectators on one machine
	// to the same frame. Inactive unless gdxsv:SpectateSyncGroup is set.
	GdxsvSpectateSync spectate_sync_;
	int sync_subframe_ = 0;
	int sync_max_wait_ms_ = 2;

	// ---- 4-player replay ----
	bool multi_pov_host_ = false;
	bool multi_pov_guest_ = false;
	bool MultiPov() const { return multi_pov_host_ || multi_pov_guest_; }

	// Host: the position last published; a jump from it becomes a seek the
	// guests follow. Guest: the last seek generation applied.
	int multi_pov_published_frame_ = -1;
	uint32_t multi_pov_seek_generation_ = 0;
	// Host: the replay is moving the position itself (round change, briefing
	// skip), which every screen does on its own, so it is not a seek.
	bool multi_pov_system_move_ = false;
	// Host: a SetRound landed and is published as a round jump, which a guest
	// replays as SetRound: a seek cannot cross a round boundary.
	bool multi_pov_round_jump_ = false;
	int32_t multi_pov_seek_round_ = 0;

	// The start barrier is armed at the first StartMsg and taken at the next
	// frame boundary outside a seek.
	bool multi_pov_start_barrier_pending_ = false;
	bool multi_pov_start_barrier_done_ = false;

	bool takeover_ = false;
	int takeover_saved_frame_ = -1;
	int takeover_countdown_ = 0;
	u16 takeover_target_input_ = 0;
	std::deque<u16> takeover_input_buf_;
};
