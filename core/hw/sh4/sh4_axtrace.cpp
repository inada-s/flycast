// ai-analysis: executed-block / observed-edge recorder. See sh4_axtrace.h for the switches and format.
#include "sh4_axtrace.h"
#include "sh4_mem.h"
#include "sh4_if.h"
#include "dyna/blockmanager.h"
#include "hw/pvr/Renderer_if.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace axtrace
{
static bool on;
static bool onInit;
static bool rec;
static const char *exitLog;
static std::string runLabel;
static std::vector<std::string> marks;

static std::deque<Block> blocks;			// stable addresses: the codegen holds pointers into this
static std::unordered_map<u64, Block *> byKey;	// (vaddr << 32) | sh4_code_size -> record
static Block *prev;

// open-addressing edge table: key = (branchpc << 32) | blockstart. Fixed size, no allocation at runtime.
static constexpr u32 EDGE_BITS = 21;
static constexpr u32 EDGE_SIZE = 1u << EDGE_BITS;
struct Edge { u64 key; u64 count; };
static std::vector<Edge> edges;
static u32 nedges;
static u64 edgesDropped;

static u64 hash64(u64 x)
{
	x += 0x9e3779b97f4a7c15ull;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
	return x ^ (x >> 31);
}

bool enabled()
{
	if (!onInit)
	{
		onInit = true;
		const char *log = std::getenv("AX_TRACE");
		const char *mech = std::getenv("AX_TRACE_ON");
		if (log != nullptr && *log != 0)
		{
			on = true;
			exitLog = log;
			rec = true;
		}
		else if (mech != nullptr && *mech != 0 && std::strcmp(mech, "0") != 0)
		{
			on = true;
		}
		if (on)
		{
			edges.resize(EDGE_SIZE);
			std::atexit([]() {
				if (exitLog != nullptr)
					save(exitLog);
			});
		}
	}
	return on;
}

bool recording() {
	return rec;
}

// The branch that ends the block: a delay-slot branch sits 2 instructions before the end, a bt/bf
// without delay slot is the last instruction. A block that ends for any other reason (interrupt
// check, size limit) has no branch and reports its last instruction.
static u32 exitOffset(u32 vaddr, u32 size)
{
	if (size >= 4)
	{
		u16 op = IReadMem16(vaddr + size - 4);
		bool delayBranch =
			(op & 0xf000) == 0xa000 ||		// bra
			(op & 0xf000) == 0xb000 ||		// bsr
			(op & 0xff00) == 0x8d00 ||		// bt/s
			(op & 0xff00) == 0x8f00 ||		// bf/s
			(op & 0xf0ff) == 0x402b ||		// jmp
			(op & 0xf0ff) == 0x400b ||		// jsr
			(op & 0xf0ff) == 0x0023 ||		// braf
			(op & 0xf0ff) == 0x0003 ||		// bsrf
			op == 0x000b ||					// rts
			op == 0x002b;					// rte
		if (delayBranch)
			return size - 4;
	}
	return size >= 2 ? size - 2 : 0;
}

Block *registerBlock(const RuntimeBlockInfo *rbi)
{
	u64 key = ((u64)rbi->vaddr << 32) | rbi->sh4_code_size;
	auto it = byKey.find(key);
	if (it != byKey.end())
		return it->second;
	blocks.push_back(Block());
	Block *b = &blocks.back();
	b->addr = rbi->addr;
	b->endpc = rbi->addr + rbi->sh4_code_size;
	b->exitpc = rbi->addr + exitOffset(rbi->vaddr, rbi->sh4_code_size);
	b->count = 0;
	byKey[key] = b;
	return b;
}

static void bumpEdge(u32 src, u32 dst)
{
	u64 key = ((u64)src << 32) | dst;
	u32 i = (u32)(hash64(key) & (EDGE_SIZE - 1));
	for (u32 n = 0; n < 64; n++)
	{
		Edge& e = edges[(i + n) & (EDGE_SIZE - 1)];
		if (e.count == 0)
		{
			e.key = key;
			e.count = 1;
			nedges++;
			return;
		}
		if (e.key == key)
		{
			e.count++;
			return;
		}
	}
	edgesDropped++;
}

static std::unordered_map<u32, bool> hooks;	// physical pc -> hooked
static bool anyHook;
static std::vector<HookHit> hookHits;
static u64 hookSeq, hookDrop;
static constexpr size_t HOOK_MAX = 65536;
struct HookCap { int reg; s32 off; u32 n; bool deref; s32 ptrOff; };
static std::vector<HookCap> hookCaps;

void hookAdd(u32 pc) { hooks[pc & 0x1fffffff] = true; anyHook = true; }
void hookRemove(u32 pc) { hooks.erase(pc & 0x1fffffff); anyHook = !hooks.empty(); }
void hookClear() { hooks.clear(); anyHook = false; hookHits.clear(); hookDrop = 0; hookCaps.clear(); }
void hookCapture(int reg, s32 off, u32 nwords) { if (reg >= 0 && reg <= 16 && nwords > 0 && nwords <= 256) hookCaps.push_back({ reg, off, nwords, false, 0 }); }
void hookCaptureDeref(int reg, s32 ptrOff, s32 off, u32 nwords) { if (reg >= 0 && reg < 16 && nwords > 0 && nwords <= 256) hookCaps.push_back({ reg, off, nwords, true, ptrOff }); }
std::vector<HookHit> hookTake() { std::vector<HookHit> v; v.swap(hookHits); return v; }
u64 hookDropped() { return hookDrop; }

void DYNACALL enter(Block *b)
{
	if (anyHook && hooks.count(b->addr))
	{
		if (hookHits.size() < HOOK_MAX)
		{
			HookHit h;
			h.pc = b->addr;
			h.pr = Sh4cntx.pr;
			for (int i = 0; i < 16; i++)
				h.r[i] = Sh4cntx.r[i];
			h.seq = hookSeq++;
			for (const HookCap& c : hookCaps)
			{
				if (c.reg == 16)	// s166: FP bank, fr[off/4 + i]
				{
					for (u32 i = 0; i < c.n; i++)
					{
						s32 k = c.off / 4 + (s32)i;
						h.mem.push_back(k >= 0 && k < 16 ? Sh4cntx.fr_hex(k) : 0xdeadbeef);
					}
					continue;
				}
				u32 base = Sh4cntx.r[c.reg];
				if (c.deref)
				{
					u32 p = base + c.ptrOff;
					base = (p & 0x1f000003) == 0x0c000000 ? addrspace::read32(p) : 0;
				}
				for (u32 i = 0; i < c.n; i++)
				{
					u32 a = base + c.off + i * 4;
					h.mem.push_back((a & 0x1f000003) == 0x0c000000 ? addrspace::read32(a) : 0xdeadbeef);
				}
			}
			hookHits.push_back(std::move(h));
		}
		else
			hookDrop++;
	}
	if (!rec)
	{
		prev = nullptr;
		return;
	}
	b->count++;
	if (prev != nullptr)
		bumpEdge(prev->exitpc, b->addr);
	prev = b;
}

void start(const char *label)
{
	runLabel = label != nullptr ? label : "";
	prev = nullptr;
	rec = true;
}

void stop()
{
	rec = false;
	prev = nullptr;
}

void mark(const char *text)
{
	char line[256];
	std::snprintf(line, sizeof(line), "%u %s", FrameCount, text != nullptr ? text : "");
	marks.push_back(line);
}

void clear()
{
	for (Block& b : blocks)
		b.count = 0;
	if (!edges.empty())
		std::memset(edges.data(), 0, edges.size() * sizeof(Edge));
	nedges = 0;
	edgesDropped = 0;
	marks.clear();
	prev = nullptr;
}

int save(const char *path)
{
	if (!on || path == nullptr || *path == 0)
		return -1;
	FILE *f = std::fopen(path, "w");
	if (f == nullptr)
		return -1;
	std::fprintf(f, "# ax-trace v1 label=%s frame=%u dropped_edges=%llu\n",
			runLabel.c_str(), FrameCount, (unsigned long long)edgesDropped);
	for (const std::string& m : marks)
		std::fprintf(f, "# mark %s\n", m.c_str());
	int n = 0;
	for (const Block& b : blocks)
	{
		if (b.count == 0)
			continue;
		std::fprintf(f, "B %08x %08x %llu\n", b.addr, b.endpc, (unsigned long long)b.count);
		n++;
	}
	for (const Edge& e : edges)
	{
		if (e.count == 0)
			continue;
		std::fprintf(f, "E %08x %08x %llu\n", (u32)(e.key >> 32), (u32)e.key,
				(unsigned long long)e.count);
		n++;
	}
	std::fclose(f);
	return n;
}

u32 blockCount()
{
	u32 n = 0;
	for (const Block& b : blocks)
		if (b.count != 0)
			n++;
	return n;
}

u32 edgeCount() {
	return nedges;
}
}
