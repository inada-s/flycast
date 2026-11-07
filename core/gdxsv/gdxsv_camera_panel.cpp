// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// The god's eye view's CAMERA panel in the in-game radar's place, and its controls: a drag on the picture, the d-pad, B,
// X and Y, and the 1 to 5 keys.
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>

#include "cfg/option.h"
#include "gdxsv_camera.h"
#include "gdxsv_camera_internal.h"
#include "imgui.h"
#include "input/gamepad.h"
#include "input/gamepad_device.h"
#include "input/keyboard_device.h"
#include "input/mouse.h"
#include "oslib/i18n.h"
#include "ui/IconsFontAwesome6.h"
#include "ui/gui_util.h"
#ifdef USE_SDL
#include "SDL.h"
#endif

extern ImFont *boldFont;

namespace {
// Degrees per window point dragged, and per second a button is held.
constexpr float kOrbitYawPerPoint = 0.6f;
constexpr float kOrbitTiltPerPoint = 0.4f;
constexpr float kSteerYawPerSecond = 90.f;
constexpr float kSteerTiltPerSecond = 30.f;
constexpr float kSteerEaseSeconds = 0.12f;
// Up zooms in and down out: the factor halves or doubles per second held.
constexpr float kZoomPerSecond = 0.6931f;
// Four notches of a mouse wheel halve or double it, a trackpad's scrolling a third as fast; either glides there.
constexpr float kZoomPerWheelStep = 0.1733f;
constexpr float kTrackpadShare = 1.f / 3.f;
constexpr u32 kTrackpadMs = 300;  // a whole step this soon after a fraction of one is the trackpad's too
constexpr float kWheelEaseSeconds = 0.1f;
// The framing moves to the players picked this long after the last pick, and picks this close together add up.
constexpr double kFocusDelaySeconds = 0.5;
// A player button pressed by the mouse or its key shows its color solid, then fades back.
constexpr float kPressHoldSeconds = 0.1f;
constexpr float kPressFadeSeconds = 0.5f;
// The panel, in the game's 640x480 pixels: the in-game radar's block, a title band over the body, and a row of player
// buttons under it. In the body, the circle leaves a lane each side, the zoom slider's on the right.
constexpr float kPanelLeft = 517.f;
constexpr float kPanelRight = 625.f;
constexpr float kPanelTop = 19.2f;
constexpr float kPanelBottom = 138.7f;
constexpr float kPanelTitle = 9.6f;
constexpr float kPanelInset = 5.f;
constexpr float kPanelGap = 3.f;
constexpr float kPanelReadout = 10.f;
constexpr float kPanelLane = 14.f;
constexpr float kPanelKnob = 5.5f;	// a quarter-turn knob's radius
constexpr float kPanelLabel = 7.5f;
constexpr float kPanelValue = 9.5f;
constexpr float kPanelFocus = 12.f;
constexpr float kPanelZoomTrack = 2.5f;	 // the zoom slider's track width; its handle, a pill
constexpr float kPanelZoomHandleW = 9.f;
constexpr float kPanelZoomHandleH = 5.f;
constexpr float kPi = 3.14159265f;

struct Panel {
	bool orbiting = false;					   // the view is being dragged
	bool zooming = false;					   // the zoom slider is being dragged
	bool steering = false;					   // a button turned, tilted or zoomed the view last time
	float steer_turn = 0.f, steer_tilt = 0.f;  // degrees per second the buttons turn and tilt it
	float steer_zoom = 0.f;					   // its zoom's log per second
	std::chrono::steady_clock::time_point steer_time;
	u32 focus_shown = kGdxsvCameraEveryone;	 // the players picked; the framing follows them from focus_at
	double focus_at = 0.0;
	bool focus_key[5] = {};	  // [1] to [4] and [All]: their keys held last frame
	bool step_held = false;	  // B, when the buttons were last read
	bool start_held = false;  // START, likewise
	int focus_steps = 0;	  // B presses the panel has yet to take: it knows which players are there
	int wheel_frame = -1;	  // the frame the wheel was last taken in
	float wheel_glide = 0.f;  // log of the zoom the wheel has yet to glide by
	// The ImGui time each of [1] to [4] and [All] was last pressed.
	double pressed_at[5] = {-1e9, -1e9, -1e9, -1e9, -1e9};
	u32 replay = 0;	 // gdxsv_camera_settings.replay the picks were made in
};

Panel panel;

void ItemTip(const char *text) {
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
}

// The mouse wheel as SDL reports it: a mouse wheel turns a whole step at a time, a trackpad scrolls in fractions of one,
// which the emulator's mouse rounds away.
struct Wheel {
	bool watching = false;
	std::atomic<float> pending{0.f};  // steps scrolled up since the panel last took them, the trackpad's at its share
	u32 trackpad_at = 0;			  // SDL's time of the trackpad's last fraction of a step
};

Wheel wheel;

#ifdef USE_SDL
int WatchWheel(void *, SDL_Event *event) {
	if (event->type != SDL_MOUSEWHEEL) return 1;
	const float y = event->wheel.preciseY;
	if (y != std::trunc(y)) wheel.trackpad_at = event->wheel.timestamp;
	const bool trackpad = wheel.trackpad_at != 0 && event->wheel.timestamp - wheel.trackpad_at < kTrackpadMs;
	const float steps = trackpad ? y * kTrackpadShare : y;
	float pending = wheel.pending;
	while (!wheel.pending.compare_exchange_weak(pending, pending + steps)) {
	}
	return 1;
}
#endif

// Steps scrolled up since the frame before; none when the panel skipped a frame. At most the whole zoom range a frame.
float WheelSteps() {
#ifdef USE_SDL
	if (!wheel.watching) {
		SDL_AddEventWatch(WatchWheel, nullptr);
		wheel.watching = true;
	}
#endif
	const float steps = wheel.pending.exchange(0.f);
	const bool next = panel.wheel_frame == ImGui::GetFrameCount() - 1;
	panel.wheel_frame = ImGui::GetFrameCount();
	return next ? std::clamp(steps, -16.f, 16.f) : 0.f;
}

void Turn(float degrees) {
	float yaw = std::fmod(gdxsv_camera_settings.yaw + degrees, 360.f);
	if (yaw < 0.f) yaw += 360.f;
	gdxsv_camera_settings.yaw = yaw;
}

void Tilt(float degrees) { gdxsv_camera_settings.angle = std::clamp(gdxsv_camera_settings.angle + degrees, 0.f, kGdxsvCameraAngleMax); }

// Back to every MS at the automatic zoom, at once.
void FocusAtOnce(u32 shown) {
	panel.focus_shown = shown;
	panel.focus_at = 0.0;
	gdxsv_camera_settings.focus = shown;
	gdxsv_camera_settings.zoom = 1.f;
	panel.wheel_glide = 0.f;
}

void FocusEveryone() { FocusAtOnce(kGdxsvCameraEveryone); }

// B: the next player alone, at once, and everyone after the last; from everyone or several players, the first.
void FocusStep(const GdxsvCameraBattle &battle) {
	int from = -1;
	for (int i = 0; i < 4; i++) {
		if (panel.focus_shown == 1u << i) from = i;
	}
	for (int i = from + 1; i < 4; i++) {
		if (battle.name[i].empty()) continue;
		FocusAtOnce(1u << i);
		return;
	}
	FocusEveryone();
}

// Picks less than kFocusDelaySeconds apart add players up or take them away. A pick after that starts anew: that player
// alone, or everyone again for the player already followed alone. No one is everyone.
void FocusPick(int player) {
	const u32 bit = 1u << player;
	const double now = ImGui::GetTime();
	u32 shown;
	if (now < panel.focus_at)
		shown = panel.focus_shown == kGdxsvCameraEveryone ? bit : panel.focus_shown ^ bit;
	else
		shown = panel.focus_shown == bit ? kGdxsvCameraEveryone : bit;
	panel.focus_shown = shown == 0 ? kGdxsvCameraEveryone : shown;
	panel.focus_at = now + kFocusDelaySeconds;
}

// Whether a slot's key is held: 1 to 4 for [1] to [4] and 5 for [All], on the main row or the numpad, in the same place
// on every layout.
bool SlotKeyHeld(int slot) {
#ifdef USE_SDL
	const Uint8 *keys = SDL_GetKeyboardState(nullptr);
	return keys != nullptr && (keys[SDL_SCANCODE_1 + slot] || keys[SDL_SCANCODE_KP_1 + slot]);
#else
	return false;
#endif
}

// How much of a player button's press flash is left: solid at first, then fading back.
float PressFlash(int slot) {
	const float since = static_cast<float>(ImGui::GetTime() - panel.pressed_at[slot]);
	const float t = std::clamp((since - kPressHoldSeconds) / kPressFadeSeconds, 0.f, 1.f);
	return 1.f - t * t * (3.f - 2.f * t);
}

}  // namespace

