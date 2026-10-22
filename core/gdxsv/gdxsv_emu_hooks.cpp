#include "gdxsv_emu_hooks.h"
#include "gdxsv_config.h"
#include <cstdlib>

#include <algorithm>
#include <chrono>
#include <locale>
#include <regex>
#include <thread>
#include <sstream>

#include "cfg/cfg.h"
#include "gdxsv.h"
#include "libs.h"
#include "stdclass.h"
#include <nowide/cstdio.hpp>
#include "gdxsv_custom_texture_source.h"
#include "gdxsv_frame_limiter.h"
#include "gdxsv_gui_settings.h"
#include "gdxsv_multi_pov.h"
#include "gdxsv_multi_pov_audio.h"
#include "gdxsv_multi_pov_viewer.h"
#include "gdxsv_video_hooks.h"
#include "gdxsv_platform.h"
#include "gdxsv_save_state.h"
#include "emulator.h"
#include "gdxsv_replay_util.h"
#include "gdxsv_update.h"
#include "gdxsv_custom_texture_update.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "json.hpp"
#include "nowide/fstream.hpp"
#include "oslib/directory.h"
#include "oslib/oslib.h"
#include "oslib/http_client.h"
#include "ui/gui_util.h"
#include "ui/IconsFontAwesome6.h"
#include "ui/imgui_driver.h"
#include "hw/pvr/Renderer_if.h"
#include "network/ggpo.h"
#include "gdxsv_ggpo_hooks.h"
#include "stdclass.h"
#include "xxhash.h"

using namespace nlohmann;

static void gdxsv_update_popup();
static void gdxsv_texture_update_popup();
static void wireless_warning_popup(const std::string& connection_medium);
static void vpn_warning_toast(const std::string& connection_medium);
static void p2p_connection_toast();

std::atomic<int> gdxsv_frame_period_trim_us{0};

// Live 4-screen guest: the host's battle generation this screen is on, and
// whether it is sitting that battle out.
static uint32_t g_live_guest_generation = 0;
static bool g_live_guest_idle = false;

namespace {
// A GGPO session of a gdxsv battle: its frame order differs from upstream's netplay.
bool RollbackBattle() { return ggpo::active() && gdxsv.Enabled(); }
}  // namespace

bool gdxsv_enabled() { return gdxsv.Enabled(); }

bool gdxsv_is_multi_pov_guest() { return 0 <= gdxsv_multi_pov_guest_pov(); }

bool gdxsv_emu_toggle_fullscreen() { return gdxsv_video_hidden_window(); }

bool gdxsv_is_ingame() { return gdxsv.InGame(); }

bool gdxsv_is_online() { return gdxsv.IsOnline(); }

bool gdxsv_is_savestate_allowed() { return gdxsv.IsSaveStateAllowed(); }

void gdxsv_emu_flycast_init() {
	config::GGPOEnable = false;
	if (gdxsv_video_hidden_window()) {
		if (gui_state == GuiState::Onboarding) throw FlycastException("Four-screen worker has no configuration directory");
		// Join before loading content, so a failed worker cannot become an
		// invisible standalone emulator with no host to close it.
		if (settings.content.path.empty() ||
			!gdxsv_multi_pov_guest_open(config::loadStr("gdxsv", "MultiPovSession", ""), gdxsv_multi_pov_guest_pov()) ||
			gdxsv_multi_pov_host_gone())
			throw FlycastException("Four-screen worker could not join its host");
	}
	// Publish the renderer and UI thread before the command-line loader starts.
	gdxsv_multi_pov_viewer_tick();
}

void gdxsv_emu_flycast_term() { gdxsv.StopNetworkTests(); }

