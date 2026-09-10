#include "gdxsv_backend_replay.h"

#include <nowide/cstdio.hpp>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <ctime>

#include "SDL_events.h"
#include "cfg/option.h"
#include "audio/audiostream.h"
#include "emulator.h"
#include "ui/mainui.h"
#include "gdx_rpc.h"
#include "gdxsv.h"
#include "gdxsv_emu_hooks.h"
#include "gdxsv_translation.h"
#include "gdxsv_replay_util.h"
#include "input/gamepad_device.h"
#include "libs.h"
#include "oslib/oslib.h"
#include "ui/gui.h"
#include "ui/gui_util.h"
#include "ui/settings.h"
#include "ui/IconsFontAwesome6.h"
#include "sdl/sdl.h"

using namespace std::chrono;

namespace {
std::string formatLocalClock(time_t ts) {
	char buf[16];
	const std::tm* t = std::localtime(&ts);
	if (t == nullptr || std::strftime(buf, sizeof(buf), "%H:%M", t) == 0) {
		return {};
	}
	return buf;
}

void refreshLiveInputState() {
	if (!config::ThreadedRendering) {
		os_UpdateInputState();
	} else if (config::JoystickPolling) {
		os_UpdateJoystickState();
	}
}

MapleInputState liveInputState() {
	MapleInputState input;

	input.kcode = kcode[0];
	input.halfAxes[PJTI_L] = lt[0];
	input.halfAxes[PJTI_R] = rt[0];
	input.halfAxes[PJTI_L2] = lt2[0];
	input.halfAxes[PJTI_R2] = rt2[0];
	input.fullAxes[PJAI_X1] = joyx[0];
	input.fullAxes[PJAI_Y1] = joyy[0];

	if (input.halfAxes[PJTI_L2] >= 64) input.kcode &= ~(DC_BTN_A | DC_BTN_X);
	if (input.halfAxes[PJTI_R2] >= 64) input.kcode &= ~(DC_BTN_A | DC_BTN_Y);

	return input;
}

// Maple input to MCS pad input. Keep GGPO/replay packed trigger bits, and also
// accept live trigger axes for replay takeover input.
u16 convertInput(MapleInputState input) {
	u16 r = 0;
	if (~input.kcode & DC_BTN_A) r |= McsKeyCode::A;
	if (~input.kcode & DC_BTN_B) r |= McsKeyCode::B;
	if (~input.kcode & DC_BTN_C) r |= McsKeyCode::C;
	if (~input.kcode & DC_BTN_X) r |= McsKeyCode::X;
	if (~input.kcode & DC_BTN_Y) r |= McsKeyCode::Y;
	if (~input.kcode & DC_BTN_Z) r |= McsKeyCode::Z;
	if (~input.kcode & DC_DPAD_UP) r |= McsKeyCode::UP;
	if (~input.kcode & DC_DPAD_DOWN) r |= McsKeyCode::DOWN;
	if (~input.kcode & DC_DPAD_RIGHT) r |= McsKeyCode::RIGHT;
	if (~input.kcode & DC_DPAD_LEFT) r |= McsKeyCode::LEFT;
	if (~input.kcode & DC_BTN_START) r |= McsKeyCode::START;
	if ((~input.kcode & (DC_BTN_BITMAPPED_LAST << 1)) || input.halfAxes[PJTI_L] >= 0x4000) r |= McsKeyCode::LT;
	if ((~input.kcode & (DC_BTN_BITMAPPED_LAST << 2)) || input.halfAxes[PJTI_R] >= 0x4000) r |= McsKeyCode::RT;
	if ((input.fullAxes[0] >> 8) + 128 <= 128 - 0x20) r |= McsKeyCode::LEFT;
	if ((input.fullAxes[0] >> 8) + 128 >= 128 + 0x20) r |= McsKeyCode::RIGHT;
	if ((input.fullAxes[1] >> 8) + 128 <= 128 - 0x20) r |= McsKeyCode::UP;
	if ((input.fullAxes[1] >> 8) + 128 >= 128 + 0x20) r |= McsKeyCode::DOWN;
	return r;
}

u16 liveTakeoverMcsInput() {
	refreshLiveInputState();
	return convertInput(liveInputState());
}

u16 replayMcsInput(const proto::BattleLogFile& log_file, int frame, int player) {
	if (frame < 0 || frame >= log_file.inputs_size() || player < 0 || player >= log_file.users_size()) {
		return 0;
	}
	return u16(log_file.inputs(frame) >> (player * 16));
}

std::string formatMcsInput(u16 input) {
	if (input == 0) {
		return "Neutral";
	}

	std::string text;
	auto append = [&](u16 code, const char* name) {
		if ((input & code) == 0) {
			return;
		}
		if (!text.empty()) {
			text += " + ";
		}
		text += name;
	};

	append(McsKeyCode::UP, "Up");
	append(McsKeyCode::DOWN, "Down");
	append(McsKeyCode::LEFT, "Left");
	append(McsKeyCode::RIGHT, "Right");
	append(McsKeyCode::A, "A");
	append(McsKeyCode::B, "B");
	append(McsKeyCode::X, "X");
	append(McsKeyCode::Y, "Y");
	append(McsKeyCode::LT, "LT");
	append(McsKeyCode::RT, "RT");
	append(McsKeyCode::START, "Start");

	return text;
}
}  // namespace

void GdxsvBackendReplay::Reset() {
	live_downlink_.Stop();
	live_mode_ = false;
	live_catching_up_ = true;
	live_following_ = true;
	live_at_edge_ = false;
	live_round_jump_pending_ = false;
	state_ = State::None;
	ui_commands_.clear();
	ctrl_commands_.clear();
	lbs_tx_reader_.Clear();
	log_file_.Clear();
	recv_buf_.clear();
	pov_ = 0;
	key_msg_count_ = 0;
	start_msg_count_ = 0;
	briefing_start_frame_ = 0;
	briefing_start_frame_round_ = 0;
	recv_delay_ = 0;
	end_of_frame_ = false;
	seeking_ = false;
	pause_menu_opend_ = false;
	lbs_first_skip_ = false;
	ctrl_play_speed_ = 0;
	ctrl_step_frame_ = false;
	ctrl_pause_ = false;
	ctrl_loading_ = false;
	ctrl_loading_wait_frames_ = 0;
	save_converted_log_ = false;
	timeline_revision_ = 0;
	displayed_timeline_revision_ = 0;
	ctrl_bar_visibility_ = 0.0f;
	ctrl_bar_idle_timer_ = 0.0f;
	ctrl_bar_prev_kcode_ = ~0u;
	audio_fade_frames_ = 0;
	step_hold_timer_ = 0.0f;
	flash_left_ = 0.0f;
	flash_right_ = 0.0f;
	flash_up_ = 0.0f;
	flash_down_ = 0.0f;
	ctrl_bar_prev_mouse_x_ = -1.0f;
	ctrl_bar_prev_mouse_y_ = -1.0f;
	ctrl_bar_dragging_ = false;
	ctrl_bar_drag_target_frame_ = -1;
	ctrl_input_release_pending_ = false;
	settings.gdxsv.replayModeActive = false;
	settings.aica.audioFade = 1.0f;
	takeover_ = false;
	takeover_saved_frame_ = -1;
	takeover_countdown_ = 0;
	takeover_aligning_ = false;
	takeover_skip_input_matching_ = false;
	takeover_target_input_ = 0;
	takeover_input_buf_.clear();
	gdxsv_save_state.Reset();
	gdxsv.key_display_.Clear();
	ui_snapshot_.Publish({});
}

void GdxsvBackendReplay::OnMainUiLoop() {
	const UiState ui = ui_snapshot_.Read();
	if (ui.state == State::End) {
		// A headless replay run is done the moment playback ends: exit with
		// success so a backward-compat harness sees the replay played through.
		if (gdxsv_headless()) {
			gdxsv_headless_exit(0);
		}
		// Join emulation before resetting its state or restoring the lobby.
		emu.stop();
		state_ = State::None;
		gdxsv_save_state.Reset();
		ctrl_input_release_pending_ = false;
		ui_snapshot_.Publish({});
		gdxsv.netmode_ = Gdxsv::NetMode::Offline;
		gdxsv_end_replay();
		return;
	}

	if (ui.state <= State::LbsStartBattleFlow) {
		static int counter = 0;
		if (++counter % 10 < 5)
			kcode[0] = ~DC_BTN_A;
		else
			kcode[0] = ~0u;
		if (ctrl_commands_.empty() && ui_commands_.empty() && !lbs_first_skip_) {
			ui_commands_.emplace_back(ReplayCtrlCommand::SeekForward, ui.bootSeekFrames);
			lbs_first_skip_ = true;
		}
	}

	if (State::LbsStartBattleFlow <= ui.state && !ui.pauseMenuOpen) {
		auto input = mapleInputState[0];
		// Map analog stick to d-pad (fullAxes are 16-bit, >> 8 to match convertInput thresholds)
		if ((input.fullAxes[0] >> 8) + 128 <= 128 - 0x20) input.kcode &= ~DC_DPAD_LEFT;
		if ((input.fullAxes[0] >> 8) + 128 >= 128 + 0x20) input.kcode &= ~DC_DPAD_RIGHT;
		if ((input.fullAxes[1] >> 8) + 128 <= 128 - 0x20) input.kcode &= ~DC_DPAD_UP;
		if ((input.fullAxes[1] >> 8) + 128 >= 128 + 0x20) input.kcode &= ~DC_DPAD_DOWN;

		static u32 prev_kcode = 0;
		if (prev_kcode == 0) prev_kcode = input.kcode;
		if (ctrl_input_release_pending_) {
			constexpr u32 replay_control_buttons =
				DC_BTN_A | DC_BTN_START | DC_DPAD_UP | DC_DPAD_DOWN | DC_DPAD_LEFT | DC_DPAD_RIGHT;
			step_hold_timer_ = 0.0f;
			prev_kcode = input.kcode;
			if ((~input.kcode & replay_control_buttons) == 0) {
				ctrl_input_release_pending_ = false;
			}
			gui_delayed_keys_up();
			return;
		}
		const u32 pressed = ~((input.kcode ^ prev_kcode) & ~input.kcode);

		if (input.kcode != prev_kcode) {
			if (ui.takeover) {
				if (~pressed & DC_BTN_START) {
					ui_commands_.emplace_back(ReplayCtrlCommand::RetryTakeover);
				}
			} else {
				// A: Play/Pause toggle
				if (~pressed & DC_BTN_A) {
					ui_commands_.emplace_back(ReplayCtrlCommand::TogglePause);
				}
				// Right: Step frame (paused) / Seek forward (playing)
				else if (~pressed & DC_DPAD_RIGHT) {
					if (ui.paused) {
						ui_commands_.emplace_back(ReplayCtrlCommand::StepFrame);
					} else {
						ui_commands_.emplace_back(ReplayCtrlCommand::SeekForward);
					}
					flash_right_ = 0.3f;
				}
				// Left: Step frame backward (paused) / Seek backward (playing)
				else if (~pressed & DC_DPAD_LEFT) {
					if (ui.paused) {
						ui_commands_.emplace_back(ReplayCtrlCommand::StepFrameBackward);
					} else {
						ui_commands_.emplace_back(ReplayCtrlCommand::SeekBackward);
					}
					flash_left_ = 0.3f;
				}
				// Up/Down: Speed control (ignored while Left/Right is held)
				else if ((~pressed & DC_DPAD_UP) && !(~input.kcode & DC_DPAD_LEFT) && !(~input.kcode & DC_DPAD_RIGHT)) {
					ui_commands_.emplace_back(ReplayCtrlCommand::NextSpeed, 1);
					flash_up_ = 0.3f;
				}
				else if ((~pressed & DC_DPAD_DOWN) && !(~input.kcode & DC_DPAD_LEFT) && !(~input.kcode & DC_DPAD_RIGHT)) {
					ui_commands_.emplace_back(ReplayCtrlCommand::NextSpeed, -1);
					flash_down_ = 0.3f;
				}
			}
		}
		prev_kcode = input.kcode;

		// Auto-repeat for Left/Right held
		if (!ui.takeover) {
			bool holding_lr = (~input.kcode & DC_DPAD_RIGHT) || (~input.kcode & DC_DPAD_LEFT);
			if (holding_lr) {
				step_hold_timer_ += ImGui::GetIO().DeltaTime;
				if (step_hold_timer_ >= 0.5f && ctrl_commands_.empty() && ui_commands_.empty()) {
					if (~input.kcode & DC_DPAD_RIGHT) {
						if (ui.paused) {
							ui_commands_.emplace_back(ReplayCtrlCommand::StepFrame);
						} else {
							ui_commands_.emplace_back(ReplayCtrlCommand::SeekForward);
						}
						flash_right_ = 0.3f;
					} else {
						if (ui.paused) {
							ui_commands_.emplace_back(ReplayCtrlCommand::StepFrameBackward);
						} else {
							ui_commands_.emplace_back(ReplayCtrlCommand::SeekBackward);
						}
						flash_left_ = 0.3f;
					}
				}
			} else {
				step_hold_timer_ = 0.0f;
			}
		} else {
			step_hold_timer_ = 0.0f;
		}
	}

	gui_delayed_keys_up();
}

void GdxsvBackendReplay::UpdateReplayFlow() {
	if (state_ == State::McsWaitJoin) {
		if (ctrl_commands_.empty()) {
			if (takeover_) {
				ctrl_commands_.emplace_back(ReplayCtrlCommand::RetryTakeover);
			} else if (config::GdxReplaySkipMsSelection && !live_mode_) {
				BeginLoadingHud();
				ctrl_commands_.emplace_back(ReplayCtrlCommand::SeekToBriefing);
			}
		}
	}

	if (state_ == State::McsInBattle) {
		const int disk = gdxsv.Disk();
		const int COM_R_No0 = disk == 1 ? 0x0c2f6639 : 0x0c391d79;
		if (gdxsv_ReadMem8(COM_R_No0) == 4 && (gdxsv_ReadMem8(COM_R_No0 + 5) == 3 || gdxsv_ReadMem8(COM_R_No0 + 5) == 4)) {
			if (takeover_) {
				if (ctrl_commands_.empty()) {
					ctrl_commands_.emplace_back(ReplayCtrlCommand::RetryTakeover);
				}
			} else {
				// re-battle end
				Stop();
			}
		} else if (gdxsv_ReadMem8(COM_R_No0) == 4 && gdxsv_ReadMem8(COM_R_No0 + 5) != 0) {
			if (ctrl_commands_.empty()) {
				if (takeover_) {
					ctrl_commands_.emplace_back(ReplayCtrlCommand::RetryTakeover);
				} else if (config::GdxReplaySkipMsSelection && !live_mode_) {
					BeginLoadingHud();
					ctrl_commands_.emplace_back(ReplayCtrlCommand::SeekToBriefing);
				}
			}
		}
	}
}

bool GdxsvBackendReplay::IsInBriefing() const {
	const int disk = gdxsv.Disk();
	const u32 addr1 = disk == 1 ? 0x0c336254u : 0x0c3d16d4u;
	const u32 addr2 = disk == 1 ? 0x0c336255u : 0x0c3d16d5u;
	return gdxsv_ReadMem8(addr1) == 2 && gdxsv_ReadMem8(addr2) == 5;
}

bool GdxsvBackendReplay::IsInGame() const {
	const int disk = gdxsv.Disk();
	const u32 addr1 = disk == 1 ? 0x0c336254u : 0x0c3d16d4u;
	const u32 addr2 = disk == 1 ? 0x0c336255u : 0x0c3d16d5u;
	return gdxsv_ReadMem8(addr1) == 2 && gdxsv_ReadMem8(addr2) == 7;
}

void GdxsvBackendReplay::OnEndOfFrame() {
	end_of_frame_ = true;
	emu.getSh4Executor()->Stop();
}

void GdxsvBackendReplay::BeginSilentSeek() {
	settings.aica.muteAudio = true;
	rend_enable_renderer(false);
	seeking_ = true;
	// A burst right after normal playback inherits a stale end_of_frame_=true
	// from the frame that just finished, because RunSilentSeekFrame only
	// clears it after its own emu.run() returns. Without this the burst's
	// first emu.run() sees "frame already done" and key_msg_count_ silently
	// fails to advance.
	end_of_frame_ = false;
}

