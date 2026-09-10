#include "gdxsv_backend_rollback.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <future>
#include <map>
#include <string>
#include <vector>

#include "emulator.h"
#include "gdx_rpc.h"
#include "gdxsv.h"
#include "gdxsv_emu_hooks.h"
#include "gdxsv.pb.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "input/gamepad_device.h"
#include "libs.h"
#include "log/InMemoryListener.h"
#include "network/ggpo.h"
#include "network/net_platform.h"
#include "oslib/http_client.h"
#include "ui/IconsFontAwesome6.h"
#include "ui/gui_util.h"
#include "rend/transform_matrix.h"

namespace {
u8 DummyGameParam[] = {0x00, 0x00, 0x01, 0x00, 0x03, 0x00, 0x02, 0x00, 0x05, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x83,
					   0x76, 0x83, 0x8c, 0x83, 0x43, 0x83, 0x84, 0x81, 0x5b, 0x82, 0x50, 0x00, 0x00, 0x00, 0x00, 0x07};
u8 DummyRuleData[] = {0x03, 0x02, 0x03, 0x00, 0x00, 0x01, 0x58, 0x02, 0x58, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
					  0x3f, 0xff, 0xff, 0xff, 0x3f, 0x00, 0x00, 0xff, 0x01, 0xff, 0xff, 0xff, 0x3f, 0xff, 0xff, 0xff, 0x3f, 0x00};

constexpr u16 ExInputNone = 0;
constexpr u16 ExInputWaitStart = 1;
constexpr u16 ExInputWaitLoadEnd = 2;

// maple input to mcs pad input
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

void drawConnectionDiagram(int elapsed, const uint8_t matrix[4][4], const std::map<int, int>& pos_to_id);
void drawNetworkStat(const proto::P2PMatching& matching);
}  // namespace

void GdxsvBackendRollback::DisplayOSD() {
	const auto elapsed = ping_pong_.ElapsedMs();
	if (1550 < elapsed && elapsed < 6900) {
		uint8_t matrix[4][4] = {};
		ping_pong_.GetRttMatrix(matrix);
		std::map<int, int> id_to_team, pos_to_id;
		for (const auto& c : matching_.candidates()) {
			id_to_team[c.peer_id()] = c.team();
		}
		for (const auto& p : id_to_team) {
			int pos = (p.second == 2 ? 2 : 0);
			if (pos_to_id.find(pos) != pos_to_id.end()) pos++;
			pos_to_id[pos] = p.first;
		}
		drawConnectionDiagram(elapsed, matrix, pos_to_id);
	}

	if (osd_network_stat_ || osd_network_stat_countdown_) {
		if (osd_network_stat_countdown_) osd_network_stat_countdown_--;
		drawNetworkStat(matching_);
	}
}

void GdxsvBackendRollback::Reset() {
	RestorePatch();

	state_ = State::None;
	is_local_test_ = false;
	error_fast_return_ = false;
	ggpo_game_renderer_reset_ = false;
	osd_network_stat_ = false;
	osd_network_stat_countdown_ = 0;
	start_button_counter_ = 0;
	recv_delay_ = 0;
	port_ = 0;
	recv_buf_.clear();
	lbs_tx_reader_.Clear();
	matching_.Clear();
	report_.Clear();
	ping_pong_.Reset();
	spectator_uplink_.Stop();
	start_network_ = std::future<bool>();

	start_at_ = 0;
	input_logs_.clear();
	start_msg_indexes_.clear();
	start_msg_randoms_.clear();
	round_data_.clear();
	spectator_flushed_inputs_ = 0;
	spectator_flushed_round_events_ = 0;
	pending_spectator_round_results_.clear();

	ggpo::stopSession();
	gdxsv.key_display_.Clear();
	config::GGPOEnable.reset();
}

void GdxsvBackendRollback::OnMainUiLoop() {
	const int disk = gdxsv.Disk();
	const int COM_R_No0 = disk == 1 ? 0x0c2f6639 : 0x0c391d79;

	/*
	if (emu.running()) {
		const int ConnectionStatus = disk == 1 ? 0x0c310444 : 0x0c3abb84;
		const int NetCountDown = disk == 1 ? 0x0c310202 : 0x0c3ab942;
		const int DataStopCounter = 0x0c3ab51a;
		NOTICE_LOG(COMMON, "DataStopCounter=%d ConnectionStatus=%d %d %d NetCountDown=%d", gdxsv_ReadMem16(DataStopCounter),
	gdxsv_ReadMem16(ConnectionStatus), gdxsv_ReadMem16(ConnectionStatus + 2), gdxsv_ReadMem16(ConnectionStatus + 4),
	gdxsv_ReadMem16(NetCountDown));
	}
	*/

	if (state_ == State::StartLocalTest) {
		kcode[0] = ~0x0004;
	}

	if (state_ == State::StopEmulator) {
		NOTICE_LOG(COMMON, "StopEmulator");
		emu.stop();
		state_ = State::WaitPingPong;
	}

	if (state_ == State::WaitPingPong && !ping_pong_.Running()) {
		state_ = State::StartGGPOSession;
	}

	static auto session_start_time = std::chrono::high_resolution_clock::now();
	if (state_ == State::StartGGPOSession) {
		NOTICE_LOG(COMMON, "StartGGPOSession");
		/*
		if (matching_.peer_id() == 0) {
			ping_pong_.DebugSetRtt(0, 1, 10);
			ping_pong_.DebugSetRtt(0, 2, 50);
			ping_pong_.DebugSetRtt(0, 3, 200);
			ping_pong_.PrintRttMatrix();
		}
		if (matching_.peer_id() == 1) {
			ping_pong_.DebugSetRtt(1, 2, 10);
			ping_pong_.DebugSetRtt(1, 3, 200);
			ping_pong_.PrintRttMatrix();
		}
		*/
		bool ok = true;
		uint8_t rtt_matrix[4][4] = {};
		ping_pong_.GetRttMatrix(rtt_matrix);

		std::vector<std::string> ips(matching_.player_count());
		std::vector<u16> ports(matching_.player_count());
		std::vector<u8> relays(matching_.player_count());
		static const auto get_ip_port = [](const sockaddr_storage& storage) -> std::tuple<std::string, u16> {
			if (storage.ss_family == AF_INET) {
				const auto addr = (sockaddr_in*)&storage;
				char str[INET_ADDRSTRLEN] = {};
				inet_ntop(AF_INET, &(addr->sin_addr), str, sizeof(str));
				return {str, ntohs(addr->sin_port)};
			}
			if (storage.ss_family == AF_INET6) {
				const auto addr = (sockaddr_in6*)&storage;
				char str[INET6_ADDRSTRLEN] = {};
				inet_ntop(AF_INET6, &(addr->sin6_addr), str, sizeof(str));
				return {str, ntohs(addr->sin6_port)};
			}
			return {"", 0};
		};

		auto find_relay_peer = [&](int dst_peer) -> std::pair<int, int> {
			int i = dst_peer;
			int relay_rtt = INT_MAX;
			int relay_peer = -1;
			for (int j = 0; j < matching_.player_count(); j++) {
				if (j == i) continue;
				if (j == matching_.peer_id()) continue;
				if (rtt_matrix[matching_.peer_id()][j] && 0 < rtt_matrix[j][i] && rtt_matrix[j][i] < 255) {
					int rtt = rtt_matrix[matching_.peer_id()][j] + rtt_matrix[j][i];
					if (rtt < relay_rtt) {
						relay_rtt = rtt;
						relay_peer = j;
					}
				}
			}

			return {relay_peer, relay_rtt};
		};

		float max_rtt = 0;
		NOTICE_LOG(COMMON, "Peer count %d", matching_.player_count());
		for (int i = 0; i < matching_.player_count(); i++) {
			if (i == matching_.peer_id()) {
				NOTICE_LOG(COMMON, "Peer%d is self", i);
				ips[i] = "";
				ports[i] = static_cast<u16>(port_);
				continue;
			}

			sockaddr_storage addr_storage{};

			float rtt;
			bool direct_ok = ping_pong_.GetAvailableAddress(i, &addr_storage, &rtt);
			bool relay_ok = false;
			auto [relay_peer, relay_rtt] = find_relay_peer(i);
			if (relay_peer != -1 && (!direct_ok || relay_rtt + 32 < rtt)) {
				relay_ok = ping_pong_.GetAvailableAddress(relay_peer, &addr_storage, &rtt);
				rtt += static_cast<float>(rtt_matrix[relay_peer][i]);
				relays[i] = true;
			}

			if (direct_ok || relay_ok) {
				max_rtt = std::max(max_rtt, rtt);
				std::tie(ips[i], ports[i]) = get_ip_port(addr_storage);
				NOTICE_LOG(COMMON, "Peer%d %.2fms IP:%s Port:%d Relay:%d", i, rtt, mask_ip_address(ips[i]).c_str(), ports[i], relays[i]);
			} else {
				ok = false;
				NOTICE_LOG(COMMON, "Peer%d unreachable", i);
			}
		}

		if (ok) {
			const int delay = std::max<int>({2, config::GdxMinDelay.get(), static_cast<int>(max_rtt / 2.0 / 16.0 + 0.9999)});
			NOTICE_LOG(COMMON, "max_rtt=%.2f delay=%d", max_rtt, delay);
			config::GGPOEnable.override(true);
			config::GGPODelay.override(delay);
			config::NetworkStats.override(false);
			config::FixedFrequency.override(2);
			config::LimitFPS.override(false);
			config::AudioBufferSize.override(2822);

			start_network_ = ggpo::gdxsvStartNetwork(matching_.battle_code().c_str(), matching_.peer_id(), ips, ports, relays);
			ggpo::receiveChatMessages(nullptr);
			session_start_time = std::chrono::high_resolution_clock::now();
			state_ = State::WaitGGPOSession;
		} else {
			NOTICE_LOG(COMMON, "Network unreachable");
			SetCloseReason("unreachable");
			error_fast_return_ = true;
			emu.start();
		}
		{
			std::ostringstream ss;
			const auto lines = InMemoryListener::getInstance()->getLog();
			for (const auto& line : lines) {
				ss << line;
			}
			report_.set_before_log(ss.str());
		}
	}

	if (state_ == State::WaitGGPOSession) {
		const auto now = std::chrono::high_resolution_clock::now();
		const auto timeout = 10000 <= std::chrono::duration_cast<std::chrono::milliseconds>(now - session_start_time).count();

		if (start_network_.valid() && start_network_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
			if (ggpo::active()) {
				start_network_ = std::future<bool>();
				state_ = State::McsInBattle;
				emu.start();
			} else {
				NOTICE_LOG(COMMON, "StartNetwork failure");
				SetCloseReason("ggpo_start_failure");
				error_fast_return_ = true;
				emu.start();
			}
		} else if (timeout) {
			NOTICE_LOG(COMMON, "StartNetwork timeout");
			SetCloseReason("ggpo_start_timeout");
			error_fast_return_ = true;
			emu.start();
		}
	}

	static int disconnect_frame = 0;

	// Re battle end
	if (gdxsv_ReadMem8(COM_R_No0) == 4 && gdxsv_ReadMem8(COM_R_No0 + 5) == 3 && ggpo::active() && !ggpo::isInRollback()) {
		if (state_ != State::CloseWait) {
			SetCloseReason("game_end");
			ggpo::getCurrentFrame(&disconnect_frame);
			for (int i = 0; i < matching_.users_size(); i++) {
				ggpo::disconnect(matching_.peer_id());
			}
			state_ = State::CloseWait;
		}
	}

	// Friend save scene
	if (gdxsv_ReadMem8(COM_R_No0) == 4 && gdxsv_ReadMem8(COM_R_No0 + 5) == 4 && ggpo::active() && !ggpo::isInRollback()) {
		ResetGgpoGameRendererState();

		int frame = 0;
		ggpo::getCurrentFrame(&frame);

		if (16 < frame - disconnect_frame) {
			ggpo::stopSession();
			config::GGPOEnable.reset();
			state_ = State::End;
		}
	}

	// Close session on error
	if (error_fast_return_) {
		SetCloseReason("error_fast_return");
		ggpo::stopSession();
		config::GGPOEnable.reset();
		if (state_ < State::End) {
			state_ = State::End;
		}
	}

	// A local test exits on its own once the match is over. CloseWait is set
	// the moment the battle ends (close_reason "game_end"); triggering here,
	// rather than at Closed, gets the process out before the normal
	// return-to-lobby flow blocks retrying a lobby server that isn't there.
	if (is_local_test_ && State::CloseWait <= state_) {
		static int local_test_closing = 180;
		if (--local_test_closing == 0) {
			// The exit status is how a test harness learns whether the match
			// ran to its end or broke off. A normal battle closes with
			// "game_end" or "cl_app_close" (the game closing its socket on the
			// way back to the lobby); these reasons mean the netcode never got
			// going or broke mid-match.
			const std::string reason = report_.close_reason();
			const bool broke = reason == "unreachable" || reason == "ggpo_start_failure"
							|| reason == "ggpo_start_timeout" || reason == "error_fast_return"
							|| reason == "cl_error" || reason.empty();
			NOTICE_LOG(COMMON, "RollbackNet local test %s: close_reason=%s", broke ? "failed" : "finished",
					   reason.c_str());
			const int code = broke ? 3 : 0;
			if (gdxsv_headless()) {
				// A finished headless test has nothing to release; exit hard so
				// the status is exactly this code, not whatever SDL teardown
				// might turn it into.
				gdxsv_headless_exit(code);
			}
			gdxsv_set_exit_code(code);
			dc_exit();
		}
	}
}