void gdxsv_emu_start() {
	gdxsv.Reset();
	const bool hidden_worker = gdxsv_video_hidden_window();
	if (hidden_worker && !gdxsv.Enabled()) throw FlycastException("Four-screen worker loaded unsupported content");

	if (gdxsv.Enabled()) {
		const auto replay = config::loadStr("gdxsv", "replay", "");
		const auto spectate = config::loadStr("gdxsv", "spectate", "");
		const auto rbk_test = config::loadStr("gdxsv", "rbk_test", "");

		// A 4-player replay guest gets the replay from the host in
		// gdxsv_emu_loadstate; it boots from the same slot-99 savestate.
		const bool multi_pov_guest = 0 <= gdxsv_multi_pov_guest_pov();

		if (!replay.empty() || !spectate.empty() || multi_pov_guest) {
			// Both resume from the shared slot-99 bootstrap savestate;
			// gdxsv_emu_loadstate picks which of the two to start from the
			// same config once the state is loaded.
			if (gdxsv_ensure_replay_savestate(gdxsv.Disk())) {
				// Start every instance from one wall-clock instant. Launch time
				// varies by seconds, and that offset would otherwise show up as
				// drift the sync had no way to remove.
				const int sync_at = config::loadInt("gdxsv", "SyncStartTime", 0);
				if (0 < sync_at) {
					NOTICE_LOG(COMMON, "waiting until SyncStartTime %d to load state", sync_at);
					std::this_thread::sleep_until(std::chrono::system_clock::from_time_t(sync_at));
				}
				if (gdxsv.IsSaveStateAllowed()) {
					dc_savestate(90);
				}
				dc_loadstate(99);
				if (hidden_worker && !gdxsv.IsReplaying() && !g_live_guest_idle)
					throw FlycastException("Four-screen worker could not load the replay savestate");
			} else if (hidden_worker) {
				throw FlycastException("Four-screen worker has no replay savestate");
			}
		} else if (!rbk_test.empty()) {
			if (gdxsv_ensure_replay_savestate(gdxsv.Disk())) {
				dc_loadstate(99);
			}
		} else if (gdxsv_headless() && config::loadBool("gdxsv", "headless_loadstate", false)) {
			// Headless inspection boot: resume from the shared slot-99 lobby
			// savestate (no battle, no online boot) so HookVBlank->WritePatch
			// applies the gdxsv patches within a few frames. A test harness then
			// reads the patched memory from a Lua script (flycast.memory.*)
			// without a long cold boot or any network.
			if (gdxsv_ensure_replay_savestate(gdxsv.Disk())) {
				dc_loadstate(99);
			}
		} else {
			gdxsv.StartPingTest();
			gui_setState(GuiState::GdxsvLatencyCheck);
			gdxsv.FetchPublicIP();
			gdxsv.StartP2PFeasibilityTest();
		}
	}
}

void gdxsv_emu_reset() {
	gdxsv.Reset();
}

void gdxsv_emu_state_restored() {
	if (gdxsv.Enabled()) {
		gdxsv.InvalidateWidescreenPatch();
		gdxsv_multi_pov_audio_reset();
	}
}

// Headless boot probe: after gdxsv:headless_probe_frames vblanks, exit cleanly.
// Lets a test boot the game a fixed number of frames (e.g. far enough for
// HookVBlank->WritePatch to apply the gdxsv patches) and then exit, without
// starting a battle. Reading memory is done from a Lua script in the test
// harness (flycast.memory.read*), so no dump code lives here.
static void gdxsv_headless_probe_tick() {
	if (!gdxsv_headless())
		return;
	static int probe_frames = -1;
	if (probe_frames == -1)
		probe_frames = config::loadInt("gdxsv", "headless_probe_frames", 0);
	if (probe_frames <= 0)
		return;
	static int count = 0;
	if (++count < probe_frames)
		return;
	NOTICE_LOG(COMMON, "headless probe: exiting after %d frames", probe_frames);
	gdxsv_headless_exit(0);
}

void gdxsv_emu_vblank() {
	if (gdxsv.Enabled()) {
		gdxsv.HookVBlank();
	}
	gdxsv_headless_probe_tick();
}

void gdxsv_emu_end_frame() {
	if (gdxsv.Enabled()) {
		gdxsv.HookEndOfFrame();
	}
}

// A guest closes when its host is closed or killed, and in a live session
// moves on to the host's next battle. Must run on the UI thread: dc_exit()
// joins the emulation thread.
static void gdxsv_multi_pov_tick() {
	if (gdxsv_multi_pov_current_role() != GdxsvMultiPovRole::Guest) return;
	// The loader owns the session until its future completes.
	if (gui_state == GuiState::Loading) return;
	const bool failed_worker = gdxsv_video_hidden_window() &&
							   (gui_state == GuiState::Main || gui_state == GuiState::Settings || gui_state == GuiState::Onboarding);
	if (failed_worker || gdxsv_multi_pov_host_gone()) {
		NOTICE_LOG(COMMON, "multi-pov: %s, closing this screen", failed_worker ? "worker cannot continue playback" : "host is gone");
		// The savestate reset lifts memwatch's page protection before the unload
		// writes into guest memory from this thread.
		emu.stop();
		gdxsv_save_state.Reset();
		gdxsv_multi_pov_close();
		dc_exit();
		return;
	}
	if (!gdxsv_multi_pov_is_live()) return;

	std::string battle_code;
	uint32_t generation = 0;
	if (gdxsv_multi_pov_read_live_battle(battle_code, generation) && generation != g_live_guest_generation) {
		NOTICE_LOG(COMMON, "multi-pov: host moved on to %s", battle_code.c_str());
		emu.stop();
		gdxsv_save_state.Reset();
		gdxsv.StopReplay();
		// The slot-99 hook starts the new battle.
		dc_loadstate(99);
		if (!g_live_guest_idle) emu.start();
		return;
	}

	// Between battles: nothing runs, so nothing paces the UI loop either.
	if (g_live_guest_idle && emu.running()) emu.stop();
	if (!emu.running()) std::this_thread::sleep_for(std::chrono::milliseconds(16));
}

