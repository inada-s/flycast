#include "gdxsv_backend_replay.h"

#include <cstdlib>
#include <sstream>

#include "SDL_events.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "emulator.h"
#include "ui/mainui.h"
#include "gdx_rpc.h"
#include "gdxsv.h"
#include "gdxsv_replay_util.h"
#include "input/gamepad_device.h"
#include "json.hpp"
#include "libs.h"
#include "ui/gui.h"
#include "ui/gui_util.h"
#include "ui/IconsFontAwesome6.h"
#include "sdl/sdl.h"


using namespace std::chrono;

namespace {
// maple input to mcs pad input (same as gdxsv_backend_rollback.cpp)
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
	if (~input.kcode & (DC_BTN_BITMAPPED_LAST << 1)) r |= McsKeyCode::LT;
	if (~input.kcode & (DC_BTN_BITMAPPED_LAST << 2)) r |= McsKeyCode::RT;
	if ((input.fullAxes[0] >> 8) + 128 <= 128 - 0x20) r |= McsKeyCode::LEFT;
	if ((input.fullAxes[0] >> 8) + 128 >= 128 + 0x20) r |= McsKeyCode::RIGHT;
	if ((input.fullAxes[1] >> 8) + 128 <= 128 - 0x20) r |= McsKeyCode::UP;
	if ((input.fullAxes[1] >> 8) + 128 >= 128 + 0x20) r |= McsKeyCode::DOWN;
	return r;
}
}  // namespace

void GdxsvBackendReplay::Reset() {
	state_ = State::None;
	ctrl_commands_.clear();
	lbs_tx_reader_.Clear();
	log_file_.Clear();
	recv_buf_.clear();
	pov_ = 0;
	key_msg_count_ = 0;
	start_msg_count_ = 0;
	round_start_frame_ = 0;
	recv_delay_ = 0;
	end_of_frame_ = false;
	seeking_ = false;
	pending_round_ = 0;
	pause_menu_opend_ = false;
	lbs_first_skip_ = false;
	ctrl_play_speed_ = 0;
	ctrl_step_frame_ = false;
	ctrl_pause_ = false;
	save_converted_log_ = false;
	ctrl_bar_visibility_ = 0.0f;
	ctrl_bar_idle_timer_ = 0.0f;
	ctrl_bar_prev_kcode_ = ~0u;
	audio_fade_frames_ = 0;
	step_hold_timer_ = 0.0f;
	flash_left_ = 0.0f;
	flash_right_ = 0.0f;
	flash_up_ = 0.0f;
	flash_down_ = 0.0f;
	settings.aica.audioFade = 1.0f;
	takeover_ = false;
	takeover_saved_frame_ = -1;
	takeover_countdown_ = 0;
	takeover_input_buf_.clear();
	round_data_.clear();
	prev_win_team_ = 0;
	gdxsv_save_state.Reset();
	gdxsv.key_display_.Clear();
}

