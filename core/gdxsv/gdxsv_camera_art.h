// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// The god's eye view's nameplate art: each MS's image and the force logos, from one image of the ex texture pack.
#pragma once
#include "imgui.h"

// A picture in a texture, upright. Its size is in texels of the briefing's pictures.
struct GdxsvCameraSprite {
	ImTextureID texture{};
	ImVec2 uv0, uv1;
	float w = 0.f;
	float h = 0.f;
	explicit operator bool() const { return texture != ImTextureID{}; }
};

// Render thread. Empty until read: the first request starts reading it in the background.
GdxsvCameraSprite gdxsv_camera_art_ms(int ms, int team);
GdxsvCameraSprite gdxsv_camera_art_logo(int team);

// Stops the reading and forgets the image.
void gdxsv_camera_art_reset();
