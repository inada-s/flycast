// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// An arcade stick drawn in perspective from the player's seat: the lever on the left, three buttons in an arc right of it
// and a fourth under the first, as a stick has them, named and colored as the Dreamcast pad's X, Y, A and B; START
// apart on the right.
#pragma once
#include "imgui.h"

enum class GdxsvFightstickPart { Lever, X, Y, A, B, Start };
constexpr int kGdxsvFightstickParts = 6;

struct GdxsvFightstickLook {
	float lever_x = 0.f, lever_y = 0.f;	 // where the lever leans: right and away from the player, -1 to 1
	bool held[kGdxsvFightstickParts] = {};
	bool grey[kGdxsvFightstickParts] = {};	// drawn without its colors, as a control that does nothing now
	float alpha = 1.f;
	bool with_start = false;
};

// origin: where the X button's center is drawn; scale: pixels per millimetre there.
void gdxsv_fightstick_draw(ImDrawList *list, ImVec2 origin, float scale, const GdxsvFightstickLook &look);
// A part's own color, light, for words that go with it.
ImU32 gdxsv_fightstick_color(GdxsvFightstickPart part);
// Where a point of the stick's top is drawn: millimetres from a part's center toward the far edge (away from the player).
ImVec2 gdxsv_fightstick_point(ImVec2 origin, float scale, GdxsvFightstickPart part, float away);
