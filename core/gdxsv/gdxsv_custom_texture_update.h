#pragma once
#include <future>
#include <string>
#include <vector>

class GdxsvCustomTexutreUpdate {
   public:
	void Reset();
	bool IsUpdateAvailable();
	void FetchLatestVersionInfo();
	std::string GetLatestVersion() const;
	std::shared_future<bool> StartUpdate();
	float UpdateProgress() const;

   private:
	struct LatestVersionInfo {
		bool is_new_version = false;
		std::string name;
		std::string version;
		std::string xxhash;
		int size;
		std::vector<std::tuple<std::string, int, std::string>> chunks;
	};

	static void HandleReleaseJSON(const std::string& json_string, LatestVersionInfo& out);
	static std::string ComputeXXHash(std::string path);
	static std::string GetTempDir();

	std::shared_future<LatestVersionInfo> fetch_latest_version_future_;
	std::vector<uint8_t> download_buf_;
	int download_completed_size_ = 0;
	bool update_started_ = false;
};

extern GdxsvCustomTexutreUpdate gdxsv_custom_texture_update;
