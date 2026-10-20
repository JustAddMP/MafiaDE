-- spawns a motorcycle_1935 3 m to the side of the player and puts the player on it (exploration)
local p = game.game:GetActivePlayer()
local pos, d = p:GetPos(), p:GetDir()
local so, id = game.traffic:ObtainSpecificCar("motorcycle_1935")
MafiaCoop.tbike = { id = id, st = "obtain" }
StartThread(function()
  Wait(so)
  local s = game.traffic:SpawnCar(id, Math:newVector(pos.x + d.y * 3, pos.y - d.x * 3, pos.z + 0.3), d)
  Wait(s)
  local e = game.traffic:GetSpawnedEntity(id)
  MafiaCoop.tbike.ent0 = e
  if e == nil then local ok, so2 = pcall(game.traffic.GetSpawnedSceneObject, game.traffic, id) e = ok and so2 or nil end
  MafiaCoop.tbike.ent = e
  MafiaCoop.tbike.st = "spawned"
  Sleep(0.5)
  local mc = nil
  pcall(function() mc = e:GetComponent("C_RuntimeMotorcycleComponent") end)
  MafiaCoop.tbike.mc = mc
  local ok, r = pcall(function()
    p:LockControls(true)
    local en
    if mc and mc.FindSeatForHuman then en = p:GetOnVehicle(e, 1, false, "WALK") else en = p:GetOnVehicle(e, 1, false, "WALK") end
    Wait(en)
    p:LockControls(false)
  end)
  MafiaCoop.tbike.st = "on:" .. tostring(ok) .. ":" .. tostring(r)
end)
return "obtaining " .. tostring(id)