bool GdxsvBackendRollback::StartLocalTest(const char* param) {
	auto args = std::string(param);
	int me = 0;
	int n = 4;
	if (!args.empty() && '1' <= args[0] && args[0] <= '4') {
		me = args[0] - '1';
	}
	if (2 < args.size() && args[1] == '/' && '1' <= args[2] && args[2] <= '4') {
		n = args[2] - '0';
	}

	if (const u64 seed = config::loadInt64("gdxsv", "rand_input", 0)) {
		NOTICE_LOG(COMMON, "RandomInput Seed=%d", seed + me);
		ggpo::randomInput(true, seed + me, 0x0004 | 0x0400 | 0x0200 | 0x0010 | 0x0040);
	}
	DummyRuleData[6] = 1;
	DummyRuleData[7] = 0;
	DummyRuleData[8] = 1;
	DummyRuleData[9] = 0;

	proto::P2PMatching matching;
	matching.set_battle_code("0123456");
	matching.set_peer_id(me);
	matching.set_session_id(12345);
	matching.set_ping_test_duration(7500);
	matching.set_player_count(n);
	for (int i = 0; i < n; i++) {
		proto::PlayerAddress player{};
		if (i < 2)
			player.set_ip("::1");
		else
			player.set_ip("127.0.0.1");
		player.set_port(20010 + i);
		player.set_user_id("USER0" + std::to_string(i));
		player.set_peer_id(i);
		player.set_team(i / 2 + 1);
		matching.mutable_candidates()->Add(std::move(player));
	}
	for (int i = 0; i < n; i++) {
		proto::BattleLogUser user{};
		user.set_user_id("USER0" + std::to_string(i));
		user.set_user_name("USER0" + std::to_string(i));
		user.set_pilot_name("PILOT0" + std::to_string(i));
		user.set_team(i / 2 + 1);
		matching.mutable_users()->Add(std::move(user));
	}

	Prepare(matching, 20010 + me);
	state_ = State::StartLocalTest;
	is_local_test_ = true;
	gdxsv.maxlag_ = 0;
	gdxsv.maxrebattle_ = 1;

	if (getenv("MAXREBATTLE")) {
		gdxsv.maxrebattle_ = atoi(getenv("MAXREBATTLE"));
	}

	NOTICE_LOG(COMMON, "RollbackNet StartLocalTest %d", me);
	return true;
}

