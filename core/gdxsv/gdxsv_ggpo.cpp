// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_ggpo.h"

#include "build.h"
#include "gdxsv_ggpo_hooks.h"

GdxsvGgpoState gdxsv_ggpo_state;

#ifdef USE_GGPO
#include <cstdlib>
#include <cstring>
#include <thread>
#include <type_traits>

#include "cfg/option.h"
#include "emulator.h"
#include "gdxsv_sleep.h"
#include "ggponet.h"
#include "sync.h"
#include "input/gamepad_device.h"
#include "log/Log.h"
#include "network/ggpo.h"
#include "ui/gui_util.h"

static_assert(std::is_same_v<GGPOPlayerHandle, int>);
static_assert(sizeof(GdxsvGgpoNetworkStats) >= sizeof(GGPONetworkStats));

namespace {
// Default port of player i is kServerPort + i, as in core/network/ggpo.cpp.
constexpr int kServerPort = 19713;

void StartSession(const char* sessionCode, int me, const std::vector<std::string>& ips, const std::vector<u16>& ports,
				  const std::vector<u8>& relays, const std::vector<GdxsvGgpoRelayServerAddr>& relayServers) {
	GGPOSessionCallbacks cb{};
	GdxsvGgpoUpstream u = gdxsv_ggpo_upstream(&cb);
	GdxsvGgpoState& s = gdxsv_ggpo_state;
	gdxsv_ggpo_session_start();
	s.inputBeforeAdvance = true;
	s.testLog = getenv("GGPO_TEST_LOG") != nullptr;
	s.testNoSkipReplay = getenv("GGPO_TEST_NO_SKIP_REPLAY") != nullptr;
	memset(s.playerHandles, 0, sizeof(s.playerHandles));
	s.disconnectFlags = 0;
	s.inputBlockCount.fill(0);
	s.useExInput = true;
	ggpo::setExInput(0);
	ggpo::setExInputFlags(0);
	*u.analogAxes = 2;
	*u.inputSize = sizeof(kcode[0]) + sizeof(u16) + *u.analogAxes;	// kcode, exInput, analog axes
	NOTICE_LOG(NETWORK, "inputSize:%d", *u.inputSize);

	*u.localPlayerNum = me;
	GGPOErrorCode result = ggpo_start_session(u.session, &cb, settings.content.gameId.c_str(), ips.size(), *u.inputSize, ports[me],
											  sessionCode, strlen(sessionCode));
	if (result != GGPO_OK) {
		WARN_LOG(NETWORK, "GGPO start session failed: %d", result);
		*u.session = nullptr;
		throw FlycastException("GGPO network initialization failed");
	}
	GGPOSession* session = *u.session;

	NOTICE_LOG(NETWORK, "LOCAL PLAYER: %d", me);

	ggpo_set_disconnect_timeout(session, 10000);
	ggpo_set_disconnect_notify_start(session, 1000);
	ggpo_set_disconnect_without_rollback(session, true);
	for (const auto& server : relayServers) {
		ggpo_add_relay_server(session, server.ip.c_str(), server.port, server.altIp.c_str());
	}

	GGPOPlayer player{sizeof(GGPOPlayer), GGPO_PLAYERTYPE_LOCAL, me + 1};
	result = ggpo_add_player(session, &player, &s.playerHandles[me]);
	if (result != GGPO_OK) {
		WARN_LOG(NETWORK, "GGPO cannot add local player: %d", result);
		ggpo::stopSession();
		return;
	}
	*u.localPlayer = s.playerHandles[me];

	for (int i = 0; i < (int)ips.size(); i++) {
		if (i == me) continue;
		GGPOPlayer player{sizeof(GGPOPlayer), GGPO_PLAYERTYPE_REMOTE, i + 1};
		if (ips[i].empty()) {
			strcpy(player.u.remote.ip_address, "127.0.0.1");
		} else {
			strcpy(player.u.remote.ip_address, ips[i].c_str());
		}

		if (ports[i] == 0) {
			player.u.remote.port = kServerPort + i;
		} else {
			player.u.remote.port = ports[i];
		}

		if (relays[i]) {
			player.u.remote.relay = true;
		}

		result = ggpo_add_player(session, &player, &s.playerHandles[i]);
		if (result != GGPO_OK) {
			WARN_LOG(NETWORK, "GGPO cannot add remote player: %d", result);
			ggpo::stopSession();
			return;
		}

		*u.remotePlayer = s.playerHandles[i];
	}

	if (getenv("GGPO_DELAY") != nullptr) {
		const int v = atoi(getenv("GGPO_DELAY"));
		if (0 <= v && v <= 64) {
			config::GGPODelay.override(v);
		}
	}
	ggpo_set_frame_delay(session, *u.localPlayer, gdxsv_ggpo_frame_delay(config::GGPODelay.get()));

	DEBUG_LOG(NETWORK, "GGPO session started");
}

// Pumps the GGPO network while the emulation thread is busy elsewhere.
void IdleLoop() {
	GdxsvGgpoUpstream u = gdxsv_ggpo_upstream(nullptr);
	while (ggpo::active()) {
		{
			std::lock_guard<std::recursive_mutex> lock(*u.mutex);
			if (*u.session == nullptr) return;
			ggpo_idle(*u.session, -1);	// only pump
		}
		gdxsv_sleep_us(1000);
	}
}
}  // namespace

