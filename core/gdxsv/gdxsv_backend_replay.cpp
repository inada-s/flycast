#include "gdxsv_backend_replay.h"

#include <sstream>

#include "SDL_events.h"
#include "cfg/option.h"
#include "emulator.h"
#include "gdx_rpc.h"
#include "gdxsv.h"
#include "gdxsv_prof.h"
#include "gdxsv_replay_util.h"
#include "input/gamepad_device.h"
#include "libs.h"
#include "rend/gui.h"
#include "rend/gui_util.h"
#include "sdl/sdl.h"

using namespace std::chrono;

void GdxsvBackendReplay::Reset() {
	state_ = State::None;
	lbs_tx_reader_.Clear();
	log_file_.Clear();
	recv_buf_.clear();
	recv_delay_ = 0;
	start_msg_count_ = 0;
	pov_ = 0;
	ctrl_commands_.clear();
	gdxsv_save_state.Reset();
	gdxsv.key_display_.Clear();
}

void GdxsvBackendReplay::OnMainUiLoop() {
	/*
	if (emu.running()) {
		const int disk = gdxsv.Disk();
		const int COM_R_No0 = disk == 1 ? 0x0c2f6639 : 0x0c391d79;
		const int ConnectionStatus = disk == 1 ? 0x0c310444 : 0x0c3abb84;
		const int NetCountDown = disk == 1 ? 0x0c310202 : 0x0c3ab942;
		const int DataStopCounter = 0x0c3ab51a;
		NOTICE_LOG(COMMON, "DataStopCounter=%d ConnectionStatus=%d %d %d NetCountDown=%d",
			gdxsv_ReadMem16(DataStopCounter),
			gdxsv_ReadMem16(ConnectionStatus),
			gdxsv_ReadMem16(ConnectionStatus + 2),
			gdxsv_ReadMem16(ConnectionStatus + 4),
			gdxsv_ReadMem16(NetCountDown));
		NOTICE_LOG(COMMON, "COM_R_No0 %d %d", gdxsv_ReadMem8(COM_R_No0), gdxsv_ReadMem8(COM_R_No0 + 5));
	}
	*/

	if (state_ == State::End) {
		gdxsv_save_state.Reset();
		gdxsv_end_replay();
		return;
	}

	if (state_ == State::Start) {
		kcode[0] = ~0x0004u;
		ctrl_commands_.push_back(ReplayCtrlCommand{ReplayCtrlCommand::SomeFrameForward, 60});
	}

	if (state_ == State::McsInBattle) {
		const int disk = gdxsv.Disk();
		const int COM_R_No0 = disk == 1 ? 0x0c2f6639 : 0x0c391d79;
		if (gdxsv_ReadMem8(COM_R_No0) == 4 && (gdxsv_ReadMem8(COM_R_No0 + 5) == 3 || gdxsv_ReadMem8(COM_R_No0 + 5) == 4)) {
			// re-battle end
			Stop();
		} else if (gdxsv_ReadMem8(COM_R_No0) == 4 && gdxsv_ReadMem8(COM_R_No0 + 5) != 0) {
			// not game scene
			if (ctrl_commands_.empty()) {
				ctrl_commands_.push_back(ReplayCtrlCommand{ReplayCtrlCommand::SeekToBriefing });
			}
		}
	}

	if (State::LbsStartBattleFlow <= state_ && !pause_menu_opend_) {
		constexpr u32 BTN_TRIGGER_LEFT = DC_BTN_BITMAPPED_LAST << 1;
		constexpr u32 BTN_TRIGGER_RIGHT = DC_BTN_BITMAPPED_LAST << 2;
		auto input = mapleInputState[0];
		if (input.fullAxes[0] + 128 <= 128 - 0x20) input.kcode &= ~DC_DPAD_LEFT;
		if (input.fullAxes[0] + 128 >= 128 + 0x20) input.kcode &= ~DC_DPAD_RIGHT;
		if (input.fullAxes[1] + 128 <= 128 - 0x20) input.kcode &= ~DC_DPAD_UP;
		if (input.fullAxes[1] + 128 >= 128 + 0x20) input.kcode &= ~DC_DPAD_DOWN;
		if (rt[0] >= 64)
			input.kcode |= BTN_TRIGGER_RIGHT;
		else
			input.kcode &= ~BTN_TRIGGER_RIGHT;
		if (lt[0] >= 64)
			input.kcode |= BTN_TRIGGER_LEFT;
		else
			input.kcode &= ~BTN_TRIGGER_LEFT;

		static u32 prev_kcode = 0;
		if (prev_kcode == 0) prev_kcode = input.kcode;
		u32 pressed_kcode = ~((input.kcode ^ prev_kcode) & ~input.kcode);
		u32 released_kcode = ~((input.kcode ^ prev_kcode) & ~prev_kcode);

		if (input.kcode != prev_kcode) {
			if (~input.kcode & DC_BTN_X) {
				if (~pressed_kcode & (BTN_TRIGGER_RIGHT | DC_BTN_Z)) CtrlSomeFrameForward();
				if (~pressed_kcode & (BTN_TRIGGER_LEFT | DC_BTN_C)) CtrlSomeFrameBackward();
			} else {
				if (~pressed_kcode & DC_BTN_B) CtrlSetSpeed(0), CtrlTogglePause();
				if (~pressed_kcode & DC_BTN_A) CtrlSetSpeed(0), CtrlStepFrame();
				if (~pressed_kcode & (BTN_TRIGGER_RIGHT | DC_BTN_Z)) CtrlSetSpeed(0), CtrlNextRound();
				if (~pressed_kcode & (BTN_TRIGGER_LEFT | DC_BTN_C)) CtrlSetSpeed(0), CtrlPrevRound();
				if (~pressed_kcode & DC_DPAD_RIGHT) CtrlSetSpeed(1);
				if (~released_kcode & DC_DPAD_RIGHT) CtrlSetSpeed(0);
				if (~pressed_kcode & DC_DPAD_LEFT) CtrlSetSpeed(0), CtrlSomeFrameBackward();
				if (~pressed_kcode & DC_DPAD_UP) CtrlSpeedUp();
				if (~pressed_kcode & DC_DPAD_DOWN) CtrlSpeedDown();
			}
		}
		prev_kcode = input.kcode;
	}
}