void GdxsvBackendReplay::RunSilentSeekFrame(bool skip_rendering) {
	settings.gdxsv.skipRenderingAddr = skip_rendering ? settings.gdxsv.skipRenderingBaseAddr : 0;
	emu.run();
	end_of_frame_ = false;
}

void GdxsvBackendReplay::EndSilentSeek() {
	settings.aica.muteAudio = false;
	settings.gdxsv.skipRenderingAddr = 0;
	rend_enable_renderer(true);
	seeking_ = false;
}

void GdxsvBackendReplay::BeginSilentSeekWithAudioReset() {
	FlushAudio();
	TermAudio();
	BeginSilentSeek();
}

void GdxsvBackendReplay::EndSilentSeekWithAudioReset() {
	EndSilentSeek();
	InitAudio();
}

void GdxsvBackendReplay::SendStartMsgs() {
	for (int i = 0; i < log_file_.users_size(); ++i) {
		if (i != pov_) {
			auto start_msg = McsMessage::Create(McsMessage::MsgType::StartMsg, i);
			std::copy(start_msg.body.begin(), start_msg.body.end(), std::back_inserter(recv_buf_));
		}
	}
	gdxsv.maxlag_ = 1;
}

void GdxsvBackendReplay::PrepareRoundStartReplayState() {
	gdxsv.key_display_.Clear();
	SendStartMsgs();
	EventManager::event(Event::GGPOGameEnd);
}

void GdxsvBackendReplay::RebuildKeyDisplay() const {
	gdxsv.key_display_.Clear();
	const int lookback = std::min(key_msg_count_, 256);
	const int start = key_msg_count_ - lookback;
	for (int t = start; t < key_msg_count_; t++) {
		if (t < log_file_.inputs_size()) {
			const u64 inputs = log_file_.inputs(t);
			for (int i = 0; i < log_file_.users_size(); i++) {
				gdxsv.key_display_.AppendInput(i, u16(inputs >> (i * 16)));
			}
		}
	}
}

void GdxsvBackendReplay::BeginLoadingHud() {
	ctrl_loading_ = true;
	ctrl_loading_wait_frames_ = 0;
}

// Restore disc-2 MS-selection roles after loading another round's savestate.
// start_msg_randoms contains the post-draw RNG state; its high byte is the
// random value used by 0x0c04816c to choose the map selector.
static void gdxsv_patch_map_selector(u16 round_seed, int player_count) {
	if (gdxsv.Disk() != 2)
		return;
	if (gdxsv_ReadMem8(0x0c3913c7) == 0) // stage_flag 0: side7, no map selection
		return;
	constexpr u32 kSelWork = 0x0c3d0724u;
	constexpr u32 kSelMask = kSelWork + 0x1c;	// one-hot map-selector mask
	constexpr u32 kSelRoles = kSelWork + 0x10;	// per-player role, 0 = map selector

	const int selector = static_cast<u8>(round_seed >> 8) % player_count;
	const u8 mask = static_cast<u8>(1u << selector);
	gdxsv_WriteMem8(kSelMask, mask);
	for (int p = 0; p < 4; ++p) {
		gdxsv_WriteMem8(kSelRoles + p, (mask & (1u << p)) ? 0 : 1);
	}
	NOTICE_LOG(COMMON, "map selector recomputed: seed=%04x players=%d -> index=%d mask=%02x", round_seed, player_count,
			   selector, mask);
}

// Playback pacing. The match itself runs at 59.94Hz, so replay and Live
// Spectate lock to the same period and correct the residual by trimming it
// rather than by stalling whole frames: a stall is a visible 16.7ms step,
// while a percent of trim is not. See gdxsv_frame_period_trim_us.
namespace {
constexpr int kLiveDefaultBuffer = 30;	  // ~0.5s at 60fps
constexpr int kLiveMinBuffer = 10;		  // below this the buffer is thinner than one arrival clump
constexpr int kLiveMaxBuffer = 3600;	  // 60s

// Corrections smaller than this are chasing arrival noise. Scales with the
// buffer, so a small buffer still gets a proportionate deadband.
constexpr int kPacingMaxDeadbandFrames = 2;
constexpr int kPacingTrimUsPerFrame = 40;

// Integral gain. Proportional control alone settles at a standing offset,
// because its output is zero exactly at zero error; the integrator nulls it.
constexpr double kPacingIntegralGain = 0.25;
constexpr double kPacingIntegralLimit = 600.0;	 // us; conditional integration does the real anti-windup

// Overall trim bounds, wide enough for the feedforward term. The live match
// runs well below nominal because the players carry GGPO's rollback cost while
// a spectator only replays inputs, so the standing correction is large.
constexpr double kPacingTrimFloorUs = -4000.0;
constexpr double kPacingTrimCeilUs = 8000.0;

// Source-rate estimate: window, smoothing, and a sane band.
constexpr double kPacingRateWindowSec = 1.0;
constexpr double kPacingRateAlpha = 0.25;
constexpr double kPacingRateMinHz = 30.0;
constexpr double kPacingRateMaxHz = 65.0;
constexpr double kPacingNominalPeriodUs = 16683.0;	// what get_period returns for 59.94Hz

// Unpaces the loop while catching up. get_trimmed_period floors the period at
// 1000us, so any large negative trim lets the main loop run flat out.
constexpr int kPacingCatchUpTrimUs = -100000;

// How many frames without a new input before we treat production as stalled.
// A frame or two of no arrival is ordinary jitter at 59.94Hz; a scene
// transition is hundreds.
constexpr int kPacingStallFrames = 5;

// How far past the target playback must fall before it is allowed a second
// input frame per rendered frame to claw back.
constexpr int kPacingRecoverMargin = 5;

// Longest a sync may stall the emulation thread, per frame. Not a
// timeout on an operation that has to finish: healthy peers meet in
// microseconds, and this only bounds the case that cannot succeed at all - a
// peer stalled on data, or gone. It spends the 16.7ms frame budget, so keep it
// small. Giving up early costs a frame of sync and nobody sees it; waiting too
// long drops the frame rate and everybody hears it.
constexpr int kSyncMaxWaitPerFrameMs = 2;

// Sync position = key_msg_count_ * kSyncSubFrames + frames run since that input.
//
// key_msg_count_ is comparable across instances but freezes outside battle,
// leaving nothing to sync on through the scenes where drift is most
// visible. The sub-frame term keeps the position advancing there while staying
// a function of game state rather than execution history.
// Lengthen the frame period when this instance is ahead of the group, instead
// of only spinning inside the frame.
//
// Spinning cannot slow a paced loop that has headroom: replay runs at a fixed
// 59.94Hz, so a frame that finishes early sleeps out the rest of its 16.7ms,
// and a wait inside the frame just eats idle time the instance was going to
// spend sleeping anyway. Measured, that left a 12 frame offset closing at
// 0.04 frames a second. Stretching the period is a real slowdown.
//
// Per frame of lead, so a small lead is corrected gently. At 200us a 12 frame
// lead runs the period ~14% long and closes in about 1.5s.
constexpr int kSyncTrimUsPerFrame = 200;
constexpr int kSyncTrimMaxUs = 4000;

// How far past the buffer target still counts as live, in frames.
constexpr int kLiveAtEdgeMargin = 60;

constexpr int kSyncSubFrames = 1024;  // ~17s of stall before it could overflow

}  // namespace