void gdxsv_emu_next_frame() {
	if (gdxsv.Enabled()) {
		gdxsv.HookNextFrame();
	}
	// Without threaded rendering, mainui_loop presents after ggpo::nextFrame: a rollback there
	// delays the picture. Present the frame rendered at its start of render now instead; mainui_loop
	// then has nothing left to present. Rollback frames render nothing (renderer disabled).
	if (!config::ThreadedRendering && RollbackBattle() && rend_is_enabled() && imguiDriver != nullptr) {
		try {
			imguiDriver->present();
		} catch (const FlycastException&) {
			// mainui_loop presents again and recreates the context on failure
		}
	}
}

void gdxsv_emu_frame_end() {
	// With threaded rendering, a rollback battle paces the emulation thread, after the local input is sent and the
	// rollback ran; the UI thread renders and presents each queued frame at once.
	if (config::ThreadedRendering && RollbackBattle()) gdxsv_frame_limiter_wait();
}

void gdxsv_emu_mainui_loop(bool rendered) {
	if (gdxsv.Enabled()) {
		gdxsv.HookMainUiLoop();
	}
	// Window calls must happen on the UI thread.
	if (const char* error = gdxsv_multi_pov_viewer_tick(); error && gui_state != GuiState::Loading) {
		emu.stop();
		gdxsv.StopReplay();
		gdxsv_multi_pov_close();
		gdxsv_end_replay(error, /*user_exit=*/true);
	}
	gdxsv_multi_pov_tick();

	if (rendered && !(config::ThreadedRendering && RollbackBattle()))
		gdxsv_frame_limiter_wait();
}

void gdxsv_emu_rpc() {
	if (gdxsv.Enabled()) {
		gdxsv.HandleRPC();
	}
}

void gdxsv_emu_savestate(int slot) {
	if (gdxsv.Enabled()) {
		gdxsv.RestoreOnlinePatch();
	}
}

