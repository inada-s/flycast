// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// The overlay of the god's eye view, drawn over the game in window coordinates: each player's nameplate, trail and lock pointer,
// and the view's own pilot name in the team gauge.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "cfg/option.h"
#include "gdxsv.h"
#include "gdxsv_camera_art.h"
#include "gdxsv_camera_internal.h"
#include "gdxsv_translation.h"
#include "imgui.h"
#include "libs.h"
#include "log/Log.h"
#include "oslib/resources.h"

namespace {
// The same on both discs; the rest is in gdxsv_camera_game(). An actor's boost (s16) is what is left of kBoostFull, its
// target what its lock mark is on, and its beyond-range flag set in space while that target is further than its lock range.
constexpr u8 kStateInPlay = 1;
constexpr u8 kPhaseControl = 3;
constexpr u8 kPhaseRoundOver = 4;
constexpr u32 kActorHpMax = 0x180;		 // actor: s16
constexpr u32 kActorHp = 0x182;			 // actor: s16, HP left
constexpr u32 kActorUnhittable = 0x194;	 // actor: nothing can damage it now, as when it lies down or gets up
constexpr u32 kActorMs = 0x1f02;		 // actor byte: the MS id
constexpr u32 kActorTeam = 0x1f05;		 // actor byte: 0 Federation, else Zeon
constexpr u32 kMarkState = 0x161;		 // lock mark object: 0 no target, 1 on it, 2 on its way
constexpr float kBoostFull = 4000.f;	 // for every MS and MA

// A HUD point sits at depth 0.2 in front of the eye, so the 4:3 screen spans +-0.1 and +-0.075.
constexpr float kHudDepth = 0.2f;
constexpr float kHudHalfHeight = 0.075f;
// Nameplates that would overlap move apart vertically, this many game pixels clear of each other.
constexpr float kNameplateGap = 2.f;
// A nameplate's height in HUD units; its anchor stays two of them below the top of the picture.
constexpr float kNameplateH = 0.0075f;
constexpr float kNameplateSettle = 0.15f;
// World units over the MS's feet where its nameplate is pinned, with its leader line: a little over its head.
constexpr float kNameplateAbove = kGdxsvCameraMsHeight + 1.f;

// A nameplate, in the game's 640x480 pixels from its anchor, laid out as the game's own target label: the pilot name
// on a rule, the HP gauge under it, a leader line from the MS to the rule's left end. Left of the name the force logo
// and, as an option, the MS image, above that line. The image's scale makes a Gelgoog as tall as the band.
constexpr float kNameplateBandTop = -53.4f;
constexpr float kNameplateBandBottom = -30.3f;
constexpr float kNameplateRule = -40.4f;
constexpr float kNameplateRuleLeft = 40.f;
constexpr float kNameplateLine = 0.5f;	// rule and leader line width
constexpr float kNameplateLineEdge = 0.4f;
constexpr float kNameplateNameLeft = 44.f;
constexpr float kNameplateNameSize = 10.8f;	 // a full-width character's advance
constexpr float kNameplateNameBaseline = -43.3f;
constexpr float kNameplateGaugeLeft = 45.8f;
constexpr float kNameplateGaugeRight = 86.6f;
constexpr float kNameplateGaugeTop = -37.3f;
constexpr float kNameplateGaugeBottom = -31.2f;
constexpr float kNameplateImageRight = 29.f;
constexpr float kNameplateImageScale = 0.348f;
constexpr float kNameplateLogoRight = 43.f;
constexpr float kNameplateLogoBottom = -41.6f;
constexpr float kNameplateLogoSize = 13.f;
// The name's font and outline as gdxsv:FontRaster draws the game's pilot names: GDXGothic Medium, outlined in
// see-through near black about a game pixel wide. Two rings of 8 copies add up to about 40% next to the glyph.
constexpr const char *kNameFontFile = "gdxsv/GDXGothic-Medium.otf";
constexpr float kNameOutline = 1.f;
constexpr ImU32 kNameOutlineColor = IM_COL32(30, 30, 30, 8);
// A thin boost gauge right under the HP gauge, in the game's colors, what is left on the right.
constexpr float kBoostGap = 1.f;
constexpr float kBoostHeight = 2.2f;
constexpr ImU32 kBoostLeftColor = IM_COL32(116, 250, 252, 255);
constexpr ImU32 kBoostUsedColor = IM_COL32(215, 25, 20, 190);
constexpr ImU32 kBoostBackColor = IM_COL32(0, 0, 0, 120);
// A whole nameplate fades while it is this close to another MS, and comes back once clear for a while. Nameplates do not
// fade for each other: they move apart.
constexpr float kNameplateClearance = 8.f;
constexpr float kNameplateHoldSeconds = 0.5f;
constexpr float kNameplateFadeOpacity = 0.3f;
constexpr float kNameplateFadeOutSeconds = 0.3f;
constexpr float kNameplateFadeInSeconds = 0.3f;

// By team: Federation, Zeon. The HP gauge: what is left, then what is lost; or the game's own colors.
constexpr ImU32 kTeamColors[2] = {IM_COL32(70, 165, 255, 255), IM_COL32(255, 60, 50, 255)};
constexpr ImU32 kHpColors[2][2] = {
	{IM_COL32(64, 153, 255, 255), IM_COL32(153, 153, 153, 128)},
	{IM_COL32(255, 51, 38, 255), IM_COL32(153, 153, 153, 128)},
};
constexpr ImU32 kHpGameColors[2] = {IM_COL32(255, 179, 0, 255), IM_COL32(255, 0, 0, 128)};
constexpr ImU32 kEdgeColor = IM_COL32(0, 0, 0, 150);

constexpr float kPi = 3.14159265f;
constexpr float kInputsPerSecond = 60.f;
constexpr int kTrailStep = 2;			 // replay inputs between trail samples
constexpr int kTrailJump = 60;			 // a jump of the replay position this large starts the trails over
constexpr float kTrailTeleport = 150.f;	 // world units: a move this large between samples is a respawn, not drawn
constexpr float kTrailGap = 10.f;		 // a trail stays this far from its own MS, to leave it visible
// Where a trail crosses another MS or a nameplate, dots of the trail's color, this much as opaque.
constexpr float kTrailCrossOpacity = 0.45f;
constexpr float kTrailDot = 0.6f;  // dot radius, in line widths
constexpr float kTrailDotPeriod = 2.5f;
constexpr double kTrailDotsMax = 2000;	// a stretch with more is left out: only a projection gone wrong gives one
constexpr float kTrailNearest = 10.f;	// world units ahead of the eye a trail point must be to be drawn
// World units: an MS seen from above covers a circle this wide; seen at an angle, a column of such circles up its height.
constexpr float kMsRadius = 9.f;
constexpr int kMsBodyCircles = 4;

// Where the game draws the POV player's name in the team gauge, as a fraction of the 4:3 picture. The god's eye view draws a
// pilot name of its own there, in the gdxsv language as the game's text.
constexpr float kPilotNameX = 0.083f;
constexpr float kPilotNameY = 0.131f;
constexpr float kPilotNameSize = 0.042f;

// The game's yellow cannot-damage mark, a circle with a flat arrow above and below pointing in, on an MS, reaches 18 from
// it. The lock pointer stays outside that while there is room, narrow and pointing out, so that the two do not mix.
// In the game's 640x480 pixels.
constexpr ImU32 kUnhittableColor = IM_COL32(255, 230, 0, 255);
constexpr float kMarkRadius = 12.f;
constexpr float kPointerBase = 20.f;
constexpr float kPointerTip = 34.f;
constexpr float kPointerHalfWidth = 4.f;
// Toward a close target the pointer keeps to its half of the way, so two players locked on each other have this gap
// between the tips; it moves in over the mark as far as its own MS, and shrinks rather than touch it.
constexpr float kPointerTipGap = 5.f;
constexpr float kPointerNearest = 8.f;
constexpr float kPointerSmallest = 0.25f;
// White, filled toward its target and faint except when it has just turned to a new one; its rim is this much of that.
// Without a target, an outline the way the player looks.
constexpr float kPointerRestOpacity = 0.28f;
constexpr float kPointerBlinkOpacity = 0.67f;
constexpr float kPointerRimOpacity = 0.53f;
constexpr float kPointerBlinkInputs = 30.f;
constexpr float kPointerLookOpacity = 0.55f;
constexpr float kPointerLookRimOpacity = 0.3f;

struct TrailPoint {
	GdxsvCameraVec3 at;
	int key;
	bool starts;  // not connected to the point before it
	float along;  // world distance from the first point, so that dots stay put on the path
};

struct PlayerView {
	bool present = false;
	bool on = false;  // in control of its MS
	bool unhittable = false;
	GdxsvCameraVec3 at;
	float look_x = 0.f, look_z = 0.f;  // the direction it looks, seen from above
	bool locking = false;
	GdxsvCameraVec3 target;
	float boost = 1.f;	// what is left, as a fraction of full; full on the ground
};

// Written by the emulator thread, drawn by the render thread.
struct Overlay {
	std::mutex mutex;
	bool valid = false;	 // the battle view is up
	int key = -1;		 // the replay position
	int last_sample = -1;
	std::deque<TrailPoint> trail[4];
	bool breaks[4] = {};
	GdxsvCameraVec3 eye, right, up, forward;
	float gauge_slide = 0.f;  // in HUD units: the battle start and end slide, and the widescreen HUD layout
	float timer_slide = 0.f;  // the same for the TIME block
	float push[4] = {};		  // in HUD units: each nameplate moved apart from others
	bool anchor_on[4] = {};
	float anchor_x[4] = {};	 // in HUD units
	float anchor_y[4] = {};
	int ms[4] = {};
	int team[4] = {};
	float hp[4] = {};  // of full
	std::string name[4];
	PlayerView view[4];
	u32 pointer_target[4] = {};
	int pointer_key[4] = {};  // replay position when the pointer last turned to a new target
};

// Render thread only.
struct Nameplates {
	float opacity[4] = {};
	double busy_until[4] = {};
	ImFont *font = nullptr;
	bool font_missing = false;
};

// Emulator thread: what stays the same through a replay, read once at its first update.
struct ReplayFacts {
	u32 replay = ~0u;  // gdxsv_camera_settings.replay they were read for
	u32 hud_left = 0;  // the payload's widescreen HUD offsets
	u32 hud_right = 0;
	std::string name[4];
};

// Each nameplate's extent as the render thread last laid it out, in game pixels from its anchor (down and right
// positive), read by the emulator thread to move nameplates apart. Until then: the logo, a short name and the HP gauge.
struct PlateExtent {
	std::atomic<float> left{kNameplateLogoRight - kNameplateLogoSize};
	std::atomic<float> right{kNameplateGaugeRight + 1.f};
	std::atomic<float> top{kNameplateLogoBottom - kNameplateLogoSize};
	std::atomic<float> bottom{kNameplateGaugeBottom};
};

Overlay overlay;
ReplayFacts facts;
Nameplates nameplates;
PlateExtent extents[4];

GdxsvCameraVec3 ReadVec3(u32 addr) {
	return {gdxsv_camera_read_float(addr), gdxsv_camera_read_float(addr + 4), gdxsv_camera_read_float(addr + 8)};
}

GdxsvCameraVec3 Minus(const GdxsvCameraVec3 &a, const GdxsvCameraVec3 &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

struct Cockpit {
	bool alive = false;		  // its timer object goes away when the results screen takes over from the battle view
	float gauge_slide = 0.f;  // how far the team gauge has slid from its place, at battle start and end
	float timer_slide = 0.f;  // and the TIME block
	u32 lock_mark[4] = {};	  // every player has a lock mark object; the game only draws the local player's
};

Cockpit ReadCockpit() {
	const GdxsvCameraGame &game = gdxsv_camera_game();
	Cockpit cockpit;
	u32 obj = gdxsv_ReadMem32(game.hud_list_head);
	for (int i = 0; i < 256 && (obj >> 24) == 0x0c; i++) {
		if (gdxsv_ReadMem8(obj) != 0) {
			const u8 family = gdxsv_ReadMem8(obj + 1);
			const u8 kind = gdxsv_ReadMem8(obj + 2);
			const u8 type = gdxsv_ReadMem8(obj + 3);
			if (family == 0x14 && kind == 0) {
				if (type == 0) {
					cockpit.alive = true;
					cockpit.timer_slide = gdxsv_camera_read_float(obj + 0x5c);
				}
				if (type == 6) cockpit.gauge_slide = gdxsv_camera_read_float(obj + 0x5c);
			}
			if (family == 0x12 && kind == 1 && type == 0) {
				const u32 owner = gdxsv_ReadMem32(obj + 0x18) - game.player_work;
				if (owner / kGdxsvCameraPlayerStride < 4) cockpit.lock_mark[owner / kGdxsvCameraPlayerStride] = obj;
			}
		}
		obj = gdxsv_ReadMem32(obj + 0x7c);
	}
	return cockpit;
}

// Trying to lock counts. In space the game also draws a green mark on a target beyond range, which does not.
bool ReadLocking(u32 actor, u32 mark, u32 target) {
	if (mark == 0 || (target >> 24) != 0x0c) return false;
	const u8 state = gdxsv_ReadMem8(mark + kMarkState);
	const u16 beyond = gdxsv_camera_game().actor_beyond_range;
	return state == 2 || (state == 1 && (beyond == 0 || gdxsv_ReadMem8(actor + beyond) == 0));
}

PlayerView ReadPlayerView(int player, const GdxsvCameraFrame::Player &p, u32 mark) {
	const GdxsvCameraGame &game = gdxsv_camera_game();
	const u32 actor = game.actor(player);
	PlayerView view;
	view.present = p.on;
	view.at = p.at;
	const bool alive = p.on && gdxsv_ReadMem8(actor + kGdxsvCameraActorState) == kStateInPlay;
	const u8 phase = gdxsv_ReadMem8(actor + kGdxsvCameraActorPhase);
	// Every player has the flag once a round has ended.
	view.unhittable = alive && phase != kPhaseRoundOver && gdxsv_ReadMem8(actor + kActorUnhittable) != 0;
	// Unlike the game's own gauge not on the ground, where a step uses boost too: this is to spot who is flying.
	if (alive && (gdxsv_ReadMem16(actor + game.actor_move_flags) & kGdxsvCameraMoveAirborne) != 0) {
		view.boost = std::clamp(static_cast<s16>(gdxsv_ReadMem16(actor + game.actor_boost)) / kBoostFull, 0.f, 1.f);
	}
	// Only while in control of the MS: before and after, its camera is the game's, for a landing or a wreck.
	if (!alive || phase != kPhaseControl) return view;
	// The view matrix is column major: its rows are the camera's axes, and it looks along the negative third.
	view.look_x = -gdxsv_camera_player_camera(player, game.cam_matrix + 0x08);
	view.look_z = -gdxsv_camera_player_camera(player, game.cam_matrix + 0x28);
	const u32 target = gdxsv_ReadMem32(actor + game.actor_target);
	view.locking = ReadLocking(actor, mark, target);
	if (view.locking) view.target = ReadVec3(target + kGdxsvCameraActorPosition);
	view.on = std::isfinite(view.look_x + view.look_z + view.target.x + view.target.y + view.target.z);
	return view;
}

void SampleTrails(const GdxsvCameraFrame &frame, int key) {
	if (overlay.last_sample >= 0 && key < overlay.last_sample + kTrailStep) return;
	overlay.last_sample = key;
	for (int i = 0; i < 4; i++) {
		const GdxsvCameraFrame::Player &p = frame.players[i];
		auto &trail = overlay.trail[i];
		while (!trail.empty() && (key - trail.front().key) / kInputsPerSecond > kGdxsvCameraTrailSecondsMax) trail.pop_front();
		// Nor a trail down from where a respawned MS drops in.
		if (!p.landed) {
			overlay.breaks[i] = true;
			continue;
		}
		const float step = trail.empty() ? 0.f : std::hypot(p.at.x - trail.back().at.x, p.at.z - trail.back().at.z);
		const bool starts = overlay.breaks[i] || trail.empty() || step > kTrailTeleport;
		trail.push_back({p.at, key, starts, trail.empty() ? 0.f : trail.back().along + step});
		overlay.breaks[i] = false;
	}
}

// Added to ImGui's atlas on first use, and again after the UI reloads its fonts, which clears the atlas.
ImFont *NameFont() {
	ImFontAtlas *atlas = ImGui::GetIO().Fonts;
	if (nameplates.font != nullptr && atlas->Fonts.contains(nameplates.font)) return nameplates.font;
	nameplates.font = nullptr;
	if (!nameplates.font_missing) {
		size_t size = 0;
		const std::unique_ptr<u8[]> data = resource::load(kNameFontFile, size);
		if (data != nullptr && size != 0) {
			void *owned = IM_ALLOC(size);
			std::memcpy(owned, data.get(), size);
			ImFontConfig config;
			std::snprintf(config.Name, sizeof(config.Name), "GDXGothic");
			nameplates.font = atlas->AddFontFromMemoryTTF(owned, static_cast<int>(size), 16.f, &config);
		}
		nameplates.font_missing = nameplates.font == nullptr;
		if (nameplates.font_missing) WARN_LOG(COMMON, "God's eye view: no %s, nameplates use the UI font", kNameFontFile);
	}
	return nameplates.font != nullptr ? nameplates.font : ImGui::GetFont();
}

ImU32 Faded(ImU32 color, float by) {
	const ImU32 alpha = static_cast<ImU32>(((color >> IM_COL32_A_SHIFT) & 0xff) * by);
	return (color & ~IM_COL32_A_MASK) | alpha << IM_COL32_A_SHIFT;
}

// The part of segment a-b inside a circle or a box, as fractions of it.
struct Span {
	float t0, t1;
};

bool CircleSpan(ImVec2 a, ImVec2 b, ImVec2 c, float r, Span &span) {
	const float dx = b.x - a.x, dy = b.y - a.y;
	const float fx = a.x - c.x, fy = a.y - c.y;
	const float qa = dx * dx + dy * dy;
	const float qb = fx * dx + fy * dy;
	const float qc = fx * fx + fy * fy - r * r;
	const float root = qb * qb - qa * qc;
	if (qa <= 0.f || root <= 0.f) return false;
	span.t0 = std::max((-qb - std::sqrt(root)) / qa, 0.f);
	span.t1 = std::min((-qb + std::sqrt(root)) / qa, 1.f);
	return span.t0 < span.t1;
}

bool BoxSpan(ImVec2 a, ImVec2 b, ImVec2 lo, ImVec2 hi, Span &span) {
	span = {0.f, 1.f};
	const float from[] = {a.x, a.y}, to[] = {b.x, b.y}, low[] = {lo.x, lo.y}, high[] = {hi.x, hi.y};
	for (int k = 0; k < 2; k++) {
		const float d = to[k] - from[k];
		if (d == 0.f) {
			if (from[k] < low[k] || from[k] > high[k]) return false;
			continue;
		}
		const float e = (low[k] - from[k]) / d, l = (high[k] - from[k]) / d;
		span.t0 = std::max(span.t0, std::min(e, l));
		span.t1 = std::min(span.t1, std::max(e, l));
	}
	return span.t0 < span.t1;
}

// The window's view of the game's picture, and what is drawn on it this frame.
struct Scene {
	ImDrawList *list = nullptr;
	ImVec2 display;
	ImVec2 picture_min;
	float picture_w = 0.f, picture_h = 0.f;
	float per_hud = 0.f, per_hud_x = 0.f;  // window pixels per HUD unit
	float pixel = 0.f;					   // window pixels per game pixel
	ImFont *font = nullptr;
	float name_size = 0.f;
	// Each MS from its feet to its head, which a tilted view spreads up the screen.
	ImVec2 body_at[4][kMsBodyCircles];
	float body_radius[4][kMsBodyCircles] = {};
	float name_w[4] = {};
	// What a nameplate shows, without its leader line: the force logo, the name and gauges, the MS image.
	ImVec2 part_min[4][3], part_max[4][3];
	int parts[4] = {};
	GdxsvCameraSprite images[4];
	ImVec2 image_min[4], image_max[4];
	float opacity[4] = {};

	ImVec2 Project(const GdxsvCameraVec3 &at) const {
		const GdxsvCameraVec3 d = Minus(at, overlay.eye);
		const float depth = std::max(gdxsv_camera_dot(d, overlay.forward), 1.f);
		return ImVec2(display.x * 0.5f + kHudDepth * gdxsv_camera_dot(d, overlay.right) / depth * per_hud_x,
					  display.y * 0.5f - kHudDepth * gdxsv_camera_dot(d, overlay.up) / depth * per_hud);
	}

	ImVec2 Anchor(int i) const {
		return ImVec2(display.x * 0.5f + overlay.anchor_x[i] * per_hud_x, display.y * 0.5f - overlay.anchor_y[i] * per_hud);
	}
};

// Where each MS and each nameplate is, and how faded each nameplate is.
void LayOut(Scene &s) {
	const GdxsvCameraSettings &settings = gdxsv_camera_settings;
	for (int i = 0; i < 4; i++) {
		const PlayerView &view = overlay.view[i];
		for (int k = 0; k < kMsBodyCircles && view.present; k++) {
			const GdxsvCameraVec3 at{view.at.x, view.at.y + kGdxsvCameraMsHeight * k / (kMsBodyCircles - 1), view.at.z};
			s.body_at[i][k] = s.Project(at);
			s.body_radius[i][k] =
				kHudDepth * kMsRadius / std::max(gdxsv_camera_dot(Minus(at, overlay.eye), overlay.forward), 1.f) * s.per_hud;
		}
		if (!overlay.anchor_on[i]) continue;
		const ImVec2 a = s.Anchor(i);
		const auto part = [&](ImVec2 min, ImVec2 max) {
			s.part_min[i][s.parts[i]] = min;
			s.part_max[i][s.parts[i]] = max;
			s.parts[i]++;
		};
		s.name_w[i] = s.font->CalcTextSizeA(s.name_size, FLT_MAX, 0.f, overlay.name[i].c_str()).x;
		const float name_top = kNameplateNameBaseline * s.pixel - s.font->GetFontBaked(s.name_size)->Ascent;
		const float right = std::max(kNameplateGaugeRight * s.pixel, kNameplateNameLeft * s.pixel + s.name_w[i]) + s.pixel;
		const bool boost = settings.boost && overlay.view[i].boost < 1.f;
		const float bottom = boost ? kNameplateBandBottom + kBoostGap + kBoostHeight : kNameplateGaugeBottom;
		part(ImVec2(a.x + kNameplateNameLeft * s.pixel, a.y + name_top), ImVec2(a.x + right, a.y + bottom * s.pixel));
		const ImVec2 logo_max(a.x + kNameplateLogoRight * s.pixel, a.y + kNameplateLogoBottom * s.pixel);
		const float logo = kNameplateLogoSize * s.pixel;
		if (gdxsv_camera_art_logo(overlay.team[i])) part(ImVec2(logo_max.x - logo, logo_max.y - logo), logo_max);
		if (!settings.ms_image) continue;
		s.images[i] = gdxsv_camera_art_ms(overlay.ms[i], overlay.team[i]);
		if (!s.images[i]) continue;
		const float h = s.images[i].h * kNameplateImageScale;
		const float image_bottom = std::min(kNameplateBandBottom, (kNameplateBandTop + kNameplateBandBottom + h) * 0.5f);
		s.image_max[i] = ImVec2(a.x + kNameplateImageRight * s.pixel, a.y + image_bottom * s.pixel);
		s.image_min[i] = ImVec2(s.image_max[i].x - s.images[i].w * kNameplateImageScale * s.pixel, s.image_max[i].y - h * s.pixel);
		part(s.image_min[i], s.image_max[i]);
	}
	for (int i = 0; i < 4; i++) {
		if (s.parts[i] == 0 || s.pixel <= 0.f) continue;
		const ImVec2 a = s.Anchor(i);
		ImVec2 min = s.part_min[i][0], max = s.part_max[i][0];
		for (int k = 1; k < s.parts[i]; k++) {
			min = ImVec2(std::min(min.x, s.part_min[i][k].x), std::min(min.y, s.part_min[i][k].y));
			max = ImVec2(std::max(max.x, s.part_max[i][k].x), std::max(max.y, s.part_max[i][k].y));
		}
		extents[i].left = (min.x - a.x) / s.pixel;
		extents[i].right = (max.x - a.x) / s.pixel;
		extents[i].top = (min.y - a.y) / s.pixel;
		extents[i].bottom = (max.y - a.y) / s.pixel;
	}
	const float clearance = kNameplateClearance * s.pixel;
	const auto circle_near = [&](ImVec2 c, float r, ImVec2 b0, ImVec2 b1) {
		const float dx = c.x - std::clamp(c.x, b0.x, b1.x);
		const float dy = c.y - std::clamp(c.y, b0.y, b1.y);
		return dx * dx + dy * dy < (r + clearance) * (r + clearance);
	};
	const auto plate_near_circle = [&](int i, ImVec2 c, float r) {
		for (int k = 0; k < s.parts[i]; k++)
			if (circle_near(c, r, s.part_min[i][k], s.part_max[i][k])) return true;
		return false;
	};
	const double now = ImGui::GetTime();
	const float step = ImGui::GetIO().DeltaTime;
	for (int i = 0; i < 4; i++) {
		float &opacity = nameplates.opacity[i];
		// A nameplate that comes back (a landed MS, nameplates turned on) fades in.
		if (!overlay.anchor_on[i]) {
			opacity = 0.f;
			continue;
		}
		// Its own MS is right under the nameplate, so close in it would always count.
		bool busy = false;
		for (int j = 0; j < 4; j++) {
			if (j == i) continue;
			for (int k = 0; k < kMsBodyCircles && overlay.view[j].present; k++)
				busy |= plate_near_circle(i, s.body_at[j][k], s.body_radius[j][k]);
		}
		if (busy) nameplates.busy_until[i] = now + kNameplateHoldSeconds;
		const float target = settings.fade && now < nameplates.busy_until[i] ? kNameplateFadeOpacity : 1.f;
		opacity += std::clamp(target - opacity, -step / kNameplateFadeOutSeconds, step / kNameplateFadeInSeconds);
		s.opacity[i] = opacity;
	}
}

// Fading behind each player, dotted where it crosses another MS or a nameplate.
void DrawTrails(const Scene &s) {
	const float seconds = gdxsv_camera_settings.trail_seconds;
	const float thickness = std::max(2.f, s.picture_h * 0.0045f);
	const float gap = kTrailGap * s.pixel;
	const float dot = thickness * kTrailDot;
	const float period = thickness * kTrailDotPeriod;
	const auto ahead = [&](const TrailPoint &point) {
		return gdxsv_camera_dot(Minus(point.at, overlay.eye), overlay.forward) >= kTrailNearest;
	};
	for (int i = 0; i < 4; i++) {
		const auto &trail = overlay.trail[i];
		for (size_t n = 1; n < trail.size(); n++) {
			if (trail[n].starts) continue;
			const float fade = 1.f - (overlay.key - trail[n].key) / kInputsPerSecond / seconds;
			if (fade <= 0.f) continue;
			// A point behind the eye, or right in front of it, would project far off, and so would a dotted stretch's dots.
			if (!ahead(trail[n - 1]) || !ahead(trail[n])) continue;
			const ImU32 rgb = kTeamColors[overlay.team[i]] & ~IM_COL32_A_MASK;
			const ImU32 color = rgb | (static_cast<ImU32>(fade * 230.f) << IM_COL32_A_SHIFT);
			const ImU32 faint = rgb | (static_cast<ImU32>(fade * 230.f * kTrailCrossOpacity) << IM_COL32_A_SHIFT);
			const ImVec2 a = s.Project(trail[n - 1].at);
			const ImVec2 b = s.Project(trail[n].at);
			const auto at = [&](float t) { return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t); };
			Span spans[4 * (kMsBodyCircles + 3)];
			int count = 0;
			for (int j = 0; j < 4; j++) {
				for (int k = 0; k < kMsBodyCircles && j != i && overlay.view[j].present; k++)
					if (CircleSpan(a, b, s.body_at[j][k], s.body_radius[j][k], spans[count])) count++;
				for (int k = 0; k < s.parts[j] && s.opacity[j] > 0.f; k++)
					if (BoxSpan(a, b, s.part_min[j][k], s.part_max[j][k], spans[count])) count++;
			}
			std::sort(spans, spans + count, [](const Span &x, const Span &y) { return x.t0 < y.t0; });
			const float length = std::hypot(b.x - a.x, b.y - a.y);
			const float world = trail[n].along - trail[n - 1].along;
			const float start = world > 0.f ? trail[n - 1].along * length / world : 0.f;
			const auto dots = [&](float t0, float t1) {
				if (length <= 0.f) return;
				const double first = std::ceil((static_cast<double>(start) + t0 * length) / period);
				const double last = (static_cast<double>(start) + t1 * length) / period;
				if (!(last - first < kTrailDotsMax)) return;
				for (double k = first; k < last; k += 1.0)
					s.list->AddCircleFilled(at(static_cast<float>((k * period - start) / length)), dot, faint, 8);
			};
			const auto draw = [&](float t0, float t1) {
				float t = t0;
				for (int k = 0; k < count && t < t1; k++) {
					const float s0 = std::max(spans[k].t0, t), s1 = std::min(spans[k].t1, t1);
					if (s1 <= s0) continue;
					if (s0 > t) s.list->AddLine(at(t), at(s0), color, thickness);
					dots(s0, s1);
					t = s1;
				}
				if (t < t1) s.list->AddLine(at(t), at(t1), color, thickness);
			};
			// Close in, an MS is larger than the fixed gap: in a crowd its own trail keeps off all of it.
			Span own{1.f, 0.f};
			for (int k = 0; k < kMsBodyCircles && overlay.view[i].present; k++) {
				Span part;
				if (!CircleSpan(a, b, s.body_at[i][k], std::max(gap, s.body_radius[i][k]), part)) continue;
				own.t0 = std::min(own.t0, part.t0);
				own.t1 = std::max(own.t1, part.t1);
			}
			if (own.t1 <= own.t0) {
				draw(0.f, 1.f);
				continue;
			}
			if (own.t0 > 0.f) draw(0.f, own.t0);
			if (own.t1 < 1.f) draw(own.t1, 1.f);
		}
	}
}

// Beside each player in control: a pointer toward what it locks, or an outline the way it looks.
void DrawPointers(const Scene &s) {
	const float rim = std::max(1.f, s.pixel * 0.4f);
	const float outline = std::max(1.25f, s.pixel * 0.55f);
	for (int i = 0; i < 4; i++) {
		const PlayerView &view = overlay.view[i];
		if (!view.on) continue;
		const ImVec2 from = s.Project(view.at);
		const ImVec2 to = view.locking ? s.Project(view.target)
									   : s.Project({view.at.x + view.look_x * 100.f, view.at.y, view.at.z + view.look_z * 100.f});
		const float dx = to.x - from.x;
		const float dy = to.y - from.y;
		const float length = std::sqrt(dx * dx + dy * dy);
		// A view straight up or down has no direction to show.
		if (length < (view.locking ? 1.f : s.pixel * 2.f)) continue;
		const ImVec2 along(dx / length, dy / length);
		float tip_at = kPointerTip;
		float base_at = kPointerBase;
		float size = 1.f;
		if (view.locking) {
			tip_at = std::min(tip_at, (length / s.pixel - kPointerTipGap) * 0.5f);
			base_at = tip_at - (kPointerTip - kPointerBase);
			if (base_at < kPointerNearest) {
				size = (tip_at - kPointerNearest) / (kPointerTip - kPointerBase);
				base_at = kPointerNearest;
			}
			if (size < kPointerSmallest) continue;
		}
		const ImVec2 base(from.x + along.x * base_at * s.pixel, from.y + along.y * base_at * s.pixel);
		const ImVec2 side(-along.y * kPointerHalfWidth * size * s.pixel, along.x * kPointerHalfWidth * size * s.pixel);
		const ImVec2 tip(from.x + along.x * tip_at * s.pixel, from.y + along.y * tip_at * s.pixel);
		const ImVec2 left(base.x + side.x, base.y + side.y);
		const ImVec2 right(base.x - side.x, base.y - side.y);
		if (!view.locking) {
			s.list->AddTriangle(tip, left, right, IM_COL32(0, 0, 0, static_cast<int>(kPointerLookRimOpacity * 255.f)), outline + rim);
			s.list->AddTriangle(tip, left, right, IM_COL32(255, 255, 255, static_cast<int>(kPointerLookOpacity * 255.f)), outline);
			continue;
		}
		const float blink = 1.f - std::clamp((overlay.key - overlay.pointer_key[i]) / kPointerBlinkInputs, 0.f, 1.f);
		const float opacity = kPointerRestOpacity + (kPointerBlinkOpacity - kPointerRestOpacity) * blink;
		s.list->AddTriangleFilled(tip, left, right, IM_COL32(255, 255, 255, static_cast<int>(opacity * 255.f)));
		s.list->AddTriangle(tip, left, right, IM_COL32(0, 0, 0, static_cast<int>(opacity * kPointerRimOpacity * 255.f)), rim);
	}
}

// The game's mark on every MS nothing can damage now, whoever locks it, at its middle.
void DrawUnhittableMarks(const Scene &s) {
	const float radius = s.pixel * kMarkRadius;
	const float width = std::max(1.5f, s.pixel * 0.9f);
	const float arrow = s.pixel * 4.f;
	const ImU32 dark = IM_COL32(0, 0, 0, 150);
	for (const PlayerView &view : overlay.view) {
		if (!view.unhittable) continue;
		const ImVec2 c = s.Project({view.at.x, view.at.y + kGdxsvCameraMsHeight * 0.5f, view.at.z});
		s.list->AddCircle(c, radius, dark, 32, width + std::max(1.f, s.pixel * 0.5f));
		s.list->AddCircle(c, radius, kUnhittableColor, 32, width);
		for (const float side : {-1.f, 1.f}) {
			const float tip = c.y + side * (radius + s.pixel * 1.5f);
			const float base = tip + side * arrow * 1.2f;
			const float edge = s.pixel * 0.8f;
			s.list->AddTriangleFilled(ImVec2(c.x, tip - side * edge), ImVec2(c.x + arrow + edge, base + side * s.pixel * 0.5f),
									  ImVec2(c.x - arrow - edge, base + side * s.pixel * 0.5f), dark);
			s.list->AddTriangleFilled(ImVec2(c.x, tip), ImVec2(c.x + arrow, base), ImVec2(c.x - arrow, base), kUnhittableColor);
		}
	}
}

void DrawNameplates(const Scene &s) {
	const GdxsvCameraSettings &settings = gdxsv_camera_settings;
	const float line = std::max(1.f, kNameplateLine * s.pixel);
	const float edge = std::max(1.f, kNameplateLineEdge * s.pixel);
	const float name_top = kNameplateNameBaseline * s.pixel - s.font->GetFontBaked(s.name_size)->Ascent;
	const bool team_colors = settings.team_colors;
	for (int i = 0; i < 4; i++) {
		const float opacity = s.opacity[i];
		if (!overlay.anchor_on[i] || opacity <= 0.f) continue;
		const ImVec2 a = s.Anchor(i);
		const ImVec2 rule_left(a.x + kNameplateRuleLeft * s.pixel, a.y + kNameplateRule * s.pixel);
		const float rule_right =
			a.x + kNameplateNameLeft * s.pixel + std::max(s.name_w[i], (kNameplateGaugeRight - kNameplateNameLeft) * s.pixel);
		const GdxsvCameraVec3 &feet = overlay.view[i].at;
		const ImVec2 pin = overlay.view[i].present ? s.Project({feet.x, feet.y + kNameplateAbove, feet.z}) : a;
		const ImVec2 lines[] = {pin, rule_left, ImVec2(rule_right, rule_left.y)};
		s.list->AddPolyline(lines, 3, Faded(kEdgeColor, opacity), ImDrawFlags_None, line + 2.f * edge);
		s.list->AddPolyline(lines, 3, Faded(team_colors ? kTeamColors[overlay.team[i]] : IM_COL32_WHITE, opacity), ImDrawFlags_None, line);

		const ImU32 *colors = team_colors ? kHpColors[overlay.team[i]] : kHpGameColors;
		const float left = a.x + kNameplateGaugeLeft * s.pixel;
		const float right = a.x + kNameplateGaugeRight * s.pixel;
		const float split = left + (right - left) * overlay.hp[i];
		const float top = a.y + kNameplateGaugeTop * s.pixel;
		const float bottom = a.y + kNameplateGaugeBottom * s.pixel;
		if (split > left) s.list->AddRectFilled(ImVec2(left, top), ImVec2(split, bottom), Faded(colors[0], opacity));
		if (split < right) s.list->AddRectFilled(ImVec2(split, top), ImVec2(right, bottom), Faded(colors[1], opacity));

		const char *name = overlay.name[i].c_str();
		const ImVec2 at(a.x + kNameplateNameLeft * s.pixel, a.y + name_top);
		for (int k = 0; k < 16; k++) {
			const float r = (k < 8 ? 0.45f : 1.f) * kNameOutline * s.pixel;
			const float angle = k * 0.25f * kPi;
			s.list->AddText(s.font, s.name_size, ImVec2(at.x + r * std::cos(angle), at.y + r * std::sin(angle)),
							Faded(kNameOutlineColor, opacity), name);
		}
		s.list->AddText(s.font, s.name_size, at, Faded(IM_COL32_WHITE, opacity), name);
	}
	// The MS images, then every nameplate's force logo, so that no image covers another player's logo.
	for (int i = 0; i < 4; i++) {
		if (!s.images[i] || s.opacity[i] <= 0.f) continue;
		s.list->AddImage(s.images[i].texture, s.image_min[i], s.image_max[i], s.images[i].uv0, s.images[i].uv1,
						 Faded(IM_COL32_WHITE, s.opacity[i]));
	}
	for (int i = 0; i < 4; i++) {
		if (!overlay.anchor_on[i] || s.opacity[i] <= 0.f) continue;
		const GdxsvCameraSprite logo = gdxsv_camera_art_logo(overlay.team[i]);
		if (!logo) continue;
		const ImVec2 a = s.Anchor(i);
		const ImVec2 max(a.x + kNameplateLogoRight * s.pixel, a.y + kNameplateLogoBottom * s.pixel);
		const float size = kNameplateLogoSize * s.pixel;
		s.list->AddImage(logo.texture, ImVec2(max.x - size, max.y - size), max, logo.uv0, logo.uv1, Faded(IM_COL32_WHITE, s.opacity[i]));
	}
	if (!settings.boost) return;
	const float margin = std::max(0.5f, s.pixel * 0.3f);
	for (int i = 0; i < 4; i++) {
		if (!overlay.anchor_on[i] || overlay.view[i].boost >= 1.f || s.opacity[i] <= 0.f) continue;
		const ImVec2 a = s.Anchor(i);
		const float left = a.x + kNameplateGaugeLeft * s.pixel;
		const float right = a.x + kNameplateGaugeRight * s.pixel;
		const float top = a.y + (kNameplateBandBottom + kBoostGap) * s.pixel;
		const float bottom = top + kBoostHeight * s.pixel;
		const float split = right - (right - left) * overlay.view[i].boost;
		s.list->AddRectFilled(ImVec2(left - margin, top - margin), ImVec2(right + margin, bottom + margin),
							  Faded(kBoostBackColor, s.opacity[i]));
		if (split > left) s.list->AddRectFilled(ImVec2(left, top), ImVec2(split, bottom), Faded(kBoostUsedColor, s.opacity[i]));
		if (split < right) s.list->AddRectFilled(ImVec2(split, top), ImVec2(right, bottom), Faded(kBoostLeftColor, s.opacity[i]));
	}
}
}  // namespace