void GdxsvBackendReplay::OnEndOfFrame() {
	end_of_frame_ = true;
	sh4_cpu.Stop();
}

void GdxsvBackendReplay::OnNextFrame() {
	if (!end_of_frame_) return;
	if (seeking_) return;

	constexpr int save_interval = 180;
	auto in_briefing = [disk = gdxsv.Disk()]() -> bool {
		return disk == 1 ? gdxsv_ReadMem8(0x0c336254) == 2 && gdxsv_ReadMem8(0x0c336255) == 5
						 : gdxsv_ReadMem8(0x0c3d16d4) == 2 && gdxsv_ReadMem8(0x0c3d16d5) == 5;
	};
	auto in_game = [disk = gdxsv.Disk()]() -> bool {
		return disk == 1 ? gdxsv_ReadMem8(0x0c336254) == 2 && gdxsv_ReadMem8(0x0c336255) == 7
						 : gdxsv_ReadMem8(0x0c3d16d4) == 2 && gdxsv_ReadMem8(0x0c3d16d5) == 7;
	};

	auto need_cancel = [&]() -> bool {
		return ctrl_commands_.contains(ReplayCtrlCommand::SaveFirstFrame) || state_ == State::End;
	};

	gdxsv.key_display_.enabled(config::GdxReplayKeyDisplay && in_game());

	// Regular save state
	if ((in_briefing() || in_game()) && gdxsv_save_state.LastSavedFrame() + save_interval <= key_msg_count_ && recv_buf_.empty()) {
		gdxsv_save_state.SaveState(key_msg_count_);
	}

	ReplayCtrlCommand ctrl{};
	while (ctrl_commands_.try_get_front(ctrl)) {
		constexpr int duration = 1000;

		if (ctrl.cmd == ReplayCtrlCommand::TogglePauseMenu) {
			pause_menu_opend_ = !pause_menu_opend_;
			SDL_ShowCursor(pause_menu_opend_ ? SDL_ENABLE : SDL_DISABLE);
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SaveFirstFrame) {
			if (!recv_buf_.empty()) break;
			NOTICE_LOG(COMMON, "SaveFirstFrame saved");
			gdxsv_save_state.Clear();
			gdxsv_save_state.SaveState(key_msg_count_);
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SetMaxLag) {
			if (!recv_buf_.empty()) break;
			gdxsv.maxlag_ = ctrl.arg1;
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::Pause) {
			ctrl_pause_ = true;
			ctrl_commands_.pop_front();
			gui_display_notification("Paused", duration);
		}

		if (ctrl.cmd == ReplayCtrlCommand::Resume) {
			ctrl_pause_ = false;
			ctrl_commands_.pop_front();
			gui_display_notification("Resumed", duration);
		}

		if (ctrl.cmd == ReplayCtrlCommand::TogglePause) {
			ctrl_pause_ = !ctrl_pause_;
			ctrl_commands_.pop_front();
			if (ctrl_pause_)
				gui_display_notification("Paused", duration);
			else
				gui_display_notification("Resumed", duration);
		}

		if (ctrl.cmd == ReplayCtrlCommand::StepFrame) {
			if (ctrl_pause_) {
				ctrl_step_frame_ = true;
				gui_display_notification(("StepFrame:" + std::to_string(key_msg_count_)).c_str(), duration);
			}
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SomeFrameForward) {
			gui_display_notification(">>", duration);
			const int skip_frames = 1 <= ctrl.arg1 ? ctrl.arg1 : save_interval;
			int skipped_frame;
			auto t0 = high_resolution_clock::now();
			gdxsv_prof.Reset();

			const int prev_key_msg_count = key_msg_count_;
			for (skipped_frame = 0; skipped_frame < skip_frames; skipped_frame++) {
				settings.aica.muteAudio = true;
				settings.gdxsv.skipRenderingHack = skipped_frame + 1 < skip_frames;
				settings.gdxsv.skipVBlankHack = true;
				rend_enable_renderer(false);
				seeking_ = true;
				emu.run();
				// Regular save state
				if ((in_briefing() || in_game()) && gdxsv_save_state.LastSavedFrame() + save_interval <= key_msg_count_ && recv_buf_.empty()) {
					gdxsv_save_state.SaveState(key_msg_count_);
				}
				settings.aica.muteAudio = false;
				settings.gdxsv.skipRenderingHack = false;
				settings.gdxsv.skipVBlankHack = false;
				rend_enable_renderer(true);
				seeking_ = false;
				end_of_frame_ = false;
				if (need_cancel()) break;
			}

			const auto ms = duration_cast<milliseconds>(high_resolution_clock::now() - t0).count();
			NOTICE_LOG(COMMON, "SomeFrameForward skipped %d[fr] in %ld[ms] (%.2f[ms/fr]) %d->%d(%d keys)", skipped_frame, ms, (float)ms / skipped_frame, prev_key_msg_count, key_msg_count_, key_msg_count_ - prev_key_msg_count);
			char buf[256];
			sprintf_s(buf, "%.2f[ms/fr]", (float)ms / skipped_frame);
			gui_display_notification(buf, duration);
			gdxsv_prof.Print();
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SeekToBriefing) {
			while (!(in_briefing() || in_game() || need_cancel())) {
				settings.aica.muteAudio = true;
				settings.gdxsv.skipRenderingHack = true;
				settings.gdxsv.skipVBlankHack = true;
				rend_enable_renderer(false);
				seeking_ = true;
				emu.run();
				settings.aica.muteAudio = false;
				settings.gdxsv.skipRenderingHack = false;
				settings.gdxsv.skipVBlankHack = false;
				rend_enable_renderer(true);
				seeking_ = false;
				end_of_frame_ = false;
				if (need_cancel()) break;
			}
			ctrl_commands_.pop_front();
			gdxsv.key_display_.Clear();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SomeFrameBackward) {
			if (in_game()) {
				const int ahead_frame = key_msg_count_ - gdxsv_save_state.LastSavedFrame();
				int target_frame = key_msg_count_ - (60 < ahead_frame ? 0 : save_interval);
				if (gdxsv_save_state.LoadStateMostRecent(target_frame)) {
					key_msg_count_ = target_frame;
					recv_buf_.clear();
					gdxsv.key_display_.Clear();
					if (!in_game()) {
						KillTex = true;
					}
					gui_display_notification("<<", duration);
				}
			}
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SetSpeed || ctrl.cmd == ReplayCtrlCommand::ChangeSpeed) {
			int speed = ctrl.cmd == ReplayCtrlCommand::SetSpeed ? ctrl.arg1 : ctrl_play_speed_ + ctrl.arg1;
			speed = std::max<int>(-2, std::min<int>(2, speed));
			if (speed != ctrl_play_speed_) {
				ctrl_play_speed_ = speed;
				if (0 <= ctrl_play_speed_) {
					config::SkipFrame.override(ctrl_play_speed_);
				}
			}
			std::string speed_text;
			if (ctrl_play_speed_ == 0) speed_text = "Speed:100%";
			if (ctrl_play_speed_ == 1) speed_text = "Speed:200%";
			if (ctrl_play_speed_ == 2) speed_text = "Speed:300%";
			if (ctrl_play_speed_ == -1) speed_text = "Speed:50%";
			if (ctrl_play_speed_ == -2) speed_text = "Speed:33%";
			gui_display_notification(speed_text.c_str(), duration);
			ctrl_commands_.pop_front();
		}

		if (ctrl.cmd == ReplayCtrlCommand::SetRound || ctrl.cmd == ReplayCtrlCommand::ChangeRound) {
			const int round = ctrl.cmd == ReplayCtrlCommand::SetRound ? ctrl.arg1 : start_msg_count_ + ctrl.arg1;
			if (0 < round && round != start_msg_count_ && round - 1 < log_file_.start_msg_indexes_size() &&
				round - 1 < log_file_.start_msg_randoms_size() && gdxsv_save_state.FirstSavedFrame() != -1) {
				gdxsv_save_state.LoadState(gdxsv_save_state.FirstSavedFrame());
				gdxsv_save_state.Clear();
				KillTex = true;
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

				gdxsv_save_state.SaveState(key_msg_count_);
				recv_buf_.clear();
				gdxsv.key_display_.Clear();
				gdxsv.maxlag_ = 1;	// for StartMsg
				NOTICE_LOG(COMMON, "ctrl_change_round_:%d key_msg_count_:%d", round, key_msg_count_);
				NOTICE_LOG(COMMON, "start_msg_randoms_size:%d", log_file_.start_msg_randoms_size());
				ctrl_commands_.push_back(ReplayCtrlCommand{ReplayCtrlCommand::SeekToBriefing});
			}

			ctrl_commands_.pop_front();
		}
	}
}

