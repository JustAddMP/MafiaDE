-- The guest's own world right now (tools/onepc run_test.ps1 "suppress", guest_world.lua / docs/host-authority.md WP-A):
-- his own moving traffic cars (a generator human at the wheel) and his own ambient humans by distance from him (both
-- must be 0 once 10 s in the world: hidden or never spawned), how many guest_world hid, whether the switches stand
-- and the pedestrian budget now; and whether what must still spawn does: the puppets (player_sync's factory), the
-- pedestrian copies (ped_sync's copy spawner), npc_sync's copies and the mission spawners' humans. One line back.
local M = MafiaCoop
if not M then return "no MafiaCoop" end
local GW = M.GuestWorld
local p = M.Player and M.Player()
if not p then return "no player in the world" end
local me = p:GetPos()
local function valid(o) return o ~= nil and IsObjectValid(o) end
local function d2(q) return math.sqrt((q.x - me.x) ^ 2 + (q.y - me.y) ^ 2) end
local function idOf(o)
  local good, g = pcall(o.GetGUID, o)
  return good and tonumber(tostring(g):match("(%d+)%s*$")) or nil
end
local function nameOf(h)
  local good, n = pcall(h.GetName, h)
  return good and type(n) == "string" and n or ""
end
local function activeOf(e)
  local good, a = pcall(e.IsActive, e)
  return not good or a ~= false
end
local oursCars, oursHumans = {}, {}
if GW and GW.Ours then
  local good, c, h = pcall(GW.Ours, p)
  if good then oursCars, oursHumans = c or {}, h or {} end
end
local bands = { { 0, 50 }, { 50, 100 }, { 100, 150 }, { 150, 1e9 } }
local cars, peds = {}, {}
for i = 1, #bands do cars[i], peds[i] = 0, 0 end
local function band(t, d)
  for i, b in ipairs(bands) do
    if d >= b[1] and d < b[2] then t[i] = t[i] + 1 return end
  end
end
local mission = 0
for s in string.gmatch(mc_entity_ids(10), "%d+") do
  local id = tonumber(s)
  local e = id and mc_entity(id)
  if e and e.GetVehicleModelName and activeOf(e) and not oursCars[id] then
    local okf, fixed = pcall(e.IsCarPosFixed, e)
    local okd, drv = pcall(e.GetSeatOwner, e, 0)
    if not (okf and fixed) and okd and valid(drv) and nameOf(drv):match("^behavior_object_prime%-%x+%-$") then
      local okv, v = pcall(e.GetSpeed, e)
      if okv and v and math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z) > 1 then band(cars, d2(e:GetPos())) end
    end
  end
end
for s in string.gmatch(mc_entity_ids(8), "%d+") do
  local id = tonumber(s)
  local h = id and mc_entity(id)
  if h and activeOf(h) and not oursHumans[id] and not (M.PedOwned and M.PedOwned[id]) then
    local n = nameOf(h)
    local suffix = n:match("^behavior_object_prime%-%x+%-(.*)$")
    if suffix and suffix ~= "" then
      mission = mission + 1
    else
      local okA, ai = pcall(h.GetAIType, h)
      local okO, o = pcall(h.GetOwner, h)
      if okA and tonumber(ai) == 0 and not (okO and o) then band(peds, d2(h:GetPos())) end
    end
  end
end
local pup, pupAlive = 0, 0
for _, pp in pairs(M.pup or {}) do
  pup = pup + 1
  if pp.so and valid(pp.so) then pupAlive = pupAlive + 1 end
end
local P = M.Peds
local pedCopies = 0
for _, r in pairs(P and P.reps or {}) do
  if r.how == "spawned" and r.h and valid(r.h) then pedCopies = pedCopies + 1 end
end
local N = M.Npc
local npcCopies = 0
for _, c in pairs(N and N.copies or {}) do
  if c.st == "alive" then npcCopies = npcCopies + 1 end
end
local hc, hp = 0, 0
for _, x in pairs(GW and GW.hidden or {}) do
  if x.car then hc = hc + 1 else hp = hp + 1 end
end
local function list(t)
  local parts = {}
  for i, b in ipairs(bands) do
    parts[#parts + 1] = string.format("%d-%s m %d", b[1], b[2] < 1e9 and tostring(b[2]) or "", t[i])
  end
  return table.concat(parts, ", ")
end
local bud = mc_gw_budget and mc_gw_budget() or nil
return string.format("own moving cars: %s; own ambient humans: %s; hidden now %d cars / %d humans (%d so far); "
  .. "switches %s, budget %s; puppets %d of %d, factory %s; ped copies %d, spawner %s; npc copies %d, spawner %s; "
  .. "mission humans %d", list(cars), list(peds), hc, hp, GW and GW.made or 0, GW and GW.off and "off" or "ON",
  tostring(bud), pupAlive, pup, tostring(M.fac and M.fac.st), pedCopies, tostring(P and P.fac and P.fac.st),
  npcCopies, tostring(N and N.cfac and N.cfac.st), mission)
