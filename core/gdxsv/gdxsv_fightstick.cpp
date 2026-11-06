// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_fightstick.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

extern ImFont *boldFont;

namespace {
constexpr float kPi = 3.14159265f;
// The eye: this far from the stick and tilted this far from looking straight down, in millimetres.
constexpr float kDistance = 700.f;
constexpr float kTilt = 42.f * kPi / 180.f;
// The stick in millimetres: x right, y up, z away from the player, from the X button's center.
constexpr float kLeverX = -62.f;
constexpr float kLeverZ = -15.f;
constexpr float kShaft = 40.f;
constexpr float kShaftWidth = 5.4f;
constexpr float kBall = 16.f;
constexpr float kLean = 20.f * kPi / 180.f;	 // as far as the lever leans
constexpr float kHole = 23.f;
constexpr float kWasher = 20.f;
constexpr float kWasherSlide = 6.f;	 // how far the washer slides as the lever leans all the way
constexpr float kBezel = 16.f;
constexpr float kGlow = 17.5f;
constexpr float kPlunger = 12.5f;
constexpr float kPlungerUp = 5.f;
constexpr float kPlungerDown = 1.2f;
// The name on a plunger, as ImGui sizes a font: ascent to descent. START's five letters are smaller.
constexpr float kLetter = 13.f;
constexpr float kStartLetter = 6.5f;
constexpr float kHairline = 0.45f;
constexpr float kGreyShade = 0.5f;
// The ball's light red, for words about the lever.
constexpr ImU32 kLeverColor = IM_COL32(0xff, 0x8f, 0x80, 0xff);

struct Spot {
	float x, z;
};
// By part: the lever, X, Y, A, B, START. Three buttons in an arc, the first lowest, the fourth under it; START past
// them, further than they are apart.
constexpr Spot kSpots[kGdxsvFightstickParts] = {{kLeverX, kLeverZ}, {0.f, 0.f}, {34.f, 9.f}, {68.f, 9.f}, {-2.f, -34.f}, {108.f, 4.f}};
constexpr const char *kNames[kGdxsvFightstickParts] = {"", "X", "Y", "A", "B", "START"};
// X, Y, A, B, START: a button's light, base and dark colors.
constexpr ImU32 kButtonColors[5][3] = {
	{IM_COL32(0xff, 0xe5, 0x8a, 0xff), IM_COL32(0xf2, 0xb8, 0x21, 0xff), IM_COL32(0x7d, 0x5c, 0x08, 0xff)},
	{IM_COL32(0x9d, 0xeb, 0xb4, 0xff), IM_COL32(0x2f, 0xb4, 0x5f, 0xff), IM_COL32(0x15, 0x5a, 0x2b, 0xff)},
	{IM_COL32(0xff, 0xa6, 0x99, 0xff), IM_COL32(0xe5, 0x39, 0x2f, 0xff), IM_COL32(0x7a, 0x14, 0x0d, 0xff)},
	{IM_COL32(0xa6, 0xc8, 0xff, 0xff), IM_COL32(0x3a, 0x78, 0xe6, 0xff), IM_COL32(0x18, 0x39, 0x7c, 0xff)},
	{IM_COL32(0xf4, 0xf5, 0xf7, 0xff), IM_COL32(0xb9, 0xbe, 0xc8, 0xff), IM_COL32(0x4a, 0x4e, 0x57, 0xff)},
};

struct Stop {
	float at;
	ImU32 color;
};
constexpr Stop kBallStops[] = {{0.f, IM_COL32(0xff, 0xe0, 0xd8, 0xff)},
							   {0.22f, IM_COL32(0xff, 0x55, 0x45, 0xff)},
							   {0.64f, IM_COL32(0xc3, 0x16, 0x1b, 0xff)},
							   {1.f, IM_COL32(0x5e, 0x06, 0x07, 0xff)}};
constexpr Stop kWasherStops[] = {
	{0.f, IM_COL32(0xb9, 0xbe, 0xc8, 0xff)}, {0.7f, IM_COL32(0x6b, 0x70, 0x7b, 0xff)}, {1.f, IM_COL32(0x3a, 0x3d, 0x45, 0xff)}};

ImU32 Mix(ImU32 a, ImU32 b, float t) {
	ImU32 out = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		const float from = static_cast<float>(a >> shift & 0xff), to = static_cast<float>(b >> shift & 0xff);
		out |= static_cast<ImU32>(from + (to - from) * t + 0.5f) << shift;
	}
	return out;
}

