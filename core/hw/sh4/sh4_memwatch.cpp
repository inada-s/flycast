// ai-analysis: guest memory write watch, see sh4_memwatch.h
#include "sh4_memwatch.h"
#include "sh4_mem.h"
#include "sh4_if.h"
#include "hw/mem/addrspace.h"
#include "hw/pvr/Renderer_if.h"
#include "cfg/option.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace memwatch
{
u32 fallbackPc;

namespace {
constexpr int MaxRanges = 32;
constexpr size_t MaxHits = 1 << 20;

struct Range { u32 lo, hi; };
Range ranges[MaxRanges];
std::atomic<int> rangeCount;
std::mutex mutex;	// guards ranges (writers), hits, log
std::vector<Hit> hits;
u32 dropped;
FILE *logFile;
int state = -1;	// -1 unknown, 0 off, 1 on

WriteMem8Func origWrite8;
WriteMem16Func origWrite16;
WriteMem32Func origWrite32;
WriteMem64Func origWrite64;

void init()
{
	state = 0;
	const char *env = std::getenv("FLYCAST_WATCH");
	const char *sw = std::getenv("FLYCAST_MEMWATCH");
	bool haveRanges = env != nullptr && *env != 0;
	if (!haveRanges && (sw == nullptr || std::strcmp(sw, "1") != 0))
		return;
	state = 1;
	if (!haveRanges)
		return;
	const char *p = env;
	while (*p)
	{
		char *end;
		u32 lo = (u32)std::strtoul(p, &end, 16);
		u32 hi = lo;
		if (end == p)
			break;
		if (*end == '-')
			hi = (u32)std::strtoul(end + 1, &end, 16);
		addRange(lo, hi);
		p = end;
		while (*p == ',' || *p == ' ')
			p++;
	}
	const char *logName = std::getenv("FLYCAST_WATCH_LOG");
	logFile = std::fopen(logName != nullptr ? logName : "watch.log", "w");
}

inline bool hit(u32 addr, u32 size)
{
	int n = rangeCount.load(std::memory_order_relaxed);
	if (n == 0)
		return false;
	u32 a = addr & 0x1fffffff;
	for (int i = 0; i < n; i++)
		if (a <= ranges[i].hi && a + size - 1 >= ranges[i].lo)
			return true;
	return false;
}

u64 oldValue(u32 addr, u32 size)
{
	// RAM only: reading a register can have side effects
	u8 *p = GetMemPtr(addr, size);
	if (p == nullptr)
		return 0;
	u64 v = 0;
	std::memcpy(&v, p, size);
	return v;
}

void record(u32 addr, u32 size, u64 newv, u32 pc, u32 pr)
{
	u64 oldv = oldValue(addr, size);
	std::lock_guard<std::mutex> lock(mutex);
	if (logFile != nullptr)
	{
		std::fprintf(logFile, "%u %08x %08x %08x %u %llx %llx\n", FrameCount, pc + 2, pr, addr, size,
				(unsigned long long)oldv, (unsigned long long)newv);
		std::fflush(logFile);
	}
	if (hits.size() >= MaxHits)
		dropped++;
	else
		hits.push_back({ FrameCount, pc, pr, addr, size, oldv, newv });
}

// Interpreter: Sh4cntx.pc is the instruction + 2. Dynarec: only the interpreter fallback ops get here.
inline u32 handlerPc() {
	return config::DynarecEnabled ? fallbackPc : Sh4cntx.pc - 2;
}

void DYNACALL hookWrite8(u32 addr, u8 data)
{
	if (hit(addr, 1))
		record(addr, 1, data, handlerPc(), Sh4cntx.pr);
	origWrite8(addr, data);
}
void DYNACALL hookWrite16(u32 addr, u16 data)
{
	if (hit(addr, 2))
		record(addr, 2, data, handlerPc(), Sh4cntx.pr);
	origWrite16(addr, data);
}
void DYNACALL hookWrite32(u32 addr, u32 data)
{
	if (hit(addr, 4))
		record(addr, 4, data, handlerPc(), Sh4cntx.pr);
	origWrite32(addr, data);
}
void DYNACALL hookWrite64(u32 addr, u64 data)
{
	if (hit(addr, 8))
		record(addr, 8, data, handlerPc(), Sh4cntx.pr);
	origWrite64(addr, data);
}
}

bool enabled()
{
	if (state < 0)
		init();
	return state == 1;
}

void installHandlers()
{
	if (!enabled())
		return;
	if (WriteMem8 == &hookWrite8)
		return;
	origWrite8 = WriteMem8;
	origWrite16 = WriteMem16;
	origWrite32 = WriteMem32;
	origWrite64 = WriteMem64;
	WriteMem8 = &hookWrite8;
	WriteMem16 = &hookWrite16;
	WriteMem32 = &hookWrite32;
	WriteMem64 = &hookWrite64;
}

bool addRange(u32 lo, u32 hi)
{
	lo &= 0x1fffffff;
	hi &= 0x1fffffff;
	if (hi < lo)
		std::swap(lo, hi);
	std::lock_guard<std::mutex> lock(mutex);
	int n = rangeCount.load();
	if (n >= MaxRanges)
		return false;
	ranges[n] = { lo, hi };
	rangeCount.store(n + 1);
	return true;
}

void removeRange(u32 lo, u32 hi)
{
	lo &= 0x1fffffff;
	hi &= 0x1fffffff;
	std::lock_guard<std::mutex> lock(mutex);
	int n = rangeCount.load();
	for (int i = 0; i < n; i++)
		if (ranges[i].lo == lo && ranges[i].hi == hi)
		{
			ranges[i] = ranges[n - 1];
			rangeCount.store(n - 1);
			return;
		}
}

void clearRanges()
{
	std::lock_guard<std::mutex> lock(mutex);
	rangeCount.store(0);
}

std::vector<Hit> takeHits()
{
	std::lock_guard<std::mutex> lock(mutex);
	std::vector<Hit> v;
	v.swap(hits);
	return v;
}

u32 takeDropped()
{
	std::lock_guard<std::mutex> lock(mutex);
	u32 d = dropped;
	dropped = 0;
	return d;
}

void DYNACALL dynWrite8(u32 addr, u32 data, u32 pc, u32 pr)
{
	if (hit(addr, 1))
		record(addr, 1, (u8)data, pc, pr);
	addrspace::write8(addr, (u8)data);
}
void DYNACALL dynWrite16(u32 addr, u32 data, u32 pc, u32 pr)
{
	if (hit(addr, 2))
		record(addr, 2, (u16)data, pc, pr);
	addrspace::write16(addr, (u16)data);
}
void DYNACALL dynWrite32(u32 addr, u32 data, u32 pc, u32 pr)
{
	if (hit(addr, 4))
		record(addr, 4, data, pc, pr);
	addrspace::write32(addr, data);
}
void DYNACALL dynWrite64(u32 addr, u64 data, u32 pc, u32 pr)
{
	if (hit(addr, 8))
		record(addr, 8, data, pc, pr);
	addrspace::write64(addr, data);
}
}
