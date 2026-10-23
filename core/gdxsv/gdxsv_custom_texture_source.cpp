#ifdef _WIN32
#define _AMD64_	 // Fixing GitHub runner's winnt.h error
#endif

#include "gdxsv_custom_texture_source.h"
#include "gdxsv_config.h"
#include "gdxsv_custom_texture_cells.h"

#include <stb_image.h>
#include <zip.h>

#include "gdxsv_translation.h"
#include "stdclass.h"
#include "oslib/storage.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>

#include <regex>

#include "oslib/directory.h"
#elif _WIN32
#include <winbase.h>
#include <winnls.h>
#include <winuser.h>
#endif

static std::string get_game_id() {
	std::string game_id(settings.content.gameId);
	const size_t str_end = game_id.find_last_not_of(' ');
	if (str_end == std::string::npos) return "";
	game_id = game_id.substr(0, str_end + 1);
	std::replace(game_id.begin(), game_id.end(), ' ', '_');
	return game_id;
}

static std::string get_textures_path() {
#ifdef __APPLE__
	uint32_t bufSize = PATH_MAX + 1;
	char result[PATH_MAX + 1];
	if (_NSGetExecutablePath(result, &bufSize) == 0) {
		auto path = std::string(result);
		size_t pos = path.find("MacOS/Flycast");
		if (pos != std::string::npos) {
			path.replace(pos, sizeof("MacOS/Flycast") - 1, "Resources/Textures/");
			return path;
		}
	}
#endif
	return "";
}

GdxsvEmbedTextureSource::GdxsvEmbedTextureSource() {
	gdxsv_custom_texture_cells_add_source(this, false);
#ifdef __APPLE__
	textures_path = get_textures_path();
	DIR* dir = flycast::opendir(textures_path.c_str());
	if (dir != nullptr) {
		INFO_LOG(RENDERER, "Found custom textures directory: %s", textures_path.c_str());
		closedir(dir);
	}
#endif
}

bool GdxsvEmbedTextureSource::langmodDisabled() {
	return GdxsvLanguage::Language() == GdxsvLanguage::Lang::Disabled;
}

#ifdef _WIN32
static BOOL CALLBACK StaticEnumRCNamesFunc(HMODULE hModule, LPCTSTR lpType, LPTSTR lpName, LONG_PTR lParam) {
	// Only add target language's hash & resource index into texture_map
	auto mapping = reinterpret_cast<std::map<u32, std::string>*>(lParam);
	const auto name = std::string(lpName);
	auto lang_dir = GdxsvLanguage::TextureDirectoryName();
	std::transform(lang_dir.begin(), lang_dir.end(), lang_dir.begin(), ::toupper);

	if (name.find(lang_dir) == 0 || name.find("COMMON") == 0) {
		auto tex_hash = name.substr(name.find_last_of('_') + 1);
		u32 hash = strtoul(tex_hash.c_str(), NULL, 16);
		mapping->emplace(hash, name);
	}

	return true;
}
#endif

bool GdxsvEmbedTextureSource::loadMap() {
	std::map<u32, std::string> mapping;

	if (GdxsvLanguage::Language() != GdxsvLanguage::Lang::Disabled) {
		// Normal image files by language
		if (!textures_path.empty()) {
			std::string localized_textures_path = textures_path + GdxsvLanguage::TextureDirectoryName();
			hostfs::DirectoryTree tree(localized_textures_path);
			for (const hostfs::FileInfo& item : tree) {
				std::string extension = get_file_extension(item.name);
				if (extension != "jpg" && extension != "jpeg" && extension != "png") continue;
				std::string::size_type dotpos = item.name.find_last_of('.');
				std::string basename = item.name.substr(0, dotpos);
				char* endptr;
				u32 hash = (u32)strtoll(basename.c_str(), &endptr, 16);
				if (endptr - basename.c_str() < (ptrdiff_t)basename.length()) {
					INFO_LOG(RENDERER, "Invalid hash %s", basename.c_str());
					continue;
				}
				mapping.emplace(hash, item.path);
			}
		}

#if _WIN32
		// Embed textures (mainly localization)
		const auto ret = EnumResourceNames(GetModuleHandle(NULL), "GDXSV_TEXTURE", (ENUMRESNAMEPROC)&StaticEnumRCNamesFunc,
										   reinterpret_cast<LONG_PTR>(&mapping));
		if (!ret) {
			ERROR_LOG(COMMON, "EnumResourceNames error:%d", GetLastError());
		}
#endif
	}

	texture_map = mapping;
	return !texture_map.empty();
}

