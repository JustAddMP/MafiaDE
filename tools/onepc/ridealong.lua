-- (one-PC scenario, either game) our player gets into the OTHER player's car (our copy of it) at seat
-- MafiaCoop.rideSeat (0 = driver, 1 = front passenger, 2 / 3 = the back; default 1), at once (no walk): the ride
-- along. A seat the car does not have is refused (the 2026-10-21 04:06 run: seat 2 of a two-seat smith_v12 put the
-- guest under the street); if we are not in the car 2 s later and fell, we are put back where we stood.
local M = MafiaCoop
local p = game.game:GetActivePlayer()
if not p then return "not in the world" end
local seat = M.rideSeat or 1
local best, bd
local q = p:GetPos()
for key, c in pairs(M.cars or {}) do
  if c.driver and c.driver ~= "local" and c.st == "ready" and c.ent and IsObjectValid(c.ent) then
    local e = c.ent:GetPos()
    local d = (e.x - q.x) ^ 2 + (e.y - q.y) ^ 2
    if not bd or d < bd then best, bd = c, d end
  end
end
if not best then return "no car of the other player here" end
local okSeat = pcall(best.ent.GetSeatOwner, best.ent, seat)
local model = tostring(best.model)
if not okSeat or (seat >= 2 and (model:find("smith_v12") or model:find("coupe") or model:find("truck"))) then
  return string.format("car %s (%s) has no seat %d: not getting in", tostring(best.key), model, seat)
end
local q0 = { x = q.x, y = q.y, z = q.z }
StartThread(function()
  p:LockControls(true)
  local ok, e = pcall(p.GetOnVehicle, p, best.ent, seat + 1, false, "WALK")
  if ok and e then Wait(e) end
  p:LockControls(false)
  Sleep(2)
  local own = p:GetOwner()
  local z = p:GetPos().z
  if not (own and IsObjectValid(own)) and z < q0.z - 5 then
    p:SetPos(Math:newVector(q0.x, q0.y, q0.z + 0.5))
    if mc_log then mc_log("one-PC scenario: ride along: not in the car and fell: put back where we stood") end
  end
end)
return string.format("getting into car %s (%s, driver %s) seat %d, %.1f m away", tostring(best.key), model,
  tostring(best.driver), seat, math.sqrt(bd))