void GdxsvBackendReplay::UpdateFramePacing() {
	if (!live_mode_ || takeover_ || ctrl_pause_ || pause_menu_opend_) {
		gdxsv_frame_period_trim_us = 0;
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	const int recv = log_file_.inputs_size();
	if (pacing_rate_time_ == std::chrono::steady_clock::time_point{}) {
		pacing_rate_time_ = now;
		pacing_rate_recv_ = recv;
	}

	// Still closing a gap: run flat out rather than at playback speed. Hold the
	// integrator and restart the rate window so neither carries catch-up into
	// steady state.
	if (0 < ctrl_play_speed_ || live_catching_up_) {
		gdxsv_frame_period_trim_us = kPacingCatchUpTrimUs;
		pacing_integral_ = 0.0;

		// Reset the estimate too. A catch-up burst is not the match's speed,
		// and leaving it pinned at the clamp poisons the feedforward for the
		// first seconds of steady state.
		pacing_rate_hz_ = 59.94;
		pacing_rate_time_ = now;
		pacing_rate_recv_ = recv;
		return;
	}

	// Track whether inputs are still flowing. Outside battle the game exchanges
	// no inputs, so the gap collapses to zero while we are just as far behind in
	// wall clock. Braking against that phantom deficit leaves the brake on when
	// battle resumes.
	if (recv != pacing_last_recv_) {
		pacing_last_recv_ = recv;
		pacing_stall_frames_ = 0;
	} else if (pacing_stall_frames_ < kPacingStallFrames) {
		++pacing_stall_frames_;
	}
	const int64_t live_gap = static_cast<int64_t>(recv) - key_msg_count_;
	const bool stalled = kPacingStallFrames <= pacing_stall_frames_;


	// Outside battle, run at nominal speed. The trim corrects the input buffer,
	// which only means anything while the battle consumes inputs. MS selection,
	// briefing and the result screen are timed scenes, and the trim bounds are
	// wide enough (40-79Hz) that warping them is obvious.
	if (!IsInGame()) {
		gdxsv_frame_period_trim_us = 0;
		pacing_rate_time_ = now;
		pacing_rate_recv_ = recv;
		return;
	}

	if (stalled) {
		// Nothing is flowing, so run the scene at nominal speed. A leftover
		// trim corrects an input-buffer error that no longer exists, and each
		// instance carries a different one - so they walk the same scene at
		// different speeds.
		gdxsv_frame_period_trim_us = 0;

		// Restart the rate window too, so the silence is not averaged in as if
		// the match itself had slowed down.
		pacing_rate_time_ = now;
		pacing_rate_recv_ = recv;
		return;
	}

	// Positive error means we are further behind the edge than we want to be,
	// so we need to run fast, which is a shorter period, which is a negative
	// trim. Getting that sign backwards makes the loop diverge.
	const int64_t gap = live_gap;
	const int64_t error = gap - live_buffer_frames_;

	// Feedforward: play at the speed the match is actually being played at, not
	// at nominal. Anchored on nominal, a constant surplus remains that no clamp
	// centred there can cancel.
	const double win = std::chrono::duration<double>(now - pacing_rate_time_).count();
	if (kPacingRateWindowSec <= win) {
		const double observed = (recv - pacing_rate_recv_) / win;
		if (1.0 < observed) {
			pacing_rate_hz_ = std::clamp((1.0 - kPacingRateAlpha) * pacing_rate_hz_ + kPacingRateAlpha * observed,
										 kPacingRateMinHz, kPacingRateMaxHz);
		}
		pacing_rate_time_ = now;
		pacing_rate_recv_ = recv;
	}
	const double feedforward = 1.0e6 / pacing_rate_hz_ - kPacingNominalPeriodUs;

	const int64_t deadband = std::clamp<int64_t>(live_buffer_frames_ / 4, 1, kPacingMaxDeadbandFrames);
	double p = 0.0;
	if (deadband < std::abs(error)) {
		p = static_cast<double>(-error * kPacingTrimUsPerFrame);
	}

	// Conditional integration. A clamp alone is not anti-windup - it saturates
	// and stays there. Integrate only while the output is inside its bounds, or
	// while the new term would move it back toward the middle.
	const double unsaturated = feedforward + p + pacing_integral_;
	const double step = -error * kPacingIntegralGain;
	const bool at_ceiling = kPacingTrimCeilUs <= unsaturated && 0.0 < step;
	const bool at_floor = unsaturated <= kPacingTrimFloorUs && step < 0.0;
	if (deadband < std::abs(error) && !at_ceiling && !at_floor) {
		pacing_integral_ = std::clamp(pacing_integral_ + step, -kPacingIntegralLimit, kPacingIntegralLimit);
	}

	gdxsv_frame_period_trim_us =
		static_cast<int>(std::clamp(feedforward + p + pacing_integral_, kPacingTrimFloorUs, kPacingTrimCeilUs));
}

void GdxsvBackendReplay::OnNextFrame() {
	// Must run here, on the emulation thread, before the early-returns below.
	//
	// Thread: DeliverKeyMsgBatch reads log_file_ on the emulation thread while
	// this appends to it. Appending to a repeated field can reallocate, so
	// draining from mainui_loop is a real race under ThreadedRendering.
	//
	// Position: a seek burst runs nested emu.run() calls and never returns to
	// mainui_loop. Draining here lets each one pick up newly arrived frames,
	// so a catch-up seek can chase an edge that is still moving.
	CheckLiveUpdate();
	ProcessUiCommands();
	if (state_ != State::End) {
		UpdateReplayFlow();
		OnNextFrameInternal();
	}
	PublishUiState();
}

void GdxsvBackendReplay::OnNextFrameInternal() {
	if (state_ == State::End) return;

	// One sync point for every scene, here rather than in the delivery path
	// because outside battle no input is delivered - which is exactly where the
	// group used to drift apart.
	//
	// Offline replay syncs too. It is the only repeatable harness: the same
	// recorded file, the same deterministic simulation, no join-time skew and
	// no network variance - so a measured change is caused by the change,
	// which live battles cannot tell us.
	if (spectate_sync_.Active()) {
		// Advances while key_msg_count_ is frozen, so there is still something
		// to sync on outside battle.
		if (sync_subframe_ < kSyncSubFrames - 1) ++sync_subframe_;

		const int64_t pos = static_cast<int64_t>(key_msg_count_) * kSyncSubFrames + sync_subframe_;

		// Heartbeat unconditionally, sync only when eligible. Publishing
		// only from inside WaitForPeers lets a catching-up instance go stale
		// and lose its slot.
		spectate_sync_.Publish(static_cast<int32_t>(pos));

		// Runs in every scene. This never speeds anything up - it only holds
		// back an instance that is ahead, which while production is stalled is
		// indistinguishable from waiting for data.
		if (!takeover_ && !seeking_ && (!live_mode_ || !live_catching_up_)) {
			spectate_sync_.WaitForPeers(static_cast<int32_t>(pos), sync_max_wait_ms_);
		}
	}
	UpdateFramePacing();

	// After UpdateFramePacing, not before: its offline branch clears the trim
	// every frame, so setting it earlier had no effect at all.
	//
	// Offline replay only. In live mode the pacing controller owns the trim -
	// the buffer target has to win over group alignment, or playback starves.
	if (spectate_sync_.Active() && !live_mode_ && !takeover_ && !seeking_) {
		const int64_t pos = static_cast<int64_t>(key_msg_count_) * kSyncSubFrames + sync_subframe_;
		const int lead_frames = spectate_sync_.LeadOverSlowest(static_cast<int32_t>(pos)) / kSyncSubFrames;
		gdxsv_frame_period_trim_us =
			lead_frames <= 0 ? 0 : std::min(lead_frames * kSyncTrimUsPerFrame, kSyncTrimMaxUs);
	}

	if (!end_of_frame_) return;
	if (seeking_) return;

	constexpr int save_interval = ReplaySeekFrames;
	auto need_cancel = [&]() -> bool {
		return ctrl_commands_.contains(ReplayCtrlCommand::SaveFirstFrame) || state_ == State::End;
	};
	auto regular_save_state = [&]() {
		if ((IsInGame() || IsInBriefing()) && gdxsv_save_state.LastSavedFrame() + save_interval <= key_msg_count_ && recv_buf_.empty() && !takeover_) {
			gdxsv_save_state.SaveState(key_msg_count_);
		}
	};
	auto can_run_silent_replay_frame = [&]() -> bool {
		// Leave the final replay input for the normal frame path. Ending replay
		// from a nested silent emu.run() can stall teardown, especially when the
		// requested seek overshoots EOF while render skipping is still enabled.
		return takeover_ || state_ != State::McsInBattle || key_msg_count_ + 1 < log_file_.inputs_size();
	};

	// Audio fade-in after save state load (e.g. StepFrameBackward)
	// Quadratic curve: stays near 0 initially, ramps up quickly at the end
	if (audio_fade_frames_ > 0) {
		audio_fade_frames_--;
		const float t = 1.0f - (float)audio_fade_frames_ / 20.0f; // 0.0 -> 1.0
		settings.aica.audioFade = t * t;
	}

	// Unpause if we left the game phase (e.g. stepped into briefing)
	if (ctrl_pause_ && !IsInGame()) {
		ctrl_pause_ = false;
	}

	// Never in Live Spectate: it is an analysis overlay, and it shows the
	// players' inputs while they are still playing.
	gdxsv.key_display_.enabled(config::GdxReplayKeyDisplay && IsInGame() && !live_mode_);
	regular_save_state();

	// Reaching the live edge resumes following, however it got there. Outside
	// the catch-up gate below so it stays right while paused.
	if (live_mode_) {
		const int64_t gap_now = static_cast<int64_t>(log_file_.inputs_size()) - key_msg_count_;
		live_at_edge_ = gap_now <= live_buffer_frames_ + kLiveAtEdgeMargin;

		// Not while a requested jump is queued: key_msg_count_ is still at the
		// live edge until it lands, so this would undo the request.
		const bool reposition_pending = ctrl_commands_.contains(ReplayCtrlCommand::JumpToKeyMsg) ||
										ctrl_commands_.contains(ReplayCtrlCommand::SetRound) ||
										ctrl_commands_.contains(ReplayCtrlCommand::SeekBackward) ||
										ctrl_commands_.contains(ReplayCtrlCommand::StepFrameBackward) ||
										ctrl_commands_.contains(ReplayCtrlCommand::SeekToBriefing);
		if (live_at_edge_ && !ctrl_loading_ && !reposition_pending) {
			live_following_ = true;
		}
	}

	// Catch-up runs coarsest first: a round jump crosses whole rounds, one
	// skip-render seek closes the gap within the round, and 300% playback takes
	// the residual. All three stop a buffer behind the live edge, not at it,
	// because frames arrive in clumps.
	const int kLiveTargetBufferFrames = live_buffer_frames_;

	// 300% recovery only runs in battle. The result, briefing and MS-selection
	// screens have animation and music, so speeding through them is just as
	// visible as speeding through a fight. A skip-render seek is different: it
	// is a jump, not fast playback, so it may run in any scene.
	const int kLiveCatchUpThreshold = live_buffer_frames_ + 240;
	const int kLiveCatchUpFastSeekThreshold = live_buffer_frames_ + 270;

	// start_msg_count_ > 0, not IsInGame(): during the pre-round-1 boot
	// inputs_size() is already full while key_msg_count_ has not started
	// moving, so the gap reads enormous.
	if (live_mode_ && 0 < start_msg_count_ && !live_round_jump_pending_ && !ctrl_pause_ && !pause_menu_opend_ &&
		!need_cancel() && !takeover_) {
		const int64_t gap = static_cast<int64_t>(log_file_.inputs_size()) - key_msg_count_;

		// Measured every frame, never latched. Right after a round jump
		// playback sits at the round's start while the log is still streaming,
		// so the gap reads negative - a latch set from that never reopens.
		// Never catching up while not following, so the intake cap and the sync
		// barrier do not treat a deliberate rewind as a catch-up burst.
		live_catching_up_ = live_following_ && kLiveCatchUpFastSeekThreshold <= gap;
		if (live_catching_up_) {
			// One seek closes it. The seek re-reads the live edge every
			// iteration, and CheckLiveUpdate keeps delivering inside the
			// nested emu.run, so it chases an edge that is still moving.
			// Seeking runs about 10x realtime, so it converges in one pass.
			if (!ctrl_commands_.contains(ReplayCtrlCommand::SeekForward)) {
				// BeginLoadingHud() resets the counter that lets a queued
				// SeekForward run, so re-queueing every frame would stall it.
				BeginLoadingHud();
				// Bound generously. The seek folds in new frames as it runs, so
				// it covers more than the gap measured now. It stops on the gap
				// itself; this is only a safety cap.
				const int64_t bound = std::min<int64_t>(gap * 3 + 1200, 1 << 20);
				ctrl_commands_.emplace_back(ReplayCtrlCommand::SeekForward, static_cast<int>(bound), /*live=*/1);
			}
		} else if (live_following_ && IsInGame() && kLiveCatchUpThreshold <= gap && ctrl_play_speed_ == 0) {
			// Use 300% recovery below the fast-seek threshold.
			ctrl_play_speed_ = 2;
		}
	}

	if (0 < ctrl_play_speed_ && !ctrl_pause_ && !pause_menu_opend_ && !need_cancel() && !takeover_) {
		BeginSilentSeek();
		int skipped_frame = 0;
		for (; skipped_frame < ctrl_play_speed_ && can_run_silent_replay_frame(); skipped_frame++) {
			RunSilentSeekFrame(config::GdxSkipRenderingHack && skipped_frame + 1 < ctrl_play_speed_);
			regular_save_state();
			if (need_cancel()) break;
		}
		EndSilentSeek();

		// Stop at the target buffer, not at the live edge, and not merely
		// because this burst ran out of data. With kLiveCatchUpThreshold this
		// is the hysteresis band that stops the speed flapping.
		if (live_mode_ && !need_cancel()) {
			const int64_t gap_after = static_cast<int64_t>(log_file_.inputs_size()) - key_msg_count_;
			if (gap_after <= kLiveTargetBufferFrames + 10) {
				ctrl_play_speed_ = 0;
			}
		}
	}

	ReplayCtrlCommand ctrl{};
	while (ctrl_commands_.try_get_front(ctrl)) {
		const bool wait_for_loading_hud = ctrl.cmd == ReplayCtrlCommand::JumpToKeyMsg || ctrl.cmd == ReplayCtrlCommand::SetRound ||
										  ctrl.cmd == ReplayCtrlCommand::NextRound || ctrl.cmd == ReplayCtrlCommand::SeekToBriefing ||
										  (ctrl.cmd == ReplayCtrlCommand::SeekForward && ctrl.arg1 > 0);
		if (ctrl_loading_ && wait_for_loading_hud && ctrl_loading_wait_frames_++ < 2) {
			return;
		}

		if (ctrl.cmd == ReplayCtrlCommand::TogglePauseMenu) {
			if (takeover_aligning_) {
				CancelPendingTakeover();
			} else if (takeover_countdown_ == 0) {
				pause_menu_opend_ = !pause_menu_opend_;
				if (pause_menu_opend_) {
					if (!recv_buf_.empty()) {
						BeginSilentSeek();
						RunSilentSeekFrame(false);
						EndSilentSeek();
					}
					verify(recv_buf_.empty());
					gdxsv_save_state.SaveState(key_msg_count_);
					NOTICE_LOG(COMMON, "Save Menu Opened frame %d", key_msg_count_);
				}
				SDL_ShowCursor(SDL_ENABLE);
			}

			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SaveFirstFrame) {
			verify(recv_buf_.empty());
			NOTICE_LOG(COMMON, "SaveFirstFrame saved (FirstSavedFrame before clear=%d)", gdxsv_save_state.FirstSavedFrame());
			gdxsv_save_state.Clear();
			gdxsv_save_state.SaveState(key_msg_count_);
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SendStartMsg) {
			SendStartMsgs();
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::TogglePause) {
			if (state_ == State::McsInBattle && IsInGame()) {
				ctrl_pause_ = !ctrl_pause_;
			}
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::StepFrame) {
			if (ctrl_pause_) {
				int roundStart, roundEnd, totalRounds;
				GetRoundReplayBounds(roundStart, roundEnd, totalRounds);
				if (key_msg_count_ >= roundEnd) {
					// Don't step past the end of the current round
					ctrl_commands_.pop_front();
					continue;
				}
				ctrl_step_frame_ = true;
			}
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::StepFrameBackward) {
			// When paused, the game has already pre-read the input for the next frame:
			//   key_msg_count_ = N (frame N's input processed), displayed frame = N-1
			// To display frame N-2, we fast-forward to key_msg_count_ = N-2,
			// then let the game naturally re-read frame N-1's input and pause again.
			if (ctrl_pause_ && key_msg_count_ > 1) {
				const int goal = key_msg_count_ - 2;
				int load_frame = goal;
				bool loaded = gdxsv_save_state.LoadStateMostRecent(load_frame);
				if (!loaded) {
					int first = gdxsv_save_state.FirstSavedFrame();
					if (first >= 0 && first <= goal) {
						loaded = gdxsv_save_state.LoadState(first);
						if (loaded) load_frame = first;
					}
				}
				if (loaded) {
					settings.aica.muteAudio = true;
					key_msg_count_ = load_frame;
					recv_buf_.clear();
					recv_delay_ = 0;
					int frames_run = 0;
					BeginSilentSeek();
					while (key_msg_count_ < goal) {
						RunSilentSeekFrame(config::GdxSkipRenderingHack && key_msg_count_ + 1 < goal);
						regular_save_state();
						frames_run++;
						if (need_cancel() || frames_run > 1000) break;
					}
					EndSilentSeek();
					RebuildKeyDisplay();
					settings.aica.muteAudio = false;
					settings.aica.audioFade = 0.0f;
					audio_fade_frames_ = 20;
					NOTICE_LOG(COMMON, "StepFrameBackward: %d -> %d (loaded:%d ran:%d frames)", goal + 1, key_msg_count_, load_frame, frames_run);
				} else {
					NOTICE_LOG(COMMON, "StepFrameBackward: no save state for goal=%d", goal);
				}
			}
			ctrl_commands_.pop_front();
		}
		
		if (ctrl.cmd == ReplayCtrlCommand::JumpToKeyMsg) {
			if (log_file_.inputs_size() <= 0) {
				target_frame_ = 0;
				++timeline_revision_;
				ctrl_loading_ = false;
				ctrl_commands_.pop_front();
				continue;
			}
			const int target_key_msg_count = std::clamp(ctrl.arg1, 0, log_file_.inputs_size());
			const int previous_key_msg_count = key_msg_count_;
			const auto jump_start = high_resolution_clock::now();
			int loaded_frame = -1;
			int roundStart, roundEnd, totalRounds;
			GetRoundReplayBounds(roundStart, roundEnd, totalRounds);

			const int load_frame = gdxsv_save_state.FindSavedFrameAtOrBefore(target_key_msg_count);
			if (load_frame >= 0 && load_frame != key_msg_count_ &&
				((key_msg_count_ < target_key_msg_count && key_msg_count_ < load_frame) ||
				 (target_key_msg_count < key_msg_count_ && load_frame <= target_key_msg_count))) {
				if (gdxsv_save_state.LoadState(load_frame)) {
					key_msg_count_ = load_frame;
					loaded_frame = load_frame;
					recv_buf_.clear();
					recv_delay_ = 0;
				} else {
					WARN_LOG(COMMON, "Failed loading replay save state frame=%d target=%d", load_frame, target_key_msg_count);
				}
			}

			if (loaded_frame == roundStart) {
				PrepareRoundStartReplayState();
			}

			BeginSilentSeekWithAudioReset();
			int frames_run = 0;
			while (key_msg_count_ < target_key_msg_count && can_run_silent_replay_frame()) {
				const bool skip_rendering = config::GdxSkipRenderingHack && key_msg_count_ + 1 < target_key_msg_count;
				RunSilentSeekFrame(skip_rendering);
				regular_save_state();
				frames_run++;
				if (need_cancel()) break;
			}
			EndSilentSeekWithAudioReset();
			const long long jump_ms = duration_cast<milliseconds>(high_resolution_clock::now() - jump_start).count();
			const float ms_per_frame = frames_run > 0 ? static_cast<float>(jump_ms) / frames_run : 0.0f;
			NOTICE_LOG(COMMON, "JumpToKeyMsg %d->%d target=%d load=%d ran=%d frames in %lld[ms] (%.2f[ms/fr])",
					   previous_key_msg_count, key_msg_count_, target_key_msg_count, loaded_frame, frames_run, jump_ms, ms_per_frame);

			if (key_msg_count_ != previous_key_msg_count) {
				RebuildKeyDisplay();
				settings.aica.audioFade = 0.0f;
				audio_fade_frames_ = 20;
			}

			target_frame_ = 0;
			++timeline_revision_;
			ctrl_loading_ = false;
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SeekForward) {
			const int skip_frames = 1 <= ctrl.arg1 ? ctrl.arg1 : save_interval;
			// arg2 marks a Live Spectate catch-up seek. It differs from a
			// plain seek in that new frames keep arriving while it runs, so
			// it folds them in as it goes and keeps seeking until it is
			// actually at the live edge - see live_seek_target below.
			const bool live_catch_up = ctrl.arg2 != 0;
			const int prev_key_msg_count = key_msg_count_;
			auto t0 = high_resolution_clock::now();
			int skipped_frame = 0;
			// A live catch-up seek is done when it is within the target
			// buffer of the live edge, not when it has run a fixed number of
			// frames: folding in new frames as it goes means the edge it is
			// chasing keeps moving. skip_frames stays as a safety bound in
			// case the host cannot emulate faster than the match advances.
			auto live_seek_unfinished = [&]() -> bool {
				return static_cast<int64_t>(log_file_.inputs_size()) - key_msg_count_ > kLiveTargetBufferFrames;
			};
			BeginSilentSeekWithAudioReset();
			for (; skipped_frame < skip_frames && can_run_silent_replay_frame() &&
				   (!live_catch_up || live_seek_unfinished());
				 skipped_frame++) {
				RunSilentSeekFrame(config::GdxSkipRenderingHack && skipped_frame + 1 < skip_frames);
				regular_save_state();
				if (need_cancel()) break;
			}
			EndSilentSeekWithAudioReset();
			if (0 < skipped_frame) {
				const auto ms = duration_cast<milliseconds>(high_resolution_clock::now() - t0).count();
				NOTICE_LOG(COMMON, "SeekForward skipped %d[fr] in %ld[ms] (%.2f[ms/fr]) %d->%d(%d keys)", skipped_frame, ms,
						   (float)ms / skipped_frame, prev_key_msg_count, key_msg_count_, key_msg_count_ - prev_key_msg_count);
				settings.aica.audioFade = 0.0f;
				audio_fade_frames_ = 20;
			}
			ctrl_loading_ = false;
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SeekToBriefing) {
			const int org_speed = ctrl_play_speed_;
			ctrl_play_speed_ = 0;

			BeginSilentSeekWithAudioReset();
			while (!(IsInBriefing() || IsInGame() || need_cancel())) {
				RunSilentSeekFrame(config::GdxSkipRenderingHack);
				regular_save_state();
				if (need_cancel()) break;
			}
			EndSilentSeekWithAudioReset();

			if (config::GdxReplaySkipMsSelection && !live_mode_) {
				briefing_start_frame_ = key_msg_count_;
				briefing_start_frame_round_ = start_msg_count_;
			}
			ctrl_play_speed_ = org_speed;
			ctrl_loading_ = false;
			ctrl_commands_.pop_front();
			gdxsv.key_display_.Clear();

			if (target_round_ > 1) {
				BeginLoadingHud();
				ctrl_commands_.emplace_back(ReplayCtrlCommand::SetRound, target_round_);
			} else if (target_frame_ != 0) {
				BeginLoadingHud();
				ctrl_commands_.emplace_back(ReplayCtrlCommand::JumpToKeyMsg, target_frame_);
			} else {
				// Nothing further queued, so a live round jump (if one was
				// what got us here) ends now rather than chaining into
				// SetRound - e.g. need_cancel() cut the loop above short.
				// Must clear here too, or catch-up stays suppressed for the
				// rest of the connect.
				live_round_jump_pending_ = false;
			}
		}

		if (ctrl.cmd == ReplayCtrlCommand::SeekBackward) {
			if (state_ == State::McsInBattle) {
				int roundStart, roundEnd, totalRounds;
				GetRoundReplayBounds(roundStart, roundEnd, totalRounds);
				int timelineStart, timelineEnd, timelineRounds;
				GetControlTimelineBounds(timelineStart, timelineEnd, timelineRounds);
				const int seekStart = key_msg_count_ < timelineStart ? roundStart : timelineStart;
				const int target_frame = std::max(seekStart, key_msg_count_ - save_interval);
				const int load_frame = gdxsv_save_state.FindSavedFrameAtOrBefore(target_frame);
				if (load_frame >= roundStart && gdxsv_save_state.LoadState(load_frame)) {
					key_msg_count_ = load_frame;
					recv_buf_.clear();
					recv_delay_ = 0;
					if (key_msg_count_ == roundStart) {
						PrepareRoundStartReplayState();
					} else {
						RebuildKeyDisplay();
						if (!IsInGame()) {
							EventManager::event(Event::GGPOGameEnd);
						}
					}
					BeginSilentSeek();
					while (key_msg_count_ < target_frame) {
						RunSilentSeekFrame(config::GdxSkipRenderingHack && key_msg_count_ + 1 < target_frame);
						regular_save_state();
						if (need_cancel()) break;
					}
					EndSilentSeek();
					if (IsInGame()) {
						RebuildKeyDisplay();
					} else {
						gdxsv.key_display_.Clear();
					}
					settings.aica.audioFade = 0.0f;
					audio_fade_frames_ = 20;
				}
			}
			ctrl_loading_ = false;
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SetSpeed || ctrl.cmd == ReplayCtrlCommand::NextSpeed) {
			int speed = ctrl.cmd == ReplayCtrlCommand::SetSpeed ? ctrl.arg1 : ctrl_play_speed_ + ctrl.arg1;
			speed = std::max<int>(-2, std::min<int>(2, speed));
			if (speed != ctrl_play_speed_) {
				ctrl_play_speed_ = speed;
			}
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SetRound || ctrl.cmd == ReplayCtrlCommand::NextRound) {
			const int round = ctrl.cmd == ReplayCtrlCommand::SetRound ? ctrl.arg1 : start_msg_count_ + ctrl.arg1;
			if (0 < round && round - 1 < log_file_.start_msg_indexes_size() &&
				round - 1 < log_file_.start_msg_randoms_size() && gdxsv_save_state.FirstSavedFrame() != -1) {
				gdxsv_save_state.LoadState(gdxsv_save_state.FirstSavedFrame());
				key_msg_count_ = log_file_.start_msg_indexes(round - 1);
				start_msg_count_ = round;
				briefing_start_frame_ = 0;
				briefing_start_frame_round_ = 0;
				const int k_rnd0 = gdxsv.Disk() == 1 ? 0x0c310800 : 0x0c3abf40;
				const int battle_count = gdxsv.Disk() == 1 ? 0x0c2f6919 : 0x0c392059;
				const int net_battle_count = gdxsv.Disk() == 1 ? 0x0c2f5cab : 0x0c3913eb;
				const int net_battle_count_copy = gdxsv.Disk() == 1 ? 0x0c336719 : 0x0c3d1b99;
				const u16 random_data = log_file_.start_msg_randoms(round - 1) & 0xffffu;
				gdxsv_WriteMem16(k_rnd0, random_data);
				gdxsv_WriteMem8(battle_count, round - 1);
				gdxsv_WriteMem8(net_battle_count, round - 1);
				gdxsv_WriteMem8(net_battle_count_copy, round - 1);
				NOTICE_LOG(COMMON, "ctrl_change_round_:%d key_msg_count_:%d", round, key_msg_count_);
				NOTICE_LOG(COMMON, "start_msg_randoms_size:%d", log_file_.start_msg_randoms_size());
				gdxsv_patch_map_selector(random_data, log_file_.users_size());

				recv_buf_.clear();
				gdxsv.key_display_.Clear();
				target_round_ = 0;
				ctrl_commands_.emplace_back(ReplayCtrlCommand::SaveFirstFrame);
				ctrl_commands_.emplace_back(ReplayCtrlCommand::SendStartMsg);
				if (config::GdxReplaySkipMsSelection && !live_mode_) {
					BeginLoadingHud();
					ctrl_commands_.emplace_back(ReplayCtrlCommand::SeekToBriefing);
				}

				EventManager::event(Event::GGPOGameEnd);
			}

			// The jump has landed (or was rejected by the guard above) -
			// either way it is no longer in flight, so let live catch-up
			// resume and close the remaining within-round gap.
			live_round_jump_pending_ = false;
			ctrl_loading_ = false;
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::TakeOver) {
			// SaveState has been performed when the menu opened
			gdxsv_save_state.LoadStateMostRecent(key_msg_count_);
			RebuildKeyDisplay();
			settings.aica.muteAudio = true;
			takeover_saved_frame_ = key_msg_count_;
			BeginTakeoverAlignment(replayMcsInput(log_file_, takeover_saved_frame_, pov_));
			pause_menu_opend_ = true;
			ctrl_pause_ = false;
			ctrl_play_speed_ = 0;
			recv_buf_.clear();
			SDL_ShowCursor(SDL_ENABLE);
			NOTICE_LOG(COMMON, "TakeOver alignment at key_msg_count_:%d", key_msg_count_);
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::StartTakeover) {
			gdxsv_save_state.LoadState(takeover_saved_frame_);
			key_msg_count_ = takeover_saved_frame_;
			RebuildKeyDisplay();
			recv_buf_.clear();
			const int delay = config::GdxMinDelay.get();
			while (static_cast<int>(takeover_input_buf_.size()) > delay) {
				takeover_input_buf_.pop_front();
			}
			takeover_ = true;
			takeover_aligning_ = false;
			takeover_skip_input_matching_ = false;
			takeover_target_input_ = 0;
			settings.gdxsv.replayModeActive = false;
			pause_menu_opend_ = false;
			settings.aica.muteAudio = false;
			SDL_ShowCursor(SDL_DISABLE);
			NOTICE_LOG(COMMON, "StartTakeover at key_msg_count_:%d", key_msg_count_);
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::RetryTakeover) {
			gdxsv_save_state.LoadState(takeover_saved_frame_);
			key_msg_count_ = takeover_saved_frame_;
			RebuildKeyDisplay();
			settings.aica.muteAudio = true;
			recv_buf_.clear();
			BeginTakeoverAlignment(replayMcsInput(log_file_, takeover_saved_frame_, pov_));
			settings.gdxsv.replayModeActive = true;
			pause_menu_opend_ = true;
			ctrl_pause_ = false;
			SDL_ShowCursor(SDL_ENABLE);
			NOTICE_LOG(COMMON, "RetryTakeover at key_msg_count_:%d", key_msg_count_);
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::ReturnToReplay) {
			gdxsv_save_state.LoadState(takeover_saved_frame_);
			key_msg_count_ = takeover_saved_frame_;
			RebuildKeyDisplay();
			recv_buf_.clear();
			takeover_ = false;
			takeover_aligning_ = false;
			takeover_skip_input_matching_ = false;
			takeover_countdown_ = 0;
			takeover_target_input_ = 0;
			takeover_input_buf_.clear();
			settings.gdxsv.replayModeActive = true;
			takeover_saved_frame_ = -1;
			ctrl_pause_ = true;
			ctrl_step_frame_ = false;
			pause_menu_opend_ = true;
			SDL_ShowCursor(SDL_ENABLE);
			NOTICE_LOG(COMMON, "ReturnToReplay at key_msg_count_:%d", key_msg_count_);
			ctrl_commands_.pop_front();
		}
	}
}

bool GdxsvBackendReplay::OnOpenMenu() {
	if (ui_snapshot_.Read().state <= State::LbsStartBattleFlow) {
		return false;
	}

	ui_commands_.emplace_back(ReplayCtrlCommand::TogglePauseMenu);

	return false;
}

// Live Spectate buffer readout, drawn next to the FPS counter (bottom left).
// Shows the buffer we aim to hold and how far behind the live match we
// actually are, in frames and milliseconds.
void GdxsvBackendReplay::DisplayLivePacingOSD(const UiState& ui) {
	if (!ui.liveMode) return;

	const int64_t gap = static_cast<int64_t>(ui.inputCount) - ui.playbackFrame;
	const double ms = gap * 1000.0 / 59.94;

	char text[64];
	snprintf(text, sizeof(text), "buf %d | behind %lldf (%.0fms)", ui.liveBufferFrames, (long long)gap, ms);

	ImDrawList* dl = ImGui::GetForegroundDrawList();
	ImFont* font = ImGui::GetFont();
	const float fontSize = ImGui::GetFontSize();
	const ImVec2 padding = ScaledVec2(5.f, 5.f);
	const ImVec2 size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, text) + padding * 2.f;

	// Sit to the right of the FPS box when it is up, otherwise take its place.
	const float x = insetLeft + (config::ShowFPS ? uiScaled(150.f) : 0.f);
	const ImVec2 pos(x, ImGui::GetIO().DisplaySize.y - size.y);

	dl->AddRectFilled(pos, pos + size, IM_COL32(32, 32, 32, 90), 0.f);

	// Green when inside the deadband, amber when the controller is correcting.
	const bool holding = std::abs(gap - ui.liveBufferFrames) <= 2;
	dl->AddText(font, fontSize, pos + padding, holding ? IM_COL32(0, 255, 128, 200) : IM_COL32(255, 200, 0, 200), text);
}

