// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// The god's eye view's controls guide, shown while the CAMERA panel's ? is hovered: a fightstick with what its lever and
// buttons do in the mode in use, following the first player's own lever and buttons, and the keyboard and mouse controls
// beside it.
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <string>

#include "gdxsv_camera.h"
#include "gdxsv_camera_internal.h"
#include "gdxsv_fightstick.h"
#include "imgui.h"
#include "input/gamepad.h"
#include "oslib/i18n.h"
#include "ui/IconsFontAwesome6.h"

extern ImFont *boldFont;

namespace {
constexpr float kFadeSeconds = 0.2f;
constexpr float kLeverSeconds = 0.05f;	// the drawn lever follows the real one this closely

// The layout, in units of a picture 540 high. Text sizes are ImGui's, which span ascent to descent: 1.17 times a CSS
// font size.
constexpr float kBoxW = 810.f;
constexpr float kBoxH = 289.f;
constexpr float kBoxTop = 108.f;
constexpr float kRounding = 14.f;
constexpr float kPadTop = 12.f;
constexpr float kPadSide = 18.f;
constexpr float kEdge = 16.f;  // from the picture's edge
constexpr float kDrop = 14.f;  // it rises this far as it fades in
constexpr float kHeaderH = 17.f;
constexpr float kTitleText = 20.f;
constexpr float kGap = 9.f;	 // between an icon or key and its words
constexpr float kBodyTop = 33.f;
constexpr float kBodyH = 236.f;
constexpr float kSubH = 11.f;
constexpr float kSubText = 13.f;
constexpr float kSubTracking = 1.5f;
constexpr float kArtW = 570.f;
constexpr float kColumnGap = 16.f;
// The stick: the X button's center in the art, and pixels per millimetre.
constexpr float kStickX = 225.f;
constexpr float kStickY = 104.f;
constexpr float kStickScale = 2.3f;
// Where the labels stand, in millimetres from their part toward the stick's far edge, and below that in units.
constexpr float kLeverLabel = -31.f;
constexpr float kZoomBelow = 19.f;
constexpr float kButtonLabel = 25.f;
constexpr float kBelowLabel = -24.f;  // under B and START
constexpr float kBelowDrop = 8.f;
constexpr float kLabelText = 17.f;
constexpr float kRowGap = 14.f;
constexpr float kKeyLineGap = 8.f;	// between the two lines of keys
constexpr float kLineRowH = 18.f;
constexpr float kHintText = 17.6f;
constexpr float kIconW = 18.f;
constexpr float kCap = 24.f;
constexpr float kCapPad = 6.f;
constexpr float kCapRounding = 5.f;
constexpr float kCapText = 14.f;
constexpr float kDashGap = 5.f;
constexpr float kSwitchH = 22.f;
constexpr float kSwitchPad = 9.f;
constexpr float kSwitchText = 15.f;
constexpr float kUnusedKeys = 0.4f;	 // the number keys' opacity in timeline mode, where they pick no one

constexpr ImU32 kBack = IM_COL32(10, 12, 16, 199);
constexpr ImU32 kAccent = IM_COL32(66, 150, 250, 255);
constexpr ImU32 kLit = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kPlain = IM_COL32(255, 255, 255, 230);
constexpr ImU32 kDim = IM_COL32(255, 255, 255, 102);

struct Guide {
	std::atomic<u32> kcode{~0u};  // the first player's buttons, pressed = bit clear
	int frame = -10;			  // the ImGui frame it was last drawn in
	float shown = 0.f;			  // 0 gone to 1 fully up
	ImVec2 lever;				  // where the drawn lever leans
};

Guide guide;

float Ease(float t) { return t * t * (3.f - 2.f * t); }

// Colors as the guide fades.
struct Ink {
	float fade;
	ImU32 operator()(ImU32 color) const {
		const float a = static_cast<float>(color >> IM_COL32_A_SHIFT & 0xff) * fade;
		return (color & ~IM_COL32_A_MASK) | static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT;
	}
};

// Text with its line centered on y; returns where it ends.
float Line(ImDrawList *list, ImFont *font, float size, float x, float y, ImU32 color, const char *text) {
	list->AddText(font, size, ImVec2(x, y - size * 0.5f), color, text);
	return x + font->CalcTextSizeA(size, FLT_MAX, 0.f, text).x;
}

// Capitals spaced out, the line centered on y.
void Spaced(ImDrawList *list, ImFont *font, float size, float tracking, float x, float y, ImU32 color, const char *text) {
	std::string upper(text);
	for (char &c : upper) {
		if ('a' <= c && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
	}
	for (size_t i = 0; i < upper.size();) {
		size_t next = i + 1;
		while (next < upper.size() && (upper[next] & 0xc0) == 0x80) next++;
		const char *begin = upper.data() + i, *end = upper.data() + next;
		list->AddText(font, size, ImVec2(x, y - size * 0.5f), color, begin, end);
		x += font->CalcTextSizeA(size, FLT_MAX, 0.f, begin, end).x + tracking;
		i = next;
	}
}

enum class Arrows { None, Across, Upright };

// A label of the drawing, centered on at.x with its baseline at at.y, after drawn ◀▶ or ▲▼. ▲▼ sit closer: the same gap
// looks wider between their corners than between the tips of ◀▶.
void Label(ImDrawList *list, ImFont *font, float size, ImVec2 at, ImU32 color, Arrows arrows, const char *text) {
	const bool across = arrows == Arrows::Across;
	const float w = (across ? 0.47f : 0.56f) * size, h = (across ? 0.56f : 0.47f) * size;
	const float gap = (across ? 0.53f : 0.13f) * size, space = (across ? 0.6f : 0.47f) * size;
	const float lead = arrows == Arrows::None ? 0.f : w * 2.f + gap + space;
	float x = at.x - (lead + font->CalcTextSizeA(size, FLT_MAX, 0.f, text).x) * 0.5f;
	const float mid = at.y - 0.3f * size;  // the capitals' middle
	if (across) {
		list->AddTriangleFilled(ImVec2(x, mid), ImVec2(x + w, mid - h * 0.5f), ImVec2(x + w, mid + h * 0.5f), color);
		x += w + gap;
		list->AddTriangleFilled(ImVec2(x + w, mid), ImVec2(x, mid + h * 0.5f), ImVec2(x, mid - h * 0.5f), color);
		x += w + space;
	} else if (arrows == Arrows::Upright) {
		list->AddTriangleFilled(ImVec2(x + w * 0.5f, mid - h * 0.5f), ImVec2(x + w, mid + h * 0.5f), ImVec2(x, mid + h * 0.5f), color);
		x += w + gap;
		list->AddTriangleFilled(ImVec2(x, mid - h * 0.5f), ImVec2(x + w, mid - h * 0.5f), ImVec2(x + w * 0.5f, mid + h * 0.5f), color);
		x += w + space;
	}
	list->AddText(font, size, ImVec2(x, at.y - font->GetFontBaked(size)->Ascent), color, text);
}

// A key cap, its middle on y; returns where it ends.
float Cap(ImDrawList *list, float unit, float x, float y, const Ink &ink, const char *text) {
	const float size = kCapText * unit;
	const float text_w = boldFont->CalcTextSizeA(size, FLT_MAX, 0.f, text).x;
	const ImVec2 min(x, y - kCap * 0.5f * unit);
	const ImVec2 max(x + std::max(kCap * unit, text_w + kCapPad * 2.f * unit), y + kCap * 0.5f * unit);
	const float rounding = kCapRounding * unit;
	list->AddRectFilled(min, max, ink(IM_COL32(255, 255, 255, 31)), rounding);
	// Its near edge in shadow.
	list->PushClipRect(ImVec2(min.x, max.y - 2.f * unit), max, true);
	list->AddRectFilled(min, max, ink(IM_COL32(0, 0, 0, 89)), rounding);
	list->PopClipRect();
	list->AddRect(min, max, ink(IM_COL32(255, 255, 255, 46)), rounding, 0, unit);
	list->AddText(boldFont, size, ImVec2((min.x + max.x - text_w) * 0.5f, y - size * 0.5f), ink(IM_COL32_WHITE), text);
	return max.x;
}

// The switch START flips, centered on at: the lever's two modes side by side in a pill, the one in use filled.
void ModeSwitch(ImDrawList *list, ImFont *font, float unit, ImVec2 at, const Ink &ink, bool camera) {
	const float size = kSwitchText * unit, pad = kSwitchPad * unit, h = kSwitchH * unit;
	const char *const names[] = {i18n::T("Camera"), i18n::T("Timeline")};
	const float widths[] = {font->CalcTextSizeA(size, FLT_MAX, 0.f, names[0]).x + pad * 2.f,
							font->CalcTextSizeA(size, FLT_MAX, 0.f, names[1]).x + pad * 2.f};
	const ImVec2 min(at.x - (widths[0] + widths[1]) * 0.5f, at.y - h * 0.5f);
	const ImVec2 max(min.x + widths[0] + widths[1], min.y + h);
	list->AddRectFilled(min, max, ink(IM_COL32(255, 255, 255, 20)), h * 0.5f);
	float x = min.x;
	for (int i = 0; i < 2; i++) {
		const bool lit = (i == 0) == camera;
		if (lit) list->AddRectFilled(ImVec2(x, min.y), ImVec2(x + widths[i], max.y), ink(kAccent), h * 0.5f);
		Line(list, font, size, x + pad, at.y, ink(lit ? kLit : IM_COL32(255, 255, 255, 140)), names[i]);
		x += widths[i];
	}
	list->AddRect(min, max, ink(IM_COL32(255, 255, 255, 46)), h * 0.5f, 0, unit);
}
}  // namespace

void gdxsv_camera_guide_buttons(u32 kcode) { guide.kcode = kcode; }

void gdxsv_camera_render_guide(bool hovered) {
	const ImGuiIO &io = ImGui::GetIO();
	// Drawn again after a gap, as in the next battle, it starts from nothing.
	const int frame = ImGui::GetFrameCount();
	if (guide.frame != frame - 1) guide.shown = 0.f;
	guide.frame = frame;
	guide.shown = std::clamp(guide.shown + (hovered ? 1.f : -1.f) * io.DeltaTime / kFadeSeconds, 0.f, 1.f);
	if (guide.shown <= 0.f) return;
	const Ink ink{Ease(guide.shown)};

	// Centered over the game's picture and scaled with it.
	const ImVec2 display = io.DisplaySize;
	const float aspect = gdxsv_camera_picture_aspect(display.x / display.y);
	const float picture_h = display.x / display.y >= aspect ? display.y : display.x / aspect;
	const float picture_w = picture_h * aspect;
	const float unit = std::min(picture_h / 540.f, picture_w / (kBoxW + kEdge * 2.f));
	const ImVec2 box((display.x - kBoxW * unit) * 0.5f, (display.y - picture_h) * 0.5f + (kBoxTop + (1.f - ink.fade) * kDrop) * unit);
	const ImVec2 box_max(box.x + kBoxW * unit, box.y + kBoxH * unit);
	const auto at = [&](float x, float y) { return ImVec2(box.x + x * unit, box.y + y * unit); };

	// It takes no input: a drag over it still turns the view.
	ImGui::SetNextWindowPos(box);
	ImGui::SetNextWindowSize(ImVec2(box_max.x - box.x, box_max.y - box.y));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::Begin("##gods-eye-guide", nullptr,
				 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
					 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
	ImGui::PopStyleVar(2);
	ImDrawList *list = ImGui::GetWindowDrawList();
	ImFont *font = ImGui::GetFont();
	list->AddRectFilled(box, box_max, ink(kBack), kRounding * unit);
	list->AddRect(box, box_max, ink(IM_COL32(255, 255, 255, 20)), kRounding * unit, 0, unit);
	const float title = kTitleText * unit;
	const float title_y = box.y + (kPadTop + kHeaderH * 0.5f) * unit;
	const float title_x = Line(list, font, title, box.x + kPadSide * unit, title_y, ink(kAccent), ICON_FA_EYE);
	Line(list, boldFont, title, title_x + kGap * unit, title_y, ink(IM_COL32(255, 255, 255, 235)), i18n::T("God's Eye View"));
	const float sub = kSubText * unit, tracking = kSubTracking * unit;
	const ImU32 sub_color = ink(IM_COL32(255, 255, 255, 102));
	Spaced(list, boldFont, sub, tracking, box.x + kPadSide * unit, box.y + (kBodyTop + kSubH * 0.5f) * unit, sub_color,
		   i18n::T("Stick or pad"));

	// The stick as the first player holds it. Camera mode gives the view the lever, X, Y and B; timeline mode gives the
	// lever to the replay and leaves X, Y and B unused.
	const u32 kcode = guide.kcode;
	const auto held = [&](u32 button) { return (~kcode & button) != 0; };
	const bool camera = gdxsv_camera_settings.dpad;
	const ImVec2 lean((held(DC_DPAD_RIGHT) ? 1.f : 0.f) - (held(DC_DPAD_LEFT) ? 1.f : 0.f),
					  (held(DC_DPAD_UP) ? 1.f : 0.f) - (held(DC_DPAD_DOWN) ? 1.f : 0.f));
	const float follow = 1.f - std::exp(-io.DeltaTime / kLeverSeconds);
	guide.lever = ImVec2(guide.lever.x + (lean.x - guide.lever.x) * follow, guide.lever.y + (lean.y - guide.lever.y) * follow);
	const auto part = [](GdxsvFightstickPart p) { return static_cast<int>(p); };
	GdxsvFightstickLook look;
	look.lever_x = guide.lever.x;
	look.lever_y = guide.lever.y;
	look.held[part(GdxsvFightstickPart::X)] = held(DC_BTN_X);
	look.held[part(GdxsvFightstickPart::Y)] = held(DC_BTN_Y);
	look.held[part(GdxsvFightstickPart::A)] = held(DC_BTN_A);
	look.held[part(GdxsvFightstickPart::B)] = held(DC_BTN_B);
	look.held[part(GdxsvFightstickPart::Start)] = held(DC_BTN_START);
	look.grey[part(GdxsvFightstickPart::X)] = look.grey[part(GdxsvFightstickPart::Y)] = look.grey[part(GdxsvFightstickPart::B)] = !camera;
	look.with_start = true;
	look.alpha = ink.fade;
	const ImVec2 origin = at(kPadSide + kStickX, kBodyTop + kSubH + kStickY);
	const float scale = kStickScale * unit;
	gdxsv_fightstick_draw(list, origin, scale, look);

	// A label lights in its part's color while that part does it, and is grey while the part does nothing.
	const float label = kLabelText * unit;
	const auto point = [&](GdxsvFightstickPart p, float away) { return gdxsv_fightstick_point(origin, scale, p, away); };
	const auto shade = [&](GdxsvFightstickPart p, bool doing, bool works) {
		return ink(!works ? kDim : doing ? gdxsv_fightstick_color(p) : kPlain);
	};
	const ImVec2 lever = point(GdxsvFightstickPart::Lever, kLeverLabel);
	Label(list, font, label, lever, shade(GdxsvFightstickPart::Lever, lean.x != 0.f, true), Arrows::Across,
		  camera ? i18n::T("Turn") : i18n::T("Seek"));
	Label(list, font, label, ImVec2(lever.x, lever.y + kZoomBelow * unit), shade(GdxsvFightstickPart::Lever, lean.y != 0.f, true),
		  Arrows::Upright, camera ? i18n::T("Zoom") : i18n::T("Speed"));
	Label(list, font, label, point(GdxsvFightstickPart::X, kButtonLabel), shade(GdxsvFightstickPart::X, held(DC_BTN_X), camera),
		  Arrows::None, i18n::T("Tilt down"));
	Label(list, font, label, point(GdxsvFightstickPart::Y, kButtonLabel), shade(GdxsvFightstickPart::Y, held(DC_BTN_Y), camera),
		  Arrows::None, i18n::T("Tilt up"));
	Label(list, font, label, point(GdxsvFightstickPart::A, kButtonLabel), shade(GdxsvFightstickPart::A, held(DC_BTN_A), true),
		  Arrows::None, i18n::T("Pause"));
	const ImVec2 b = point(GdxsvFightstickPart::B, kBelowLabel);
	Label(list, font, label, ImVec2(b.x, b.y + kBelowDrop * unit), shade(GdxsvFightstickPart::B, held(DC_BTN_B), camera), Arrows::None,
		  i18n::T("Next player"));
	// START gives the lever to the view or back to the replay's timeline controls.
	const ImVec2 start = point(GdxsvFightstickPart::Start, kBelowLabel);
	ModeSwitch(list, font, unit, ImVec2(start.x, start.y + kBelowDrop * unit - 0.3f * label), ink, camera);

	// Beside it the keyboard and mouse, the column centered on the stick's: the number keys on two lines, then the mouse.
	const float keys_h = kCap * 2.f + kKeyLineGap;
	const float column = kSubH + kRowGap * 3.f + keys_h + kLineRowH * 2.f;
	const float side_x = box.x + (kPadSide + kArtW + kColumnGap) * unit;
	float y = kBodyTop + (kBodyH - column) * 0.5f;
	Spaced(list, boldFont, sub, tracking, side_x, box.y + (y + kSubH * 0.5f) * unit, sub_color, i18n::T("Keyboard and mouse"));
	y += kSubH + kRowGap;
	const float text = kHintText * unit;
	const ImU32 words = ink(kPlain);
	const Ink keys{ink.fade * (camera ? 1.f : kUnusedKeys)};
	const float first = box.y + (y + kCap * 0.5f) * unit;
	const float second = box.y + (y + kCap * 1.5f + kKeyLineGap) * unit;
	float x = Cap(list, unit, side_x, first, keys, "1");
	x = Line(list, font, text, x + kDashGap * unit, first, keys(IM_COL32(255, 255, 255, 128)), "–");
	x = Cap(list, unit, x + kDashGap * unit, first, keys, "4");
	Line(list, font, text, x + kGap * unit, first, keys(kPlain), i18n::T("Follow players"));
	x = Cap(list, unit, side_x, second, keys, "5");
	Line(list, font, text, x + kGap * unit, second, keys(kPlain), "All");
	y += keys_h + kRowGap;
	for (const bool drag : {true, false}) {
		const char *icon = drag ? ICON_FA_COMPUTER_MOUSE : ICON_FA_ARROWS_UP_DOWN;
		const float mid = box.y + (y + kLineRowH * 0.5f) * unit;
		const float icon_w = font->CalcTextSizeA(text, FLT_MAX, 0.f, icon).x;
		Line(list, font, text, side_x + (kIconW * unit - icon_w) * 0.5f, mid, ink(IM_COL32(255, 255, 255, 191)), icon);
		Line(list, font, text, side_x + (kIconW + kGap) * unit, mid, words, drag ? i18n::T("Drag: turn and tilt") : i18n::T("Scroll: zoom"));
		y += kLineRowH + kRowGap;
	}
	ImGui::End();
}