void gdxsv_emu_loadstate(int slot) {
	if (gdxsv.Enabled()) {
		auto replay = config::loadStr("gdxsv", "replay", "");
		auto spectate = config::loadStr("gdxsv", "spectate", "");

		// One backend, one source: replay wins.
		if (!replay.empty() && !spectate.empty()) {
			NOTICE_LOG(COMMON, "gdxsv:replay and gdxsv:spectate are both set; ignoring spectate=%s", spectate.c_str());
			spectate.clear();
		}

		if (!replay.empty() && slot == 99) {
			auto replay_pov = config::loadInt("gdxsv", "ReplayPOV", 1);
			// Command-line four-screen playback is a one-shot request, not a
			// saved preference or an override of the browser's replay buttons.
			if (gdxsv_multi_pov_take_four_screen_request() && !gdxsv_headless()) {
				std::vector<uint8_t> hosted;
				if (gdxsv_multi_pov_begin_host_session(replay, hosted)) {
					if (gdxsv.StartReplayBuffer(hosted, 0)) {
						NOTICE_LOG(COMMON, "multi-pov: hosting %s from the command line", replay.c_str());
						return;
					}
					ERROR_LOG(COMMON, "multi-pov: could not start the hosted replay; closing the session");
					gdxsv_multi_pov_close();
				}
			}
			if (!gdxsv.StartReplayFile(replay.c_str(), replay_pov - 1) && gdxsv_headless()) {
				// The replay could not be loaded (missing, corrupt, or an
				// unreadable old format): fail the headless run instead of
				// dropping into a GUI that will never be shown.
				ERROR_LOG(COMMON, "headless replay failed to start: %s", replay.c_str());
				gdxsv_headless_exit(3);
			}
		}

		if (!spectate.empty() && slot == 99) {
			// One shot, like the four-screen request: Live autoplay loads
			// slot 99 again for every battle it moves on to.
			if (config::isTransient("gdxsv", "spectate")) config::setTransient("gdxsv", "spectate", "");
			auto spectate_pov = config::loadInt("gdxsv", "ReplayPOV", 1);
			if (gdxsv_multi_pov_take_four_screen_request() && !gdxsv_headless()) {
				if (gdxsv_multi_pov_begin_live_host_session(spectate)) {
					if (gdxsv.StartLiveSpectate(spectate.c_str(), 0)) {
						NOTICE_LOG(COMMON, "multi-pov: hosting live %s from the command line", spectate.c_str());
						gdxsv_live_autoplay_begin(spectate, 0, true);
						return;
					}
					ERROR_LOG(COMMON, "multi-pov: could not start the hosted live battle; closing the session");
					gdxsv_multi_pov_close();
				}
			}
			if (gdxsv.StartLiveSpectate(spectate.c_str(), spectate_pov - 1))
				gdxsv_live_autoplay_begin(spectate, spectate_pov - 1, false);
		}

		// 4-player replay guest: the replay, or the live battle, comes from the host.
		const int multi_pov = gdxsv_multi_pov_guest_pov();
		if (0 <= multi_pov && slot == 99) {
			GdxsvMultiPovGuestStart start;
			const bool joined = gdxsv_multi_pov_begin_guest_session(start);
			if (joined && !start.live_battle_code.empty()) {
				// A live guest that cannot start this battle waits for the next.
				g_live_guest_generation = start.live_generation;
				g_live_guest_idle = !gdxsv.StartLiveFromHost(multi_pov);
				if (g_live_guest_idle) {
					WARN_LOG(COMMON, "multi-pov: guest %dP cannot watch %s; waiting for the next battle", multi_pov + 1,
							 start.live_battle_code.c_str());
					gdxsv_multi_pov_skip_start_barrier();
				} else {
					NOTICE_LOG(COMMON, "multi-pov: guest %dP watching %s live", multi_pov + 1, start.live_battle_code.c_str());
				}
			} else if (joined && gdxsv.StartReplayBuffer(start.replay, multi_pov)) {
				NOTICE_LOG(COMMON, "multi-pov: guest %dP playing from the host's replay", multi_pov + 1);
			} else {
				ERROR_LOG(COMMON, "multi-pov: guest %dP could not start; closing this screen", multi_pov + 1);
				gdxsv_multi_pov_close();
				dc_exit();
			}
		}

		auto rbk_test = config::loadStr("gdxsv", "rbk_test", "");
		if (!rbk_test.empty() && slot == 99) {
			gdxsv.StartRollbackTest(rbk_test.c_str());
		}
	}
}

bool gdxsv_emu_menu_open() {
	if (gdxsv.Enabled()) {
		return gdxsv.HookOpenMenu();
	}
	return true;
}

static void gui_header(const char* title) {
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ScaledVec2(0.f, 0.5f));	// Left
	ImGui::ButtonEx(title, ScaledVec2(-1, 0), ImGuiItemFlags_Disabled);
	ImGui::PopStyleVar();
}

void gdxsv_emu_gui_display_commands(float buttonWidth) {
	if (!gdxsv_enabled()) return;
	DisabledScope scope(gdxsv_is_online());
	if (ImGui::Button(ICON_FA_TV "  Live & Replays", ScaledVec2(buttonWidth, 50)) && !scope.isDisabled()) {
		gui_state = GuiState::GdxsvReplay;
	}
	if (gdxsv_is_online() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("Please disconnect from the Gdxsv server first.");
	}
}

std::string gdxsv_connection_medium_or_setting() {
	const std::string medium = config::loadStr("gdxsv", "ConnectionMedium", "");
	return medium.empty() ? gdxsv_connection_medium() : medium;
}

