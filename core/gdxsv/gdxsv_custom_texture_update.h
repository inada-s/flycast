#pragma once
#include <future>
#include <string>
#include <tuple>
#include <vector>

// The downloaded texture pack comes in layers, each installed on its own (docs/gdxsv/features.md). The ex layer is
// always on and has no setting.
enum class GdxsvTexturePackLayer { Ex, TwoD, ThreeD };

// The size a 2D or 3D layer is used at: 1, 2 or 4. 2D at 1 holds only the language images at the original size; 3D at
// 1 is off.
int gdxsv_texture_pack_resolution(GdxsvTexturePackLayer layer);
// The pack a layer wants ("ex", "2d-1x", "3d-4x"...), empty when the layer is off.
std::string gdxsv_texture_pack_name(GdxsvTexturePackLayer layer);
// Where a layer's pack is installed.
std::string gdxsv_texture_pack_path(GdxsvTexturePackLayer layer);

class GdxsvCustomTexutreUpdate {
   public:
	void Reset();
	bool IsUpdateAvailable();
	void FetchLatestVersionInfo();
	std::shared_future<bool> StartUpdate();
	float UpdateProgress() const;

   private:
	struct LatestVersionInfo {
		std::string channel_file;
		std::string install_path;
		std::string version;
		std::string xxhash;
		int size;
		std::vector<std::tuple<std::string, int, std::string>> chunks;
	};

	static bool HandleReleaseJSON(const std::string& json_string, LatestVersionInfo& out);
	static std::string ComputeXXHash(std::string path);
	bool Download(const LatestVersionInfo& latest);

	std::shared_future<std::vector<LatestVersionInfo>> fetch_latest_version_future_;
	std::vector<uint8_t> download_buf_;
	int download_completed_size_ = 0;
	bool update_started_ = false;
};

extern GdxsvCustomTexutreUpdate gdxsv_custom_texture_update;
