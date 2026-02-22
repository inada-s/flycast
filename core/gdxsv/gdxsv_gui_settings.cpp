#include "gdxsv_gui_settings.h"

#include "gdxsv.h"
#include "gdxsv_network.h"
#include "gdxsv_custom_texture_update.h"
#include "hw/maple/maple_if.h"
#include "imgui.h"
#include "libs.h"
#include "oslib/i18n.h"
#include "ui/gui_util.h"

using namespace i18n;

inline static void header(const char* title) {
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f));	// Left
	ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
	ImGui::BeginDisabled();
	ImGui::ButtonEx(title, ImVec2(-1, 0));
	ImGui::EndDisabled();
	ImGui::PopStyleVar();
	ImGui::PopStyleVar();
}

void gdxsv_apply_base_settings() {
	// Frame Limit
	config::LimitFPS = true;
	config::FixedFrequency = 2;

	// Controls
	config::MapleMainDevices[0].set(MapleDeviceType::MDT_SegaController);
	config::MapleExpansionDevices[0][0].set(MDT_SegaVMU);
	config::Sh4Clock = 200;

	// Video
	config::PerStripSorting = false;
	config::DelayFrameSwapping = false;
#if defined(_WIN32)
	config::RendererType.set(RenderType::DirectX11);
#else
	config::RendererType.set(RenderType::OpenGL);
#endif
	config::RenderResolution = 720;
	config::SkipFrame = 0;
	config::AutoSkipFrame = 0;
	config::TextureUpscale = 1;

	// Audio
	config::DSPEnabled = false;
	config::AudioVolume.set(50);
	config::AudioVolume.calcDbPower();
	config::AudioBufferSize = 706 * 4;

	// Others
	config::DynarecEnabled = true;

	// Network
	config::EnableUPnP = true;
	if (config::GdxLocalPort == 0) {
		config::GdxLocalPort = get_random_port_number();
	}
	config::GdxMinDelay = 2;

	maple_ReconnectDevices();
}