void gdxsv_emu_gui_display() {
	// A failed worker must not start launcher dialogs or update requests
	// whose pending futures could keep the hidden process alive at exit.
	if (gdxsv_video_hidden_window()) return;
	gdxsv_multi_pov_viewer_draw();
	if (gui_state == GuiState::Main) {
		gdxsv_update_popup();
		gdxsv_texture_update_popup();
		
		static std::string connection_medium = gdxsv_connection_medium_or_setting();
		wireless_warning_popup(connection_medium);
		vpn_warning_toast(connection_medium);
	}

	if (gui_state == GuiState::GdxsvReplay) {
		gdxsv_replay_select_dialog();
	}
	
	if (gui_state == GuiState::GdxsvLatencyCheck) {
		drawBoxartBackground();
		centerNextWindow();
		ImGui::SetNextWindowSize(ScaledVec2(330, 0));
		ImGui::SetNextWindowBgAlpha(0.8f);
		ImguiStyleVar _(ImGuiStyleVar_WindowPadding, ScaledVec2(20, 20));
		
		if (ImGui::Begin("##latencyCheck", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(20, 10));
			ImGui::AlignTextToFramePadding();
			ImGui::SetCursorPosX(uiScaled(20.f));
			
			ImGui::Text("%s", gdxsv.PingResult().c_str());
		}
		ImGui::End();
		
		if (gdxsv.PingResult() == "Done") {
			// A cached result can be ready before automatic state loading ends.
			// Let the normal loader finish before it closes the UI and starts emulation.
			gui_setState(GuiState::Loading);
		}
		
		p2p_connection_toast();
	}
}

void gdxsv_emu_settings_gdxsv_tab() { gdxsv_gui_settings_tab(); }

void gdxsv_emu_settings_video() { gdxsv_gui_settings_video(); }

void gdxsv_emu_apply_base_settings() {
	gdxsv_apply_base_settings();
	// Into the new config file: loading a game resets the options to what the file holds.
	config::Settings::instance().save();
}

const char* gdxsv_emu_settings_text_for_preparing_font() { return gdxsv_gui_settings_text_for_preparing_font(); }

uint32_t gdxsv_emu_skip_rendering_addr() {
	return config::GdxSkipRenderingHack ? settings.gdxsv.skipRenderingBaseAddr : 0;
}

void gdxsv_gui_display_osd() {
	gdxsv_multi_pov_viewer_draw();
	gdxsv.DisplayOSD();
	p2p_connection_toast();
}

void gdxsv_crash_append_log(FILE* f) {
	if (gdxsv.Enabled()) {
		fprintf(f, "[gdxsv]disk: %d\n", gdxsv.Disk());
		fprintf(f, "[gdxsv]user_id: %s\n", gdxsv.UserId().c_str());
		fprintf(f, "[gdxsv]netmode: %s\n", gdxsv.NetModeString());
	}
}

static bool trim_prefix(const std::string& s, const std::string& prefix, std::string& out) {
	auto size = prefix.size();
	if (s.size() < size) return false;
	if (std::equal(std::begin(prefix), std::end(prefix), std::begin(s))) {
		out = s.substr(prefix.size());
		return true;
	}
	return false;
}

void gdxsv_crash_append_tag(const std::string& logfile, std::vector<http::PostField>& post_fields) {
	if (file_exists(logfile)) {
		nowide::ifstream ifs(logfile);
		if (ifs.is_open()) {
			std::string line;
			std::string f_disk, f_user_id, f_netmode;

			while (std::getline(ifs, line)) {
				trim_prefix(line, "[gdxsv]disk: ", f_disk);
				trim_prefix(line, "[gdxsv]user_id: ", f_user_id);
				trim_prefix(line, "[gdxsv]netmode: ", f_netmode);
			}

			if (!f_disk.empty()) post_fields.emplace_back("sentry[tags][disk]", f_disk);
			if (!f_user_id.empty()) post_fields.emplace_back("sentry[tags][user_id]", f_user_id);
			if (!f_netmode.empty()) post_fields.emplace_back("sentry[tags][netmode]", f_netmode);
		}
	}

	const std::string machine_id = gdxsv_machine_id();
	if (machine_id.length()) {
		const auto digest = XXH64(machine_id.c_str(), machine_id.size(), 37);
		std::stringstream ss;
		ss.imbue(std::locale::classic());
		ss << std::hex << digest;
		post_fields.emplace_back("sentry[tags][machine_id]", ss.str().c_str());
	}
}

static void textCentered(const std::string& text) {
	const auto windowWidth = ImGui::GetWindowSize().x;
	const auto textWidth = ImGui::CalcTextSize(text.c_str()).x;
	ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
	ImGui::Text(text.c_str());
}

static void textCentered(const ImVec4& color, const std::string& text) {
	const auto windowWidth = ImGui::GetWindowSize().x;
	const auto textWidth = ImGui::CalcTextSize(text.c_str()).x;
	ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
	ImGui::TextColored(color, text.c_str());
}

