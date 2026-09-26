#include "gdxsv/gdxsv_backend_replay.h"

#include "cfg/option.h"
#include "emulator.h"
#include "gdxsv/gdxsv_emu_hooks.h"
#include "gdxsv/libs.h"
#include "gtest/gtest.h"
#include "imgui.h"
#include "input/gamepad_device.h"

class GdxsvReplayInputTest : public ::testing::Test {
 protected:
	using Command = GdxsvBackendReplay::ReplayCtrlCommand;

	void SetUp() override {
		for (int player = 0; player < 4; ++player)
			replay_.log_file_.add_users();
		replay_.state_ = GdxsvBackendReplay::State::McsInBattle;
		replay_.live_mode_ = true;
		// Exercise input delivery without rendering or starting an emulator.
		replay_.seeking_ = true;
	}

	void TearDown() override {
		config::ThreadedRendering = threaded_rendering_;
#ifdef _WIN32
		config::JoystickPolling = joystick_polling_;
#endif
		config::GdxMinDelay = input_delay_;
		settings.gdxsv.replayModeActive = replay_mode_active_;
		settings.aica.muteAudio = mute_audio_;
		settings.aica.audioFade = audio_fade_;
		gdxsv_frame_period_trim_us = frame_period_trim_;
	}

	void AddInput(u64 input) { replay_.log_file_.add_inputs(input); }
	void SetOffline() { replay_.live_mode_ = false; }
	int InputIndex() const { return replay_.key_msg_count_; }
	std::deque<u8>& Reply() { return replay_.recv_buf_; }
	void DeliverDirectly() { replay_.DeliverKeyMsgBatch(); }
	void SetPlaybackFrame(int frame) { replay_.key_msg_count_ = frame; }
	void SetPlaybackSpeed(int speed) { replay_.ctrl_play_speed_ = speed; }
	void SetPaused(bool paused) { replay_.ctrl_pause_ = paused; }
	void SetFollowing(bool following) { replay_.live_following_ = following; }
	void SetRoundCount(int round) { replay_.start_msg_count_ = round; }
	void SetLiveBuffer(int frames) { replay_.live_buffer_frames_ = frames; }
	void SetDownloadedInputs(int count) { replay_.log_file_.mutable_inputs()->Resize(count, FirstInput); }
	void PrepareInitialLiveCatchUp(bool backlog = true) {
		replay_.live_initial_catchup_ = true;
		replay_.live_initial_backlog_ = backlog;
	}
	bool IsInitialLiveCatchUp() const { return replay_.live_initial_catchup_; }
	bool InitialLiveCatchUpReady(int quiet_ms) const {
		return replay_.InitialLiveCatchUpReady(std::chrono::milliseconds(quiet_ms));
	}
	Command PendingControlCommand() {
		Command cmd;
		replay_.ctrl_commands_.try_get_front(cmd);
		return cmd;
	}
	void BeginLoading() { replay_.BeginLoadingHud(); }
	bool IsControlLoading() const { return replay_.ctrl_loading_; }
	GdxsvReplayUiState PublishedUi() {
		replay_.PublishUiState();
		return replay_.ui_snapshot_.Read();
	}
	void SetSteadyPlayback() {
		replay_.seeking_ = false;
		replay_.live_catching_up_ = false;
	}
	void NewDeliveryFrame() { replay_.deliver_last_mainui_ = 0xffffffffu; }
	bool IsFollowing() const { return replay_.live_following_; }
	bool IsAtLiveEdge() const { return replay_.live_at_edge_; }
	bool IsCatchingUp() const { return replay_.live_catching_up_; }
	bool IsRoundJumpPending() const { return replay_.live_round_jump_pending_; }
	bool IsPaused() const { return replay_.ctrl_pause_; }
	int PlaybackSpeed() const { return replay_.ctrl_play_speed_; }
	const char* DisplayedSpeed() { return GdxsvBackendReplay::SpeedText(PublishedUi().playSpeed); }
	void UpdatePacing() { replay_.UpdateFramePacing(); }
	void QueueUi(Command::Command cmd, int arg = 0) {
		replay_.ui_commands_.emplace_back(cmd, arg);
		replay_.ProcessUiCommands();
	}
	void RunMainUiInput(u32 buttons) {
		mapleInputState[0].kcode = ~buttons;
		replay_.PublishUiState();
		replay_.OnMainUiLoop();
		replay_.ProcessUiCommands();
	}
	void PrepareAutoRecovery() {
		replay_.ctrl_commands_.clear();
		replay_.live_following_ = true;
		replay_.live_catching_up_ = true;
		replay_.live_at_edge_ = true;
		replay_.ctrl_play_speed_ = 2;
		replay_.ctrl_loading_ = true;
		gdxsv_frame_period_trim_us = -4000;
	}
	void PrepareFrameControl() {
		ASSERT_TRUE(addrspace::reserve());
		emu.init();
		mem_map_default();
		emu.dc_reset(true);
		gdxsv_save_state.Reset();
		gdxsv_WriteMem8(0x0c3d16d4, 2);
		gdxsv_WriteMem8(0x0c3d16d5, 7);
		for (int i = 0; i < 40; ++i)
			AddInput(FirstInput);
		SetPlaybackFrame(10); // Below the automatic save interval.
		SetSteadyPlayback();
		replay_.start_msg_count_ = 1;
	}
	void RunFrameControl() {
		replay_.end_of_frame_ = true;
		replay_.OnNextFrameInternal();
	}
	void QueueAutoSeek(bool initial = false) { replay_.ctrl_commands_.emplace_back(Command::SeekForward, 1000, initial ? 2 : 1); }

