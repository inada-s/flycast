#include "gdxsv_replay_util.h"
#include "gdxsv_translation.h"

#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef _WIN32
#define stat _stat
#define WIN32_LEAN_AND_MEAN
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif

#include "dirent.h"
#include "gdxsv.h"
#include "gdxsv_multi_pov.h"
#include "gdxsv_multi_pov_window.h"
#include "json.hpp"
#include "libs.h"
#ifdef _WIN32
#undef stat  // Undefine the macro before including directory.h to use flycast::stat()
#endif
#include "oslib/directory.h"
#include "oslib/oslib.h"
#include "oslib/http_client.h"
#include "ui/gui_util.h"
#include "ui/IconsFontAwesome6.h"
#include "stdclass.h"

#include <sstream>
#include <locale>
#include <future>
#include <thread>
#include <algorithm>
#include <chrono>
#include <map>

// For macOS
std::string os_PrecomposedString(std::string string);

namespace {

static const char* ms_names(int id) {
	return GdxsvLanguage::GetMSName(id);
}

std::vector<int> parse_csv_ints(const std::string& s) {
	std::vector<int> result;
	if (s.empty()) return result;
	std::istringstream iss(s);
	std::string token;
	while (std::getline(iss, token, ',')) {
		result.push_back(std::atoi(token.c_str()));
	}
	return result;
}

struct UserEntry {
	std::string user_id;
	std::string name;
	int battle_count;
	int win_count;
	int lose_count;
	int kill_count;
	int renpo_battle_count;
	int renpo_win_count;
	int renpo_lose_count;
	int renpo_kill_count;
	int zeon_battle_count;
	int zeon_win_count;
	int zeon_lose_count;
	int zeon_kill_count;
	int daily_battle_count;
	int daily_win_count;
	int daily_lose_count;
};

struct ReplayEntry {
	std::string disk;
	std::vector<proto::BattleLogUser> users;
	int round = 0;
	int renpo_win = 0;
	int zeon_win = 0;
	time_t start_unix = 0;
	std::string replay_url;
	std::string round_win;
	std::vector<std::string> user_used_ms_list;
	int play_count = -1; // Not present in local recordings.
	std::string filename;
	std::string battle_code;
	time_t end_unix = 0;
	std::string close_reason;
	bool readable = true;
};

constexpr size_t kLocalReplayPageSize = 100;
struct LocalReplayPage {
	std::vector<ReplayEntry> entries;
	size_t page = 0;
	size_t page_count = 1;
};

std::shared_future<LocalReplayPage> local_replays_future;
size_t local_replay_page = 0;
std::string selected_replay_file;
std::string broken_replay_path;

std::vector<std::string> search_user_ids;
std::vector<std::string> search_user_names;
std::vector<std::string> search_pilot_names;
std::string search_lobby_id;
unsigned int search_no_of_players;
std::string search_battle_code;
int search_ranking = -1;
std::string search_disk;
bool search_reverse;
int search_used_ms = -1;

bool select_server_tab = false;

void fetch_new_results(bool reset_page = true);

// Matches maxReplayPlayerFilters in the server's replay API.
constexpr size_t kMaxPlayerFilters = 4;

const char* player_filter_disabled_hint(const std::vector<std::string>& filters, const std::string& value) {
	if (value.empty())
		return "Empty values cannot be used as filters.";
	if (std::find(filters.begin(), filters.end(), value) != filters.end())
		return "This filter is already applied.";
	if (filters.size() >= kMaxPlayerFilters)
		return "Up to four filters per field. Remove one to add another.";
	return nullptr;
}

bool add_player_filter(std::vector<std::string>& filters, const std::string& value) {
	if (player_filter_disabled_hint(filters, value) != nullptr)
		return false;
	filters.push_back(value);
	return true;
}

int filtered_replay_pov(const std::vector<proto::BattleLogUser>& users) {
	const auto name_matches = [](const std::string& name, const std::vector<std::string>& filters) {
		return std::any_of(filters.begin(), filters.end(), [&](const std::string& filter) {
			return ImStristr(name.c_str(), nullptr, filter.c_str(), nullptr) != nullptr;
		});
	};
	// Different filters may identify different participants. Prefer the first
	// matching slot, using exact IDs and partial names like the filter UI.
	for (size_t i = 0; i < users.size(); ++i) {
		const auto& user = users[i];
		if (std::find(search_user_ids.begin(), search_user_ids.end(), user.user_id()) != search_user_ids.end() ||
			name_matches(user.user_name(), search_user_names) || name_matches(user.pilot_name(), search_pilot_names))
			return static_cast<int>(i);
	}
	return users.empty() ? -1 : 0;
}

std::string replay_api_url(const char* path) {
	return config::loadStr("gdxsv", "ReplayApiUrl", "https://asia-northeast1-gdxsv-274515.cloudfunctions.net/lbsapi") + path;
}

std::shared_future<std::vector<UserEntry>> fetch_user_entry_future_;
int fetch_user_entry_http_status;

std::shared_future<std::vector<ReplayEntry>> fetch_replay_entry_future_;
int fetch_replay_entry_http_status;
bool replay_results_dirty = false;

bool replay_results_ready() {
	return !replay_results_dirty && future_is_ready(fetch_replay_entry_future_);
}

// One in-progress battle from /lbs/status, joining active_games with the
// battle_users that belong to it.
struct LiveEntry {
	std::string battle_code;
	std::string disk;
	std::string state;
	int lobby_id = 0;
	bool live_spectate = false;
	int spectators = 0;
	time_t updated_unix = 0;
	std::vector<proto::BattleLogUser> users;
};

std::shared_future<std::vector<LiveEntry>> fetch_live_entry_future_;
int fetch_live_entry_http_status;
std::chrono::steady_clock::time_point live_last_fetch_;

// The last good result stays on screen while the next fetch is in flight -
// otherwise the list blinks out every refresh. Only the very first load has
// nothing to show.
std::vector<LiveEntry> live_entries_;
bool live_first_load_ = true;

// By battle_code, not by index: battles come and go between refreshes, so an
// index would silently end up pointing at a different battle.
std::string selected_live_battle_code;

// Battles start and end while the tab is open, so the list refreshes itself.
// /lbs/status collapses concurrent requests with singleflight, so this is
// cheap on the server; polling only runs while the tab is actually visible.
constexpr int kLiveRefreshSeconds = 10;
int selected_replay_entry_index = -1;

int entry_paging = 0;
char page_buf[4] = "1";
int pov_index = -1;
ImVec2 normal_padding;
float scaling;

const std::array<std::array<const char*, 2>, 17> lobby_data{{{"Taklamakan Desert", "2"},
															 {"Black Sea Forest", "4"},
															 {"Odessa", "5"},
															 {"Belfast", "6"},
															 {"New York", "9"},
															 {"Grand Canyon", "10"},
															 {"Jaburo", "11"},
															 {"UG Complex", "12"},
															 {"Solomon", "13"},
															 {"Solomon (Space)", "14"},
															 {"A Baoa Qu (Space)", "15"},
															 {"A Baoa Qu (Outter)", "16"},
															 {"A Baoa Qu (Inner)", "17"},
															 {"Sat.Orbit 1", "19"},
															 {"Sat.Orbit 2", "20"},
															 {"SIDE 6 (Space)", "21"},
															 {"SIDE 7 (Inner)", "22"}}};

// active_games carries no start time, only when the record was last touched -
// which for a battle still in progress is when it started.
time_t parse_iso8601(const std::string& v) {
	std::tm tm{};
	int frac = 0;
	char sign = '+';
	int off_h = 0, off_m = 0;
	if (sscanf(v.c_str(), "%d-%d-%dT%d:%d:%d.%d%c%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &frac,
			   &sign, &off_h, &off_m) < 6) {
		return 0;
	}
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	// timegm is POSIX. Windows spells it _mkgmtime, in both MinGW and MSVC.
#ifdef _WIN32
	const time_t utc = _mkgmtime(&tm);
#else
	const time_t utc = timegm(&tm);
#endif
	const time_t offset = (off_h * 3600 + off_m * 60) * (sign == '-' ? -1 : 1);
	return utc - offset;
}

// "dc1"/"dc2" are internal names. Players call the two discs MUJI and DX, which
// is what the Server tab's own disk filter buttons already say.
const char* disk_display_name(const std::string& disk) {
	if (disk == "dc1") return "MUJI";
	if (disk == "dc2") return "DX";
	return disk.c_str();
}

// Translated, so the Live list reads the same as the Server tab's filter.
const char* lobby_name_by_id(int lobby_id) {
	const auto id = std::to_string(lobby_id);
	for (const auto& row : lobby_data) {
		if (id == row[1]) return GdxsvLanguage::gdxT(row[0]);
	}
	return nullptr;
}

void textCentered(const std::string& text) {
	auto windowWidth = ImGui::GetWindowSize().x;
	auto textWidth = ImGui::CalcTextSize(text.c_str()).x;
	ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
	ImGui::Text(text.c_str());
};

void gdxsv_replay_draw_forces(const bool is_renpo, const std::vector<int>& force_index, int& user_index,
							  const std::vector<proto::BattleLogUser>& users, bool server_tab) {
	ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f * scaling);

