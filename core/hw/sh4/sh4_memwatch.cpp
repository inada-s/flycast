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
u32 fallbackPc = NoPc;

namespace {
constexpr int MaxRanges = 32;
constexpr size_t MaxHits = 1 << 20;

struct Range { u32 lo, hi; };
Range ranges[MaxRanges];
std::atomic<int> rangeCount;
Range rranges[MaxRanges];
std::atomic<int> rrangeCount;
std::mutex mutex;	// guards ranges (writers), hits, logs
std::vector<Hit> hits;
u32 dropped;
FILE *logFile;
FILE *rlogFile;
int state = -1;	// -1 unknown, 0 off, 1 on
int rstate = -1;	// read watch, same

WriteMem8Func origWrite8;
WriteMem16Func origWrite16;
WriteMem32Func origWrite32;
WriteMem64Func origWrite64;

ReadMem8Func origRead8;
ReadMem16Func origRead16;
ReadMem32Func origRead32;
ReadMem64Func origRead64;

// "lo-hi,lo-hi" (hex, inclusive, physical) -> addRange/addReadRange
void parseRanges(const char *p, bool read)
{
	while (*p)
	{
		char *end;
		u32 lo = (u32)std::strtoul(p, &end, 16);
		u32 hi = lo;
		if (end == p)
			break;
		if (*end == '-')
			hi = (u32)std::strtoul(end + 1, &end, 16);
		if (read)
			addReadRange(lo, hi);
		else
			addRange(lo, hi);
		p = end;
		while (*p == ',' || *p == ' ')
			p++;
	}
}

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
	parseRanges(env, false);
	const char *logName = std::getenv("FLYCAST_WATCH_LOG");
	logFile = std::fopen(logName != nullptr ? logName : "watch.log", "w");
}

void rinit()
{
	rstate = 0;
	const char *env = std::getenv("FLYCAST_RWATCH");
	const char *sw = std::getenv("FLYCAST_MEMREADWATCH");
	bool haveRanges = env != nullptr && *env != 0;
	if (!haveRanges && (sw == nullptr || std::strcmp(sw, "1") != 0))
		return;
	rstate = 1;
	if (!haveRanges)
		return;
	parseRanges(env, true);
	const char *logName = std::getenv("FLYCAST_RWATCH_LOG");
	rlogFile = std::fopen(logName != nullptr ? logName : "rwatch.log", "w");
}

inline bool rhit(u32 addr, u32 size)
{
	int n = rrangeCount.load(std::memory_order_relaxed);
	if (n == 0)
		return false;
	u32 a = addr & 0x1fffffff;
	for (int i = 0; i < n; i++)
		if (a <= rranges[i].hi && a + size - 1 >= rranges[i].lo)
			return true;
	return false;
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
		std::fprintf(logFile, "%u %08x %08x %08x %u %llx %llx\n", FrameCount, pc == NoPc ? NoPc : pc + 2, pr, addr, size,
				(unsigned long long)oldv, (unsigned long long)newv);
		std::fflush(logFile);
	}
	if (hits.size() >= MaxHits)
		dropped++;
	else
		hits.push_back({ FrameCount, pc, pr, addr, size, oldv, newv, false });
}

void recordRead(u32 addr, u32 size, u64 v, u32 pc, u32 pr)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (rlogFile != nullptr)
	{
		std::fprintf(rlogFile, "%u %08x %08x %08x %u 0 %llx\n", FrameCount, pc == NoPc ? NoPc : pc + 2, pr, addr, size,
				(unsigned long long)v);
		std::fflush(rlogFile);
	}
	if (hits.size() >= MaxHits)
		dropped++;
	else
		hits.push_back({ FrameCount, pc, pr, addr, size, 0, v, true });
}

// Interpreter: Sh4cntx.pc is the instruction + 2. Dynarec: compiled memops go through dyn*; a handler hit is an
// interpreter fallback op (fallbackPc set around the call) or has no guest instruction (fallbackPc = NoPc).
inline u32 handlerPc() {
	return config::DynarecEnabled ? fallbackPc : Sh4cntx.pc - 2;
}

