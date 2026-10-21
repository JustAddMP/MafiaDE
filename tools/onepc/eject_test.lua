-- (exploration, guest) throws the host's puppet off the bike it rides once (GetOutCarFast, what a game AI exit does),
-- so the seat keeping in M.SeatBike has to put it back; run_test.ps1 counts the "is off bike" lines that follow
local M = MafiaCoop
local pp
for _, x in pairs(M.pup) do pp = x end
if not pp then return "no puppet" end
local c = pp.vkey and M.cars[pp.vkey]
if not (c and c.ent) then return "puppet " .. pp.id .. " has no vehicle record" end
local ok, r = pcall(pp.so.GetOutCarFast, pp.so, c.ent)
return string.format("puppet %s off bike %s: GetOutCarFast %s %s", pp.id, tostring(pp.vkey), tostring(ok), tostring(r))