u8* GdxsvEmbedTextureSource::loadCustomTexture(u32 hash, int& width, int& height) {
	const auto it = texture_map.find(hash);
	if (it == texture_map.end()) return nullptr;

#if _WIN32
	// Load texture from EnumResource
	HRSRC source = FindResourceA(GetModuleHandle(NULL), it->second.c_str(), "GDXSV_TEXTURE");
	if (source != NULL) {
		unsigned int size = SizeofResource(NULL, source);
		HGLOBAL memory = LoadResource(NULL, source);

		if (memory != NULL) {
			void* data = LockResource(memory);

			int n;
			stbi_set_flip_vertically_on_load(1);
			u8* imgData = stbi_load_from_memory(static_cast<unsigned char*>(data), size, &width, &height, &n, STBI_rgb_alpha);

			FreeResource(memory);
			return imgData;
		}
	}
	return nullptr;

#else
	// Load texture from file
	FILE* file = nowide::fopen(it->second.c_str(), "rb");
	if (file == nullptr) return nullptr;
	int n;
	stbi_set_flip_vertically_on_load(1);
	u8* imgData = stbi_load_from_file(file, &width, &height, &n, STBI_rgb_alpha);
	std::fclose(file);
	return imgData;
#endif
}

bool GdxsvEmbedTextureSource::isTextureReplaced(u32 hash) {
	return texture_map.find(hash) != texture_map.end();
}

void GdxsvEmbedTextureSource::preloadTextures(TextureCallback callback, std::atomic<bool> *stop_flag) {
	for (const auto& [hash, path] : texture_map)
	{
		if (stop_flag != nullptr && *stop_flag)
			return;
		int w, h;
		u8* data = loadCustomTexture(hash, w, h);
		if (data != nullptr)
		{
			size_t size = (size_t)w * h * 4;
			TextureData tex;
			tex.w = w;
			tex.h = h;
			tex.data.resize(size);
			memcpy(tex.data.data(), data, size);
			stbi_image_free(data);
			callback(hash, std::move(tex));
		}
	}
}

u8* GdxsvEmbedTextureSource::LoadExtraTexture(const char* name, bool v_flip, int& width, int& height) {
#ifdef _WIN32
	u8* imgData = nullptr;
	std::string upper_name = name;
	std::transform(upper_name.begin(), upper_name.end(), upper_name.begin(), ::toupper);
	if (upper_name.find_last_of("PNG") != std::string::npos) {
		upper_name = upper_name.substr(0, upper_name.find_last_of('.'));
	}
	HRSRC source = FindResourceA(GetModuleHandle(NULL), ("EXTRA_" + upper_name).c_str(), "GDXSV_TEXTURE");
	if (source != NULL) {
		unsigned int size = SizeofResource(NULL, source);
		HGLOBAL memory = LoadResource(NULL, source);

		if (memory != NULL) {
			void* data = LockResource(memory);

			int n;
			stbi_set_flip_vertically_on_load((int)v_flip);
			imgData = stbi_load_from_memory(static_cast<unsigned char*>(data), size, &width, &height, &n, STBI_rgb_alpha);

			FreeResource(memory);
		}
	}
	return imgData;

#else

	std::string path = get_textures_path() + "/Extra/" + name;
	FILE* file = nowide::fopen(path.c_str(), "rb");
	if (file == nullptr) return nullptr;
	int n;
	stbi_set_flip_vertically_on_load((int)v_flip);
	u8* imgData = stbi_load_from_file(file, &width, &height, &n, STBI_rgb_alpha);
	std::fclose(file);
	return imgData;

#endif
}

GdxsvEmbedTextureSource::~GdxsvEmbedTextureSource() { gdxsv_custom_texture_cells_remove_source(this); }

GdxsvTexturePackSource::GdxsvTexturePackSource() {
	gdxsv_custom_texture_cells_add_source(this, true);
}

GdxsvTexturePackSource::~GdxsvTexturePackSource() {
	gdxsv_custom_texture_cells_remove_source(this);
	if (texp_zip != nullptr) zip_close(texp_zip);
	texp_zip = nullptr;
}

void GdxsvTexturePackSource::preloadTextures(TextureCallback callback, std::atomic<bool> *stop_flag) {
	for (const auto& [hash, path] : texture_map)
	{
		if (stop_flag != nullptr && *stop_flag)
			return;
		int w, h;
		u8* data = loadCustomTexture(hash, w, h);
		if (data != nullptr)
		{
			size_t size = (size_t)w * h * 4;
			TextureData tex;
			tex.w = w;
			tex.h = h;
			tex.data.resize(size);
			memcpy(tex.data.data(), data, size);
			stbi_image_free(data);
			callback(hash, std::move(tex));
		}
	}
}

void GdxsvTexturePackSource::terminate() {
	if (texp_zip != nullptr) zip_close(texp_zip);
	texture_map.clear();
	font.Close();
	texp_zip = nullptr;
}

