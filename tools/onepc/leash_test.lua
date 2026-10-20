-- The leash (src/game/leash.h, docs/host-authority.md 3), run on the GUEST: puts us 450 m from the host's puppet
-- (on foot; our car with us when we sit at the wheel), on the road there if the game finds one. Then within ~1 s the
-- banner "TOO FAR FROM THE HOST - BACK IN 10 S" (log `leash: near -> countdown` / `warn -> countdown`), after ~10 s
-- `leash: pulled back beside the host (foot|road|beside)` and the LSH trace rows (ev=pull, ev=pulled ok=1). With the
-- host paused meanwhile: `THE HOST PAUSED`, no pull until he resumes.
local M = MafiaCoop
local p = game.game:GetActivePlayer()
if not p then return "not in the world" end
if M.isHost then return "this is the host: run it on the guest" end
local host = M.pup and M.pup["0"]
if not (host and host.tx) then return "no host puppet here" end
local q = p:GetPos()
local dx, dy = q.x - host.tx, q.y - host.ty
local l = math.sqrt(dx * dx + dy * dy)
if l < 1 then dx, dy, l = 1, 0, 1 end
local tx, ty, tz = host.tx + dx / l * 450, host.ty + dy / l * 450, host.tz
local how = "straight"
local good, pt = pcall(function() return game.traffic:GetClosestPointOnRoad(Math:newVector(tx, ty, tz), 0) end)
if good and pt then
  local g2, px, py, pz = pcall(function() return pt.x, pt.y, pt.z end)
  if g2 and type(px) == "number" and math.sqrt((px - host.tx) ^ 2 + (py - host.ty) ^ 2) > 420 then
    tx, ty, tz, how = px, py, pz, "road"
  end
end
local my = M.my
local car = my and my.c and my.c.ent
if car and IsObjectValid(car) and my.seat == 0 then
  car:SetPos(Math:newVector(tx, ty, tz + 0.5))
  how = how .. ", in our car"
else
  p:SetPos(Math:newVector(tx, ty, tz + 0.5))
  how = how .. ", on foot"
end
if mc_log then mc_log(string.format("leash test: moved %.0f m from the host (%s)", math.sqrt((tx - host.tx) ^ 2 +
  (ty - host.ty) ^ 2), how)) end
return string.format("moved to %.0f %.0f (%.0f m from the host, %s)", tx, ty,
  math.sqrt((tx - host.tx) ^ 2 + (ty - host.ty) ^ 2), how)