	void EnableTakeover(u16 input) {
		replay_.takeover_ = true;
		replay_.pov_ = 2;
		replay_.takeover_input_buf_.push_back(input);
		// Supply one delayed controller sample without polling host devices.
		config::GdxMinDelay = 1;
		config::ThreadedRendering = true;
#ifdef _WIN32
		config::JoystickPolling = false;
#endif
	}

	void PrepareTakeoverCountdown(bool retry, u16 target) {
		SetOffline();
		// Model the states left by TakeOver and RetryTakeover without loading
		// emulator save states. Retry deliberately keeps takeover_ true.
		replay_.takeover_ = retry;
		replay_.pause_menu_opend_ = true;
		replay_.BeginTakeoverCountdown(target);
		replay_.ctrl_commands_.clear();
	}

	void SendTakeoverInput(u16 input) {
		replay_.ui_commands_.emplace_back(GdxsvBackendReplay::ReplayCtrlCommand::TakeoverInput, input);
		replay_.ProcessUiCommands();
	}

	void ResetReplay() { replay_.Reset(); }
	void SetReplayState(GdxsvBackendReplay::State state) { replay_.state_ = state; }
	void SetLive() { replay_.live_mode_ = true; }
	void CloseTakeoverMenu() { replay_.pause_menu_opend_ = false; }
	void RenderTakeoverMenu(u32 buttons) {
		kcode[0] = ~buttons;
		ImGui::NewFrame();
		replay_.RenderPauseMenu(PublishedUi());
		ImGui::EndFrame();
		replay_.ProcessUiCommands();
	}
	bool IsTakingOver() const { return replay_.takeover_; }
	u16 TakeoverTargetInput() const { return replay_.takeover_target_input_; }
	int TakeoverCountdown() const { return replay_.takeover_countdown_; }
	const std::deque<u16>& TakeoverInputs() const { return replay_.takeover_input_buf_; }
	size_t PendingControlCommands() { return replay_.ctrl_commands_.size(); }
	bool StartTakeoverQueued() {
		return replay_.ctrl_commands_.contains(GdxsvBackendReplay::ReplayCtrlCommand::StartTakeover);
	}

	void Send(McsMessage::MsgType type) {
		replay_.ProcessMcsMessage(McsMessage::Create(type, 0));
	}

	void CloseStream() {
		replay_.log_file_.set_close_reason("battle ended");
		replay_.CheckLiveUpdate();
		ASSERT_FALSE(replay_.live_mode_);
	}

	void ExpectBatch(u64 input) {
		auto bytes = Reply();
		ASSERT_EQ(40u, bytes.size());
		for (int player = 0; player < 4; ++player) {
			McsMessage msg;
			ASSERT_EQ(10, msg.Deserialize(bytes));
			ASSERT_EQ(McsMessage::KeyMsg1, msg.Type());
			EXPECT_EQ(player, msg.Sender());
			EXPECT_EQ(static_cast<u16>(input >> (player * 16)), msg.FirstInput());
			for (int byte = 0; byte < 10; ++byte)
				bytes.pop_front();
		}
		EXPECT_TRUE(bytes.empty());
	}

	static constexpr u64 FirstInput = 0x0008000400020001ULL;
	static constexpr u64 SecondInput = 0x0080004000200010ULL;

 private:
	GdxsvBackendReplay replay_;
	const bool threaded_rendering_ = config::ThreadedRendering;
#ifdef _WIN32
	const bool joystick_polling_ = config::JoystickPolling;
#endif
	const int input_delay_ = config::GdxMinDelay;
	const bool replay_mode_active_ = settings.gdxsv.replayModeActive;
	const bool mute_audio_ = settings.aica.muteAudio;
	const float audio_fade_ = settings.aica.audioFade;
	const int frame_period_trim_ = gdxsv_frame_period_trim_us;
};

TEST_F(GdxsvReplayInputTest, ManualActionsDetachAndCancelAutomaticSpeed) {
	PrepareFrameControl();
	for (auto cmd : {Command::SeekBackward, Command::StepFrameBackward, Command::JumpToKeyMsg,
		Command::SetRound, Command::NextRound, Command::TogglePause, Command::TogglePauseMenu,
		Command::SetSpeed, Command::NextSpeed}) {
		SCOPED_TRACE(cmd);
		PrepareAutoRecovery();
		QueueUi(cmd, 1);
		EXPECT_FALSE(IsFollowing());
		EXPECT_FALSE(IsCatchingUp());
		EXPECT_FALSE(IsAtLiveEdge());
		EXPECT_EQ(0, PlaybackSpeed());
		UpdatePacing();
		EXPECT_EQ(0, gdxsv_frame_period_trim_us.load());
	}
}

TEST_F(GdxsvReplayInputTest, RewindingPreservesManualSpeedWithoutLivePacing) {
	SetFollowing(false);
	for (int speed : {-2, -1, 0, 1, 2}) {
		SetPlaybackSpeed(speed);
		QueueUi(Command::SeekBackward);
		EXPECT_EQ(speed, PlaybackSpeed());
		gdxsv_frame_period_trim_us = -4000;
		UpdatePacing();
		EXPECT_EQ(0, gdxsv_frame_period_trim_us.load());
	}
}