void gdxsv_camera_panel_reset() { panel = Panel{}; }

void gdxsv_camera_render_view_control() {
	// A new replay follows everyone, as gdxsv_camera_replay_started has set: the last one's picks go.
	if (panel.replay != gdxsv_camera_settings.replay) {
		panel.focus_shown = kGdxsvCameraEveryone;
		panel.focus_at = 0.0;
		panel.focus_steps = 0;
		panel.wheel_glide = 0.f;
		panel.replay = gdxsv_camera_settings.replay;
	}
	if (!gdxsv_camera_gods_eye()) return;
	// Outside a battle B presses and the wheel are dropped. ImGui gets the wheel only over its windows, so it comes from SDL.
	const int steps = panel.focus_steps;
	panel.focus_steps = 0;
	const float wheel = WheelSteps();
	GdxsvCameraBattle battle;
	if (!gdxsv_camera_overlay_battle(battle)) return;
	GdxsvCameraSettings &settings = gdxsv_camera_settings;

	// A drag anywhere on the picture turns and tilts the view, and lasts until the button is let go, wherever the mouse
	// goes: the replay's other windows can take ImGui's focus, and with it an item's active state.
	const ImGuiIO &io = ImGui::GetIO();
	if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse) panel.orbiting = true;

	// Right of the TIME block, where the god's eye view hides the radar, laid out in the game's pixels so that it scales
	// and slides with the HUD.
	const ImVec2 display = io.DisplaySize;
	const float picture_aspect = gdxsv_camera_picture_aspect(display.x / display.y);
	const float picture_h = display.x / display.y >= picture_aspect ? display.y : display.x / picture_aspect;
	const float px = picture_h / 480.f;
	const float px_x = config::Widescreen ? px : px * config::ScreenStretching / 100.f;
	const float cockpit_left = display.x * 0.5f - 320.f * px_x;
	const ImVec2 pos(cockpit_left + (kPanelLeft + battle.timer_slide) * px_x, (display.y - picture_h) * 0.5f + kPanelTop * px);
	const ImVec2 size((kPanelRight - kPanelLeft) * px_x, (kPanelBottom - kPanelTop) * px);
	const float title_h = kPanelTitle * px;
	const float inset = kPanelInset * px;
	const float gap = kPanelGap * px;
	const float readout_h = kPanelReadout * px;
	const float focus_h = kPanelFocus * px;
	// The readouts at the body's foot; the circle between them and the title band, as large as the zoom slider's lane
	// beside it lets it be.
	const float readout_y = pos.y + size.y - gap - readout_h;
	const float body_top = pos.y + title_h + gap;
	const float radius = std::min(size.x * 0.5f - kPanelLane * px_x, (readout_y - gap - body_top) * 0.5f);
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(size.x, size.y + gap + focus_h));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.122f, 0.122f, 0.122f, 0.055f));
	ImGui::Begin("##gods-eye-camera", nullptr,
				 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
					 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(3);
	// The wheel zooms over the picture and the panel, as a map does: what the system calls scrolling up brings the view
	// nearer, so its natural scrolling setting holds. The zoom glides there, as with the buttons, and stops at its ends.
	if (wheel != 0.f && (!io.WantCaptureMouse || ImGui::IsWindowHovered())) {
		panel.wheel_glide -= wheel * kZoomPerWheelStep;
	}
	if (panel.wheel_glide != 0.f) {
		gdxsv_camera_hold_distance();
		const float part = std::fabs(panel.wheel_glide) < 1e-3f ? panel.wheel_glide
																: panel.wheel_glide * (1.f - std::exp(-io.DeltaTime / kWheelEaseSeconds));
		const float zoom = std::clamp(settings.zoom * std::exp(part), kGdxsvCameraZoomMin, kGdxsvCameraZoomMax);
		settings.zoom = zoom;
		panel.wheel_glide = zoom == kGdxsvCameraZoomMin || zoom == kGdxsvCameraZoomMax ? 0.f : panel.wheel_glide - part;
	}
	ImDrawList *list = ImGui::GetWindowDrawList();
	ImFont *font = ImGui::GetFont();
	const ImVec2 max(pos.x + size.x, pos.y + size.y);
	list->AddRectFilled(pos, ImVec2(max.x, pos.y + title_h), ImGui::GetColorU32(ImVec4(0.122f, 0.122f, 0.122f, 0.16f)));
	ImGui::PushFont(boldFont, 0.f);
	const char *title = "CAMERA";
	const ImVec2 title_size = ImGui::GetFont()->CalcTextSizeA(title_h * 0.95f, FLT_MAX, 0.f, title);
	list->AddText(ImGui::GetFont(), title_h * 0.95f,
				  ImVec2(pos.x + (size.x - title_size.x) * 0.5f, pos.y + (title_h - title_size.y) * 0.5f),
				  ImGui::GetColorU32(ImVec4(0.925f, 0.941f, 1.f, 0.26f)), title);
	ImGui::PopFont();
	// The zoom, as a magnification, at the title band's right end.
	char magnification[16];
	std::snprintf(magnification, sizeof(magnification), "%.1f×", 1.f / settings.zoom);
	const float zoom_size = kPanelLabel * px;
	const ImVec2 zoom_text = font->CalcTextSizeA(zoom_size, FLT_MAX, 0.f, magnification);
	list->AddText(font, zoom_size, ImVec2(pos.x + size.x - inset - zoom_text.x, pos.y + (title_h - zoom_text.y) * 0.5f),
				  IM_COL32(255, 255, 255, 200), magnification);
	// At its left end, as a mode of the whole panel: whether the d-pad steers the view or the replay. Kept.
	const bool dpad = settings.dpad;
	const float pad_w = title_h * 2.f;
	ImGui::SetCursorScreenPos(pos);
	if (ImGui::InvisibleButton("##dpad", ImVec2(inset + pad_w, title_h))) {
		settings.dpad = !dpad;
		gdxsv_camera_keep_settings();
	}
	if (dpad)
		ItemTip(i18n::T("Camera mode: the d-pad turns and zooms the view. Click or press [START] for timeline mode"));
	else
		ItemTip(i18n::T("Timeline mode: the d-pad controls the replay. Click or press [START] for camera mode"));
	const bool pad_hot = ImGui::IsItemHovered();
	const float pad_size = title_h * 0.9f;
	const ImVec2 pad_glyph = font->CalcTextSizeA(pad_size, FLT_MAX, 0.f, ICON_FA_GAMEPAD);
	const ImVec2 pad_at(pos.x + inset, pos.y + (title_h - pad_glyph.y) * 0.5f);
	list->AddText(font, pad_size, pad_at, IM_COL32(255, 255, 255, dpad ? (pad_hot ? 255 : 215) : (pad_hot ? 170 : 105)), ICON_FA_GAMEPAD);
	if (!dpad)
		list->AddLine(ImVec2(pad_at.x, pad_at.y + pad_glyph.y * 0.85f), ImVec2(pad_at.x + pad_glyph.x, pad_at.y + pad_glyph.y * 0.15f),
					  IM_COL32(255, 120, 100, 230), std::max(1.f, 0.4f * px));

	// The orbit circle, as the radar's: the map as the camera sees it. Double-click for the default view. The knobs on
	// its rim come after it, over it.
	const ImVec2 center(pos.x + size.x * 0.5f, (body_top + readout_y - gap) * 0.5f);
	ImGui::SetCursorScreenPos(ImVec2(center.x - radius, center.y - radius));
	ImGui::SetNextItemAllowOverlap();
	ImGui::InvisibleButton("##orbit", ImVec2(radius * 2.f, radius * 2.f));
	if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) panel.orbiting = true;
	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
		settings.yaw = 0.f;
		settings.angle = kGdxsvCameraAngleDefault;
		settings.zoom = 1.f;
		panel.wheel_glide = 0.f;
	}
	ItemTip(i18n::T("Drag anywhere on the picture to turn and tilt the view. Double-click here for the default view"));
	if (panel.orbiting && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
		panel.orbiting = false;
		gdxsv_camera_keep_settings();
	}
	if (panel.orbiting) {
		gdxsv_camera_hold_distance();
		const float point = uiScaled(1.f);
		Turn(io.MouseDelta.x / point * kOrbitYawPerPoint);
		Tilt(-io.MouseDelta.y / point * kOrbitTiltPerPoint);
	}
	const float alpha = panel.orbiting || ImGui::IsItemHovered() ? 1.f : 0.75f;
	const auto color = [&](int r, int g, int b, float a) { return IM_COL32(r, g, b, static_cast<int>(a * alpha * 255.f)); };
	list->AddCircleFilled(center, radius, color(12, 16, 24, 0.4f), 64);
	list->AddCircle(center, radius - 0.5f, color(255, 255, 255, 0.45f), 64, std::max(1.5f, 1.2f * px));
	// A tile in perspective: u along the briefing map's up (+X), s across it (+Z).
	const float heading = settings.yaw * kPi / 180.f;
	const float pitch = settings.angle * kPi / 180.f;
	const float half = radius * 0.56f;
	const auto at = [&](float u, float s) {
		const float across = -u * std::sin(heading) + s * std::cos(heading);
		const float along = u * std::cos(heading) + s * std::sin(heading);
		const float near = 1.f / (1.f + 0.45f * along * std::sin(pitch));
		return ImVec2(center.x + across * half * near, center.y - along * std::cos(pitch) * half * near);
	};
	const ImVec2 tile[] = {at(1.f, -1.f), at(1.f, 1.f), at(-1.f, 1.f), at(-1.f, -1.f)};
	list->AddConvexPolyFilled(tile, 4, color(70, 120, 70, 0.7f));
	list->AddLine(at(0.f, -1.f), at(0.f, 1.f), color(255, 255, 255, 0.3f), std::max(1.f, 0.5f * px));
	list->AddLine(at(-1.f, 0.f), at(1.f, 0.f), color(255, 255, 255, 0.3f), std::max(1.f, 0.5f * px));
	list->AddPolyline(tile, 4, color(255, 255, 255, 0.9f), ImDrawFlags_Closed, std::max(1.5f, 1.f * px));
	// The briefing map's top: a marker beyond that edge.
	const ImVec2 edge = at(1.f, 0.f);
	const ImVec2 beyond = at(1.2f, 0.f);
	const float len = std::max(std::hypot(beyond.x - edge.x, beyond.y - edge.y), 0.01f);
	const ImVec2 out((beyond.x - edge.x) / len, (beyond.y - edge.y) / len);
	const float mark = 2.f * px;
	list->AddTriangleFilled(ImVec2(edge.x + out.x * mark * 1.6f, edge.y + out.y * mark * 1.6f),
							ImVec2(edge.x - out.y * mark, edge.y + out.x * mark), ImVec2(edge.x + out.y * mark, edge.y - out.x * mark),
							color(255, 220, 90, 1.f));
	// Round knobs, as the dial: quarter turns on the rim either side of its foot, below where the tile's near corners reach
	// at any tilt, and at the body's top left, clear of the circle, a ? that shows the controls guide while hovered.
	const float knob = kPanelKnob * px;
	const auto round_knob = [&](const char *id, const char *glyph, ImVec2 at) {
		ImGui::SetCursorScreenPos(ImVec2(at.x - knob, at.y - knob));
		const bool pressed = ImGui::InvisibleButton(id, ImVec2(knob * 2.f, knob * 2.f));
		const bool hot = ImGui::IsItemHovered();
		list->AddCircleFilled(at, knob, IM_COL32(12, 16, 24, hot ? 230 : 180), 24);
		list->AddCircle(at, knob - 0.5f, IM_COL32(255, 255, 255, hot ? 230 : 115), 24, std::max(1.f, 0.8f * px));
		const float glyph_size = knob * 1.2f;
		const ImVec2 g = font->CalcTextSizeA(glyph_size, FLT_MAX, 0.f, glyph);
		list->AddText(font, glyph_size, ImVec2(at.x - g.x * 0.5f, at.y - g.y * 0.5f), IM_COL32(255, 255, 255, hot ? 255 : 210), glyph);
		return pressed;
	};
	const float knob_y = center.y + radius * 0.866f;
	if (round_knob("##turn-left", ICON_FA_ROTATE_LEFT, ImVec2(center.x - radius * 0.5f, knob_y))) {
		const float to = 90.f - std::fmod(settings.yaw, 90.f);
		Turn(to > 0.5f ? to : to + 90.f);
	}
	ItemTip(i18n::T("Turn 90° left"));
	if (round_knob("##turn-right", ICON_FA_ROTATE_RIGHT, ImVec2(center.x + radius * 0.5f, knob_y))) {
		const float into = std::fmod(settings.yaw, 90.f);
		Turn(-(into > 0.5f ? into : into + 90.f));
	}
	ItemTip(i18n::T("Turn 90° right"));
	round_knob("##guide", "?", ImVec2(pos.x + inset + knob, body_top + knob));
	const bool guide = ImGui::IsItemHovered();

	// The zoom on a slider right of the circle, on a log scale: up is nearer, the middle the automatic zoom, which a
	// double-click goes back to. Dragged until the button is let go, as the view is.
	const float lane = size.x * 0.5f - radius;
	const float slider_x = center.x + radius + lane * 0.5f;
	const float handle_w = std::min(kPanelZoomHandleW * px, lane * 0.8f), handle_h = kPanelZoomHandleH * px;
	const float slider_top = center.y - radius + handle_h * 0.5f, slider_bottom = center.y + radius - handle_h * 0.5f;
	const float zoom_low = std::log(kGdxsvCameraZoomMin), zoom_span = std::log(kGdxsvCameraZoomMax) - zoom_low;
	ImGui::SetCursorScreenPos(ImVec2(center.x + radius, center.y - radius));
	ImGui::InvisibleButton("##zoom", ImVec2(lane, radius * 2.f));
	if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) panel.zooming = true;
	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
		panel.zooming = false;
		settings.zoom = 1.f;
		panel.wheel_glide = 0.f;
	}
	ItemTip(i18n::T("Zoom. Double-click for the automatic zoom"));
	if (panel.zooming && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) panel.zooming = false;
	if (panel.zooming) {
		panel.wheel_glide = 0.f;
		gdxsv_camera_hold_distance();
		const float t = std::clamp((io.MousePos.y - slider_top) / (slider_bottom - slider_top), 0.f, 1.f);
		settings.zoom = std::exp(zoom_low + t * zoom_span);
	}
	const float slider_alpha = panel.zooming || ImGui::IsItemHovered() ? 1.f : 0.75f;
	const auto faint = [&](float a) { return IM_COL32(255, 255, 255, static_cast<int>(a * slider_alpha * 255.f)); };
	const float track_w = kPanelZoomTrack * px;
	list->AddRectFilled(ImVec2(slider_x - track_w * 0.5f, slider_top), ImVec2(slider_x + track_w * 0.5f, slider_bottom), faint(0.2f),
						track_w * 0.5f);
	const float middle = (slider_top + slider_bottom) * 0.5f;
	list->AddLine(ImVec2(slider_x - handle_w * 0.35f, middle), ImVec2(slider_x + handle_w * 0.35f, middle), faint(0.35f),
				  std::max(1.f, 0.5f * px));
	const float zoom = settings.zoom;
	const float handle_y = slider_top + std::clamp((std::log(zoom) - zoom_low) / zoom_span, 0.f, 1.f) * (slider_bottom - slider_top);
	const ImVec2 handle_min(slider_x - handle_w * 0.5f, handle_y - handle_h * 0.5f);
	const ImVec2 handle_max(slider_x + handle_w * 0.5f, handle_y + handle_h * 0.5f);
	list->AddRectFilled(handle_min, handle_max, faint(0.8f), handle_h * 0.5f);
	list->AddRect(handle_min, handle_max, IM_COL32(0, 0, 0, static_cast<int>(0.35f * slider_alpha * 255.f)), handle_h * 0.5f, 0,
				  std::max(1.f, 0.4f * px));

	// At the body's foot: the tilt and the turn.
	const float label_size = kPanelLabel * px;
	const float value_size = kPanelValue * px;
	const auto readout = [&](const char *name, float degrees, bool right) {
		char value[16];
		std::snprintf(value, sizeof(value), "%.0f°", degrees);
		const float name_w = font->CalcTextSizeA(label_size, FLT_MAX, 0.f, name).x;
		const float value_w = font->CalcTextSizeA(value_size, FLT_MAX, 0.f, value).x;
		const float space = label_size * 0.4f;
		const float x = right ? pos.x + size.x - inset - name_w - space - value_w : pos.x + inset;
		// Label and value share a baseline.
		const float baseline = readout_y + value_size * 0.92f;
		list->AddText(font, label_size, ImVec2(x, baseline - label_size * 0.92f), ImGui::GetColorU32(ImVec4(0.925f, 0.941f, 1.f, 0.6f)),
					  name);
		list->AddText(font, value_size, ImVec2(x + name_w + space, readout_y), IM_COL32_WHITE, value);
	};
	readout("TILT", settings.angle, false);
	readout("TURN", settings.yaw, true);

	// [1] to [4] in each player's force color, then [All], faint over the picture: the picked players light up at once.
	// Their keys only with the gamepad toggle on, whatever the keyboard mapping says.
	for (int step = 0; step < steps; step++) FocusStep(battle);
	const float focus_y = pos.y + size.y + gap;
	const float focus_w = (size.x - inset * 2.f - gap * 4.f) / 5.f;
	for (int slot = 0; slot < 5; slot++) {
		const int i = slot < 4 ? slot : -1;	 // the player, or -1 for [All]
		const bool present = i < 0 || !battle.name[i].empty();
		const bool key = settings.dpad && SlotKeyHeld(slot);
		const bool key_pressed = key && !panel.focus_key[slot] && present && !io.WantTextInput;
		panel.focus_key[slot] = key;
		const ImVec2 min(pos.x + inset + slot * (focus_w + gap), focus_y);
		ImGui::SetCursorScreenPos(min);
		char id[16];
		std::snprintf(id, sizeof(id), "##focus-%d", slot);
		ImGui::BeginDisabled(!present);
		const bool clicked = ImGui::InvisibleButton(id, ImVec2(focus_w, focus_h));
		ImGui::EndDisabled();
		if (clicked || key_pressed) {
			panel.pressed_at[slot] = ImGui::GetTime();
			if (i < 0)
				FocusEveryone();
			else
				FocusPick(i);
		}
		const bool picked = present && (i < 0 ? panel.focus_shown == kGdxsvCameraEveryone : (panel.focus_shown >> i & 1) != 0);
		const bool hover = ImGui::IsItemHovered();
		if (present && hover) {
			char tip[96];
			if (i < 0)
				std::snprintf(tip, sizeof(tip), "%s", i18n::T("Every MS, at the automatic zoom (5 key)"));
			else
				std::snprintf(tip, sizeof(tip), "%dP %s", i + 1, battle.name[i].c_str());
			ItemTip(tip);
		}
		const ImU32 tint = (i < 0 ? IM_COL32_WHITE : gdxsv_camera_team_color(battle.team[i])) & ~IM_COL32_A_MASK;
		const float flash = PressFlash(slot);
		const float faint = (!present ? 0.04f : picked ? (hover ? 0.42f : 0.32f) : (hover ? 0.18f : 0.1f)) * (i < 0 ? 0.6f : 1.f);
		const float shade = faint + (1.f - faint) * flash;
		list->AddRectFilled(min, ImVec2(min.x + focus_w, min.y + focus_h), tint | static_cast<ImU32>(shade * 255.f) << IM_COL32_A_SHIFT);
		char digit[2] = {static_cast<char>('1' + i), 0};
		const char *text = i < 0 ? "All" : digit;
		const ImVec2 text_size = font->CalcTextSizeA(label_size, FLT_MAX, 0.f, text);
		// White on the forces' colors; [All]'s goes dark while it is solid white.
		const float opacity = (picked ? 235.f : present ? 120.f : 50.f) * (1.f - flash) + 255.f * flash;
		const int level = static_cast<int>(255.f - (i < 0 ? 225.f * flash : 0.f));
		list->AddText(font, label_size, ImVec2(min.x + (focus_w - text_size.x) * 0.5f, min.y + (focus_h - text_size.y) * 0.5f),
					  IM_COL32(level, level, level, static_cast<int>(opacity)), text);
	}
	// Other players frame at the automatic zoom: a zoom kept from the last ones may show none of them.
	if (panel.focus_shown != settings.focus && ImGui::GetTime() >= panel.focus_at) {
		settings.focus = panel.focus_shown;
		settings.zoom = 1.f;
	}
	ImGui::End();
	gdxsv_camera_render_guide(guide);
}