void GdxsvBackendRollback::Prepare(const proto::P2PMatching& matching, int port) {
	NOTICE_LOG(COMMON, "GdxsvBackendRollback.Prepare");
	Reset();

	matching_ = matching;
	port_ = port;

	for (const auto& c : matching.candidates()) {
		if (c.peer_id() != matching_.peer_id()) {
			ping_pong_.AddCandidate(c.user_id(), c.peer_id(), c.ip(), c.port());
		}
	}
	ping_pong_.Start(matching.session_id(), matching.peer_id(), port, matching.ping_test_duration());
	spectator_uplink_.Start(gdxsv.lbs_net_.RemoteHost(), gdxsv.lbs_net_.RemotePort(), matching.battle_code(),
							 matching.session_id(), matching.is_training_game());

	report_.Clear();
	report_.set_battle_code(matching.battle_code());
	report_.set_session_id(matching.session_id());
	report_.set_frame_count(0);
	report_.set_peer_id(matching.peer_id());
	report_.set_player_count(matching.player_count());
	start_at_ = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void GdxsvBackendRollback::Open() {
	NOTICE_LOG(COMMON, "GdxsvBackendRollback.Open");
	recv_buf_.assign({0x0e, 0x61, 0x00, 0x22, 0x10, 0x31, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd});
	state_ = State::McsSessionExchange;
	ggpo_game_renderer_reset_ = false;
	gdxsv.maxlag_ = 0;
	ApplyPatch(true);
	osd_network_stat_ = config::NetworkStats;
	gdxsv.key_display_.SetDisplayPlayer(matching_.peer_id());
}

void GdxsvBackendRollback::Close() {
	if (state_ < State::McsWaitJoin || state_ == State::Closed) {
		NOTICE_LOG(COMMON, "GdxsvBackendRollback.Close Skipped");
		return;
	}

	NOTICE_LOG(COMMON, "GdxsvBackendRollback.Close");
	SetCloseReason("close");
	ggpo::stopSession();
	config::GGPOEnable.reset();
	config::NetworkStats.load();
	config::FixedFrequency.load();
	config::LimitFPS.load();
	config::AudioBufferSize.load();
	RestorePatch();
	osd_network_stat_ = false;
	error_fast_return_ = false;
	SaveReplay();
	// Finish ACK-tracked round/input retries before returning to the lobby,
	// where P2PMatchingReport tells LBS to close the spectator recording.
	spectator_uplink_.Stop(true);
	gdxsv.key_display_.enabled(false);
	state_ = State::Closed;
	EventManager::event(Event::GGPOGameEnd);
	NOTICE_LOG(COMMON, "GdxsvBackendRollback.Close Done");
}

void GdxsvBackendRollback::ResetGgpoGameRendererState() {
	if (ggpo_game_renderer_reset_) {
		return;
	}
	ggpo_game_renderer_reset_ = true;
	EventManager::event(Event::GGPOGameEnd);
}

void GdxsvBackendRollback::FlushConfirmedToSpectatorUplink() {
	if (matching_.is_training_game() || !ggpo::active()) {
		return;
	}
	// Highest frame GGPO can no longer roll back, derived from public API so
	// no GGPO source needs patching: GetPredictedFrames() is simply
	// (framecount - last_confirmed_frame), and it is already surfaced as
	// stats.sync.predicted_frames.
	int current_frame = 0;
	if (!ggpo::getCurrentFrame(&current_frame)) {
		return;
	}
	ggpo::NetworkStats stats{};
	ggpo::getNetworkStats(matching_.peer_id(), &stats);
	const int confirmed_frame = current_frame - stats.sync.predicted_frames;

	while (spectator_flushed_inputs_ < static_cast<int32_t>(input_logs_.size()) &&
		   input_logs_[spectator_flushed_inputs_].first <= confirmed_frame) {
		spectator_uplink_.PushInput(spectator_flushed_inputs_, input_logs_[spectator_flushed_inputs_].second);
		++spectator_flushed_inputs_;
	}

	while (spectator_flushed_round_events_ < static_cast<int32_t>(start_msg_indexes_.size()) &&
		   start_msg_indexes_[spectator_flushed_round_events_].first <= confirmed_frame) {
		const int idx = spectator_flushed_round_events_;
		spectator_uplink_.PushRoundEvent(start_msg_indexes_[idx].second, static_cast<uint64_t>(start_msg_randoms_[idx].second));
		++spectator_flushed_round_events_;
	}

	for (auto it = pending_spectator_round_results_.begin(); it != pending_spectator_round_results_.end();) {
		if (it->first <= confirmed_frame) {
			const int32_t round_index = it->second;
			const auto& round = round_data_[round_index];
			spectator_uplink_.PushRoundResult(round_index, round.win_team(),
											   std::vector<int32_t>(round.used_ms().begin(), round.used_ms().end()));
			it = pending_spectator_round_results_.erase(it);
		} else {
			++it;
		}
	}
}

u32 GdxsvBackendRollback::OnSockWrite(u32 addr, u32 size) {
	if (state_ <= State::LbsStartBattleFlow) {
		u8 buf[InetBufSize];
		for (int i = 0; i < size; ++i) {
			buf[i] = gdxsv_ReadMem8(addr + i);
		}

		lbs_tx_reader_.Write(reinterpret_cast<const char*>(buf), size);
		ProcessLbsMessage();
	}

	ApplyPatch(false);
	return size;
}

u32 GdxsvBackendRollback::OnSockRead(u32 addr, u32 size) {
	if (state_ <= State::LbsStartBattleFlow) {
		ProcessLbsMessage();

		int n = std::min<int>(recv_buf_.size(), size);
		for (int i = 0; i < n; ++i) {
			gdxsv_WriteMem8(addr + i, recv_buf_.front());
			recv_buf_.pop_front();
		}

		return n;
	}

	int frame = 0;
	ggpo::getCurrentFrame(&frame);

	const int disk = gdxsv.Disk();
	const int InetBuf = disk == 1 ? 0x0c310244 : 0x0c3ab984;
	const int NetCountDown = disk == 1 ? 0x0c310202 : 0x0c3ab942;
	const int DataStopCounter = disk == 1 ? 0x0c30fdda : 0x0c3ab51a;
	const int COM_R_No0 = disk == 1 ? 0x0c2f6639 : 0x0c391d79;
	const int PlayerWork = disk == 1 ? 0x0c336854: 0xc3d1cd4;
	const int WinTeam = disk == 1 ? 0x0c3364b6 : 0xc3d1948;
	const auto inputState = mapleInputState;
	const auto memExInputAddr = gdxsv.symbols_.at("rbk_ex_input");
	const auto in_game = [disk = gdxsv.Disk()]() -> bool {
		return disk == 1 ? gdxsv_ReadMem8(0x0c336254) == 2 && gdxsv_ReadMem8(0x0c336255) == 7
						 : gdxsv_ReadMem8(0x0c3d16d4) == 2 && gdxsv_ReadMem8(0x0c3d16d5) == 7;
	};
	const int skipFrameCount = ggpo::getSkippedFrames(frame);
	const auto appendKeyMsg1Inputs = [&]() {
		u64 inputs = 0;
		for (int i = 0; i < matching_.player_count(); ++i) {
			auto [body] = McsMessage::Create(McsMessage::KeyMsg1, i);
			const auto input = convertInput(inputState[i]);
			body[2] = input >> 8 & 0xff;
			body[3] = input & 0xff;
			std::copy(body.begin(), body.end(), std::back_inserter(recv_buf_));
			inputs |= static_cast<u64>(input) << (i * 16);
			if (matching_.is_training_game() && !ggpo::isInRollback()) {
				gdxsv.key_display_.AppendInput(i, input);
			}
		}
		while (!input_logs_.empty() && frame <= input_logs_.back().first) {
			input_logs_.pop_back();
		}
		if (!matching_.is_training_game()) {
			// Runs on every simulation pass, speculative and rollback alike.
			// Pushing to spectator_uplink_ from here would leak mispredicted
			// values whenever the ack beats GGPO's rollback window. So only
			// touch the local record; FlushConfirmedToSpectatorUplink pushes
			// once GGPO confirms the frame can't change.
			input_logs_.emplace_back(frame, inputs);
		}
		return inputs;
	};

	gdxsv.key_display_.enabled(matching_.is_training_game() && !osd_network_stat_ && in_game());

	// Disconnect from training game
	if (ggpo::active() && !ggpo::isInRollback() && matching_.is_training_game()) {
		auto start_btn_pressed = false;
		for (int i = 0; i < 4; i++) {
			start_btn_pressed |= ~inputState[i].kcode & DC_BTN_START;
		}

		if (start_btn_pressed) {
			start_button_counter_++;
		} else {
			start_button_counter_ = 0;
		}

		if (60 * 4 <= start_button_counter_) {
			for (int i = 0; i < matching_.player_count(); ++i) {
				ggpo::disconnect(matching_.peer_id());
			}
			error_fast_return_ = true;
		}
	}

	// Disconnect check
	if (ggpo::active() && !(gdxsv_ReadMem8(COM_R_No0) == 4 && gdxsv_ReadMem8(COM_R_No0 + 5) == 2)) {
		for (int i = 0; i < matching_.player_count(); ++i) {
			if (!ggpo::isConnected(i)) {
				char buf[256] = {0};
				const auto& user = matching_.users(i);
				snprintf(buf, sizeof(buf), "player_disconnect peer=%d fr=%d ID=%s HN=%s PN=%s", i, frame, user.user_id().c_str(),
						 user.user_name().c_str(), user.pilot_name().c_str());
				if (SetCloseReason(buf)) {
					report_.set_disconnected_peer_id(i);
				}
				osd_network_stat_countdown_ = 60 * 10;
				error_fast_return_ = true;
				break;
			}
		}
	}

	// round_data
	if (ggpo::active() && !round_data_.empty() && gdxsv_ReadMem8(WinTeam) != 0 && gdxsv_ReadMem8(WinTeam) != round_data_.back().win_team()) {
		round_data_.back().set_win_team(gdxsv_ReadMem8(WinTeam));
		round_data_.back().clear_used_ms();
		NOTICE_LOG(COMMON, "ROUND %d WIN_TEAM = %d", round_data_.size(), round_data_.back().win_team());
		for (int i = 0; i < matching_.player_count(); ++i) {
			const auto ms_index = gdxsv_ReadMem8(PlayerWork + i * 0x2000 + 0x1f02);
			round_data_.back().add_used_ms(ms_index + 1);  // 0-origin → 1-origin
			NOTICE_LOG(COMMON, "%d USED MS = %d", i, ms_index + 1);
		}
		// Deferred like input_logs_ above - round_data_.back() can still be
		// corrected by a later rollback re-simulation of this same frame;
		// FlushConfirmedToSpectatorUplink sends it once that can't happen.
		pending_spectator_round_results_.emplace_back(frame, static_cast<int32_t>(round_data_.size()) - 1);
	}

	// Fast disconnect dialog appear
	if (error_fast_return_) {
		gdxsv_WriteMem16(DataStopCounter, 1800);
		gdxsv_WriteMem16(NetCountDown, 0);
		return 0;
	}

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

		if (msg.Type() == McsMessage::ConnectionIdMsg) {
			state_ = State::StopEmulator;
		}

		if (msg.Type() == McsMessage::IntroMsg) {
			for (int i = 0; i < matching_.player_count(); i++) {
				if (i == matching_.peer_id()) continue;
				auto a = McsMessage::Create(McsMessage::IntroMsg, i);
				std::copy(a.body.begin(), a.body.end(), std::back_inserter(recv_buf_));
			}
		}

		if (msg.Type() == McsMessage::IntroMsgReturn) {
			for (int i = 0; i < matching_.player_count(); i++) {
				if (i == matching_.peer_id()) continue;
				auto a = McsMessage::Create(McsMessage::IntroMsgReturn, i);
				std::copy(a.body.begin(), a.body.end(), std::back_inserter(recv_buf_));
			}
		}

		if (msg.Type() == McsMessage::PingMsg) {
			for (int i = 0; i < matching_.player_count(); i++) {
				if (i == matching_.peer_id()) continue;
				auto a = McsMessage::Create(McsMessage::PongMsg, i);
				a.SetPongTo(matching_.peer_id());
				a.PongCount(msg.PingCount());
				std::copy(a.body.begin(), a.body.end(), std::back_inserter(recv_buf_));
			}
		}

		if (msg.Type() == McsMessage::StartMsg) {
			gdxsv_WriteMem16(memExInputAddr, ExInputWaitStart);
			if (!ggpo::isInRollback()) {
				ggpo::setExInput(ExInputWaitStart);
				NOTICE_LOG(COMMON, "StartMsg KeyFrame:%d", frame);
			}
		}

		if (msg.Type() == McsMessage::KeyMsg1) {
			const int tsFrames = ggpo::timeSyncFrames;
			if (!ggpo::isInRollback() && 0 < gdxsv_ReadMem16(DataStopCounter) && tsFrames > 0 && frame % 10 == 0) {
				ggpo::timeSyncFrames.fetch_sub(1);
				ggpo::notifySkipInput();
				DEBUG_LOG(COMMON, "KeyMsg1 frame=%d: skipFrame remaining=%d", frame, tsFrames - 1);
			} else if (0 < skipFrameCount && gdxsv_ReadMem16(DataStopCounter) < skipFrameCount + 1) {
				DEBUG_LOG(COMMON, "KeyMsg1 frame=%d: skipFrame replaying", frame);
			} else {
				appendKeyMsg1Inputs();
			}
		}

		if (msg.Type() == McsMessage::LoadEndMsg) {
			for (int i = 0; i < matching_.player_count(); i++) {
				if (i == matching_.peer_id()) continue;
				auto a = McsMessage::Create(McsMessage::LoadStartMsg, i);
				std::copy(a.body.begin(), a.body.end(), std::back_inserter(recv_buf_));
			}

			gdxsv_WriteMem16(memExInputAddr, ExInputWaitLoadEnd);
			if (!ggpo::isInRollback()) {
				ggpo::setExInput(ExInputWaitLoadEnd);
				NOTICE_LOG(COMMON, "LoadEndMsg KeyFrame:%d", frame);
			}
		}

		verify(recv_buf_.size() <= size);
	}

	if (gdxsv_ReadMem16(memExInputAddr) != ExInputNone) {
		auto exInput = gdxsv_ReadMem16(memExInputAddr);
		bool ok = true;
		for (int i = 0; i < matching_.player_count(); i++) {
			ok &= inputState[i].exInput == exInput;
		}

		if (ok && exInput == ExInputWaitStart) {
			NOTICE_LOG(COMMON, "StartMsg Join:%d", frame);
			gdxsv_WriteMem16(memExInputAddr, ExInputNone);
			if (!ggpo::isInRollback()) {
				ggpo::setExInput(ExInputNone);
			}
			for (int i = 0; i < matching_.player_count(); i++) {
				if (i == matching_.peer_id()) continue;
				auto a = McsMessage::Create(McsMessage::MsgType::StartMsg, i);
				std::copy(a.body.begin(), a.body.end(), std::back_inserter(recv_buf_));
			}

			while (!start_msg_indexes_.empty() &&
				   (frame <= start_msg_indexes_.back().first || start_msg_indexes_.back().second == input_logs_.size())) {
				start_msg_indexes_.pop_back();
				start_msg_randoms_.pop_back();
			}
			u16 rand_value = gdxsv_ReadMem16(gdxsv.Disk() == 1 ? 0x0c310800 : 0x0c3abf40);
			start_msg_indexes_.emplace_back(frame, input_logs_.size());
			start_msg_randoms_.emplace_back(frame, rand_value);
			round_data_.resize(start_msg_indexes_.size());
			// Deferred like input_logs_ above - this can re-fire during a
			// rollback re-simulation of this same frame, and LBS dedups
			// SpectatorRoundEvent by frame with "first arrival wins", so an
			// early speculative push here could permanently lock in the
			// wrong RNG seed. FlushConfirmedToSpectatorUplink sends it once
			// GGPO confirms this frame can't change again.
		}

		if (ok && exInput == ExInputWaitLoadEnd) {
			NOTICE_LOG(COMMON, "LoadEndMsg Join:%d", frame);
			gdxsv_WriteMem16(memExInputAddr, ExInputNone);
			if (!ggpo::isInRollback()) {
				ggpo::setExInput(ExInputNone);
			}
			for (int i = 0; i < matching_.player_count(); i++) {
				if (i == matching_.peer_id()) continue;
				auto a = McsMessage::Create(McsMessage::MsgType::LoadEndMsg, i);
				std::copy(a.body.begin(), a.body.end(), std::back_inserter(recv_buf_));
			}
		}
	}

	if (!ggpo::isInRollback() && !matching_.is_training_game()) {
		report_.set_frame_count(frame);

		if (0 < frame && frame % 600 == 0) {
			auto me = matching_.peer_id();
			ggpo::NetworkStats stats{};
			if (ggpo::active()) {
				ggpo::getNetworkStats(me, &stats);
				report_.add_fps_history(stats.extra.current_fps);
				report_.set_total_timesync(stats.extra.total_timesync);
				report_.set_input_block_count_0(stats.extra.input_block_count[0]);
				report_.set_input_block_count_1(stats.extra.input_block_count[1]);
				report_.set_input_block_count_2(stats.extra.input_block_count[2]);
			}
		}
	}

	if (0 < skipFrameCount && skipFrameCount + 1 == gdxsv_ReadMem16(DataStopCounter)) {
		appendKeyMsg1Inputs();
	}

	if (!ggpo::isInRollback()) {
		FlushConfirmedToSpectatorUplink();
	}

	verify(recv_buf_.size() <= size);

	int n = std::min<int>(recv_buf_.size(), size);
	for (int i = 0; i < n; ++i) {
		gdxsv_WriteMem8(addr + i, recv_buf_.front());
		recv_buf_.pop_front();
	}

	return n;
}

