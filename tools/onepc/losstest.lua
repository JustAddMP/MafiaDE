-- simulates the 2026-10-20 field case on this game: the game removes puppet <id> and the spawner hangs on every
-- later load. The old entity is hidden out of the way (we cannot destroy it from Lua).
local M = MafiaCoop
local id = M.lossId or "0"
local pp = M.pup[id]
if not pp then return "no puppet " .. id end
local old = pp.so
if old then pcall(old.Deactivate, old); pcall(old.SetPos, old, Math:newVector(0, 0, -300)) end
M.retired = M.retired or {}
pp.so = nil  -- M.Drive sees it gone
local f = M.fac
if f and f.comp then
  local real = f.comp
  f.comp = {
    SetSpawnProfile = function(self, p) M.lossSet = p end,
    GetSpawnProfileLoadSyncObject = function() return { IsDone = function() return false end } end,
    CreateObject = function() return real:CreateObject() end,
  }
end
M.lossT = M.now
return "puppet " .. id .. " removed, spawner made to hang"