// A part's colors as the stick fades; a grey part's are their brightness, dimmed.
struct Paint {
	float alpha;
	bool grey;
	ImU32 operator()(ImU32 color, float opacity = 1.f) const {
		const float a = static_cast<float>(color >> IM_COL32_A_SHIFT & 0xff) * opacity * alpha;
		ImU32 rgb = color & ~IM_COL32_A_MASK;
		if (grey) {
			const float r = static_cast<float>(color >> IM_COL32_R_SHIFT & 0xff), g = static_cast<float>(color >> IM_COL32_G_SHIFT & 0xff),
						b = static_cast<float>(color >> IM_COL32_B_SHIFT & 0xff);
			const ImU32 v = static_cast<ImU32>((0.3f * r + 0.59f * g + 0.11f * b) * kGreyShade);
			rgb = v << IM_COL32_R_SHIFT | v << IM_COL32_G_SHIFT | v << IM_COL32_B_SHIFT;
		}
		return rgb | static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT;
	}
};

struct Point {
	ImVec2 at;
	float k;  // pixels per millimetre there
};

struct View {
	ImVec2 origin;
	float scale;
	float sin_tilt = std::sin(kTilt);
	float cos_tilt = std::cos(kTilt);

	Point Project(float x, float y, float z) const {
		const float k = kDistance / (kDistance + z * sin_tilt - y * cos_tilt) * scale;
		return {ImVec2(origin.x + x * k, origin.y - (y * sin_tilt + z * cos_tilt) * k), k};
	}
	// A circle lying on the stick's top.
	ImVec2 Flat(const Point &p, float r) const { return ImVec2(r * p.k, r * p.k * cos_tilt); }
};

ImU32 Along(const Stop *stops, int count, float t) {
	for (int i = 1; i < count; i++) {
		if (t <= stops[i].at) return Mix(stops[i - 1].color, stops[i].color, std::max(0.f, (t - stops[i - 1].at) / (stops[i].at - stops[i - 1].at)));
	}
	return stops[count - 1].color;
}

// An ellipse filled as SVG fills one with a radial gradient: the stops run from focus over reach, both in units of the
// ellipse's bounding box. Rays from the focus to the rim carry the colors, and a clear pixel beyond the rim smooths it.
void Gradient(ImDrawList *list, ImVec2 center, ImVec2 radius, ImVec2 focus, float reach, const Stop *stops, int count, const Paint &paint) {
	constexpr int kSegments = 48;
	constexpr int kRings = 10;
	const ImVec2 corner(center.x - radius.x, center.y - radius.y);
	const ImVec2 from(corner.x + focus.x * 2.f * radius.x, corner.y + focus.y * 2.f * radius.y);
	const auto color = [&](ImVec2 p) {
		const float u = (p.x - corner.x) / (2.f * radius.x) - focus.x, v = (p.y - corner.y) / (2.f * radius.y) - focus.y;
		return paint(Along(stops, count, std::sqrt(u * u + v * v) / reach));
	};
	const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
	list->PrimReserve(kSegments * (3 + kRings * 6), 1 + kSegments * (kRings + 1));
	const unsigned int base = list->_VtxCurrentIdx;
	list->PrimWriteVtx(from, uv, color(from));
	for (int s = 0; s < kSegments; s++) {
		const float a = 2.f * kPi * s / kSegments;
		const ImVec2 rim(center.x + std::cos(a) * radius.x, center.y + std::sin(a) * radius.y);
		for (int r = 1; r <= kRings; r++) {
			const float t = static_cast<float>(r) / kRings;
			const ImVec2 p(from.x + (rim.x - from.x) * t, from.y + (rim.y - from.y) * t);
			list->PrimWriteVtx(p, uv, color(p));
		}
		const float nx = std::cos(a) / radius.x, ny = std::sin(a) / radius.y, n = std::max(std::sqrt(nx * nx + ny * ny), FLT_MIN);
		list->PrimWriteVtx(ImVec2(rim.x + nx / n, rim.y + ny / n), uv, color(rim) & ~IM_COL32_A_MASK);
	}
	const auto vertex = [&](int s, int r) { return static_cast<ImDrawIdx>(base + 1 + (s % kSegments) * (kRings + 1) + r - 1); };
	for (int s = 0; s < kSegments; s++) {
		list->PrimWriteIdx(static_cast<ImDrawIdx>(base));
		list->PrimWriteIdx(vertex(s, 1));
		list->PrimWriteIdx(vertex(s + 1, 1));
		for (int r = 1; r <= kRings; r++) {
			list->PrimWriteIdx(vertex(s, r));
			list->PrimWriteIdx(vertex(s + 1, r));
			list->PrimWriteIdx(vertex(s + 1, r + 1));
			list->PrimWriteIdx(vertex(s, r));
			list->PrimWriteIdx(vertex(s + 1, r + 1));
			list->PrimWriteIdx(vertex(s, r + 1));
		}
	}
}