u32 GdxsvBackendRollback::OnSockPoll() {
	if (state_ <= State::LbsStartBattleFlow) {
		ProcessLbsMessage();
	}

	if (0 < recv_delay_) {
		recv_delay_--;
		return 0;
	}

	return recv_buf_.size();
}

void GdxsvBackendRollback::ProcessLbsMessage() {
	if (state_ == State::StartLocalTest) {
		LbsMessage::SvNotice(LbsMessage::lbsReadyBattle).Serialize(recv_buf_);
		recv_delay_ = 1;
		state_ = State::LbsStartBattleFlow;
	}

	LbsMessage msg;
	if (lbs_tx_reader_.Read(msg)) {
		if (state_ == State::StartLocalTest) {
			state_ = State::LbsStartBattleFlow;
		}

		if (msg.command == LbsMessage::lbsLobbyMatchingEntry) {
			LbsMessage::SvAnswer(msg).Serialize(recv_buf_);
			LbsMessage::SvNotice(LbsMessage::lbsReadyBattle).Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskMatchingJoin) {
			LbsMessage::SvAnswer(msg).Write8(matching_.player_count())->Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskPlayerSide) {
			LbsMessage::SvAnswer(msg).Write8(matching_.peer_id() + 1)->Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskPlayerInfo) {
			int pos = msg.Read8();
			DummyGameParam[16] = '0' + pos;
			DummyGameParam[17] = 0;
			LbsMessage::SvAnswer(msg)
				.Write8(pos)
				->WriteString("USER0" + std::to_string(pos))
				->WriteString("USER0" + std::to_string(pos))
				->WriteBytes(reinterpret_cast<char*>(DummyGameParam), sizeof(DummyGameParam))
				->Write16(1)
				->Write16(0)
				->Write16(0)
				->Write16(0)
				->Write16(0)
				->Write16(0)
				->Write16(1 + (pos - 1) / 2)
				->Write16(0)
				->Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskRuleData) {
			LbsMessage::SvAnswer(msg).WriteBytes((char*)DummyRuleData, sizeof(DummyRuleData))->Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskBattleCode) {
			LbsMessage::SvAnswer(msg).WriteString("012345")->Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskMcsVersion) {
			LbsMessage::SvAnswer(msg).Write8(10)->Serialize(recv_buf_);
		}

		if (msg.command == LbsMessage::lbsAskMcsAddress) {
			LbsMessage::SvAnswer(msg).Write16(4)->Write8(255)->Write8(255)->Write8(255)->Write8(255)->Write16(2)->Write16(255)->Serialize(
				recv_buf_);
		}

		if (msg.command == LbsMessage::lbsLogout) {
			state_ = State::McsWaitJoin;
		}

		recv_delay_ = 1;
	}
}

bool GdxsvBackendRollback::SetCloseReason(const char* reason) {
	if (report_.close_reason().empty()) {
		report_.set_close_reason(reason);
		return true;
	}
	return false;
}

void GdxsvBackendRollback::SaveReplay() const {
	if (!config::GdxSaveReplay || matching_.is_training_game()) {
		NOTICE_LOG(COMMON, "Skip SaveReplay is_training=%d", matching_.is_training_game());
		return;
	}

	if (matching_.battle_code().empty() || input_logs_.empty()) {
		return;
	}

	auto log = std::make_unique<proto::BattleLogFile>();
	log->set_game_disk(gdxsv.Disk() == 1 ? "dc1" : "dc2");
	log->set_battle_code(matching_.battle_code());
	log->set_log_file_version(20230730);
	for (int i = 0; i < gdxsv.patch_list_.patches_size(); ++i) {
		log->add_patches()->CopyFrom(gdxsv.patch_list_.patches(i));
	}
	log->set_rule_bin(matching_.rule_bin());
	log->mutable_users()->CopyFrom(matching_.users());

	for (const auto& kv : input_logs_) {
		log->add_inputs(kv.second);
	}
	for (const auto& kv : start_msg_indexes_) {
		log->add_start_msg_indexes(kv.second);
	}
	for (const auto& kv : start_msg_randoms_) {
		log->add_start_msg_randoms(kv.second);
	}
	for (const auto& rd : round_data_) {
		log->add_round_data()->CopyFrom(rd);
	}

	log->set_start_at(start_at_);
	const auto now = std::chrono::system_clock::now();
	log->set_end_at(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());

	log->set_close_reason(report_.close_reason());
	log->set_disconnect_user_index(report_.disconnected_peer_id());

	std::thread([log = std::move(log)]() {
		auto replay_dir = get_writable_data_path("replays");
		if (!file_exists(replay_dir)) {
			if (!make_directory(replay_dir)) {
				ERROR_LOG(COMMON, "Failed to create replay directory");
				return;
			}
		}

		auto replay_file = replay_dir + "/" + log->battle_code() + ".pb";
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

		bool ok = log->SerializeToFileDescriptor(fd);
		fclose(f);

		if (!ok) {
			ERROR_LOG(COMMON, "SaveReplay: SerializeToFileDescriptor failure");
			return;
		}

		if (!config::GdxUploadReplay) {
			return;
		}

		std::vector<http::PostField> fields;
		fields.emplace_back("file", replay_file, "application/octet-stream");
		int rc = http::post("https://asia-northeast1-gdxsv-274515.cloudfunctions.net/uploader", fields);
		if (rc == 200 || rc == 409) {
			NOTICE_LOG(COMMON, "SaveReplay: upload OK");
		} else {
			ERROR_LOG(COMMON, "SaveReplay: upload Failed staus: %d", rc);
		}
	}).detach();
}