TEST_F(GdxsvReplayInputTest, RewoundPlaybackDeliversBufferedInputsWithoutWaitingForRendering) {
	for (int i = 0; i < 12000; ++i)
		AddInput(i % 2 == 0 ? FirstInput : SecondInput);
	SetSteadyPlayback();
	for (auto cmd : {Command::SeekBackward, Command::JumpToKeyMsg}) {
		SCOPED_TRACE(cmd);
		SetFollowing(true);
		SetPlaybackFrame(9000);
		QueueUi(cmd, 9000);
		ASSERT_FALSE(IsFollowing());
		ASSERT_FALSE(IsCatchingUp());
		// The game may request another batch before the host renders again.
		// Once the previous reply drains, buffered inputs must remain usable.
		for (int i = 0; i < 3; ++i) {
			Send(i % 2 == 0 ? McsMessage::KeyMsg1 : McsMessage::ForceMsg);
			EXPECT_EQ(9001 + i, InputIndex());
			ExpectBatch(i % 2 == 0 ? FirstInput : SecondInput);
			Reply().clear();
		}
		EXPECT_FALSE(IsFollowing());
		EXPECT_EQ(0, PlaybackSpeed());
		UpdatePacing();
		EXPECT_EQ(0, gdxsv_frame_period_trim_us.load());
	}
}

TEST_F(GdxsvReplayInputTest, DetachedPlaybackStillChecksPendingRepliesAndInputAvailability) {
	SetSteadyPlayback();
	SetFollowing(false);
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(0, InputIndex());
	EXPECT_TRUE(Reply().empty());

	AddInput(FirstInput);
	AddInput(SecondInput);
	Send(McsMessage::KeyMsg1);
	ExpectBatch(FirstInput);
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(1, InputIndex());
	ExpectBatch(FirstInput);
	Reply().pop_front();
	const auto partial = Reply();
	Send(McsMessage::KeyMsg1);
	EXPECT_EQ(1, InputIndex());
	EXPECT_EQ(partial, Reply());

	Reply().clear();
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(2, InputIndex());
	ExpectBatch(SecondInput);
	Reply().clear();
	Send(McsMessage::KeyMsg1);
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(2, InputIndex());
	EXPECT_TRUE(Reply().empty());

	AddInput(FirstInput);
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(3, InputIndex());
	ExpectBatch(FirstInput);
	EXPECT_FALSE(IsFollowing());
}

TEST_F(GdxsvReplayInputTest, ResumingLiveRestoresTheInputRecoveryCap) {
	for (int i = 0; i < 128; ++i)
		AddInput(FirstInput);
	SetSteadyPlayback();
	SetFollowing(false);
	QueueUi(Command::FollowLive);
	ASSERT_TRUE(IsFollowing());
	NewDeliveryFrame();
	DeliverDirectly();
	Reply().clear();
	DeliverDirectly();
	EXPECT_EQ(2, InputIndex());
	Reply().clear();
	DeliverDirectly();
	EXPECT_EQ(2, InputIndex());
	EXPECT_TRUE(Reply().empty());

	// Close to the live edge, the normal one-input allowance still applies.
	SetPlaybackFrame(100);
	NewDeliveryFrame();
	DeliverDirectly();
	EXPECT_EQ(101, InputIndex());
	Reply().clear();
	DeliverDirectly();
	EXPECT_EQ(101, InputIndex());
	EXPECT_TRUE(Reply().empty());
}

TEST_F(GdxsvReplayInputTest, ReachingTheEdgeDoesNotResumeFollowing) {
	PrepareFrameControl();
	SetFollowing(false);
	for (int frame : {10, 30, 40}) {
		SetPlaybackFrame(frame);
		RunFrameControl();
		EXPECT_FALSE(IsFollowing());
		EXPECT_FALSE(IsAtLiveEdge());
		EXPECT_EQ(0, gdxsv_frame_period_trim_us.load());
	}
	SetPaused(true);
	SetPlaybackSpeed(-1);
	QueueUi(Command::FollowLive);
	EXPECT_TRUE(IsFollowing());
	EXPECT_FALSE(IsPaused());
	EXPECT_EQ(0, PlaybackSpeed());
	RunFrameControl();
	EXPECT_TRUE(IsAtLiveEdge());
}

TEST_F(GdxsvReplayInputTest, BootstrapAPressesDoNotCancelInitialLiveCatchUp) {
	PrepareFrameControl();
	const auto previous_context = ImGui::GetCurrentContext();
	const auto context = ImGui::CreateContext();
	const auto previous_input = mapleInputState[0];
	const u32 previous_kcode = kcode[0];
	mapleInputState[0] = {};
	gdxsv_WriteMem8(0x0c3d16d5, 3); // MS selection, not the pausable battle scene.
	for (auto state : {GdxsvBackendReplay::State::LbsStartBattleFlow,
		GdxsvBackendReplay::State::McsWaitJoin, GdxsvBackendReplay::State::McsSessionExchange,
		GdxsvBackendReplay::State::McsInBattle}) {
		SCOPED_TRACE(static_cast<int>(state));
		SetReplayState(state);
		SetFollowing(true);
		PrepareInitialLiveCatchUp();
		// Model the automatic lobby A pulses, including a sampled press still
		// present after the network state advances to MS selection.
		RunMainUiInput(0);
		RunMainUiInput(DC_BTN_A);
		EXPECT_TRUE(IsFollowing());
		EXPECT_TRUE(IsInitialLiveCatchUp());
	}

	// A real pause in battle must still cancel the startup catch-up.
	gdxsv_WriteMem8(0x0c3d16d5, 7);
	SetFollowing(true);
	PrepareInitialLiveCatchUp();
	RunMainUiInput(0);
	RunMainUiInput(DC_BTN_A);
	EXPECT_FALSE(IsFollowing());
	EXPECT_FALSE(IsInitialLiveCatchUp());
	mapleInputState[0] = previous_input;
	kcode[0] = previous_kcode;
	ImGui::DestroyContext(context);
	ImGui::SetCurrentContext(previous_context);
}