bool GdxsvTexturePackSource::loadMap() {
	if (texp_zip != nullptr) zip_close(texp_zip);
	texp_zip = nullptr;

	std::map<u32, int> mapping;

	// Zipped texture pack
	const auto texture_pack_path = get_writable_data_path(get_game_id() + ".texp");
	if (config::GdxUseTexturePack && file_exists(texture_pack_path)) do {
			auto zip_file = nowide::fopen(texture_pack_path.c_str(), "rb");
			if (zip_file == nullptr) {
				ERROR_LOG(COMMON, "LoadMap: fopen zip_path failure");
				break;
			}

			zip_error_t error;
			zip_source_t* source = zip_source_filep_create(zip_file, 0, -1, &error);
			if (source == nullptr) {
				ERROR_LOG(COMMON, "LoadMap: zip_source_filep_create failure: %s", error.str);
				std::fclose(zip_file);
				break;
			}
			// From here the source owns zip_file, and the zip owns the source once opened.

			const auto zip = zip_open_from_source(source, ZIP_RDONLY, &error);
			if (zip == nullptr) {
				ERROR_LOG(COMMON, "LoadMap: zip_open_from_source failure: %s", error.str);
				zip_source_free(source);
				break;
			}

#ifdef GDXSV_TEXTURE_PACK_PASS
			zip_set_default_password(zip, GDXSV_TEXTURE_PACK_PASS);
#endif

			const auto num_entries = zip_get_num_entries(zip, 0);
			if (num_entries < 0) {
				ERROR_LOG(COMMON, "LoadMap: zip_get_num_entries failure");
				zip_discard(zip);
				break;
			}

			this->texp_zip = zip;

			for (int i = 0; i < num_entries; i++) {
				const char* entry_name = zip_get_name(zip, i, 0);
				std::string name(entry_name ? entry_name : "");
				if (name.empty()) {
					ERROR_LOG(COMMON, "LoadMap: zip_get_name");
					break;
				}

				std::string extension = get_file_extension(name);
				if (extension != "jpg" && extension != "jpeg" && extension != "png") continue;
				std::string::size_type begin_pos = 0;
				std::string::size_type slash_pos = name.find_last_of('/');
				if (slash_pos != std::string::npos) begin_pos = slash_pos + 1;
				std::string::size_type dot_pos = name.find_first_of('.', begin_pos);
				std::string basename = name.substr(begin_pos, dot_pos - begin_pos);
				char* endptr;
				u32 hash = (u32)strtoll(basename.c_str(), &endptr, 16);
				if (endptr - basename.c_str() < (ptrdiff_t)basename.length()) {
					INFO_LOG(COMMON, "Invalid hash %s", basename.c_str());
					continue;
				}

				GdxsvLanguage::Lang lang = GdxsvLanguage::Lang::Disabled;
				if (name.find("English/") != std::string::npos) lang = GdxsvLanguage::Lang::English;
				if (name.find("Japanese/") != std::string::npos) lang = GdxsvLanguage::Lang::Japanese;
				if (name.find("Cantonese/") != std::string::npos) lang = GdxsvLanguage::Lang::Cantonese;

				if (mapping.find(hash) == mapping.end()) {
					if (lang == GdxsvLanguage::Lang::Disabled || lang == GdxsvLanguage::Language()) {
						mapping[hash] = i;
					}
				} else {
					if (lang == GdxsvLanguage::Language()) {
						mapping[hash] = i;
					}
				}
			}
		} while (false);

	texture_map = mapping;
	return !texture_map.empty();
}

u8* GdxsvTexturePackSource::loadCustomTexture(u32 hash, int& width, int& height) {
	const auto it = texture_map.find(hash);
	if (it == texture_map.end()) return config::GdxFontRaster ? font.Render(hash, width, height) : nullptr;

	// Load texture from texture pack file
	zip_stat_t z_stat;
	if (zip_stat_index(texp_zip, it->second, 0, &z_stat) < 0) {
		ERROR_LOG(COMMON, "LoadCustomTexture: zip_stat_index failure");
		return nullptr;
	}

	auto zfp = zip_fopen_index(texp_zip, it->second, 0);
	if (zfp == nullptr) {
		ERROR_LOG(COMMON, "LoadCustomTexture: zip_fopen_index failure");
	} else {
		auto mem = std::malloc(z_stat.size);
		if (zip_fread(zfp, mem, z_stat.size) < 0) {
			ERROR_LOG(COMMON, "LoadCustomTexture: zip_fread failure");
			zip_fclose(zfp);
			std::free(mem);
			return nullptr;
		}
		stbi_set_flip_vertically_on_load(1);
		int n;
		u8* imgData = stbi_load_from_memory((stbi_uc*)mem, z_stat.size, &width, &height, &n, STBI_rgb_alpha);
		zip_fclose(zfp);
		std::free(mem);
		return imgData;
	}

	return nullptr;
}

bool GdxsvTexturePackSource::isTextureReplaced(u32 hash) {
	return texture_map.find(hash) != texture_map.end() || (config::GdxFontRaster && font.HasGlyph(hash));
}
