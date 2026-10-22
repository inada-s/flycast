#include "gdxsv_key_display.h"

#include "gdxsv.h"
#include "gdxsv_custom_texture_source.h"
#include "gdxsv_video_hooks.h"
#include "imgui/imgui.h"
#include "ui/imgui_driver.h"
#include "rend/transform_matrix.h"
#include "types.h"
#include <memory>
#include <stb_image.h>

namespace {
struct GdxsvKeyTexture {
	const char* name;
	bool failed = false;

	ImTextureID Load() {
		const auto texture = imguiDriver->getTexture(name);
		if (texture != ImTextureID{} || failed) return texture;
		int width, height;
		std::unique_ptr<u8, decltype(&stbi_image_free)> data(GdxsvEmbedTextureSource::LoadExtraTexture(name, false, width, height),
															 stbi_image_free);
		// These bundled images do not change at runtime. Do not retry a
		// missing image every frame; the text fallback remains available.
		failed = data == nullptr;
		return data ? imguiDriver->updateTexture(name, data.get(), width, height, false) : ImTextureID{};
	}
};
GdxsvKeyTexture buttons{"BUTTONS.png"};
GdxsvKeyTexture arrows{"ARROWS.png"};

void getArrowsUV(u16 mcs_key, ImVec2& uv0, ImVec2& uv1);
std::string getArrowsStr(u16 mcs_key);
}  // namespace

void GdxsvKeyDisplay::DisplayOSD() {
	if (!enabled_) return;

	/*
	static bool once = false;
	if (!once) {
		once = true;
		AppendInput(0, McsKeyCode::RIGHT);
		AppendInput(0, McsKeyCode::LEFT);
		AppendInput(0, McsKeyCode::DOWN);
		AppendInput(0, McsKeyCode::UP);
		AppendInput(0, McsKeyCode::A);
		AppendInput(0, McsKeyCode::B);
		AppendInput(0, McsKeyCode::X);
		AppendInput(0, McsKeyCode::Y);
		AppendInput(0, McsKeyCode::RT);
		AppendInput(0, McsKeyCode::LT);
		AppendInput(0, McsKeyCode::START);
	}
	*/

	if (history_[display_player_].empty()) return;
	const auto buttonsTexture = buttons.Load();
	const auto arrowsTexture = arrows.Load();
	const auto display = ImGui::GetIO().DisplaySize;
	int x = 0, y = 0, w = static_cast<int>(display.x), h = static_cast<int>(display.y);
	if (gdxsv_video_grid_active) gdxsv_video_viewport(x, y, w, h);
	if (w <= 0 || h <= 0) return;
	const auto& style = ImGui::GetStyle();
	const float scale = gdxsv_osd_scale(w, h);
	// Text and spacing follow the same game-image scale as the button icons,
	// including when this view is rendered in a hidden guest's smaller window.
	const float display_scale = 16 * scale / style.FontSizeBase;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x * display_scale, style.WindowPadding.y * display_scale));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * display_scale, style.ItemSpacing.y * display_scale));
	ImGui::PushFont(nullptr, 16 * scale);
	ImGui::SetNextWindowPos(ImVec2(x, y));
	ImGui::SetNextWindowSize(ImVec2(w, h));
	ImGui::SetNextWindowBgAlpha(0.0f);
	ImGui::Begin("##gdxsv_keydisplay", NULL,
				 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs |
					 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoFocusOnAppearing);

	const auto button_size = ImVec2(16 * scale, 16 * scale);

	ImGui::SetCursorPosY(100 * scale);
	for (const auto& e : history_[display_player_]) {
		const auto cursor_y = ImGui::GetCursorPosY();
		ImGui::GetWindowDrawList()->AddRectFilled({0, cursor_y - 3 * display_scale}, {button_size.x * 4, cursor_y - display_scale},
												  ImColor(255, 255, 255, 64));

		int frame = e.frame;
		if (99 < frame) frame = 99;
		ImGui::TextColored(ImVec4(1, 1, 1, 1), "%02d", frame);
		ImGui::SameLine();

		ImVec2 uv0, uv1;
		if (arrowsTexture == ImTextureID{}) {
			const auto s = getArrowsStr(e.code);
			ImGui::Text("%s", s.c_str());
		} else {
			getArrowsUV(e.code, uv0, uv1);
			ImGui::Image(arrowsTexture, button_size, uv0, uv1);
		}

		uv0 = ImVec2{0, 0};
		uv1 = ImVec2{0, 1};
		if (e.code & McsKeyCode::A) {
			uv0.x = 0.f / 7;
			uv1.x = 1.f / 7;
			ImGui::SameLine();
			if (buttonsTexture == ImTextureID{}) ImGui::Text("A");
			else ImGui::Image(buttonsTexture, button_size, uv0, uv1);
		}
		if (e.code & McsKeyCode::B) {
			uv0.x = 1.f / 7;
			uv1.x = 2.f / 7;
			ImGui::SameLine();
			if (buttonsTexture == ImTextureID{}) ImGui::Text("B");
			else ImGui::Image(buttonsTexture, button_size, uv0, uv1);
		}
		if (e.code & McsKeyCode::X) {
			uv0.x = 2.f / 7;
			uv1.x = 3.f / 7;
			ImGui::SameLine();
			if (buttonsTexture == ImTextureID{}) ImGui::Text("X");
			else ImGui::Image(buttonsTexture, button_size, uv0, uv1);
		}
		if (e.code & McsKeyCode::Y) {
			uv0.x = 3.f / 7;
			uv1.x = 4.f / 7;
			ImGui::SameLine();
			if (buttonsTexture == ImTextureID{}) ImGui::Text("Y");
			else ImGui::Image(buttonsTexture, button_size, uv0, uv1);
		}
		if (e.code & McsKeyCode::RT) {
			uv0.x = 4.f / 7;
			uv1.x = 5.f / 7;
			ImGui::SameLine();
			if (buttonsTexture == ImTextureID{}) ImGui::Text("RT");
			else ImGui::Image(buttonsTexture, button_size, uv0, uv1);
		}
		if (e.code & McsKeyCode::LT) {
			uv0.x = 5.f / 7;
			uv1.x = 6.f / 7;
			ImGui::SameLine();
			if (buttonsTexture == ImTextureID{}) ImGui::Text("LT");
			else ImGui::Image(buttonsTexture, button_size, uv0, uv1);
		}
		if (e.code & McsKeyCode::START) {
			uv0.x = 6.f / 7;
			uv1.x = 7.f / 7;
			ImGui::SameLine();
			if (buttonsTexture == ImTextureID{}) ImGui::Text("ST");
			else ImGui::Image(buttonsTexture, button_size, uv0, uv1);
		}
	}
	if (!history_[display_player_].empty()) {
		const auto cursor_y = ImGui::GetCursorPosY();
		ImGui::GetWindowDrawList()->AddRectFilled({0, cursor_y - 3 * display_scale}, {button_size.x * 4, cursor_y - display_scale},
												  ImColor(255, 255, 255, 64));
	}

	ImGui::End();
	ImGui::PopFont();
	ImGui::PopStyleVar(4);
}

