// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_font_provider.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_STROKER_H

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <list>
#include <mutex>
#include <nowide/cstdio.hpp>
#include <unordered_map>
#include <vector>

#include "gdxsv_config.h"
#include "log/Log.h"
#include "stdclass.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

struct GdxsvFontGlyph {
	u32 hash;
	u32 code_point;
	// horizontal centre of the drawn character in the 24 px box, in half game px
	u8 centre2;
};

const GdxsvFontGlyph kGlyphs[] = {
#include "gdxsv_font_glyphs.inc"
};

const char* const kFontName = "GenSenRounded2JP-R.otf";
// Output px per game px; a glyph texture is 32x32 game px.
constexpr int kScale = 4;
constexpr int kSize = 32 * kScale;
// Font size in game px: the game's ink box of a full-width character.
constexpr float kEm = 21.f;
// Vertical centre of the 24 px glyph box, game px.
constexpr int kMiddle = 12;
// Outline bands outermost first: stroke radius in output px, grey. The game tints texels with the vertex colour, so
// the grey shows as a dark outline as with the game's own 2-bit glyphs. The white core is drawn last.
constexpr struct {
	int radius;
	u8 grey;
} kBands[] = {{6, 70}, {3, 181}, {0, 255}};
constexpr size_t kCacheEntries = 256;

const GdxsvFontGlyph* find_glyph(u32 hash) {
	const GdxsvFontGlyph* end = kGlyphs + sizeof(kGlyphs) / sizeof(kGlyphs[0]);
	const GdxsvFontGlyph* it = std::lower_bound(kGlyphs, end, hash, [](const GdxsvFontGlyph& g, u32 h) { return g.hash < h; });
	return it != end && it->hash == hash ? it : nullptr;
}

// Composites a grey coverage bitmap over an RGBA image (rows top-down).
void blend(std::vector<u8>& image, const FT_BitmapGlyph bitmap, int left, int top, u8 grey) {
	const FT_Bitmap& b = bitmap->bitmap;
	for (unsigned y = 0; y < b.rows; y++) {
		const int iy = top + (int)y;
		if (iy < 0 || iy >= kSize) continue;
		for (unsigned x = 0; x < b.width; x++) {
			const int ix = left + (int)x;
			const u8 a = b.buffer[y * b.pitch + x];
			if (ix < 0 || ix >= kSize || a == 0) continue;
			u8* p = &image[(iy * kSize + ix) * 4];
			const float sa = a / 255.f;
			const float da = p[3] / 255.f;
			const float oa = sa + da * (1 - sa);
			const u8 c = (u8)std::lround((grey * sa + p[0] * da * (1 - sa)) / oa);
			p[0] = p[1] = p[2] = c;
			p[3] = (u8)std::lround(oa * 255);
		}
	}
}

}  // namespace

struct GdxsvFontProvider::Impl {
	std::mutex mutex;
	bool tried = false;
	FT_Library library = nullptr;
	FT_Face face = nullptr;
	FT_Stroker stroker = nullptr;
	std::vector<u8> file_data;

	// Rendered images, most recent first.
	std::list<std::pair<u32, std::vector<u8>>> cache;
	std::unordered_map<u32, decltype(cache)::iterator> cache_index;

	u64 render_count = 0;
	u64 render_us = 0;

	const u8* LoadFontData(size_t& size) {
		const std::string path = config::GdxFontRasterFile.get();
		if (path.empty()) {
#ifdef _WIN32
			HRSRC source = FindResourceA(GetModuleHandle(NULL), "GENSEN_ROUNDED2_JP_R", "GDXSV_FONT");
			if (source != nullptr) {
				HGLOBAL memory = LoadResource(NULL, source);
				if (memory != nullptr) {
					size = SizeofResource(NULL, source);
					return (const u8*)LockResource(memory);
				}
			}
#endif
		}
		const std::string file = path.empty() ? get_readonly_data_path(kFontName) : path;
		FILE* f = nowide::fopen(file.c_str(), "rb");
		if (f == nullptr) {
			WARN_LOG(COMMON, "FontRaster: cannot open %s", file.c_str());
			return nullptr;
		}
		std::fseek(f, 0, SEEK_END);
		const long len = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		if (len > 0) {
			file_data.resize(len);
			if (std::fread(file_data.data(), 1, len, f) != (size_t)len) file_data.clear();
		}
		std::fclose(f);
		size = file_data.size();
		return file_data.empty() ? nullptr : file_data.data();
	}