void GdxsvBackendReplay::DisplayOSD() {
	const UiState ui = ui_snapshot_.Read();
	if (displayed_timeline_revision_ != ui.timelineRevision) {
		displayed_timeline_revision_ = ui.timelineRevision;
		ctrl_bar_drag_target_frame_ = -1;
	}
	// Kept for measuring the buffer during pacing work; not shown by default.
	// DisplayLivePacingOSD(ui);
	if (!ui.seeking && ui.pauseMenuOpen) {
		RenderPauseMenu(ui);
	}
	UpdateControlBarVisibility(ui);
	RenderControlBar(ui);
	RenderLoadingHud(ui);
}

bool GdxsvBackendReplay::StartFile(const char* path, int pov) {
	FILE* fp = nowide::fopen(path, "rb");
	if (fp == nullptr) {
		NOTICE_LOG(COMMON, "fopen failed path:%s", path);
		return false;
	}

	bool ok = log_file_.ParseFromFileDescriptor(fileno(fp));
	if (!ok) {
		NOTICE_LOG(COMMON, "ParseFromFileDescriptor failed");
		return false;
	}
	fclose(fp);

	if (log_file_.users_size() <= pov) {
		NOTICE_LOG(COMMON, "ReplayPOV %d does not exist: this battle has %d players", pov + 1, log_file_.users_size());
		return false;
	}

	pov_ = pov;
	
	target_round_ = config::loadInt("gdxsv", "replay_target_round", 0);
	target_frame_ = config::loadInt("gdxsv", "replay_target_frame", 0);

	return Start();
}

bool GdxsvBackendReplay::StartBuffer(const std::vector<u8>& buf, int pov) {
	bool ok = log_file_.ParseFromArray(buf.data(), buf.size());
	if (!ok) {
		NOTICE_LOG(COMMON, "ParseFromArray failed");
		return false;
	}

	if (log_file_.users_size() <= pov) {
		NOTICE_LOG(COMMON, "ReplayPOV %d does not exist: this battle has %d players", pov + 1, log_file_.users_size());
		return false;
	}

	pov_ = pov;

	return Start();
}

bool GdxsvBackendReplay::StartLive(const std::string& host, const std::string& battle_code, int pov) {
	// Bootstrap over UDP, not HTTP - LBS's HTTP port isn't reachable from
	// outside the server host. Subscribing from frame 0 makes LBS send the
	// header first, then stream the whole match through the same
	// resend-until-acked path that keeps us live afterwards.
	live_downlink_.Start(host, kGdxsvLbsPort, battle_code, /*from_frame=*/0);

	constexpr int kBootstrapTimeoutMs = 10000;
	if (!live_downlink_.WaitForBootstrap(&log_file_, kBootstrapTimeoutMs)) {
		NOTICE_LOG(COMMON, "StartLive: bootstrap from %s did not complete within %dms", host.c_str(), kBootstrapTimeoutMs);
		live_downlink_.Stop();
		return false;
	}

	if (log_file_.users_size() <= pov) {
		NOTICE_LOG(COMMON, "ReplayPOV %d does not exist: this battle has %d players", pov + 1, log_file_.users_size());
		live_downlink_.Stop();
		return false;
	}
	pov_ = pov;
	if (!Start()) {
		live_downlink_.Stop();
		return false;
	}

	// Catching up to the live edge (a match may already be thousands of
	// frames in by the time a spectator connects) is handled by the
	// continuous live_mode_ check at the top of OnNextFrame.
	live_mode_ = true;
	PublishUiState();
	return true;
}


void GdxsvBackendReplay::PublishUiState() {
	UiState ui = UiState::FromLog(log_file_);
	ui.state = state_;
	ui.playbackFrame = key_msg_count_;
	ui.currentRound = start_msg_count_;
	int totalRounds;
	GetControlTimelineBounds(ui.timelineStart, ui.timelineEnd, totalRounds);
	ui.timelineRevision = timeline_revision_;
	ui.bootSeekFrames = target_frame_ > 0 ? 1000 : 180;
	ui.playSpeed = ctrl_play_speed_;
	ui.inGame = state_ >= State::McsInBattle && state_ != State::End && IsInGame();
	ui.seeking = seeking_;
	ui.paused = ctrl_pause_;
	ui.pauseMenuOpen = pause_menu_opend_;
	ui.loading = ctrl_loading_;
	ui.takeover = takeover_;
	ui.takeoverAligning = takeover_aligning_;
	ui.takeoverSkipInputMatching = takeover_skip_input_matching_;
	ui.takeoverCountdown = takeover_countdown_;
	ui.takeoverTargetInput = takeover_target_input_;
	ui.liveMode = live_mode_;
	ui.liveAtEdge = live_at_edge_;
	ui.liveBufferFrames = live_buffer_frames_;
	ui_snapshot_.Publish(std::move(ui));
}

void GdxsvBackendReplay::ProcessUiCommands() {
	ReplayCtrlCommand cmd;
	while (ui_commands_.try_get_front(cmd)) {
		ui_commands_.pop_front();
		if (state_ == State::None || state_ == State::End)
			continue;
		switch (cmd.cmd) {
		case ReplayCtrlCommand::FollowLive: {
			if (!live_mode_) break;
			live_following_ = true;
			ctrl_pause_ = false;
			ctrl_play_speed_ = 0;
			const int live_round = log_file_.start_msg_indexes_size();
			if (start_msg_count_ < live_round && live_round - 1 < log_file_.start_msg_randoms_size() &&
				gdxsv_save_state.FirstSavedFrame() != -1) {
				// Block automatic catch-up until this round jump has landed.
				live_round_jump_pending_ = true;
				BeginLoadingHud();
				ctrl_commands_.emplace_back(ReplayCtrlCommand::SetRound, live_round);
			}
			break;
		}
		case ReplayCtrlCommand::ResumePlayback:
			ctrl_pause_ = false;
			ctrl_step_frame_ = false;
			pause_menu_opend_ = false;
			break;
		case ReplayCtrlCommand::ExitReplay:
			Stop();
			break;
		case ReplayCtrlCommand::CancelTakeover:
			CancelPendingTakeover();
			break;
		case ReplayCtrlCommand::SkipTakeoverAlignment:
			// A queued input sample may have already started the matching countdown.
			if (live_mode_ || !pause_menu_opend_ || takeover_skip_input_matching_ ||
				(!takeover_aligning_ && takeover_countdown_ == 0)) break;
			takeover_skip_input_matching_ = true;
			takeover_aligning_ = false;
			takeover_countdown_ = 60;
			takeover_input_buf_.clear();
			break;
		case ReplayCtrlCommand::TakeoverInput: {
			// RetryTakeover keeps takeover_ true while waiting for matching input.
			if (!pause_menu_opend_) break;
			const u16 input = static_cast<u16>(cmd.arg1);
			if (takeover_aligning_) {
				if (input == takeover_target_input_) {
					takeover_aligning_ = false;
					takeover_countdown_ = 60;
					takeover_input_buf_.clear();
				}
			} else if (takeover_countdown_ > 0) {
				if (!takeover_skip_input_matching_ && input != takeover_target_input_) {
					takeover_countdown_ = 0;
					takeover_aligning_ = true;
					takeover_input_buf_.clear();
				} else {
					takeover_input_buf_.push_back(input);
					if (--takeover_countdown_ == 0)
						ctrl_commands_.emplace_back(ReplayCtrlCommand::StartTakeover);
				}
			}
			break;
		}
		case ReplayCtrlCommand::SetRound:
			live_following_ = false;
			live_round_jump_pending_ = live_mode_;
			pause_menu_opend_ = false;
			BeginLoadingHud();
			ctrl_commands_.emplace_back(cmd);
			break;
		case ReplayCtrlCommand::JumpToKeyMsg:
			live_following_ = false;
			BeginLoadingHud();
			ctrl_commands_.emplace_back(cmd);
			break;
		case ReplayCtrlCommand::SeekBackward:
		case ReplayCtrlCommand::StepFrameBackward:
			live_following_ = false;
			ctrl_commands_.emplace_back(cmd);
			break;
		case ReplayCtrlCommand::TakeOver:
			if (!live_mode_ && IsInGame())
				ctrl_commands_.emplace_back(cmd);
			break;
		default:
			ctrl_commands_.emplace_back(cmd);
			break;
		}
	}
}

