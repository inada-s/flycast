#include "gdxsv_custom_texture_update.h"
#include "gdxsv_config.h"
#include "gdxsv_update.h"

#include <locale>
#include <sstream>
#include <xxhash.h>
#include "json.hpp"
#include "libs.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "oslib/http_client.h"

static constexpr size_t MaxDownloadSize = 30 * 1024 * 1024;

int gdxsv_texture_pack_resolution(GdxsvTexturePackLayer layer) {
	const int resolution = layer == GdxsvTexturePackLayer::TwoD ? config::GdxTextureResolution2D : config::GdxTextureResolution3D;
	return 4 <= resolution ? 4 : 2 <= resolution ? 2 : 1;
}

std::string gdxsv_texture_pack_name(GdxsvTexturePackLayer layer) {
	if (layer == GdxsvTexturePackLayer::Ex) return "ex";
	const int resolution = gdxsv_texture_pack_resolution(layer);
	if (layer == GdxsvTexturePackLayer::ThreeD && resolution == 1) return "";
	return std::string(layer == GdxsvTexturePackLayer::TwoD ? "2d-" : "3d-") + std::to_string(resolution) + "x";
}

std::string gdxsv_texture_pack_path(GdxsvTexturePackLayer layer) {
	const char* name = layer == GdxsvTexturePackLayer::Ex ? "ex" : layer == GdxsvTexturePackLayer::TwoD ? "2d" : "3d";
	return get_writable_data_path(std::string("T13306M-") + name + ".texp");
}

// gdxsv:TexturePackUrl: tests serve the pack locally.
static std::string base_url() {
	return config::loadStr("gdxsv", "TexturePackUrl", "https://storage.googleapis.com/gdxsv/custom-texture/");
}

void GdxsvCustomTexutreUpdate::Reset() {
	fetch_latest_version_future_ = {};
	update_started_ = false;
	download_buf_.clear();
	download_buf_.shrink_to_fit();
}

bool GdxsvCustomTexutreUpdate::IsUpdateAvailable() {
	if (update_started_) {
		return false;
	}

	if (!fetch_latest_version_future_.valid()) {
		FetchLatestVersionInfo();
		return false;
	}

	if (!future_is_ready(fetch_latest_version_future_)) {
		return false;
	}

	return !fetch_latest_version_future_.get().empty();
}

void GdxsvCustomTexutreUpdate::FetchLatestVersionInfo() {
	if (fetch_latest_version_future_.valid()) {
		return;
	}

	const auto future_fn = []() -> std::vector<LatestVersionInfo> {
		std::vector<LatestVersionInfo> updates;
		http::init();
		const std::string channel = config::GdxTexturePackChannel.get().empty() ? "release" : config::GdxTexturePackChannel.get();

		for (const auto layer : {GdxsvTexturePackLayer::Ex, GdxsvTexturePackLayer::TwoD, GdxsvTexturePackLayer::ThreeD}) {
			const std::string pack = gdxsv_texture_pack_name(layer);
			if (pack.empty()) continue;

			LatestVersionInfo latest{};
			latest.channel_file = channel + "-" + pack + ".json";
			latest.install_path = gdxsv_texture_pack_path(layer);

			std::vector<u8> dl;
			std::string content_type;
			const auto t = std::chrono::steady_clock::now().time_since_epoch().count();
			const std::string url = base_url() + latest.channel_file + "?t=" + std::to_string(t);
			const int rc = http::get(url, dl, content_type);
			if (rc != 200) {
				ERROR_LOG(COMMON, "version check failure: %s", url.c_str());
				continue;
			}

			const bool is_new_version = HandleReleaseJSON(std::string(dl.begin(), dl.end()), latest);
			NOTICE_LOG(COMMON, "texture pack check: %s, new %d", latest.channel_file.c_str(), is_new_version);
			if (is_new_version) updates.push_back(latest);
		}
		return updates;
	};

	fetch_latest_version_future_ = std::async(std::launch::async, future_fn).share();
}

std::shared_future<bool> GdxsvCustomTexutreUpdate::StartUpdate() {
	download_completed_size_ = 0;
	update_started_ = true;
	verify(future_is_ready(fetch_latest_version_future_));

	auto update_fn = [this]() -> bool {
		http::init();
		for (const auto& latest : fetch_latest_version_future_.get()) {
			if (!Download(latest)) return false;
		}
		return true;
	};

	return std::async(std::launch::async, update_fn).share();
}

bool GdxsvCustomTexutreUpdate::Download(const LatestVersionInfo& latest) {
	const auto download_file_path = latest.install_path + ".part";
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

	if (ok) {
		auto new_xxhash = ComputeXXHash(download_file_path);
		if (new_xxhash != latest.xxhash) {
			ERROR_LOG(COMMON, "invalid xxhash expected:%s actual%s", latest.xxhash.c_str(), new_xxhash.c_str());
			ok = false;
		}
	}
	if (!ok) {
		nowide::remove(download_file_path.c_str());
		return false;
	}

	nowide::remove(latest.install_path.c_str());
	if (!gdxsv_rename_file(download_file_path, latest.install_path)) {
		ERROR_LOG(COMMON, "failed to move latest version");
		return false;
	}

	NOTICE_LOG(COMMON, "texture pack updated to %s: %s", latest.version.c_str(), latest.channel_file.c_str());
	return true;
}

float GdxsvCustomTexutreUpdate::UpdateProgress() const
{
	verify(future_is_ready(fetch_latest_version_future_));

	int total_size = 0;
	for (const auto& latest : fetch_latest_version_future_.get()) {
		total_size += latest.size;
	}

	if (total_size == 0) {
		return 0;
	}

	return static_cast<float>(download_completed_size_ + download_buf_.size()) / total_size;
}

bool GdxsvCustomTexutreUpdate::HandleReleaseJSON(const std::string& json_string, LatestVersionInfo& out) {
	std::string latest_version;
	std::string latest_xxhash;
	int latest_size = 0;
	std::vector<std::tuple<std::string, int, std::string>> latest_chunks;

	try {
		nlohmann::json v = nlohmann::json::parse(json_string);
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

	if (latest_version.empty()) return false;
	if (latest_xxhash.empty()) return false;
	if (latest_chunks.empty()) return false;

	int sum_size = 0;
	for (auto&e : latest_chunks) {
		sum_size += std::get<1>(e);
	}
	if (sum_size != latest_size) return false;

	auto current_xxhash = ComputeXXHash(out.install_path);
	if (!current_xxhash.empty() && current_xxhash == latest_xxhash) return false;

	out.version = latest_version;
	out.xxhash = latest_xxhash;
	out.size = latest_size;
	out.chunks = latest_chunks;
	return true;
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

GdxsvCustomTexutreUpdate gdxsv_custom_texture_update;
