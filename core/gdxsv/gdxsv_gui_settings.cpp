#include "gdxsv_gui_settings.h"

#include "gdxsv.h"
#include "gdxsv_network.h"
#include "hw/maple/maple_if.h"
#include "imgui.h"
#include "input/gamepad_device.h"
#include "libs.h"
#include "rend/gui_util.h"

inline static void header(const char* title) {
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f));	// Left
	ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
	ImGui::BeginDisabled();
	ImGui::ButtonEx(title, ImVec2(-1, 0));
	ImGui::EndDisabled();
	ImGui::PopStyleVar();
	ImGui::PopStyleVar();
}

// Switch setting text
// Index 0: English
// Index 1: Japanese
// Index 2: Cantonese
static const char* t(std::initializer_list<const char*> texts) {
	verify(0 < texts.size());

	unsigned index = 0;
	if (config::GdxLanguage == 0) index = 1;
	if (config::GdxLanguage == 1) index = 2;
	if (texts.size() <= index) index = 0;
	return *(texts.begin() + index);
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
	ShowHelpMarker(t({ "Patch game language and texture, for DX only" , u8"ゲーム内の文字列やテクスチャに変更を加えます"}));
	ImGui::NextColumn();
	OptionRadioButton(u8"日本語", config::GdxLanguage, 0);
	ImGui::NextColumn();
	OptionRadioButton("Cantonese", config::GdxLanguage, 1);
	ImGui::NextColumn();
	OptionRadioButton("English", config::GdxLanguage, 2);
	ImGui::NextColumn();
	OptionRadioButton("Disabled", config::GdxLanguage, 3);
	ImGui::Columns(1, nullptr, false);

	auto apply_base_settings = []() {
		// Frame Limit
		config::LimitFPS = false;
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
	};

	if (ImGui::Button(t({ "Apply Recommended Settings\nfor Low-Spec PC", u8"低スペックPC向け\nおすすめ設定適用" }), ScaledVec2(200, 50))) {
		apply_base_settings();
		config::ThreadedRendering = true;
		config::GdxMinDelay = 3;
		config::AutoSkipFrame = 1;
	}
	ImGui::SameLine();
	if (ImGui::Button(t({ "Apply Recommended Settings\nfor Mid-Spec PC", u8"中スペックPC向け\nおすすめ設定適用" }), ScaledVec2(200, 50))) {
		apply_base_settings();
		config::ThreadedRendering = false;
		config::RenderResolution = 960;
	}
	ImGui::SameLine();
	if (ImGui::Button(t({ "Apply Recommended Settings\nfor High-Spec PC", u8"高スペックPC向け\nおすすめ設定適用" }), ScaledVec2(200, 50))) {
		apply_base_settings();
		config::ThreadedRendering = false;
		config::RenderResolution = 1440;
		config::TextureUpscale = 2;
	}

	ImGui::SameLine();
	ShowHelpMarker(t({ "Use gdxsv recommended settings", u8"開発者おすすめの設定を適用します。このページ以外の設定も変更されます。\n\
低スペック向け:\n快適なプレイよりも負荷を下げてゲームスピードの安定を優先させる設定です。\n\
中スペック向け:\n快適なプレイが可能で負荷も少ないバランスの良い設定です。\n\
高スペック向け:\n快適なプレイが可能で負荷の高いオプションも使用する設定です。"}));

	OptionCheckbox(t({ "Multi-threaded emulation", u8"マルチスレッドエミュレーション" }), config::ThreadedRendering, t({
R"(Run the emulated CPU and GPU on different threads.
	Enable = Best for low spec CPU.
	Disable = Best for high spec CPU.)",
u8"エミュレーターの計算と描画を別のスレッドで行います。有効にした場合負荷が軽くなりますが、最大1フレームの入力遅延が発生します。\n\
低スペックCPUを使用している場合、有効を推奨\n\
高スペックCPUを使用している場合、無効を推奨"}));

	bool widescreen = config::Widescreen.get() && config::WidescreenGameHacks.get();
	bool pressed = ImGui::Checkbox(t({ "Enable 16:9 Widescreen Hack", u8"16:9 ワイドモニター対応" }), &widescreen);
	if (pressed) {
		config::Widescreen.set(widescreen);
		config::SuperWidescreen.set(widescreen);
		config::WidescreenGameHacks.set(widescreen);
	}
	ImGui::SameLine();
	ShowHelpMarker(t({
R"(Use the following rendering options:
    rend.WideScreen=true
    rend.SuperWideScreen=true
    rend.WidescreenGameHacks=true)",
u8"画面左右の黒縁を無くしワイドモニターに対応します。一部画面が正しく表示されなくなる可能性があります。"
		}));

	OptionCheckbox(t({ "VSync", u8"垂直同期" }), config::VSync, t({
"Limit frame rate by VSync. Minimize video glitch",
u8"モニターの更新タイミングを待って描画します。描画乱れが発生しにくくなりますが、表示に遅延が発生する可能性があります。\n\
無効にしても表示乱れが気にならない場合、無効を推奨"
		}));
	ImGui::Indent();
	{
		DisabledScope scope(!config::VSync);

		OptionCheckbox(t({ "Duplicate frames", u8"重複フレーム表示" }), config::DupeFrames, t({
"Duplicate frames on high refresh rate monitors (120 Hz and higher)",
u8"同じフレームを2回表示します。120Hzモニターで60FPSに制限することができます。"
			}));
	}
	ImGui::Unindent();

	header(t({ "Network Settings", u8"ネットワーク設定" }));
	OptionCheckbox(t({ "Enable UPnP", u8"UPnPを使用" }), config::EnableUPnP, t({
"Automatically configure your network router for netplay (IPv4 Only)",
u8"ルーターの設定を自動で変更し、ポート開放を行います。(IPv4のみ対応)",
}));

	ImGui::InputInt(t({ "Gdx UDP Port", u8"UDPポート" }), &config::GdxLocalPort.get());
	ImGui::SameLine();
	ShowHelpMarker(t({
"UDP port number used for P2P communication. Cannot use the same number as another application using.",
u8"オンライン対戦で使用するUDPポート番号を設定します。同時に使用する他のアプリケーションと同じポート番号は使用できません。"
}));

	if (config::GdxLocalPort == 0) {
		config::GdxLocalPort = get_random_port_number();
	}

	static std::string upnp_result;
	static std::future<std::string> upnp_future;
	if (upnp_future.valid() && upnp_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
		upnp_result = upnp_future.get();
	}

	const auto buttonWidth = 200;
	if (ImGui::Button(t({ "UPnP Now", u8"UPnP 実行" }), ScaledVec2(buttonWidth, 0)) && !upnp_future.valid()) {
		upnp_result = "Please wait...";
		int port = config::GdxLocalPort;
		upnp_future = std::async(std::launch::async, [port]() -> std::string {
			auto& upnp = gdxsv.UPnP();
			std::string result = upnp.Init() && upnp.AddPortMapping(port, false) ? "Success" : upnp.getLastError();
			return result;
			});
	}
	ImGui::SameLine();
	ShowHelpMarker(t({ "Open the port using UPnP", u8"UPnPを使用してUDPポート開放を行います。" }));
	ImGui::SameLine();
	ImGui::Text("%s", upnp_result.c_str());

	static std::string v4_result;
	static std::future<std::string> v4_future;
	if (future_is_ready(v4_future)) {
		v4_result = v4_future.get();
	}
	if (ImGui::Button(t({ "Test The Port", u8"ポート開放確認" }), ScaledVec2(buttonWidth, 0)) && !v4_future.valid()) {
		v4_result = "Please wait...";
		v4_future = test_udp_port_connectivity(config::GdxLocalPort, false);
	}
	ImGui::SameLine();
	ShowHelpMarker(t({
"Test receiving data using this UDP port",
u8"設定されたUDPポートを使って外部からデータを受信できるかテストします。\nこのテストに失敗する場合、対戦が成立しないか通信遅延が増加する場合があります。"
}));
	ImGui::SameLine();
	ImGui::Text("%s", v4_result.c_str());

	OptionArrowButtons(
		t({"Gdx Minimum Delay", u8"最小入力遅延"}), config::GdxMinDelay, 2, 6,
		t({
"Minimum frame of input delay used for rollback communication.\nSmaller value reduces latency, but uses more CPU and introduces glitches.",
u8"オンライン対戦時の最小入力遅延フレーム数を設定します。小さい値を設定すると入力遅延が減りますが、CPU使用率が増え表示乱れが発生しやすくなります。"
}));

	OptionCheckbox(t({ "Save Replay" , u8"リプレイ保存" }), config::GdxSaveReplay, t({ "Save replay file to replays directory", u8"オンライン対戦のリプレイファイルを保存します。"}));
	{
		DisabledScope scope(!config::GdxSaveReplay);
		ImGui::SameLine();
		OptionCheckbox(t({ "Upload Replay", u8"リプレイアップロード" }), config::GdxUploadReplay, t({ "Automatically upload the replay file after save", u8"オンライン対戦のリプレイファイルを自動アップロードします。"}));
	}

	OptionCheckbox(t({ "Display Network Statistics", u8"通信状況表示" }), config::NetworkStats,
		t({
"Display network statistics on screen by default.\nUse Flycast Menu button to show/hide.",
u8"オンライン対戦時に通信の統計情報を表示します。.\nMenu ボタンを押して表示・非表示を切り替えることができます。",
}));
}
// clang-format on