// clang-format off
void gdxsv_gui_settings_tab()
{
	header("gdxsv Settings");

	if (config::PerStripSorting.get()) {
		ImGui::TextColored(ImVec4(0.8f, 0.1f, 0.1f, 1),
			"WARNING: Transparent Sorting is not Per Triangle. Per Triangle is strongly recommended.");
		ImGui::SameLine();
		if (ImGui::Button("Set Per Triangle")) {
			config::PerStripSorting = false;
		}
	}

	ImGui::Columns(5, "gdxlang", false);
	ImGui::Text("Language mod:");
	ImGui::SameLine();
	ShowHelpMarker(T("Patch game language and texture, for DX only"));
	ImGui::NextColumn();
	OptionRadioButton(u8"日本語", config::GdxLanguage, 0);
	ImGui::NextColumn();
	OptionRadioButton(u8"廣東話", config::GdxLanguage, 1);
	ImGui::NextColumn();
	OptionRadioButton("English", config::GdxLanguage, 2);
	ImGui::NextColumn();
	OptionRadioButton("Disabled", config::GdxLanguage, 3);
	ImGui::Columns(1, nullptr, false);

	auto setting = [](const char* name, const char* value) -> std::string {
		return std::string(T(name)) + " = " + value + "\n";
	};

	auto settings_to_be_changed = [&setting]() -> std::string {
		std::string str = std::string(T("Settings to be changed:")) + "\n\n";
		// Frame Limit
		if (config::LimitFPS == false)
			str += setting("AudioSync", "true");
		if (config::FixedFrequency != 2)
			str += setting("Fixed frequency", "59.94 Hz");

		// Controls
		if (config::MapleMainDevices[0] != MapleDeviceType::MDT_SegaController)
			str += setting("Dreamcast Device A", "Sega Controller");
		if (config::MapleExpansionDevices[0][0] != MapleDeviceType::MDT_SegaVMU)
			str += setting("Dreamcast Device A Slot 1", "Sega VMU");
		if (config::Sh4Clock != 200)
			str += setting("SH4 Clock", "200 Mhz");

		// Video
		if (config::PerStripSorting)
			str += setting("Transparent Sorting", "Per Triangle");
		if (config::DelayFrameSwapping)
			str += setting("Delay Frame Swapping", "false");
#if defined(_WIN32)
		if (config::RendererType != RenderType::DirectX11)
			str += setting("Graphics API", "DirectX 11");
#else
		if (config::RendererType != RenderType::OpenGL)
			str += setting("Graphics API", "Open GL");
#endif

		if (config::SkipFrame != 0)
			str += setting("Frame Skipping", "0");

		// Audio
		if (config::DSPEnabled != false)
			str += setting("Enable DSP", "false");
		if (config::AudioVolume != 50)
			str += setting("Volume Level", "50");
		if (config::AudioBufferSize < 2822 || config::AudioBufferSize > 2824)
			str += setting("Audio Latency", "64ms");

		// Others
		if (config::DynarecEnabled != true)
			str += setting("CPU Mode", "Dynarec");

		// Network
		if (config::EnableUPnP != true)
			str += setting("Enable UPnP", "true");
		if (config::GdxLocalPort == 0)
			str += std::string(T("Set a random Gdx UDP Port")) + "\n";

		return str;
	};

	if (ImGui::Button(T("Apply Recommended Settings\nfor Low-Spec PC"), ScaledVec2(200, 50))) {
		gdxsv_apply_base_settings();
		config::ThreadedRendering = true;
		config::GdxMinDelay = 3;
		config::AutoSkipFrame = 1;
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
	{
		auto tip = settings_to_be_changed();
		if (config::ThreadedRendering != true)
			tip += setting("Multi-threaded emulation", "true");
		if (config::RenderResolution != 720)
			tip += setting("Internal Resolution", "1280x720 (x1.5)");
		if (config::AutoSkipFrame != 1)
			tip += setting("Automatic Frame Skipping", "Normal");
		if (config::TextureUpscale != 1)
			tip += setting("Texture Upscaling", "1");
		if (config::GdxMinDelay != 3)
			tip += setting("Gdx Minimum Delay", "3");

		ImGui::SetTooltip("%s", tip.c_str());
	}

	ImGui::SameLine();
	if (ImGui::Button(T("Apply Recommended Settings\nfor Mid-Spec PC"), ScaledVec2(200, 50))) {
		gdxsv_apply_base_settings();
		config::ThreadedRendering = false;
		config::RenderResolution = 960;
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
	{
		auto tip = settings_to_be_changed();
		if (config::ThreadedRendering != false)
			tip += setting("Multi-threaded emulation", "false");
		if (config::RenderResolution != 960)
			tip += setting("Internal Resolution", "1706x960 (x2)");
		if (config::AutoSkipFrame != 0)
			tip += setting("Automatic Frame Skipping", "Disabled");
		if (config::TextureUpscale != 1)
			tip += setting("Texture Upscaling", "1");
		if (config::GdxMinDelay != 2)
			tip += setting("Gdx Minimum Delay", "2");

		ImGui::SetTooltip("%s", tip.c_str());
	}

	ImGui::SameLine();
	if (ImGui::Button(T("Apply Recommended Settings\nfor High-Spec PC"), ScaledVec2(200, 50))) {
		gdxsv_apply_base_settings();
		config::ThreadedRendering = false;
		config::RenderResolution = 1440;
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
	{
		auto tip = settings_to_be_changed();
		if (config::ThreadedRendering != false)
			tip += setting("Multi-threaded emulation", "false");
		if (config::RenderResolution != 1440)
			tip += setting("Internal Resolution", "2560x1440 (x3)");
		if (config::AutoSkipFrame != 0)
			tip += setting("Automatic Frame Skipping", "Disabled");
		if (config::GdxMinDelay != 2)
			tip += setting("Gdx Minimum Delay", "2");

		ImGui::SetTooltip("%s", tip.c_str());
	}

	ImGui::SameLine();
	ShowHelpMarker(T("Use gdxsv recommended settings. Settings on other tabs will also be modified.\n\n"
		"Low-Spec:\nPrioritizes stability of gameplay by minimal load.\n\n"
		"Mid-Spec:\nWell-balanced setting that allows for comfortable gameplay with medium load.\n\n"
		"High-Spec:\nAllows for comfortable gameplay by utilizing high load options."));

	bool pressed = OptionCheckbox(T("Use Texture Pack"), config::GdxUseTexturePack,
		T("Use up-scaled textures."));
	if (pressed) {
		gdxsv_custom_texture_update.Reset();
	}

	OptionCheckbox(T("Multi-threaded emulation"), config::ThreadedRendering,
		T("Run the emulated CPU and GPU on different threads.\n"
		"Can reduce the loading, but it may introduce a delay of 1 frame for input.\n"
		"\tEnable = Best for low spec CPU.\n"
		"\tDisable = Best for high spec CPU."));

#ifdef _WIN32
	OptionCheckbox(T("Joystick Polling"), config::JoystickPolling,
		T("Use polling instead of events for joystick input. May improve input latency in multi-threaded mode."));
#endif

	bool widescreen = config::Widescreen.get() && config::WidescreenGameHacks.get();
	pressed = ImGui::Checkbox(T("Enable 16:9 Widescreen Hack"), &widescreen);
	if (pressed) {
		config::Widescreen.set(widescreen);
		config::SuperWidescreen.set(widescreen);
		config::WidescreenGameHacks.set(widescreen);
	}
	ImGui::SameLine();
	ShowHelpMarker(T("Use the following rendering options:\n"
		"    rend.WideScreen=true\n"
		"    rend.SuperWideScreen=true\n"
		"    rend.WidescreenGameHacks=true"));

	OptionCheckbox(T("VSync"), config::VSync,
		T("Limit frame rate by VSync. Minimize video glitch"));
	ImGui::Indent();
	{
		DisabledScope scope(!config::VSync);

		OptionCheckbox(T("Duplicate frames"), config::DupeFrames,
			T("Duplicate frames on high refresh rate monitors (120 Hz and higher)"));
	}
	ImGui::Unindent();

	header(T("Network Settings"));
	OptionCheckbox(T("Enable UPnP"), config::EnableUPnP,
		T("Automatically configure your network router for netplay (IPv4 Only)"));

	ImGui::InputInt(T("Gdx UDP Port"), &config::GdxLocalPort.get());
	ImGui::SameLine();
	ShowHelpMarker(T("UDP port number used for P2P communication. Cannot use the same number as another application using."));

	if (config::GdxLocalPort == 0) {
		config::GdxLocalPort = get_random_port_number();
	}

	static std::string upnp_result;
	static std::future<std::string> upnp_future;
	if (upnp_future.valid() && upnp_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
		upnp_result = upnp_future.get();
	}

	const auto buttonWidth = 200;
	if (ImGui::Button(T("UPnP Now"), ScaledVec2(buttonWidth, 0)) && !upnp_future.valid()) {
		upnp_result = "Please wait...";
		int port = config::GdxLocalPort;
		upnp_future = std::async(std::launch::async, [port]() -> std::string {
			auto& upnp = gdxsv.UPnP();
			std::string result = upnp.Init() && upnp.AddPortMapping(port, false) ? "Success" : upnp.getLastError();
			return result;
			});
	}
	ImGui::SameLine();
	ShowHelpMarker(T("Open the port using UPnP"));
	ImGui::SameLine();
	ImGui::Text("%s", upnp_result.c_str());

	static std::string v4_result;
	static std::future<std::string> v4_future;
	if (future_is_ready(v4_future)) {
		v4_result = v4_future.get();
	}
	static P2PFeasibility p2p_feasibility;
	static std::future<P2PFeasibility> p2p_future;
	if (future_is_ready(p2p_future)) {
		p2p_feasibility = p2p_future.get();
	} else if (p2p_future.valid()) {
		p2p_feasibility.description = gdxsv.P2PStatus();
	}

	if (ImGui::Button(T("Test Connectivity"), ScaledVec2(buttonWidth, 0)) && !p2p_future.valid()) {
		p2p_feasibility = { P2PStatus::Testing, "Testing...", "Running diagnostics...", "", "", 0xFFFFFFFF };
		p2p_future = test_p2p_feasibility(config::GdxLocalPort);
	}
	ImGui::SameLine();
	ShowHelpMarker(T("Analyzes your network for P2P play:\n\n"
		"OK (Direct): Ideal. Port Forwarding, UPnP, or Full Cone NAT detected.\n"
		"OK (Hole Punching): Good. Moderate NAT detected. Hole Punching supported.\n"
		"Limited (May use Relay): Restricted. Symmetric NAT detected. May require a relay peer to connect."));
	if (!p2p_feasibility.status.empty()) {
		ImGui::SameLine();
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(p2p_feasibility.color), "%s ", p2p_feasibility.status.c_str());
		ImGui::SameLine();
		ImGui::Text("%s", p2p_feasibility.description.c_str());
	}

	OptionArrowButtons(
		T("Gdx Minimum Delay"), config::GdxMinDelay, 2, 6,
		T("Minimum frame of input delay used for rollback communication.\n"
		"Smaller value reduces latency, but uses more CPU and introduces glitches."));

	OptionCheckbox(T("Save Replay"), config::GdxSaveReplay,
		T("Save replay file to replays directory"));
	{
		DisabledScope scope(!config::GdxSaveReplay);
		ImGui::SameLine();
		OptionCheckbox(T("Upload Replay"), config::GdxUploadReplay,
			T("Automatically upload the replay file after save"));
	}

	OptionCheckbox(T("Display Network Statistics"), config::NetworkStats,
		T("Display network statistics on screen by default.\n"
		"Use Flycast Menu button to show/hide."));
}
// clang-format on

const char* gdxsv_gui_settings_text_for_preparing_font()
{
	return u8"檔嘅黑吓增開另幫你←↑→↓↖↗↘↙";
}
