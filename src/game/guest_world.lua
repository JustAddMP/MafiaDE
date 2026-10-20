-- MafiaCoop guest world glue (host authority, docs/host-authority.md section 1, work package A): on a guest's game the
-- game's own world is switched off for the whole session - ambient traffic and police spawning, the pedestrians - and
-- what the game already made is hidden, so that only the host's traffic, police and pedestrians (their copies) are
-- seen. Runs in the game's Lua state on GameMainThread after the other glue, called once per script tick by
-- src/game/guest_world_sync.cpp (MafiaCoop.GuestWorld.Update). Embedded into the dll; a copy at
-- <dll dir>\guest_world.lua overrides it.
--
-- The decisions are the dll's (src/game/guest_world.h, pure and tested): when to switch (the first tick of every
-- world, again every 5 s and whenever traffic_sync.lua's 0.16 region code switched ambient traffic back on), when to
-- give everything back (the session ended / we host / turned off), the fallback for the pedestrians, and for every car
-- and human this glue scans whether it may be hidden (mc_gw_judge). This glue reads the facts and does the calls.
--
-- What is never hidden (the facts sent with every entity; any of them keeps it): our puppets and the puppet factory
-- (player_sync.lua M.pup, M.fac, retired puppet entities), traffic copies and their crews (MafiaCoop.Traffic.copies),
-- ped_sync's copies, adopted / hidden humans and its copy spawner (MafiaCoop.PedOwned, MafiaCoop.Peds.fac),
-- npc_sync's copies, replicas, held NPCs and its copy spawner (MafiaCoop.Npc.copyIds / byLocal / hidden / tcrew /
-- cfac), player cars and their copies (MafiaCoop.KeyOf), the car we sit in, every fixed car (FixCarPos: every spawner),
-- a car far below us, any car while one of the factories loads its car, a crewed car at a loading traffic copy's spawn
-- spot, and anything first seen in the last 2 s. What may be hidden: a car with a traffic / police generator human at
-- the wheel (`behavior_object_prime-NN-`, empty suffix, docs/game-api.md 15) and an ambient human (AI type 0, on foot,
-- no mission spawner's name: MafiaCoop.IsAmbientHuman's rule, docs/game-api.md 26). Trams, parked cars and boats are
-- left alone this round (host-authority D4).
--
-- Input (cmds): "S active host" from the dll. The rest comes back from mc_gw_tick (the dll's Actions this tick):
--   OFF b                    switch our own world off (b 1: also the pedestrian budget 0, its old value read first)
--   ON v                     everything back (v >= 0: the budget back to v; -1: unknown, left to the next world load)
--   H mode scan              hide mode (0 none, 1 at once, 2 out of view); scan 1: scan the cars and humans now
--   BL x y z / UB x y z      BlockSpawnersInSphere(pos, 150, true / false)
-- Output (events): "SW traffic police budget" (1 ok / 0 failed / - not asked), "BR v" (the budget read, -1 not
-- readable), "RS budget" (restore done; budget 1 ok / 0 failed / - not asked), "CT cars peds hidC hidP hidNow made
-- cand" (after a scan: own moving cars within 150 m and own ambient humans within 80 m still shown, cars / humans
-- hidden now, hidden by this scan, own ambient entities seen), "ST text" (status, once a second).
MafiaCoop = MafiaCoop or {}
local M = MafiaCoop
M.GuestWorld = M.GuestWorld or {}
local GW = M.GuestWorld
GW.version = 1

