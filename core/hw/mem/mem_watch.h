/*
	Copyright 2021 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
#include "types.h"
#include "addrspace.h"
#include "hw/aica/aica_if.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/elan.h"
#include "rend/TexCache.h"
#include "gdxsv/gdxsv_emu_hooks.h"
#include <unordered_map>
#include <unordered_set> // gdxsv: Watcher::protectRestored
#include <memory>

namespace memwatch
{

struct Page
{
	std::unique_ptr<u8[]> data = std::make_unique<u8[]>(PAGE_SIZE);
};
using PageMap = std::unordered_map<u32, Page>;

template<typename T>
class Watcher
{
	bool started;
	PageMap pages;
	// gdxsv: pages written in HotFaultSaves consecutive saves stay unprotected (hot): at each
	// save their content is compared with the copy taken at the previous save instead, which
	// costs less than a write fault and a re-protect per page. Used by the rollback save only.
	static constexpr u32 HotFaultSaves = 2;
	static constexpr u32 HotIdleSaves = 60; // unchanged for this many saves: protected again
	struct HotPage
	{
		Page page; // content at the last save
		u32 idleSaves = 0;
	};
	std::unordered_map<u32, HotPage> hot;
	std::unordered_map<u32, u32> faultStreak; // pages written in the last save: consecutive saves

public:
	void protect()
	{
		if (!started)
		{
			static_cast<T&>(*this).protectMem(0, 0xffffffff);
			started = true;
		}
		else
		{
			for (const auto& pair : pages)
				static_cast<T&>(*this).protectMem(pair.first, PAGE_SIZE);
		}
	}

	void unprotect()
	{
		static_cast<T&>(*this).unprotectMem(0, 0xffffffff);
	}

	// gdxsv: a rollback restores single pages
	void unprotectPage(u32 addr)
	{
		static_cast<T&>(*this).unprotectMem(addr, PAGE_SIZE);
	}

	// gdxsv: after a rollback, protect the restored pages and the pages written since the last
	// save. Every other page is still protected from its last save.
	void protectRestored(const std::unordered_set<u32>& restored)
	{
		protect();
		for (u32 addr : restored)
			if (hot.count(addr) == 0)
				static_cast<T&>(*this).protectMem(addr, PAGE_SIZE);
		pages.clear();
		for (auto& pair : hot)
			memcpy(&pair.second.page.data[0], static_cast<T&>(*this).getMemPage(pair.first), PAGE_SIZE);
	}

	// gdxsv: before a rollback restores older pages, undo the writes to hot pages since the last save
	void restoreHot()
	{
		for (const auto& pair : hot)
			memcpy(static_cast<T&>(*this).getMemPage(pair.first), &pair.second.page.data[0], PAGE_SIZE);
	}

	// gdxsv: protect() then getPages(), with hot pages. delta gets the content at the previous
	// save of each page changed since.
	void save(PageMap& delta)
	{
		T& t = static_cast<T&>(*this);
		if (!started)
		{
			protect();
			getPages(delta);
			return;
		}
		delta = PageMap();
		for (auto it = hot.begin(); it != hot.end();)
		{
			void *mem = t.getMemPage(it->first);
			HotPage& hp = it->second;
			if (memcmp(mem, &hp.page.data[0], PAGE_SIZE) != 0)
			{
				std::swap(delta[it->first].data, hp.page.data);
				memcpy(&hp.page.data[0], mem, PAGE_SIZE);
				hp.idleSaves = 0;
				++it;
			}
			else if (++hp.idleSaves >= HotIdleSaves)
			{
				t.protectMem(it->first, PAGE_SIZE);
				it = hot.erase(it);
			}
			else
				++it;
		}
		std::unordered_map<u32, u32> streaks;
		for (auto& pair : pages)
		{
			auto s = faultStreak.find(pair.first);
			u32 n = (s == faultStreak.end() ? 0 : s->second) + 1;
			if (n >= HotFaultSaves)
				memcpy(&hot[pair.first].page.data[0], t.getMemPage(pair.first), PAGE_SIZE);
			else
			{
				streaks[pair.first] = n;
				t.protectMem(pair.first, PAGE_SIZE);
			}
			delta[pair.first] = std::move(pair.second);
		}
		faultStreak = std::move(streaks);
		pages.clear();
	}

	void reset()
	{
		started = false;
		pages.clear();
		hot.clear(); // gdxsv
		faultStreak.clear(); // gdxsv
	}

	bool hit(void *addr)
	{
		u32 offset = static_cast<T&>(*this).getMemOffset(addr);
		if (offset == (u32)-1)
			return false;
		offset &= ~PAGE_MASK;
		// gdxsv: a hot page protected again by the code or texture cache: its copy is already taken
		if (hot.count(offset) != 0)
		{
			static_cast<T&>(*this).unprotectMem(offset, PAGE_SIZE);
			return true;
		}
	    auto rv = pages.try_emplace(offset);
	    if (!rv.second)
	    {
	      // gdxsv: already saved, yet it faulted: something protected the page again
	      // (gdxsv). Returning without unprotecting faults on the same write
	      // forever, as the code-protection handler may consider it unprotected.
	      static_cast<T&>(*this).unprotectMem(offset, PAGE_SIZE);
	      return true;
	    }
	    Page& page = rv.first->second;
	    memcpy(&page.data[0], static_cast<T&>(*this).getMemPage(offset), PAGE_SIZE);
		static_cast<T&>(*this).unprotectMem(offset, PAGE_SIZE);
		return true;
	}

	void getPages(PageMap& other)
	{
		std::swap(pages, other);
		pages = PageMap();
	}
};

class VramWatcher : public Watcher<VramWatcher>
{
	friend class Watcher<VramWatcher>;

protected:
	void protectMem(u32 addr, u32 size)
	{
		addrspace::protectVram(addr, std::min(VRAM_SIZE - addr, size) & ~PAGE_MASK);
	}

	void unprotectMem(u32 addr, u32 size)
	{
		addrspace::unprotectVram(addr, std::min(VRAM_SIZE - addr, size) & ~PAGE_MASK);
	}

	u32 getMemOffset(void *p)
	{
		return addrspace::getVramOffset(p);
	}

public:
	void *getMemPage(u32 addr)
	{
		return &vram[addr];
	}
};

class RamWatcher : public Watcher<RamWatcher>
{
	friend class Watcher<RamWatcher>;

protected:
	void protectMem(u32 addr, u32 size)
	{
		bm_LockPage(addr, std::min(RAM_SIZE - addr, size) & ~PAGE_MASK);
	}

	void unprotectMem(u32 addr, u32 size)
	{
		bm_UnlockPage(addr, std::min(RAM_SIZE - addr, size) & ~PAGE_MASK);
	}

	u32 getMemOffset(void *p)
	{
		return bm_getRamOffset(p);
	}

public:
	void *getMemPage(u32 addr)
	{
		return &mem_b[addr];
	}
};

class AicaRamWatcher : public Watcher<AicaRamWatcher>
{
	friend class Watcher<AicaRamWatcher>;

protected:
	void protectMem(u32 addr, u32 size);
	void unprotectMem(u32 addr, u32 size);
	u32 getMemOffset(void *p);

public:
	void *getMemPage(u32 addr)
	{
		return &aica::aica_ram[addr];
	}
};

class ElanRamWatcher : public Watcher<ElanRamWatcher>
{
	friend class Watcher<ElanRamWatcher>;

protected:
	void protectMem(u32 addr, u32 size);
	u32 getMemOffset(void *p);

public:
	void unprotectMem(u32 addr, u32 size);
	void *getMemPage(u32 addr)
	{
		return &elan::RAM[addr];
	}
};

extern VramWatcher vramWatcher;
extern RamWatcher ramWatcher;
extern AicaRamWatcher aramWatcher;
extern ElanRamWatcher elanWatcher;

inline static bool writeAccess(void *p)
{
	if (!config::GGPOEnable && !gdxsv_is_using_memwatch())
		return false;
	if (ramWatcher.hit(p))
	{
		bm_RamWriteAccess(p);
		return true;
	}
	if (vramWatcher.hit(p))
	{
		VramLockedWrite((u8 *)p);
		return true;
	}
	if (settings.platform.isNaomi2() && elanWatcher.hit(p))
		return true;
	return aramWatcher.hit(p);
}

inline static void protect()
{
	if (!config::GGPOEnable && !gdxsv_is_using_memwatch())
		return;
	vramWatcher.protect();
	ramWatcher.protect();
	aramWatcher.protect();
	elanWatcher.protect();
}

inline static void unprotect()
{
	vramWatcher.unprotect();
	ramWatcher.unprotect();
	aramWatcher.unprotect();
	elanWatcher.unprotect();
}

inline static void reset()
{
	vramWatcher.reset();
	ramWatcher.reset();
	aramWatcher.reset();
	elanWatcher.reset();
}

}