void GdxsvBackendRollback::ApplyPatch(bool first_time) {
	if (state_ == State::None || state_ == State::End) {
		return;
	}

	gdxsv.WritePatch();

	// Skip Key MsgPush
	if (gdxsv.Disk() == 1) {
		gdxsv_WriteMem16(0x8c058b7c, 9);
		gdxsv_WriteMem8(0x0c310450, 1);
	}
	if (gdxsv.Disk() == 2) {
		gdxsv_WriteMem16(0x8c045f64, 9);
		gdxsv_WriteMem8(0x0c3abb90, 1);
	}
}

void GdxsvBackendRollback::RestorePatch() {
	if (gdxsv.Disk() == 1) {
		gdxsv_WriteMem16(0x8c058b7c, 0x410b);
		gdxsv_WriteMem8(0x0c310450, 2);
	}
	if (gdxsv.Disk() == 2) {
		gdxsv_WriteMem16(0x8c045f64, 0x410b);
		gdxsv_WriteMem8(0x0c3abb90, 2);
	}
}

namespace {
float getScale() {
	const float w = ImGui::GetIO().DisplaySize.x;
	const float h = ImGui::GetIO().DisplaySize.y;
	const float renderAR = getOutputFramebufferAspectRatio();
	const float screenAR = w / h;
	float dx = 0;
	float dy = 0;
	if (renderAR > screenAR)
		dy = h * (1 - screenAR / renderAR) / 2;
	else
		dx = w * (1 - renderAR / screenAR) / 2;

	return std::min((w - dx * 2) / 640.f, (h - dy * 2) / 480.f);
}

ImVec2 fromCenter(float x, float y, float scale) {
	const float w = ImGui::GetIO().DisplaySize.x;
	const float h = ImGui::GetIO().DisplaySize.y;
	const float cx = w / 2.f;
	const float cy = h / 2.f;
	return ImVec2(cx + (x * scale), cy + (y * scale));
}

ImColor fadeColor(ImColor color, int elapsed) {
	if (elapsed <= 1800)
		color.Value.w *= (elapsed - 1550) / 250.0;
	else if (elapsed >= 6600 && elapsed < 6900)
		color.Value.w *= 1.0 - (elapsed - 6600) / 300.0;
	return color;
}

ImColor msColor(int ms) {
	if (ms <= 0) return ImColor(64, 64, 64);
	if (ms <= 30) return ImColor(87, 213, 213);
	if (ms <= 60) return ImColor(0, 255, 149);
	if (ms <= 90) return ImColor(255, 255, 0);
	if (ms <= 120) return ImColor(255, 170, 0);
	return ImColor(255, 0, 0);
}

ImColor barStep(int ms) {
	if (ms <= 0) return 5;
	if (ms <= 30) return 5;
	if (ms <= 60) return 4;
	if (ms <= 90) return 3;
	if (ms <= 120) return 2;
	return 1;
}

ImColor barColor(int ms, int elapsed) { return fadeColor(msColor(ms), elapsed); }

void drawDot(ImDrawList* draw_list, ImVec2 center, ImColor c, float scale) {
	draw_list->AddCircleFilled(center, 6.5 * scale, ImColor(0, 0, 0, 128), 20);
	draw_list->AddCircleFilled(center, 5.5 * scale, c, 20);
}

void baseRect(ImVec2 points[4], float sx, float sy) {
	const float v = sx / 2.0;
	const float w = sy / 2.0;
	points[0].x = -v;
	points[0].y = -w;
	points[1].x = v;
	points[1].y = -w;
	points[2].x = v;
	points[2].y = w;
	points[3].x = -v;
	points[3].y = w;
}

void scaleRect(ImVec2 points[4], float scale) {
	for (int i = 0; i < 4; i++) {
		points[i].x *= scale;
		points[i].y *= scale;
	}
}

void scaleRectX(ImVec2 points[4], float scale) {
	for (int i = 0; i < 4; i++) {
		points[i].x *= scale;
	}
}

void moveRect(ImVec2 points[4], ImVec2 delta) {
	for (int i = 0; i < 4; i++) {
		points[i].x += delta.x;
		points[i].y += delta.y;
	}
}

void rotRect(ImVec2 points[4], float rad) {
	for (int i = 0; i < 4; i++) {
		auto x = points[i].x;
		auto y = points[i].y;
		points[i].x = x * cos(rad) - y * sin(rad);
		points[i].y = y * cos(rad) + x * sin(rad);
	}
}

void drawRectWave(ImDrawList* draw_list, ImVec2 anchor, ImColor color, float scale, int step, int dir, int elapsed) {
	const float rad = (3.141592 / 4) * dir;
	ImVec2 points[4] = {};
	for (int i = 0; i < 5; i++) {
		baseRect(points, 5, 3.5);
		auto c = color;
		if (step <= i)
			c = ImColor(64, 64, 64);
		else if (i == (elapsed / 100 % 5)) {
			c.Value.x = 200;
			c.Value.y = 200;
			c.Value.z = 200;
		}
		moveRect(points, ImVec2(0, i * 5.3));
		scaleRectX(points, 1 + i * 0.50);
		scaleRect(points, scale);
		moveRect(points, ImVec2(0, scale * 9.5));
		rotRect(points, rad);
		moveRect(points, anchor);
		draw_list->AddConvexPolyFilled(points, sizeof(points) / sizeof(points[0]), c);
		draw_list->AddPolyline(points, sizeof(points) / sizeof(points[0]), ImColor(0, 0, 0, 128), true, 1.0 * scale);
	}
}

void drawConnectionDiagram(int elapsed, const uint8_t matrix[4][4], const std::map<int, int>& pos_to_id) {
	const float w = ImGui::GetIO().DisplaySize.x;
	const float h = ImGui::GetIO().DisplaySize.y;
	const float scale = getScale();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2.f, ImGui::GetIO().DisplaySize.y / 2.f), ImGuiCond_Always,
							ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(w, h));
	ImGui::SetNextWindowBgAlpha(0.0f);
	ImGui::Begin("##gdxsvosd", NULL,
				 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs);

	ImDrawList* draw_list = ImGui::GetWindowDrawList();

	draw_list->AddRectFilled(fromCenter(-45, -97, scale), fromCenter(45.25, -51.875, scale), fadeColor(ImColor(0, 0, 0, 128), elapsed));
	draw_list->AddRectFilled(fromCenter(-45, 53.125, scale), fromCenter(45.25, 98.25, scale), fadeColor(ImColor(0, 0, 0, 128), elapsed));

	ImVec2 d(36, 89);
	ImVec2 origins[4] = {
		fromCenter(0, 0, scale) + ImVec2(-d.x * scale, -d.y * scale),
		fromCenter(0, 0, scale) + ImVec2(d.x * scale, -d.y * scale),
		fromCenter(0, 0, scale) + ImVec2(-d.x * scale, d.y * scale),
		fromCenter(0, 0, scale) + ImVec2(d.x * scale, d.y * scale),
	};

	int dirs[4][4] = {};
	dirs[0][1] = dirs[2][3] = 6;
	dirs[0][3] = 7;
	dirs[0][2] = dirs[1][3] = 0;
	dirs[1][2] = 1;
	dirs[1][0] = dirs[3][2] = 2;
	dirs[3][0] = 3;
	dirs[2][0] = dirs[3][1] = 4;
	dirs[2][1] = 5;

	for (const auto& p : pos_to_id) {
		int i = p.first;
		if (i < 0 || i >= 4) continue;
		int max_ms = 0;
		for (int j = 0; j < 4; j++) {
			if (i == j) continue;
			if (pos_to_id.find(j) == pos_to_id.end()) continue;
			auto ms = matrix[pos_to_id.at(i)][pos_to_id.at(j)];
			drawRectWave(draw_list, origins[i], barColor(ms, elapsed), scale, barStep(ms), dirs[i][j], elapsed);
			max_ms = std::max(max_ms, (int)ms);
		}
		drawDot(draw_list, origins[i], barColor(max_ms, elapsed), scale);
	}

	ImGui::End();
	ImGui::PopStyleVar();
	ImGui::PopStyleVar();
}

void textCentered(std::string text) {
	auto windowWidth = ImGui::GetWindowSize().x;
	auto textWidth = ImGui::CalcTextSize(text.c_str()).x;
	ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
	ImGui::Text(text.c_str());
}

const ImVec4 kNetworkStatBarColor(0.557f, 0.268f, 0.965f, 1.f);
const ImVec4 kNetworkStatWarningColor(0.6f, 0.2f, 0.2f, 1.f);

