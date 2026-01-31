/*
	Copyright 2024 flyinghead

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
#include "watchpoint.h"
#include "gdb_server.h"
#include <algorithm>

namespace watchpoint
{

static std::vector<Watchpoint> watchpoints;
static u32 lastHitAddr = 0;
static bool hasWatchpoints = false;

void init()
{
    reset();
}

void reset()
{
    watchpoints.clear();
    lastHitAddr = 0;
    hasWatchpoints = false;
}

bool add(WatchType type, u32 addr, u32 len)
{
    // Check if already exists
    for (const auto& wp : watchpoints) {
        if (wp.addr == addr && wp.len == len && wp.type == type) {
            return true;  // Already exists
        }
    }

    Watchpoint wp;
    wp.addr = addr;
    wp.len = len;
    wp.type = type;
    watchpoints.push_back(wp);
    hasWatchpoints = true;

    DEBUG_LOG(COMMON, "Watchpoint added: type=%d addr=0x%08x len=%d", type, addr, len);
    return true;
}

bool remove(WatchType type, u32 addr, u32 len)
{
    auto it = std::find_if(watchpoints.begin(), watchpoints.end(),
        [type, addr, len](const Watchpoint& wp) {
            return wp.addr == addr && wp.len == len && wp.type == type;
        });

    if (it != watchpoints.end()) {
        watchpoints.erase(it);
        hasWatchpoints = !watchpoints.empty();
        DEBUG_LOG(COMMON, "Watchpoint removed: type=%d addr=0x%08x len=%d", type, addr, len);
        return true;
    }
    return false;
}

static inline bool rangesOverlap(u32 addr1, u32 len1, u32 addr2, u32 len2)
{
    return addr1 < addr2 + len2 && addr2 < addr1 + len1;
}

bool checkRead(u32 addr, u32 len)
{
    if (!hasWatchpoints)
        return false;

    for (const auto& wp : watchpoints) {
        if ((wp.type == WATCH_READ || wp.type == WATCH_ACCESS) &&
            rangesOverlap(addr, len, wp.addr, wp.len)) {
            lastHitAddr = addr;
            DEBUG_LOG(COMMON, "Read watchpoint hit at 0x%08x", addr);
            // Trigger debug trap - event 0x1E0 is User break
            // This will throw debugger::Stop exception
            debugger::debugTrap(0x1E0);
            // unreachable - Stop exception is thrown
        }
    }
    return false;
}

bool checkWrite(u32 addr, u32 len)
{
    if (!hasWatchpoints)
        return false;

    for (const auto& wp : watchpoints) {
        if ((wp.type == WATCH_WRITE || wp.type == WATCH_ACCESS) &&
            rangesOverlap(addr, len, wp.addr, wp.len)) {
            lastHitAddr = addr;
            DEBUG_LOG(COMMON, "Write watchpoint hit at 0x%08x", addr);
            // Trigger debug trap - event 0x1E0 is User break
            // This will throw debugger::Stop exception
            debugger::debugTrap(0x1E0);
            // unreachable - Stop exception is thrown
        }
    }
    return false;
}

u32 getLastHitAddr()
{
    return lastHitAddr;
}

bool hasActiveWatchpoints()
{
    return hasWatchpoints;
}

} // namespace watchpoint