std::future<bool> gdxsv_ggpo_start_network(const char* sessionCode, int me, const std::vector<std::string>& ips,
										   const std::vector<u16>& ports, const std::vector<u8>& relays,
										   const std::vector<GdxsvGgpoRelayServerAddr>& relayServers) {
	GdxsvGgpoUpstream u = gdxsv_ggpo_upstream(nullptr);
	*u.synchronized = false;
	const std::string session_code(sessionCode);
	return std::async(std::launch::async, [=] {
		{
			std::lock_guard<std::recursive_mutex> lock(*u.mutex);
			StartSession(session_code.c_str(), me, ips, ports, relays, relayServers);
		}
		while (!*u.synchronized && ggpo::active()) {
			{
				std::lock_guard<std::recursive_mutex> lock(*u.mutex);
				if (*u.session == nullptr) break;
				GGPOErrorCode result = ggpo_idle(*u.session, 0);
				if (result == GGPO_ERRORCODE_VERIFICATION_ERROR)
					throw FlycastException("Peer verification failed");
				else if (result != GGPO_OK) {
					WARN_LOG(NETWORK, "ggpo_idle failed %d", result);
					throw FlycastException("GGPO error");
				}
			}
			gdxsv_sleep_us(20 * 1000);
		}
		emu.setNetworkState(ggpo::active());

		if (*u.synchronized && ggpo::active()) {
			std::thread(IdleLoop).detach();
		}
		return ggpo::active();
	});
}

bool gdxsv_ggpo_wait_prediction_barrier() {
	GdxsvGgpoUpstream u = gdxsv_ggpo_upstream(nullptr);
	GdxsvGgpoState& s = gdxsv_ggpo_state;
	for (int loop_count = 0; ggpo::active(); loop_count++) {
		int frame = 0;
		GGPONetworkStats stats{};
		ggpo_get_current_frame(*u.session, &frame);
		if (ggpo_get_network_stats(*u.session, *u.remotePlayer, &stats) != GGPO_OK) return true;
		// The check of ggpo_add_local_input, at the frame the next input is added at
		if (frame < MAX_PREDICTION_FRAMES || stats.sync.predicted_frames < MAX_PREDICTION_FRAMES) {
			// Counted as ggpo::nextFrame counts its prediction barrier waits
			if (0 < loop_count) {
				if (loop_count * 5 <= 30)
					s.inputBlockCount[0]++;
				else if (loop_count * 5 <= 500)
					s.inputBlockCount[1]++;
				else
					s.inputBlockCount[2]++;
			}
			if (2 < loop_count) NOTICE_LOG(NETWORK, "prediction barrier reached looped %dms", loop_count * 5);
			return true;
		}
		gdxsv_sleep_us(5 * 1000);
		if (ggpo_idle(*u.session, 0) != GGPO_OK) return false;
	}
	return true;
}

bool gdxsv_ggpo_is_connected(int playerNum) {
	if (playerNum == *gdxsv_ggpo_upstream(nullptr).localPlayerNum) return true;
	return !(gdxsv_ggpo_state.disconnectFlags >> playerNum & 1);
}

void gdxsv_ggpo_disconnect(int playerNum) {
	GdxsvGgpoUpstream u = gdxsv_ggpo_upstream(nullptr);
	std::lock_guard<std::recursive_mutex> lock(*u.mutex);
	if (*u.session == nullptr) {
		return;
	}
	ggpo_disconnect_player(*u.session, gdxsv_ggpo_state.playerHandles[playerNum]);
}

void gdxsv_ggpo_random_input(bool enable, u64 seed, u32 inputMask) {
	gdxsv_ggpo_state.useRandInput = enable;
	gdxsv_ggpo_state.randInputMask = inputMask;
	gdxsv_ggpo_state.randSource.seed(seed);
}

