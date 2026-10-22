#include "gdxsv_lobby_status.h"

#include "gdxsv_key_display.h"
#include "gdxsv_translation.h"
#include "imgui/imgui.h"
#include "stdclass.h"
#include "ui/gui_util.h"

void GdxsvLobbyStatus::DisplayOSD() {
	if (!enabled_) return;

	Status status;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		status = status_;
	}
	if (!status.entered) return;
	constexpr int kPlayers = 4;
	const std::string text = strprintf(GdxsvLanguage::gdxT("Matching... %d/%d"), status.waiting, kPlayers);

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	if (display.x <= 0 || display.y <= 0) return;
	const float scale = gdxsv_osd_scale(display.x, display.y);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * scale, 4 * scale));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5 * scale, 0));
	ImGui::PushFont(nullptr, 12 * scale);
	// Centered just above the replay seek bar (GdxsvBackendReplay::RenderControlBar: 36 high, 20 off the bottom).
	ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y - uiScaled(36.0f + 20.0f + 8.0f)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
	ImGui::SetNextWindowBgAlpha(0.7f);
	ImGui::Begin("##gdxsv_lobby_status", nullptr,
				 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
					 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
	const float line = ImGui::GetTextLineHeight();
	const float radius = line * 0.4f;
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const ImVec2 center(pos.x + radius, pos.y + line * 0.5f);
	const float angle = (float)ImGui::GetTime() * 6.0f;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->PathArcTo(center, radius, angle, angle + 4.5f, 16);
	draw->PathStroke(ImGui::GetColorU32(ImGuiCol_Text), 0, 1.5f * scale);
	ImGui::Dummy(ImVec2(radius * 2, line));
	ImGui::SameLine();
	ImGui::TextUnformatted(text.c_str());
	ImGui::End();
	ImGui::PopFont();
	ImGui::PopStyleVar(2);
}

void GdxsvLobbyStatus::Observe(const LbsMessage& msg) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (msg.command == LbsMessage::lbsReadyBattle) {
		status_.entered = false;
		return;
	}
	// Team (1: Federation, 2: Zeon) and its users waiting for a match.
	if (msg.command != LbsMessage::lbsLobbyMatchingJoin || msg.body.size() < 4) return;
	const int team = msg.body[0] << 8 | msg.body[1];
	if (team != 1 && team != 2) return;
	team_waiting_[team - 1] = msg.body[2] << 8 | msg.body[3];
	status_.waiting = team_waiting_[0] + team_waiting_[1];
}

void GdxsvLobbyStatus::ObserveSent(const LbsMessage& msg) {
	// The game's own entry (1) or its cancel (0).
	if (msg.command != LbsMessage::lbsLobbyMatchingEntry || msg.body.empty()) return;
	std::lock_guard<std::mutex> lock(mutex_);
	status_.entered = msg.body[0] == 1;
}

void GdxsvLobbyStatus::Clear() {
	std::lock_guard<std::mutex> lock(mutex_);
	status_ = {};
	team_waiting_[0] = team_waiting_[1] = 0;
}
