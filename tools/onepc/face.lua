-- turns our player (and so the camera behind him) toward the other player's puppet; returns the distance and the
-- puppet's state
local M = MafiaCoop
local p = game.game:GetActivePlayer()
local pp
for _, x in pairs(M.pup) do pp = x end
if not pp then return "no puppet record" end
local tgt
if pp.so and IsObjectValid(pp.so) then tgt = pp.so:GetPos() elseif pp.tx then tgt = { x = pp.tx, y = pp.ty, z = pp.tz } end
if not tgt then return "puppet " .. pp.id .. " " .. tostring(pp.st) .. ": no position" end
local a = p:GetPos()
local dx, dy = tgt.x - a.x, tgt.y - a.y
local l = math.sqrt(dx * dx + dy * dy)
if l > 0.5 then p:SetDir(Math:newVector(dx / l, dy / l, 0)) end
local own = pp.so and IsObjectValid(pp.so) and pp.so:GetOwnerSceneObject() or nil
return string.format("puppet %s st=%s hidden=%s veh=%s seated=%s dist=%.1f m dz=%.2f owner=%s", pp.id, tostring(pp.st),
  tostring(pp.hidden), tostring(pp.veh), tostring(pp.seated), l, (tgt.z - a.z), tostring(own))
