-- How far the host's world reaches (tools/onepc run_test.ps1 "suppress", docs/host-authority.md section 3 / D1): his
-- own moving traffic cars (a generator human at the wheel) and ambient humans (AI 0, on foot, no mission spawner's
-- name) per 50 m band from his player, and his pedestrian budget (the value a guest's ini may name as
-- guest_ped_budget). Tells whether the leash's 300 / 400 m is right: where the bands run empty the guest's streets do
-- too. One line back.
local M = MafiaCoop
local p = M and M.Player and M.Player()
if not p then return "no player in the world" end
local me = p:GetPos()
local function valid(o) return o ~= nil and IsObjectValid(o) end
local function nameOf(h)
  local good, n = pcall(h.GetName, h)
  return good and type(n) == "string" and n or ""
end
local function activeOf(e)
  local good, a = pcall(e.IsActive, e)
  return not good or a ~= false
end
local STEP, LAST = 50, 400
local cars, peds = {}, {}
local n = LAST / STEP + 1
for i = 1, n do cars[i], peds[i] = 0, 0 end
local function band(t, q)
  local d = math.sqrt((q.x - me.x) ^ 2 + (q.y - me.y) ^ 2)
  local i = math.min(n, math.floor(d / STEP) + 1)
  t[i] = t[i] + 1
end
for s in string.gmatch(mc_entity_ids(10), "%d+") do
  local e = mc_entity(tonumber(s))
  if e and e.GetVehicleModelName and activeOf(e) then
    local okf, fixed = pcall(e.IsCarPosFixed, e)
    local okd, drv = pcall(e.GetSeatOwner, e, 0)
    if not (okf and fixed) and okd and valid(drv) and nameOf(drv):match("^behavior_object_prime%-%x+%-$") then
      band(cars, e:GetPos())
    end
  end
end
for s in string.gmatch(mc_entity_ids(8), "%d+") do
  local h = mc_entity(tonumber(s))
  if h and activeOf(h) and not (M.PedOwned and M.PedOwned[tonumber(s)]) then
    local suffix = nameOf(h):match("^behavior_object_prime%-%x+%-(.*)$")
    local okA, ai = pcall(h.GetAIType, h)
    local okO, o = pcall(h.GetOwner, h)
    if not (suffix and suffix ~= "") and okA and tonumber(ai) == 0 and not (okO and o) then band(peds, h:GetPos()) end
  end
end
local parts = {}
for i = 1, n do
  local lo = (i - 1) * STEP
  parts[#parts + 1] = string.format("%d%s: %d/%d", lo, i == n and "+" or "-" .. (lo + STEP), cars[i], peds[i])
end
local bud = mc_gw_budget and mc_gw_budget() or nil
return "cars/humans per band (m): " .. table.concat(parts, ", ") .. "; pedestrian budget " .. tostring(bud)
