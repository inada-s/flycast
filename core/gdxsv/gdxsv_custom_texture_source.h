#pragma once

#include "zip.h"
#include "gdxsv_config.h"
#include "gdxsv_font_provider.h"

// clang-format off
#include "../rend/TexCache.h"
#include "../rend/CustomTexture.h"
// clang-format on

class GdxsvEmbedTextureSource : public BaseCustomTextureSource {
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
	static u8* LoadExtraTexture(const char* name, bool v_flip, int& width, int& height);

   private:
	static bool langmodDisabled();
	std::string textures_path;
	std::map<u32, std::string> texture_map;
};

class GdxsvTexturePackSource : public BaseCustomTextureSource {
   public:
	GdxsvTexturePackSource();
	~GdxsvTexturePackSource() override;
	// The font glyphs drawn from an outline font fill in where the pack has no entry.
	bool shouldReplace() const override { return config::GdxUseTexturePack || config::GdxFontRaster; }
	bool shouldPreload() const override { return false; }
	bool loadMap() override;
	size_t getTextureCount() const override { return texture_map.size(); }
	void preloadTextures(TextureCallback callback, std::atomic<bool>* stop_flag) override;
	void terminate() override;
	u8* loadCustomTexture(u32 hash, int& width, int& height) override;
	bool isTextureReplaced(u32 hash) override;

   private:
	std::map<u32, int> texture_map;
	zip_t* texp_zip = nullptr;
	GdxsvFontProvider font;
};