void GdxsvBackendReplay::CheckLiveUpdate() {
	if (!live_mode_) {
		return;
	}

	live_downlink_.DrainInto(&log_file_);
	live_downlink_.ReportAcked(log_file_.inputs_size());

	if (!log_file_.close_reason().empty()) {
		// Battle ended - stop the downlink and let the normal exhaustion
		// path Stop() once playback drains what's left of log_file_.
		live_mode_ = false;
		live_downlink_.Stop();
	}
}

void GdxsvBackendReplay::Stop() {
	config::FixedFrequency.load();
	gdxsv_frame_period_trim_us = 0;
	ctrl_commands_.clear();
	settings.gdxsv.replayModeActive = false;
	settings.gdxsv.skipRenderingAddr = 0;
	settings.aica.muteAudio = false;
	takeover_ = false;
	takeover_saved_frame_ = -1;
	takeover_countdown_ = 0;
	takeover_aligning_ = false;
	takeover_skip_input_matching_ = false;
	takeover_target_input_ = 0;
	takeover_input_buf_.clear();
	SDL_ShowCursor(SDL_ENABLE);
	rend_enable_renderer(true);
	gdxsv_save_state.EndUsing();
	gdxsv.key_display_.enabled(false);
	state_ = State::End;
	emu.getSh4Executor()->Stop(); // Fix fastForwardMode hang

	if (save_converted_log_) {
		auto replay_dir = get_writable_data_path("replays");
		if (!file_exists(replay_dir)) {
			if (!make_directory(replay_dir)) {
				ERROR_LOG(COMMON, "Failed to create replay directory");
				return;
			}
		}

		auto replay_file = replay_dir + "/" + log_file_.battle_code() + "_converted.pb";
		FILE* f = nowide::fopen(replay_file.c_str(), "wb");
		if (f == nullptr) {
			ERROR_LOG(COMMON, "SaveReplay: fopen failure");
			return;
		}

		int fd = fileno(f);
		if (fd == -1) {
			ERROR_LOG(COMMON, "SaveReplay: fileno failure");
			return;
		}

		bool ok = log_file_.SerializeToFileDescriptor(fd);
		fclose(f);

		if (!ok) {
			ERROR_LOG(COMMON, "SaveReplay: SerializeToFileDescriptor failure");
		}
		NOTICE_LOG(COMMON, "SaveReplay: Done");
	}
}

void GdxsvBackendReplay::BeginTakeoverAlignment(u16 target_input) {
	takeover_countdown_ = 0;
	takeover_aligning_ = true;
	// Each attempt starts with input matching enabled.
	takeover_skip_input_matching_ = false;
	takeover_target_input_ = target_input;
	takeover_input_buf_.clear();
}

void GdxsvBackendReplay::CancelPendingTakeover() {
	takeover_ = false;
	takeover_aligning_ = false;
	takeover_skip_input_matching_ = false;
	takeover_countdown_ = 0;
	takeover_target_input_ = 0;
	takeover_input_buf_.clear();
	takeover_saved_frame_ = -1;
	settings.aica.muteAudio = false;
	settings.gdxsv.replayModeActive = true;
	pause_menu_opend_ = true;
	SDL_ShowCursor(SDL_ENABLE);
}

void GdxsvBackendReplay::Open() {
	recv_buf_.assign({0x0e, 0x61, 0x00, 0x22, 0x10, 0x31, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd});
	state_ = State::McsSessionExchange;
	ApplyPatch(true);
}

void GdxsvBackendReplay::Close() {
	if (state_ <= State::McsWaitJoin) {
		return;
	}

	if (state_ != State::End) {
		PrintDisconnectionSummary();
	}

	Stop();
}

u32 GdxsvBackendReplay::OnSockWrite(u32 addr, u32 size) {
	if (state_ <= State::McsWaitJoin) {
		u8 buf[InetBufSize];
		for (int i = 0; i < size; ++i) {
			buf[i] = gdxsv_ReadMem8(addr + i);
		}
		lbs_tx_reader_.Write((const char*)buf, size);
		ProcessLbsMessage();
	}

	ApplyPatch(false);
	return size;
}

u32 GdxsvBackendReplay::OnSockRead(u32 addr, u32 size) {
	if (state_ <= State::McsWaitJoin) {
		ProcessLbsMessage();
	} else {
		const int disk = gdxsv.Disk();
		const int InetBuf = disk == 1 ? 0x0c310244 : 0x0c3ab984;
		int msg_len = gdxsv_ReadMem8(InetBuf);
		if (0 < msg_len) {
			if (msg_len == 0x82) {
				msg_len = 20;
			}
			McsMessage msg;
			msg.body.resize(msg_len);
			for (int i = 0; i < msg_len; i++) {
				msg.body[i] = gdxsv_ReadMem8(InetBuf + i);
				gdxsv_WriteMem8(InetBuf + i, 0);
			}
			ProcessMcsMessage(msg);
		}
	}

	if (0 < recv_delay_) {
		recv_delay_--;
		return 0;
	}

	if (ctrl_pause_ && !seeking_) {
		if (!ctrl_step_frame_) {
			return 0;
		}
		ctrl_step_frame_ = false;
	}

	if (pause_menu_opend_ && !seeking_) {
		return 0;
	}

	int n = std::min<int>(recv_buf_.size(), size);
	for (int i = 0; i < n; ++i) {
		gdxsv_WriteMem8(addr + i, recv_buf_.front());
		recv_buf_.pop_front();
	}

	return n;
}

u32 GdxsvBackendReplay::OnSockPoll() {
	if (state_ <= State::LbsStartBattleFlow) {
		ProcessLbsMessage();
	}
	if (0 < recv_delay_) {
		recv_delay_--;
		return 0;
	}
	return recv_buf_.size();
}

bool GdxsvBackendReplay::Start() {
	NOTICE_LOG(COMMON, "game_disk = %s", log_file_.game_disk().c_str());

	// The match was played at 59.94Hz - the online battle path pins
	// FixedFrequency to it - so play it back on the same clock. Without this
	// the default is 0, meaning no pacing at all: playback then runs at
	// whatever the renderer manages, and in Live Spectate the only thing
	// holding it back is running out of input, which makes arrival jitter the
	// clock. Restored in Stop().
	// Headless test runs have no display to pace to, so run flat out instead of
	// pinning to 59.94Hz: fast-forward skips the frame wait and lets a whole
	// replay play in a fraction of its real duration. Restored in Stop().
	if (gdxsv_headless()) {
		settings.input.fastForwardMode = true;
		config::FixedFrequency.override(0);
	} else {
		config::FixedFrequency.override(2);
	}
	gdxsv_frame_period_trim_us = 0;

	live_buffer_frames_ = std::clamp(config::loadInt("gdxsv", "LiveBufferFrames", kLiveDefaultBuffer), kLiveMinBuffer, kLiveMaxBuffer);
	spectate_sync_.Join(config::loadStr("gdxsv", "SpectateSyncGroup", ""));

	// Tunable so the sync harness can sweep it without a rebuild. 0 disables
	// waiting entirely, which is the A/B for "is the barrier costing frames?".
	sync_max_wait_ms_ = std::clamp(config::loadInt("gdxsv", "SyncMaxWaitMs", kSyncMaxWaitPerFrameMs), 0, 200);
	NOTICE_LOG(COMMON, "spectate sync max wait %d ms/frame", sync_max_wait_ms_);
	NOTICE_LOG(COMMON, "replay pacing: 59.94Hz, live buffer %d frames (%.2fs)", live_buffer_frames_, live_buffer_frames_ / 59.94);

	if (log_file_.log_file_version() < 20210802) {
		// Parse old replay file
		// proto2 required fields was moved to UnknownFields
		for (int i = 0; i < log_file_.battle_data_size(); ++i) {
			auto data = log_file_.mutable_battle_data(i);
			const auto& fields = proto::BattleLogFile::GetReflection()->GetUnknownFields(*data);
			if (!fields.empty()) {
				for (int j = 0; j < fields.field_count(); ++j) {
					const auto& field = fields.field(j);
					if (j == 0 && field.type() == google::protobuf::UnknownField::TYPE_LENGTH_DELIMITED) {
						const auto& body = field.length_delimited();
						data->set_body(body.data(), body.size());
					}
					if (j == 1 && field.type() == google::protobuf::UnknownField::TYPE_VARINT) {
						data->set_seq(field.varint());
					}
				}
			}
		}

		// Restore player position, team and grade.
		std::map<std::string, int> player_position;
		McsMessageReader r;
		McsMessage msg;
		for (int i = 0; i < log_file_.battle_data_size(); ++i) {
			const auto& data = log_file_.battle_data(i);
			if (player_position.find(data.user_id()) == player_position.end()) {
				r.Write(data.body().data(), data.body().size());
				while (r.Read(msg)) {
					if (msg.Type() == McsMessage::MsgType::PingMsg) {
						player_position[data.user_id()] = msg.Sender();
						break;
					}
				}
			}
			if (log_file_.users_size() == player_position.size()) {
				break;
			}
		}

		for (int i = 0; i < log_file_.users_size(); ++i) {
			const int pos = player_position[log_file_.users(i).user_id()];
			log_file_.mutable_users(i)->set_pos(pos + 1);
			log_file_.mutable_users(i)->set_team(1 + pos / 2);
			// NOTE: Surprisingly, player's grade seems to affect the game.
			log_file_.mutable_users(i)->set_grade(std::min(14, log_file_.users(i).win_count() / 100));
			log_file_.mutable_users(i)->set_user_name_sjis(log_file_.users(i).user_id());
		}

		std::sort(log_file_.mutable_users()->begin(), log_file_.mutable_users()->end(),
				  [](const proto::BattleLogUser& a, const proto::BattleLogUser& b) { return a.pos() < b.pos(); });
	}

	if (log_file_.log_file_version() == 20230729) {
		// Fix broken replay: duplicated mutable_start_msg_indexes and randoms
		auto indexes = log_file_.mutable_start_msg_indexes();
		auto randoms = log_file_.mutable_start_msg_randoms();
		for (int i = 1; i < indexes->size(); i++) {
			if (indexes->at(i) == indexes->at(i - 1)) {
				indexes->erase(indexes->begin() + i - 1);
				randoms->erase(randoms->begin() + i - 1);
			}
		}
	}

	if (log_file_.inputs_size() == 0 && log_file_.battle_data_size() != 0) {
		// Convert McsMessage into uint64 input.
		// start_msg_index_ holds input indexes of round start.
		NOTICE_LOG(COMMON, "Converting inputs..");
		McsMessageReader r;
		McsMessage msg;
		std::vector<std::vector<std::vector<u16>>> player_chunked_inputs(log_file_.users_size());

		for (const auto& data : log_file_.battle_data()) {
			r.Write(data.body().data(), data.body().size());

			while (r.Read(msg)) {
				const int p = msg.Sender();
				if (msg.Type() == McsMessage::StartMsg) {
					player_chunked_inputs[p].emplace_back();
				}
				if (msg.Type() == McsMessage::KeyMsg1) {
					player_chunked_inputs[p].back().emplace_back(msg.FirstInput());
				}
				if (msg.Type() == McsMessage::KeyMsg2) {
					player_chunked_inputs[p].back().emplace_back(msg.FirstInput());
					player_chunked_inputs[p].back().emplace_back(msg.SecondInput());
				}
			}
		}

		for (int chunk = 0; chunk < player_chunked_inputs[0].size(); chunk++) {
			int min_t = player_chunked_inputs[0][chunk].size();
			for (int p = 0; p < log_file_.users_size(); p++) {
				min_t = std::min<int>(min_t, player_chunked_inputs[p][chunk].size());
			}

			log_file_.add_start_msg_indexes(log_file_.inputs_size());

			for (int t = 0; t < min_t; t++) {
				uint64_t input = 0;
				for (int p = 0; p < log_file_.users_size(); p++) {
					input |= static_cast<uint64_t>(player_chunked_inputs[p][chunk][t]) << (p * 16);
				}
				log_file_.add_inputs(input);
			}
		}

		PrintDisconnectionSummary();
	}

	NOTICE_LOG(COMMON, "battle_code = %s", log_file_.battle_code().c_str());
	NOTICE_LOG(COMMON, "users = %d", log_file_.users_size());
	NOTICE_LOG(COMMON, "patch_size = %d", log_file_.patches_size());
	NOTICE_LOG(COMMON, "inputs_size = %d", log_file_.inputs_size());
	std::ostringstream ss;
	for (const int a : log_file_.start_msg_indexes()) ss << a << " ";
	NOTICE_LOG(COMMON, "start_msg_indexes = %s", ss.str().c_str());
	ss.str("");
	for (const int a : log_file_.start_msg_randoms()) ss << a << " ";
	NOTICE_LOG(COMMON, "start_msg_randoms = %s", ss.str().c_str());

	state_ = State::Start;
	gdxsv.maxlag_ = 0;
	gdxsv.key_display_.SetDisplayPlayer(pov_);
	gdxsv.key_display_.enabled(false);
	key_msg_count_ = 0;
	gdxsv_save_state.StartUsing();
	rend_allow_rollback();
	settings.gdxsv.replayModeActive = true;
	SDL_ShowCursor(SDL_ENABLE);

	NOTICE_LOG(COMMON, "Replay Start");
	PublishUiState();
	return true;
}

void GdxsvBackendReplay::PrintDisconnectionSummary() const {
	std::vector<McsMessage> msg_list;
	McsMessageReader r;
	McsMessage msg;

	for (int i = 0; i < log_file_.battle_data_size(); ++i) {
		const auto& data = log_file_.battle_data(i);
		r.Write(data.body().data(), data.body().size());
		while (r.Read(msg)) {
			if (msg.Type() == McsMessage::KeyMsg2) {
				msg_list.emplace_back(msg.FirstKeyMsg());
				msg_list.emplace_back(msg.SecondKeyMsg());
			} else {
				msg_list.emplace_back(msg);
			}
		}
	}

	std::vector<int> last_keymsg_seq(log_file_.users_size());
	std::vector<int> last_force_msg_index(log_file_.users_size());
	for (int i = 0; i < msg_list.size(); ++i) {
		const auto& msg = msg_list[i];
		if (msg.Type() == McsMessage::KeyMsg1) {
			last_keymsg_seq[msg.Sender()] = msg.FirstSeq();
			last_force_msg_index[msg.Sender()] = 0;
		}
		if (msg.Type() == McsMessage::KeyMsg2) {
			last_keymsg_seq[msg.Sender()] = msg.SecondSeq();
			last_force_msg_index[msg.Sender()] = 0;
		}
		if (msg.Type() == McsMessage::ForceMsg) {
			last_force_msg_index[msg.Sender()] = i;
		}
	}

	NOTICE_LOG(COMMON, "== Disconnection Summary ==");
	NOTICE_LOG(COMMON, " KeyCount LastForceMsg UserID Name");
	for (int i = 0; i < log_file_.users_size(); ++i) {
		NOTICE_LOG(COMMON, "%9d %12d %6s %s", last_keymsg_seq[i], last_force_msg_index[i], log_file_.users(i).user_id().c_str(),
				   log_file_.users(i).user_name().c_str());
	}

	const auto it_seq_min = std::min_element(begin(last_keymsg_seq), end(last_keymsg_seq));
	const auto it_seq_max = std::max_element(begin(last_keymsg_seq), end(last_keymsg_seq));
	if (*it_seq_min != *it_seq_max) {
		int i = it_seq_min - begin(last_keymsg_seq);
		bool no_force_msg = last_force_msg_index[i] == 0;
		bool other_player_send_force_msg = std::count(begin(last_force_msg_index), end(last_force_msg_index), 0) == 1;
		if (no_force_msg && other_player_send_force_msg) {
			NOTICE_LOG(COMMON, "!! Disconnected Player Detected !!");
			NOTICE_LOG(COMMON, " KeyCount LastForceMsg UserID Name");
			NOTICE_LOG(COMMON, "%9d %12d %6s %s", last_keymsg_seq[i], last_force_msg_index[i], log_file_.users(i).user_id().c_str(),
					   log_file_.users(i).user_name().c_str());
		}
	}
}

