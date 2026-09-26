// Host/guest orchestration for 4-player replay: loading the replay, spawning
// the guests and the start barrier. The transport is in gdxsv_multi_pov.cpp.
#include <algorithm>
#include <iterator>
#include <cstdio>
#include <string>
#include <vector>

#include "cfg/cfg.h"
#include "gdxsv.pb.h"
#include "gdxsv_multi_pov.h"
#include "gdxsv_multi_pov_window.h"
#include "log/LogManager.h"
#include <nowide/cstdio.hpp>

#include "oslib/http_client.h"
#include "oslib/oslib.h"
#include "stdclass.h"
#include "types.h"

// How long a screen waits at the start barrier. A guest has a whole process
// start ahead of it, so this has to cover that.
constexpr int kStartBarrierMs = 180 * 1000;

// How long a guest waits for the host's replay payload.
constexpr int kReplayFetchMs = 30 * 1000;

// Config keys the host passes to a guest on the command line (transient, never
// written to the config file).
constexpr char kSessionKey[] = "MultiPovSession";
constexpr char kScreenKey[] = "MultiPovScreen";

// Host only: how many guests the barrier should expect.
static int g_spawned_guests = 0;

static std::string SessionId() { return config::loadStr("gdxsv", kSessionKey, ""); }

static int ScreenArg() { return config::loadInt("gdxsv", kScreenKey, 0); }

static bool ReadLocalFile(const std::string& path, std::vector<uint8_t>& out) {
	FILE* fp = nowide::fopen(path.c_str(), "rb");
	if (fp == nullptr) {
		WARN_LOG(COMMON, "multi-pov: cannot open replay %s", path.c_str());
		return false;
	}
	std::fseek(fp, 0, SEEK_END);
	const long size = std::ftell(fp);
	std::fseek(fp, 0, SEEK_SET);
	if (size <= 0) {
		std::fclose(fp);
		WARN_LOG(COMMON, "multi-pov: empty replay %s", path.c_str());
		return false;
	}
	out.resize(static_cast<size_t>(size));
	const size_t read = std::fread(out.data(), 1, out.size(), fp);
	std::fclose(fp);
	if (read != out.size()) {
		WARN_LOG(COMMON, "multi-pov: short read on replay %s", path.c_str());
		return false;
	}
	return true;
}

static bool LoadReplaySource(const std::string& source, std::vector<uint8_t>& out) {
	if (source.compare(0, 4, "http") == 0) {
		http::init();
		std::string content_type;
		std::vector<u8> downloaded;
		const int rc = http::get(source, downloaded, content_type);
		if (rc != 200) {
			WARN_LOG(COMMON, "multi-pov: replay download failed rc=%d %s", rc, source.c_str());
			return false;
		}
		out.assign(downloaded.begin(), downloaded.end());
		return true;
	}
	return ReadLocalFile(source, out);
}

static bool IsFourPlayerBattle(const std::vector<uint8_t>& replay) {
	proto::BattleLogFile log;
	if (!log.ParseFromArray(replay.data(), static_cast<int>(replay.size()))) {
		WARN_LOG(COMMON, "multi-pov: replay does not parse");
		return false;
	}
	if (log.users_size() != kGdxsvMultiPovScreens) {
		NOTICE_LOG(COMMON, "multi-pov: battle has %d players, not %d - using one screen", log.users_size(), kGdxsvMultiPovScreens);
		return false;
	}
	return true;
}

// Where the grid starts, from the saved window geometry. The window layer
// re-lays it out from the host's real rect once it is up.
static GdxsvMultiPovRect InitialGroupRect() {
	GdxsvMultiPovRect group;
	group.w = std::max(320, config::loadInt("window", "width", 1280)) * 2;
	group.h = std::max(240, config::loadInt("window", "height", 720)) * 2;
	group.x = config::loadInt("window", "left", 0);
	group.y = config::loadInt("window", "top", 0);
	return group;
}