void GdxsvBackendReplay::OnMainUiLoop() {
	if (state_ == State::End) {
		if (IsBatchReplay()) {
			OutputBatchResult();
		}
		state_ = State::None;
		gdxsv_save_state.Reset();
		gdxsv.netmode_ = Gdxsv::NetMode::Offline;
		gdxsv_end_replay();
		return;
	}

	if (state_ <= State::LbsStartBattleFlow) {
		static int counter = 0;
		if (++counter % 10 < 5)
			kcode[0] = ~DC_BTN_A;
		else
			kcode[0] = ~0u;
		if (ctrl_commands_.empty() && !lbs_first_skip_) {
			ctrl_commands_.emplace_back(ReplayCtrlCommand::SeekForward, target_frame_ > 0 ? 1000 : 180);
			lbs_first_skip_ = true;
		}
	}

	if (state_ == State::McsWaitJoin) {
		if (ctrl_commands_.empty()) {
			if (takeover_) {
				ctrl_commands_.emplace_back(ReplayCtrlCommand::RetryTakeover);
			} else {
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
				} else {
					ctrl_commands_.emplace_back(ReplayCtrlCommand::SeekToBriefing);
				}
			}
		}
	}

	if (State::LbsStartBattleFlow <= state_ && !pause_menu_opend_ && !IsBatchReplay()) {
		auto input = mapleInputState[0];
		// Map analog stick to d-pad (fullAxes are 16-bit, >> 8 to match convertInput thresholds)
		if ((input.fullAxes[0] >> 8) + 128 <= 128 - 0x20) input.kcode &= ~DC_DPAD_LEFT;
		if ((input.fullAxes[0] >> 8) + 128 >= 128 + 0x20) input.kcode &= ~DC_DPAD_RIGHT;
		if ((input.fullAxes[1] >> 8) + 128 <= 128 - 0x20) input.kcode &= ~DC_DPAD_UP;
		if ((input.fullAxes[1] >> 8) + 128 >= 128 + 0x20) input.kcode &= ~DC_DPAD_DOWN;

		static u32 prev_kcode = 0;
		if (prev_kcode == 0) prev_kcode = input.kcode;
		const u32 pressed = ~((input.kcode ^ prev_kcode) & ~input.kcode);

		if (input.kcode != prev_kcode) {
			if (takeover_) {
				if (~pressed & DC_BTN_START) {
					ctrl_commands_.emplace_back(ReplayCtrlCommand::RetryTakeover);
				}
			} else {
				// A: Play/Pause toggle
				if (~pressed & DC_BTN_A) {
					ctrl_commands_.emplace_back(ReplayCtrlCommand::TogglePause);
				}
				// Right: Step frame (paused) / Seek forward (playing)
				else if (~pressed & DC_DPAD_RIGHT) {
					if (ctrl_pause_) {
						ctrl_commands_.emplace_back(ReplayCtrlCommand::StepFrame);
					} else {
						ctrl_commands_.emplace_back(ReplayCtrlCommand::SeekForward);
					}
					flash_right_ = 0.3f;
				}
				// Left: Step frame backward (paused) / Seek backward (playing)
				else if (~pressed & DC_DPAD_LEFT) {
					if (ctrl_pause_) {
						ctrl_commands_.emplace_back(ReplayCtrlCommand::StepFrameBackward);
					} else {
						ctrl_commands_.emplace_back(ReplayCtrlCommand::SeekBackward);
					}
					flash_left_ = 0.3f;
				}
				// Up/Down: Speed control (ignored while Left/Right is held)
				else if ((~pressed & DC_DPAD_UP) && !(~input.kcode & DC_DPAD_LEFT) && !(~input.kcode & DC_DPAD_RIGHT)) {
					ctrl_commands_.emplace_back(ReplayCtrlCommand::NextSpeed, 1);
					flash_up_ = 0.3f;
				}
				else if ((~pressed & DC_DPAD_DOWN) && !(~input.kcode & DC_DPAD_LEFT) && !(~input.kcode & DC_DPAD_RIGHT)) {
					ctrl_commands_.emplace_back(ReplayCtrlCommand::NextSpeed, -1);
					flash_down_ = 0.3f;
				}
			}
		}
		prev_kcode = input.kcode;

		// Auto-repeat for Left/Right held
		if (!takeover_) {
			bool holding_lr = (~input.kcode & DC_DPAD_RIGHT) || (~input.kcode & DC_DPAD_LEFT);
			if (holding_lr) {
				step_hold_timer_ += ImGui::GetIO().DeltaTime;
				if (step_hold_timer_ >= 0.5f && ctrl_commands_.empty()) {
					if (~input.kcode & DC_DPAD_RIGHT) {
						if (ctrl_pause_) {
							ctrl_commands_.emplace_back(ReplayCtrlCommand::StepFrame);
						} else {
							ctrl_commands_.emplace_back(ReplayCtrlCommand::SeekForward);
						}
						flash_right_ = 0.3f;
					} else {
						if (ctrl_pause_) {
							ctrl_commands_.emplace_back(ReplayCtrlCommand::StepFrameBackward);
						} else {
							ctrl_commands_.emplace_back(ReplayCtrlCommand::SeekBackward);
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

void GdxsvBackendReplay::RunFrameSilently(bool skip_rendering) {
	settings.aica.muteAudio = true;
	settings.gdxsv.skipRenderingAddr = skip_rendering ? settings.gdxsv.skipRenderingBaseAddr : 0;
	rend_enable_renderer(false);
	seeking_ = true;
	emu.run();
	seeking_ = false;
	end_of_frame_ = false;
	settings.aica.muteAudio = false;
	settings.gdxsv.skipRenderingAddr = 0;
	rend_enable_renderer(true);
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

void GdxsvBackendReplay::OnNextFrame() {
	if (!end_of_frame_) return;
	if (seeking_) return;

	const bool batch_mode = IsBatchReplay();

	constexpr int save_interval = 60;
	auto need_cancel = [&]() -> bool { return ctrl_commands_.contains(ReplayCtrlCommand::SaveFirstFrame) || state_ == State::End; };
	auto regular_save_state = [&]() {
		if (batch_mode) return;  // Skip save states in batch mode
		if ((IsInGame() || IsInBriefing()) && gdxsv_save_state.LastSavedFrame() + save_interval <= key_msg_count_ && recv_buf_.empty() && !takeover_) {
			gdxsv_save_state.SaveState(key_msg_count_);
		}
	};

	if (!batch_mode) {
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

		gdxsv.key_display_.enabled(config::GdxReplayKeyDisplay && IsInGame());
	}
	regular_save_state();

	// Batch mode: run all remaining frames silently at maximum speed
	if (batch_mode && !need_cancel()) {
		while (state_ != State::End && !need_cancel()) {
			RunFrameSilently(true);
		}
	} else if (0 < ctrl_play_speed_ && !ctrl_pause_ && !pause_menu_opend_ && !need_cancel() && !takeover_) {
		for (int skipped_frame = 0; skipped_frame < ctrl_play_speed_; skipped_frame++) {
			RunFrameSilently(config::GdxSkipRenderingHack && skipped_frame + 1 < ctrl_play_speed_);
			regular_save_state();
			if (need_cancel()) break;
		}
	}

	ReplayCtrlCommand ctrl{};
	while (ctrl_commands_.try_get_front(ctrl)) {
		constexpr int duration = 1000;

		if (ctrl.cmd == ReplayCtrlCommand::TogglePauseMenu) {
			if (takeover_countdown_ == 0) {
				pause_menu_opend_ = !pause_menu_opend_;
				if (pause_menu_opend_) {
					if (!recv_buf_.empty()) {
						RunFrameSilently(false);
					}
					verify(recv_buf_.empty());
					gdxsv_save_state.SaveState(key_msg_count_);
					pending_round_ = start_msg_count_;
					NOTICE_LOG(COMMON, "Save Menu Opened frame %d", key_msg_count_);
				} else {
					// Apply pending round change on menu close
					if (pending_round_ != 0 && pending_round_ != start_msg_count_) {
						ctrl_commands_.emplace_back(ReplayCtrlCommand::SetRound, pending_round_);
					}
					pending_round_ = 0;
				}
				SDL_ShowCursor(pause_menu_opend_ ? SDL_ENABLE : SDL_DISABLE);
			}

			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SaveFirstFrame) {
			verify(recv_buf_.empty());
			if (!batch_mode) {
				NOTICE_LOG(COMMON, "SaveFirstFrame saved");
				gdxsv_save_state.Clear();
				gdxsv_save_state.SaveState(key_msg_count_);
			}
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SendStartMsg) {
			for (int i = 0; i < log_file_.users_size(); ++i) {
				if (i != pov_) {
					auto start_msg = McsMessage::Create(McsMessage::MsgType::StartMsg, i);
					std::copy(start_msg.body.begin(), start_msg.body.end(), std::back_inserter(recv_buf_));
				}
			}
			gdxsv.maxlag_ = 1;
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
				GetRoundBounds(roundStart, roundEnd, totalRounds);
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
					while (key_msg_count_ < goal) {
						RunFrameSilently(config::GdxSkipRenderingHack && key_msg_count_ + 1 < goal);
						regular_save_state();
						frames_run++;
						if (need_cancel() || frames_run > 1000) break;
					}
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
			const int target_key_msg_count = ctrl.arg1 ? ctrl.arg1 : 1;
			while (key_msg_count_ < target_key_msg_count) {
				RunFrameSilently(config::GdxSkipRenderingHack);
				regular_save_state();
				if (need_cancel()) break;
			}

			target_frame_ = 0;
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SeekForward) {
			const int skip_frames = 1 <= ctrl.arg1 ? ctrl.arg1 : save_interval;
			const int prev_key_msg_count = key_msg_count_;
			auto t0 = high_resolution_clock::now();
			int skipped_frame = 0;
			for (; skipped_frame < skip_frames; skipped_frame++) {
				RunFrameSilently(config::GdxSkipRenderingHack && skipped_frame + 1 < skip_frames);
				regular_save_state();
				if (need_cancel()) break;
			}
			if (0 < skipped_frame) {
				const auto ms = duration_cast<milliseconds>(high_resolution_clock::now() - t0).count();
				NOTICE_LOG(COMMON, "SeekForward skipped %d[fr] in %ld[ms] (%.2f[ms/fr]) %d->%d(%d keys)", skipped_frame, ms,
						   (float)ms / skipped_frame, prev_key_msg_count, key_msg_count_, key_msg_count_ - prev_key_msg_count);
				settings.aica.audioFade = 0.0f;
				audio_fade_frames_ = 20;
			}
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SeekToBriefing) {
			const int org_speed = ctrl_play_speed_;
			ctrl_play_speed_ = 0;

			while (!(IsInBriefing() || IsInGame() || need_cancel())) {
				RunFrameSilently(config::GdxSkipRenderingHack);
				regular_save_state();
				if (need_cancel()) break;
			}

			round_start_frame_ = key_msg_count_;
			ctrl_play_speed_ = org_speed;
			ctrl_commands_.pop_front();
			gdxsv.key_display_.Clear();

			if (target_round_ > 1) {
				ctrl_commands_.emplace_back(ReplayCtrlCommand::SetRound, target_round_);
			} else if (target_frame_ != 0) {
				ctrl_commands_.emplace_back(ReplayCtrlCommand::JumpToKeyMsg, target_frame_);
			}
		}

		if (ctrl.cmd == ReplayCtrlCommand::SeekBackward) {
			if (IsInGame()) {
				const int ahead_frame = key_msg_count_ - gdxsv_save_state.LastSavedFrame();
				int target_frame = key_msg_count_ - (60 < ahead_frame ? 0 : save_interval);
				if (gdxsv_save_state.LoadStateMostRecent(target_frame)) {
					key_msg_count_ = target_frame;
					recv_buf_.clear();
					RebuildKeyDisplay();
					settings.aica.audioFade = 0.0f;
					audio_fade_frames_ = 20;
					if (!IsInGame()) {
						EventManager::event(Event::GGPOGameEnd);
					}
				}
			}
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
			if (0 < round && round != start_msg_count_ && round - 1 < log_file_.start_msg_indexes_size() &&
				round - 1 < log_file_.start_msg_randoms_size() && gdxsv_save_state.FirstSavedFrame() != -1) {
				gdxsv_save_state.LoadState(gdxsv_save_state.FirstSavedFrame());
				key_msg_count_ = log_file_.start_msg_indexes(round - 1);
				start_msg_count_ = round;
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

				recv_buf_.clear();
				gdxsv.key_display_.Clear();
				target_round_ = 0;
				ctrl_commands_.emplace_back(ReplayCtrlCommand::SaveFirstFrame);
				ctrl_commands_.emplace_back(ReplayCtrlCommand::SendStartMsg);
				ctrl_commands_.emplace_back(ReplayCtrlCommand::SeekToBriefing);

				EventManager::event(Event::GGPOGameEnd);
			}

			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::TakeOver) {
			// SaveState has been performed when the menu opened
			gdxsv_save_state.LoadStateMostRecent(key_msg_count_);
			RebuildKeyDisplay();
			settings.aica.muteAudio = true;
			takeover_saved_frame_ = key_msg_count_;
			takeover_countdown_ = 60;
			pause_menu_opend_ = true;
			ctrl_pause_ = false;
			ctrl_play_speed_ = 0;
			recv_buf_.clear();
			NOTICE_LOG(COMMON, "TakeOver countdown at key_msg_count_:%d", key_msg_count_);
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
			takeover_input_buf_.clear();
			takeover_countdown_ = 60;
			pause_menu_opend_ = true;
			ctrl_pause_ = false;
			NOTICE_LOG(COMMON, "RetryTakeover at key_msg_count_:%d", key_msg_count_);
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::ReturnToReplay) {
			gdxsv_save_state.LoadState(takeover_saved_frame_);
			key_msg_count_ = takeover_saved_frame_;
			RebuildKeyDisplay();
			recv_buf_.clear();
			takeover_ = false;
			takeover_saved_frame_ = -1;
			pause_menu_opend_ = false;
			SDL_ShowCursor(SDL_DISABLE);
			NOTICE_LOG(COMMON, "ReturnToReplay at key_msg_count_:%d", key_msg_count_);
			ctrl_commands_.pop_front();
		}
	}
}

bool GdxsvBackendReplay::OnOpenMenu() {
	if (state_ <= State::LbsStartBattleFlow) {
		return false;
	}

	ctrl_commands_.emplace_back(ReplayCtrlCommand::TogglePauseMenu);

	return false;
}

void GdxsvBackendReplay::DisplayOSD() {
	if (IsBatchReplay()) return;
	if (!seeking_ && pause_menu_opend_) {
		RenderPauseMenu();
	}
	UpdateControlBarVisibility();
	RenderControlBar();
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
		return false;
	}

	pov_ = pov;

	return Start();
}

void GdxsvBackendReplay::Stop() {
	ctrl_commands_.clear();
	settings.gdxsv.skipRenderingAddr = 0;
	settings.aica.muteAudio = false;
	rend_enable_renderer(true);
	gdxsv_save_state.EndUsing();
	gdxsv.key_display_.enabled(false);
	state_ = State::End;

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

bool GdxsvBackendReplay::ChangeRoundAvailable() const {
	return 0 < log_file_.start_msg_indexes_size() && log_file_.start_msg_indexes_size() == log_file_.start_msg_randoms_size();
}

bool GdxsvBackendReplay::IsBatchReplay() const {
	return config::loadInt("gdxsv", "batch_replay", 0) != 0;
}

void GdxsvBackendReplay::OutputBatchResult() {
	nlohmann::json result;
	result["battle_code"] = log_file_.battle_code();
	result["disk"] = log_file_.game_disk();
	result["players"] = log_file_.users_size();
	result["rounds"] = (int)round_data_.size();

	nlohmann::json round_data_json = nlohmann::json::array();
	for (const auto& rd : round_data_) {
		nlohmann::json rd_json;
		rd_json["win_team"] = rd.win_team();
		nlohmann::json used_ms_json = nlohmann::json::array();
		for (int i = 0; i < rd.used_ms_size(); ++i) {
			used_ms_json.push_back(rd.used_ms(i));
		}
		rd_json["used_ms"] = used_ms_json;
		round_data_json.push_back(rd_json);
	}
	result["round_data"] = round_data_json;

	nlohmann::json users_json = nlohmann::json::array();
	for (int i = 0; i < log_file_.users_size(); ++i) {
		const auto& user = log_file_.users(i);
		nlohmann::json u;
		u["user_id"] = user.user_id();
		u["pos"] = user.pos();
		u["team"] = user.team();
		users_json.push_back(u);
	}
	result["users"] = users_json;

	std::string json_str = result.dump();

	// Write to file (reliable for WIN32 subsystem apps where stdout may not be connected)
	FILE* f = fopen("batch_result.json", "w");
	if (f) {
		fprintf(f, "%s\n", json_str.c_str());
		fclose(f);
	}

	// Also try stdout and log
	printf("BATCH_RESULT:%s\n", json_str.c_str());
	fflush(stdout);
	NOTICE_LOG(COMMON, "BATCH_RESULT:%s", json_str.c_str());
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

	NOTICE_LOG(COMMON, "Replay Start");
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
		round_data_.resize(start_msg_count_);
		prev_win_team_ = 0;
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
		ctrl_commands_.emplace_back(ReplayCtrlCommand::SeekToBriefing);
	} else if (msg_type == McsMessage::MsgType::ForceMsg) {
		// do nothing
	} else if (msg_type == McsMessage::MsgType::LagControlTestMsg) {
		// do nothing
	} else if (msg_type == McsMessage::MsgType::KeyMsg1) {
		verify(recv_buf_.empty());
		gdxsv.maxlag_ = 0;

		for (int i = 0; i < log_file_.users_size(); ++i) {
			u16 input = 0;
			if (takeover_ && i == pov_) {
				const int delay = config::GdxMinDelay.get();
				takeover_input_buf_.push_back(convertInput(mapleInputState[0]));
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
			if (takeover_countdown_ == 0) {
				gdxsv.key_display_.AppendInput(i, input);
			}
		}

		// Monitor WinTeam and collect used_ms (ported from rollback backend)
		if (!round_data_.empty()) {
			const int disk = gdxsv.Disk();
			const u32 WinTeam = disk == 1 ? 0x0c3364b6 : 0x0c3d1948;
			const u32 PlayerWork = disk == 1 ? 0x0c336854 : 0x0c3d1cd4;
			const u8 win_team = gdxsv_ReadMem8(WinTeam);
			if (win_team != 0 && win_team != prev_win_team_) {
				round_data_.back().set_win_team(win_team);
				round_data_.back().clear_used_ms();
				NOTICE_LOG(COMMON, "REPLAY ROUND %zu WIN_TEAM = %d", round_data_.size(), win_team);
				for (int i = 0; i < log_file_.users_size(); ++i) {
					const auto ms_index = gdxsv_ReadMem8(PlayerWork + i * 0x2000 + 0x1f02);
					round_data_.back().add_used_ms(ms_index + 1);  // 0-origin -> 1-origin
					NOTICE_LOG(COMMON, "REPLAY %d USED MS = %d", i, ms_index + 1);
				}
				prev_win_team_ = win_team;
			}
		}

		if (key_msg_count_ < log_file_.inputs_size()) {
			++key_msg_count_;
			if (key_msg_count_ == log_file_.inputs_size() && !takeover_) {
				Stop();
			}
		}

		if (!takeover_ && ctrl_play_speed_ < 0) {
			recv_delay_ = -ctrl_play_speed_;
		}
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

void GdxsvBackendReplay::RenderPauseMenu() {
	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
	ImguiStyleVar _1(ImGuiStyleVar_WindowBorderSize, 0);
	centerNextWindow();
	ImGui::SetNextWindowSize(ScaledVec2(320, 0));
	ImGui::SetNextWindowBgAlpha(0.9f);

	ImGui::Begin("##gdxsv-replay-pause", NULL,
				 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);

	if (takeover_countdown_ > 0) {
		takeover_countdown_--;
		takeover_input_buf_.push_back(convertInput(mapleInputState[0]));
		if (takeover_countdown_ == 0) {
			ctrl_commands_.emplace_back(ReplayCtrlCommand::StartTakeover);
		} else {
			RenderTakeoverCountdown();
		}
		ImGui::End();
		return;
	}

	if (takeover_) {
		if (ImGui::Button(ICON_FA_ROTATE_LEFT "  Retry Takeover", ScaledVec2(300, 40))) {
			ctrl_commands_.emplace_back(ReplayCtrlCommand::RetryTakeover);
		}
		if (ImGui::Button(ICON_FA_BACKWARD "  Return to Replay", ScaledVec2(300, 40))) {
			ctrl_commands_.emplace_back(ReplayCtrlCommand::ReturnToReplay);
		}
		if (ImGui::Button(ICON_FA_DOOR_OPEN "  Exit", ScaledVec2(300, 40))) {
			pause_menu_opend_ = false;
			Stop();
		}
	} else {
		bool canTakeOver = IsInGame();
		ImGui::BeginDisabled(!canTakeOver);
		if (ImGui::Button(ICON_FA_GAMEPAD "  Take Over", ScaledVec2(300, 40))) {
			ctrl_commands_.emplace_back(ReplayCtrlCommand::TakeOver);
		}
		ImGui::EndDisabled();

		// Round control: slider
		if (ChangeRoundAvailable()) {
			ImGui::Separator();

			int roundStart, roundEnd, totalRounds;
			GetRoundBounds(roundStart, roundEnd, totalRounds);

			ImGui::AlignTextToFramePadding();
			ImGui::Text("Round");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			ImGui::SliderInt("##round", &pending_round_, 1, totalRounds, "%d");
		}

		ImGui::Separator();

		OptionCheckbox("Show Ally HP", config::GdxReplayShowAllyHP, "Hack the total HP field to display Ally HP");
		OptionCheckbox("Key Display", config::GdxReplayKeyDisplay, "Display controller inputs");

		ImGui::Separator();

		if (ImGui::Button(ICON_FA_DOOR_OPEN "  Exit", ScaledVec2(300, 40))) {
			pause_menu_opend_ = false;
			Stop();
		}
	}

	ImGui::End();
}

void GdxsvBackendReplay::RenderTakeoverCountdown() {
	float progress = 1.0f - (float)takeover_countdown_ / 60.0f;
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

void GdxsvBackendReplay::GetRoundBounds(int& roundStart, int& roundEnd, int& totalRounds) const {
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

	if (round_start_frame_ > 0) {
		roundStart = round_start_frame_;
	} else {
		roundStart = log_file_.start_msg_indexes(round - 1);
	}
	if (round < totalRounds) {
		roundEnd = log_file_.start_msg_indexes(round);
	} else {
		roundEnd = log_file_.inputs_size();
	}
}

const char* GdxsvBackendReplay::SpeedText() const {
	switch (ctrl_play_speed_) {
		case -2: return "33%";
		case -1: return "50%";
		case  0: return "100%";
		case  1: return "200%";
		case  2: return "300%";
		default: return "100%";
	}
}

void GdxsvBackendReplay::UpdateControlBarVisibility() {
	if (state_ < State::McsInBattle) return;
	if (takeover_) return;

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

	float dt = ImGui::GetIO().DeltaTime;
	ctrl_bar_idle_timer_ = std::max(0.0f, ctrl_bar_idle_timer_ - dt);

	constexpr float fadeSpeed = 4.0f;
	if (ctrl_bar_idle_timer_ > 0.0f) {
		ctrl_bar_visibility_ = std::min(1.0f, ctrl_bar_visibility_ + dt * fadeSpeed);
	} else {
		ctrl_bar_visibility_ = std::max(0.0f, ctrl_bar_visibility_ - dt * fadeSpeed);
	}
}

void GdxsvBackendReplay::RenderControlBar() {
	if (state_ < State::McsInBattle) return;
	if (takeover_) return;
	if (ctrl_bar_visibility_ <= 0.001f) return;

	const float alpha = ctrl_bar_visibility_;
	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	if (displaySize.x <= 0 || displaySize.y <= 0) return;

	// Bar dimensions
	const float barH = uiScaled(36.0f);
	const float barW = displaySize.x * 0.85f;
	const float barX = (displaySize.x - barW) * 0.5f;
	const float barY = displaySize.y - barH - uiScaled(20.0f);
	const float rounding = uiScaled(8.0f);
	const float pad = uiScaled(8.0f);

	// Create a transparent, non-interactive overlay window
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::SetNextWindowPos(ImVec2(barX, barY));
	ImGui::SetNextWindowSize(ImVec2(barW, barH));
	ImGui::SetNextWindowBgAlpha(0.0f);
	ImGui::Begin("##gdxsv-replay-ctrlbar", nullptr,
				 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
				 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoFocusOnAppearing |
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

	// Layout positions
	float cx = barX + pad;
	const float cy = barY + barH * 0.5f;

	// --- Left: Play/Pause icon + Up/Down guide + speed + Left/Right guide ---
	const char* stateIcon = ctrl_pause_ ? ICON_FA_PAUSE : ICON_FA_PLAY;
	ImVec2 iconSize = ImGui::CalcTextSize(stateIcon);
	// Fixed-width slot for play/pause icon to prevent layout shift
	float iconSlotW = std::max(ImGui::CalcTextSize(ICON_FA_PLAY).x, ImGui::CalcTextSize(ICON_FA_PAUSE).x);
	float iconOfs = (iconSlotW - iconSize.x) * 0.5f;
	dl->AddText(ImVec2(cx + iconOfs, cy - iconSize.y * 0.5f), textCol, stateIcon);
	cx += iconSlotW + uiScaled(10.0f);

	// Up/Down guide icons (for speed control)
	ImVec2 udSize = ImGui::CalcTextSize(ICON_FA_ANGLE_UP);
	dl->AddText(ImVec2(cx, cy - udSize.y - uiScaled(1.0f)), flashCol(flash_up_), ICON_FA_ANGLE_UP);
	dl->AddText(ImVec2(cx, cy + uiScaled(1.0f)), flashCol(flash_down_), ICON_FA_ANGLE_DOWN);
	cx += udSize.x + uiScaled(4.0f);

	const char* speedTxt = SpeedText();
	ImVec2 speedSize = ImGui::CalcTextSize(speedTxt);
	dl->AddText(ImVec2(cx, cy - speedSize.y * 0.5f), textCol, speedTxt);
	cx += speedSize.x + uiScaled(10.0f);

	// Left/Right guide icons (for seek/step) - placed next to each other
	ImVec2 lrSize = ImGui::CalcTextSize(ICON_FA_ANGLE_LEFT);
	dl->AddText(ImVec2(cx, cy - lrSize.y * 0.5f), flashCol(flash_left_), ICON_FA_ANGLE_LEFT);
	cx += lrSize.x + uiScaled(2.0f);
	dl->AddText(ImVec2(cx, cy - lrSize.y * 0.5f), flashCol(flash_right_), ICON_FA_ANGLE_RIGHT);
	cx += lrSize.x + uiScaled(6.0f);

	// --- Right: Round/Frame info ---
	int roundStart, roundEnd, totalRounds;
	GetRoundBounds(roundStart, roundEnd, totalRounds);

	const int roundLen = roundEnd - roundStart;
	const int posInRound = key_msg_count_ - roundStart;

	char rbuf[128];
	if (totalRounds > 0) {
		snprintf(rbuf, sizeof(rbuf), "Round %d/%d  %d/%d fr", start_msg_count_, totalRounds, key_msg_count_, log_file_.inputs_size());
	} else {
		snprintf(rbuf, sizeof(rbuf), "%d/%d fr", key_msg_count_, log_file_.inputs_size());
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
		// Track background
		const ImU32 trackCol = IM_COL32(80, 80, 80, (int)(180 * alpha));
		dl->AddRectFilled(ImVec2(progX0, progY0), ImVec2(progX1, progY1), trackCol, progH * 0.5f);

		// Fill
		float progress = (roundLen > 0) ? std::clamp((float)posInRound / (float)roundLen, 0.0f, 1.0f) : 0.0f;
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
