// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>

#include "types.h"

// Replacement textures for DC2's 24 px font glyphs, drawn from an outline font
// (gdxsv:FontRaster). A glyph texture is known by its hash
// (gdxsv_font_glyphs.inc); the character is drawn in the game's 24 px box with
// a grey outline, at 4x.
class GdxsvFontProvider {
   public:
	GdxsvFontProvider();
	~GdxsvFontProvider();
	bool HasGlyph(u32 hash) const;
	// A malloc'ed RGBA image, rows top-down like the texture, or nullptr.
	// Thread-safe.
	u8* Render(u32 hash, int& width, int& height);
	// Frees the font and the cache.
	void Close();

   private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
