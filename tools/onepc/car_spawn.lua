-- spawns a car 4 m to the side of the player and puts the player at its wheel
local p = game.game:GetActivePlayer()
local pos, d = p:GetPos(), p:GetDir()
local model = MafiaCoop.tcarModel or "smith_v12"
local so, id = game.traffic:ObtainSpecificCar(model)
MafiaCoop.tcar = { id = id, st = "obtain" }
StartThread(function()
  Wait(so)
  local s = game.traffic:SpawnCar(id, Math:newVector(pos.x + d.y * 4, pos.y - d.x * 4, pos.z + 0.3), d)
  Wait(s)
  local car = game.traffic:GetSpawnedEntity(id)
  MafiaCoop.tcar.ent = car
  Sleep(0.5)
  p:LockControls(true)
  local e = p:GetOnVehicle(car, 1, false, "WALK")
  Wait(e)
  p:LockControls(false)
  MafiaCoop.tcar.st = "in"
end)
return "obtaining " .. model
