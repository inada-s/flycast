#include "gdxsv_gui_settings.h"

#include "gdxsv.h"
#include "gdxsv_network.h"
#include "gdxsv_custom_texture_update.h"
#include "hw/maple/maple_if.h"
#include "imgui.h"
#include "libs.h"
#include "ui/gui_util.h"

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
	ShowHelpMarker(t({ "Patch game language and texture, for DX only" , u8"ゲーム内の文字列やテクスチャに変更を加えます", u8"中文化補丁，修改遊戲圖檔和文字"}));
	ImGui::NextColumn();
	OptionRadioButton(u8"日本語", config::GdxLanguage, 0);
	ImGui::NextColumn();
	OptionRadioButton(u8"廣東話", config::GdxLanguage, 1);
	ImGui::NextColumn();
	OptionRadioButton("English", config::GdxLanguage, 2);
	ImGui::NextColumn();
	OptionRadioButton("Disabled", config::GdxLanguage, 3);
	ImGui::Columns(1, nullptr, false);

	auto settings_to_be_changed = []() -> std::string {
		std::string str = t({ "Settings to be changed:\n\n", u8"設定を変更する:\n\n", u8"將會被更改的設定:\n\n" });
		// Frame Limit
		if (config::LimitFPS == false)
			str += "AudioSync = true\n";
		if (config::FixedFrequency != 2)
			str += "FixedFrequency = 59.94 Hz\n";
		
		// Controls
		if (config::MapleMainDevices[0] != MapleDeviceType::MDT_SegaController)
			str += "Dreamcast Device A = Sega Controller\n";
		if (config::MapleExpansionDevices[0][0] != MapleDeviceType::MDT_SegaVMU)
			str += "Dreamcast Device A Slot 1 = Sega VMU\n";
		if (config::Sh4Clock != 200)
			str += "SH4 Clock = 200 Mhz\n";
		
		// Video
		if (config::PerStripSorting)
			str += "Transparent Sorting = Per Triangle\n";
		if (config::DelayFrameSwapping)
			str += "Delay Frame Swapping = false\n";
#if defined(_WIN32)
		if (config::RendererType != RenderType::DirectX11)
			str += "Graphics API = DirectX 11\n";
#else
		if (config::RendererType != RenderType::OpenGL)
			str += "Graphics API = Open GL\n";
#endif

		if (config::SkipFrame != 0)
			str += "Frame Skipping = 0\n";

		// Audio
		if (config::DSPEnabled != false)
			str += "Enable DSP = false\n";
		if (config::AudioVolume != 50)
			str += "Volume Level = 50\n";
		if (config::AudioBufferSize < 2822 || config::AudioBufferSize > 2824)
			str += "Audio Latency = 64ms\n";

		// Others
		if (config::DynarecEnabled != true)
			str += "CPU Mode = Dynarec\n";

		// Network
		if (config::EnableUPnP != true)
			str += "Enable UPnP = true\n";
		if (config::GdxLocalPort == 0)
			str += "Set a random Gdx UDP Port\n";

		return str;
	};

	if (ImGui::Button(t({ "Apply Recommended Settings\nfor Low-Spec PC", u8"低スペックPC向け\nおすすめ設定適用", u8"使用建議偏好設定\n低階電腦適用" }), ScaledVec2(200, 50))) {
		gdxsv_apply_base_settings();
		config::ThreadedRendering = true;
		config::GdxMinDelay = 3;
		config::AutoSkipFrame = 1;
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
	{
		auto tip = settings_to_be_changed();
		if (config::ThreadedRendering != true)
			tip += "Multi-threaded emulation = true\n";
		if (config::RenderResolution != 720)
			tip += "Internal Resolution = 1280x720 (x1.5)\n";
		if (config::AutoSkipFrame != 1)
			tip += "Automatic Frame Skipping = Normal\n";
		if (config::TextureUpscale != 1)
			tip += "Texture Upscaling = 1\n";
		if (config::GdxMinDelay != 3)
			tip += "Gdx Minimum Delay = 3\n";
		
		ImGui::SetTooltip(tip.c_str());
	}
	
	ImGui::SameLine();
	if (ImGui::Button(t({ "Apply Recommended Settings\nfor Mid-Spec PC", u8"中スペックPC向け\nおすすめ設定適用", u8"使用建議偏好設定\n中階電腦適用" }), ScaledVec2(200, 50))) {
		gdxsv_apply_base_settings();
		config::ThreadedRendering = false;
		config::RenderResolution = 960;
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
	{
		auto tip = settings_to_be_changed();
		if (config::ThreadedRendering != false)
			tip += "Multi-threaded emulation = false\n";
		if (config::RenderResolution != 960)
			tip += "Internal Resolution = 1706x960 (x2)\n";
		if (config::AutoSkipFrame != 0)
			tip += "Automatic Frame Skipping = Disabled\n";
		if (config::TextureUpscale != 1)
			tip += "Texture Upscaling = 1\n";
		if (config::GdxMinDelay != 2)
			tip += "Gdx Minimum Delay = 2\n";
		
		ImGui::SetTooltip(tip.c_str());
	}

	ImGui::SameLine();
	if (ImGui::Button(t({ "Apply Recommended Settings\nfor High-Spec PC", u8"高スペックPC向け\nおすすめ設定適用", u8"使用建議偏好設定\n高階電腦適用" }), ScaledVec2(200, 50))) {
		gdxsv_apply_base_settings();
		config::ThreadedRendering = false;
		config::RenderResolution = 1440;
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
	{
		auto tip = settings_to_be_changed();
		if (config::ThreadedRendering != false)
			tip += "Multi-threaded emulation = false\n";
		if (config::RenderResolution != 1440)
			tip += "Internal Resolution = 2560x1440 (x3)\n";
		if (config::AutoSkipFrame != 0)
			tip += "Automatic Frame Skipping = Disabled\n";
		if (config::GdxMinDelay != 2)
			tip += "Gdx Minimum Delay = 2\n";
		
		ImGui::SetTooltip(tip.c_str());
	}

	ImGui::SameLine();
	ShowHelpMarker(t({ "Use gdxsv recommended settings. Settings on other tabs will also be modified.\n\n\
Low-Spec:\nPrioritizes stability of gameplay by minimal load.\n\n\
Mid-Spec:\nWell-balanced setting that allows for comfortable gameplay with medium load.\n\n\
High-Spec:\nAllows for comfortable gameplay by utilizing high load options.",
u8"開発者おすすめの設定を適用します。このページ以外の設定も変更されます。\n\n\
低スペック向け:\n快適なプレイよりも負荷を下げてゲームスピードの安定を優先させる設定です。\n\n\
中スペック向け:\n快適なプレイが可能で負荷も少ないバランスの良い設定です。\n\n\
高スペック向け:\n快適なプレイが可能で負荷の高いオプションも使用する設定です。",
u8"使用開發者建議嘅偏好設定 (其他版嘅設定都會改埋)\n\n\
低階:\n\
以穩定遊戲速度為優先，降低負荷而非追求流暢遊戲嘅設定\n\n\
中階:\n\
流暢遊戲同負荷之間取個平衡嘅設定\n\n\
高階:\n\
最爽亦係最高負荷嘅設定"
		}));

	bool pressed = OptionCheckbox(t({ "UseTexturePack", u8"高解像度テクスチャを使用" }), config::GdxUseTexturePack, t({
"Use up-scaled textures.",
u8"ゲーム内で高品質なテクスチャを使用します. ",
}));
	if (pressed) {
		gdxsv_custom_texture_update.Reset();
	}

	OptionCheckbox(t({ "Multi-threaded emulation", u8"マルチスレッドエミュレーション" }), config::ThreadedRendering, t({
R"(Run the emulated CPU and GPU on different threads.
Can reduce the loading, but it may introduce a delay of 1 frame for input.
	Enable = Best for low spec CPU.
	Disable = Best for high spec CPU.)",
u8"エミュレーターの計算と描画を別のスレッドで行います。有効にした場合負荷が軽くなりますが、最大1フレームの入力遅延が発生します。\n\
低スペックCPUを使用している場合、有効を推奨\n\
高スペックCPUを使用している場合、無効を推奨",
u8"分拆GPU運算去另一線程，可以降低負荷但係有可能有 1 frame 輸入延遲\n\
低階 CPU 建議啓用\n\
高階 CPU 建議停用"
		}));

	ImGui::Indent();
	{
		DisabledScope scope(!config::ThreadedRendering.get());
		OptionCheckbox(t({ "Reduce input lag (Experimental)", u8"入力遅延の軽減(試験中)" }), config::FrequentInputPolling, t({
R"(Poll input during frame wait to reduce input lag in threaded rendering.
Only effective when Multi-threaded emulation is enabled.)",
u8"フレーム待機中も入力をポーリングしマルチスレッド時の入力遅延を軽減します。\nマルチスレッドエミュレーション有効時のみ効果があります。"}));
	}
	ImGui::Unindent();

	bool widescreen = config::Widescreen.get() && config::WidescreenGameHacks.get();
	pressed = ImGui::Checkbox(t({ "Enable 16:9 Widescreen Hack", u8"16:9 ワイドモニター対応", u8"使用 16:9 闊螢幕補丁" }), &widescreen);
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
u8"画面左右の黒縁を無くしワイドモニターに対応します。一部画面が正しく表示されなくなる可能性があります。",
u8"會將螢幕左右嘅黑邊去除，支援闊螢幕。(部分畫面可能會無法正確顯示)"
		}));

	OptionCheckbox(t({ "VSync", u8"垂直同期" }), config::VSync, t({
"Limit frame rate by VSync. Minimize video glitch",
u8"モニターの更新タイミングを待って描画します。描画乱れが発生しにくくなりますが、表示に遅延が発生する可能性があります。\n\
無効にしても表示乱れが気にならない場合、無効を推奨",
u8"等到 VSync 果吓先繪圖。\n防止畫面撕裂，但係會增加輸入延遲。\n如果唔覺有撕裂問題，建議停用。"
		}));
	ImGui::Indent();
	{
		DisabledScope scope(!config::VSync);

		OptionCheckbox(t({ "Duplicate frames", u8"重複フレーム表示" }), config::DupeFrames, t({
"Duplicate frames on high refresh rate monitors (120 Hz and higher)",
u8"同じフレームを2回表示します。120Hzモニターで60FPSに制限することができます。",
u8"會重複顯示同一個畫面兩次。120Hz螢幕適用。"
			}));
	}
	ImGui::Unindent();

	header(t({ "Network Settings", u8"ネットワーク設定", u8"網絡設定" }));
	OptionCheckbox(t({ "Enable UPnP", u8"UPnPを使用", u8"使用 UPnP" }), config::EnableUPnP, t({
"Automatically configure your network router for netplay (IPv4 Only)",
u8"ルーターの設定を自動で変更し、ポート開放を行います。(IPv4のみ対応)",
u8"會自動變更Router設定，幫你Port Forward（僅支援IPv4）"
		}));

	ImGui::InputInt(t({ "Gdx UDP Port", u8"UDPポート" }), &config::GdxLocalPort.get());
	ImGui::SameLine();
	ShowHelpMarker(t({
"UDP port number used for P2P communication. Cannot use the same number as another application using.",
u8"オンライン対戦で使用するUDPポート番号を設定します。同時に使用する他のアプリケーションと同じポート番号は使用できません。",
u8"網絡對戰用嘅 UDP Port。\n唔可以同其他程式/電腦用一樣嘅 Port，會撞。"
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
	if (ImGui::Button(t({ "UPnP Now", u8"UPnP 実行", u8"而家打通 UPnP" }), ScaledVec2(buttonWidth, 0)) && !upnp_future.valid()) {
		upnp_result = "Please wait...";
		int port = config::GdxLocalPort;
		upnp_future = std::async(std::launch::async, [port]() -> std::string {
			auto& upnp = gdxsv.UPnP();
			std::string result = upnp.Init() && upnp.AddPortMapping(port, false) ? "Success" : upnp.getLastError();
			return result;
			});
	}
	ImGui::SameLine();
	ShowHelpMarker(t({ "Open the port using UPnP", u8"UPnPを使用してUDPポート開放を行います。", u8"試吓同 Router 開個 Port。" }));
	ImGui::SameLine();
	ImGui::Text("%s", upnp_result.c_str());

	static std::string v4_result;
	static std::future<std::string> v4_future;
	if (future_is_ready(v4_future)) {
		v4_result = v4_future.get();
	}
	if (ImGui::Button(t({ "Test The Port", u8"ポート開放確認", u8"確認開 Port 成功" }), ScaledVec2(buttonWidth, 0)) && !v4_future.valid()) {
		v4_result = "Please wait...";
		v4_future = test_udp_port_connectivity(config::GdxLocalPort, false);
	}
	ImGui::SameLine();
	ShowHelpMarker(t({
"Test receiving data using this UDP port.\nIf this test fails, it may result in the inability to establish a match or an increase in communication latency",
u8"設定されたUDPポートを使って外部からデータを受信できるかテストします。\nこのテストに失敗する場合、対戦が成立しないか通信遅延が増加する場合があります。",
u8"測試個 Port 能否接收數據。\n如果此測試失敗，可能會導致對戰無法建立或增加通訊延遲。"
		}));
	ImGui::SameLine();
	ImGui::Text("%s", v4_result.c_str());

	OptionArrowButtons(
		t({"Gdx Minimum Delay", u8"最小入力遅延", u8"最少輸入延遲"}), config::GdxMinDelay, 2, 6,
		t({
"Minimum frame of input delay used for rollback communication.\nSmaller value reduces latency, but uses more CPU and introduces glitches.",
u8"オンライン対戦時の最小入力遅延フレーム数を設定します。小さい値を設定すると入力遅延が減りますが、CPU使用率が増え表示乱れが発生しやすくなります。",
u8"設定網絡對戰時最小輸入延遲。數値越細、輸入延遲越細，但會增加CPU負荷，並增加畫面跳格嘅可能性。"
		}));

	OptionCheckbox(t({ "Save Replay" , u8"リプレイ保存", u8"儲存 Replay" }), config::GdxSaveReplay, t({ "Save replay file to replays directory", u8"オンライン対戦のリプレイファイルを保存します。", u8"儲存網絡對戰嘅重播檔案到重播資料夾。" }));
	{
		DisabledScope scope(!config::GdxSaveReplay);
		ImGui::SameLine();
		OptionCheckbox(t({ "Upload Replay", u8"リプレイアップロード", u8"上載 Replay" }), config::GdxUploadReplay, t({ "Automatically upload the replay file after save", u8"オンライン対戦のリプレイファイルを自動アップロードします。", u8"上載網絡對戰嘅重播檔案。" }));
	}

	OptionCheckbox(t({ "Display Network Statistics", u8"通信状況表示", u8"對戰時顯示連線状態" }), config::NetworkStats,
		t({
"Display network statistics on screen by default.\nUse Flycast Menu button to show/hide.",
u8"オンライン対戦時に通信の統計情報を表示します。.\nMenu ボタンを押して表示・非表示を切り替えることができます。",
u8"網絡對戰時顯示連線状態。\n按下Menu button可以切換顯示或隱藏。",
		}));
}
// clang-format on

const char* gdxsv_gui_settings_text_for_preparing_font()
{
	return u8"檔嘅黑吓增開另幫你←↑→↓↖↗↘↙";
}