void gdxsv_camera_overlay_update(const GdxsvCameraFrame &frame) {
	// A seek's frames are not shown. The overlay is updated again once the replay plays, its trails anew.
	if (gdxsv.IsReplaySeeking()) return;
	const GdxsvCameraSettings &settings = gdxsv_camera_settings;
	const bool new_replay = facts.replay != settings.replay;
	if (new_replay) {
		facts.replay = settings.replay;
		facts.hud_left = gdxsv.SymbolAddress("gdx_widescreen_hud_left_offset");
		facts.hud_right = gdxsv.SymbolAddress("gdx_widescreen_hud_right_offset");
		for (int i = 0; i < 4; i++) facts.name[i] = gdxsv.ReplayPilotName(i);
	}
	// A nameplate waits for its MS to land as the framing does: up there it would be held at the screen's edge.
	float label_x[4] = {};
	float label_y[4] = {};
	for (int i = 0; i < 4; i++) {
		if (!frame.players[i].landed) continue;
		const GdxsvCameraVec3 &at = frame.players[i].at;
		const GdxsvCameraVec3 d = Minus(GdxsvCameraVec3{at.x, at.y + kNameplateAbove, at.z}, frame.eye);
		const float depth = std::max(gdxsv_camera_dot(d, frame.forward), 1.f);
		label_x[i] = kHudDepth * gdxsv_camera_dot(d, frame.right) / depth;
		label_y[i] = kHudDepth * gdxsv_camera_dot(d, frame.up) / depth;
	}
	// Nameplates that would overlap move apart vertically, by their extents in HUD units (up positive).
	const float hud_half_w = kHudHalfHeight * frame.aspect;
	const float unit = kHudHalfHeight / 240.f;	// a game pixel
	const float gap = kNameplateGap * unit;
	float plate_x[4] = {}, left[4] = {}, right[4] = {}, low[4] = {}, high[4] = {};
	for (int i = 0; i < 4; i++) {
		left[i] = extents[i].left * unit;
		right[i] = extents[i].right * unit;
		low[i] = -extents[i].bottom * unit;
		high[i] = -extents[i].top * unit;
		plate_x[i] = std::clamp(label_x[i], -hud_half_w + 0.002f, hud_half_w - 0.002f - right[i]);
	}
	float push[4] = {};
	for (int pass = 0; pass < 3; pass++) {
		for (int i = 0; i < 4; i++) {
			for (int j = i + 1; j < 4; j++) {
				if (!frame.players[i].landed || !frame.players[j].landed) continue;
				if (plate_x[i] + left[i] >= plate_x[j] + right[j] + gap || plate_x[j] + left[j] >= plate_x[i] + right[i] + gap) continue;
				const float yi = label_y[i] + push[i], yj = label_y[j] + push[j];
				const float overlap = std::min(yi + high[i], yj + high[j]) - std::max(yi + low[i], yj + low[j]) + gap;
				if (overlap <= 0.f) continue;
				const float move = overlap * 0.5f * (yi >= yj ? 1.f : -1.f);
				push[i] += move;
				push[j] -= move;
			}
		}
	}
	const Cockpit cockpit = ReadCockpit();
	PlayerView views[4];
	for (int i = 0; i < 4; i++) views[i] = ReadPlayerView(i, frame.players[i], cockpit.lock_mark[i]);
	const int key = gdxsv.ReplayKeyMsgCount();

	std::lock_guard<std::mutex> lock(overlay.mutex);
	if (key < overlay.key || key > overlay.key + kTrailJump) {
		for (int i = 0; i < 4; i++) {
			overlay.trail[i].clear();
			overlay.breaks[i] = true;
			overlay.pointer_key[i] = key - static_cast<int>(kPointerBlinkInputs);
		}
		overlay.last_sample = -1;
	}
	overlay.key = key;
	SampleTrails(frame, key);
	overlay.eye = frame.eye;
	overlay.right = frame.right;
	overlay.up = frame.up;
	overlay.forward = frame.forward;
	const GdxsvCameraGame &game = gdxsv_camera_game();
	for (int i = 0; i < 4; i++) {
		const u32 actor = game.actor(i);
		overlay.push[i] += (push[i] - overlay.push[i]) * kNameplateSettle;
		overlay.anchor_on[i] = settings.nameplates && frame.players[i].landed;
		overlay.anchor_x[i] = plate_x[i];
		overlay.anchor_y[i] = std::clamp(label_y[i] + overlay.push[i], -0.073f, 0.073f - 2.f * kNameplateH);
		overlay.ms[i] = gdxsv_ReadMem8(actor + kActorMs);
		overlay.team[i] = gdxsv_ReadMem8(actor + kActorTeam) == 0 ? 0 : 1;
		const int hp_max = static_cast<s16>(gdxsv_ReadMem16(actor + kActorHpMax));
		const int hp = static_cast<s16>(gdxsv_ReadMem16(actor + kActorHp));
		overlay.hp[i] = hp_max > 0 ? std::clamp(static_cast<float>(hp) / hp_max, 0.f, 1.f) : 0.f;
		if (new_replay) overlay.name[i] = facts.name[i];
		overlay.view[i] = views[i];
		const u32 pointer_target = views[i].locking ? gdxsv_ReadMem32(actor + game.actor_target) : 0;
		if (pointer_target != overlay.pointer_target[i]) overlay.pointer_key[i] = key;
		overlay.pointer_target[i] = pointer_target;
	}
	overlay.valid = cockpit.alive;
	overlay.gauge_slide = cockpit.gauge_slide + (facts.hud_left != 0 ? gdxsv_camera_read_float(facts.hud_left) : 0.f);
	overlay.timer_slide = cockpit.timer_slide + (facts.hud_right != 0 ? gdxsv_camera_read_float(facts.hud_right) : 0.f);
}

