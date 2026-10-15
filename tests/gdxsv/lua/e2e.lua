-- Helpers for the e2e case scripts; lib/emu.py loads this file followed by the
-- case's own script. A case defines main(), which runs as a coroutine resumed
-- once per vblank, so waits count frames, never wall-clock time.
-- Results go to e2e.log in the working directory as "E2E key=value" lines;
-- "E2E done=<frame>" when main() returns, "E2E error=..." if it fails.
-- Standard flycast Lua API only.

local mem = flycast.memory
local inp = flycast.input

e2e = { frame = 0 }

-- Dreamcast controller buttons (pressed = bit cleared in kcode)
e2e.BTN = {
  C = 0x0001, B = 0x0002, A = 0x0004, START = 0x0008,
  UP = 0x0010, DOWN = 0x0020, LEFT = 0x0040, RIGHT = 0x0080,
  Z = 0x0100, Y = 0x0200, X = 0x0400, D = 0x0800,
}

local out = io.open("e2e.log", "w")

function e2e.log(key, value)
  out:write("E2E " .. key .. "=" .. tostring(value) .. "\n")
  out:flush()
end

function e2e.wait(frames)
  for _ = 1, frames do coroutine.yield() end
end

-- Wait until fn() is true; false after max frames.
function e2e.wait_until(fn, max)
  local n = 0
  while not fn() do
    if max and n >= max then return false end
    coroutine.yield()
    n = n + 1
  end
  return true
end

function e2e.press(buttons, frames, player)
  player = player or 1
  inp.pressButtons(player, buttons)
  e2e.wait(frames or 3)
  inp.releaseButtons(player, buttons)
end

e2e.read8 = mem.read8
e2e.read16 = mem.read16
e2e.read32 = mem.read32

-- len bytes from addr as a hex string
function e2e.hex(addr, len)
  local s = {}
  for i = 0, len - 1 do s[#s + 1] = string.format("%02x", mem.read8(addr + i)) end
  return table.concat(s)
end

local co = nil

local function vblank()
  e2e.frame = e2e.frame + 1
  if co == nil then co = coroutine.create(main) end
  if coroutine.status(co) == "dead" then return end
  local ok, err = coroutine.resume(co)
  if not ok then
    e2e.log("error", err)
  elseif coroutine.status(co) == "dead" then
    e2e.log("done", e2e.frame)
  end
end

flycast_callbacks = { vblank = vblank }
