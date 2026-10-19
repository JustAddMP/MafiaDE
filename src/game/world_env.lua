-- MafiaCoop world glue: time of day, time flow speed and weather set (game.gfx, C_GameGfxEnvEffModule; docs/game-api.md
-- 8). Runs in the game's Lua state on GameMainThread after player_sync.lua, called once per script tick by
-- src/game/world_env.cpp (MafiaCoop.WorldEnv.Update). Embedded into the dll; a copy at <dll dir>\world_env.lua
-- overrides it. The decisions (when to send, how a guest's clock follows the host's) are in C++
-- (src/game/world_env.h); this glue only reads the game and carries out what it is told.
--
-- Input (cmds, one line per command, carried out before the reading):
--   T hours        SetTime(hours, 0)
--   F speed        SetTimeFlowSpeed(speed)
--   W name         SetWeatherSet(name, 0, 0)
-- Output: "E time flow weather" (the game's reading after the commands) while we are in a loaded world, else "".
MafiaCoop = MafiaCoop or {}
local M = MafiaCoop
M.WorldEnv = M.WorldEnv or {}
local W = M.WorldEnv
W.version = 1

local function log(s) if mc_log then mc_log("world: " .. s) end end

local function try(what, f, ...)
  local good, err = pcall(f, ...)
  if not good then
    W.errors = (W.errors or 0) + 1
    if W.errors <= 20 then log(what .. " failed: " .. tostring(err)) end
  end
  return good
end

local function inWorld()
  if M.Player then
    local good, p = pcall(M.Player)
    return good and p ~= nil
  end
  local good, p = pcall(function() return game.game:GetActivePlayer() end)
  return good and p ~= nil
end

function W.Update(dt, cmds)
  local g = game and game.gfx
  if not g then return "" end
  local here = inWorld()
  for line in string.gmatch(cmds, "[^\n]+") do
    local op, arg = line:match("^(%S+) (%S+)")
    if here and op == "T" and tonumber(arg) then
      try("SetTime", g.SetTime, g, tonumber(arg), 0)
    elseif here and op == "F" and tonumber(arg) then
      try("SetTimeFlowSpeed", g.SetTimeFlowSpeed, g, tonumber(arg))
    elseif here and op == "W" and arg then
      try("SetWeatherSet", g.SetWeatherSet, g, arg, 0, 0)
    end
  end
  if not here then return "" end
  local good1, t = pcall(g.GetTime, g)
  local good2, flow = pcall(g.GetTimeFlowSpeed, g)
  local good3, w = pcall(g.GetCurrentWeatherSetName, g)
  if not (good1 and good2 and good3) or type(t) ~= "number" then return "" end
  if type(flow) ~= "number" then flow = 0 end
  w = w and tostring(w) or "-"
  if w == "" or w:find("%s") then w = "-" end
  return string.format("E %.5f %.5f %s", t, flow, w)
end

return W.version