void gdxsv_camera_overlay_clear() {
	std::lock_guard<std::mutex> lock(overlay.mutex);
	overlay.valid = false;
	overlay.key = -1;
	overlay.last_sample = -1;
	for (int i = 0; i < 4; i++) {
		overlay.trail[i].clear();
		overlay.breaks[i] = true;
		overlay.push[i] = 0.f;
	}
}

bool gdxsv_camera_overlay_battle(GdxsvCameraBattle &battle) {
	std::lock_guard<std::mutex> lock(overlay.mutex);
	if (!overlay.valid) return false;
	battle.timer_slide = overlay.timer_slide * 240.f / kHudHalfHeight;
	for (int i = 0; i < 4; i++) {
		battle.team[i] = overlay.team[i];
		battle.name[i] = overlay.name[i];
	}
	return true;
}

u32 gdxsv_camera_team_color(int team) { return kTeamColors[team != 0 ? 1 : 0]; }

void gdxsv_camera_render_overlay() {
	if (!gdxsv_camera_gods_eye()) return;
	std::lock_guard<std::mutex> lock(overlay.mutex);
	if (!overlay.valid) return;

	// The game's picture keeps its aspect, centered in the window.
	Scene s;
	s.display = ImGui::GetIO().DisplaySize;
	const float aspect = gdxsv_camera_picture_aspect(s.display.x / s.display.y);
	s.picture_h = s.display.x / s.display.y >= aspect ? s.display.y : s.display.x / aspect;
	s.picture_w = s.picture_h * aspect;
	s.picture_min = ImVec2((s.display.x - s.picture_w) * 0.5f, (s.display.y - s.picture_h) * 0.5f);
	s.per_hud = s.picture_h / (2.f * kHudHalfHeight);
	s.per_hud_x = config::Widescreen ? s.per_hud : s.per_hud * config::ScreenStretching / 100.f;
	s.pixel = s.picture_h / 480.f;
	s.font = NameFont();
	s.name_size = kNameplateNameSize * s.pixel;
	s.list = ImGui::GetBackgroundDrawList();
	LayOut(s);

	s.list->PushClipRect(s.picture_min, ImVec2(s.picture_min.x + s.picture_w, s.picture_min.y + s.picture_h), false);
	if (gdxsv_camera_settings.trail) DrawTrails(s);
	DrawPointers(s);
	DrawUnhittableMarks(s);
	DrawNameplates(s);
	const float cockpit_w = s.picture_h * 4.f / 3.f;
	const float cockpit_left = s.display.x * 0.5f - cockpit_w * 0.5f;
	const ImVec2 name(cockpit_left + cockpit_w * kPilotNameX + overlay.gauge_slide * s.per_hud_x,
					  s.picture_min.y + s.picture_h * kPilotNameY);
	s.list->AddText(s.font, s.picture_h * kPilotNameSize, name, IM_COL32_WHITE, GdxsvLanguage::gdxT("God"));
	s.list->PopClipRect();
}