void GdxsvKeyDisplay::AppendInput(int player, u16 mcs_key) {
	verify(0 <= player && player < 4);
	if (!history_[player].empty() && history_[player].front().code == mcs_key) {
		history_[player].front().frame++;
		return;
	}
	history_[player].emplace_front(McsPadInput{1, mcs_key});
	if (15 <= history_[player].size()) history_[player].pop_back();
}

void GdxsvKeyDisplay::SetDisplayPlayer(int player) { display_player_ = player; }

void GdxsvKeyDisplay::Clear() {
	for (int i = 0; i < history_.size(); i++) {
		history_[i].clear();
	}
}

namespace {
void getArrowsUV(u16 mcs_key, ImVec2& uv0, ImVec2& uv1) {
	uv0 = ImVec2{1.f / 3, 1.f / 3};
	uv1 = ImVec2{2.f / 3, 2.f / 3};

	if (mcs_key & McsKeyCode::UP) {
		if (mcs_key & McsKeyCode::LEFT) {
			uv0 = ImVec2{0.f / 3, 0.f / 3};
			uv1 = ImVec2{1.f / 3, 1.f / 3};
		} else if (mcs_key & McsKeyCode::RIGHT) {
			uv0 = ImVec2{2.f / 3, 0.f / 3};
			uv1 = ImVec2{3.f / 3, 1.f / 3};
		} else {
			uv0 = ImVec2{1.f / 3, 0.f / 3};
			uv1 = ImVec2{2.f / 3, 1.f / 3};
		}
	} else if (mcs_key & McsKeyCode::DOWN) {
		if (mcs_key & McsKeyCode::LEFT) {
			uv0 = ImVec2{0.f / 3, 2.f / 3};
			uv1 = ImVec2{1.f / 3, 3.f / 3};
		} else if (mcs_key & McsKeyCode::RIGHT) {
			uv0 = ImVec2{2.f / 3, 2.f / 3};
			uv1 = ImVec2{3.f / 3, 3.f / 3};
		} else {
			uv0 = ImVec2{1.f / 3, 2.f / 3};
			uv1 = ImVec2{2.f / 3, 3.f / 3};
		}
	} else if (mcs_key & McsKeyCode::LEFT) {
		uv0 = ImVec2{0.f / 3, 1.f / 3};
		uv1 = ImVec2{1.f / 3, 2.f / 3};
	} else if (mcs_key & McsKeyCode::RIGHT) {
		uv0 = ImVec2{2.f / 3, 1.f / 3};
		uv1 = ImVec2{3.f / 3, 2.f / 3};
	}
}

std::string getArrowsStr(u16 mcs_key) {
#define AR_L u8"\u2190"
#define AR_U u8"\u2191"
#define AR_R u8"\u2192"
#define AR_D u8"\u2193"
#define AR_UL u8"\u2196"
#define AR_UR u8"\u2197"
#define AR_DR u8"\u2198"
#define AR_DL u8"\u2199"

	if (mcs_key & McsKeyCode::UP) {
		if (mcs_key & McsKeyCode::LEFT) return AR_UL;
		if (mcs_key & McsKeyCode::RIGHT) return AR_UR;
		return AR_U;
	}
	if (mcs_key & McsKeyCode::DOWN) {
		if (mcs_key & McsKeyCode::LEFT) return AR_DL;
		if (mcs_key & McsKeyCode::RIGHT) return AR_DR;
		return AR_D;
	}
	if (mcs_key & McsKeyCode::LEFT) {
		return AR_L;
	}
	if (mcs_key & McsKeyCode::RIGHT) {
		return AR_R;
	}
	return "    ";

#undef AR_L
#undef AR_R
#undef AR_U
#undef AR_D
#undef AR_UL
#undef AR_UR
#undef AR_DL
#undef AR_DR
}

}  // namespace

float gdxsv_osd_scale(float w, float h) {
	const float renderAR = getOutputFramebufferAspectRatio();
	const float screenAR = w / h;
	float dx = 0;
	float dy = 0;
	if (renderAR > screenAR)
		dy = h * (1 - screenAR / renderAR) / 2;
	else
		dx = w * (1 - renderAR / screenAR) / 2;

	return std::min((w - dx * 2) / 640.f, (h - dy * 2) / 480.f);
}