u8 DYNACALL hookRead8(u32 addr)
{
	u8 v = origRead8(addr);
	if (rhit(addr, 1))
		recordRead(addr, 1, v, handlerPc(), Sh4cntx.pr);
	return v;
}
u16 DYNACALL hookRead16(u32 addr)
{
	u16 v = origRead16(addr);
	if (rhit(addr, 2))
		recordRead(addr, 2, v, handlerPc(), Sh4cntx.pr);
	return v;
}
u32 DYNACALL hookRead32(u32 addr)
{
	u32 v = origRead32(addr);
	if (rhit(addr, 4))
		recordRead(addr, 4, v, handlerPc(), Sh4cntx.pr);
	return v;
}
u64 DYNACALL hookRead64(u32 addr)
{
	u64 v = origRead64(addr);
	if (rhit(addr, 8))
		recordRead(addr, 8, v, handlerPc(), Sh4cntx.pr);
	return v;
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

bool readEnabled()
{
	if (rstate < 0)
		rinit();
	return rstate == 1;
}

void installHandlers()
{
	if (enabled() && WriteMem8 != &hookWrite8)
	{
		origWrite8 = WriteMem8;
		origWrite16 = WriteMem16;
		origWrite32 = WriteMem32;
		origWrite64 = WriteMem64;
		WriteMem8 = &hookWrite8;
		WriteMem16 = &hookWrite16;
		WriteMem32 = &hookWrite32;
		WriteMem64 = &hookWrite64;
	}
	// instruction fetch goes through IReadMem16 and is deliberately not watched
	if (readEnabled() && ReadMem8 != &hookRead8)
	{
		origRead8 = ReadMem8;
		origRead16 = ReadMem16;
		origRead32 = ReadMem32;
		origRead64 = ReadMem64;
		ReadMem8 = &hookRead8;
		ReadMem16 = &hookRead16;
		ReadMem32 = &hookRead32;
		ReadMem64 = &hookRead64;
	}
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

bool addReadRange(u32 lo, u32 hi)
{
	lo &= 0x1fffffff;
	hi &= 0x1fffffff;
	if (hi < lo)
		std::swap(lo, hi);
	std::lock_guard<std::mutex> lock(mutex);
	int n = rrangeCount.load();
	if (n >= MaxRanges)
		return false;
	rranges[n] = { lo, hi };
	rrangeCount.store(n + 1);
	return true;
}

void removeReadRange(u32 lo, u32 hi)
{
	lo &= 0x1fffffff;
	hi &= 0x1fffffff;
	std::lock_guard<std::mutex> lock(mutex);
	int n = rrangeCount.load();
	for (int i = 0; i < n; i++)
		if (rranges[i].lo == lo && rranges[i].hi == hi)
		{
			rranges[i] = rranges[n - 1];
			rrangeCount.store(n - 1);
			return;
		}
}

void clearReadRanges()
{
	std::lock_guard<std::mutex> lock(mutex);
	rrangeCount.store(0);
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

u32 DYNACALL dynRead8(u32 addr, u32 pc, u32 pr)
{
	u8 v = addrspace::read8(addr);
	if (rhit(addr, 1))
		recordRead(addr, 1, v, pc, pr);
	return (u32)(s32)(s8)v;	// stock handlers sign-extend 8/16 bit reads
}
u32 DYNACALL dynRead16(u32 addr, u32 pc, u32 pr)
{
	u16 v = addrspace::read16(addr);
	if (rhit(addr, 2))
		recordRead(addr, 2, v, pc, pr);
	return (u32)(s32)(s16)v;
}
u32 DYNACALL dynRead32(u32 addr, u32 pc, u32 pr)
{
	u32 v = addrspace::read32(addr);
	if (rhit(addr, 4))
		recordRead(addr, 4, v, pc, pr);
	return v;
}
u64 DYNACALL dynRead64(u32 addr, u32 pc, u32 pr)
{
	u64 v = addrspace::read64(addr);
	if (rhit(addr, 8))
		recordRead(addr, 8, v, pc, pr);
	return v;
}
}
