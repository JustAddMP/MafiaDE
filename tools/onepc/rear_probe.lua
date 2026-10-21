-- (exploration, host or guest) the other player's puppet in a back seat of a car here: tries one seat command on it
-- (MafiaCoop.rearWay: "anim" = GetOnVehicle animated, "fast" = not animated, "seat3" = the other back seat) and
-- records owner / seat getters for 3 s
local M = MafiaCoop
local pp
for _, x in pairs(M.pup) do pp = x end
if not (pp and pp.so and pp.vkey) then return "no puppet in a car" end
local c = M.cars[pp.vkey]
if not (c and c.ent) then return "no car record" end
local so, car = pp.so, c.ent
local way = M.rearWay or "anim"
local seat = way == "seat3" and 4 or (pp.seat + 1)
local ok, err = pcall(so.GetOnVehicle, so, car, seat, way == "anim", "WALK")
M.rearRows = {}
local t0 = M.now
StartThread(function()
  for i = 1, 60 do
    local own = so:GetOwner()
    local seats = {}
    for s = 0, 3 do
      local g, o = pcall(car.GetSeatOwner, car, s)
      seats[#seats + 1] = g and (o == nil and "-" or (o == so and "P" or "x")) or "e"
    end
    M.rearRows[#M.rearRows + 1] = string.format("%.2f own=%s seats=%s", M.now - t0, tostring(own ~= nil and own == car), table.concat(seats))
    Sleep(0.05)
  end
end)
return string.format("puppet %s car %s seat %d: GetOnVehicle(%d, %s) %s %s", pp.id, tostring(pp.vkey), pp.seat, seat,
  tostring(way == "anim"), tostring(ok), tostring(err))