enum class ConnectionHealth {
	Excellent,
	Good,
	Medium,
	Fair,
	Poor,
};

struct LossEvent {
	double time;
	int count;
};

struct ConnectionHealthTracker {
	bool loss_initialized = false;
	int previous_loss_from = 0;
	int previous_loss_to = 0;
	std::deque<LossEvent> loss_from_events;
	std::deque<LossEvent> loss_to_events;
	int recent_loss_from = 0;
	int recent_loss_to = 0;

	bool sample_initialized = false;
	double last_sample_time = 0.0;
	std::array<int, 10> ping_samples = {};
	int ping_sample_count = 0;
	int ping_sample_index = 0;
	int ping_p20 = 0;
	int ping_median = 0;
	int ping_jitter = 0;

	ConnectionHealth displayed = ConnectionHealth::Excellent;
	ConnectionHealth pending = ConnectionHealth::Excellent;
	double pending_since = 0.0;
	double next_recovery_at = 0.0;
};

ConnectionHealth worse(ConnectionHealth lhs, ConnectionHealth rhs) {
	return static_cast<int>(lhs) >= static_cast<int>(rhs) ? lhs : rhs;
}

ConnectionHealth better(ConnectionHealth lhs, ConnectionHealth rhs) {
	return static_cast<int>(lhs) <= static_cast<int>(rhs) ? lhs : rhs;
}

ConnectionHealth lossHealth(int loss) {
	if (loss == 0) return ConnectionHealth::Excellent;
	if (loss <= 2) return ConnectionHealth::Good;
	if (loss <= 4) return ConnectionHealth::Medium;
	if (loss <= 7) return ConnectionHealth::Fair;
	return ConnectionHealth::Poor;
}

ConnectionHealth queueHealth(int queue_excess) {
	if (queue_excess <= 1) return ConnectionHealth::Excellent;
	if (queue_excess == 2) return ConnectionHealth::Good;
	if (queue_excess <= 4) return ConnectionHealth::Medium;
	if (queue_excess <= 7) return ConnectionHealth::Fair;
	return ConnectionHealth::Poor;
}

ConnectionHealth stableLatencyHealth(int ping) {
	// Stable RTT still adds input delay, but must never look as severe as a
	// connection that is actively losing packets or stalling.
	if (ping <= 60) return ConnectionHealth::Excellent;
	if (ping <= 180) return ConnectionHealth::Good;
	return ConnectionHealth::Medium;
}

ConnectionHealth jitterHealth(int jitter) {
	if (jitter <= 5) return ConnectionHealth::Excellent;
	if (jitter <= 10) return ConnectionHealth::Good;
	if (jitter <= 15) return ConnectionHealth::Medium;
	if (jitter <= 20) return ConnectionHealth::Fair;
	return ConnectionHealth::Poor;
}

ConnectionHealth latencySpikeHealth(int spike) {
	if (spike <= 10) return ConnectionHealth::Excellent;
	if (spike <= 15) return ConnectionHealth::Good;
	if (spike <= 20) return ConnectionHealth::Medium;
	if (spike <= 35) return ConnectionHealth::Fair;
	return ConnectionHealth::Poor;
}

ConnectionHealth pacingHealth(int frame_skew) {
	const int skew = std::abs(frame_skew);
	if (skew <= 2) return ConnectionHealth::Excellent;
	if (skew <= 4) return ConnectionHealth::Good;
	if (skew <= 6) return ConnectionHealth::Medium;
	if (skew <= 8) return ConnectionHealth::Fair;
	return ConnectionHealth::Poor;
}

template <size_t N>
void addSample(std::array<int, N>& samples, int& count, int& index, int value) {
	samples[index] = value;
	index = (index + 1) % N;
	count = std::min(count + 1, static_cast<int>(N));
}

template <size_t N>
std::array<int, N> sortedSamples(const std::array<int, N>& samples, int count) {
	auto sorted = samples;
	std::sort(sorted.begin(), sorted.begin() + count);
	return sorted;
}

ConnectionHealth updateConnectionHealth(ConnectionHealthTracker& tracker,
										const ggpo::NetworkStats& stats) {
	const double now = ImGui::GetTime();
	const int loss_from = stats.network.recv_packet_loss;
	const int loss_to = stats.network.send_packet_loss;
	if (!tracker.loss_initialized || loss_from < tracker.previous_loss_from || loss_to < tracker.previous_loss_to) {
		tracker = {};
		tracker.loss_initialized = true;
		tracker.previous_loss_from = loss_from;
		tracker.previous_loss_to = loss_to;
	} else {
		if (loss_from > tracker.previous_loss_from) {
			const int loss = loss_from - tracker.previous_loss_from;
			tracker.loss_from_events.push_back({now, loss});
			tracker.recent_loss_from += loss;
		}
		if (loss_to > tracker.previous_loss_to) {
			const int loss = loss_to - tracker.previous_loss_to;
			tracker.loss_to_events.push_back({now, loss});
			tracker.recent_loss_to += loss;
		}
		tracker.previous_loss_from = loss_from;
		tracker.previous_loss_to = loss_to;
	}
	while (!tracker.loss_from_events.empty() && now - tracker.loss_from_events.front().time > 2.0) {
		tracker.recent_loss_from -= tracker.loss_from_events.front().count;
		tracker.loss_from_events.pop_front();
	}
	while (!tracker.loss_to_events.empty() && now - tracker.loss_to_events.front().time > 2.0) {
		tracker.recent_loss_to -= tracker.loss_to_events.front().count;
		tracker.loss_to_events.pop_front();
	}
	const int recent_loss = std::max(tracker.recent_loss_from, tracker.recent_loss_to);

	if (!tracker.sample_initialized || now - tracker.last_sample_time >= 1.0) {
		tracker.sample_initialized = true;
		tracker.last_sample_time = now;
		if (stats.network.ping > 0) {
			addSample(tracker.ping_samples, tracker.ping_sample_count, tracker.ping_sample_index,
					  stats.network.ping);
			if (tracker.ping_sample_count >= 4) {
				const auto pings = sortedSamples(tracker.ping_samples, tracker.ping_sample_count);
				tracker.ping_p20 = pings[(tracker.ping_sample_count - 1) / 5];
				tracker.ping_median = pings[(tracker.ping_sample_count - 1) / 2];
				tracker.ping_jitter = pings[(tracker.ping_sample_count - 1) * 9 / 10] -
									  pings[(tracker.ping_sample_count - 1) / 10];
			}
		}
	}

	int latency_spike = 0;
	if (tracker.ping_sample_count >= 4)
		latency_spike = std::max(stats.network.ping - tracker.ping_p20, 0);
	const int stable_ping = tracker.ping_sample_count >= 4 ? tracker.ping_median : stats.network.ping;
	ConnectionHealth latency_health = stableLatencyHealth(stable_ping);
	latency_health = worse(latency_health, jitterHealth(tracker.ping_jitter));
	latency_health = worse(latency_health, latencySpikeHealth(latency_spike));

	const int frame_skew = stats.timesync.local_frames_behind;
	constexpr double frame_time_ms = 1000.0 / 59.94;
	// One pending input per elapsed game frame is normal while waiting for an
	// acknowledgement. Only count queue growth beyond the RTT-sized window.
	const int expected_queue = stats.network.ping > 0
		? static_cast<int>(std::ceil(stats.network.ping / frame_time_ms))
		: 0;
	const int queue_excess = std::max(stats.network.send_queue_len - expected_queue, 0);
	const ConnectionHealth loss_health = lossHealth(recent_loss);
	const ConnectionHealth queue_health = queueHealth(queue_excess);
	const ConnectionHealth pacing_health = pacingHealth(frame_skew);

	ConnectionHealth observed = ConnectionHealth::Excellent;
	observed = worse(observed, loss_health);
	observed = worse(observed, queue_health);
	observed = worse(observed, latency_health);
	observed = worse(observed, pacing_health);

	// Loss is actionable immediately. Other problems must persist for one
	// second; severe pacing alone must persist for two seconds.
	tracker.displayed = worse(tracker.displayed, loss_health);
	if (static_cast<int>(observed) > static_cast<int>(tracker.displayed)) {
		// Keep one timer across bad-bucket changes. Equal samples do not erase
		// degradation evidence already accumulated at a worse level.
		if (tracker.pending_since == 0.0) {
			tracker.pending = observed;
			tracker.pending_since = now;
		} else
			tracker.pending = better(tracker.pending, observed);
		const bool poor_only_from_pacing =
			pacing_health == ConnectionHealth::Poor &&
			loss_health != ConnectionHealth::Poor &&
			queue_health != ConnectionHealth::Poor &&
			latency_health != ConnectionHealth::Poor;
		const double degradation_delay =
			tracker.pending == ConnectionHealth::Poor && poor_only_from_pacing ? 2.0 : 1.0;
		if (now - tracker.pending_since >= degradation_delay) {
			tracker.displayed = tracker.pending;
			tracker.pending_since = 0.0;
		}
		tracker.next_recovery_at = 0.0;
	} else if (static_cast<int>(observed) < static_cast<int>(tracker.displayed)) {
		tracker.pending_since = 0.0;
		if (tracker.next_recovery_at == 0.0)
			tracker.next_recovery_at = now + 3.0;
		else if (now >= tracker.next_recovery_at) {
			tracker.displayed = static_cast<ConnectionHealth>(static_cast<int>(tracker.displayed) - 1);
			tracker.next_recovery_at = now + 2.0;
		}
	} else {
		tracker.next_recovery_at = 0.0;
	}

	return tracker.displayed;
}