TEST_F(GdxsvReplayInputTest, InitialLiveCatchUpWaitsAtTheTemporaryDownloadedEdge) {
	PrepareFrameControl();
	PrepareInitialLiveCatchUp();
	EXPECT_FALSE(InitialLiveCatchUpReady(0));
	EXPECT_FALSE(InitialLiveCatchUpReady(999));
	EXPECT_TRUE(PublishedUi().loading);
	RunFrameControl();
	EXPECT_FALSE(IsAtLiveEdge());
	ASSERT_EQ(1u, PendingControlCommands());
	const auto cmd = PendingControlCommand();
	EXPECT_EQ(Command::SeekForward, cmd.cmd);
	EXPECT_EQ(2, cmd.arg2); // One startup seek, not ordinary gap-based recovery.
	EXPECT_EQ(1 << 20, cmd.arg1); // Not bounded by the tiny currently-downloaded gap.
}

TEST_F(GdxsvReplayInputTest, InitialLiveCatchUpNeedsBothTheDownloadAndPlaybackToCatchUp) {
	PrepareFrameControl();
	PrepareInitialLiveCatchUp(false); // A fresh short input packet reached LBS's edge.
	SetDownloadedInputs(12000);
	SetPlaybackFrame(9000);
	EXPECT_FALSE(InitialLiveCatchUpReady(0));
	EXPECT_FALSE(InitialLiveCatchUpReady(2000));
	SetPlaybackFrame(11969);
	EXPECT_FALSE(InitialLiveCatchUpReady(0));
	SetPlaybackFrame(11970);
	EXPECT_TRUE(InitialLiveCatchUpReady(0));
}

TEST_F(GdxsvReplayInputTest, InitialLiveCatchUpDoesNotStartBeforeTheRoundIsDownloaded) {
	PrepareFrameControl();
	PrepareInitialLiveCatchUp(false);
	SetDownloadedInputs(InputIndex() - 1);
	RunFrameControl();
	EXPECT_FALSE(InitialLiveCatchUpReady(2000));
	EXPECT_TRUE(IsInitialLiveCatchUp());
	EXPECT_FALSE(IsCatchingUp());
	EXPECT_EQ(0u, PendingControlCommands());
	EXPECT_TRUE(PublishedUi().loading);
}

TEST_F(GdxsvReplayInputTest, InitialLiveCatchUpHasABoundedQuietNetworkFallback) {
	PrepareFrameControl();
	PrepareInitialLiveCatchUp(); // An exact full final packet need not have a short tail.
	EXPECT_FALSE(InitialLiveCatchUpReady(999));
	EXPECT_TRUE(InitialLiveCatchUpReady(1000));
	SetDownloadedInputs(InputIndex() + 31);
	EXPECT_FALSE(InitialLiveCatchUpReady(2000));
}

TEST_F(GdxsvReplayInputTest, CompletedInitialLiveCatchUpDoesNotRearmForLaterLiveClicks) {
	PrepareFrameControl();
	PrepareInitialLiveCatchUp(false);
	// Allow the normal two-frame Loading HUD lead-in, then finish without
	// running SH4: playback is already exactly one buffer behind the short tail.
	RunFrameControl();
	RunFrameControl();
	RunFrameControl();
	EXPECT_FALSE(IsInitialLiveCatchUp());
	EXPECT_FALSE(IsControlLoading());
	EXPECT_FALSE(PublishedUi().loading);
	EXPECT_EQ(0u, PendingControlCommands());

	QueueUi(Command::SeekBackward);
	QueueUi(Command::FollowLive);
	EXPECT_FALSE(IsInitialLiveCatchUp());
	EXPECT_FALSE(InitialLiveCatchUpReady(2000));
	EXPECT_TRUE(IsFollowing());
}

TEST_F(GdxsvReplayInputTest, ManualPlaybackCancelsInitialCatchUpPermanently) {
	PrepareFrameControl();
	for (auto cmd : {Command::SeekBackward, Command::JumpToKeyMsg, Command::TogglePause,
		Command::TogglePauseMenu, Command::SetRound, Command::SetSpeed}) {
		SCOPED_TRACE(cmd);
		PrepareInitialLiveCatchUp();
		SetFollowing(true);
		QueueUi(cmd, 1);
		EXPECT_FALSE(IsInitialLiveCatchUp());
		QueueUi(Command::FollowLive);
		EXPECT_FALSE(IsInitialLiveCatchUp());
	}
}

TEST_F(GdxsvReplayInputTest, LaterAutomaticCatchUpKeepsTheOrdinarySeekMode) {
	PrepareFrameControl();
	SetDownloadedInputs(InputIndex() + 300);
	RunFrameControl();
	ASSERT_EQ(1u, PendingControlCommands());
	EXPECT_EQ(Command::SeekForward, PendingControlCommand().cmd);
	EXPECT_EQ(1, PendingControlCommand().arg2);
	EXPECT_FALSE(IsInitialLiveCatchUp());
}

TEST_F(GdxsvReplayInputTest, ClosingOrResettingClearsInitialLiveCatchUp) {
	PrepareInitialLiveCatchUp();
	CloseStream();
	EXPECT_FALSE(IsInitialLiveCatchUp());
	EXPECT_FALSE(InitialLiveCatchUpReady(2000));
	PrepareInitialLiveCatchUp();
	ResetReplay();
	EXPECT_FALSE(IsInitialLiveCatchUp());
}