local BLOCK_RADIUS = 150    -- m (the dll's kBlockRadius)
local MAX_HIDES = 60        -- hides per scan at most (the rest at the next one)
local COPY_SPOT = 8         -- m around a loading traffic copy's spawn spot: no car hidden there
-- the dll's fact bits (src/game/guest_world.h Fact)
local F = { car = 1, human = 2, active = 4, puppet = 8, pfac = 16, tcopy = 32, crew = 64, pedOwned = 128,
  pedSpawner = 256, npcCopy = 512, npcSpawner = 1024, playerCar = 2048, ownVehicle = 4096, fixed = 8192,
  facBusy = 16384, copySpot = 32768, fresh = 65536, genDriver = 131072, tram = 262144, ambient = 524288,
  mission = 1048576, inVehicle = 2097152 }

local function log(s) if mc_log then mc_log("guest world: " .. s) end end
local function valid(o) return o ~= nil and IsObjectValid(o) end
local function same(a, b) return valid(a) and valid(b) and a == b end
local function V(x, y, z) return Math:newVector(x, y, z) end
local function idOf(o)
  local good, g = pcall(o.GetGUID, o)
  if not good then return nil end
  return tonumber(tostring(g):match("(%d+)%s*$"))
end
-- v with bit b set (no bit library in the game's Lua: sums of distinct powers of two)
local function bor(v, b)
  if math.floor(v / b) % 2 == 1 then return v end
  return v + b
end
local function active(e)
  local good, a = pcall(e.IsActive, e)
  return not good or a ~= false
end

GW.hidden = GW.hidden or {}  -- entity id -> { e, car = true/false, crew = {}, phys }
GW.seen = GW.seen or {}      -- entity id -> when first seen (the "fresh" rule)
GW.made = GW.made or 0       -- hidden so far this session
GW.shownBack = GW.shownBack or 0

-- ---- where we are -------------------------------------------------------------------------------------------------
-- 0 no world (menus, a world torn down), 1 the loading screen, 2 in the world (player_sync.lua M.Player()).
function GW.Phase()
  local okp, p = pcall(game.game.GetActivePlayer, game.game)
  if not okp or not p then return 0, "" end
  local oks, sid = pcall(game.game.GetLoadedSaveId, game.game)
  if not oks or sid == nil or sid == "!none!" then return 0, "" end
  local okm, menu = pcall(game.game.GetIs3DMenu, game.game)
  if okm and menu then return 0, "" end
  local okl, loading = pcall(game.hud.IsLoadingScreenUp, game.hud)
  if okl and loading then return 1, tostring(sid) .. "|loading" end
  local me = M.Player and M.Player()
  if me then return 2, tostring(sid) .. "|" .. tostring(M.inWorldAt) end
  return 0, ""
end

local function myPos(p)
  local q = p:GetPos()
  local okc, car = pcall(p.GetOwner, p)
  if okc and car then
    local okq, cq = pcall(car.GetPos, car)
    if okq and cq then q = cq end
  end
  return q
end

-- ---- the facts ------------------------------------------------------------------------------------------------------
local function nameOf(h)
  local good, n = pcall(h.GetName, h)
  return good and type(n) == "string" and n or ""
end

-- "" (no spawner name), "generator" (behavior_object_prime-NN-), "mission" (with a suffix)
local function nameClass(n)
  local suffix = n:match("^behavior_object_prime%-%x+%-(.*)$")
  if not suffix then return "" end
  return suffix == "" and "generator" or "mission"
end

-- What belongs to us (the other glue's tables), collected once per scan: ids -> fact bit.
function GW.Ours(p)
  local cars, humans = {}, {}
  local function addCar(e, bit) if valid(e) then local id = idOf(e) if id then cars[id] = bor(cars[id] or 0, bit) end end end
  local function addHuman(e, bit) if valid(e) then local id = idOf(e) if id then humans[id] = bor(humans[id] or 0, bit) end end end
  local function addId(id, bit) if id then humans[id] = bor(humans[id] or 0, bit) end end
  -- player_sync: puppets, the puppet factory, player cars and their copies
  for _, pp in pairs(M.pup or {}) do addHuman(pp.so, F.puppet) end
  local f = M.fac
  if f then addCar(f.car, F.pfac) addHuman(f.drv, F.pfac) end
  for _, c in pairs(M.cars or {}) do addCar(c.ent, F.playerCar) addCar(c.bso, F.playerCar) end
  local okc, mine = pcall(p.GetOwner, p)
  if okc and mine then addCar(mine, F.ownVehicle) end
  -- traffic_sync: the copies of the host's cars and their crews
  local T = M.Traffic
  for _, c in pairs(T and T.copies or {}) do
    addCar(c.ent, F.tcopy)
    for _, m in ipairs(c.crew or {}) do addHuman(m, F.crew) end
  end
  -- ped_sync: its copies, adopted and hidden humans, its spawner's driver (PedOwned), its spawner car
  for id, _ in pairs(M.PedOwned or {}) do addId(id, F.pedOwned) end
  local P = M.Peds
  if P and P.fac then addCar(P.fac.car, F.pedSpawner) addHuman(P.fac.drv, F.pedSpawner) end
  -- npc_sync: copies, replicas, held / hidden NPCs, traffic copy crews it knows, its copy spawner
  local N = M.Npc
  if N then
    for id, _ in pairs(N.copyIds or {}) do addId(id, F.npcCopy) end
    for id, _ in pairs(N.byLocal or {}) do addId(id, F.npcCopy) end
    for id, _ in pairs(N.hidden or {}) do addId(id, F.npcCopy) end
    for id, _ in pairs(N.tcrew or {}) do addId(id, F.crew) end
    if N.cfac then
      addCar(N.cfac.car, F.npcSpawner)
      addHuman(N.cfac.drv, F.npcSpawner)
      addId(N.cfac.drvId, F.npcSpawner)
    end
  end
  -- a factory loading its car right now (the car exists before the factory knows it: no car hidden meanwhile)
  local busy = false
  local facs = { M.fac or false, N and N.cfac or false, P and P.fac or false }
  for i = 1, 3 do
    local fac = facs[i]
    if fac and (fac.st == "obtain" or fac.st == "spawn") then busy = true end
  end
  -- traffic copies being spawned: their spots
  local spots = {}
  for _, c in pairs(T and T.copies or {}) do
    if (c.st == "obtain" or c.st == "spawn") and c.tx then spots[#spots + 1] = { x = c.tx, y = c.ty, z = c.tz or 0 } end
  end
  return cars, humans, busy, spots
end

local function speedOf(e)
  local good, s = pcall(e.GetSpeed, e)
  if not (good and s) then return 0 end
  return math.sqrt(s.x * s.x + s.y * s.y + s.z * s.z)
end

local function crewOf(car)
  local out = {}
  for s = 0, 3 do
    local good, h = pcall(car.GetSeatOwner, car, s)
    if good and valid(h) then out[#out + 1] = h end
  end
  return out
end

local function carFacts(e, id, ours, busy, spots, now)
  local f = F.car + (ours[id] or 0)
  if active(e) then f = f + F.active end
  local okf, fixed = pcall(e.IsCarPosFixed, e)
  if okf and fixed then f = f + F.fixed end
  if busy then f = f + F.facBusy end
  local q = e:GetPos()
  for _, s in ipairs(spots) do
    if (q.x - s.x) ^ 2 + (q.y - s.y) ^ 2 <= COPY_SPOT * COPY_SPOT then f = f + F.copySpot break end
  end
  if now - (GW.seen[id] or now) < 2 then f = f + F.fresh end
  local okd, drv = pcall(e.GetSeatOwner, e, 0)
  if okd and valid(drv) and nameClass(nameOf(drv)) == "generator" then f = f + F.genDriver end
  local okm, model = pcall(e.GetVehicleModelName, e)
  model = okm and tostring(model or ""):lower() or ""
  if model:find("tram") or model:find("train") or model:find("boat") then f = f + F.tram end
  return f, q
end

local function humanFacts(h, id, ours, now)
  local f = F.human + (ours[id] or 0)
  if M.IsRetired and M.IsRetired(id) then f = bor(f, F.puppet) end
  if active(h) then f = f + F.active end
  if now - (GW.seen[id] or now) < 2 then f = f + F.fresh end
  local okO, o = pcall(h.GetOwner, h)
  local inVeh = okO and o ~= nil
  if inVeh then f = f + F.inVehicle end
  local name = nameOf(h)
  local cls = nameClass(name)
  if cls == "mission" then f = f + F.mission end
  local okA, ai = pcall(h.GetAIType, h)
  local ai0 = okA and tonumber(ai) == 0
  local amb
  if mc_npc_class then
    amb = ai0 and not inVeh and mc_npc_class(name, 0, 0, 0) == 1
  else
    amb = ai0 and not inVeh and cls ~= "mission"
  end
  if amb then f = f + F.ambient end
  return f, h:GetPos()
end

-- ---- hiding -----------------------------------------------------------------------------------------------------------
-- Did guest_world hide this car / human (entity `e`, or its id)? For player_sync.lua (FindLocalCar: a hidden leftover
-- car is not adopted) and any other glue. A hidden car's crew counts as hidden too.
function GW.IsHidden(e, id)
  id = id or (e and valid(e) and idOf(e)) or nil
  if id and GW.hidden[id] then return true end
  if e then
    for _, x in pairs(GW.hidden) do
      if x.e == e then return true end
      for _, m in ipairs(x.crew or {}) do
        if m == e then return true end
      end
    end
  end
  return false
end

function GW.HideCar(e, id)
  local crew = crewOf(e)
  for _, m in ipairs(crew) do pcall(m.Deactivate, m) end
  local ok = pcall(e.Deactivate, e)
  GW.hidden[id] = { e = e, car = true, crew = crew }
  return ok
end

function GW.HideHuman(h, id)
  local okp, phys = pcall(h.GetPhysState, h)
  pcall(h.ShowModel, h, false)
  local ok = pcall(h.Deactivate, h)
  GW.hidden[id] = { e = h, car = false, crew = {}, phys = okp and phys or nil }
  return ok
end

function GW.ShowAll()
  local n = 0
  for id, x in pairs(GW.hidden) do
    if valid(x.e) then
      if x.car then
        pcall(x.e.Activate, x.e)
        for _, m in ipairs(x.crew or {}) do
          if valid(m) then pcall(m.Activate, m) end
        end
      else
        pcall(x.e.Activate, x.e)
        pcall(x.e.ShowModel, x.e, true)
        if x.phys ~= nil then pcall(x.e.SetPhysState, x.e, x.phys) end
      end
      n = n + 1
    end
    GW.hidden[id] = nil
  end
  GW.shownBack = GW.shownBack + n
  return n
end

-- One scan of every car and human: the verdicts (mc_gw_judge), the hides, the counts.
function GW.Scan(p, mode, ev)
  if not (mc_entity_ids and mc_entity and mc_gw_judge) then return end
  local now = M.now or 0
  local me = myPos(p)
  local cam, cd
  local okc, c = pcall(function() return game.cameramanager:GetPlayerCamera() end)
  if okc and c then
    local okp, cp = pcall(c.GetPos, c)
    local okd, dd = pcall(c.GetDir, c)
    if okp and okd and cp and dd then cam, cd = cp, dd end
  end
  if mc_gw_view then
    if cam then
      mc_gw_view(1, cam.x, cam.y, cam.z, cd.x, cd.y, cd.z, 1, me.x, me.y, me.z)
    else
      mc_gw_view(0, 0, 0, 0, 0, 0, 0, 1, me.x, me.y, me.z)
    end
  end
  local cars, humans, busy, spots = GW.Ours(p)
  local seen = {}
  local ownCars, ownPeds, made, cand = 0, 0, 0, 0
  -- one entity (inside a pcall: one that errors is skipped, the scan goes on)
  local function one(e, id, car)
    local facts, q, speed
    if car then
      facts, q = carFacts(e, id, cars, busy, spots, now)
      speed = speedOf(e)
    else
      facts, q = humanFacts(e, id, humans, now)
      speed = 0
    end
    local v, counts = mc_gw_judge(facts, q.x, q.y, q.z, speed)
    if v == 2 or v == 3 then cand = cand + 1 end
    local hidden = false
    if v == 3 and made < MAX_HIDES then
      if car then hidden = GW.HideCar(e, id) else hidden = GW.HideHuman(e, id) end
      made = made + 1
    end
    if counts == 1 and not hidden then
      if car then ownCars = ownCars + 1 else ownPeds = ownPeds + 1 end
    end
  end
  for _, kind in ipairs({ 10, 8 }) do
    for s in string.gmatch(mc_entity_ids(kind), "%d+") do
      local id = tonumber(s)
      if id then
        seen[id] = GW.seen[id] or now
        local e = not GW.hidden[id] and mc_entity(id)
        if e and (kind == 8 or e.GetVehicleModelName) then
          local good, err = pcall(one, e, id, kind == 10)
          if not good then
            GW.errs = (GW.errs or 0) + 1
            if GW.errs <= 20 then log("entity " .. id .. ": " .. tostring(err)) end
          end
        end
      end
    end
  end
  GW.seen = seen
  -- hidden ones the game removed meanwhile
  local hc, hp = 0, 0
  for id, x in pairs(GW.hidden) do
    if not valid(x.e) then
      GW.hidden[id] = nil
    elseif x.car then
      hc = hc + 1
    else
      hp = hp + 1
    end
  end
  GW.made = GW.made + made
  GW.counts = { ownCars, ownPeds, hc, hp }
  ev[#ev + 1] = string.format("CT %d %d %d %d %d %d %d", ownCars, ownPeds, hc, hp, made, GW.made, cand)
end

-- ---- the switches -------------------------------------------------------------------------------------------------
function GW.Off(budget, ev)
  local t = game.traffic
  local a = pcall(t.SetEnableAmbientTrafficSpawning, t, false)
  local b = pcall(game.police.PreventPoliceSpawning, game.police, true)
  local c = "-"
  if budget then
    local v = mc_gw_budget and mc_gw_budget() or nil
    ev[#ev + 1] = "BR " .. tostring(v and math.floor(v) or -1)
    c = pcall(t.SetMaxHumanElements, t, 0) and "1" or "0"
  end
  ev[#ev + 1] = string.format("SW %d %d %s", a and 1 or 0, b and 1 or 0, c)
  GW.off = true
end

function GW.On(budget, ev)
  local t = game.traffic
  pcall(t.SetEnableAmbientTrafficSpawning, t, true)
  pcall(game.police.PreventPoliceSpawning, game.police, false)
  local c = "-"
  if budget and budget >= 0 then c = pcall(t.SetMaxHumanElements, t, budget) and "1" or "0" end
  local n = GW.ShowAll()
  ev[#ev + 1] = "RS " .. c .. " " .. n
  GW.off = false
end

function GW.Block(on, x, y, z)
  local ok = pcall(game.traffic.BlockSpawnersInSphere, game.traffic, V(x, y, z), BLOCK_RADIUS, on)
  if not ok then GW.blockFails = (GW.blockFails or 0) + 1 end
  GW.blocks = (GW.blocks or 0) + (on and 1 or 0)
end

-- ---- update -----------------------------------------------------------------------------------------------------------
function GW.Update(dt, cmds)
  local ev = {}
  local phase, key = GW.Phase()
  local p = M.Player and M.Player()
  local x, y, z, have = 0, 0, 0, 0
  if p then
    local q = myPos(p)
    x, y, z, have = q.x, q.y, q.z, 1
  end
  -- traffic_sync.lua's 0.16 region code switches ambient traffic back on in "our own region": noticed, the dll
  -- switches it off again at once (B's host-traffic package removes that code)
  local T = M.Traffic
  local other = 0
  if T and T.localOn == true and GW.lastLocalOn ~= true then other = 1 end
  GW.lastLocalOn = T and T.localOn
  if not mc_gw_tick then return "" end
  local acts = mc_gw_tick(phase, key, have, x, y, z, other) or ""
  local mode, scan = 0, false
  for line in string.gmatch(acts, "[^\n]+") do
    local f = {}
    for w in string.gmatch(line, "%S+") do f[#f + 1] = w end
    local op = f[1]
    local good, err = true, nil
    if op == "OFF" then
      good, err = pcall(GW.Off, f[2] == "1", ev)
    elseif op == "ON" then
      good, err = pcall(GW.On, tonumber(f[2]) or -1, ev)
    elseif op == "H" then
      mode, scan = tonumber(f[2]) or 0, f[3] == "1"
    elseif op == "BL" or op == "UB" then
      good, err = pcall(GW.Block, op == "BL", tonumber(f[2]) or 0, tonumber(f[3]) or 0, tonumber(f[4]) or 0)
    end
    if not good then
      GW.errs = (GW.errs or 0) + 1
      if GW.errs <= 20 then log(op .. " error: " .. tostring(err)) end
    end
  end
  if scan and mode > 0 and phase ~= 0 then
    -- while the loading screen is up the player may not be placed yet: positions from the active player
    local pl = p
    if not pl then
      local okp, ap = pcall(game.game.GetActivePlayer, game.game)
      pl = okp and ap or nil
    end
    if pl then
      local good, err = pcall(GW.Scan, pl, mode, ev)
      if not good then
        GW.errs = (GW.errs or 0) + 1
        if GW.errs <= 20 then log("scan error: " .. tostring(err)) end
      end
    end
  end
  if (M.now or 0) - (GW.stT or -10) >= 1 then
    GW.stT = M.now or 0
    local c = GW.counts or { 0, 0, 0, 0 }
    ev[#ev + 1] = string.format("ST %s, own near: %d cars, %d humans; hidden %d cars, %d humans (%d so far)",
      GW.off and "own world off" or "own world on", c[1], c[2], c[3], c[4], GW.made)
  end
  return table.concat(ev, "\n")
end

return GW.version