void gdxsv_ggpo_get_network_stats(int playerNum, GdxsvGgpoNetworkStats* stats) {
	GdxsvGgpoUpstream u = gdxsv_ggpo_upstream(nullptr);
	std::lock_guard<std::recursive_mutex> lock(*u.mutex);
	if (*u.session == nullptr) {
		return;
	}
	const GdxsvGgpoState& s = gdxsv_ggpo_state;
	stats->extra.total_rollbacked_frames = s.totalRollbackFrames;
	stats->extra.total_timesync = s.totalTimeSync;
	stats->extra.current_timesync = ggpo::timeSyncFrames;
	stats->extra.current_fps = s.fpsAvg;
	stats->extra.input_block_count[0] = s.inputBlockCount[0];
	stats->extra.input_block_count[1] = s.inputBlockCount[1];
	stats->extra.input_block_count[2] = s.inputBlockCount[2];
	ggpo_get_network_stats(*u.session, s.playerHandles[playerNum], (GGPONetworkStats*)stats);
}

bool gdxsv_ggpo_display_stats() {
	const GdxsvGgpoState& s = gdxsv_ggpo_state;
	if (!s.useExInput) return false;  // upstream's 2-player session
	GdxsvGgpoUpstream u = gdxsv_ggpo_upstream(nullptr);

	GGPONetworkStats stats;
	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
	ImguiStyleVar _1(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::SetNextWindowPos(ImVec2(10, 10));
	ImGui::SetNextWindowSize(ScaledVec2(95, 0));
	ImGui::SetNextWindowBgAlpha(0.7f);
	ImGui::Begin("##ggpostats", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs);
	ImguiStyleColor _2(ImGuiCol_PlotHistogram, ImVec4(0.557f, 0.268f, 0.965f, 1.f));

	{
		ggpo_get_network_stats(*u.session, *u.localPlayerNum, &stats);
		// Frame Delay
		ImGui::Text("Delay");
		std::string delay = std::to_string(config::GGPODelay.get());
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(delay.c_str()).x);
		ImGui::Text("%s", delay.c_str());

		// Predicted Frames
		if (stats.sync.predicted_frames >= 7)
			// red
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(1, 0, 0, 1));
		else if (stats.sync.predicted_frames >= 5)
			// yellow
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(.9f, .9f, .1f, 1));
		ImGui::Text("Predicted");
		ImGui::ProgressBar(stats.sync.predicted_frames / 7.f, ImVec2(-1, uiScaled(10.f)), "");
		if (stats.sync.predicted_frames >= 5) ImGui::PopStyleColor();
	}

	for (int i = 0; i < (int)std::size(s.playerHandles); i++) {
		if (i == *u.localPlayerNum) continue;
		if (s.playerHandles[i] == 0) continue;
		ggpo_get_network_stats(*u.session, s.playerHandles[i], &stats);

		ImGui::Text("-- %dP --", i + 1);

		// Ping
		ImGui::Text("Ping");
		std::string ping = std::to_string(stats.network.ping);
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(ping.c_str()).x);
		ImGui::Text("%s", ping.c_str());

		ImGui::Text("Loss");
		std::string loss = std::to_string(stats.network.recv_packet_loss);
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(loss.c_str()).x);
		ImGui::Text("%s", loss.c_str());

		// Send Queue
		ImGui::Text("Send Q");
		ImGui::ProgressBar(stats.network.send_queue_len / 10.f, ImVec2(-1, uiScaled(10.f)), "");

		// Frames behind
		int timesync = *u.timesyncOccurred;
		if (timesync > 0) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0, 0, 1));
		ImGui::Text("Behind");
		ImGui::ProgressBar(0.5f + stats.timesync.local_frames_behind / 16.f, ImVec2(-1, uiScaled(10.f)), "");
		if (timesync > 0) {
			ImGui::PopStyleColor();
			(*u.timesyncOccurred)--;
		}
	}

	ImGui::End();
	return true;
}

#else  // USE_GGPO

std::future<bool> gdxsv_ggpo_start_network(const char*, int, const std::vector<std::string>&, const std::vector<u16>&,
										   const std::vector<u8>&, const std::vector<GdxsvGgpoRelayServerAddr>&) {
	std::promise<bool> started;
	started.set_value(false);
	return started.get_future();
}
bool gdxsv_ggpo_wait_prediction_barrier() { return true; }
bool gdxsv_ggpo_is_connected(int) { return false; }
void gdxsv_ggpo_disconnect(int) {}
void gdxsv_ggpo_random_input(bool, u64, u32) {}
void gdxsv_ggpo_get_network_stats(int, GdxsvGgpoNetworkStats*) {}
bool gdxsv_ggpo_display_stats() { return false; }

#endif	// USE_GGPO
