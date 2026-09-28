#pragma once

#include <array>

#include "gdxsv.pb.h"
#include "libs.h"

namespace gdxsv_round_counters {
// An explicit draw in the existing int32 win_team field. Zero remains unknown.
constexpr int kDraw = -1;
constexpr int kMaxRounds = 10;
constexpr u32 kSessionPointer = 0x0c394524; // Disc 2: session 0x0c392540 + 0x1fe4
constexpr u32 kSessionPointerDisk1 = 0x0c2f8de4; // Disc 1: session 0x0c2f6e00 + 0x1fe4
constexpr u32 kDrawFlagAddress = 0x0c3d1950; // Disc 2
constexpr u32 kDrawFlagAddressDisk1 = 0x0c3364c9; // Disc 1: winner 0x0c3364b6 + 0x13
constexpr u32 kPlayerStride = 0x2c0;
constexpr u32 kCountersOffset = 0x2b0;
constexpr int kPlayerSlots = 4;

inline int Outcome(u8 winner, u8 draw) {
	// Battle init clears winner, then draw: Disc 2 PC 0x0c05afb4 / 0x0c05affc,
	// Disc 1 PC 0x0c021284 / 0x0c0212d2 (same frame).
	// Both reset before gameplay; a zero winner ignores the leftover draw flag.
	return winner == 0 ? 0 : draw != 0 ? kDraw : winner;
}

inline bool BeforeRound(const proto::BattleLogFile& log, int completed, int team, std::array<u16, 4>& values) {
	if (completed < 0 || completed > kMaxRounds || completed > log.round_data_size())
		return false;
	std::array<u16, 4> result{static_cast<u16>(completed), 0, 0, 0};
	for (int i = 0; i < completed; ++i) {
		const int winner = log.round_data(i).win_team();
		if (winner == kDraw) {
			++result[3];
		} else if (winner == 1 || winner == 2) {
			if (team == 1 || team == 2)
				++result[winner == team ? 1 : 2];
		} else {
			return false; // Missing/unsupported results are not draws.
		}
	}
	values = result;
	return true;
}

// Disc 1 and 2 (same layout), on the emulation thread. Explicit completed
// rounds reseed a jump; otherwise use the guest's own completed count, including hidden
// result-card accounting. Rebuilding instead of adding deltas also repairs
// late legacy draw reconciliation and counters restored by backward seeks.
// Incomplete history preserves W/L/draw until a later update can resolve it.
inline bool Restore(const proto::BattleLogFile& log, int completed = -1, int disk = 2) {
	if (completed < -1 || completed > kMaxRounds)
		return false;
	const u32 session = gdxsv_ReadMem32(disk == 1 ? kSessionPointerDisk1 : kSessionPointer);
	// Require the full record span in main RAM before following the guest pointer.
	if (session < 0x0c000000 || session > 0x0d000000 - (kPlayerSlots + 1) * kPlayerStride) {
		// Diagnose explicit round jumps without flooding per-frame retries.
		if (completed >= 0)
			WARN_LOG(COMMON, "Round counter restore skipped: invalid session pointer 0x%08x (completed=%d)", session, completed);
		return false;
	}
	bool complete = true;
	for (int i = 0; i < kPlayerSlots; ++i) {
		const u32 player = session + (i + 1) * kPlayerStride;
		const u32 addr = player + kCountersOffset;
		const int rounds = completed >= 0 ? completed : gdxsv_ReadMem16(addr);
		if (completed >= 0 && gdxsv_ReadMem16(addr) != completed)
			gdxsv_WriteMem16(addr, completed);
		std::array<u16, 4> values;
		if (!BeforeRound(log, rounds, gdxsv_ReadMem8(player + 0x2ba), values)) {
			complete = false;
			continue;
		}
		for (int field = 0; field < 4; ++field) {
			if (gdxsv_ReadMem16(addr + field * 2) != values[field])
				gdxsv_WriteMem16(addr + field * 2, values[field]);
		}
	}
	return complete;
}
} // namespace gdxsv_round_counters