bool gdxsv_camera_dpad_steers() { return gdxsv_camera_gods_eye() && gdxsv_camera_settings.dpad; }

u32 gdxsv_camera_key_buttons() {
	u32 buttons = 0;
#ifdef USE_SDL
	if (!gdxsv_camera_dpad_steers()) return 0;
	const Uint8 *keys = SDL_GetKeyboardState(nullptr);
	if (keys == nullptr) return 0;
	for (int slot = 0; slot < 5; slot++) {
		for (const int code : {SDL_SCANCODE_1 + slot, SDL_SCANCODE_KP_1 + slot}) {
			if (!keys[code]) continue;
			// What the first player's keyboards map the key to, as Flycast's own check of a mapped key reads it.
			for (int i = 0; i < GamepadDevice::GetGamepadCount(); i++) {
				const std::shared_ptr<GamepadDevice> device = GamepadDevice::GetGamepad(i);
				if (dynamic_cast<KeyboardDevice *>(device.get()) == nullptr || device->maple_port() != 0) continue;
				const std::shared_ptr<InputMapping> mapping = device->get_input_mapping();
				if (mapping == nullptr) continue;
				const DreamcastKey button = mapping->get_button_id(0, InputMapping::InputSet{InputMapping::InputDef::from_button(code)});
				if (button != EMU_BTN_NONE && button <= DC_BTN_BITMAPPED_LAST) buttons |= button;
			}
		}
	}
#endif
	return buttons;
}