	if (is_renpo) {
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.42f, .79f, .99f, 1));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.055f, .122f, .227f, .3f));
	} else {
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.97f, .23f, .35f, 1));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.196f, .07f, .05f, .3f));
	}

	for (int i : force_index) {
		if (i != force_index.front()) ImGui::SameLine();
		const auto& user = users[user_index];
		auto pos = ImGui::GetCursorPos();
		if (ImGui::Selectable(("##pov_" + std::to_string(user_index)).c_str(), (pov_index == user_index), 0, ScaledVec2(180, 90))) {
			if (pov_index == user_index) {
				pov_index = -1;
			} else {
				pov_index = user_index;
			}
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
			ImGui::SetTooltip(server_tab ? "Right-click to copy or filter" : "Right-click to copy or filter on Server");
		if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonRight)) {
			auto field_menu = [server_tab](const char* name, const std::string& value, std::vector<std::string>& filters) {
				const std::string label = std::string(name) + ": " + (value.empty() ? "(empty)" : value) + "###" + name;
				if (ImGui::BeginMenu(label.c_str())) {
					if (ImGui::MenuItem("Copy"))
						ImGui::SetClipboardText(value.c_str());
					const std::string filter_label = std::string("Filter by ") + name + (server_tab ? "" : " (Server)");
					const char* disabled_hint = player_filter_disabled_hint(filters, value);
					if (ImGui::MenuItem(filter_label.c_str(), nullptr, false, disabled_hint == nullptr) &&
						add_player_filter(filters, value)) {
						fetch_new_results();
						select_server_tab = true;
					}
					if (disabled_hint != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						ImGui::SetTooltip("%s", disabled_hint);
					ImGui::EndMenu();
				}
			};
			field_menu("ID", user.user_id(), search_user_ids);
			field_menu("HN", user.user_name(), search_user_names);
			field_menu("PN", user.pilot_name(), search_pilot_names);
			ImGui::EndPopup();
		}
		ImGui::SetCursorPos(ImVec2(pos.x, pos.y));
		ImGui::BeginChild(ImGui::GetID(("gdxsv_replay_file_detail_renpo_" + std::to_string(i)).c_str()), ScaledVec2(180, 90), true,
						  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs);
		textCentered("ID: " + user.user_id());
		textCentered("HN: " + user.user_name());
		textCentered("PN: " + user.pilot_name());
		ImGui::EndChild();
		user_index++;
	}
	ImGui::PopStyleColor();
	ImGui::PopStyleColor();

	ImGui::PopStyleVar();
}

void gdxsv_replay_draw_players(const std::vector<proto::BattleLogUser>& users, bool server_tab) {
	std::vector<int> renpo_index, zeon_index;
	for (int i = 0; i < users.size(); i++) {
		if (users[i].team() == 1) renpo_index.push_back(i);
		if (users[i].team() == 2) zeon_index.push_back(i);
	}
	int user_index = 0;

	gdxsv_replay_draw_forces(true, renpo_index, user_index, users, server_tab);
	gdxsv_replay_draw_forces(false, zeon_index, user_index, users, server_tab);
}

void gdxsv_replay_draw_info(const std::string& battle_code, const std::string& game_disk, const int& users_size,
							const std::string& close_reason, const time_t& start_time, const time_t& end_time,
							const std::vector<proto::BattleLogUser>& users, const std::string& replay_dst, bool server_tab,
							int play_count = -1, const std::string& filename = {}) {
	const bool playable = "dc" + std::to_string(gdxsv.Disk()) == game_disk;

	// Player cards + replay actions first
	gdxsv_replay_draw_players(users, server_tab);

	ImGui::NewLine();

	{
		const ImVec2 button_size = ScaledVec2(300, 50);
		auto replay_button = [&](const char* label, int pov, bool four_screen, const char* disabled_hint) {
			std::string button_label = !playable ? std::string("Load ") + disk_display_name(game_disk) + " to replay"
				: disabled_hint != nullptr ? disabled_hint : label;
			// Keep each action's ID stable as the selected player or hint changes.
			button_label += four_screen ? "###replay-four-screen" : "###replay-single-screen";
			ImGui::BeginDisabled(!playable || disabled_hint != nullptr);
			if (ImGui::ButtonEx(button_label.c_str(), button_size))
				gdxsv_start_replay(replay_dst, pov, four_screen);
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				if (!playable)
					ImGui::SetTooltip("Load %s to play this replay.", disk_display_name(game_disk));
				else if (disabled_hint != nullptr)
					ImGui::SetTooltip("%s", disabled_hint);
			}
		};

		const bool pov_selected = 0 <= pov_index && pov_index < users_size;
		char replay_label[64];
		snprintf(replay_label, sizeof(replay_label), ICON_FA_PLAY "  Replay %dP", pov_index + 1);
		replay_button(replay_label, pov_index, false,
			pov_selected ? nullptr : ICON_FA_ARROW_POINTER "  Select a Player");
		// Four-screen playback does not require a selected player.
		const char* four_screen_hint = nullptr;
		if (users_size != kGdxsvMultiPovScreens)
			four_screen_hint = ICON_FA_TABLE_CELLS_LARGE "  4-player battles only";
		else if (!gdxsv_multi_pov_window_available())
			four_screen_hint = ICON_FA_TABLE_CELLS_LARGE "  4 screens unavailable";
		replay_button(ICON_FA_TABLE_CELLS_LARGE "  Replay (4 screens)", 0, true, four_screen_hint);

		if (!broken_replay_path.empty() && broken_replay_path == replay_dst) {
			ImGui::Text("Failed to start replay. The replay file is corrupted or outdated.");
		}
	}

	ImGui::NewLine();

	// Details below
	if (!filename.empty())
		ImGui::TextWrapped("Filename: %s", filename.c_str());
	ImGui::Text("BattleCode: %s", battle_code.c_str());
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_CLIPBOARD "  Copy")) {
		ImGui::SetClipboardText(battle_code.c_str());
	}
	ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(0, -13.0) * scaling);
	ImGui::Text("Game: %s", disk_display_name(game_disk));
	if (play_count >= 0) {
		ImGui::Text("Views: %d", play_count);
	}

	char buf[128] = {0};
	if (start_time != 0 && std::localtime(&start_time) != nullptr) {
		std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&start_time));
		ImGui::Text("StartAt: %s", buf);
	}
	if (end_time != 0 && std::localtime(&end_time) != nullptr) {
		std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&end_time));
		ImGui::Text("EndAt: %s", buf);
	}
	if (!close_reason.empty()) {
		ImGui::Text("CloseReason: %s", close_reason.c_str());
	}
	OptionCheckbox("Hide name", config::GdxReplayHideName, "Replace player names with generic names");
	OptionCheckbox("Show Ally HP", config::GdxReplayShowAllyHP, "Hack the total HP field to display Ally HP");
	OptionCheckbox("Key Display", config::GdxReplayKeyDisplay, "Display controller inputs");
	OptionCheckbox("Skip MS Selection", config::GdxReplaySkipMsSelection, "Fast-forward through the mobile suit selection screen");
	OptionCheckbox("Slowdown", config::GdxSlowdown, "Experimental: drop to 30fps while many projectiles are in play, like the arcade (DC2)");
}