void GdxsvBackendReplay::ProcessLbsMessage() {
	if (state_ == State::Start) {
		LbsMessage::SvNotice(LbsMessage::lbsReadyBattle).Serialize(recv_buf_);
		recv_delay_ = 1;
		state_ = State::LbsStartBattleFlow;
	}

	LbsMessage msg;
	if (lbs_tx_reader_.Read(msg)) {
		if (state_ == State::Start) {
			state_ = State::LbsStartBattleFlow;
		}

		if (msg.command == LbsMessage::lbsLobbyMatchingEntry) {
			LbsMessage::SvAnswer(msg).Serialize(recv_buf_);
			LbsMessage::SvNotice(LbsMessage::lbsReadyBattle).Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskMatchingJoin) {
			int n = log_file_.users_size();
			LbsMessage::SvAnswer(msg).Write8(n)->Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskPlayerSide) {
			// camera player id
			LbsMessage::SvAnswer(msg).Write8(pov_ + 1)->Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskPlayerInfo) {
			int pos = msg.Read8();
			auto user = log_file_.users(pos - 1);

			if (config::GdxReplayHideName) {
				user.set_user_id("USER0" + std::to_string(pos));
				user.set_user_name_sjis("USER0" + std::to_string(pos));
				auto game_param = user.game_param();
				user.set_game_param(game_param.replace(16, 17,
													   "\x82\x6F\x82\x68\x82\x6B\x82\x6E\x82\x73\x82\x4F\x82" +
														   std::string(1, static_cast<char>(0x4F + pos)) +
														   "\x01\x01\x07"));  // ＰＩＬＯＴ０１~０４
				user.set_battle_count(0);
				user.set_win_count(0);
				user.set_lose_count(0);
			}

			LbsMessage::SvAnswer(msg)
				.Write8(pos)
				->WriteString(user.user_id())
				->WriteBytes(user.user_name_sjis().data(), user.user_name_sjis().size())
				->WriteBytes(user.game_param().data(), user.game_param().size())
				->Write16(user.grade())
				->Write16(user.win_count())
				->Write16(user.lose_count())
				->Write16(0)
				->Write16(user.battle_count() - user.win_count() - user.lose_count())
				->Write16(0)
				->Write16(user.team())
				->Write16(0)
				->Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskRuleData) {
			LbsMessage::SvAnswer(msg).WriteBytes(log_file_.rule_bin().data(), log_file_.rule_bin().size())->Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskBattleCode) {
			LbsMessage::SvAnswer(msg).WriteBytes(log_file_.battle_code().data(), log_file_.battle_code().size())->Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskMcsVersion) {
			LbsMessage::SvAnswer(msg).Write8(10)->Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskMcsAddress) {
			LbsMessage::SvAnswer(msg).Write16(4)->Write8(127)->Write8(0)->Write8(0)->Write8(1)->Write16(2)->Write16(3333)->Serialize(
				recv_buf_);
		}

		if (msg.command == LbsMessage::lbsLogout) {
			state_ = State::McsWaitJoin;
		}

		recv_delay_ = 1;
	}
}

// Enqueues the KeyMsg1 batch for the current key_msg_count_ and advances it.
// Sends every player including pov_, because the replay patch disables the
// client's own self-push.
void GdxsvBackendReplay::DeliverKeyMsgBatch() {
	// At the live edge, wait for confirmed input. A neutral batch would make
	// the game simulate an unrecorded frame and permanently desync playback.
	if (live_mode_ && !takeover_ && log_file_.inputs_size() <= key_msg_count_) {
		return;
	}

	// Live Spectate: cap intake at one input frame per rendered frame, so the
	// frame period governs consumption. The game polls for input more often
	// than it renders once it is behind, and answering every poll decouples
	// intake from the frame clock entirely.
	//
	// Steady state only: a skip-render seek runs hundreds of emulator frames
	// inside one UI frame, so capping during catch-up would stall it.
	if (live_mode_ && !takeover_ && !seeking_ && !live_catching_up_ && ctrl_play_speed_ == 0) {
		if (deliver_last_mainui_ != MainFrameCount) {
			deliver_last_mainui_ = MainFrameCount;
			deliver_this_frame_ = 0;
		}

		// Allow a second frame while genuinely behind. A strict one-per-frame
		// cap can never regain a lost buffer when the host renders slower than
		// the match produces, leaving the 300% path to oscillate.
		const int64_t behind = static_cast<int64_t>(log_file_.inputs_size()) - key_msg_count_;
		const int allowance = (live_buffer_frames_ + kPacingRecoverMargin < behind) ? 2 : 1;
		if (allowance <= deliver_this_frame_) {
			return;
		}

		++deliver_this_frame_;
	}
	if (!recv_buf_.empty()) {
		// The previous reply hasn't drained yet. One emu.run() can trigger
		// several OnSockRead polls, so a second poll can arrive before the
		// first reply is fully delivered. Skip instead of stacking a second
		// batch on top - the next poll after the bytes drain gets a fresh one.
		return;
	}
	gdxsv.maxlag_ = 0;

	for (int i = 0; i < log_file_.users_size(); ++i) {
		u16 input = 0;
		if (takeover_ && i == pov_) {
			const int delay = config::GdxMinDelay.get();
			takeover_input_buf_.push_back(liveTakeoverMcsInput());
			if (static_cast<int>(takeover_input_buf_.size()) > delay) {
				input = takeover_input_buf_.front();
				takeover_input_buf_.pop_front();
			}
		} else if (key_msg_count_ < log_file_.inputs_size()) {
			const u64 inputs = log_file_.inputs(key_msg_count_);
			input = u16(inputs >> (i * 16));
		}
		auto key_msg = McsMessage::Create(McsMessage::MsgType::KeyMsg1, i);
		key_msg.body[2] = input >> 8 & 0xff;
		key_msg.body[3] = input & 0xff;
		std::copy(key_msg.body.begin(), key_msg.body.end(), std::back_inserter(recv_buf_));
		if (takeover_countdown_ == 0 && !seeking_) {
			gdxsv.key_display_.AppendInput(i, input);
		}
	}

	if (key_msg_count_ < log_file_.inputs_size()) {
		++key_msg_count_;
		sync_subframe_ = 0;

		if (key_msg_count_ == log_file_.inputs_size() && !takeover_) {
			// Live Spectate: hold at the live edge instead of stopping -
			// CheckLiveUpdate() will grow log_file_ as more data arrives,
			// or clear live_mode_ once the battle has ended, at which
			// point this same check naturally falls through to Stop() on
			// a later tick.
			if (!live_mode_) {
				Stop();
			}
		}
	}

	if (!takeover_ && ctrl_play_speed_ < 0) {
		recv_delay_ = -ctrl_play_speed_;
	}
}

void GdxsvBackendReplay::ProcessMcsMessage(const McsMessage& msg) {
	const auto msg_type = msg.Type();

	if (msg_type == McsMessage::MsgType::ConnectionIdMsg) {
		state_ = State::McsInBattle;
	} else if (msg_type == McsMessage::MsgType::IntroMsg) {
		for (int i = 0; i < log_file_.users_size(); ++i) {
			if (i != pov_) {
				auto intro_msg = McsMessage::Create(McsMessage::MsgType::IntroMsg, i);
				std::copy(intro_msg.body.begin(), intro_msg.body.end(), std::back_inserter(recv_buf_));
			}
		}
	} else if (msg_type == McsMessage::MsgType::IntroMsgReturn) {
		for (int i = 0; i < log_file_.users_size(); ++i) {
			if (i != pov_) {
				auto intro_msg = McsMessage::Create(McsMessage::MsgType::IntroMsgReturn, i);
				std::copy(intro_msg.body.begin(), intro_msg.body.end(), std::back_inserter(recv_buf_));
			}
		}
	} else if (msg_type == McsMessage::MsgType::PingMsg) {
		for (int i = 0; i < log_file_.users_size(); ++i) {
			if (i != pov_) {
				auto pong_msg = McsMessage::Create(McsMessage::MsgType::PongMsg, i);
				pong_msg.SetPongTo(pov_);
				pong_msg.PongCount(msg.PingCount());
				std::copy(pong_msg.body.begin(), pong_msg.body.end(), std::back_inserter(recv_buf_));
			}
		}
	} else if (msg_type == McsMessage::MsgType::PongMsg) {
		// do nothing
	} else if (msg_type == McsMessage::MsgType::StartMsg) {
		if (takeover_) {
			ctrl_commands_.emplace_back(ReplayCtrlCommand::RetryTakeover);
			return;
		}

		start_msg_count_++;
		NOTICE_LOG(COMMON, "StartMsg key_msg_count %d", key_msg_count_);

		if (start_msg_count_ - 1 < log_file_.start_msg_indexes_size()) {
			const auto key_msg_count = log_file_.start_msg_indexes(start_msg_count_ - 1);
			key_msg_count_ = log_file_.start_msg_indexes(start_msg_count_ - 1);
			NOTICE_LOG(COMMON, "key_msg_count updates %d -> %d", key_msg_count_, key_msg_count);
			key_msg_count_ = key_msg_count;
		} else if (save_converted_log_) {
			log_file_.add_start_msg_indexes(key_msg_count_);
		}

		const int k_rnd0 = gdxsv.Disk() == 1 ? 0x0c310800 : 0x0c3abf40;
		const auto random_data = gdxsv_ReadMem16(k_rnd0);
		if (start_msg_count_ - 1 < log_file_.start_msg_randoms_size()) {
			verify(random_data == (log_file_.start_msg_randoms(start_msg_count_ - 1) & 0xffffu));
		} else if (save_converted_log_) {
			log_file_.add_start_msg_randoms(random_data);
		}

		ctrl_commands_.emplace_back(ReplayCtrlCommand::SaveFirstFrame);
		ctrl_commands_.emplace_back(ReplayCtrlCommand::SendStartMsg);
		if (config::GdxReplaySkipMsSelection && !live_mode_) {
			BeginLoadingHud();
			ctrl_commands_.emplace_back(ReplayCtrlCommand::SeekToBriefing);
		} else if (live_mode_ && start_msg_count_ == 1 && start_msg_count_ < log_file_.start_msg_indexes_size()) {
			// Connected mid-match and live is already past this round, so jump
			// straight to the current one. The == 1 keeps this to the first
			// round after connecting: live's round N+1 always starts before we
			// finish round N, so it would otherwise fire at every boundary.
			target_round_ = log_file_.start_msg_indexes_size();
			// Catch-up must not run until the jump lands: it would fast-seek
			// through the intervening rounds' StartMsgs, whose queued
			// SaveFirstFrame/SendStartMsg pairs then interleave with
			// SetRound's and trip verify(recv_buf_.empty()).
			live_round_jump_pending_ = true;
			BeginLoadingHud();
			ctrl_commands_.emplace_back(ReplayCtrlCommand::SeekToBriefing);
		}
	} else if (msg_type == McsMessage::MsgType::KeyMsg1 || msg_type == McsMessage::MsgType::ForceMsg) {
		// The game sends ForceMsg when SendKeyMsg() sends nothing.
		// Supplying the next all-player input batch lets a stalled game resume.
		DeliverKeyMsgBatch();
	} else if (msg_type == McsMessage::MsgType::LagControlTestMsg) {
		// do nothing
	} else if (msg_type == McsMessage::MsgType::KeyMsg2) {
		verify(false);
	} else if (msg_type == McsMessage::MsgType::LoadEndMsg) {
		for (int i = 0; i < log_file_.users_size(); ++i) {
			if (i != pov_) {
				auto load_start_msg = McsMessage::Create(McsMessage::MsgType::LoadStartMsg, i);
				std::copy(load_start_msg.body.begin(), load_start_msg.body.end(), std::back_inserter(recv_buf_));
			}
		}
		for (int i = 0; i < log_file_.users_size(); ++i) {
			if (i != pov_) {
				auto load_end_msg = McsMessage::Create(McsMessage::MsgType::LoadEndMsg, i);
				std::copy(load_end_msg.body.begin(), load_end_msg.body.end(), std::back_inserter(recv_buf_));
			}
		}
	} else {
		WARN_LOG(COMMON, "unhandled mcs msg: %s", McsMessage::MsgTypeName(msg_type));
		WARN_LOG(COMMON, "%s", msg.ToHex().c_str());
		verify(false);
	}
}

void GdxsvBackendReplay::ApplyPatch(bool first_time) {
	if (state_ == State::None || state_ == State::End) {
		return;
	}

	// Prevent disconnections while pausing
	const int disk = gdxsv.Disk();
	const int DataStopCounter = disk == 1 ? 0x0c30fdda : 0x0c3ab51a;
	const int FrameStopCounter = disk == 1 ? 0x0c30fddc : 0x0c3ab51c;
	gdxsv_WriteMem16(DataStopCounter, 0);
	gdxsv_WriteMem16(FrameStopCounter, 0);

	// Skip Key MsgPush
	if (gdxsv.Disk() == 1) {
		gdxsv_WriteMem16(0x8c058b7c, 9);
		gdxsv_WriteMem8(0x0c310450, 1);
	}
	if (gdxsv.Disk() == 2) {
		gdxsv_WriteMem16(0x8c045f64, 9);
		gdxsv_WriteMem8(0x0c3abb90, 1);
	}

	// Online Patch
	for (const auto& patch : log_file_.patches()) {
		for (const auto& code : patch.codes()) {
			gdxsv_WriteMem(code.size(), code.address(), code.changed());
		}
	}
}

void GdxsvBackendReplay::RestorePatch() {
	if (gdxsv.Disk() == 1) {
		gdxsv_WriteMem16(0x8c058b7c, 0x410b);
		gdxsv_WriteMem8(0x0c310450, 2);
	}
	if (gdxsv.Disk() == 2) {
		gdxsv_WriteMem16(0x8c045f64, 0x410b);
		gdxsv_WriteMem8(0x0c3abb90, 2);
	}

	// Online Patch
	for (const auto& patch : log_file_.patches()) {
		for (const auto& code : patch.codes()) {
			gdxsv_WriteMem(code.size(), code.address(), code.original());
		}
	}
}