bool GdxsvBackendReplay::OnOpenMenu() {
	if (state_ <= State::LbsStartBattleFlow) {
		return false;
	}

	ctrl_commands_.push_back(ReplayCtrlCommand{ReplayCtrlCommand::TogglePauseMenu});

	return false;
}

void GdxsvBackendReplay::DisplayOSD() {
	if (pause_menu_opend_) {
		RenderPauseMenu();
	}
}

bool GdxsvBackendReplay::StartFile(const char *path, int pov) {
#ifdef NOWIDE_CONFIG_H_INCLUDED
	FILE *fp = nowide::fopen(path, "rb");
#else
	FILE *fp = fopen(path, "rb");
#endif
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

	return Start();
}

bool GdxsvBackendReplay::StartBuffer(const std::vector<u8> &buf, int pov) {
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
	RestorePatch();
	config::SkipFrame.reset();
	ctrl_commands_.clear();
	settings.gdxsv.skipRenderingHack = false;
	settings.gdxsv.skipVBlankHack = false;
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
		FILE *f = nowide::fopen(replay_file.c_str(), "wb");
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

void GdxsvBackendReplay::CtrlSpeedUp() { ctrl_commands_.push_back(ReplayCtrlCommand{ReplayCtrlCommand::ChangeSpeed, 1}); }

void GdxsvBackendReplay::CtrlSpeedDown() { ctrl_commands_.push_back(ReplayCtrlCommand{ReplayCtrlCommand::ChangeSpeed, -1}); }

void GdxsvBackendReplay::CtrlSetSpeed(int speed) {
	ctrl_commands_.push_back(ReplayCtrlCommand{
		ReplayCtrlCommand::SetSpeed,
		speed,
	});
}

void GdxsvBackendReplay::CtrlTogglePause() { ctrl_commands_.push_back(ReplayCtrlCommand{ReplayCtrlCommand::TogglePause}); }

void GdxsvBackendReplay::CtrlStepFrame() { ctrl_commands_.push_back(ReplayCtrlCommand{ReplayCtrlCommand::StepFrame}); }

void GdxsvBackendReplay::CtrlSomeFrameBackward() { ctrl_commands_.push_back(ReplayCtrlCommand{ReplayCtrlCommand::SomeFrameBackward}); }

void GdxsvBackendReplay::CtrlSomeFrameForward() { ctrl_commands_.push_back(ReplayCtrlCommand{ReplayCtrlCommand::SomeFrameForward}); }

void GdxsvBackendReplay::CtrlSetRound(int round) { ctrl_commands_.push_back(ReplayCtrlCommand{ReplayCtrlCommand::SetRound, round}); }

void GdxsvBackendReplay::CtrlNextRound() { ctrl_commands_.push_back(ReplayCtrlCommand{ReplayCtrlCommand::ChangeRound, 1}); }

void GdxsvBackendReplay::CtrlPrevRound() { ctrl_commands_.push_back(ReplayCtrlCommand{ReplayCtrlCommand::ChangeRound, -1}); }

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
	if (state_ <= State::LbsStartBattleFlow) {
		u8 buf[InetBufSize];
		for (int i = 0; i < size; ++i) {
			buf[i] = gdxsv_ReadMem8(addr + i);
		}

		lbs_tx_reader_.Write((const char *)buf, size);
		ProcessLbsMessage();
	}

	ApplyPatch(false);
	return size;
}