// A line with round ends, as one shape, so that a see-through color does not darken where they meet.
void Rod(ImDrawList *list, ImVec2 a, ImVec2 b, float width, ImU32 color) {
	const float r = width * 0.5f;
	const float dir = std::atan2(b.y - a.y, b.x - a.x);
	list->PathArcTo(b, r, dir - kPi * 0.5f, dir + kPi * 0.5f, 10);
	list->PathArcTo(a, r, dir + kPi * 0.5f, dir + kPi * 1.5f, 10);
	list->PathFillConvex(color);
}

void Lever(ImDrawList *list, const View &view, const GdxsvFightstickLook &look, const Paint &paint) {
	const float len = std::hypot(look.lever_x, look.lever_y);
	const float lean = std::min(1.f, len) * kLean;
	const float sx = len > 0.f ? std::sin(lean) * look.lever_x / len : 0.f;
	const float sz = len > 0.f ? std::sin(lean) * look.lever_y / len : 0.f;
	const Point hole = view.Project(kLeverX, 0.f, kLeverZ);
	const Point washer = view.Project(kLeverX + sx * kWasherSlide, 0.6f, kLeverZ + sz * kWasherSlide);
	const Point head = view.Project(kLeverX + sx * kShaft, std::cos(lean) * kShaft, kLeverZ + sz * kShaft);
	list->AddEllipseFilled(hole.at, view.Flat(hole, kHole), paint(IM_COL32(7, 8, 11, 255)));
	list->AddEllipse(hole.at, view.Flat(hole, kHole), paint(IM_COL32(255, 255, 255, 23)), 0.f, 0, kHairline * hole.k);
	Gradient(list, washer.at, view.Flat(washer, kWasher), ImVec2(0.45f, 0.35f), 0.8f, kWasherStops, 3, paint);
	const float w = kShaftWidth * head.k;
	Rod(list, washer.at, head.at, w, paint(IM_COL32(0x7b, 0x81, 0x8c, 0xff)));
	Rod(list, ImVec2(washer.at.x - w * 0.2f, washer.at.y), ImVec2(head.at.x - w * 0.2f, head.at.y), w * 0.3f,
		paint(IM_COL32(0xe6, 0xe9, 0xed, 0xff), 0.75f));
	Gradient(list, head.at, ImVec2(kBall * head.k, kBall * head.k), ImVec2(0.38f, 0.3f), 0.75f, kBallStops, 4, paint);
}

