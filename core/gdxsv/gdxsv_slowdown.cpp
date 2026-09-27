#include "gdxsv_slowdown.h"

#include <map>
#include <tuple>

#include "cfg/option.h"
#include "gdxsv.h"
#include "imgui/imgui.h"
#include "network/ggpo.h"

namespace {
constexpr u8 kWeaponClass = 0x11;
constexpr u8 kEffectClass = 0x13;
// Debris of a destroyed MS: 22-29 objects for ~1.5s right after the kill.
constexpr u8 kMsExplosionKind = 25;
// Spark of a hit (mostly shots, sometimes melee), ~2s.
constexpr u8 kHitSparkKind = 10;
// Load at or over which the battle runs at 30fps.
constexpr float kThreshold = 7.1f;
}  // namespace

// Kinds found by matching pool spawns with ammo drops and kills (replays and rbk_test).
// Every counted object weighs the same, so a burst of N objects weighs N times its weight.
int GdxsvSlowdown::Classify(const GdxsvProjectileView::Entry& e) {
	if (e.cls == kEffectClass && e.kind == kHitSparkKind) return HitSpark;
	if (e.cls == kEffectClass && e.kind == kMsExplosionKind) return MsExplosion;
	if (e.cls != kWeaponClass) return -1;
	switch (e.kind) {
		case 1:	 // beam rifles, Z'Gok arm beams
		case 4:	 // Dom chest beam
		case 8:	 // Gogg diffuse beam, 12 per shot
			return Beam;
		case 2:	 // vulcans, machine guns, Acguy, Z'Gok and Guntank missiles
			return Gun;
		case 3:	 // cannon shells (Guntank, Guncannon, GT Gundam)
			return Cannon;
		case 5:	 // bazooka shells (Char's Zaku, Dom)
			return Bazooka;
		case 6:	 // blasts: bazooka, cracker, torpedo, Gyan sub
			return Blast;
		case 7:	  // Char's Zaku cracker
		case 29:  // Zaku cracker
			return Cracker;
		case 14:  // Gouf heat rod: type 00 is the rod itself, held all battle; type 01 is the 8-object burst
			return e.type == 0x01 ? HeatRod : -1;
		case 26:  // Gyan needle missiles, 10 per shot
			return NeedleMissile;
		case 28:  // Gogg torpedo, 2 per shot
			return Missile;
		default:
			return -1;
	}
}

const char* GdxsvSlowdown::CategoryName(int category) {
	static const char* names[NumCategories] = {"Missile", "Gun", "Bazooka", "Blast", "Cracker", "Beam", "Heat rod", "MS explosion", "Hit spark", "Needle missile", "Cannon"};
	return 0 <= category && category < NumCategories ? names[category] : "?";
}

float GdxsvSlowdown::Weight(int category) {
	static const float weights[NumCategories] = {1.24f, 0.9f, 0.3f, 0.6f, 0.1f, 1.0f, 0.11f, 0.15f, 0.1f, 0.3f, 0.3f};
	return 0 <= category && category < NumCategories ? weights[category] : 0.f;
}

bool GdxsvSlowdown::Measure(std::vector<GdxsvProjectileView::Entry>& scratch, Load& out) {
	out = Load{};
	if (!GdxsvProjectileView::Collect(scratch, true)) return false;
	for (const auto& e : scratch) {
		const int c = Classify(e);
		if (0 <= c) out.counts[c]++;
	}
	for (int c = 0; c < NumCategories; c++) out.value += Weight(c) * out.counts[c];
	return true;
}

void GdxsvSlowdown::OnVBlank() {
	if (ggpo::active()) {
		// Rollback stalls in gdxsv_backend_rollback.cpp; here only the debug view's load.
		if (ggpo::isInRollback()) return;
		in_battle_ = GdxsvProjectileView::InBattle();
		if (in_battle_ && config::GdxProjectileView) Measure(entries_, load_);
		return;
	}
	if (gdxsv.IsReplaySeeking()) {
		stalling_ = false;
		return;
	}
	in_battle_ = gdxsv.IsReplaying() && Measure(entries_, load_);
	// Over the threshold every other vblank delivers no input.
	stalling_ = kEnabled && in_battle_ && kThreshold <= load_.value && !stalling_;
}

bool GdxsvSlowdown::LocalSlow() {
	return Measure(entries_, load_) && kThreshold <= load_.value;
}

void GdxsvSlowdown::DisplayOSD() {
	if (!config::GdxProjectileView || !in_battle_) return;

	ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 8, 8), ImGuiCond_Always, ImVec2(1, 0));
	ImGui::SetNextWindowBgAlpha(0.6f);
	ImGui::Begin("##gdxsv_slowdown", nullptr,
				 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
					 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
	// Rollback shows peer 0's synced decision next to this peer's own load.
	const bool slow = kEnabled && (ggpo::active() ? synced_slow_ : kThreshold <= load_.value);
	ImGui::TextColored(slow ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(1, 1, 1, 1), "Slowdown load %5.2f / %.2f  %s", load_.value,
					   kThreshold, slow ? "30fps" : "60fps");
	for (int c = 0; c < NumCategories; c++) {
		if (load_.counts[c] == 0) continue;
		ImGui::Text("  %-14s %3d x %.2f", CategoryName(c), load_.counts[c], Weight(c));
	}

	// Weapon objects that nothing counts, to spot new ones. Held weapons (kind 0, heat rod type 00) are skipped.
	std::map<std::tuple<int, int, int>, int> missing;  // (kind, type, owner MS) -> count
	if (GdxsvProjectileView::Collect(osd_entries_, false)) {
		for (const auto& e : osd_entries_) {
			if (e.kind == 0 || (e.kind == 14 && e.type == 0x00) || 0 <= Classify(e)) continue;
			missing[{e.kind, e.type, e.owner_ms}]++;
		}
	}
	if (!missing.empty()) {
		ImGui::Separator();
		ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Not counted:");
		for (const auto& [key, n] : missing) {
			const auto [kind, type, ms] = key;
			ImGui::Text("  kind %2d type %02x  %-14s x%d", kind, type, GdxsvProjectileView::MsName(ms), n);
		}
	}
	ImGui::End();
}
