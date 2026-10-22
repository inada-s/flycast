// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "types.h"

// Skip-frame rendering: on skip frames (rollback resimulation, replay seek) the game's render call still runs,
// because object draw callbacks inside it update state that game logic reads. Only the disk's output-only
// functions (settings.gdxsv.renderOnlyFuncs) return at their entry, from the render call until it returns.

// Called by bm_GetCodeByVAddr before the lookup. Returns the address to run (pc = pr for a skipped function).
u32 gdxsv_sh4_code_addr(u32 addr);

// A dynarec may link a block straight to the block at addr only if gdxsv_sh4_code_addr has nothing to do
// there: the render call site, its return address and the output-only function entries must go through it.
bool gdxsv_sh4_linkable(u32 addr);

// For dynarecs that check dynamic branch targets in generated code.
// Return address of the running render call, 0 outside it.
u32* gdxsv_sh4_render_return_ptr();
// Bit (addr >> 1) & 4095 is set for the output-only functions.
u64* gdxsv_sh4_render_only_mask_ptr();

// Profiling: true when GDXSV_SH4_BLOCK_MAP names a file; bm_AddBlock then logs each compiled block there.
struct RuntimeBlockInfo;
extern const bool gdxsv_sh4_block_map_on;
void gdxsv_sh4_block_compiled(const RuntimeBlockInfo* block);
// Profiling: logs the x64 main loop (mainloop up to the first handler) and the memory handlers, ordered fast, store
// queue, slow x 8, 16, 32, 64 bit x read, write; the last one ends at handlersEnd.
void gdxsv_sh4_code_regions(const void* mainloop, const void* const* handlers, int count, const void* handlersEnd);