void drawSegmentedMeter(int segment_count, int fill_count, const ImVec4& fill_color) {
	const float line_height = ImGui::GetTextLineHeight();
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float width = std::max(ImGui::GetContentRegionAvail().x, line_height * 2.f);
	const float gap = std::max(1.f, line_height * 0.1f);
	const float segment_width = (width - gap * (segment_count - 1)) / segment_count;
	const float segment_height = std::max(3.f, line_height * 0.5f);
	const float y = pos.y + (line_height - segment_height) * 0.5f + 1.f;

	ImDrawList *draw_list = ImGui::GetWindowDrawList();
	ImVec4 empty = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
	empty.w *= 0.4f;
	const ImU32 empty_color = ImGui::GetColorU32(empty);
	const ImU32 active_color = ImGui::GetColorU32(fill_color);
	for (int i = 0; i < segment_count; ++i) {
		const float left = pos.x + i * (segment_width + gap);
		const ImVec2 min(left, y);
		const ImVec2 max(left + segment_width, y + segment_height);
		draw_list->AddRectFilled(min, max, i < fill_count ? active_color : empty_color, segment_height * 0.2f);
	}
	ImGui::Dummy(ImVec2(width, line_height));
}

void drawLagMeter(ConnectionHealth health) {
	ImGui::TextUnformatted("Lag");
	ImGui::SameLine();
	const int fill_count = health == ConnectionHealth::Poor ? 5 : static_cast<int>(health);
	ImVec4 fill_color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
	fill_color.w = 0.75f;
	if (health == ConnectionHealth::Medium)
		fill_color = ImVec4(1.f, 0.85f, 0.f, 1.f);
	else if (health == ConnectionHealth::Fair)
		fill_color = ImVec4(1.f, 0.5f, 0.f, 1.f);
	else if (health == ConnectionHealth::Poor)
		fill_color = ImVec4(1.f, 0.15f, 0.12f, 1.f);
	drawSegmentedMeter(5, fill_count, fill_color);
}

void drawFramePacingMeter(int local_frames_behind, float requested_width = -1.f) {
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float available_width = ImGui::GetContentRegionAvail().x;
	const float width = requested_width > 0.f ? std::min(requested_width, available_width) : available_width;
	const float height = ImGui::GetTextLineHeight();
	const float meter_left = pos.x;
	const float meter_right = pos.x + width;
	const float track_padding = height * 0.2f;
	const float track_left = meter_left + track_padding;
	const float track_right = meter_right - track_padding;
	const float center = (track_left + track_right) * 0.5f;
	const float y = pos.y + height * 0.5f;
	const int frame_offset = std::clamp(local_frames_behind, -8, 8);
	const float marker = center - (track_right - track_left) * frame_offset / 16.f;
	const float warning = std::clamp((std::abs(frame_offset) - 3.f) / 5.f, 0.f, 1.f);
	const ImVec4 pacing_color(
		kNetworkStatBarColor.x + (kNetworkStatWarningColor.x - kNetworkStatBarColor.x) * warning,
		kNetworkStatBarColor.y + (kNetworkStatWarningColor.y - kNetworkStatBarColor.y) * warning,
		kNetworkStatBarColor.z + (kNetworkStatWarningColor.z - kNetworkStatBarColor.z) * warning,
		1.f);
	ImDrawList *draw_list = ImGui::GetWindowDrawList();
	const ImU32 text_color = ImGui::GetColorU32(ImGuiCol_Text);
	const ImU32 track_color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
	const ImU32 frame_color = ImGui::GetColorU32(ImGuiCol_FrameBg);
	const ImU32 pacing_color_u32 = ImGui::GetColorU32(pacing_color);

	draw_list->AddRectFilled(ImVec2(meter_left, pos.y), ImVec2(meter_right, pos.y + height), frame_color,
						 std::min(ImGui::GetStyle().FrameRounding, height * 0.5f));
	draw_list->AddLine(ImVec2(track_left, y), ImVec2(track_right, y), track_color,
					   std::max(1.f, height * 0.075f));
	draw_list->AddLine(ImVec2(center, y), ImVec2(marker, y), pacing_color_u32,
					   std::max(1.f, height * 0.85f));
	draw_list->AddLine(ImVec2(center, y - height * 0.45f), ImVec2(center, y + height * 0.45f), text_color,
					   std::max(1.5f, height * 0.14f));
	const char *slow_subject = frame_offset >= 5 ? "You" : frame_offset <= -5 ? "Peer" : nullptr;
	if (slow_subject != nullptr) {
		const float half_space_width = ImGui::CalcTextSize("  ").x * 0.5f;
		const float subject_width = ImGui::CalcTextSize(slow_subject).x;
		draw_list->AddText(ImVec2(center - half_space_width - subject_width, pos.y), text_color, slow_subject);
		draw_list->AddText(ImVec2(center + half_space_width, pos.y), text_color, "Slow");
	}
	ImGui::Dummy(ImVec2(width, height));
}

void drawNetworkDiagnostics(int player, const ggpo::NetworkStats& stats, bool connected) {
	static bool loss_initialized[4] = {};
	static int previous_loss_from[4] = {};
	static int previous_loss_to[4] = {};
	static double loss_from_highlight_until[4] = {};
	static double loss_to_highlight_until[4] = {};
	static int loss_from_highlight_grade[4] = {};
	static int loss_to_highlight_grade[4] = {};

	const int loss_from = stats.network.recv_packet_loss;
	const int loss_to = stats.network.send_packet_loss;
	const double now = ImGui::GetTime();
	if (!loss_initialized[player] || loss_from < previous_loss_from[player] || loss_to < previous_loss_to[player]) {
		loss_initialized[player] = true;
		previous_loss_from[player] = loss_from;
		previous_loss_to[player] = loss_to;
		loss_from_highlight_until[player] = 0.0;
		loss_to_highlight_until[player] = 0.0;
		loss_from_highlight_grade[player] = 0;
		loss_to_highlight_grade[player] = 0;
	} else {
		const auto update_loss_highlight = [&](int loss, int& previous, double& highlight_until,
											 int& highlight_grade) {
			if (loss < previous) {
				previous = loss;
				highlight_until = 0.0;
				highlight_grade = 0;
			} else if (loss > previous) {
				const int new_losses = loss - previous;
				highlight_grade = now < highlight_until
					? std::min(highlight_grade + new_losses, 5)
					: std::min(new_losses, 5);
				previous = loss;
				highlight_until = now + 0.3;
			} else if (now >= highlight_until) {
				highlight_grade = 0;
			}
		};
		update_loss_highlight(loss_from, previous_loss_from[player], loss_from_highlight_until[player],
								  loss_from_highlight_grade[player]);
		update_loss_highlight(loss_to, previous_loss_to[player], loss_to_highlight_until[player],
								  loss_to_highlight_grade[player]);
	}

	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float width = ImGui::GetContentRegionAvail().x;
	const float height = ImGui::GetTextLineHeight();
	const ImVec4 orange(1.f, 0.55f, 0.1f, 1.f);
	const auto draw_pacing_row = [&]() {
		constexpr float icon_scale = 0.8f;
		const float icon_size = ImGui::GetFontSize() * icon_scale;
		const float icon_width = ImGui::CalcTextSize(ICON_FA_CLOCK).x * icon_scale;
		const float icon_spacing = ImGui::CalcTextSize(" ").x * 0.5f;
		const ImVec2 icon_pos = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddText(
			ImGui::GetFont(), icon_size,
			ImVec2(icon_pos.x, icon_pos.y + (height - icon_size) * 0.5f),
			ImGui::GetColorU32(ImGuiCol_Text), ICON_FA_CLOCK);
		ImGui::Dummy(ImVec2(icon_width, height));
		ImGui::SameLine(0.f, icon_spacing);
		drawFramePacingMeter(stats.timesync.local_frames_behind,
			width - icon_width - icon_spacing);
	};
	if (!connected) {
		ImGui::Dummy(ImVec2(width, height));
		ImGui::PushStyleColor(ImGuiCol_Text, msColor(999).Value);
		textCentered("Disconnected");
		ImGui::PopStyleColor();
		return;
	}

	const ImVec4 text_color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
	const ImVec4 red(1.f, 0.2f, 0.2f, 1.f);
	const auto loss_highlight_color = [&](int grade) {
		const float blend = (std::clamp(grade, 1, 5) - 1.f) / 4.f;
		return ImVec4(1.f, 0.65f * (1.f - blend), 0.15f * (1.f - blend), 1.f);
	};
	const int queue = stats.network.send_queue_len;
	ImVec4 queue_color = text_color;
	if (queue >= 5 && queue <= 7) {
		const float blend = (queue - 4.f) / 3.f;
		queue_color = ImVec4(text_color.x + (orange.x - text_color.x) * blend,
							 text_color.y + (orange.y - text_color.y) * blend,
							 text_color.z + (orange.z - text_color.z) * blend, 1.f);
	} else if (queue >= 8) {
		const float blend = std::min((queue - 7.f) / 3.f, 1.f);
		queue_color = ImVec4(orange.x + (red.x - orange.x) * blend,
							 orange.y + (red.y - orange.y) * blend,
							 orange.z + (red.z - orange.z) * blend, 1.f);
	}

	ImDrawList *draw_list = ImGui::GetWindowDrawList();
	float x = pos.x;
	const auto draw_text = [&](const std::string& text, const ImVec4& color) {
		draw_list->AddText(ImVec2(x, pos.y), ImGui::GetColorU32(color), text.c_str());
		x += ImGui::CalcTextSize(text.c_str()).x;
	};
	draw_text("L: ", text_color);
	const std::string loss_from_text = "↓ " + std::to_string(loss_from);
	const std::string loss_to_text = "↑ " + std::to_string(loss_to);
	draw_text(loss_from_text,
			  now < loss_from_highlight_until[player]
				  ? loss_highlight_color(loss_from_highlight_grade[player]) : text_color);
	draw_text(" ", text_color);
	draw_text(loss_to_text,
			  now < loss_to_highlight_until[player]
				  ? loss_highlight_color(loss_to_highlight_grade[player]) : text_color);
	const char *queue_label = "Q:";
	const std::string queue_value = std::to_string(queue);
	const float queue_width = ImGui::CalcTextSize("Q:99").x;
	const float queue_left = pos.x + width - queue_width;
	draw_list->AddText(ImVec2(queue_left, pos.y), ImGui::GetColorU32(text_color), queue_label);
	draw_list->AddText(ImVec2(pos.x + width - ImGui::CalcTextSize(queue_value.c_str()).x, pos.y),
					   ImGui::GetColorU32(queue_color), queue_value.c_str());
	ImGui::Dummy(ImVec2(width, height));
	draw_pacing_row();
}

