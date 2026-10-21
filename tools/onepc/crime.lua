-- (one-PC scenario, GUEST) we are wanted as after a crime (the guest's own police zone: game.police:ZoneCreate at our
-- position, HUNT, level MafiaCoop.crimeLevel or 2). The host's police near our puppet should take us on (host log
-- "guest 1 is wanted near us: our police take him on", PoliceChase), our game spawns no police of its own.
local M = MafiaCoop
local p = game.game:GetActivePlayer()
if not p then return "not in the world" end
if M.isHost then return "this is the host: run it on the guest" end
local lvl = M.crimeLevel or 2
local zt = enums and enums.PoliceZoneType and enums.PoliceZoneType.HUNT or 1
local ok, err = pcall(function() return game.police:ZoneCreate(p:GetPos(), zt, lvl) end)
if mc_log then mc_log("one-PC scenario: crime: ZoneCreate HUNT level " .. lvl .. " at our position: " .. tostring(ok)) end
return "ZoneCreate HUNT level " .. lvl .. ": " .. tostring(ok) .. " " .. tostring(err)