void Button(ImDrawList *list, const View &view, int part, bool held, const Paint &paint) {
	const Spot spot = kSpots[part];
	const ImU32 *colors = kButtonColors[part - 1];
	const Point bezel = view.Project(spot.x, 0.f, spot.z);
	const Point top = view.Project(spot.x, held ? kPlungerDown : kPlungerUp, spot.z);
	const Point foot = view.Project(spot.x, 0.3f, spot.z);
	if (held) {
		for (int i = 0; i < 3; i++)
			list->AddEllipse(bezel.at, view.Flat(bezel, kGlow), paint(colors[1], 0.35f - i * 0.1f), 0.f, 0, (3.f + i * 2.f) * bezel.k);
	}
	list->AddEllipseFilled(bezel.at, view.Flat(bezel, kBezel), paint(IM_COL32(0x11, 0x12, 0x16, 0xff)));
	list->AddEllipse(bezel.at, view.Flat(bezel, kBezel), paint(IM_COL32(255, 255, 255, 41)), 0.f, 0, kHairline * bezel.k);
	// The plunger's side: down from its top's widest points and around the near half of its foot.
	const float top_r = kPlunger * top.k, foot_r = kPlunger * foot.k;
	list->PathLineTo(ImVec2(top.at.x + top_r, top.at.y));
	list->PathEllipticalArcTo(foot.at, ImVec2(foot_r, foot_r * view.cos_tilt), 0.f, 0.f, kPi, 16);
	list->PathLineTo(ImVec2(top.at.x - top_r, top.at.y));
	list->PathFillConvex(paint(colors[2]));
	const Stop stops[] = {{0.f, colors[0]}, {0.55f, colors[1]}, {1.f, colors[2]}};
	Gradient(list, top.at, view.Flat(top, kPlunger), ImVec2(0.4f, 0.3f), 0.8f, stops, 3, paint);
	if (held) list->AddEllipse(top.at, view.Flat(top, kPlunger), paint(colors[0]), 0.f, 0, kHairline * top.k);

	// The name lies on the plunger: squashed as its top is, with a faint dark edge.
	const char *name = kNames[part];
	const float size = (part == static_cast<int>(GdxsvFightstickPart::Start) ? kStartLetter : kLetter) * top.k;
	const ImVec2 extent = boldFont->CalcTextSizeA(size, FLT_MAX, 0.f, name);
	const ImVec2 at(top.at.x - extent.x * 0.5f, top.at.y - extent.y * 0.5f);
	const int first = list->VtxBuffer.Size;
	const float edge = 0.4f * top.k;
	for (const ImVec2 &d : {ImVec2(-edge, -edge), ImVec2(edge, -edge), ImVec2(-edge, edge), ImVec2(edge, edge)})
		list->AddText(boldFont, size, ImVec2(at.x + d.x, at.y + d.y), paint(IM_COL32(0, 0, 0, 64)), name);
	list->AddText(boldFont, size, at, paint(IM_COL32_WHITE), name);
	for (int i = first; i < list->VtxBuffer.Size; i++) {
		ImDrawVert &v = list->VtxBuffer[i];
		v.pos.y = top.at.y + (v.pos.y - top.at.y) * view.cos_tilt;
	}
}
}  // namespace

void gdxsv_fightstick_draw(ImDrawList *list, ImVec2 origin, float scale, const GdxsvFightstickLook &look) {
	const View view{origin, scale};
	// Far to near: Y and A, START, X, the lever, B.
	for (const GdxsvFightstickPart part : {GdxsvFightstickPart::Y, GdxsvFightstickPart::A, GdxsvFightstickPart::Start, GdxsvFightstickPart::X,
										   GdxsvFightstickPart::Lever, GdxsvFightstickPart::B}) {
		if (part == GdxsvFightstickPart::Start && !look.with_start) continue;
		const int i = static_cast<int>(part);
		const Paint paint{look.alpha, look.grey[i]};
		if (part == GdxsvFightstickPart::Lever)
			Lever(list, view, look, paint);
		else
			Button(list, view, i, look.held[i], paint);
	}
}

ImU32 gdxsv_fightstick_color(GdxsvFightstickPart part) {
	return part == GdxsvFightstickPart::Lever ? kLeverColor : kButtonColors[static_cast<int>(part) - 1][0];
}

ImVec2 gdxsv_fightstick_point(ImVec2 origin, float scale, GdxsvFightstickPart part, float away) {
	const Spot spot = kSpots[static_cast<int>(part)];
	return View{origin, scale}.Project(spot.x, 0.f, spot.z + away).at;
}