void drawDetailedPlayerNetworkStats(int player, const proto::P2PMatching& matching,
									const ggpo::NetworkStats& stats, bool connected) {
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::Text("%dP:%s", player + 1, matching.users(player).user_id().c_str());
	std::string ping = connected
		? std::to_string(stats.network.ping) + (stats.network.is_relay ? "(R)" : "") : "--";
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(ping.c_str()).x);
	ImGui::PushStyleColor(ImGuiCol_Text, msColor(connected ? stats.network.ping : 999).Value);
	ImGui::Text("%s", ping.c_str());
	ImGui::PopStyleColor();
	textCentered(matching.users(player).pilot_name());
	drawNetworkDiagnostics(player, stats, connected);
}

void drawNetworkStat(const proto::P2PMatching& matching) {
	auto me = matching.peer_id();
	static ggpo::NetworkStats stats[4] = {};
	static bool is_connected[4] = {};
	if (ggpo::active()) {
		for (int i = 0; i < matching.users_size(); i++) {
			ggpo::getNetworkStats(i, &stats[i]);
			is_connected[i] = ggpo::isConnected(i);
		}
	}

	const int remote_players = std::max(matching.users_size() - 1, 0);
	static std::array<ConnectionHealthTracker, 4> health_trackers;
	static std::array<std::string, 4> tracked_user_ids;
	std::array<ConnectionHealth, 4> health;
	for (int i = 0; i < matching.users_size(); ++i) {
		if (i == me) continue;
		if (tracked_user_ids[i] != matching.users(i).user_id()) {
			health_trackers[i] = {};
			tracked_user_ids[i] = matching.users(i).user_id();
		}
		if (is_connected[i])
			health[i] = updateConnectionHealth(health_trackers[i], stats[i]);
		else
			health_trackers[i] = {};
	}

	static bool panel_bounds_valid = false;
	static ImVec2 panel_min;
	static ImVec2 panel_max;
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const ImVec2 display_size = ImGui::GetIO().DisplaySize;
	const bool mouse_in_window = ImGui::IsMousePosValid(&mouse) && mouse.x >= 0.f && mouse.x < display_size.x &&
											 mouse.y >= 0.f && mouse.y < display_size.y;
	const bool show_details = mouse_in_window && panel_bounds_valid && mouse.x >= panel_min.x &&
											 mouse.x <= panel_max.x && mouse.y >= panel_min.y && mouse.y <= panel_max.y;
	const float battle_hud_scale = display_size.y / 960.f;
	const float panel_top = (192.f + 21.f) * battle_hud_scale;
	const float panel_bottom = display_size.y - (205.f + 21.f) * battle_hud_scale;
	const float max_height = panel_bottom - panel_top;
	const float left_margin = 30.f * battle_hud_scale;
	const float title_height = 20.f * battle_hud_scale;
	const float title_body_spacing = 4.f * battle_hud_scale;
	const ImGuiStyle& style = ImGui::GetStyle();
	// Keep both views at the refined diagnostic layout's scale and width so
	// hovering changes only the content, not the panel geometry or typography.
	const int content_rows = 3 + remote_players * 4;
	const float natural_item_spacing_y = std::max(style.ItemSpacing.y, ImGui::GetTextLineHeight() * 0.7f);
	const float scalable_height = style.WindowPadding.y * 2.f +
									  content_rows * (ImGui::GetTextLineHeight() + natural_item_spacing_y) +
									  remote_players * (1.f + natural_item_spacing_y * 3.f);
	const float panel_scale = std::min(1.f, (max_height - title_height - title_body_spacing) / scalable_height);
	const float window_width = 160.f * settings.display.uiScale * panel_scale;
	ImGui::PushFont(nullptr, style.FontSizeBase * panel_scale);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, style.WindowPadding * panel_scale);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
						ImVec2(style.ItemSpacing.x * panel_scale, natural_item_spacing_y * panel_scale));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, style.FramePadding * panel_scale);
	ImGui::SetNextWindowPos(ImVec2(left_margin, panel_top), ImGuiCond_Always);
	ImGui::SetNextWindowSizeConstraints(ImVec2(window_width, 0), ImVec2(window_width, max_height));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.122f, 0.122f, 0.122f, 0.055f));
	ImGui::Begin("##gdxsv_osd_network_stats", NULL,
				 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs);
	ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.557f, 0.268f, 0.965f, 1.f));
	const ImVec2 content_pos = ImGui::GetCursorScreenPos();
	const ImVec2 panel_pos = ImGui::GetWindowPos();
	const ImVec2 title_size = ImGui::CalcTextSize("NETWORK STATS");
	ImGui::GetWindowDrawList()->AddRectFilled(
		panel_pos, panel_pos + ImVec2(window_width, title_height),
		ImGui::GetColorU32(ImVec4(0.122f, 0.122f, 0.122f, 0.16f)));
	ImGui::GetWindowDrawList()->AddText(
		ImVec2(panel_pos.x + (window_width - title_size.x) * 0.5f,
			   panel_pos.y + std::max(0.f, (title_height - title_size.y) * 0.5f)),
		ImGui::GetColorU32(ImVec4(0.925f, 0.941f, 1.f, 0.24f)), "NETWORK STATS");
	ImGui::SetCursorScreenPos(
		ImVec2(content_pos.x, panel_pos.y + title_height + title_body_spacing + ImGui::GetStyle().WindowPadding.y));

	// Frame Delay
	ImGui::Text("Delay");
	const int delay_frames = config::GGPODelay.get();
	std::string delay = std::to_string(delay_frames) + "fr";
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(delay.c_str()).x);
	ImVec4 delay_color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
	if (delay_frames >= 13)
		delay_color = ImVec4(1.f, 0.15f, 0.12f, 1.f);
	else if (delay_frames >= 10)
		delay_color = ImVec4(1.f, 0.5f, 0.f, 1.f);
	else if (delay_frames >= 5)
		delay_color = ImVec4(1.f, 0.85f, 0.f, 1.f);
	ImGui::PushStyleColor(ImGuiCol_Text, delay_color);
	ImGui::Text("%s", delay.c_str());
	ImGui::PopStyleColor();

	ImGui::Text("Roll: %d", stats[me].extra.total_rollbacked_frames);
	const auto wait = "Wait:" + std::to_string(stats[me].extra.total_timesync);
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(wait.c_str()).x);
	ImGui::Text("%s", wait.c_str());

	// Predicted Frames
	const int predicted_frames = std::clamp(stats[me].sync.predicted_frames, 0, 6);
	ImVec4 predicted_color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
	predicted_color.w = 0.75f;
	if (predicted_frames == 4)
		predicted_color = ImVec4(1.f, 0.85f, 0.f, 1.f);
	else if (predicted_frames == 5)
		predicted_color = ImVec4(1.f, 0.5f, 0.f, 1.f);
	else if (predicted_frames == 6)
		predicted_color = ImVec4(1.f, 0.15f, 0.12f, 1.f);
	ImGui::Text("P:");
	ImGui::SameLine();
	drawSegmentedMeter(6, predicted_frames, predicted_color);

	if (show_details) {
		for (int i = 0; i < matching.users_size(); ++i) {
			if (i == me) continue;
			drawDetailedPlayerNetworkStats(i, matching, stats[i], is_connected[i]);
		}
	} else {
		for (int i = 0; i < matching.users_size(); ++i) {
			if (i == me) continue;
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
			textCentered(std::to_string(i + 1) + "P:" + matching.users(i).user_id());
			textCentered(matching.users(i).user_name());
			ImGui::Text("Ping");
			if (is_connected[i] && stats[i].network.is_relay) {
				ImGui::SameLine();
				ImGui::Text("(Relay)");
			}
			std::string ping = is_connected[i] ? std::to_string(stats[i].network.ping) : "--";
			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(ping.c_str()).x);
			ImGui::PushStyleColor(ImGuiCol_Text, msColor(is_connected[i] ? stats[i].network.ping : 999).Value);
			ImGui::Text("%s", ping.c_str());
			ImGui::PopStyleColor();
			if (is_connected[i]) {
				drawLagMeter(health[i]);
			} else {
				ImGui::PushStyleColor(ImGuiCol_Text, msColor(999).Value);
				textCentered("Disconnected");
				ImGui::PopStyleColor();
			}
		}
	}
	ImGui::Spacing();

	panel_min = ImGui::GetWindowPos();
	panel_max = panel_min + ImGui::GetWindowSize();
	panel_bounds_valid = true;
	ImGui::PopStyleColor();
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(5);
	ImGui::PopFont();
}
}  // namespace