static void SpawnGuest(int screen, const std::string& session_id, const GdxsvMultiPovRect& quadrant) {
	const std::string session = std::string("gdxsv:") + kSessionKey + "=" + session_id;
	const std::string screen_arg = std::string("gdxsv:") + kScreenKey + "=" + std::to_string(screen);
	// GdxsvSpectateSync keeps the four on the same frame; the session id names the group.
	const std::string sync_group = "gdxsv:SpectateSyncGroup=" + session_id;
	const std::string pov = "gdxsv:ReplayPOV=" + std::to_string(screen + 1);
	const std::string left = "window:left=" + std::to_string(quadrant.x);
	const std::string top = "window:top=" + std::to_string(quadrant.y);
	const std::string width = "window:width=" + std::to_string(quadrant.w);
	const std::string height = "window:height=" + std::to_string(quadrant.h);
	// A guest is a quadrant whatever the user's own window was last time.
	const std::string maximized = "window:maximized=no";
	const std::string fullscreen = "window:fullscreen=no";
	const std::string content = settings.content.path;

	const char* args[] = {
		"-config", session.c_str(),
		"-config", screen_arg.c_str(),
		"-config", sync_group.c_str(),
		"-config", pov.c_str(),
		"-config", left.c_str(),
		"-config", top.c_str(),
		"-config", width.c_str(),
		"-config", height.c_str(),
		"-config", maximized.c_str(),
		"-config", fullscreen.c_str(),
		content.c_str(),
#ifdef __APPLE__
		// After a crash AppKit asks whether to reopen windows, and a guest
		// would sit in that alert. Must come after the content path.
		"-ApplePersistenceIgnoreState", "YES",
#endif
	};
	NOTICE_LOG(COMMON, "multi-pov: spawning %dP at %d,%d %dx%d", screen + 1, quadrant.x, quadrant.y, quadrant.w, quadrant.h);
	os_RunInstance(static_cast<int>(std::size(args)), args);
}

void gdxsv_multi_pov_compute_grid(const GdxsvMultiPovRect& group, GdxsvMultiPovRect out[kGdxsvMultiPovScreens]) {
	const int32_t left_w = group.w / 2;
	const int32_t top_h = group.h / 2;
	const int32_t right_w = group.w - left_w;
	const int32_t bottom_h = group.h - top_h;

	out[0] = {group.x, group.y, left_w, top_h};
	out[1] = {group.x + left_w, group.y, right_w, top_h};
	out[2] = {group.x, group.y + top_h, left_w, bottom_h};
	out[3] = {group.x + left_w, group.y + top_h, right_w, bottom_h};
}

bool gdxsv_multi_pov_take_four_screen_request() {
	if (!config::isTransient("gdxsv", "ReplayFourScreen") ||
		!config::loadBool("gdxsv", "ReplayFourScreen", false))
		return false;
	// Consume before trying to host: later slot-99 loads (including browser
	// button clicks) must not launch this command-line request again.
	config::setTransient("gdxsv", "ReplayFourScreen", "no");
	return ScreenArg() == 0;
}

int gdxsv_multi_pov_guest_pov() {
	const int screen = ScreenArg();
	if (screen < 1 || kGdxsvMultiPovScreens <= screen) return -1;
	return screen;
}

std::string gdxsv_multi_pov_log_file_name() {
	const int screen = gdxsv_multi_pov_guest_pov();
	if (screen < 0) return "flycast.log";
	return "flycast-" + std::to_string(screen + 1) + "P.log";
}

bool gdxsv_multi_pov_begin_host_session(const std::string& replay_source, std::vector<uint8_t>& replay_out) {
	g_spawned_guests = 0;

	if (!gdxsv_multi_pov_window_available()) {
		WARN_LOG(COMMON, "multi-pov: four-screen playback is unavailable without a supported desktop window");
		return false;
	}

	if (!LoadReplaySource(replay_source, replay_out)) return false;
	if (!IsFourPlayerBattle(replay_out)) return false;

	const std::string session_id = gdxsv_multi_pov_new_session_id();
	if (!gdxsv_multi_pov_host_create(session_id, replay_out)) return false;

	// The host joins the same frame-sync group as its guests.
	config::setTransient("gdxsv", "SpectateSyncGroup", session_id);

	GdxsvMultiPovRect quadrants[kGdxsvMultiPovScreens];
	gdxsv_multi_pov_compute_grid(InitialGroupRect(), quadrants);

	for (int screen = 1; screen < kGdxsvMultiPovScreens; ++screen) {
		SpawnGuest(screen, session_id, quadrants[screen]);
		++g_spawned_guests;
	}
	NOTICE_LOG(COMMON, "multi-pov: host session %s with %d guests", session_id.c_str(), g_spawned_guests);
	return true;
}

bool gdxsv_multi_pov_begin_guest_session(std::vector<uint8_t>& replay_out) {
	const int screen = gdxsv_multi_pov_guest_pov();
	if (screen < 0) return false;

	if (!gdxsv_multi_pov_guest_open(SessionId(), screen)) return false;
	if (!gdxsv_multi_pov_fetch_replay(replay_out, kReplayFetchMs)) {
		gdxsv_multi_pov_close();
		return false;
	}
	return true;
}

void gdxsv_multi_pov_wait_at_start_barrier() {
	switch (gdxsv_multi_pov_current_role()) {
		case GdxsvMultiPovRole::Host:
			gdxsv_multi_pov_host_wait_for_guests(g_spawned_guests, kStartBarrierMs);
			break;
		case GdxsvMultiPovRole::Guest:
			gdxsv_multi_pov_guest_ready_and_wait(kStartBarrierMs);
			break;
		case GdxsvMultiPovRole::None:
			break;
	}
}