	bool Open() {
		if (tried) return face != nullptr;
		tried = true;
		const auto start = std::chrono::steady_clock::now();
		size_t size = 0;
		const u8* data = LoadFontData(size);
		if (data == nullptr) return false;
		if (FT_Init_FreeType(&library) != 0) {
			library = nullptr;
			return false;
		}
		if (FT_New_Memory_Face(library, data, (FT_Long)size, 0, &face) != 0 ||
			FT_Set_Char_Size(face, 0, (FT_F26Dot6)std::lround(kEm * kScale * 64), 72, 72) != 0 || FT_Stroker_New(library, &stroker) != 0) {
			ERROR_LOG(COMMON, "FontRaster: cannot load the font (%u B)", (unsigned)size);
			CloseLocked();
			tried = true;
			return false;
		}
		const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
		NOTICE_LOG(COMMON, "FontRaster: %s loaded in %d us, %u glyph textures", face->family_name, (int)us,
				   (unsigned)(sizeof(kGlyphs) / sizeof(kGlyphs[0])));
		return true;
	}

	void CloseLocked() {
		if (stroker != nullptr) FT_Stroker_Done(stroker);
		if (face != nullptr) FT_Done_Face(face);
		if (library != nullptr) FT_Done_FreeType(library);
		stroker = nullptr;
		face = nullptr;
		library = nullptr;
		file_data.clear();
		file_data.shrink_to_fit();
		cache.clear();
		cache_index.clear();
		tried = false;
	}

	// Draws the character in the game's 24 px box like the game lays it out: horizontally centred on centre2 / 2,
	// vertically on the box middle (the middle between the font's ascender and descender).
	bool Draw(const GdxsvFontGlyph& glyph, std::vector<u8>& image) {
		if (FT_Load_Char(face, glyph.code_point, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) != 0) return false;
		const FT_Pos advance = face->glyph->advance.x;
		const FT_Size_Metrics& m = face->size->metrics;
		const FT_Pos baseline = kMiddle * kScale * 64 + (m.ascender + m.descender) / 2;
		const FT_Pos pen_x = glyph.centre2 * kScale * 32 - advance / 2;
		image.assign(kSize * kSize * 4, 0);
		for (const auto& band : kBands) {
			FT_Glyph g;
			if (FT_Get_Glyph(face->glyph, &g) != 0) return false;
			if (band.radius > 0) {
				FT_Stroker_Set(stroker, band.radius * 64, FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);
				FT_Glyph_StrokeBorder(&g, stroker, false, true);
			}
			FT_Vector origin = {pen_x & 63, -(baseline & 63)};
			if (FT_Glyph_To_Bitmap(&g, FT_RENDER_MODE_NORMAL, &origin, true) != 0) {
				FT_Done_Glyph(g);
				return false;
			}
			const FT_BitmapGlyph bitmap = (FT_BitmapGlyph)g;
			blend(image, bitmap, (int)(pen_x >> 6) + bitmap->left, (int)(baseline >> 6) - bitmap->top, band.grey);
			FT_Done_Glyph(g);
		}
		return true;
	}
};

GdxsvFontProvider::GdxsvFontProvider() : impl(new Impl()) {}

GdxsvFontProvider::~GdxsvFontProvider() { Close(); }

bool GdxsvFontProvider::HasGlyph(u32 hash) const { return find_glyph(hash) != nullptr; }

u8* GdxsvFontProvider::Render(u32 hash, int& width, int& height) {
	const GdxsvFontGlyph* glyph = find_glyph(hash);
	if (glyph == nullptr) return nullptr;
	std::lock_guard<std::mutex> lock(impl->mutex);
	auto it = impl->cache_index.find(hash);
	if (it == impl->cache_index.end()) {
		if (!impl->Open()) return nullptr;
		const auto start = std::chrono::steady_clock::now();
		std::vector<u8> image;
		if (!impl->Draw(*glyph, image)) return nullptr;
		impl->cache.emplace_front(hash, std::move(image));
		impl->cache_index[hash] = impl->cache.begin();
		if (impl->cache.size() > kCacheEntries) {
			impl->cache_index.erase(impl->cache.back().first);
			impl->cache.pop_back();
		}
		impl->render_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
		if (++impl->render_count % 256 == 1)
			NOTICE_LOG(COMMON, "FontRaster: %u glyphs drawn, %u us each", (unsigned)impl->render_count,
					   (unsigned)(impl->render_us / impl->render_count));
	} else {
		impl->cache.splice(impl->cache.begin(), impl->cache, it->second);
	}
	const std::vector<u8>& image = impl->cache.front().second;
	u8* data = (u8*)std::malloc(image.size());
	if (data == nullptr) return nullptr;
	memcpy(data, image.data(), image.size());
	width = height = kSize;
	return data;
}

void GdxsvFontProvider::Close() {
	std::lock_guard<std::mutex> lock(impl->mutex);
	impl->CloseLocked();
}