TEST_F(GdxsvReplayInputTest, LiveIndicatorUsesSeparateEnterAndLeaveThresholds) {
	PrepareFrameControl();
	// Exercise the indicator without launching an automatic emulation seek.
	SetRoundCount(0);
	for (int buffer : {10, 30, 120}) {
		SCOPED_TRACE(buffer);
		SetLiveBuffer(buffer);
		SetFollowing(false);
		RunFrameControl();
		SetFollowing(true);
		auto check = [&](int gap, bool at_edge) {
			SCOPED_TRACE(gap);
			SetDownloadedInputs(InputIndex() + gap);
			RunFrameControl();
			EXPECT_EQ(at_edge, IsAtLiveEdge());
			EXPECT_TRUE(IsFollowing());
			EXPECT_EQ(0, PlaybackSpeed());
			EXPECT_EQ(0u, PendingControlCommands());
		};
		check(buffer + 61, false);
		check(buffer + 60, true);
		check(buffer + 61, true);
		check(buffer + 60 + 128, true); // A full input packet must not undo red.
		check(buffer + 269, true);
		check(buffer + 270, false);
		check(buffer + 269, false);
		check(buffer + 61, false);
		check(buffer + 60, true);
	}
}

TEST_F(GdxsvReplayInputTest, ClickingLiveAcceptsAShortGapImmediately) {
	PrepareFrameControl();
	SetRoundCount(0); // Test feedback without running an emulation seek.
	for (int buffer : {10, 30, 120}) {
		SCOPED_TRACE(buffer);
		SetLiveBuffer(buffer);
		for (int gap : {0, buffer + 60, buffer + 120, buffer + 269}) {
			SCOPED_TRACE(gap);
			SetFollowing(false);
			SetDownloadedInputs(InputIndex() + gap);
			RunFrameControl();
			ASSERT_FALSE(IsAtLiveEdge());
			SetPaused(true);
			SetPlaybackSpeed(-1);

			QueueUi(Command::FollowLive);
			EXPECT_TRUE(IsFollowing());
			EXPECT_FALSE(IsPaused());
			EXPECT_EQ(0, PlaybackSpeed());
			EXPECT_TRUE(PublishedUi().liveAtEdge);
			RunFrameControl();
			EXPECT_TRUE(IsAtLiveEdge());
			EXPECT_EQ(0u, PendingControlCommands());
		}
	}
}

TEST_F(GdxsvReplayInputTest, ClickingLiveKeepsLargeOrUndownloadedGapsGrey) {
	PrepareFrameControl();
	SetRoundCount(0);
	for (int buffer : {10, 30, 120}) {
		SCOPED_TRACE(buffer);
		SetLiveBuffer(buffer);
		for (int gap : {-1, buffer + 270, buffer + 1000}) {
			SCOPED_TRACE(gap);
			SetFollowing(false);
			SetDownloadedInputs(InputIndex() + gap);
			RunFrameControl();

			QueueUi(Command::FollowLive);
			EXPECT_TRUE(IsFollowing());
			EXPECT_FALSE(PublishedUi().liveAtEdge);
			RunFrameControl();
			EXPECT_FALSE(IsAtLiveEdge());
		}
	}
}

TEST_F(GdxsvReplayInputTest, LiveIndicatorDoesNotTreatMissingDownloadedInputsAsTheLiveEdge) {
	PrepareFrameControl();
	SetRoundCount(0);
	SetDownloadedInputs(InputIndex() - 1);
	RunFrameControl();
	EXPECT_FALSE(IsAtLiveEdge());
	SetDownloadedInputs(InputIndex() + 30);
	RunFrameControl();
	ASSERT_TRUE(IsAtLiveEdge());

	// A round jump ahead of the download must also clear an already-red pill.
	SetPlaybackFrame(100);
	RunFrameControl();
	EXPECT_FALSE(IsAtLiveEdge());
	EXPECT_TRUE(IsFollowing());
	SetDownloadedInputs(InputIndex() + 30);
	RunFrameControl();
	ASSERT_TRUE(IsAtLiveEdge());
	SetOffline();
	RunFrameControl();
	EXPECT_FALSE(IsAtLiveEdge());
}

TEST_F(GdxsvReplayInputTest, ManualControlsPreserveLoadingForQueuedRoundJump) {
	PrepareFrameControl();
	for (bool initial : {false, true}) {
		SCOPED_TRACE(initial);
		for (auto cmd : {Command::TogglePause, Command::TogglePauseMenu, Command::SetSpeed, Command::NextSpeed}) {
			SCOPED_TRACE(cmd);
			PrepareAutoRecovery();
			// SetRound uses the same pending jump and HUD as FollowLive. Keep
			// it queued here: these tests do not load or execute a guest state.
			QueueUi(Command::SetRound, 2);
			QueueUi(Command::FollowLive);
			if (initial)
				PrepareInitialLiveCatchUp();
			QueueUi(cmd, 1);
			EXPECT_FALSE(IsFollowing());
			EXPECT_FALSE(IsInitialLiveCatchUp());
			ASSERT_TRUE(IsRoundJumpPending());
			ASSERT_TRUE(IsControlLoading());
			EXPECT_TRUE(PublishedUi().loading);
			for (int frame = 0; frame < 2; ++frame) {
				RunFrameControl();
				EXPECT_TRUE(IsRoundJumpPending());
				EXPECT_TRUE(IsControlLoading());
				ASSERT_EQ(2u, PendingControlCommands());
				EXPECT_EQ(Command::SetRound, PendingControlCommand().cmd);
			}
		}
	}
}