void draw_round_detail(const ReplayEntry& entry) {
	auto round_wins = parse_csv_ints(entry.round_win);
	if (round_wins.empty()) return;

	std::vector<std::vector<int>> user_ms_lists;
	for (const auto& ms_str : entry.user_used_ms_list) {
		user_ms_lists.push_back(parse_csv_ints(ms_str));
	}

	ImGui::Separator();
	ImGui::TextDisabled("Round Detail");

	for (int r = 0; r < (int)round_wins.size(); r++) {
		int win_team = round_wins[r];
		ImGui::Text("R%d", r + 1);
		ImGui::SameLine();
		if (win_team == 1) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.42f, .79f, .99f, 1));
			ImGui::Text("%s", GdxsvLanguage::gdxT("Federation"));
		} else if (win_team == 2) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.97f, .23f, .35f, 1));
			ImGui::Text("%s", GdxsvLanguage::gdxT("Zeon"));
		} else {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.5f, .5f, .5f, 1));
			ImGui::TextUnformatted(win_team == -1 ? GdxsvLanguage::gdxT("Draw") : "  -  ");
		}
		ImGui::PopStyleColor();
		ImGui::SameLine();
		std::string renpo_ms, zeon_ms;
		for (int j = 0; j < (int)entry.users.size(); j++) {
			int ms_id = 0;
			if (j < (int)user_ms_lists.size() && r < (int)user_ms_lists[j].size()) {
				ms_id = user_ms_lists[j][r];
			}
			if (ms_id == 0) continue;
			const char* name = (ms_id >= 1 && ms_id <= 41) ? ms_names(ms_id - 1) : "?";
			if (entry.users[j].team() == 1) {
				if (!renpo_ms.empty()) renpo_ms += ", ";
				renpo_ms += name;
			} else {
				if (!zeon_ms.empty()) zeon_ms += ", ";
				zeon_ms += name;
			}
		}
		if (!renpo_ms.empty() || !zeon_ms.empty()) {
			ImGui::Text("%s vs %s", renpo_ms.c_str(), zeon_ms.c_str());
		}
	}
}

bool is_timestamp_replay_filename(const std::string& name) {
	// The lobby generates a 13-digit millisecond battle code: <code>.pb.
	return name.size() == 16 && name.compare(13, 3, ".pb") == 0 &&
		name.find_first_not_of("0123456789") == 13;
}

LocalReplayPage read_local_replays(const std::string& replay_dir, size_t requested_page) {
	LocalReplayPage result;
	std::vector<std::string> filenames;
	DIR* dir = flycast::opendir(replay_dir.c_str());
	if (dir == nullptr)
		return result;
	while (auto* file = flycast::readdir(dir)) {
		std::string name(file->d_name);
#ifdef __APPLE__
		name = os_PrecomposedString(name);
#endif
		if (get_file_extension(name) != "pb")
			continue;
		filenames.push_back(std::move(name));
	}
	flycast::closedir(dir);
	// Choose the page using filenames alone, before opening any recordings.
	std::sort(filenames.begin(), filenames.end(), [](const std::string& a, const std::string& b) {
		const bool a_timestamp = is_timestamp_replay_filename(a);
		const bool b_timestamp = is_timestamp_replay_filename(b);
		if (a_timestamp != b_timestamp)
			return !a_timestamp; // Named copies and edited replays come first.
		return a_timestamp ? a > b : a < b;
	});
	if (!filenames.empty())
		result.page_count = 1 + (filenames.size() - 1) / kLocalReplayPageSize;
	result.page = std::min(requested_page, result.page_count - 1);
	const size_t first = result.page * kLocalReplayPageSize;
	const size_t last = first + std::min(kLocalReplayPageSize, filenames.size() - first);
	result.entries.reserve(last - first);
	for (size_t i = first; i < last; ++i) {
		ReplayEntry entry;
		entry.filename = filenames[i];
		entry.replay_url = replay_dir + "/" + entry.filename;
		struct stat info {};
		if (flycast::stat(entry.replay_url.c_str(), &info) == 0)
			entry.start_unix = info.st_mtime;
		proto::BattleLogFile log;
		FILE* fp = nowide::fopen(entry.replay_url.c_str(), "rb");
		entry.readable = fp != nullptr && log.ParseFromFileDescriptor(fileno(fp));
		if (fp != nullptr)
			std::fclose(fp);
		if (entry.readable) {
			entry.disk = log.game_disk();
			entry.battle_code = log.battle_code();
			entry.users.assign(log.users().begin(), log.users().end());
			if (log.start_at() != 0)
				entry.start_unix = log.start_at();
			entry.end_unix = log.end_at();
			entry.close_reason = log.close_reason();
			entry.round = log.start_msg_indexes_size();
			entry.user_used_ms_list.resize(entry.users.size());
			for (int i = 0; i < log.round_data_size(); ++i) {
				const auto& round = log.round_data(i);
				if (i != 0)
					entry.round_win += ",";
				entry.round_win += std::to_string(round.win_team());
				entry.renpo_win += round.win_team() == 1;
				entry.zeon_win += round.win_team() == 2;
				for (int p = 0; p < entry.users.size(); ++p) {
					auto& ms = entry.user_used_ms_list[p];
					if (i != 0)
						ms += ",";
					ms += std::to_string(p < round.used_ms_size() ? round.used_ms(p) : 0);
				}
			}
		}
		result.entries.push_back(std::move(entry));
		// Retain only the small display metadata, never the input stream.
	}
	return result;
}

// Shared by Local and Server.
bool draw_replay_entry(const ReplayEntry& entry, int index, bool selected) {
	char timebuf[128] = {};
	if (const auto* local = std::localtime(&entry.start_unix))
		std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", local);
	char head[256] = {};
	snprintf(head, sizeof(head), u8"  %s  %s ― Result: %d：%d\n\n", ICON_FA_FILM, timebuf, entry.renpo_win, entry.zeon_win);
	std::string row = entry.readable ? head : "Unable to read replay\n\n";
	for (int i = 0; i < entry.users.size(); ++i) {
		row += entry.users[i].user_name();
		if (i + 1 < entry.users.size())
			row += entry.users[i + 1].team() != entry.users[i].team() ? " vs " : ", ";
	}
	const bool show_filename = !entry.filename.empty() && !is_timestamp_replay_filename(entry.filename);
	if (show_filename)
		row += "\n\n" + entry.filename;
	ImGui::PushID(index);
	auto* drawlist = ImGui::GetWindowDrawList();
	drawlist->ChannelsSplit(2);
	drawlist->ChannelsSetCurrent(1);
	const float height = ImGui::GetTextLineHeight() * (show_filename ? 5 : 3);
	const bool clicked = ImGui::Selectable(row.c_str(), selected, 0, ImVec2(0, height));
	if (index % 2 == 1) {
		drawlist->ChannelsSetCurrent(0);
		drawlist->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(50, 50, 50, 100));
	}
	drawlist->ChannelsMerge();
	ImGui::PopID();
	return clicked;
}

void gdxsv_replay_local_tab() {
	const auto replay_dir = get_writable_data_path("replays");
	const bool new_page = !local_replays_future.valid();
	if (new_page) {
		// Scan filenames and parse only the requested page off the UI thread.
		local_replays_future = std::async(std::launch::async, read_local_replays, replay_dir, local_replay_page).share();
	}
	const bool loaded = future_is_ready(local_replays_future);
	if (loaded)
		local_replay_page = local_replays_future.get().page;
	size_t requested_page = local_replay_page;
	ImGui::BeginDisabled(!loaded);
	if (ImGui::Button(ICON_FA_ARROW_ROTATE_RIGHT "  Reload")) {
		local_replay_page = 0;
		local_replays_future = {};
		selected_replay_file.clear();
		pov_index = -1;
		ImGui::EndDisabled();
		return;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
#if defined(TARGET_MAC)
	if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Reveal in Finder")) {
		char temp[512];
		snprintf(temp, sizeof(temp), "open \"%s\"", replay_dir.c_str());
		system(temp);
	}
#elif defined(_WIN32) && !defined(TARGET_UWP)
	if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open folder")) {
		const std::string lpParam = "/root, " + replay_dir;
		SHELLEXECUTEINFOA sei{};
		sei.cbSize = sizeof(sei);
		sei.fMask = SEE_MASK_NOCLOSEPROCESS;
		sei.lpFile = "Explorer.exe";
		sei.lpParameters = lpParam.c_str();
		sei.nShow = SW_SHOWDEFAULT;
		ShellExecuteExA(&sei);
	}
