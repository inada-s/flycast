#include "gdxsv_custom_texture_update.h"
#include "gdxsv_config.h"
#include "gdxsv_custom_texture_source.h"
#include "gdxsv_update.h"

#include <locale>
#include <sstream>
#include <xxhash.h>
#include "json.hpp"
#include "libs.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "oslib/http_client.h"

#if defined(_WIN32)
#include <Windows.h>
#include <nowide/stackstring.hpp>
#endif

static constexpr size_t MaxDownloadSize = 30 * 1024 * 1024;

// gdxsv:TexturePackUrl: tests serve the pack locally.
static std::string base_url() {
	return config::loadStr("gdxsv", "TexturePackUrl", "https://storage.googleapis.com/gdxsv/custom-texture/");
}

static std::string channel_file_name() {
	std::string name = config::GdxTexturePackChannel.get().empty() ? "release" : config::GdxTexturePackChannel.get();
	if (config::GdxTextureResolution == 1) {
		return name + "-localized.json";
	}
	return name + (config::GdxTextureResolution == 4 ? "-4x.json" : "-2x.json");
}

std::string get_texture_pack_path() {
	return get_writable_data_path("T13306M.texp");
}

void GdxsvCustomTexutreUpdate::Reset() {
	fetch_latest_version_future_ = {};
	download_buf_.clear();
	download_buf_.shrink_to_fit();
}

bool GdxsvCustomTexutreUpdate::IsUpdateAvailable() {
	if (!gdxsv_texture_pack_wanted()) {
		return false;
	}

	if (!fetch_latest_version_future_.valid()) {
		FetchLatestVersionInfo();
		return false;
	}

	if (!future_is_ready(fetch_latest_version_future_)) {
		return false;
	}

	const auto& v = fetch_latest_version_future_.get();
	return v.is_new_version && !v.chunks.empty();
}

void GdxsvCustomTexutreUpdate::FetchLatestVersionInfo() {
	if (fetch_latest_version_future_.valid()) {
		return;
	}

	const auto future_fn = [this]() -> LatestVersionInfo {
		LatestVersionInfo latest{};
		if (!gdxsv_texture_pack_wanted()) {
			return latest;
		}

		std::vector<u8> dl;
		std::string content_type;
		http::init();
		const std::string name = channel_file_name();

		const auto t = std::chrono::steady_clock::now().time_since_epoch().count();
		const std::string url = base_url() + name + "?t=" + std::to_string(t);
		const int rc = http::get(url, dl, content_type);
		if (rc != 200) {
			ERROR_LOG(COMMON, "version check failure: %s", url.c_str());
			return latest;
		}

		HandleReleaseJSON(std::string(dl.begin(), dl.end()), latest);
		NOTICE_LOG(COMMON, "texture pack check: %s, new %d", name.c_str(), latest.is_new_version);
		return latest;
	};

	fetch_latest_version_future_ = std::async(std::launch::async, future_fn).share();
}

std::string GdxsvCustomTexutreUpdate::GetLatestVersion() const {
	verify(future_is_ready(fetch_latest_version_future_));
	const auto& v = fetch_latest_version_future_.get();
	return v.version;
}

std::shared_future<bool> GdxsvCustomTexutreUpdate::StartUpdate() {
	download_completed_size_ = 0;
	verify(future_is_ready(fetch_latest_version_future_));

	auto update_fn = [this]() -> bool {
		const auto& latest = fetch_latest_version_future_.get();
		http::init();

		const auto tmp_dir = GetTempDir();
		if (tmp_dir.empty()) {
			ERROR_LOG(COMMON, "GetTempDir failure");
			return false;
		}

		const auto download_file_path = tmp_dir + "/" + latest.name;
		auto fp = nowide::fopen(download_file_path.c_str(), "wb");
		if (fp == nullptr) {
			ERROR_LOG(COMMON, "fopen failure: %s", download_file_path.c_str());
			return false;
		}

		bool ok = true;
		for (auto [path, size, hash] : latest.chunks) {
			if (MaxDownloadSize < size) {
				ERROR_LOG(COMMON, "latest texture pack is too big");
				ok = false;
				break;
			}

			std::string content_type;
			download_buf_.clear();
			download_buf_.reserve(size);

			const std::string url = base_url() + path;
			const int rc = http::get(url, download_buf_, content_type);

			if (rc != 200) {
				ERROR_LOG(COMMON, "download failure: %s", url.c_str());
				ok = false;
				break;
			}

			if (download_buf_.size() != size) {
				ERROR_LOG(COMMON, "invalid size e:%ld a:%ld", size, download_buf_.size());
				ok = false;
				break;
			}

			if (!file_exists(tmp_dir) && !make_directory(tmp_dir)) {
				ERROR_LOG(COMMON, "cannot access tmp_dir");
				ok = false;
				break;
			}

			const auto written = std::fwrite(download_buf_.data(), 1, download_buf_.size(), fp);
			if (written != download_buf_.size()) {
				ERROR_LOG(COMMON, "fwrite failure");
				ok = false;
				break;
			}

			{
				std::stringstream ss;
				ss.imbue(std::locale::classic());
				ss.width(16);
				ss.fill('0');
				ss << std::hex << XXH64(download_buf_.data(), download_buf_.size(), 0);
				if (hash != ss.str()) {
					ERROR_LOG(COMMON, "chunk hash missmatch");
					ok = false;
					break;
				}
			}

			download_completed_size_ += download_buf_.size();
			download_buf_.clear();
		}

		std::fclose(fp);
		download_buf_.clear();
		download_buf_.shrink_to_fit();

		if (!ok) {
			return false;
		}

		auto new_xxhash = ComputeXXHash(download_file_path);
		if (new_xxhash != latest.xxhash)
		{
			ERROR_LOG(COMMON, "invalid xxhash expected:%s actual%s", latest.xxhash.c_str(), new_xxhash.c_str());
			return false;
		}

		nowide::remove(get_texture_pack_path().c_str());
		if (!gdxsv_rename_file(download_file_path, get_texture_pack_path())) {
			ERROR_LOG(COMMON, "failed to move latest version");
			return false;
		}

		NOTICE_LOG(COMMON, "texture pack updated to %s", latest.version.c_str());
		return true;
	};

	return std::async(std::launch::async, update_fn).share();
}