TEST_F(GdxsvReplayInputTest, CancellingAutomaticSeekPreservesLoadingForQueuedTimelineJump) {
	PrepareFrameControl();
	PrepareAutoRecovery();
	QueueAutoSeek();
	QueueUi(Command::JumpToKeyMsg, InputIndex());
	QueueUi(Command::TogglePause);
	for (int frame = 0; frame < 2; ++frame) {
		RunFrameControl();
		EXPECT_TRUE(IsControlLoading());
		EXPECT_TRUE(PublishedUi().loading);
		EXPECT_FALSE(IsPaused());
		ASSERT_EQ(2u, PendingControlCommands());
		EXPECT_EQ(Command::JumpToKeyMsg, PendingControlCommand().cmd);
	}
	// The no-distance jump completes after its HUD lead-in, then pause applies.
	RunFrameControl();
	EXPECT_TRUE(IsPaused());
	EXPECT_FALSE(IsControlLoading());
	EXPECT_FALSE(PublishedUi().loading);
	EXPECT_EQ(0u, PendingControlCommands());
}

TEST_F(GdxsvReplayInputTest, PausingCancelsAnAlreadyQueuedAutomaticSeek) {
	PrepareFrameControl();
	for (bool initial : {false, true}) {
		SCOPED_TRACE(initial);
		PrepareAutoRecovery();
		SetPaused(false);
		if (initial)
			PrepareInitialLiveCatchUp();
		QueueAutoSeek(initial);
		QueueUi(Command::TogglePause);
		RunFrameControl();
		EXPECT_FALSE(IsFollowing());
		EXPECT_FALSE(IsInitialLiveCatchUp());
		EXPECT_TRUE(IsPaused());
		EXPECT_FALSE(IsControlLoading());
		EXPECT_FALSE(PublishedUi().loading);
		EXPECT_EQ(10, InputIndex());
		EXPECT_EQ(0u, PendingControlCommands());
	}
}

TEST_F(GdxsvReplayInputTest, LiveFollowingDisplaysNominalSpeedWithoutChangingRecoverySpeed) {
	PrepareFrameControl();
	for (int speed : {0, 1, 2}) {
		SetPlaybackSpeed(speed);
		EXPECT_STREQ("100%", DisplayedSpeed());
		EXPECT_EQ(speed, PlaybackSpeed());
	}
	// Manual live and offline replay speeds retain their fixed labels.
	const char* labels[] = {"33%", "50%", "100%", "200%", "300%"};
	SetFollowing(false);
	for (int speed = -2; speed <= 2; ++speed) {
		SetPlaybackSpeed(speed);
		EXPECT_STREQ(labels[speed + 2], DisplayedSpeed());
	}
	SetOffline();
	SetFollowing(true);
	for (int speed = -2; speed <= 2; ++speed) {
		SetPlaybackSpeed(speed);
		EXPECT_STREQ(labels[speed + 2], DisplayedSpeed());
	}
}

TEST_F(GdxsvReplayInputTest, LiveRoundJumpShowsLoadingUntilTheDownloadReachesTheCursor) {
	PrepareFrameControl(); // Starts with 40 downloaded inputs.
	for (int i = 40; i < 30848; ++i)
		AddInput(FirstInput);
	SetPlaybackFrame(34226);
	const auto buffering = PublishedUi();
	EXPECT_EQ(34226, buffering.playbackFrame);
	EXPECT_EQ(30848, buffering.inputCount);
	EXPECT_TRUE(buffering.loading);
	EXPECT_FALSE(IsControlLoading());
	EXPECT_EQ(0u, PendingControlCommands());

	for (int i = 30848; i < 34226; ++i)
		AddInput(FirstInput);
	// Waiting for the next live input must not flash the loading HUD.
	EXPECT_FALSE(PublishedUi().loading);
	AddInput(FirstInput);
	EXPECT_FALSE(PublishedUi().loading);
	DeliverDirectly();
	EXPECT_EQ(34227, InputIndex());
	EXPECT_FALSE(PublishedUi().loading);
	EXPECT_TRUE(buffering.loading); // Published snapshots stay independent.
}

TEST_F(GdxsvReplayInputTest, DownloadLoadingDoesNotAffectOfflineReplayOrExplicitSeekLoading) {
	PrepareFrameControl();
	SetPlaybackFrame(100);
	ASSERT_TRUE(PublishedUi().loading);
	SetOffline();
	EXPECT_FALSE(PublishedUi().loading);
	SetLive();
	SetReplayState(GdxsvBackendReplay::State::End);
	EXPECT_FALSE(PublishedUi().loading);

	SetReplayState(GdxsvBackendReplay::State::McsInBattle);
	SetPlaybackFrame(10);
	BeginLoading();
	EXPECT_TRUE(PublishedUi().loading);
	EXPECT_TRUE(IsControlLoading());
	SetOffline();
	EXPECT_TRUE(PublishedUi().loading);
}

TEST_F(GdxsvReplayInputTest, LiveStarvationDoesNotQueueNeutralInput) {
	for (int poll = 0; poll < 10; ++poll) {
		Send(McsMessage::KeyMsg1);
		Send(McsMessage::ForceMsg);
		EXPECT_EQ(0, InputIndex());
		EXPECT_TRUE(Reply().empty());
	}
}

TEST_F(GdxsvReplayInputTest, DeliveryFunctionOwnsTheLiveAvailabilityCheck) {
	DeliverDirectly();
	EXPECT_EQ(0, InputIndex());
	EXPECT_TRUE(Reply().empty());
}

