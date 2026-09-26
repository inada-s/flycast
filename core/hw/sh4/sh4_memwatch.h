// ai-analysis: guest memory write watch for reverse engineering. Off unless switched on.
//
// Switch: env FLYCAST_WATCH="lo-hi,lo-hi" (inclusive physical ranges, hex) watches from boot and logs to
// FLYCAST_WATCH_LOG (default watch.log): "frame pc pr addr size old new", pc = writing instruction + 2.
// Env FLYCAST_MEMWATCH=1 enables the mechanism with no initial range (ranges added from Lua).
//
// The READ watch is a separate switch (reads are ~10x more frequent, so it is never on by accident):
// env FLYCAST_RWATCH="lo-hi,lo-hi" logs reads to FLYCAST_RWATCH_LOG (default rwatch.log), same line
// format with newValue = the value read and old = 0; env FLYCAST_MEMREADWATCH=1 enables it with no range.
// Works with the interpreter and the x64 dynarec: when enabled, the dynarec compiles every guest write as a call
// that carries the guest pc, so no write is inlined. When disabled nothing here runs.
// Dynarec hits with no guest instruction behind them (a handler call outside an interpreter fallback) carry
// pc = NoPc, logged as ffffffff (not + 2). The interpreter always logs its current pc.
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
	u64 oldValue;	// reads: 0
	u64 newValue;	// reads: the value read
	bool isRead;
};

// true when switched on by env (fixed at first call)
bool enabled();
// same for the read watch (independent switch)
bool readEnabled();
// wraps the WriteMem*/ReadMem* handlers (interpreter and dynarec instruction fallbacks)
void installHandlers();

bool addRange(u32 lo, u32 hi);
void removeRange(u32 lo, u32 hi);
void clearRanges();
bool addReadRange(u32 lo, u32 hi);
void removeReadRange(u32 lo, u32 hi);
void clearReadRanges();
std::vector<Hit> takeHits();
u32 takeDropped();

// dynarec write entry points (pc = writing instruction)
void DYNACALL dynWrite8(u32 addr, u32 data, u32 pc, u32 pr);
void DYNACALL dynWrite16(u32 addr, u32 data, u32 pc, u32 pr);
void DYNACALL dynWrite32(u32 addr, u32 data, u32 pc, u32 pr);
void DYNACALL dynWrite64(u32 addr, u64 data, u32 pc, u32 pr);
// dynarec read entry points (pc = reading instruction). Return value matches the stock read handlers:
// 8/16 bit sign-extended into 32.
u32 DYNACALL dynRead8(u32 addr, u32 pc, u32 pr);
u32 DYNACALL dynRead16(u32 addr, u32 pc, u32 pr);
u32 DYNACALL dynRead32(u32 addr, u32 pc, u32 pr);
u64 DYNACALL dynRead64(u32 addr, u32 pc, u32 pr);
// Hit.pc of a dynarec handler hit with no guest instruction behind it
constexpr u32 NoPc = 0xffffffff;
// pc of the instruction a dynarec block is running through the interpreter fallback, else NoPc
extern u32 fallbackPc;
}