#endif

	ImGui::SameLine();
	ImGui::TextUnformatted(replay_dir.c_str());

	ImGui::BeginChild(ImGui::GetID("gdxsv_replay_file_list_paging"), ScaledVec2(450, 0), false, ImGuiWindowFlags_NoDecoration);
	ImGui::BeginChild(ImGui::GetID("gdxsv_replay_file_list"),
		ImVec2(0, std::max(1.f, ImGui::GetContentRegionAvail().y - 40.f * scaling)), true, ImGuiWindowFlags_DragScrolling);
	if (new_page)
		ImGui::SetScrollY(0);
	if (!loaded) {
		ImGui::TextUnformatted("Loading...");
	} else {
		const auto& entries = local_replays_future.get().entries;
		if (entries.empty())
			ImGui::TextUnformatted("(No replay found)");
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ScaledVec2(0, 30));
		// Custom rows include a filename and are taller. Each clipper must
		// cover a uniform-height group; sorting keeps custom names together.
		const int custom_count = static_cast<int>(std::partition_point(entries.begin(), entries.end(), [](const ReplayEntry& entry) {
			return !is_timestamp_replay_filename(entry.filename);
		}) - entries.begin());
		int first = 0;
		for (int last : {custom_count, static_cast<int>(entries.size())}) {
			ImGuiListClipper clipper;
			clipper.Begin(last - first);
			while (clipper.Step()) {
				for (int i = first + clipper.DisplayStart; i < first + clipper.DisplayEnd; ++i) {
					const auto& entry = entries[i];
					if (draw_replay_entry(entry, i, entry.filename == selected_replay_file)) {
						selected_replay_file = entry.filename;
						pov_index = 0;
					}
				}
			}
			first = last;
		}
		ImGui::PopStyleVar();
	}
	scrollWhenDraggingOnVoid();
	windowDragScroll();
	ImGui::EndChild();

	ImGui::BeginDisabled(!loaded || local_replay_page == 0);
	if (ImGui::Button(ICON_FA_CHEVRON_LEFT "  Prev Page"))
		requested_page = local_replay_page - 1;
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (loaded)
		ImGui::Text("%zu / %zu", local_replay_page + 1, local_replays_future.get().page_count);
	else
		ImGui::TextUnformatted("...");
	ImGui::SameLine();
	ImGui::BeginDisabled(!loaded || local_replay_page + 1 >= local_replays_future.get().page_count);
	if (ImGui::Button(ICON_FA_CHEVRON_RIGHT "  Next Page"))
		requested_page = local_replay_page + 1;
	ImGui::EndDisabled();
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild(ImGui::GetID("gdxsv_replay_file_detail"), ImVec2(0, 0), true, ImGuiWindowFlags_DragScrolling);
	if (loaded && !selected_replay_file.empty()) {
		const auto& entries = local_replays_future.get().entries;
		const auto selected = std::find_if(entries.begin(), entries.end(), [](const ReplayEntry& entry) {
			return entry.filename == selected_replay_file;
		});
		if (selected != entries.end()) {
			const auto& entry = *selected;
			if (entry.readable) {
				gdxsv_replay_draw_info(entry.battle_code, entry.disk, static_cast<int>(entry.users.size()),
					entry.close_reason, entry.start_unix, entry.end_unix, entry.users, entry.replay_url, false,
					entry.play_count, entry.filename);
				draw_round_detail(entry);
			} else {
				ImGui::TextWrapped("Filename: %s", entry.filename.c_str());
				ImGui::TextUnformatted("Failed to read this replay file.");
			}
		}
	}
	scrollWhenDraggingOnVoid();
	windowDragScroll();
	ImGui::EndChild();
	if (requested_page != local_replay_page) {
		// Release the completed page only after drawing all references to it.
		local_replay_page = requested_page;
		local_replays_future = {};
		selected_replay_file.clear();
		pov_index = -1;
	}
}

void parse_replay_json(const std::vector<u8>& json_string, std::vector<ReplayEntry>& out) {
	try {
		nlohmann::json j = nlohmann::json::parse(json_string);

		for (auto item : j) {
			ReplayEntry entry;
			entry.disk = item.at("disk");

			for (auto u : item.at("users")) {
				auto user = proto::BattleLogUser();
				user.set_user_id(u.at("user_id"));
				user.set_user_name(u.at("user_name"));
				user.set_pilot_name(u.at("pilot_name"));
				user.set_team(u.at("team"));
				user.set_pos(u.at("pos"));
				entry.users.push_back(user);
				entry.user_used_ms_list.push_back(u.value("used_ms_list", ""));
			}

			entry.round = item.value("round", 0);
			entry.renpo_win = item.value("renpo_win", 0);
			entry.zeon_win = item.value("zeon_win", 0);
			entry.start_unix = item.at("start_unix");
			entry.replay_url = item.at("replay_url");
			entry.round_win = item.value("round_win", "");
			entry.play_count = item.value("play_count", 0);

			out.push_back(entry);
		}
	} catch (const nlohmann::json::exception& e) {
		WARN_LOG(COMMON, "json parse failure: %s", e.what());
	}
}

void parse_user_json(const std::vector<u8>& json_string, std::vector<UserEntry>& out) {
	try {
		nlohmann::json j = nlohmann::json::parse(json_string);

		for (auto item : j) {
			UserEntry entry;
			entry.user_id = item.value("user_id", "");
			entry.name = item.value("name", "");
			entry.battle_count = item.value("battle_count", -1);
			entry.win_count = item.value("win_count", -1);
			entry.lose_count = item.value("lose_count", -1);
			entry.kill_count = item.value("kill_count", -1);
			entry.renpo_battle_count = item.value("renpo_battle_count", -1);
			entry.renpo_win_count = item.value("renpo_win_count", -1);
			entry.renpo_lose_count = item.value("renpo_lose_count", -1);
			entry.renpo_kill_count = item.value("renpo_kill_count", -1);
			entry.zeon_battle_count = item.value("zeon_battle_count", -1);
			entry.zeon_win_count = item.value("zeon_win_count", -1);
			entry.zeon_lose_count = item.value("zeon_lose_count", -1);
			entry.zeon_kill_count = item.value("zeon_kill_count", -1);
			entry.daily_battle_count = item.value("daily_battle_count", -1);
			entry.daily_win_count = item.value("daily_win_count", -1);
			entry.daily_lose_count = item.value("daily_lose_count", -1);
			out.push_back(entry);
		}
	} catch (const nlohmann::json::exception& e) {
		WARN_LOG(COMMON, "json parse failure: %s", e.what());
	}
}

void fetch_replay_json() {
	if (replay_results_dirty || fetch_replay_entry_future_.valid()) {
		return;
	}

	// Snapshot the query on the UI thread; the worker does not read filter state.
	std::string url = replay_api_url("/replay?");
	url += "page=" + http::urlEncode(std::to_string(entry_paging));
	for (const auto& value : search_user_ids)
		url += "&user_id=" + http::urlEncode(value);
	for (const auto& value : search_user_names)
		url += "&user_name=" + http::urlEncode("%" + value + "%");
	for (const auto& value : search_pilot_names)
		url += "&pilot_name=" + http::urlEncode("%" + value + "%");
	if (!search_lobby_id.empty()) {
		url += "&lobby_id=" + http::urlEncode(search_lobby_id);
	}
	if (search_no_of_players != 0) {
		url += "&players=" + http::urlEncode(std::to_string(search_no_of_players));
	}
	if (!search_battle_code.empty()) {
		url += "&battle_code=" + http::urlEncode(search_battle_code);
	}
	if (search_ranking != -1) {
		url += "&aggregate=" + http::urlEncode(std::to_string(search_ranking));
	}
	if (!search_disk.empty()) {
		url += "&disk=" + http::urlEncode(search_disk);
	}
	if (search_reverse) {
		url += "&reverse=" + http::urlEncode(std::to_string(1));
	}
	if (search_used_ms != -1) {
		url += "&used_ms=" + http::urlEncode(std::to_string(search_used_ms));
	}

	const auto future_fn = [url]() -> std::vector<ReplayEntry> {
		std::vector<ReplayEntry> entries{};
		std::vector<u8> dl;
		std::string content_type;
		http::init();
		fetch_replay_entry_http_status = http::get(url, dl, content_type);
		if (fetch_replay_entry_http_status != 200) {
			ERROR_LOG(COMMON, "version check failure: %s", url.c_str());
			return entries;
		}

		parse_replay_json(dl, entries);
		return entries;
	};

	fetch_replay_entry_future_ = std::async(std::launch::async, future_fn).share();
}

