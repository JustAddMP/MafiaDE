-- (exploration, guest) wraps M.SeatBike for a few seconds and records what the seat checks read each tick:
-- GetOwner / GetOwnerSceneObject of the puppet against the bike copy's entity and scene object, the motorcycle
-- component's seat owners, the puppet's height over the bike. Call again to get the transitions and unwrap.
local M = MafiaCoop
local P = M.flipProbe
local function v(o) return o ~= nil and IsObjectValid(o) end
local function s(o) if o == nil then return "nil" end return tostring(o) end
if P and P.orig then
  M.SeatBike = P.orig
  P.orig = nil
  local out, last = {}, nil
  for i, r in ipairs(P.rows) do
    if r.sig ~= last or i == #P.rows then out[#out + 1] = string.format("%.3f %s", r.t - P.t0, r.full) end
    last = r.sig
  end
  return string.format("%d rows, %d changes\n", #P.rows, #out) .. table.concat(out, "\n")
end
P = { rows = {}, t0 = M.now }
M.flipProbe = P
P.orig = M.SeatBike
M.SeatBike = function(pp, c)
  local so = pp.so
  local r = { t = M.now }
  local g1, own = pcall(so.GetOwner, so)
  local g2, oso = pcall(function() return so:GetOwnerSceneObject() end)
  own, oso = g1 and own or nil, g2 and oso or nil
  local seats = {}
  if c.mc then
    for k = 0, 1 do
      local g, o = pcall(function() return c.mc:GetSeatOwner(k) end)
      seats[#seats + 1] = g and (v(o) and (o == so and "pup" or "other") or "none") or "err"
    end
  end
  local q, b = so:GetPos(), c.ent:GetPos()
  local ownS = v(own) and ((own == c.ent and "ent") or (own == c.bso and "bso") or s(own)) or "nil"
  local osoS = v(oso) and ((oso == c.ent and "ent") or (oso == c.bso and "bso") or s(oso)) or "nil"
  r.sig = ownS .. "|" .. osoS .. "|" .. table.concat(seats, ",") .. "|" .. tostring(pp.seated)
  local gp, ph = pcall(function() return so:GetPhysState() end)
  r.full = string.format("own=%s oso=%s seats=%s seated=%s dz=%.2f dxy=%.2f phys=%s hid=%s", ownS, osoS,
    table.concat(seats, ","), tostring(pp.seated), q.z - b.z, math.sqrt((q.x - b.x) ^ 2 + (q.y - b.y) ^ 2),
    gp and tostring(ph) or "?", tostring(pp.hidden))
  if #P.rows < 3000 then P.rows[#P.rows + 1] = r end
  return P.orig(pp, c)
end
return "wrapped M.SeatBike"