static void gdxsv_update_popup() {
	static bool update_popup_shown = false;
	static std::shared_future<bool> self_update_result;
	bool no_popup_opened = !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);

	if (gdxsv_update.IsUpdateAvailable() && !update_popup_shown && no_popup_opened) {
		ImGui::OpenPopup("New version");
	}

	if (ImGui::BeginPopupModal("New version", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
		ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + uiScaled(400.f));
		ImGui::TextWrapped("  %s is available for download!  ", gdxsv_update.GetLatestVersionTag().c_str());
		ImGui::PopTextWrapPos();
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(16.f, 3.f));
		float currentwidth = ImGui::GetContentRegionAvail().x;
		const bool app_translocated = GdxsvUpdate::IsAppTranslocated();
		if (app_translocated) {
			ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + uiScaled(400.f));
			ImGui::TextWrapped("Please move Flycast-gdxsv.app to the /Applications folder, then reopen it to use the auto updater. Or download it and update manually.");
			ImGui::PopTextWrapPos();
			ImGui::SetCursorPosX((currentwidth - uiScaled(100.f)) / 2.f + ImGui::GetStyle().WindowPadding.x - uiScaled(55.f));
			if (ImGui::Button("Download", ScaledVec2(100.f, 0.f))) {
				gdxsv_open_url(GdxsvUpdate::DownloadPageURL());
				update_popup_shown = true;
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();
			ImGui::SetCursorPosX((currentwidth - uiScaled(100.f)) / 2.f + ImGui::GetStyle().WindowPadding.x + uiScaled(55.f));
			if (ImGui::Button("Cancel", ScaledVec2(100.f, 0.f))) {
				update_popup_shown = true;
				ImGui::CloseCurrentPopup();
			}
		} else {
			ImGui::SetCursorPosX((currentwidth - uiScaled(100.f)) / 2.f + ImGui::GetStyle().WindowPadding.x - uiScaled(55.f));
			if (GdxsvUpdate::IsSupportSelfUpdate()) {
				if (ImGui::Button("Update", ScaledVec2(100.f, 0.f))) {
					self_update_result = gdxsv_update.StartSelfUpdate();
					update_popup_shown = true;
					ImGui::CloseCurrentPopup();
				}
			} else {
				if (ImGui::Button("Download", ScaledVec2(100.f, 0.f))) {
					gdxsv_open_url(GdxsvUpdate::DownloadPageURL());
					update_popup_shown = true;
					ImGui::CloseCurrentPopup();
				}
			}

			ImGui::SameLine();
			ImGui::SetCursorPosX((currentwidth - uiScaled(100.f)) / 2.f + ImGui::GetStyle().WindowPadding.x + uiScaled(55.f));
			if (ImGui::Button("Cancel", ScaledVec2(100.f, 0.f))) {
				update_popup_shown = true;
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::SetItemDefaultFocus();
		ImGui::PopStyleVar();
		ImGui::EndPopup();
	}

	if (self_update_result.valid() && no_popup_opened) {
		ImGui::OpenPopup("Update");
	}

	ImGui::SetNextWindowSize(ScaledVec2(340, 0));
	centerNextWindow();
	ImVec2 padding = ScaledVec2(20, 20);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, padding);
	if (ImGui::BeginPopupModal("Update", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(16.f, 3.f));

		if (self_update_result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
			if (self_update_result.get()) {
				textCentered(ImVec4(0, 0.8, 0, 1), "Download Completed");
				textCentered("Please restart the emulator");

				if (ImGui::Button("Exit", ScaledVec2(300, 30))) {
					self_update_result = {};
					ImGui::CloseCurrentPopup();
					dc_exit();
				}
			} else {
				textCentered(ImVec4(0.8, 0, 0, 1), "Download Failed");
				textCentered("Please download the latest version manually");

				if (ImGui::Button("Download", ScaledVec2(300, 30))) {
					self_update_result = {};
					gdxsv_open_url(GdxsvUpdate::DownloadPageURL());
					ImGui::CloseCurrentPopup();
				}
			}
		} else {
			ImGui::Text("Updating...");
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.557f, 0.268f, 0.965f, 1.f));
			ImGui::ProgressBar(gdxsv_update.SelfUpdateProgress(), ScaledVec2(-1, 20.f));
			ImGui::PopStyleColor();
		}
		ImGui::SetItemDefaultFocus();
		ImGui::PopStyleVar();
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar(2);
}

