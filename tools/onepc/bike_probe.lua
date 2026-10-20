-- what the player's owner / owner scene object and the test bike answer to
local out = {}
local function add(s) out[#out + 1] = tostring(s) end
local function has(o, n) local ok, f = pcall(function() return o[n] end) return ok and f ~= nil end
local names = { "GetGUID", "GetName", "GetPos", "GetRot", "GetDir", "GetSpeed", "GetType", "GetComponent", "GetSeatOwner",
  "GetVehicleModelName", "SetPos", "SetRot", "Activate", "Deactivate", "GetEntity", "GetOwner", "GetSceneObject",
  "GetVehicle", "GetEntityGUID", "GetId", "GetID", "IsActive", "SetSpeed", "SetDespawnImmunity", "GetPassengers",
  "FindSeatForHuman", "GetOwnerSceneObject", "GetParent", "GetTransform", "GetMatrix" }
local function desc(label, o)
  if o == nil then add(label .. ": nil") return end
  local t = {}
  for _, n in ipairs(names) do if has(o, n) then t[#t + 1] = n end end
  local okv, v = pcall(IsObjectValid, o)
  add(label .. ": " .. tostring(o) .. " type(" .. type(o) .. ") valid=" .. tostring(okv and v) .. " has: " .. table.concat(t, " "))
  local okg, g = pcall(function() return o:GetGUID() end)
  if okg then add("  GUID " .. tostring(g)) end
  local okp, q = pcall(function() return o:GetPos() end)
  if okp and q then add(string.format("  pos %.2f %.2f %.2f", q.x, q.y, q.z)) end
  local okn, nm = pcall(function() return o:GetName() end)
  if okn then add("  name " .. tostring(nm)) end
end
local p = game.game:GetActivePlayer()
desc("player", p)
local ok1, own = pcall(p.GetOwner, p)
desc("player:GetOwner()", ok1 and own or nil)
local ok2, oso = pcall(function() return p:GetOwnerSceneObject() end)
desc("player:GetOwnerSceneObject()", ok2 and oso or nil)
if ok2 and oso then
  local okc, mc = pcall(function() return oso:GetComponent("C_RuntimeMotorcycleComponent") end)
  desc("  oso moto component", okc and mc or nil)
end
local b = MafiaCoop.tbike
if b then
  add("tbike st " .. tostring(b.st) .. " id " .. tostring(b.id))
  desc("tbike.ent", b.ent)
  desc("tbike.mc", b.mc)
  if b.ent then
    local okc, mc = pcall(function() return b.ent:GetComponent("C_RuntimeMotorcycleComponent") end)
    desc("  ent moto component", okc and mc or nil)
    for s = 0, 1 do
      local oks, h = pcall(function() return b.ent:GetSeatOwner(s) end)
      add("  ent seat " .. s .. ": " .. tostring(oks) .. " " .. tostring(h))
      if b.mc then
        local okm, hm = pcall(function() return b.mc:GetSeatOwner(s) end)
        add("  mc seat " .. s .. ": " .. tostring(okm) .. " " .. tostring(hm))
      end
    end
    if ok2 and oso then add("  oso == ent: " .. tostring(oso == b.ent)) end
  end
end


return table.concat(out, "\n")
