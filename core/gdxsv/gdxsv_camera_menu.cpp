// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// The POV row of the replay's Tab menu, and the settings popup of the god's eye view.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "cfg/option.h"
#include "gdxsv_camera.h"
#include "gdxsv_camera_internal.h"
#include "gdxsv_config.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "oslib/i18n.h"
#include "ui/IconsFontAwesome6.h"
#include "ui/gui_util.h"

namespace {
struct Menu {
	bool shown = false;	 // the Tab menu drew the POV row this frame: its popup closes with the menu
	ImGuiID popup = 0;	 // the settings popup
	// The options edited in the popup, saved once no control is held, so that a slider drag writes the file once. Each is
	// saved by itself: what the panel changed, and the emulator's other settings, stay as they are in emu.cfg.
	std::vector<config::BaseOption *> edited;
};

Menu menu;

void ItemTip(const char *text) {
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
}

void Edited(config::BaseOption &option) {
	if (std::find(menu.edited.begin(), menu.edited.end(), &option) == menu.edited.end()) menu.edited.push_back(&option);
}

void Checkbox(const char *label, std::atomic<bool> &value, config::Option<bool> &option, const char *tip) {
	bool on = value;
	const bool changed = ImGui::Checkbox(label, &on);
	if (tip != nullptr) ItemTip(tip);
	if (!changed) return;
	value = on;
	option.set(on);
	Edited(option);
}

void Slider(const char *label, std::atomic<float> &value, config::Option<float> &option, float min, float max, const char *format) {
	float v = value;
	if (!ImGui::SliderFloat(label, &v, min, max, format, ImGuiSliderFlags_AlwaysClamp)) return;
	value = v;
	option.set(v);
	Edited(option);
}

void RenderSettings() {
	GdxsvCameraSettings &settings = gdxsv_camera_settings;
	const ImGuiStyle &style = ImGui::GetStyle();
	const float width = uiScaled(300.f);
	char title[96];
	std::snprintf(title, sizeof(title), i18n::T("%s settings"), i18n::T(gdxsv_camera_pov_mode_label(GdxsvPovMode::GodsEye)));
	ImGui::SeparatorText(title);

	const char *angle = i18n::T("Angle");
	const char *closest = i18n::T("Closest");
	const float slider = width - std::max(ImGui::CalcTextSize(angle).x, ImGui::CalcTextSize(closest).x) - style.ItemInnerSpacing.x;
	ImGui::SetNextItemWidth(slider);
	Slider(angle, settings.angle, config::GdxReplayGodsEyeAngle, 0.f, kGdxsvCameraAngleMax, "%.0f°");
	if (ImGui::IsItemActive()) gdxsv_camera_hold_distance();
	ItemTip(i18n::T("How far the view leans back from straight down"));
	ImGui::SetNextItemWidth(slider);
	Slider(closest, settings.closest, config::GdxReplayGodsEyeClosest, kGdxsvCameraClosestMin, kGdxsvCameraClosestMax, "%.0f");
	ItemTip(i18n::T("How close the camera may zoom in"));
	ImGui::Separator();

	// Two columns, each half the row. The options after Nameplates are parts of a nameplate.
	const float column = style.WindowPadding.x + (width + style.ItemSpacing.x) * 0.5f;
	Checkbox(i18n::T("Nameplates"), settings.nameplates, config::GdxReplayGodsEyeNameplates, i18n::T("Pilot name and HP above each MS"));
	ImGui::BeginDisabled(!settings.nameplates);
	ImGui::SameLine(column);
	Checkbox(i18n::T("Boost"), settings.boost, config::GdxReplayGodsEyeBoost, i18n::T("Boost gauge under each nameplate"));
	Checkbox(i18n::T("Team colors"), settings.team_colors, config::GdxReplayGodsEyeTeamColors, i18n::T("Color the HP gauges by team"));
	ImGui::SameLine(column);
	Checkbox(i18n::T("MS image"), settings.ms_image, config::GdxReplayGodsEyeMsImage, nullptr);
	Checkbox(i18n::T("Fade on overlap"), settings.fade, config::GdxReplayGodsEyeFade,
			 i18n::T("Fade a nameplate while it overlaps another MS"));
	ImGui::EndDisabled();
	ImGui::Separator();

	Checkbox(i18n::T("Trail"), settings.trail, config::GdxReplayGodsEyeTrail, nullptr);
	ImGui::SameLine();
	const char *seconds = i18n::T("seconds");
	ImGui::BeginDisabled(!settings.trail);
	ImGui::SetNextItemWidth(style.WindowPadding.x + width - ImGui::GetCursorPosX() - ImGui::CalcTextSize(seconds).x -
							style.ItemInnerSpacing.x);
	Slider(seconds, settings.trail_seconds, config::GdxReplayGodsEyeTrailSeconds, 1.f, kGdxsvCameraTrailSecondsMax, "%.0f");
	ImGui::EndDisabled();
}
}  // namespace