void parse_live_json(const std::vector<u8>& json_string, std::vector<LiveEntry>& out) {
	try {
		nlohmann::json j = nlohmann::json::parse(json_string);

		std::map<std::string, LiveEntry> by_code;
		for (const auto& g : j.value("active_games", nlohmann::json::array())) {
			LiveEntry e;
			e.battle_code = g.value("battle_code", "");
			if (e.battle_code.empty()) continue;
			e.disk = g.value("disk", "");
			e.state = g.value("state", "");
			e.lobby_id = g.value("lobby_id", 0);
			e.live_spectate = g.value("live_spectate", false);
			e.spectators = g.value("spectators", 0);
			e.updated_unix = parse_iso8601(g.value("updated_at", ""));
			by_code[e.battle_code] = e;
		}

		// battle_users is a flat list across every battle, and the server
		// dedups it by user_id - so a player in two battles appears once and
		// one of those battles comes up a man short. Render what arrived.
		for (const auto& u : j.value("battle_users", nlohmann::json::array())) {
			auto it = by_code.find(u.value("battle_code", ""));
			if (it == by_code.end()) continue;
			auto user = proto::BattleLogUser();
			user.set_user_id(u.value("user_id", ""));
			user.set_user_name(u.value("name", ""));
			user.set_pilot_name(u.value("pilot_name", ""));
			user.set_pos(u.value("battle_pos", 0));
			user.set_team(u.value("team", "") == "renpo" ? 1 : 2);
			it->second.users.push_back(user);
		}

		for (auto& kv : by_code) {
			std::sort(kv.second.users.begin(), kv.second.users.end(),
					  [](const proto::BattleLogUser& a, const proto::BattleLogUser& b) { return a.pos() < b.pos(); });
			out.push_back(kv.second);
		}
	} catch (const nlohmann::json::exception& e) {
		ERROR_LOG(COMMON, "live status json parse failure: %s", e.what());
	}
}

void fetch_live_json() {
	if (fetch_live_entry_future_.valid()) {
		return;
	}
	live_last_fetch_ = std::chrono::steady_clock::now();

	const auto future_fn = []() -> std::vector<LiveEntry> {
		std::vector<LiveEntry> entries{};
		std::vector<u8> dl;
		std::string content_type;
		http::init();
		const std::string url = config::loadStr("gdxsv", "LiveApiUrl", "https://asia-northeast1-gdxsv-274515.cloudfunctions.net/lbsapi") + "/status";

		fetch_live_entry_http_status = http::get(url, dl, content_type);
		if (fetch_live_entry_http_status != 200) {
			ERROR_LOG(COMMON, "live status fetch failure %d: %s", fetch_live_entry_http_status, url.c_str());
			return entries;
		}

		parse_live_json(dl, entries);
		return entries;
	};

	fetch_live_entry_future_ = std::async(std::launch::async, future_fn).share();
}

void fetch_live_refresh() {
	fetch_live_entry_future_ = std::shared_future<std::vector<LiveEntry>>();
	fetch_live_json();
}

// Moves a finished fetch into live_entries_ and frees the future for the next
// poll. A failed fetch leaves the previous list up rather than emptying it.
void live_harvest_fetch() {
	if (!fetch_live_entry_future_.valid() || !future_is_ready(fetch_live_entry_future_)) {
		return;
	}
	if (fetch_live_entry_http_status == 200) {
		live_entries_ = fetch_live_entry_future_.get();
	}
	live_first_load_ = false;
	fetch_live_entry_future_ = std::shared_future<std::vector<LiveEntry>>();
}

// Viewer count for a battle being watched right now. Polled over HTTP, and
// only while the control bar is on screen.
std::shared_future<int> fetch_viewers_future_;
std::chrono::steady_clock::time_point viewers_last_fetch_;
int live_viewer_count_ = 0;
constexpr int kViewerRefreshSeconds = 5;

void fetch_user_json() {
	if (fetch_user_entry_future_.valid()) {
		return;
	}

	const auto future_fn = []() -> std::vector<UserEntry> {
		std::vector<UserEntry> entries{};
		std::vector<u8> dl;
		std::string content_type;
		http::init();
		std::string url = replay_api_url("/user?");

		std::string loginkey = config::loadStr("gdxsv", "loginkey", "");
		std::vector<u8> e_loginkey(loginkey.size());
		static constexpr int magic[] = {0x46, 0xcf, 0x2d, 0x55};
		for (int i = 0; i < e_loginkey.size(); ++i) e_loginkey[i] ^= loginkey[i] ^ magic[i & 3];
		unsigned hash = 2166136261U;
		for (const unsigned char e : e_loginkey) hash = hash * 16777619U ^ static_cast<unsigned>(e);
		std::ostringstream hashed_loginkey_s;
		// Account keys must be plain hex, without locale-specific separators.
		hashed_loginkey_s.imbue(std::locale::classic());
		hashed_loginkey_s << std::setfill('0') << std::setw(8) << std::hex << hash;

		url += "login_key=" + http::urlEncode(hashed_loginkey_s.str());

		fetch_user_entry_http_status = http::get(url, dl, content_type);
		if (fetch_user_entry_http_status != 200) {
			ERROR_LOG(COMMON, "version check failure: %s", url.c_str());
			return entries;
		}

		parse_user_json(dl, entries);
		return entries;
	};

	fetch_user_entry_future_ = std::async(std::launch::async, future_fn).share();
}

void fetch_new_results(bool reset_page) {
	if (reset_page) {
		entry_paging = 0;
		snprintf(page_buf, sizeof(page_buf), "%d", 1);
	}
	selected_replay_entry_index = -1;
	// Keep the current future alive until it finishes and drawing is over.
	// Further filter edits accumulate in the query, not in a single pending slot.
	replay_results_dirty = true;
}

void fetch_target_page() { fetch_new_results(false); }