TEST_F(GdxsvReplayInputTest, ForceResumesWithOneAllPlayerBatch) {
	Send(McsMessage::KeyMsg1);
	ASSERT_TRUE(Reply().empty());

	AddInput(FirstInput);
	AddInput(SecondInput);
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(1, InputIndex());
	ExpectBatch(FirstInput);

	Reply().clear();
	Send(McsMessage::KeyMsg1);
	EXPECT_EQ(2, InputIndex());
	ExpectBatch(SecondInput);

	Reply().clear();
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(2, InputIndex());
	EXPECT_TRUE(Reply().empty());
}

TEST_F(GdxsvReplayInputTest, PendingReplyBlocksBothMessageTypes) {
	AddInput(FirstInput);
	AddInput(SecondInput);
	Send(McsMessage::KeyMsg1);
	ASSERT_EQ(1, InputIndex());
	ExpectBatch(FirstInput);

	for (bool partial : {false, true}) {
		if (partial)
			Reply().pop_front();
		const auto pending = Reply();
		for (int poll = 0; poll < 10; ++poll) {
			Send(McsMessage::ForceMsg);
			Send(McsMessage::KeyMsg1);
			EXPECT_EQ(1, InputIndex());
			EXPECT_EQ(pending, Reply());
		}
	}

	Reply().clear();
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(2, InputIndex());
	ExpectBatch(SecondInput);
}

TEST_F(GdxsvReplayInputTest, ForceRecoversAfterStreamBecomesOffline) {
	Send(McsMessage::KeyMsg1);
	ASSERT_TRUE(Reply().empty());

	// Final inputs and close can arrive together while the game is stalled.
	AddInput(FirstInput);
	AddInput(SecondInput);
	CloseStream();
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(1, InputIndex());
	ExpectBatch(FirstInput);
}

TEST_F(GdxsvReplayInputTest, OfflineKeyAndForceUseTheSameDeliveryPolicy) {
	SetOffline();
	AddInput(FirstInput);
	AddInput(SecondInput);
	// Leave EOF handling to the emulator; this test only exercises delivery.
	AddInput(0);

	Send(McsMessage::KeyMsg1);
	EXPECT_EQ(1, InputIndex());
	ExpectBatch(FirstInput);
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(1, InputIndex());
	ExpectBatch(FirstInput);

	Reply().clear();
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(2, InputIndex());
	ExpectBatch(SecondInput);
}

TEST_F(GdxsvReplayInputTest, TakeoverForceUsesControllerInputOnlyForThePovPlayer) {
	SetOffline();
	AddInput(FirstInput);
	AddInput(SecondInput);
	EnableTakeover(McsKeyCode::A);
	Send(McsMessage::ForceMsg);
	EXPECT_EQ(1, InputIndex());
	const u64 expected = (FirstInput & ~(0xffffULL << 32)) | (u64(McsKeyCode::A) << 32);
	ExpectBatch(expected);

	Send(McsMessage::ForceMsg);
	Send(McsMessage::KeyMsg1);
	EXPECT_EQ(1, InputIndex());
	ExpectBatch(expected);
}

TEST_F(GdxsvReplayInputTest, TakeoverCountdownStartsImmediatelyOnFirstAndRepeatedAttempts) {
	const u16 target = McsKeyCode::A | McsKeyCode::LEFT;
	const u16 inputs[] = {0, McsKeyCode::B, McsKeyCode::A | McsKeyCode::X, target};
	for (int attempt = 0; attempt < 3; ++attempt) {
		SCOPED_TRACE(attempt);
		PrepareTakeoverCountdown(attempt > 0, target);
		EXPECT_EQ(60, TakeoverCountdown());
		EXPECT_EQ(target, TakeoverTargetInput());
		EXPECT_EQ(attempt > 0, IsTakingOver());
		EXPECT_TRUE(TakeoverInputs().empty());

		// Missing, extra and matching inputs all advance the same countdown.
		std::deque<u16> expected;
		for (int frame = 1; frame <= 60; ++frame) {
			const u16 input = inputs[(frame - 1) % 4];
			expected.push_back(input);
			SendTakeoverInput(input);
			EXPECT_EQ(60 - frame, TakeoverCountdown());
			EXPECT_EQ(expected, TakeoverInputs());
			EXPECT_EQ(frame == 60 ? 1u : 0u, PendingControlCommands());
		}
		EXPECT_TRUE(StartTakeoverQueued());

		// Late UI samples cannot add inputs or queue another start.
		SendTakeoverInput(target);
		SendTakeoverInput(0);
		EXPECT_EQ(0, TakeoverCountdown());
		EXPECT_EQ(expected, TakeoverInputs());
		EXPECT_EQ(1u, PendingControlCommands());
		EXPECT_EQ(attempt > 0, IsTakingOver());
	}
}

TEST_F(GdxsvReplayInputTest, TakeoverCountdownDoesNotRequireAnyButtons) {
	PrepareTakeoverCountdown(false, McsKeyCode::A | McsKeyCode::LEFT);
	for (int frame = 0; frame < 60; ++frame)
		SendTakeoverInput(0);
	EXPECT_EQ(0, TakeoverCountdown());
	EXPECT_EQ(std::deque<u16>(60, 0), TakeoverInputs());
	EXPECT_TRUE(StartTakeoverQueued());
}

