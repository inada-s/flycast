#pragma once

#include <array>
#include <vector>

#include "gdxsv_projectile_view.h"
#include "types.h"

// Experimental: reproduce the arcade slowdown in DC2 battles. The objects in play are weighed; at or
// over the threshold the battle runs at 30fps (the game steps once per delivered KeyMsg1, so every
// other frame delivers no input). Weights started from ac-slowdown-test ANALYSIS.md section 2.
//
// Replay decides locally. Rollback: peer 0 decides (LocalSlow) and sends it as a flag in its GGPO
// input; every peer stalls on that synced flag (gdxsv_backend_rollback.cpp).
class GdxsvSlowdown {
   public:
	// Disabled until the load model is better: no stalls in replay or rollback. The debug view still shows the load.
	static constexpr bool kEnabled = false;

	enum Category { Missile, Gun, Bazooka, Blast, Cracker, Beam, HeatRod, MsExplosion, HitSpark, NeedleMissile, Cannon, NumCategories };

	void OnVBlank();
	// Replay: true while the current vblank must not deliver input.
	bool Stalling() const { return stalling_; }
	// This peer's load at or over the threshold.
	bool LocalSlow();
	// Rollback: the synced state of the current frame, for the debug view.
	void SetSynced(bool slow) { synced_slow_ = slow; }
	// Debug view, hidden option gdxsv:ProjectileView.
	void DisplayOSD();

	static int Classify(const GdxsvProjectileView::Entry& e);
	static const char* CategoryName(int category);
	static float Weight(int category);

   private:
	struct Load {
		float value = 0;
		std::array<int, NumCategories> counts{};
	};

	// Returns false outside a DC2 battle.
	static bool Measure(std::vector<GdxsvProjectileView::Entry>& scratch, Load& out);

	bool in_battle_ = false;
	bool stalling_ = false;
	bool synced_slow_ = false;
	Load load_;
	std::vector<GdxsvProjectileView::Entry> entries_;
	std::vector<GdxsvProjectileView::Entry> osd_entries_;
};