void draw_add_player_filter_button(std::vector<std::string>& filters, char* input) {
	const char* disabled_hint = player_filter_disabled_hint(filters, input);
	ImGui::SameLine();
	ImGui::BeginDisabled(disabled_hint != nullptr);
	if (ImGui::Button("Add Filter") && add_player_filter(filters, input)) {
		input[0] = '\0';
		fetch_new_results();
	}
	ImGui::EndDisabled();
	if (disabled_hint != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", disabled_hint);
}

template <typename Callable>
void draw_filter_label(const std::string& label, const std::string& value, Callable on_click) {
	const std::string text = label + " = " + value + u8"  ×";
	const float width = ImGui::CalcTextSize(text.c_str(), nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;

	ImGui::SameLine();
	if (ImGui::GetContentRegionAvail().x < width)
		ImGui::NewLine();
	ImGui::GetStyle().FrameRounding = 5.0f * scaling;
	if (ImGui::Button(text.c_str(), ImVec2(width, 0))) {
		on_click();
		fetch_new_results();
	}
	ImGui::GetStyle().FrameRounding = 0.0f;
};

void draw_filter_label_list(const std::string& label, std::vector<std::string>& values) {
	ImGui::PushID(label.c_str());
	for (size_t i = 0; i < values.size();) {
		bool removed = false;
		draw_filter_label(label, values[i], [&]() {
			values.erase(values.begin() + i);
			removed = true;
		});
		if (!removed)
			++i;
	}
	ImGui::PopID();
}

void draw_filter_label_string(const std::string& label, std::string& value) {
	if (!value.empty()) {
		draw_filter_label(label, value, [&value]() { value = ""; });
	}
};

void draw_filter_label_int(const std::string& label, unsigned int& value) {
	if (value != 0) {
		draw_filter_label(label, std::to_string(value), [&value]() { value = 0; });
	}
};
void draw_filter_label_yesno(const std::string& label, int& value) {
	if (value != -1) {
		draw_filter_label(label, value ? "Yes" : "No", [&value]() { value = -1; });
	}
};
void draw_filter_label_bool(const std::string& label, bool& value) {
	if (value != false) {
		draw_filter_label(label, "Yes", [&value]() { value = false; });
	}
};

void gdxsv_replay_live_tab() {
	live_harvest_fetch();

	const bool due = kLiveRefreshSeconds <= std::chrono::duration_cast<std::chrono::seconds>(
											   std::chrono::steady_clock::now() - live_last_fetch_)
											   .count();
	if (!fetch_live_entry_future_.valid() && (live_first_load_ || due)) {
		fetch_live_json();
	}

	const bool refreshing = fetch_live_entry_future_.valid();
	{
		DisabledScope scope(refreshing);
		// Default size, like every other button in these tabs - so the status
		// line beside it lands on the same baseline without nudging.
		if (ImGui::Button(ICON_FA_ARROW_ROTATE_RIGHT "  Refresh") && !scope.isDisabled()) {
			fetch_live_refresh();
		}
	}
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	if (refreshing && !live_first_load_) {
		ImGui::TextDisabled("Battles in progress. Updating...");
	} else if (fetch_live_entry_http_status != 200 && !live_first_load_) {
		ImGui::TextDisabled("Battles in progress. Last update failed (HTTP %d).", fetch_live_entry_http_status);
	} else {
		ImGui::TextDisabled("Battles in progress. Updates every %ds.", kLiveRefreshSeconds);
	}

	ImGui::BeginChild(ImGui::GetID("gdxsv_live_list"), ScaledVec2(450, 0), true, ImGuiWindowFlags_DragScrolling);
	{
		if (live_first_load_) {
			ImGui::Text("Loading...");
		} else if (live_entries_.empty()) {
			ImGui::Text("No battle in progress.");
		} else {
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ScaledVec2(0, 30.0f));
			for (int i = 0; i < (int)live_entries_.size(); ++i) {
				ImGui::PushID(i);
				const auto& entry = live_entries_[i];

				const char* row_lobby = lobby_name_by_id(entry.lobby_id);
				std::string row = "  ";
				row += entry.live_spectate ? ICON_FA_TOWER_BROADCAST : ICON_FA_CIRCLE_DOT;
				row += "  ";
				row += row_lobby != nullptr ? std::string(row_lobby) : ("Lobby " + std::to_string(entry.lobby_id));
				row += "\n\n";
				for (int u = 0; u < (int)entry.users.size(); u++) {
					const auto& user = entry.users[u];
					row += user.user_name();
					if (u + 1 < (int)entry.users.size()) {
						row += entry.users[u + 1].team() != user.team() ? " vs " : ", ";
					}
				}

				const ImVec2 row_pos = ImGui::GetCursorScreenPos();

				// Listed but not selectable: everyone in it is on a build
				// without the uplink, so there is nothing to receive.
				if (!entry.live_spectate) {
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
					ImGui::Selectable(row.c_str(), false, ImGuiSelectableFlags_Disabled, ImVec2(0, 0));
					ImGui::PopStyleColor();
				} else if (ImGui::Selectable(row.c_str(), entry.battle_code == selected_live_battle_code, 0, ImVec2(0, 0))) {
					selected_live_battle_code = entry.battle_code;
					pov_index = -1;
				}

				// Viewer count, right-aligned on the lobby line. Drawn straight
				// to the draw list so it does not become a second item and eat
				// clicks meant for the row.
				if (entry.live_spectate) {
					char watchers[64] = {};
					snprintf(watchers, sizeof(watchers), ICON_FA_EYE " %d", entry.spectators);
					const ImVec2 text_size = ImGui::CalcTextSize(watchers);
					const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
					const float right_margin = 8.0f * scaling;
					ImGui::GetWindowDrawList()->AddText(ImVec2(right - text_size.x - right_margin, row_pos.y),
														ImGui::GetColorU32(ImGuiCol_TextDisabled), watchers);
				}

				ImGui::PopID();
			}
			ImGui::PopStyleVar();
		}
		scrollWhenDraggingOnVoid();
		windowDragScroll();
	}
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild(ImGui::GetID("gdxsv_live_detail"), ImVec2(0, 0), true, ImGuiWindowFlags_DragScrolling);
	{
		const LiveEntry* selected = nullptr;
		for (const auto& e : live_entries_) {
			if (e.battle_code == selected_live_battle_code) {
				selected = &e;
				break;
			}
		}

		if (selected == nullptr) {
			ImGui::Text(selected_live_battle_code.empty() ? "Select a battle to watch." : "That battle has ended.");
		} else {
			// Same blue-renpo / red-zeon cards the replay detail uses, and the
			// same pov_index, so picking a view works identically in both.
			gdxsv_replay_draw_players(selected->users, false);

			ImGui::NewLine();
			{
				const bool playable = ("dc" + std::to_string(gdxsv.Disk())) == selected->disk;
				ImGui::BeginDisabled(pov_index == -1 || !playable);
				if (ImGui::ButtonEx(pov_index == -1 ? ICON_FA_ARROW_POINTER "  Select a player" : ICON_FA_TOWER_BROADCAST "  Watch Live",
									ScaledVec2(240, 50))) {
					gdxsv_start_live_spectate(selected->battle_code, pov_index);
				}
				ImGui::EndDisabled();
				if (!playable) {
					ImGui::SameLine();
					ImGui::AlignTextToFramePadding();
					ImGui::TextDisabled("This battle is on %s.", disk_display_name(selected->disk));
				}
			}

			// Same fields the replay detail lists, so the two read alike.
			ImGui::NewLine();
			const char* sel_lobby = lobby_name_by_id(selected->lobby_id);
			if (sel_lobby != nullptr) {
				ImGui::Text("Lobby: %s", sel_lobby);
			} else {
				ImGui::Text("Lobby: %d", selected->lobby_id);
			}
			ImGui::Text("BattleCode: %s", selected->battle_code.c_str());
			ImGui::Text("Game: %s", disk_display_name(selected->disk));
			ImGui::Text("Current Viewers: %d", selected->spectators);
			if (selected->updated_unix != 0) {
				char timebuf[128] = {};
				std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", std::localtime(&selected->updated_unix));
				ImGui::Text("StartAt: %s", timebuf);
			}
		}
		scrollWhenDraggingOnVoid();
		windowDragScroll();
	}
	ImGui::EndChild();
}


void gdxsv_replay_server_tab() {
	struct TextFilters {
		// Return 0 (pass) if the character is number
		static int FilterNumber(ImGuiInputTextCallbackData* data) {
			if (data->EventChar < 256 && strchr("0123456789", (char)data->EventChar)) return 0;
			return 1;
		}

		static int FullWidthAlphaNum(ImGuiInputTextCallbackData* data) {
			if (data->EventChar >= 0x21 && data->EventChar <= 0x7E) data->EventChar = (data->EventChar + 0xFEE0);

			return 0;
		}

		static int UppercaseAlpha(ImGuiInputTextCallbackData* data) {
			if (data->EventChar >= 0x21 && data->EventChar <= 0x7E) data->EventChar = toupper(data->EventChar);

			return 0;
		}
	};

	ImGui::AlignTextToFramePadding();

	ImGui::Text("Filter by");

	const std::array<std::string, 10> filter_labels{"User ID",	  "User Name", "Pilot Name", "Lobby ID",	 "No. of Players",
												    "Battle Code", "Ranking",   "Disk",		 "Used MS", "Reverse Order"};
	static unsigned int filter_selected = 0;

	ImGui::SameLine();
	ImGui::PushItemWidth(150.0f * scaling);
	if (ImGui::BeginCombo("##FilterItems", filter_labels[filter_selected].c_str(), ImGuiComboFlags_HeightLargest)) {
		for (u32 i = 0; i < filter_labels.size(); i++) {
			bool is_selected = i == filter_selected;
			if (ImGui::Selectable(filter_labels[i].c_str(), is_selected)) filter_selected = i;
			if (is_selected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	ImGui::PopItemWidth();

	{
		DisabledScope loading_scope(!replay_results_ready());

		switch (filter_selected) {
			case 0:	 // User ID
			{
				if (!fetch_user_entry_future_.valid()) {
					fetch_user_json();
				}
				static char user_id_buf[7] = {0};
				{
					DisabledScope loading_scope(!future_is_ready(fetch_user_entry_future_));
					ImGui::SameLine();
					if (ImGui::Button("My IDs")) ImGui::OpenPopup("my_id_popup");
					ImGui::SameLine();
					if (ImGui::BeginPopup("my_id_popup")) {
						ImGui::Text("Handle Name");
						ImGui::Separator();
						auto entries = fetch_user_entry_future_.get();
						for (int i = 0; i < entries.size(); i++)
							if (ImGui::Selectable(entries[i].name.c_str())) {
								snprintf(user_id_buf, sizeof(user_id_buf), "%s", entries[i].user_id.c_str());
							}
						ImGui::EndPopup();
					}
				}

				ImGui::SameLine();
				if (ImGui::InputText("##user_id_input", user_id_buf, IM_ARRAYSIZE(user_id_buf),
									 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter,
									 TextFilters::UppercaseAlpha)) {
					if (add_player_filter(search_user_ids, user_id_buf)) {
						user_id_buf[0] = '\0';
						fetch_new_results();
					}
				}

				draw_add_player_filter_button(search_user_ids, user_id_buf);

				break;
			}
			case 1:	 // User Name
			{
				static char user_name_buf[100] = {0};
				ImGui::SameLine();
				if (ImGui::InputText("##user_name_input", user_name_buf, IM_ARRAYSIZE(user_name_buf),
									 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter,
									 TextFilters::FullWidthAlphaNum)) {
					if (add_player_filter(search_user_names, user_name_buf)) {
						user_name_buf[0] = '\0';
						fetch_new_results();
					}
				}

				draw_add_player_filter_button(search_user_names, user_name_buf);

				break;
			}
			case 2:	 // Pilot Name
			{
				static char pilot_name_buf[100] = {0};
				ImGui::SameLine();
				if (ImGui::InputText("##pilot_name_input", pilot_name_buf, IM_ARRAYSIZE(pilot_name_buf),
									 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter,
									 TextFilters::FullWidthAlphaNum)) {
					if (add_player_filter(search_pilot_names, pilot_name_buf)) {
						pilot_name_buf[0] = '\0';
						fetch_new_results();
					}
				}

				draw_add_player_filter_button(search_pilot_names, pilot_name_buf);

				break;
			}
			case 3:	 // Lobby ID
			{
				static unsigned int lobby_selected = 0;

				auto get_lobby_name = [&](size_t i) {
					return GdxsvLanguage::gdxT(lobby_data[i][0]);
				};

				ImGui::SameLine();
				ImGui::PushItemWidth(300.0f * scaling);
				if (ImGui::BeginCombo("##LobbyItems", get_lobby_name(lobby_selected), ImGuiComboFlags_HeightLargest)) {
					for (u32 i = 0; i < lobby_data.size(); i++) {
						bool is_selected = i == lobby_selected;
						if (ImGui::Selectable(get_lobby_name(i), is_selected)) {
							lobby_selected = i;
							search_lobby_id = lobby_data[i][1];
							fetch_new_results();
						}
						if (is_selected) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				ImGui::PopItemWidth();

				break;
			}
			case 4:	 // No of Players
			{
				ImGui::SameLine();
				static int no_of_players_input = 2;
				ImGui::SliderInt("##no_of_players_input", &no_of_players_input, 2, 4);

				ImGui::SameLine();
				if (ImGui::Button("Add Filter")) {
					search_no_of_players = no_of_players_input;
					fetch_new_results();
				}

				break;
			}
			case 5:	 // Battle Code
			{
				static char battle_code_buf[100] = {0};
				ImGui::SameLine();
				if (ImGui::InputText("##battle_code_input", battle_code_buf, IM_ARRAYSIZE(battle_code_buf),
									 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter,
									 TextFilters::FilterNumber)) {
					search_battle_code = std::string(battle_code_buf);
					fetch_new_results();
				}

				ImGui::SameLine();
				if (ImGui::Button("Add Filter")) {
					search_battle_code = std::string(battle_code_buf);
					fetch_new_results();
				}

				break;
			}
			case 6:	 // Ranking
			{
				ImGui::SameLine();
				if (ImGui::Button("Yes")) {
					search_ranking = 1;
					fetch_new_results();
				}
				ImGui::SameLine();
				if (ImGui::Button("No")) {
					search_ranking = 0;
					fetch_new_results();
				}
				break;
			}
			case 7:	 // Disk
			{
				ImGui::SameLine();
				if (ImGui::Button("MUJI")) {
					search_disk = "dc1";
					fetch_new_results();
				}
				ImGui::SameLine();
				if (ImGui::Button("DX")) {
					search_disk = "dc2";
					fetch_new_results();
				}
				break;
			}
			case 8:	 // Used MS
			{
				static unsigned int ms_selected = 0;

				ImGui::SameLine();
				ImGui::PushItemWidth(200.0f * scaling);
				if (ImGui::BeginCombo("##UsedMSItems", ms_names(ms_selected), ImGuiComboFlags_HeightLargest)) {
					for (int i = 0; i < GdxsvLanguage::GetMSCount(); i++) {
						const char* name = ms_names(i);
						if (strlen(name) == 0) continue;
						bool is_selected = i == ms_selected;
						if (ImGui::Selectable(name, is_selected)) {
							ms_selected = i;
							search_used_ms = i + 1;  // 1-origin
							fetch_new_results();
						}
						if (is_selected) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				ImGui::PopItemWidth();

				break;
			}
			case 9:	 // Reverse
			{
				ImGui::SameLine();
				if (ImGui::Button("Add Filter")) {
					search_reverse = true;
					fetch_new_results();
				}
				break;
			}
			default:
				break;
		}

		ImGui::Dummy(ImVec2(0, 0));	 // Newline

		draw_filter_label_list("User ID", search_user_ids);
		draw_filter_label_list("User Name", search_user_names);
		draw_filter_label_list("Pilot Name", search_pilot_names);
		draw_filter_label_string("Lobby ID", search_lobby_id);
		draw_filter_label_int("Players", search_no_of_players);
		draw_filter_label_string("Battle Code", search_battle_code);
		draw_filter_label_yesno("Ranking", search_ranking);
		if (!search_disk.empty()) {
			draw_filter_label("Disk", disk_display_name(search_disk), [&]() { search_disk = ""; });
		}
		draw_filter_label_bool("Reverse", search_reverse);
		if (search_used_ms != -1) {
			draw_filter_label("Used MS", std::to_string(search_used_ms), []() { search_used_ms = -1; });
		}
	}

	ImGui::BeginChild(ImGui::GetID("gdxsv_replay_server_list_paging"), ScaledVec2(450, 0), false, ImGuiWindowFlags_NoDecoration);
	{
		ImGui::BeginChild(ImGui::GetID("gdxsv_replay_server_list"), ImVec2(0, ImGui::GetContentRegionAvail().y - 40.f * scaling), true,
						  ImGuiWindowFlags_DragScrolling);
		{
			if (replay_results_dirty) {
				ImGui::Text("Loading...");
			} else if (!fetch_replay_entry_future_.valid()) {
				fetch_replay_json();
			} else if (!future_is_ready(fetch_replay_entry_future_)) {
				ImGui::Text("Loading...");
			} else {
				const auto& entries = fetch_replay_entry_future_.get();
				if (entries.size() == 0 || fetch_replay_entry_http_status == 204) {
					ImGui::Text("No result");
				} else if (fetch_replay_entry_http_status != 200) {
					ImGui::Text("Error: HTTP %d", fetch_replay_entry_http_status);
				} else {
					ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ScaledVec2(0, 30));
					for (int i = 0; i < entries.size(); ++i) {
						if (draw_replay_entry(entries[i], i, i == selected_replay_entry_index)) {
							selected_replay_entry_index = i;
							pov_index = filtered_replay_pov(entries[i].users);
						}
					}
					ImGui::PopStyleVar();
				}
			}
		}
		scrollWhenDraggingOnVoid();
		windowDragScroll();
		ImGui::EndChild();

		{
			DisabledScope loading_scope(!replay_results_ready());
			{
				{
					DisabledScope scope(entry_paging == 0);
					if (ImGui::Button(ICON_FA_CHEVRON_LEFT "  Prev Page") && !scope.isDisabled()) {
						entry_paging--;
						snprintf(page_buf, sizeof(page_buf), "%d", entry_paging + 1);
						fetch_target_page();
					}
				}
				{
					ImGui::SameLine();
					ImGui::SetNextItemWidth(60.f * scaling);
					if (ImGui::InputText("##page_input", page_buf, IM_ARRAYSIZE(page_buf),
						ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter,
						TextFilters::FilterNumber)) {
						entry_paging = atoi(page_buf) - 1;
						if (entry_paging < 0) entry_paging = 0;
						fetch_target_page();
					}
					ImGui::SameLine();
					DisabledScope scope(replay_results_ready() && fetch_replay_entry_future_.get().size() < 100);
					if (ImGui::Button(ICON_FA_CHEVRON_RIGHT "  Next Page")) {
						entry_paging++;
						snprintf(page_buf, sizeof(page_buf), "%d", entry_paging + 1);
						fetch_target_page();
					}
				}
			}
		}
	}
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild(ImGui::GetID("gdxsv_replay_server_detail"), ImVec2(0, 0), true, ImGuiWindowFlags_DragScrolling);
	{
		if (replay_results_ready() && selected_replay_entry_index != -1) {
			const auto& entries = fetch_replay_entry_future_.get();
			const auto& entry = entries[selected_replay_entry_index];
			std::string battle_code = entry.replay_url.substr(entry.replay_url.find_last_of("/") + 1);
			battle_code = battle_code.substr(0, battle_code.find(".pb"));

			gdxsv_replay_draw_info(battle_code, entry.disk, (int)entry.users.size(), "", entry.start_unix, 0, entry.users,
								   entry.replay_url, true, entry.play_count);
			draw_round_detail(entry);
		}
	}
	scrollWhenDraggingOnVoid();
	windowDragScroll();
	ImGui::EndChild();
}

}  // namespace

bool gdxsv_ensure_replay_savestate(int disk) {
	const auto save_path = hostfs::getSavestatePath(99, false);
	if (file_exists(save_path)) {
		return true;
	}

	http::init();
	std::vector<u8> downloaded;
	std::string content_type;
	const std::string url = disk == 1 ? "https://storage.googleapis.com/gdxsv/misc/gdx-disc1_99.state"
									  : "https://storage.googleapis.com/gdxsv/misc/gdx-disc2_99.state";
	int rc = http::get(url, downloaded, content_type);
	if (rc != 200) {
		ERROR_LOG(COMMON, "replay savestate download failure rc=%d url=%s", rc, url.c_str());
		return false;
	}

	FILE* fp = nowide::fopen(save_path.c_str(), "wb");
	if (fp == nullptr) {
		ERROR_LOG(COMMON, "replay savestate save failure: %s", save_path.c_str());
		return false;
	}

	const auto written = fwrite(downloaded.data(), 1, downloaded.size(), fp);
	std::fclose(fp);
	return written == downloaded.size();
}

int gdxsv_live_viewer_count(const std::string& battle_code, bool force_refresh) {
	if (battle_code.empty()) return 0;

	if (fetch_viewers_future_.valid() && future_is_ready(fetch_viewers_future_)) {
		live_viewer_count_ = fetch_viewers_future_.get();
		fetch_viewers_future_ = std::shared_future<int>();
	}

	const auto now = std::chrono::steady_clock::now();
	const bool due = kViewerRefreshSeconds <= std::chrono::duration_cast<std::chrono::seconds>(now - viewers_last_fetch_).count();
	if (!fetch_viewers_future_.valid() && (due || force_refresh)) {
		viewers_last_fetch_ = now;
		// Capture the current count rather than reading the global from the
		// worker: the main thread writes it when a fetch lands.
		const int last_known = live_viewer_count_;
		fetch_viewers_future_ = std::async(std::launch::async, [battle_code, last_known]() -> int {
			std::vector<u8> dl;
			std::string content_type;
			http::init();
			const std::string url =
				config::loadStr("gdxsv", "LiveApiUrl", "https://asia-northeast1-gdxsv-274515.cloudfunctions.net/lbsapi") +
				"/spectators?battle_code=" + http::urlEncode(battle_code);
			if (http::get(url, dl, content_type) != 200) return last_known;
			try {
				nlohmann::json j = nlohmann::json::parse(dl);
				return j.value("spectators", 0);
			} catch (const nlohmann::json::exception&) {
			}
			return last_known;
		}).share();
	}

	return live_viewer_count_;
}

void gdxsv_start_live_spectate(const std::string& battle_code, int pov) {
	if (gdxsv.IsSaveStateAllowed()) {
		dc_savestate(90);
	}

	if (gdxsv_ensure_replay_savestate(gdxsv.Disk())) {
		dc_loadstate(99);
		if (gdxsv.StartLiveSpectate(battle_code.c_str(), pov)) {
			gui_state = GuiState::Closed;
		}
	}
}

// Fire-and-forget: notify server of replay play (HTTP replays only)
static void gdxsv_notify_replay_played(const std::string& replay_file) {
	if (replay_file.find("http") != 0) return;
	const auto pos = replay_file.find_last_of("/");
	if (pos == std::string::npos) return;
	std::string battle_code = replay_file.substr(pos + 1);
	const auto dot = battle_code.find(".pb");
	if (dot != std::string::npos) {
		battle_code = battle_code.substr(0, dot);
	}
	std::thread([battle_code]() {
		http::init();
		std::vector<u8> dl;
		std::string content_type;
		auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		std::string url = replay_api_url("/replay_played?battle_code=")
			+ http::urlEncode(battle_code) + "&_t=" + std::to_string(ts);
		http::get(url, dl, content_type);
	}).detach();
}

void gdxsv_start_replay(const std::string& replay_file, int pov, bool four_screen) {
	if (gdxsv.IsSaveStateAllowed()) {
		dc_savestate(90);
	}

	if (gdxsv_ensure_replay_savestate(gdxsv.Disk())) {
		// 4-player replay: host it, or fall back to single-screen playback.
		std::vector<uint8_t> hosted_replay;
		if (four_screen && gdxsv_multi_pov_begin_host_session(replay_file, hosted_replay)) {
			dc_loadstate(99);
			if (gdxsv.StartReplayBuffer(hosted_replay, 0)) {
				gui_state = GuiState::Closed;
				gdxsv_notify_replay_played(replay_file);
			} else {
				gdxsv_multi_pov_close();
				dc_loadstate(90);
				broken_replay_path = replay_file;
			}
			return;
		}

		dc_loadstate(99);
		if (gdxsv.StartReplayFile(replay_file.c_str(), pov)) {
			gui_state = GuiState::Closed;
			gdxsv_notify_replay_played(replay_file);
		} else {
			dc_loadstate(90);
			broken_replay_path = replay_file;
		}
	}
}

void gdxsv_end_replay(std::string error) {
	// Own the message: restoring the previous state resets the replay backend.
	emu.stop();
	dc_loadstate(90);
	settings.input.fastForwardMode = false;

	// Reopen the browser; ImGui retains the tab that launched playback.
	gui_state = GuiState::GdxsvReplay;
	if (!error.empty()) {
		gui_error(error);
	}
}

void gdxsv_replay_select_dialog() {
	// Discard outdated results between frames, once the old request is done.
	// The next fetch snapshots all current filters; outdated results stay hidden.
	if (replay_results_dirty &&
		(!fetch_replay_entry_future_.valid() || future_is_ready(fetch_replay_entry_future_))) {
		fetch_replay_entry_future_ = {};
		replay_results_dirty = false;
	}

	centerNextWindow();
	ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);

	scaling = settings.display.uiScale;
	normal_padding = ImGui::GetStyle().FramePadding;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::Begin("Live & Replays##gdxsv_emu_replay_menu", nullptr,
				 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

	if (ImGui::Button(ICON_FA_XMARK  "  Close", ScaledVec2(100, 40))) {
		gui_state = GuiState::Commands;
	}

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(16, 6));
	if (ImGui::BeginTabBar("replays", ImGuiTabBarFlags_NoTooltip)) {
		if (ImGui::BeginTabItem(ICON_FA_TOWER_BROADCAST "  Live")) {
			gdxsv_replay_live_tab();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(ICON_FA_FOLDER "  Local")) {
			gdxsv_replay_local_tab();
			ImGui::EndTabItem();
		}

		const ImGuiTabItemFlags server_flags = select_server_tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
		select_server_tab = false;
		if (ImGui::BeginTabItem(ICON_FA_GLOBE "  Server", nullptr, server_flags)) {
			gdxsv_replay_server_tab();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::PopStyleVar();

	ImGui::End();
	ImGui::PopStyleVar();  // ImGuiStyleVar_WindowRounding
}
