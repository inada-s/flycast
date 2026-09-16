// ai-analysis: guest memory write watch for reverse engineering. Off unless switched on.
//
// Switch: env FLYCAST_WATCH="lo-hi,lo-hi" (inclusive physical ranges, hex) watches from boot and logs to
// FLYCAST_WATCH_LOG (default watch.log): "frame pc pr addr size old new", pc = writing instruction + 2.
// Env FLYCAST_MEMWATCH=1 enables the mechanism with no initial range (ranges added from Lua).
// Works with the interpreter and the x64 dynarec: when enabled, the dynarec compiles every guest write as a call
// that carries the guest pc, so no write is inlined. When disabled nothing here runs.
#pragma once
#include "types.h"
#include <vector>

namespace memwatch
{
struct Hit
{
	u32 frame;
	u32 pc;		// address of the writing instruction
	u32 pr;
	u32 addr;
	u32 size;
	u64 oldValue;
	u64 newValue;
};

// true when switched on by env (fixed at first call)
bool enabled();
// wraps the WriteMem* handlers (interpreter and dynarec instruction fallbacks)
void installHandlers();

bool addRange(u32 lo, u32 hi);
void removeRange(u32 lo, u32 hi);
void clearRanges();
std::vector<Hit> takeHits();
u32 takeDropped();

// dynarec write entry points (pc = writing instruction)
void DYNACALL dynWrite8(u32 addr, u32 data, u32 pc, u32 pr);
void DYNACALL dynWrite16(u32 addr, u32 data, u32 pc, u32 pr);
void DYNACALL dynWrite32(u32 addr, u32 data, u32 pc, u32 pr);
void DYNACALL dynWrite64(u32 addr, u64 data, u32 pc, u32 pr);
// pc of the instruction a dynarec block is running through the interpreter fallback
extern u32 fallbackPc;
}
