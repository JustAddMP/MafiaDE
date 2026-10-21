-- (one-PC scenario) where our player sits and where the other player's puppet sits, as this game sees it
local M = MafiaCoop
local p = game.game:GetActivePlayer()
if not p then return "not in the world" end
local my = M.my
local out = { string.format("we: %s", my and string.format("car %s seat %s", tostring(my.key), tostring(my.seat)) or "on foot") }
for id, pp in pairs(M.pup or {}) do
  out[#out + 1] = string.format("puppet %s: veh %s seat %s seated %s perch %s hidden %s ejects %s", id, tostring(pp.vkey),
    tostring(pp.seat), tostring(pp.seated), tostring(pp.perch ~= nil), tostring(pp.hidden), tostring(pp.ejectsAll or 0))
end
return table.concat(out, "; ")