float GdxsvCustomTexutreUpdate::UpdateProgress() const
{
	verify(future_is_ready(fetch_latest_version_future_));

	const auto& latest = fetch_latest_version_future_.get();

	if (latest.size == 0) {
		return 0;
	}

	return static_cast<float>(download_completed_size_ + download_buf_.size()) / latest.size;
}

void GdxsvCustomTexutreUpdate::HandleReleaseJSON(const std::string& json_string, LatestVersionInfo& out) {
	std::string latest_name;
	std::string latest_version;
	std::string latest_xxhash;
	int latest_size = 0;
	std::vector<std::tuple<std::string, int, std::string>> latest_chunks;

	try {
		nlohmann::json v = nlohmann::json::parse(json_string);
		latest_name = v.at("name");
		latest_version = v.at("version");
		latest_xxhash = v.at("xxhash");
		latest_size = v.at("size");
		for (auto e : v.at("chunks")) {
			std::string path = e.at("path");
			int size = e.at("size");
			std::string hash = e.at("hash");
			latest_chunks.emplace_back(path, size, hash);
		}
	} catch (const nlohmann::json::exception& e) {
		WARN_LOG(COMMON, "json parse failure: %s", e.what());
	}

	if (latest_name.empty()) return;
	if (latest_version.empty()) return;
	if (latest_xxhash.empty()) return;
	if (latest_chunks.empty()) return;

	int sum_size = 0;
	for (auto&e : latest_chunks) {
		sum_size += std::get<1>(e);
	}
	if (sum_size != latest_size) return;

	auto current_xxhash = ComputeXXHash(get_texture_pack_path());
	if (!current_xxhash.empty() && current_xxhash == latest_xxhash) return;

	out.name = latest_name;
	out.version = latest_version;
	out.xxhash = latest_xxhash;
	out.size = latest_size;
	out.chunks = latest_chunks;
	out.is_new_version = true;
}

std::string GdxsvCustomTexutreUpdate::ComputeXXHash(std::string path) {
	if (!file_exists(path))
		return "";

	XXH64_state_t* state = XXH64_createState();
	if (state == nullptr)
		return "";

	if (XXH64_reset(state, 0) == XXH_ERROR) {
		XXH64_freeState(state);
		return "";
	}

	const auto fp = nowide::fopen(path.c_str(), "rb");
	if (fp == nullptr) {
		XXH64_freeState(state);
		return "";
	}

	std::string result;
	bool ok = true;
	int n;
	do {
		char buf[4096];
		n = std::fread(buf, 1, 4096, fp);
		if (0 < n) {
			ok &= XXH64_update(state, buf, n) != XXH_ERROR;
		}
	} while (n && ok);

	if (ok) {
		std::stringstream ss;
		ss.imbue(std::locale::classic());
		ss.width(16);
		ss.fill('0');
		ss << std::hex << XXH64_digest(state);
		result = ss.str();
	}

	XXH64_freeState(state);
	std::fclose(fp);
	return result;
}

std::string GdxsvCustomTexutreUpdate::GetTempDir() {
#if defined(_WIN32)
	wchar_t temp_path[_MAX_PATH + 1];
	GetTempPathW(_MAX_PATH, temp_path);
	nowide::stackstring nws;
	if (!nws.convert(temp_path)) return "";
	auto result = std::string(nws.get());
	if (result.back() == '/' || result.back() == '\\') result.pop_back();
	return result;
#else
	return "/tmp";
#endif
}

GdxsvCustomTexutreUpdate gdxsv_custom_texture_update;