bool gdxsv_camera_render_pov_row(int pov_player, float cell, float height) {
	menu.shown = true;
	const ImGuiStyle &style = ImGui::GetStyle();
	const GdxsvPovMode mode = gdxsv_camera_pov_mode();
	char player[8];
	std::snprintf(player, sizeof(player), "%dP", pov_player + 1);
	// The two views side by side in the width of four round buttons, lit as the round buttons above when in use: the POV
	// player's as wide as its label needs, the god's eye view the rest. The gear takes the fifth.
	bool changed = false;
	const auto view_button = [&](GdxsvPovMode m, const std::string &label, float width) {
		const bool current = m == mode;
		if (current) ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
		if (ImGui::Button(label.c_str(), ImVec2(width, height)) && !current) {
			gdxsv_camera_set_pov_mode(m);
			changed = true;
		}
		if (current) ImGui::PopStyleColor();
	};
	const std::string player_label = std::string(ICON_FA_USER "  ") + player;
	const float player_w = ImGui::CalcTextSize(player_label.c_str()).x + style.FramePadding.x * 4.f;
	view_button(GdxsvPovMode::Player, player_label + "##pov-player", player_w);
	ImGui::SameLine();
	view_button(GdxsvPovMode::GodsEye, std::string(ICON_FA_EYE "  ") + i18n::T("God's Eye") + "##pov-gods-eye",
				cell * 4.f + style.ItemSpacing.x * 2.f - player_w);

	ImGui::SameLine();
	const bool gods_eye = mode == GdxsvPovMode::GodsEye;
	const bool settings_open = ImGui::IsPopupOpen("##pov-settings");
	if (settings_open) ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
	ImGui::BeginDisabled(!gods_eye);
	if (ImGui::Button(ICON_FA_GEAR "##pov-gear", ImVec2(cell, height))) ImGui::OpenPopup("##pov-settings");
	ImGui::EndDisabled();
	if (settings_open) ImGui::PopStyleColor();
	const float gear_top = ImGui::GetItemRectMin().y;
	char tip[96];
	if (gods_eye)
		std::snprintf(tip, sizeof(tip), i18n::T("%s settings"), i18n::T(gdxsv_camera_pov_mode_label(mode)));
	else
		std::snprintf(tip, sizeof(tip), i18n::T("No settings for %s"), player);
	ItemTip(tip);

	// Beside the menu, its top on the gear.
	menu.popup = ImGui::GetID("##pov-settings");
	ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x + style.ItemSpacing.x, gear_top));
	if (ImGui::BeginPopup("##pov-settings")) {
		if (gods_eye)
			RenderSettings();
		else
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (!menu.edited.empty() && !ImGui::IsAnyItemActive()) {
		config::setAutoSave(false);
		for (config::BaseOption *option : menu.edited) option->save();
		config::setAutoSave(true);	// writes emu.cfg once
		menu.edited.clear();
	}
	return changed;
}

void gdxsv_camera_pov_row_hidden() {
	if (!menu.shown) return;
	menu.shown = false;
	// The settings popup and any popup opened from it; the others are not the POV row's.
	const ImGuiContext &g = *ImGui::GetCurrentContext();
	for (int n = 0; n < g.OpenPopupStack.Size; n++) {
		if (g.OpenPopupStack[n].PopupId != menu.popup) continue;
		ImGui::ClosePopupToLevel(n, true);
		break;
	}
}
