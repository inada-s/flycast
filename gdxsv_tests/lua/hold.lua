-- Stall the emulator thread at vblank E2E_PARAMS.frame so the host can grab a frame-exact window:
-- 0.8 s three vblanks early (queued texture loads finish), then log "E2E hold=<frame>" and stall
-- until the file "go" exists in the working dir (at most E2E_PARAMS.secs, default 15 s).

local function stall(secs, path)
  local t = os.clock()
  while os.clock() - t < secs do
    if path then
      local f = io.open(path, "r")
      if f then f:close(); return end
    end
  end
end

function main()
  e2e.wait_until(function() return e2e.frame >= E2E_PARAMS.frame - 3 end)
  stall(0.8)
  e2e.wait_until(function() return e2e.frame >= E2E_PARAMS.frame end)
  e2e.log("hold", e2e.frame)
  stall(E2E_PARAMS.secs or 15, "go")
end