TEST_F(GdxsvReplayInputTest, TakeoverCountdownIgnoresRecordedAndHeldStartOnFirstAndRetry) {
	for (bool retry : {false, true}) {
		for (u16 target : {u16(0), u16(McsKeyCode::A)}) {
			PrepareTakeoverCountdown(retry, target | McsKeyCode::START);
			EXPECT_EQ(target, TakeoverTargetInput());
			EXPECT_EQ(60, TakeoverCountdown());
			for (int frame = 1; frame <= 60; ++frame) {
				SendTakeoverInput(target | (frame % 2 ? McsKeyCode::START : 0));
				EXPECT_EQ(60 - frame, TakeoverCountdown());
				EXPECT_EQ(std::deque<u16>(frame, target), TakeoverInputs());
			}
			EXPECT_TRUE(StartTakeoverQueued());
			EXPECT_EQ(1u, PendingControlCommands());
		}
	}
}

TEST_F(GdxsvReplayInputTest, HeldStartDuringCountdownDoesNotRetryUntilReleased) {
	PrepareFrameControl();
	const auto previous_context = ImGui::GetCurrentContext();
	const auto context = ImGui::CreateContext();
	const auto previous_input = mapleInputState[0];
	const u32 previous_kcode = kcode[0];
	ImGui::GetIO().DisplaySize = ImVec2(640, 480);
	ImGui::GetIO().Fonts->Build();
	config::ThreadedRendering = true;
#ifdef _WIN32
	config::JoystickPolling = false;
#endif
	PrepareTakeoverCountdown(true, McsKeyCode::A);
	CloseTakeoverMenu();
	mapleInputState[0] = {};
	RunMainUiInput(0);
	RunMainUiInput(DC_BTN_START);
	PrepareTakeoverCountdown(true, McsKeyCode::A);
	RenderTakeoverMenu(DC_BTN_START);
	EXPECT_EQ(59, TakeoverCountdown());
	RenderTakeoverMenu(0);
	EXPECT_EQ(58, TakeoverCountdown());
	RenderTakeoverMenu(DC_BTN_START);
	EXPECT_EQ(57, TakeoverCountdown());
	for (int frame = 0; frame < 57; ++frame)
		SendTakeoverInput(McsKeyCode::START);
	EXPECT_TRUE(StartTakeoverQueued());
	EXPECT_EQ(std::deque<u16>(60, 0), TakeoverInputs());
	CloseTakeoverMenu();
	mapleInputState[0] = {};
	RunMainUiInput(DC_BTN_START | DC_BTN_A);
	EXPECT_EQ(1u, PendingControlCommands());
	// Only START needs releasing; gameplay buttons can remain held.
	RunMainUiInput(DC_BTN_A);
	EXPECT_EQ(1u, PendingControlCommands());
	RunMainUiInput(DC_BTN_START | DC_BTN_A);
	EXPECT_EQ(2u, PendingControlCommands());
	mapleInputState[0] = previous_input;
	kcode[0] = previous_kcode;
	ImGui::DestroyContext(context);
	ImGui::SetCurrentContext(previous_context);
}

TEST_F(GdxsvReplayInputTest, NewTakeoverAttemptRestartsCountdownAndClearsOldInputs) {
	PrepareTakeoverCountdown(false, McsKeyCode::A);
	SendTakeoverInput(McsKeyCode::B);
	ASSERT_EQ(59, TakeoverCountdown());
	ASSERT_FALSE(TakeoverInputs().empty());

	PrepareTakeoverCountdown(true, McsKeyCode::LEFT);
	EXPECT_EQ(60, TakeoverCountdown());
	EXPECT_EQ(McsKeyCode::LEFT, TakeoverTargetInput());
	EXPECT_TRUE(TakeoverInputs().empty());
	SendTakeoverInput(0);
	EXPECT_EQ(59, TakeoverCountdown());
	EXPECT_EQ(std::deque<u16>{0}, TakeoverInputs());
	EXPECT_EQ(0u, PendingControlCommands());
}

TEST_F(GdxsvReplayInputTest, TakeoverInputIsIgnoredAfterTheMenuCloses) {
	for (bool retry : {false, true}) {
		SCOPED_TRACE(retry);
		PrepareTakeoverCountdown(retry, McsKeyCode::A);
		CloseTakeoverMenu();
		SendTakeoverInput(McsKeyCode::A);
		EXPECT_EQ(60, TakeoverCountdown());
		EXPECT_TRUE(TakeoverInputs().empty());
		EXPECT_EQ(0u, PendingControlCommands());
	}
}

TEST_F(GdxsvReplayInputTest, ResetClearsTakeoverCountdown) {
	PrepareTakeoverCountdown(false, McsKeyCode::A);
	SendTakeoverInput(McsKeyCode::B);
	ResetReplay();
	SendTakeoverInput(McsKeyCode::B);
	EXPECT_EQ(0, TakeoverCountdown());
	EXPECT_EQ(0, TakeoverTargetInput());
	EXPECT_TRUE(TakeoverInputs().empty());
	EXPECT_EQ(0u, PendingControlCommands());
}

TEST_F(GdxsvReplayInputTest, TakeoverInputIsIgnoredInLiveOrInactiveReplay) {
	using State = GdxsvBackendReplay::State;
	for (State state : {State::McsInBattle, State::None, State::End}) {
		SCOPED_TRACE(static_cast<int>(state));
		PrepareTakeoverCountdown(false, McsKeyCode::A);
		SetReplayState(state);
		if (state == State::McsInBattle)
			SetLive();
		SendTakeoverInput(0);
		EXPECT_EQ(60, TakeoverCountdown());
		EXPECT_TRUE(TakeoverInputs().empty());
		EXPECT_EQ(0u, PendingControlCommands());
	}
}