bool gdxsv_camera_takes_mouse() {
	// The first player's mouse buttons (pressed = bit clear), set by the same input event that presses the game button
	// the mouse stands for. ImGui would see the press only in its next frame.
	return gdxsv_camera_gods_eye() && (mo_buttons[0] & (1 << Mouse::LEFT_BUTTON)) == 0;
}

void gdxsv_camera_steer(u32 kcode) {
	GdxsvCameraSettings &settings = gdxsv_camera_settings;
	// Eased in and out: the speed follows the held directions over a short time. Timed here, since the caller runs once
	// per pass of the main loop, not once per drawn frame; a long gap starts again at rest.
	const auto now = std::chrono::steady_clock::now();
	float seconds = std::chrono::duration<float>(now - panel.steer_time).count();
	panel.steer_time = now;
	if (seconds > 0.25f) {
		seconds = 0.f;
		panel.steer_turn = panel.steer_tilt = panel.steer_zoom = 0.f;
	}
	gdxsv_camera_guide_buttons(kcode);
	// START flips the gamepad toggle, as a click on it does: the replay does not use START outside a takeover.
	const bool start = (~kcode & DC_BTN_START) != 0;
	if (start && !panel.start_held) {
		settings.dpad = !settings.dpad;
		gdxsv_camera_keep_settings();
	}
	panel.start_held = start;
	// Camera mode, the toggle on, gives the view the d-pad, X, Y and B; timeline mode gives the d-pad back to the replay,
	// which has no use for the others.
	const bool dpad = settings.dpad;
	const bool step = dpad && (~kcode & DC_BTN_B) != 0;
	if (step && !panel.step_held) panel.focus_steps++;
	panel.step_held = step;
	const float turn = dpad ? (~kcode & DC_DPAD_RIGHT ? 1.f : 0.f) - (~kcode & DC_DPAD_LEFT ? 1.f : 0.f) : 0.f;
	const float zoom = dpad ? (~kcode & DC_DPAD_DOWN ? 1.f : 0.f) - (~kcode & DC_DPAD_UP ? 1.f : 0.f) : 0.f;
	const float tilt = dpad ? (~kcode & DC_BTN_Y ? 1.f : 0.f) - (~kcode & DC_BTN_X ? 1.f : 0.f) : 0.f;
	const float follow = 1.f - std::exp(-seconds / kSteerEaseSeconds);
	panel.steer_turn += (turn * kSteerYawPerSecond - panel.steer_turn) * follow;
	panel.steer_tilt += (tilt * kSteerTiltPerSecond - panel.steer_tilt) * follow;
	panel.steer_zoom += (zoom * kZoomPerSecond - panel.steer_zoom) * follow;
	if (turn == 0.f && std::fabs(panel.steer_turn) < 1.f) panel.steer_turn = 0.f;
	if (tilt == 0.f && std::fabs(panel.steer_tilt) < 0.5f) panel.steer_tilt = 0.f;
	if (zoom == 0.f && std::fabs(panel.steer_zoom) < 0.01f) panel.steer_zoom = 0.f;
	Turn(panel.steer_turn * seconds);
	Tilt(panel.steer_tilt * seconds);
	if (panel.steer_zoom != 0.f)
		settings.zoom = std::clamp(settings.zoom * std::exp(panel.steer_zoom * seconds), kGdxsvCameraZoomMin, kGdxsvCameraZoomMax);
	// The angle is kept, as the slider keeps it, once the view comes to rest.
	const bool moving = panel.steer_turn != 0.f || panel.steer_tilt != 0.f || panel.steer_zoom != 0.f;
	if (moving) gdxsv_camera_hold_distance();
	if (panel.steering && !moving) gdxsv_camera_keep_settings();
	panel.steering = moving;
}