u32 GdxsvBackendReplay::OnSockRead(u32 addr, u32 size) {
	if (state_ <= State::LbsStartBattleFlow) {
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

	if (ctrl_pause_) {
		if (!ctrl_step_frame_) {
			return 0;
		}
		ctrl_step_frame_ = false;
	}

	if (pause_menu_opend_) {
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
			const auto &fields = proto::BattleLogFile::GetReflection()->GetUnknownFields(*data);
			if (!fields.empty()) {
				for (int j = 0; j < fields.field_count(); ++j) {
					const auto &field = fields.field(j);
					if (j == 0 && field.type() == google::protobuf::UnknownField::TYPE_LENGTH_DELIMITED) {
						const auto &body = field.length_delimited();
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
			const auto &data = log_file_.battle_data(i);
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
				  [](const proto::BattleLogUser &a, const proto::BattleLogUser &b) { return a.pos() < b.pos(); });
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

		for (const auto &data : log_file_.battle_data()) {
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

void GdxsvBackendReplay::PrintDisconnectionSummary() {
	std::vector<McsMessage> msg_list;
	McsMessageReader r;
	McsMessage msg;

	for (int i = 0; i < log_file_.battle_data_size(); ++i) {
		const auto &data = log_file_.battle_data(i);
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
		const auto &msg = msg_list[i];
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

void GdxsvBackendReplay::ProcessMcsMessage(const McsMessage &msg) {
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
		start_msg_count_++;
		NOTICE_LOG(COMMON, "StartMsg key_msg_count %d", key_msg_count_);

		if (start_msg_count_ - 1 < log_file_.start_msg_indexes_size()) {
			const auto it = std::lower_bound(log_file_.start_msg_indexes().begin(), log_file_.start_msg_indexes().end(), key_msg_count_);
			if (it != log_file_.start_msg_indexes().end()) {
				NOTICE_LOG(COMMON, "key_msg_count updates %d -> %d", key_msg_count_, *it);
				key_msg_count_ = *it;
			}
		} else {
			log_file_.add_start_msg_indexes(key_msg_count_);
		}

		const int k_rnd0 = gdxsv.Disk() == 1 ? 0x0c310800 : 0x0c3abf40;
		const auto random_data = gdxsv_ReadMem16(k_rnd0);
		if (start_msg_count_ - 1 < log_file_.start_msg_randoms_size()) {
			verify(random_data == (log_file_.start_msg_randoms(start_msg_count_ - 1) & 0xffffu));
		} else {
			log_file_.add_start_msg_randoms(random_data);
		}

		for (int i = 0; i < log_file_.users_size(); ++i) {
			if (i != pov_) {
				auto start_msg = McsMessage::Create(McsMessage::MsgType::StartMsg, i);
				std::copy(start_msg.body.begin(), start_msg.body.end(), std::back_inserter(recv_buf_));
			}
		}

		ctrl_commands_.push_back(ReplayCtrlCommand{ ReplayCtrlCommand::SaveFirstFrame });
		ctrl_commands_.push_back(ReplayCtrlCommand{ ReplayCtrlCommand::SetMaxLag, 1 });
		ctrl_commands_.push_back(ReplayCtrlCommand{ ReplayCtrlCommand::SeekToBriefing });
	} else if (msg_type == McsMessage::MsgType::ForceMsg) {
		// do nothing
	} else if (msg_type == McsMessage::MsgType::KeyMsg1) {
		gdxsv.maxlag_ = 0;

		if (ctrl_play_speed_ < 0) {
			recv_delay_ = -ctrl_play_speed_;
		}

		if (log_file_.inputs_size()) {
			if (key_msg_count_ < log_file_.inputs_size()) {
				const u64 inputs = log_file_.inputs(key_msg_count_);

				for (int i = 0; i < log_file_.users_size(); ++i) {
					const u16 input = u16(inputs >> (i * 16));
					auto key_msg = McsMessage::Create(McsMessage::MsgType::KeyMsg1, i);
					key_msg.body[2] = input >> 8 & 0xff;
					key_msg.body[3] = input & 0xff;
					std::copy(key_msg.body.begin(), key_msg.body.end(), std::back_inserter(recv_buf_));
					gdxsv.key_display_.AppendInput(i, input);
				}

				++key_msg_count_;
				if (key_msg_count_ == log_file_.inputs_size()) {
					Stop();
				}
			}
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
	for (const auto &patch : log_file_.patches()) {
		for (const auto &code : patch.codes()) {
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
	for (const auto &patch : log_file_.patches()) {
		for (const auto &code : patch.codes()) {
			gdxsv_WriteMem(code.size(), code.address(), code.original());
		}
	}
}

void GdxsvBackendReplay::RenderPauseMenu() {
	centerNextWindow();
	ImGui::SetNextWindowSize(ScaledVec2(330, 0));

	ImGui::Begin("##gdxsv-replay-pause", NULL,
				 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);

	ImGui::Columns(2, "buttons", false);
	if (ImGui::Button("Stop Replay", ScaledVec2(150, 50))) {
		pause_menu_opend_ = false;
		gdxsv.StopReplay();
	}
	ImGui::NextColumn();
	if (ImGui::Button("Resume", ScaledVec2(150, 50))) {
		pause_menu_opend_ = false;
	}
	ImGui::EndColumns();

	OptionCheckbox("Show Ally HP", config::GdxReplayShowAllyHP, "Hack the total HP field to display Ally HP");
	OptionCheckbox("Key Display", config::GdxReplayKeyDisplay, "Display controller inputs");

	ImGui::Columns(1, "usage", true);
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
	ImGui::BeginDisabled();
	ImGui::ButtonEx("Replay Control Commands", ImVec2(-1, 0));
	ImGui::EndDisabled();
	ImGui::PopStyleVar();
	ImGui::PopStyleVar();
	ImGui::Text("Menu: Toggle this menu");
	ImGui::Text("B: Pause / Resume");
	ImGui::Text("A: Step Frame (available during Pause)");
	ImGui::Text("Up: Speed Up");
	ImGui::Text("Down: Speed Down");
	ImGui::Text("Right: Speed 2x (hold)");
	ImGui::Text("Left: Seek Backward");
	if (ChangeRoundAvailable()) {
		ImGui::Text("RT: Next Round");
		ImGui::Text("LT: Previous Round");
	}

	// ImGui::Checkbox("Save converted replay on end", &save_converted_log_);

	ImGui::End();
}
