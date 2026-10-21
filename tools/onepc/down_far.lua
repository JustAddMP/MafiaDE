-- (one-PC scenario, GUEST) far from the host (MafiaCoop.downDist or 200 m, under the leash's 300 m warning) we are
-- hurt down to the downed health (player_sync.lua M.DownTick: at or below 12 we are downed, 5 s later respawned next
-- to the host). Damage in small steps so we are downed, not killed.
local M = MafiaCoop
local p = game.game:GetActivePlayer()
if not p then return "not in the world" end
if M.isHost then return "this is the host: run it on the guest" end
local host = M.pup and M.pup["0"]
if not (host and host.tx) then return "no host puppet here" end
local dist = M.downDist or 200
local q = p:GetPos()
local dx, dy = q.x - host.tx, q.y - host.ty
local l = math.sqrt(dx * dx + dy * dy)
if l < 1 then dx, dy, l = 1, 0, 1 end
local tx, ty, tz = host.tx + dx / l * dist, host.ty + dy / l * dist, host.tz
local good, pt = pcall(function() return game.traffic:GetClosestPointOnRoad(Math:newVector(tx, ty, tz), 0) end)
if good and pt then
  local g2, px, py, pz = pcall(function() return pt.x, pt.y, pt.z end)
  if g2 and type(px) == "number" then tx, ty, tz = px, py, pz end
end
p:SetPos(Math:newVector(tx, ty, tz + 0.5))
M.downTest = { t0 = M.now, steps = 0 }
StartThread(function()
  Sleep(3)
  for i = 1, 200 do
    local h = tonumber(p.health) or 100
    M.downTest.h = h
    if h <= 11 or p:IsDeath() or M.down then break end
    pcall(p.DoDamage, p, h > 30 and 5 or 1)
    M.downTest.steps = i
    Sleep(0.05)
  end
end)
if mc_log then mc_log(string.format("one-PC scenario: down far: moved %.0f m from the host, hurting us to the downed health", math.sqrt((tx - host.tx) ^ 2 + (ty - host.ty) ^ 2))) end
return string.format("moved %.0f m from the host; hurting in 3 s", math.sqrt((tx - host.tx) ^ 2 + (ty - host.ty) ^ 2))
