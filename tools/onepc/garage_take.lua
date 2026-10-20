-- changes the player's Free Ride garage from a script, so that the game writes garage.pf (the 2026-10-20 field case:
-- the host's garage.pf was written for real at each garage visit). Exploration: that the garage is game.garage and what
-- CheatAddVehicle takes are UNVERIFIED (C_GarageManager's methods are known from the exe, docs/re-notes.md); every
-- call is guarded and the answer says what worked. Expected with the fix: the log line
-- "savesys: profile write #n kept out of the player's profile (progress: the cars in the player's garage)" and no
-- "savesys: written: garage.pf".
local g = game.garage
if g == nil then return "no game.garage: the garage cannot be scripted, take a car from the garage by hand" end
local out = {}
local function try(name, ...)
  local f = g[name]
  if f == nil then out[#out + 1] = name .. "=missing" return nil end
  local ok, r = pcall(f, g, ...)
  out[#out + 1] = name .. "=" .. (ok and tostring(r) or ("error " .. tostring(r)))
  return ok
end
try("GetNumGaragePlaces")
try("GetFreeGaragePlaces")
try("IsGarageEmpty")
local model = MafiaCoop.garageModel or "smith_v12"
if not try("CheatAddVehicle", model) then try("AddVehicle", model) end
try("HasVehicle", model)
return table.concat(out, ", ")