static void gdxsv_texture_update_popup() {
	static bool update_triggered = false;
	static std::shared_future<bool> self_update_result;
	bool no_popup_opened = !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);

	if (!update_triggered && no_popup_opened && gdxsv_custom_texture_update.IsUpdateAvailable()) {
		self_update_result = gdxsv_custom_texture_update.StartUpdate();
		update_triggered = true;
	}

	if (self_update_result.valid() && no_popup_opened) {
		ImGui::OpenPopup("Updating texture");
	}

	ImGui::SetNextWindowSize(ScaledVec2(340, 0));
	centerNextWindow();
	ImVec2 padding = ScaledVec2(20, 20);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, padding);
	if (ImGui::BeginPopupModal("Updating texture", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(16.f, 3.f));

		if (self_update_result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
			if (self_update_result.get()) {
				textCentered(ImVec4(0, 0.8, 0, 1), "Update Completed");
				if (ImGui::Button("Continue", ScaledVec2(300, 30))) {
					self_update_result = {};
					ImGui::CloseCurrentPopup();
				}
			} else {
				textCentered(ImVec4(0.8, 0, 0, 1), "Update Failed");
			}
		} else {
			ImGui::Text("Updating...");
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.557f, 0.268f, 0.965f, 1.f));
			ImGui::ProgressBar(gdxsv_custom_texture_update.UpdateProgress(), ScaledVec2(-1, 20.f));
			ImGui::PopStyleColor();
		}

		ImGui::SetItemDefaultFocus();
		ImGui::PopStyleVar();
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar(2);
}

static void wireless_warning_popup(const std::string& connection_medium) {
	static bool initialized = false;
	const bool no_popup_opened = !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);

	if (!initialized && no_popup_opened && connection_medium == "Wireless") {
		ImGui::OpenPopup("Wireless connection detected");
		NOTICE_LOG(COMMON, "wireless warning shown");
		initialized = true;
	}

	if (ImGui::BeginPopupModal("Wireless connection detected", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
		ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + uiScaled(400.f));
		ImGui::TextWrapped("  Please use LAN cable for the best gameplay experience!  ");
		ImGui::PopTextWrapPos();
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(16.f, 3.f));
		float currentwidth = ImGui::GetContentRegionAvail().x;

		ImGui::SetCursorPosX((currentwidth - uiScaled(100.f)) / 2.f + ImGui::GetStyle().WindowPadding.x);
		if (ImGui::Button("OK", ScaledVec2(100.f, 0.f))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::SetItemDefaultFocus();
		ImGui::PopStyleVar();
		ImGui::EndPopup();
	}
}

static void vpn_warning_toast(const std::string& connection_medium) {
	struct Toast
	{
		bool active = false;
		bool shown = false;
		float timer = 6.0f;
		u32 last_frame_id = 0;
	};
	static Toast vpnToast;
	
	static bool initialized = false;
	if (!initialized) {
		vpnToast.active = connection_medium.find("VPN") == 0;
		if (vpnToast.active) NOTICE_LOG(COMMON, "VPN warning shown: %s", connection_medium.c_str());
		initialized = true;
	}
	
	static float anim = 0.f;
	
	u32 current_frame_id = (u32)ImGui::GetFrameCount();
	bool should_update = (current_frame_id != vpnToast.last_frame_id);
	vpnToast.last_frame_id = current_frame_id;

	float dt = should_update ? ImGui::GetIO().DeltaTime : 0.f;

	if (vpnToast.active && !vpnToast.shown)
	{
		if (vpnToast.timer > 0.f)
		{
			vpnToast.timer -= dt;
			anim = ImMin(anim + dt * 3.0f, 1.0f);
		}
		else
		{
			anim = ImMax(anim - dt * 3.0f, 0.0f);
			if (anim <= 0.f)
				vpnToast.shown = true;
		}
	}

	if (anim > 0.f)
	{
		ImGui::SetNextWindowBgAlpha(0.8f * anim);
		float margin = uiScaled(24.0f);
		
		float slide = (vpnToast.timer > 0.f) ? ImLerp(-(uiScaled(460.0f) + margin), 0.0f, anim) : 0.0f;
		ImGuiViewport* vp = ImGui::GetMainViewport();

		ImVec2 pos;
		pos.x = vp->WorkPos.x + margin + slide;
		pos.y = vp->WorkPos.y + vp->WorkSize.y - margin;

		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, anim);
		ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.0f, 1.0f));
		ImGui::SetNextWindowSizeConstraints(ScaledVec2(460.f, 0.f), ImVec2(FLT_MAX, FLT_MAX));

		ImGuiWindowFlags flags =
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_Tooltip;

		if (ImGui::Begin("##VPNToast", nullptr, flags))
		{
			std::string vpn_name = "";
			if (connection_medium.find("VPN: ") == 0)
				vpn_name = " - " + connection_medium.substr(5);
			
			ImGui::Text("Possible VPN / virtual network detected %s", vpn_name.c_str());
			ImGui::Separator();
			ImGui::TextWrapped(
				"Your connection appears to use a VPN or virtual network interface.\n"
				"If matches feel unstable, try a direct connection."
			);
		}
		ImGui::End();
		ImGui::PopStyleVar();
	}
}

