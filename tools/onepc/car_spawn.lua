-- spawns a car 2.3 m to the side (its door next to us: getting in at once moves our state about 1.5 m, not 4 m - a puppet snap on the other game) of the player and puts the player at its wheel
local p = game.game:GetActivePlayer()
local pos, d = p:GetPos(), p:GetDir()
local model = MafiaCoop.tcarModel or "smith_v12"
local so, id = game.traffic:ObtainSpecificCar(model)
MafiaCoop.tcar = { id = id, st = "obtain" }
StartThread(function()
  Wait(so)
  local s = game.traffic:SpawnCar(id, Math:newVector(pos.x + d.y * 2.3, pos.y - d.x * 2.3, pos.z + 0.3), d)
  Wait(s)
  local car = game.traffic:GetSpawnedEntity(id)
  MafiaCoop.tcar.ent = car
  Sleep(0.5)
  p:LockControls(true)
  local e = p:GetOnVehicle(car, 1, false, "WALK")  -- at once (animated from 4 m it took up to 45 s or never)
  Wait(e)
  p:LockControls(false)
  MafiaCoop.tcar.st = "in"
end)
return "obtaining " .. model
