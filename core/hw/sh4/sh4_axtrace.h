// ai-analysis: executed-block / observed-edge recorder for reverse engineering. Off unless switched on.
//
// Switch: env AX_TRACE=<file> records from boot and writes <file> at exit; env AX_TRACE_ON=1 turns the
// mechanism on with recording stopped, so a Lua script scopes the trace to one scenario
// (flycast.trace.start/stop/mark/save/clear/stats).
// The AX_TRACE exit write only happens on a clean exit: a harness that kills the process (ac_trials.py
// does) gets nothing, so scripted runs should call flycast.trace.save(path) themselves.
//
// What it records, at every dynarec block ENTRY (x64 recompiler only):
//   count[block]++            -> log line "B <blockstart> <endpc> <count>"
//   edge[prev.exitpc, block]  -> log line "E <branchpc> <blockstart> <count>"
// Addresses are physical guest addresses (0x0c......), the same space the analysis db uses.
// <endpc> is one past the last instruction of the block; <branchpc> is the address of the branch
// instruction that ended the previous block (so an edge matches a static call/jump site), or the
// address of its last instruction when the block ends without a branch.
// Caveat: an interrupt or an exception entering a new block produces an edge from whatever branch
// ended the interrupted block. Those edges exist in the log and have no static counterpart.
#pragma once
#include "types.h"
#include <vector>

struct RuntimeBlockInfo;

namespace axtrace
{
struct Block
{
	u32 addr;	// physical block start
	u32 endpc;	// physical, one past the last instruction
	u32 exitpc;	// physical, the branch instruction that ends the block
	u32 pad;
	u64 count;
};

// true when switched on by env (fixed at first call)
bool enabled();
// true while entries are being recorded
bool recording();

// called by the recompiler while compiling a block; returns the stable record to pass to enter()
Block *registerBlock(const RuntimeBlockInfo *rbi);
// called at the start of every compiled block
void DYNACALL enter(Block *b);

void start(const char *label);
void stop();
void mark(const char *text);
void clear();
// writes the log (format v1, what ax.py ingest reads); returns the number of B+E lines, -1 on error
int save(const char *path);
u32 blockCount();
u32 edgeCount();

// PC hooks (s112): when a compiled block that STARTS at a hooked physical address is entered, snapshot
// r0-r15 + pr (context is flushed at block entry, before the block's register allocation). Works whenever
// the tracer is enabled (AX_TRACE / AX_TRACE_ON), independent of start/stop. Only block starts can be
// hooked (function entries, branch targets): check the address is a "B" line of a trace first.
struct HookHit { u32 pc; u32 pr; u32 r[16]; u64 seq; std::vector<u32> mem; };
void hookAdd(u32 pc);
// s136: at every hook hit also copy nwords u32 from r[reg]+off (per capture, in order added; main RAM
// 0x0c000000-0x0cffffff only, else 0xdeadbeef) into HookHit::mem. Cleared by hookClear.
// s166: reg 16 = FP registers: words fr[off/4 .. off/4+nwords-1] at the hit (off in bytes, 0..0x3c)
void hookCapture(int reg, s32 off, u32 nwords);
// same, base = u32 at r[reg]+ptrOff (one dereference: a field of the object r[reg] points to)
void hookCaptureDeref(int reg, s32 ptrOff, s32 off, u32 nwords);
void hookRemove(u32 pc);
void hookClear();
// moves the recorded hits out (max 65536 buffered; the rest counted in hookDropped)
std::vector<HookHit> hookTake();
u64 hookDropped();
}