static void p2p_connection_toast() {
	struct Toast {
		bool shown = false;
		float timer = 0.0f;
		P2PFeasibility result;
		u32 last_frame_id = 0;
		bool test_finished = false;
	};
	static Toast p2pToast;
	static float anim = 0.f;
	auto future = gdxsv.P2PFeasibilityResult();
	
	if (p2pToast.shown || !future.valid())
		return;
	
	// Prevent double-updating logic if called from both GUI and OSD in the same frame
	u32 current_frame_id = (u32)ImGui::GetFrameCount();
	bool should_update = (current_frame_id != p2pToast.last_frame_id);
	p2pToast.last_frame_id = current_frame_id;

	float dt = should_update ? ImGui::GetIO().DeltaTime : 0.f;

	if (!p2pToast.shown)
	{
		if (p2pToast.timer > 0.f)
		{
			p2pToast.timer -= dt;
			anim = ImMin(anim + dt * 3.0f, 1.0f);
		}
		else if (p2pToast.test_finished)
		{
			anim = ImMax(anim - dt * 3.0f, 0.0f);
			if (anim <= 0.f)
				p2pToast.shown = true;
		}

		if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
			if (!p2pToast.test_finished) {
				p2pToast.result = future.get();
				p2pToast.test_finished = true;
				
				// Only start displaying the toast if there are connection issues
				if (p2pToast.result.status_code == P2PStatus::Poor || p2pToast.result.status_code == P2PStatus::Blocked) {
					p2pToast.timer = 6.0f; 
				} else {
					p2pToast.shown = true;
				}
			}
		}
	}
	if (anim > 0.f)
	{
		ImGui::SetNextWindowBgAlpha(0.8f * anim);
		float margin = uiScaled(24.0f);
		
		float slide = (p2pToast.timer > 0.f) ? ImLerp(-(uiScaled(300.0f) + margin), 0.0f, anim) : 0.0f;
		ImGuiViewport* vp = ImGui::GetMainViewport();

		ImVec2 pos;
		pos.x = vp->WorkPos.x + margin + slide;
		pos.y = vp->WorkPos.y + vp->WorkSize.y - margin;
		
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, anim);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ScaledVec2(10.0f, 10.0f));
		ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.0f, 1.0f));
		ImGui::SetNextWindowSizeConstraints(ScaledVec2(300.f, 0.f), ImVec2(FLT_MAX, FLT_MAX));

		ImGuiWindowFlags flags =
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_Tooltip;

		if (ImGui::Begin("##P2PToast", nullptr, flags))
		{
			ImGui::Text("P2P Connectivity Test");
			if (p2pToast.test_finished) {
				ImGui::SameLine();
				ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(p2pToast.result.color), "%s", p2pToast.result.status.c_str());
			}
			ImGui::Separator();
			ImGui::TextWrapped("%s", p2pToast.result.description.c_str());
		}
		ImGui::End();
		ImGui::PopStyleVar(2);
	}
}

bool gdxsv_is_using_memwatch() { return gdxsv.Enabled() && gdxsv_save_state.Enabled(); }

bool gdxsv_headless() {
	return config::loadBool("gdxsv", "headless", false);
}

static int gdxsv_exit_code_ = 0;

void gdxsv_set_exit_code(int code) {
	gdxsv_exit_code_ = code;
}

int gdxsv_exit_code() {
	return gdxsv_exit_code_;
}

void gdxsv_headless_exit(int code) {
	fflush(nullptr);
	std::_Exit(code);
}

int gdxsv_video_max_frame_latency() { return std::clamp(config::GdxMaxFrameLatency.get(), 0, 16); }

bool gdxsv_video_rollback_battle() { return RollbackBattle(); }