void GdxsvBackendReplay::RenderLoadingHud(const UiState& ui) {
	if (!ui.loading || ui.pauseMenuOpen || ui.takeover) {
		return;
	}
	ImguiStyleVar rounding(ImGuiStyleVar_WindowRounding, uiScaled(8.0f));
	ImguiStyleVar border(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImguiStyleVar padding(ImGuiStyleVar_WindowPadding, ScaledVec2(28.0f, 18.0f));
	centerNextWindow();
	ImGui::SetNextWindowSize(ScaledVec2(200.0f, 76.0f));
	ImGui::SetNextWindowBgAlpha(0.82f);
	ImGui::Begin("##gdxsv-replay-loading", nullptr,
				 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
					 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs);
	const char* text = "Loading...";
	const ImVec2 textSize = ImGui::CalcTextSize(text);
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	ImGui::SetCursorPos(
		ImVec2((avail.x - textSize.x) * 0.5f + ImGui::GetStyle().WindowPadding.x, (avail.y - textSize.y) * 0.5f + ImGui::GetStyle().WindowPadding.y));
	ImGui::TextUnformatted(text);
	ImGui::End();
}

void GdxsvBackendReplay::RenderPauseMenu(const UiState& ui) {
	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
	ImguiStyleVar _1(ImGuiStyleVar_WindowBorderSize, 0);
	centerNextWindow();
	ImGui::SetNextWindowSize(ScaledVec2(320, 0));
	ImGui::SetNextWindowBgAlpha(0.9f);

	ImGuiWindowFlags window_flags =
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings;
	if (ui.takeoverCountdown > 0 || ui.takeoverAligning) {
		window_flags |= ImGuiWindowFlags_NoNav;
	}
	ImGui::Begin("##gdxsv-replay-pause", NULL, window_flags);

	if (ui.takeoverCountdown > 0 || ui.takeoverAligning) {
		ImGui::SetNextFrameWantCaptureKeyboard(false);
		const u16 current_input = liveTakeoverMcsInput();
		ui_commands_.emplace_back(ReplayCtrlCommand::TakeoverInput, current_input);
		if (ui.NeedsTakeoverAlignment(current_input))
			RenderTakeoverAlignment(ui, current_input);
		else
			RenderTakeoverCountdown(ui);
		ImGui::End();
		return;
	}

	if (ui.takeover) {
		if (ImGui::Button(ICON_FA_ROTATE_LEFT "  Retry Takeover", ScaledVec2(300, 40))) {
			ui_commands_.emplace_back(ReplayCtrlCommand::RetryTakeover);
		}
		if (ImGui::Button(ICON_FA_BACKWARD "  Back to Pause Menu", ScaledVec2(300, 40))) {
			ui_commands_.emplace_back(ReplayCtrlCommand::ReturnToReplay);
		}
	} else {
		// Not in live: takeover disables the pacing trim, the sync barrier, the
		// intake cap and the starve guard.
		if (!ui.liveMode) {
			ImGui::BeginDisabled(!ui.inGame);
			if (ImGui::Button(ICON_FA_GAMEPAD "  Take Over", ScaledVec2(300, 40))) {
				ui_commands_.emplace_back(ReplayCtrlCommand::TakeOver);
			}
			ImGui::EndDisabled();
		}

		// Round control: buttons
		if (ui.canChangeRound) {
			ImGui::Separator();

			const int totalRounds = ui.roundCount;

			ImGui::BeginGroup();
			float buttonHeight = uiScaled(40.0f);
			float textHeight = ImGui::GetFontSize();
			float initialY = ImGui::GetCursorPosY();

			ImGui::SetCursorPosY(initialY + (buttonHeight - textHeight) * 0.5f);
			ImGui::Text("Round");
			ImGui::SameLine();

			for (int i = 1; i <= totalRounds; i++) {
				ImGui::SetCursorPosY(initialY);
				char label[16];
				snprintf(label, sizeof(label), "%d", i);

				bool is_current = (i == ui.currentRound);
				if (is_current) {
					ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
				}

				if (ImGui::Button(label, ScaledVec2(40, 40))) {
					// A round the viewer picked, unlike the automatic jump on
					// connect, which must keep following.
					ui_commands_.emplace_back(ReplayCtrlCommand::SetRound, i);
					SDL_ShowCursor(SDL_ENABLE);
				}

				if (ImGui::IsItemHovered()) {
					ImGui::BeginTooltip();
					if (ui.startAt != 0 && i - 1 < ui.roundCount) {
						const int roundStart = ui.rounds[i - 1].startFrame;
						const int roundEnd = ui.rounds[i - 1].endFrame;
						const double inputSeconds = ui.inputSeconds;
						const int startSeconds = static_cast<int>(std::llround(roundStart * inputSeconds));
						const int endSeconds = static_cast<int>(std::llround(roundEnd * inputSeconds));
						const std::string startClock = formatLocalClock(static_cast<time_t>(ui.startAt + startSeconds));
						const std::string endClock = formatLocalClock(static_cast<time_t>(ui.startAt + endSeconds));
						if (!startClock.empty() && !endClock.empty()) {
							ImGui::Text("Estimated Time: %s ~ %s", startClock.c_str(), endClock.c_str());
							ImGui::Separator();
						}
					}
					if (ui.rounds[i - 1].hasResult) {
						const auto& rd = ui.rounds[i - 1];
						if (rd.winTeam == 1) {
							ImGui::TextColored(ImVec4(.42f, .79f, .99f, 1), "%s", strprintf(GdxsvLanguage::gdxT("%s Wins"), GdxsvLanguage::gdxT("Federation")).c_str());
						} else if (rd.winTeam == 2) {
							ImGui::TextColored(ImVec4(.97f, .23f, .35f, 1), "%s", strprintf(GdxsvLanguage::gdxT("%s Wins"), GdxsvLanguage::gdxT("Zeon")).c_str());
						} else {
							ImGui::TextDisabled("  -  ");
						}

						if (rd.usedMsCount > 0) {
							std::string renpo_ms, zeon_ms;
							for (int j = 0; j < rd.usedMsCount; ++j) {
								if (j >= ui.userCount) break;
								int ms_id = rd.usedMs[j];
								const char* ms_name = GdxsvLanguage::GetMSName(ms_id - 1);
								std::string name = (ms_name && strlen(ms_name) > 0 ? ms_name : "Unknown");

								if (ui.userTeams[j] == 1) {
									if (!renpo_ms.empty()) renpo_ms += " ";
									renpo_ms += name;
								} else if (ui.userTeams[j] == 2) {
									if (!zeon_ms.empty()) zeon_ms += " ";
									zeon_ms += name;
								}
							}
							if (!renpo_ms.empty() && !zeon_ms.empty()) {
								ImGui::Text("%s vs %s", renpo_ms.c_str(), zeon_ms.c_str());
							} else if (!renpo_ms.empty() || !zeon_ms.empty()) {
								ImGui::Text("%s%s", renpo_ms.c_str(), zeon_ms.c_str());
							}
						}
					} else {
						ImGui::TextDisabled("  -  ");
					}
					ImGui::EndTooltip();
				}

				if (is_current) {
					ImGui::PopStyleColor();
				}

				if (i < totalRounds) {
					ImGui::SameLine();
				}
			}
			ImGui::EndGroup();
		}

		ImGui::Separator();

		OptionCheckbox("Show Ally HP", config::GdxReplayShowAllyHP, "Hack the total HP field to display Ally HP");

		// Key Display and Skip MS Selection do nothing during Live Spectate, so
		// they are not offered there rather than sitting inert.
		if (!ui.liveMode) {
			OptionCheckbox("Key Display", config::GdxReplayKeyDisplay, "Display controller inputs");
			OptionCheckbox("Skip MS Selection", config::GdxReplaySkipMsSelection,
						   "Fast-forward through the mobile suit selection screen");
		}

		ImGui::Separator();

		if (ImGui::Button(ICON_FA_PLAY "  Resume Playback", ScaledVec2(300, 40))) {
			ui_commands_.emplace_back(ReplayCtrlCommand::ResumePlayback);
			ctrl_input_release_pending_ = true;
			SDL_ShowCursor(SDL_ENABLE);
		}
		if (ImGui::Button(ICON_FA_DOOR_OPEN "  Exit Replay", ScaledVec2(300, 40))) {
			ui_commands_.emplace_back(ReplayCtrlCommand::ExitReplay);
		}
	}

	ImGui::End();
}

void GdxsvBackendReplay::RenderTakeoverAlignment(const UiState& ui, u16 current_input) {
	ImGui::TextUnformatted("Match replay input");
	ImGui::Separator();
	ImGui::Text("Replay: %s", formatMcsInput(ui.takeoverTargetInput).c_str());
	ImGui::Text("Current: %s", formatMcsInput(current_input).c_str());
	ImGui::Dummy(ScaledVec2(0, 8));
	if (ImGui::Button(ICON_FA_FORWARD "  Skip Matching", ScaledVec2(300, 40))) {
		ui_commands_.emplace_back(ReplayCtrlCommand::SkipTakeoverAlignment);
	}
	if (ImGui::Button(ICON_FA_XMARK "  Cancel", ScaledVec2(300, 40))) {
		ui_commands_.emplace_back(ReplayCtrlCommand::CancelTakeover);
	}
	ImGui::Dummy(ScaledVec2(0, 8));
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("During Takeover, press START to retry, or select \"Retry Takeover\" from the pause menu.");
	ImGui::PopTextWrapPos();
}

void GdxsvBackendReplay::RenderTakeoverCountdown(const UiState& ui) {
	float progress = 1.0f - (float)ui.takeoverCountdown / 60.0f;
	float radius = 30.0f * ImGui::GetIO().FontGlobalScale;

	ImGui::Dummy(ScaledVec2(0, 10));
	ImGui::Dummy(ScaledVec2(0, radius * 2));

	ImVec2 pie_center = ImGui::GetWindowPos();
	pie_center.x += ImGui::GetWindowSize().x * 0.5f;
	pie_center.y += ImGui::GetCursorPosY() - radius - ImGui::GetStyle().ItemSpacing.y;

	ImDrawList* draw_list = ImGui::GetWindowDrawList();

	// Background circle
	draw_list->AddCircleFilled(pie_center, radius, IM_COL32(255, 255, 255, 40), 30);

	// Pie fill
	float start_angle = -IM_PI * 0.5f;
	float end_angle = start_angle + progress * IM_PI * 2.0f;
	if (progress > 0.0f) {
		draw_list->PathLineTo(pie_center);
		draw_list->PathArcTo(pie_center, radius, start_angle, end_angle, 30);
		draw_list->PathLineTo(pie_center);
		draw_list->PathFillConvex(IM_COL32(255, 255, 255, 200));
	}

	// "Get Ready!" text
	ImGui::Dummy(ScaledVec2(0, 5));
	{
		auto w = ImGui::GetWindowSize().x;
		auto tw = ImGui::CalcTextSize("Get Ready!").x;
		ImGui::SetCursorPosX((w - tw) * 0.5f);
		ImGui::Text("Get Ready!");
	}
	ImGui::Dummy(ScaledVec2(0, 10));
}

void GdxsvBackendReplay::GetRoundReplayBounds(int& roundStart, int& roundEnd, int& totalRounds) const {
	if (log_file_.start_msg_indexes_size() == 0) {
		roundStart = 0;
		roundEnd = log_file_.inputs_size();
		totalRounds = 0;
		return;
	}

	totalRounds = log_file_.start_msg_indexes_size();
	int round = start_msg_count_;
	if (round < 1) round = 1;
	if (round > totalRounds) round = totalRounds;

	roundStart = log_file_.start_msg_indexes(round - 1);
	if (round < totalRounds) {
		roundEnd = log_file_.start_msg_indexes(round);
	} else {
		roundEnd = log_file_.inputs_size();
	}
}

void GdxsvBackendReplay::GetControlTimelineBounds(int& timelineStart, int& timelineEnd, int& totalRounds) const {
	GetRoundReplayBounds(timelineStart, timelineEnd, totalRounds);

	// Live never skips MS selection, so trimming the timeline to the briefing
	// would leave the scrubber disagreeing with what is on screen.
	if (config::GdxReplaySkipMsSelection && !live_mode_ && briefing_start_frame_ > timelineStart &&
		briefing_start_frame_ < timelineEnd && briefing_start_frame_round_ == start_msg_count_) {
		timelineStart = briefing_start_frame_;
	}
}

const char* GdxsvBackendReplay::SpeedText(int speed) {
	switch (speed) {
		case -2: return "33%";
		case -1: return "50%";
		case  0: return "100%";
		case  1: return "200%";
		case  2: return "300%";
		default: return "100%";
	}
}

void GdxsvBackendReplay::UpdateControlBarVisibility(const UiState& ui) {
	if (ui.state < State::McsInBattle) return;
	if (ui.takeover) return;

	ImGuiIO& io = ImGui::GetIO();

	// Map analog stick to d-pad (same as OnMainUiLoop) so stick input triggers visibility
	u32 cur_kcode = mapleInputState[0].kcode;
	auto axes = mapleInputState[0].fullAxes;
	if ((axes[0] >> 8) + 128 <= 128 - 0x20) cur_kcode &= ~DC_DPAD_LEFT;
	if ((axes[0] >> 8) + 128 >= 128 + 0x20) cur_kcode &= ~DC_DPAD_RIGHT;
	if ((axes[1] >> 8) + 128 <= 128 - 0x20) cur_kcode &= ~DC_DPAD_UP;
	if ((axes[1] >> 8) + 128 >= 128 + 0x20) cur_kcode &= ~DC_DPAD_DOWN;
	bool holding_lr = (~cur_kcode & DC_DPAD_LEFT) || (~cur_kcode & DC_DPAD_RIGHT);
	if (cur_kcode != ctrl_bar_prev_kcode_ || holding_lr) {
		ctrl_bar_idle_timer_ = 3.0f;
	}
	ctrl_bar_prev_kcode_ = cur_kcode;

	if (io.MousePos.x >= 0.0f && io.MousePos.y >= 0.0f && io.MousePos.x <= io.DisplaySize.x && io.MousePos.y <= io.DisplaySize.y) {
		const bool hasPrevMouse = ctrl_bar_prev_mouse_x_ >= 0.0f && ctrl_bar_prev_mouse_y_ >= 0.0f;
		const float dx = io.MousePos.x - ctrl_bar_prev_mouse_x_;
		const float dy = io.MousePos.y - ctrl_bar_prev_mouse_y_;
		const bool mouseMoved = hasPrevMouse && dx * dx + dy * dy > 1.0f;
		if ((mouseMoved && io.MousePos.y >= io.DisplaySize.y - uiScaled(96.0f)) || ctrl_bar_dragging_) {
			ctrl_bar_idle_timer_ = 3.0f;
		}
		ctrl_bar_prev_mouse_x_ = io.MousePos.x;
		ctrl_bar_prev_mouse_y_ = io.MousePos.y;
	}

	float dt = io.DeltaTime;
	ctrl_bar_idle_timer_ = std::max(0.0f, ctrl_bar_idle_timer_ - dt);

	// The bar only shows for a few seconds, so refresh the viewer count as it
	// appears rather than whenever the interval happens to come round.
	const bool visible_now = 0.001f < ctrl_bar_visibility_;
	if (visible_now && !ctrl_bar_was_visible_) {
		live_viewers_stale_ = true;
	}
	ctrl_bar_was_visible_ = visible_now;

	constexpr float fadeSpeed = 4.0f;
	if (ctrl_bar_idle_timer_ > 0.0f) {
		ctrl_bar_visibility_ = std::min(1.0f, ctrl_bar_visibility_ + dt * fadeSpeed);
	} else {
		ctrl_bar_visibility_ = std::max(0.0f, ctrl_bar_visibility_ - dt * fadeSpeed);
	}
}

void GdxsvBackendReplay::RenderControlBar(const UiState& ui) {
	if (ui.state < State::McsInBattle) return;
	if (ui.takeover) return;
	if (ctrl_bar_visibility_ <= 0.001f) return;

	const float alpha = ctrl_bar_visibility_;
	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	if (displaySize.x <= 0 || displaySize.y <= 0) return;

	// Bar dimensions
	const float barH = uiScaled(36.0f);
	const float rounding = uiScaled(8.0f);
	const float pad = uiScaled(8.0f);

	// Live Spectate puts a Live pill to the left of the bar, as its own rounded
	// rect, so the two read as separate controls.
	const float liveGap = uiScaled(8.0f);
	const float liveDotR = uiScaled(4.0f);
	const float liveW = ui.liveMode ? (pad + liveDotR * 2.0f + pad * 0.75f + ImGui::CalcTextSize("Live").x + pad) : 0.0f;

	// Viewer count on the right, so the bar sits centred between two pills
	// rather than being pushed off-centre by the Live one.
	live_viewers_ = ui.liveMode ? gdxsv_live_viewer_count(ui.battleCode, live_viewers_stale_) : 0;
	live_viewers_stale_ = false;
	char viewersProbe[32] = {};
	snprintf(viewersProbe, sizeof(viewersProbe), ICON_FA_EYE " %d", live_viewers_);
	const float viewersW = ui.liveMode ? (pad + ImGui::CalcTextSize(viewersProbe).x + pad) : 0.0f;

	const float sideW = ui.liveMode ? (liveW + liveGap + viewersW + liveGap) : 0.0f;
	const float barW = displaySize.x * 0.85f - sideW;
	const float totalW = barW + sideW;
	const float totalX = (displaySize.x - totalW) * 0.5f;
	const float liveX = totalX;
	const float barX = totalX + (ui.liveMode ? liveW + liveGap : 0.0f);
	const float viewersX = barX + barW + liveGap;
	const float barY = displaySize.y - barH - uiScaled(20.0f);

	// Create a transparent overlay window. The drawn progress track has an invisible hit target for dragging.
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::SetNextWindowPos(ImVec2(totalX, barY));
	ImGui::SetNextWindowSize(ImVec2(totalW, barH));
	ImGui::SetNextWindowBgAlpha(0.0f);
	ImGui::Begin("##gdxsv-replay-ctrlbar", nullptr,
				 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
				 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoFocusOnAppearing |
				 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);

	ImDrawList* dl = ImGui::GetWindowDrawList();

	// Background rounded rect
	const ImU32 bgCol = IM_COL32(0, 0, 0, (int)(180 * alpha));
	dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH), bgCol, rounding);

	// Decay flash timers
	float dt = ImGui::GetIO().DeltaTime;
	flash_left_ = std::max(0.0f, flash_left_ - dt);
	flash_right_ = std::max(0.0f, flash_right_ - dt);
	flash_up_ = std::max(0.0f, flash_up_ - dt);
	flash_down_ = std::max(0.0f, flash_down_ - dt);

	// Color helpers
	const ImU32 textCol = IM_COL32(255, 255, 255, (int)(255 * alpha));
	auto flashCol = [&](float flash) -> ImU32 {
		float t = flash / 0.3f; // 1.0 -> 0.0
		int brightness = 80 + (int)(175 * t);
		return IM_COL32(255, 255, 255, (int)(brightness * alpha));
	};
	const ImU32 disabledCol = IM_COL32(120, 120, 120, (int)(120 * alpha));

	auto iconButton = [&](const char* id, ImVec2 center, ImVec2 hitSize, bool enabled) -> bool {
		ImGui::SetCursorScreenPos(center - hitSize * 0.5f);
		ImGui::InvisibleButton(id, hitSize);
		if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
			ctrl_bar_idle_timer_ = 3.0f;
		}
		return enabled && ImGui::IsItemClicked(ImGuiMouseButton_Left);
	};

	// Draw above the bar without clipping to its window or taking mouse focus.
	auto hoverOverlay = [&](const std::string& text, float anchorX) {
		ctrl_bar_idle_timer_ = 3.0f;
		const ImVec2 padding = ScaledVec2(8.0f, 5.0f);
		const float wrapWidth = std::max(1.0f, displaySize.x - pad * 2.0f - padding.x * 2.0f);
		const ImVec2 size = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrapWidth) + padding * 2.0f;
		const float x = std::clamp(anchorX - size.x * 0.5f, pad, std::max(pad, displaySize.x - pad - size.x));
		const float y = std::max(pad, barY - uiScaled(8.0f) - size.y);
		const ImVec2 pos(x, y);
		ImDrawList* overlay = ImGui::GetForegroundDrawList();
		overlay->AddRectFilled(pos, pos + size, IM_COL32(20, 20, 20, (int)(235 * alpha)), uiScaled(4.0f));
		overlay->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos + padding, textCol, text.c_str(), nullptr, wrapWidth);
	};
	auto controlTooltip = [&](const char* label, const char* button = nullptr, const char* detail = nullptr) {
		if (!ImGui::IsItemHovered() && !ImGui::IsItemActive()) return;
		std::string text = label;
		if (button != nullptr) text += std::string(" (") + button + ")";
		if (detail != nullptr) text += std::string("\n") + detail;
		hoverOverlay(text, (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f);
	};

	// Live pill: a dot and the word Live, red while chasing the live edge and
	// grey once the viewer has moved somewhere else. Clicking it resumes the
	// chase - the catch-up seek does the actual travelling.
	if (ui.liveMode) {
		dl->AddRectFilled(ImVec2(liveX, barY), ImVec2(liveX + liveW, barY + barH), bgCol, rounding);

		const ImU32 liveOn = IM_COL32(255, 60, 60, (int)(255 * alpha));
		const ImU32 liveOff = IM_COL32(150, 150, 150, (int)(200 * alpha));
		const ImU32 dotCol = ui.liveAtEdge ? liveOn : liveOff;

		const float dotX = liveX + pad + liveDotR;
		dl->AddCircleFilled(ImVec2(dotX, barY + barH * 0.5f), liveDotR, dotCol);

		const ImVec2 liveTextSize = ImGui::CalcTextSize("Live");
		dl->AddText(ImVec2(dotX + liveDotR + pad * 0.75f, barY + (barH - liveTextSize.y) * 0.5f),
					ui.liveAtEdge ? textCol : liveOff, "Live");

		if (iconButton("##gdxsv-live", ImVec2(liveX + liveW * 0.5f, barY + barH * 0.5f), ImVec2(liveW, barH), !ui.liveAtEdge)) {
			ui_commands_.emplace_back(ReplayCtrlCommand::FollowLive);
		}
		if (!ui.liveAtEdge)
			controlTooltip("Skip ahead to live");
	}

	// Viewer count on the right, balancing the Live pill on the left.
	if (ui.liveMode && 0 < viewersW) {
		dl->AddRectFilled(ImVec2(viewersX, barY), ImVec2(viewersX + viewersW, barY + barH), bgCol, rounding);
		char viewersTxt[32] = {};
		snprintf(viewersTxt, sizeof(viewersTxt), ICON_FA_EYE " %d", live_viewers_);
		const ImVec2 vSize = ImGui::CalcTextSize(viewersTxt);
		dl->AddText(ImVec2(viewersX + (viewersW - vSize.x) * 0.5f, barY + (barH - vSize.y) * 0.5f), textCol, viewersTxt);
		iconButton("##gdxsv-viewers", ImVec2(viewersX + viewersW * 0.5f, barY + barH * 0.5f), ImVec2(viewersW, barH), false);
		char viewersHint[48];
		snprintf(viewersHint, sizeof(viewersHint), "%d watching now", live_viewers_);
		controlTooltip(viewersHint);
	}

	// Layout positions
	float cx = barX + pad;
	const float cy = barY + barH * 0.5f;

	// --- Left: Play/Pause icon + Up/Down guide + speed + Left/Right guide ---
	const char* stateIcon = ui.paused ? ICON_FA_PLAY : ICON_FA_PAUSE;
	ImVec2 iconSize = ImGui::CalcTextSize(stateIcon);
	// Fixed, non-overlapping hit slots keep both the icons and hover targets stable.
	float iconSlotW = std::max({ImGui::CalcTextSize(ICON_FA_PLAY).x, ImGui::CalcTextSize(ICON_FA_PAUSE).x, uiScaled(20.0f)});
	float iconOfs = (iconSlotW - iconSize.x) * 0.5f;
	const ImVec2 stateIconPos(cx + iconOfs, cy - iconSize.y * 0.5f);
	dl->AddText(stateIconPos, textCol, stateIcon);
	if (iconButton("##gdxsv-play-pause", ImVec2(cx + iconSlotW * 0.5f, cy), ImVec2(iconSlotW, barH), true)) {
		ui_commands_.emplace_back(ReplayCtrlCommand::TogglePause);
	}
	controlTooltip(ui.paused ? "Play" : "Pause", "A");
	cx += iconSlotW + uiScaled(6.0f);

	// Up/Down guide icons (for speed control)
	ImVec2 udSize = ImGui::CalcTextSize(ICON_FA_ANGLE_UP);
	const float udSlotW = std::max(udSize.x, uiScaled(20.0f));
	const bool canSpeedUp = ui.playSpeed < 2;
	const bool canSpeedDown = ui.playSpeed > -2;
	const ImVec2 upCenter(cx + udSlotW * 0.5f, barY + barH * 0.25f);
	const ImVec2 downCenter(cx + udSlotW * 0.5f, barY + barH * 0.75f);
	const ImVec2 upPos = upCenter - udSize * 0.5f;
	const ImVec2 downPos = downCenter - udSize * 0.5f;
	dl->AddText(upPos, canSpeedUp ? flashCol(flash_up_) : disabledCol, ICON_FA_ANGLE_UP);
	dl->AddText(downPos, canSpeedDown ? flashCol(flash_down_) : disabledCol, ICON_FA_ANGLE_DOWN);
	if (iconButton("##gdxsv-speed-up", upCenter, ImVec2(udSlotW, barH * 0.5f), canSpeedUp)) {
		ui_commands_.emplace_back(ReplayCtrlCommand::NextSpeed, 1);
		flash_up_ = 0.3f;
	}
	controlTooltip(ICON_FA_ARROW_UP " Speed", "D-pad Up");
	if (iconButton("##gdxsv-speed-down", downCenter, ImVec2(udSlotW, barH * 0.5f), canSpeedDown)) {
		ui_commands_.emplace_back(ReplayCtrlCommand::NextSpeed, -1);
		flash_down_ = 0.3f;
	}
	controlTooltip(ICON_FA_ARROW_DOWN " Speed", "D-pad Down");
	cx += udSlotW + uiScaled(4.0f);

	const char* speedTxt = SpeedText(ui.playSpeed);
	ImVec2 speedSize = ImGui::CalcTextSize(speedTxt);
	dl->AddText(ImVec2(cx, cy - speedSize.y * 0.5f), textCol, speedTxt);
	iconButton("##gdxsv-current-speed", ImVec2(cx + speedSize.x * 0.5f, cy), ImVec2(speedSize.x, barH), false);
	controlTooltip("Current speed");
	cx += speedSize.x + uiScaled(10.0f);

	// Left/Right guide icons (for seek/step) - placed next to each other
	ImVec2 lrSize = ImGui::CalcTextSize(ICON_FA_ANGLE_LEFT);
	const float lrSlotW = std::max(lrSize.x, uiScaled(20.0f));
	const ImVec2 leftCenter(cx + lrSlotW * 0.5f, cy);
	const ImVec2 leftPos = leftCenter - lrSize * 0.5f;
	char backwardLabel[32], forwardLabel[32], seekDetail[64];
	if (ui.paused) {
		snprintf(backwardLabel, sizeof(backwardLabel), "- 1 frame");
		snprintf(forwardLabel, sizeof(forwardLabel), "+ 1 frame");
	} else {
		snprintf(backwardLabel, sizeof(backwardLabel), "- %.0fs", ReplaySeekFrames / 59.94);
		snprintf(forwardLabel, sizeof(forwardLabel), "+ %.0fs", ReplaySeekFrames / 59.94);
	}
	snprintf(seekDetail, sizeof(seekDetail), "%d frames; hold D-pad to repeat", ReplaySeekFrames);
	dl->AddText(leftPos, flashCol(flash_left_), ICON_FA_ANGLE_LEFT);
	if (iconButton("##gdxsv-prev", leftCenter, ImVec2(lrSlotW, barH), true)) {
		ui_commands_.emplace_back(ui.paused ? ReplayCtrlCommand::StepFrameBackward : ReplayCtrlCommand::SeekBackward);
		flash_left_ = 0.3f;
	}
	controlTooltip(backwardLabel, "D-pad Left", ui.paused ? "Hold D-pad to repeat" : seekDetail);
	cx += lrSlotW + uiScaled(2.0f);
	const ImVec2 rightCenter(cx + lrSlotW * 0.5f, cy);
	const ImVec2 rightPos = rightCenter - lrSize * 0.5f;
	dl->AddText(rightPos, flashCol(flash_right_), ICON_FA_ANGLE_RIGHT);
	if (iconButton("##gdxsv-next", rightCenter, ImVec2(lrSlotW, barH), true)) {
		ui_commands_.emplace_back(ui.paused ? ReplayCtrlCommand::StepFrame : ReplayCtrlCommand::SeekForward);
		flash_right_ = 0.3f;
	}
	controlTooltip(forwardLabel, "D-pad Right", ui.paused ? "Hold D-pad to repeat" : seekDetail);
	cx += lrSlotW + uiScaled(6.0f);

	// --- Right: Round/Frame info ---
	const int timelineStart = ui.timelineStart;
	const int timelineEnd = ui.timelineEnd;
	const int totalRounds = ui.roundCount;

	const int timelineLen = timelineEnd - timelineStart;
	const int displayFrame = ctrl_bar_drag_target_frame_ >= 0 ? ctrl_bar_drag_target_frame_ : ui.playbackFrame;
	const int displayPosInTimeline = displayFrame - timelineStart;

	char rbuf[128];
	if (totalRounds > 0) {
		snprintf(rbuf, sizeof(rbuf), "Round %d/%d  %d/%d fr", ui.currentRound, totalRounds, displayFrame, ui.inputCount);
	} else {
		snprintf(rbuf, sizeof(rbuf), "%d/%d fr", displayFrame, ui.inputCount);
	}

	ImVec2 rSize = ImGui::CalcTextSize(rbuf);
	const float rightTotalW = rSize.x + pad;
	float rx = barX + barW - rightTotalW;
	dl->AddText(ImVec2(rx, cy - rSize.y * 0.5f), textCol, rbuf);

	// --- Center: Progress bar ---
	const float progX0 = cx;
	const float progX1 = rx - uiScaled(10.0f);
	const float progH = uiScaled(6.0f);
	const float progY0 = cy - progH * 0.5f;
	const float progY1 = cy + progH * 0.5f;

	if (progX1 > progX0 + uiScaled(20.0f)) {
		const float progW = progX1 - progX0;
		auto frameFromProgressX = [&](float x) -> int {
			return ui.FrameAtProgress((x - progX0) / progW);
		};

		const float hitH = uiScaled(24.0f);
		ImGui::SetCursorScreenPos(ImVec2(progX0, cy - hitH * 0.5f));
		ImGui::InvisibleButton("##gdxsv-replay-progress-slider", ImVec2(progW, hitH));
		if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
			ctrl_bar_idle_timer_ = 3.0f;
			char targetFrame[32];
			snprintf(targetFrame, sizeof(targetFrame), "Frame %d", frameFromProgressX(ImGui::GetIO().MousePos.x));
			hoverOverlay(targetFrame, std::clamp(ImGui::GetIO().MousePos.x, progX0, progX1));
		}
		if (ImGui::IsItemActivated() || ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
			ctrl_bar_dragging_ = true;
			ctrl_bar_drag_target_frame_ = frameFromProgressX(ImGui::GetIO().MousePos.x);
		}

		if (ctrl_bar_dragging_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			ctrl_bar_drag_target_frame_ = frameFromProgressX(ImGui::GetIO().MousePos.x);
			ctrl_bar_idle_timer_ = 3.0f;
		}

		if (ctrl_bar_dragging_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
			if (ctrl_bar_drag_target_frame_ >= 0 && ctrl_bar_drag_target_frame_ != ui.playbackFrame) {
				// Going somewhere on purpose stops the chase, or the catch-up
				// would drag the viewer straight back to the live edge.
				ui_commands_.emplace_back(ReplayCtrlCommand::JumpToKeyMsg, ctrl_bar_drag_target_frame_);
			} else {
				ctrl_bar_drag_target_frame_ = -1;
			}
			ctrl_bar_dragging_ = false;
		}

		// Track background
		const ImU32 trackCol = IM_COL32(80, 80, 80, (int)(180 * alpha));
		dl->AddRectFilled(ImVec2(progX0, progY0), ImVec2(progX1, progY1), trackCol, progH * 0.5f);

		// Fill
		float progress = (timelineLen > 0) ? std::clamp((float)displayPosInTimeline / (float)timelineLen, 0.0f, 1.0f) : 0.0f;
		float fillX = progX0 + (progX1 - progX0) * progress;
		const ImU32 fillCol = IM_COL32(60, 130, 230, (int)(220 * alpha));
		dl->AddRectFilled(ImVec2(progX0, progY0), ImVec2(fillX, progY1), fillCol, progH * 0.5f);

		// Playhead dot
		const float dotR = uiScaled(5.0f);
		const ImU32 dotCol = IM_COL32(255, 255, 255, (int)(240 * alpha));
		dl->AddCircleFilled(ImVec2(fillX, cy), dotR, dotCol);
	}

	ImGui::End();
	ImGui::PopStyleVar(3);
}
