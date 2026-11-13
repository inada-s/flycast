// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_camera_art.h"

#include <stb_image.h>

#include <memory>
#include <mutex>
#include <vector>

#include "gdxsv_custom_texture_source.h"
#include "log/Log.h"
#include "types.h"
#include "ui/imgui_driver.h"
#include "util/worker_thread.h"

namespace {

// One image holds every unit and the team logos. Rectangles count in its 1725 by 498 units, whatever size the file is.
// The game paints an MS in its team's colors, whichever force the MS is from, and the Dreamcast has no picture of that, so each
// MS is here twice: the Zeon team's picture is the Federation team's rectangle, kZeonTeamOffset lower. An MA is the same for both.
constexpr char kImage[] = "camera-units-1.png";  // in the ex texture pack
constexpr float kImageWidth = 1725.f;
constexpr float kImageHeight = 498.f;
constexpr float kZeonTeamOffset = 146.f;
// Of the briefing's pictures, the measure sprite sizes are given in. ImGui textures have no mip levels, so a denser image aliases.
constexpr float kUnitsPerTexel = 1.53f;

struct Rect {
	float x, y, w, h;
};

struct UnitSprite {
	int ms;
	Rect rect;
};

// The Federation team's row: Federation's units, then Zeon's, each by MS id.
constexpr UnitSprite kMsSprites[] = {
	{0x00, {8, 51, 53, 95}},	   // Gundam
	{0x01, {69, 43, 73, 103}},	   // Guncannon
	{0x02, {150, 51, 57, 95}},	   // GM
	{0x11, {215, 8, 87, 138}},	   // Guntank
	{0x13, {310, 52, 47, 94}},	   // Ground Type Gundam
	{0x14, {365, 52, 48, 94}},	   // Ground Type GM
	{0x21, {421, 49, 89, 97}},	   // Ball
	{0x03, {518, 54, 46, 92}},	   // Old Zaku
	{0x04, {572, 54, 50, 92}},	   // Zaku
	{0x05, {630, 50, 50, 96}},	   // Char's Zaku
	{0x06, {688, 49, 48, 97}},	   // Gouf
	{0x07, {744, 45, 72, 101}},	   // Dom
	{0x08, {824, 45, 72, 101}},	   // Rick Dom
	{0x09, {904, 40, 61, 106}},	   // Gelgoog
	{0x0a, {973, 41, 61, 105}},	   // Char's Gelgoog
	{0x0b, {1042, 44, 67, 102}},   // Gyan
	{0x0c, {1117, 44, 77, 102}},   // Gogg
	{0x0d, {1202, 44, 77, 102}},   // Acguy
	{0x0e, {1287, 47, 73, 99}},	   // Z'Gok
	{0x0f, {1368, 47, 73, 99}},	   // Char's Z'Gok
	{0x10, {1449, 26, 158, 120}},  // Zock
	{0x12, {1615, 55, 93, 91}},	   // Zeong
};

constexpr UnitSprite kMaSprites[] = {
	{0x20, {8, 372, 195, 118}},		// Elmeth
	{0x22, {211, 344, 191, 146}},	// Braw Bro
	{0x23, {410, 398, 191, 92}},	// Grublo
	{0x24, {609, 378, 193, 112}},	// Zakrello
	{0x25, {810, 447, 190, 43}},	// Bigro
	{0x26, {1008, 300, 144, 190}},	// Big Zam
	{0x27, {1160, 404, 191, 86}},	// Adzam
	{0x28, {1359, 422, 190, 68}},	// G-Fighter
};

// By team, same square around each.
constexpr Rect kLogos[] = {
	{1557, 414, 76, 76},
	{1641, 414, 76, 76},
};

struct Art {
	std::mutex mutex;
	std::vector<u8> rgba;
	int w = 0;
	int h = 0;
	bool asked = false;
	WorkerThread worker{"GdxsvCamArt"};
};

Art art;

void ReadImage() {
	int w = 0;
	int h = 0;
	std::unique_ptr<u8, decltype(&stbi_image_free)> data(gdxsv_texture_pack_ex_image(kImage, w, h), stbi_image_free);
	if (data == nullptr) {
		WARN_LOG(COMMON, "Camera art: cannot load %s", kImage);
		return;
	}
	std::vector<u8> rgba(data.get(), data.get() + static_cast<size_t>(w) * h * 4);
	std::lock_guard<std::mutex> lock(art.mutex);
	art.rgba.swap(rgba);
	art.w = w;
	art.h = h;
}

GdxsvCameraSprite MakeSprite(const Rect &rect) {
	std::lock_guard<std::mutex> lock(art.mutex);
	GdxsvCameraSprite sprite;
	if (!art.asked) {
		art.asked = true;
		art.worker.run(ReadImage);
		return sprite;
	}
	if (art.rgba.empty()) return sprite;
	const ImTextureID id = imguiDriver->getTexture("gdxsv_camera_units");
	sprite.texture = id != ImTextureID{} ? id : imguiDriver->updateTexture("gdxsv_camera_units", art.rgba.data(), art.w, art.h, false);
	sprite.uv0 = ImVec2(rect.x / kImageWidth, rect.y / kImageHeight);
	sprite.uv1 = ImVec2((rect.x + rect.w) / kImageWidth, (rect.y + rect.h) / kImageHeight);
	sprite.w = rect.w / kUnitsPerTexel;
	sprite.h = rect.h / kUnitsPerTexel;
	return sprite;
}

}  // namespace

GdxsvCameraSprite gdxsv_camera_art_ms(int ms, int team) {
	for (const UnitSprite &sprite : kMsSprites) {
		const auto &r = sprite.rect;
		if (sprite.ms == ms) return MakeSprite({r.x, r.y + (team == 0 ? 0.f : kZeonTeamOffset), r.w, r.h});
	}
	for (const UnitSprite &sprite : kMaSprites) {
		if (sprite.ms == ms) return MakeSprite(sprite.rect);
	}
	return GdxsvCameraSprite{};
}

GdxsvCameraSprite gdxsv_camera_art_logo(int team) { return MakeSprite(kLogos[team]); }

void gdxsv_camera_art_reset() {
	art.worker.stop();
	std::lock_guard<std::mutex> lock(art.mutex);
	art.rgba.clear();
	art.w = 0;
	art.h = 0;
	art.asked = false;
}
