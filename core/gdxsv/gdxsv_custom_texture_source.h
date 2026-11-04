#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include "zip.h"
#include "gdxsv_config.h"
#include "gdxsv_custom_texture_cells.h"
#include "gdxsv_font_provider.h"

// clang-format off
#include "../rend/TexCache.h"
#include "../rend/CustomTexture.h"
// clang-format on

// gdxsv:FontRaster: DC1/DC2's text drawn from the embedded GDXGothic (gdxsv_font_raster_source.cpp). Added after the
// data/textures folder and before the embedded and downloaded textures, so that it wins over them.
class GdxsvFontRasterSource : public BaseCustomTextureSource, public GdxsvCellTextureSource {
   public:
	GdxsvFontRasterSource();
	~GdxsvFontRasterSource() override;
	bool shouldReplace() const override { return config::GdxFontRaster; }
	// Draws the battle-message sheet's characters, before a battle needs them.
	bool loadMap() override;
	void terminate() override;
	u8* loadCustomTexture(u32 hash, int& width, int& height) override;
	bool isTextureReplaced(u32 hash) override;
	// The glyph drawn in the battle name style.
	u8* LoadCellTexture(u32 hash, int& width, int& height) override;
	// Keeps the game's battle-message sheet from VRAM: the characters are drawn over it.
	void OnLoadQueued(const BaseTextureCacheData& texture) override;

   private:
	GdxsvFontProvider font;
	// The battle-message sheet's characters at its output size, drawn by loadMap on the loader thread; ready is also read
	// on the render thread
	std::vector<u8> message_chars;
	std::atomic<bool> message_chars_ready{false};
	// The game's sheet as last queued: its palette indices from VRAM and its palette as RGBA
	std::mutex message_vram_mutex;
	std::vector<u8> message_texels;
	std::vector<u8> message_palette;
};

class GdxsvEmbedTextureSource : public BaseCustomTextureSource, public GdxsvCellTextureSource {
   public:
	GdxsvEmbedTextureSource();
	~GdxsvEmbedTextureSource() override;
	bool shouldReplace() const override { return !langmodDisabled(); }
	bool shouldPreload() const override { return !langmodDisabled() && config::PreloadCustomTextures; }
	bool loadMap() override;
	size_t getTextureCount() const override { return texture_map.size(); }
	u8* loadCustomTexture(u32 hash, int& width, int& height) override;
	bool isTextureReplaced(u32 hash) override;
	void preloadTextures(TextureCallback callback, std::atomic<bool>* stop_flag) override;
	u8* LoadTexture(u32 hash, int& width, int& height) override {
		return shouldReplace() && isTextureReplaced(hash) ? loadCustomTexture(hash, width, height) : nullptr;
	}
	static u8* LoadExtraTexture(const char* name, bool v_flip, int& width, int& height);

   private:
	static bool langmodDisabled();
	std::string textures_path;
	std::map<u32, std::string> texture_map;
};

bool gdxsv_texture_pack_wanted();

class GdxsvTexturePackSource : public BaseCustomTextureSource, public GdxsvCellTextureSource {
   public:
	GdxsvTexturePackSource();
	~GdxsvTexturePackSource() override;
	bool shouldReplace() const override { return gdxsv_texture_pack_wanted(); }
	bool shouldPreload() const override { return false; }
	bool loadMap() override;
	size_t getTextureCount() const override { return texture_map.size(); }
	void preloadTextures(TextureCallback callback, std::atomic<bool>* stop_flag) override;
	void terminate() override;
	u8* loadCustomTexture(u32 hash, int& width, int& height) override;
	bool isTextureReplaced(u32 hash) override;
	u8* LoadTexture(u32 hash, int& width, int& height) override {
		return shouldReplace() && isTextureReplaced(hash) ? loadCustomTexture(hash, width, height) : nullptr;
	}

   private:
	std::map<u32, int> texture_map;
	zip_t* texp_zip = nullptr;
};
