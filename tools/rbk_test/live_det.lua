-- Live spectator rig probe (Disc 2): one line per battle sim step, written when the
-- battle step counter changes. A rollback or a replay seek rewrites a step; the
-- comparison keeps the last write.
--   S <round> <step> <rng> <rng2> <hp:x:y:z per player>
local mem = flycast.memory
local PW = 0x0c3d1cd4
local out = io.open("det.txt", "w")
local last = nil
local n = 0
function cbVBlank()
  if mem.read8(PW + 5) ~= 3 then last = nil; return end
  local c = mem.read32(0x0c3d17d8)
  if c == last then return end
  last = c
  local t = { string.format("S %d %d %04x %04x", mem.read8(0x0c392059), c, mem.read16(0x0c3abf40), mem.read16(0x0c3abf42)) }
  for p = 0, 3 do
    local b = PW + p * 0x2000
    t[#t + 1] = string.format("%d:%08x:%08x:%08x", mem.read16(b + 0x182), mem.read32(b + 0x20), mem.read32(b + 0x24), mem.read32(b + 0x28))
  end
  out:write(table.concat(t, " "), "\n")
  n = n + 1
  if n % 60 == 0 then out:flush() end
end
flycast_callbacks = { vblank = cbVBlank }
