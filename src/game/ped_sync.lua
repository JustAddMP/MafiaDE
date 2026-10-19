-- MafiaCoop pedestrian glue (ambient pedestrians shared). Runs in the game's Lua state on GameMainThread after
-- player_sync.lua, npc_sync.lua and traffic_sync.lua, called once per script tick by src/game/ped_sync.cpp
-- (MafiaCoop.Peds.Update). Embedded into the dll; a copy at <dll dir>\ped_sync.lua overrides it.
--
-- Every game is the authority for the ambient pedestrians where it simulates the ambient world (core RegionAuthority:
-- the host within 250 m of him, elsewhere the nearest player, with hysteresis; decided in C++, game/ped_model.h).
-- This glue reads this game's ambient humans (MafiaCoop.IsAmbientHuman) near any player 20 times a second for the C++
-- side, which sends the ones near each other player. As a receiver it carries out what the C++ side decides: a local
-- ambient human is adopted as the replica of another game's pedestrian (its own wandering interrupted, walked like a
-- puppet: MoveVec with the sender's gait, speed multiplier, nudges and snaps of core's LocomotionController), a
-- shortfall gets a spawned copy (a spawner of our own, the recipe of player_sync.lua's puppet factory, one civilian
-- profile), a surplus local human is hidden while nobody looks (ShowModel false, Deactivate) and shown again later.
-- Deaths follow the authority (a replica is killed); a replica that dies here by our hand is reported (PedKill).
--
-- Contract with npc_sync.lua (package npc-authority): an ambient human is not a player / puppet / any copy, not in a
-- vehicle, AI type 0, and not named after a mission spawner (`behavior_object_prime-NN-<suffix>` with a non-empty
-- suffix, docs/game-api.md 15). Those are ours on every game: npc_sync skips them (MafiaCoop.IsAmbientHuman(h)), and
-- never pairs or steers a human in MafiaCoop.PedOwned (entity id -> true): every local human this glue controls
-- (adopted and spawned replicas, hidden humans, its spawner's driver).
--
-- Input (cmds, one line per command):
--   S active host localId                  session running, we host, our player id
--   A key id x y z hx hy                   local human `id` is the replica of pedestrian `key` (authority << 16 | net)
--   N key x y z hx hy                      spawn a copy for `key` there
--   R key how local                        release `key`'s replica (how: adopted / spawned / none)
--   H id                                   hide local human `id`
--   W id                                   show local human `id` again
--   D key gait mult reissue mx my mz fix nx ny turn fx fy tx ty tz hx hy flags
--                                          drive `key`'s replica (core LocomotionCommand; t = the target, flags as
--                                          PedStates: 1 dead 2 panic 4 crouch 8 running)
--   K key local                            `key`'s replica dies (the authority's died)
--   KL id                                  our own pedestrian `id` dies (a receiver killed his replica of it)
--   B 1|0                                  keep our pedestrian spawners around us blocked (another's pedestrians show)
-- Output (events, one per line):
--   PP id x y z inWorld                    every player (us too): where he is (his car while he drives)
--   PC x y z dx dy dz                      our camera
--   PH id x y z hx hy flags look           an ambient human of ours near a player (flags: 1 dead, 2 panic, 4 crouch)
--   PE                                     end of a round of PH lines
--   PR key x y z hx hy dead                where a replica is, after this tick's commands
--   PSR key id / PSF key why               a spawned copy is in the world (its id) / failed
--   PGL id                                 a local human we controlled (replica / hidden) is gone (the game)
--   PW                                     a new world: everything of the old one is gone
--   ST text                                status for the overlay (once a second)
MafiaCoop = MafiaCoop or {}
local M = MafiaCoop
M.Peds = M.Peds or {}
local P = M.Peds
P.version = 1
M.PedOwned = M.PedOwned or {}

local SCAN_INTERVAL = 0.25   -- s between scans of the human table
local ROUND_INTERVAL = 0.05  -- s between rounds of positions for the C++ side
local REPORT_RANGE = 75      -- m from any player: our ambient humans reported (the C++ side sends within 50 m)
local MAX_REPORT = 200       -- humans per round
local RECHECK = 5            -- s: a human found not ambient is checked again after this long
local FACTORY_CAR = "shubert_e_six"
local FACTORY_PROFILE = "visuals/hero/game_main_tommy_gangster_trenchcoat"
-- The copies' spawn profile: the game's own ambient civilian profile (its hash is in spawnprofiles.sds, docs/game-api.md
-- 19; npc_sync.lua uses it as the civilian fallback). The authority's pedestrian's own look is not readable: every
-- spawned copy looks like this. UNVERIFIED in game that it loads through a runtime spawner.
local COPY_PROFILE = "characters/traffic/traffic_base_ambient_combined"
local LOAD_TIMEOUT = 12      -- s a profile may take to load
local FACTORY_IDLE = 30      -- s without a spawned copy or request: the spawner goes
local BLOCK_RADIUS = 60      -- m: spawners blocked around us (BlockSpawnersInSphere, UNVERIFIED for pedestrians)
local BLOCK_STEP = 20        -- m: blocked again when we moved this far from every blocked spot
local BLOCK_MAX = 40         -- spots remembered (to unblock them later)
local GAITS = { "WALK", "RUN", "SPRINT" }

local function log(s) if mc_log then mc_log("peds: " .. s) end end
local function valid(o) return o ~= nil and IsObjectValid(o) end
local function V(x, y, z) return Math:newVector(x, y, z) end
local function idOf(o)
  local good, g = pcall(o.GetGUID, o)
  if not good then return nil end
  return tonumber(tostring(g):match("(%d+)%s*$"))
end
local errs = {}
local function try(what, f, ...)
  local good, a, b = pcall(f, ...)
  if not good then
    errs[what] = (errs[what] or 0) + 1
    if errs[what] <= 5 then log(what .. " failed: " .. tostring(a)) end
    return nil
  end
  return a, b
end
local function bit(v, b) return math.floor((tonumber(v) or 0) / b) % 2 == 1 end
local function active(h)
  local good, a = pcall(h.IsActive, h)
  return not good or a ~= false
end
local function trace(t, s) if mc_trace then mc_trace(t, s) end end

P.amb = P.amb or {}      -- our ambient humans: id -> { h, id, look, flags }
P.notAmb = P.notAmb or {}  -- id -> time it was found not ambient (checked again after RECHECK)
P.reps = P.reps or {}    -- key -> { key, how = "adopted" | "spawning" | "spawned", h, id, ... }
P.hidden = P.hidden or {}  -- id -> { h, phys }
P.queue = P.queue or {}  -- spawn requests: { key, x, y, z, hx, hy }
P.fac = P.fac or { st = "none", t = 0 }
P.blocks = P.blocks or {}  -- spots our spawners were blocked around

-- Humans that are not ambient on this game: the puppets, player_sync's factory driver, npc_sync's copies and their
-- spawner's driver, traffic copy crews, our own (M.PedOwned).
function P.OurSet()
  if P.ourSetT == M.now and P.ourSet then return P.ourSet end
  local set = {}
  for _, pp in pairs(M.pup or {}) do
    if pp.so and valid(pp.so) then set[idOf(pp.so) or -1] = true end
  end
  local f = M.fac
  if f and f.drv and valid(f.drv) then set[idOf(f.drv) or -1] = true end
  local N = M.Npc
  if N then
    for id, _ in pairs(N.copyIds or {}) do set[id] = true end
    if N.cfac and N.cfac.drvId then set[N.cfac.drvId] = true end
    for id, _ in pairs(N.tcrew or {}) do set[id] = true end
  end
  local T = M.Traffic
  for _, c in pairs(T and T.copies or {}) do
    for _, m in ipairs(c.crew or {}) do
      if valid(m) then set[idOf(m) or -1] = true end
    end
  end
  P.ourSet, P.ourSetT = set, M.now
  return set
end

-- The name / AI / vehicle part of the contract (the "not ours" part is P.OurSet + M.PedOwned).
local function ambientKind(h)
  local okA, a = pcall(h.GetAIType, h)
  if not okA or tonumber(a) ~= 0 then return false end
  local okO, o = pcall(h.GetOwner, h)
  if okO and o then return false end  -- in a vehicle
  local okN, name = pcall(h.GetName, h)
  if okN and type(name) == "string" then
    local suffix = name:match("^behavior_object_prime%-%x+%-(.*)$")
    if suffix and suffix ~= "" then return false end  -- a mission spawner's (or a factory's) human
  end
  return true
end

-- Is h an ambient human of this game (ped_sync's, never npc_sync's)? See the contract above.
function M.IsAmbientHuman(h)
  if not valid(h) then return false end
  local id = idOf(h)
  if not id or id % 256 ~= 8 then return false end  -- humans (players are 9)
  if M.PedOwned[id] then return false end
  if M.IsRetired and M.IsRetired(id) then return false end
  if P.OurSet()[id] then return false end
  return ambientKind(h)
end

local function lookOf(h)
  local good, a = pcall(h.GetCivilArchetype, h)
  if not good or a == nil then return 0 end
  local s = tostring(a)
  local n = 5381
  for i = 1, #s do n = (n * 33 + string.byte(s, i)) % 4294967296 end
  return n
end

local function flagsOf(h)
  local f = 0
  local okR, r = pcall(h.GetCurrentReactionPriority, h)
  if okR and (tonumber(r) or 0) > 0 then f = f + 2 end  -- UNVERIFIED that this means panic
  local okC, c = pcall(h.IsCoverCrouch, h)
  if okC and c then f = f + 4 end
  return f
end

-- ---- our ambient humans -----------------------------------------------------------------------------------------------
function P.Scan()
  if not mc_entity_ids then return end
  local ourSet = P.OurSet()
  local seen = {}
  for s in string.gmatch(mc_entity_ids(8), "%d+") do
    local id = tonumber(s)
    if id and not M.PedOwned[id] and not ourSet[id] and not (M.IsRetired and M.IsRetired(id))
        and not (P.notAmb[id] and M.now - P.notAmb[id] < RECHECK) then
      local e = P.amb[id]
      local h = e and valid(e.h) and e.h or mc_entity(id)
      if h and active(h) and ambientKind(h) then
        seen[id] = true
        if not e or e.h ~= h then
          e = { h = h, id = id, look = lookOf(h) }
          P.amb[id] = e
        end
        e.flags = flagsOf(h)
        P.notAmb[id] = nil
      elseif h then
        P.notAmb[id] = M.now
      end
    end
  end
  for id, _ in pairs(P.amb) do
    if not seen[id] then P.amb[id] = nil end
  end
  local n = 0
  for _, _ in pairs(P.amb) do n = n + 1 end
  P.ambCount = n
end

local function myPos(p)
  local q = p:GetPos()
  local okc, car = pcall(p.GetOwner, p)
  if okc and car then
    local okp, cq = pcall(car.GetPos, car)
    if okp and cq then q = cq end
  end
  return q
end

function P.Round(p, ev)
  local me = myPos(p)
  local vps = { { x = me.x, y = me.y } }
  ev[#ev + 1] = string.format("PP %s %.2f %.2f %.2f 1", tostring(P.localId or M.localId), me.x, me.y, me.z)
  for id, pp in pairs(M.pup or {}) do
    local x, y, z
    if pp.veh and pp.cx then
      x, y, z = pp.cx, pp.cy, pp.cz
    elseif pp.tx then
      x, y, z = pp.tx, pp.ty, pp.tz
    end
    if x then
      ev[#ev + 1] = string.format("PP %s %.2f %.2f %.2f 1", tostring(id), x, y, z)
      vps[#vps + 1] = { x = x, y = y }
    end
  end
  local okc, cam = pcall(function() return game.cameramanager:GetPlayerCamera() end)
  if okc and cam then
    local okp, cp = pcall(cam.GetPos, cam)
    local okd, cd = pcall(cam.GetDir, cam)
    if okp and okd and cp and cd then
      ev[#ev + 1] = string.format("PC %.2f %.2f %.2f %.4f %.4f %.4f", cp.x, cp.y, cp.z, cd.x, cd.y, cd.z)
    end
  end
  local n = 0
  for id, e in pairs(P.amb) do
    local h = e.h
    if valid(h) and n < MAX_REPORT then
      local q = h:GetPos()
      local near = false
      for _, v in ipairs(vps) do
        if (q.x - v.x) ^ 2 + (q.y - v.y) ^ 2 <= REPORT_RANGE * REPORT_RANGE then near = true break end
      end
      if near then
        local d = h:GetDir()
        local okD, dead = pcall(h.IsDeath, h)
        local flags = (e.flags or 0) + ((okD and dead) and 1 or 0)
        ev[#ev + 1] = string.format("PH %d %.3f %.3f %.3f %.4f %.4f %d %.0f", id, q.x, q.y, q.z, d.x, d.y, flags,
          e.look or 0)
        n = n + 1
      end
    end
  end
  ev[#ev + 1] = "PE"
  P.reported = n
end

local function kill(h)
  pcall(function() h.invulnerability = false end)
  pcall(h.SetDemigod, h, false)
  pcall(h.DoDamage, h, 100000)
end

-- ---- replicas ---------------------------------------------------------------------------------------------------------
-- A local human under our control: its own wandering and reactions are interrupted (UNVERIFIED which of these the
-- game's civilian AI honours; MoveVec orders are given every few tenths of a second anyway).
local function control(h)
  pcall(h.InterruptCivilWander, h)
  pcall(h.InterruptAvoidingCar, h)
  pcall(h.SetReactionOnSound, h, false)
  pcall(h.DespawnImmunity, h, true)
end

local function giveBack(h)
  pcall(h.SetMovementSpeedMult, h, 1)
  pcall(h.SetReactionOnSound, h, true)
  pcall(h.ResumeAvoidingCar, h)
  pcall(h.ResumeCivilWander, h)
  pcall(h.DespawnImmunity, h, false)
end

function P.Adopt(key, id, f)
  local e = P.amb[id]
  local h = (e and valid(e.h)) and e.h or mc_entity(id)
  if not valid(h) then
    P.ev[#P.ev + 1] = "PGL " .. id
    return
  end
  M.PedOwned[id] = true
  P.amb[id] = nil
  control(h)
  P.reps[key] = { key = key, how = "adopted", h = h, id = id }
  P.adopted = (P.adopted or 0) + 1
  if P.adopted <= 30 or P.adopted % 100 == 0 then
    local q = h:GetPos()
    log(string.format("our human %d is the replica of pedestrian %s of player %d now (%.1f m from it, #%d)", id,
      tostring(key % 65536), math.floor(key / 65536), math.sqrt((q.x - (tonumber(f[4]) or q.x)) ^ 2 +
      (q.y - (tonumber(f[5]) or q.y)) ^ 2), P.adopted))
  end
end

function P.Release(key, how)
  local r = P.reps[key]
  P.reps[key] = nil
  for i = #P.queue, 1, -1 do
    if P.queue[i].key == key then table.remove(P.queue, i) end
  end
  if not r then return end
  if r.how == "adopted" and r.h then
    if valid(r.h) then giveBack(r.h) end
    if r.id then M.PedOwned[r.id] = nil end
  elseif r.how == "spawned" and r.h then
    if valid(r.h) and P.fac.comp then pcall(P.fac.comp.ReleaseObjectRaw, P.fac.comp, r.h) end
    if r.id then M.PedOwned[r.id] = nil end
  end
  P.released = (P.released or 0) + 1
  if P.released <= 30 or P.released % 100 == 0 then
    log(string.format("replica of pedestrian %d of player %d released (%s, #%d)", key % 65536, math.floor(key / 65536),
      tostring(how or r.how), P.released))
  end
end

function P.Hide(id)
  local e = P.amb[id]
  local h = (e and valid(e.h)) and e.h or mc_entity(id)
  if not valid(h) then
    P.ev[#P.ev + 1] = "PGL " .. id
    return
  end
  local okp, phys = pcall(h.GetPhysState, h)
  pcall(h.ShowModel, h, false)
  pcall(h.Deactivate, h)
  P.hidden[id] = { h = h, phys = okp and phys or nil }
  M.PedOwned[id] = true
  P.amb[id] = nil
  P.hides = (P.hides or 0) + 1
end

function P.Show(id)
  local x = P.hidden[id]
  P.hidden[id] = nil
  M.PedOwned[id] = nil
  if not (x and valid(x.h)) then return end
  pcall(x.h.Activate, x.h)
  pcall(x.h.ShowModel, x.h, true)
  if x.phys ~= nil then pcall(x.h.SetPhysState, x.h, x.phys) end
  P.shows = (P.shows or 0) + 1
end

local function speedMult(r, h, m)
  if r.noMult or math.abs(m - (r.mult or 1)) < 0.02 then return end
  local good = pcall(h.SetMovementSpeedMult, h, m)
  if not good then r.noMult = true return end
  r.mult = m
end

-- Walk a replica as the C++ side's LocomotionController says (player_sync.lua M.Walk does the same for puppets).
function P.Drive(r, f)
  local h = r.h
  if not valid(h) or h:IsDeath() then return end
  local n = tonumber
  local gait, mult, reissue = n(f[3]) or 0, n(f[4]) or 1, f[5] == "1"
  local mx, my, mz = n(f[6]) or 0, n(f[7]) or 0, n(f[8]) or 0
  local fix, nx, ny = n(f[9]) or 0, n(f[10]) or 0, n(f[11]) or 0
  local turn, fx, fy = f[12] == "1", n(f[13]) or 0, n(f[14]) or 1
  local tx, ty, tz = n(f[15]), n(f[16]), n(f[17])
  local hx, hy, flags = n(f[18]) or 0, n(f[19]) or 1, n(f[20]) or 0
  if not (tx and ty and tz) then return end
  if fix == 2 then
    h:SetPos(V(tx, ty, tz))
    h:SetDir(V(hx, hy, 0))
    r.snaps = (r.snaps or 0) + 1
  elseif fix == 1 then
    local q = h:GetPos()
    h:SetPos(V(q.x + nx, q.y + ny, q.z))
  end
  if reissue then
    if gait == 0 then
      speedMult(r, h, 1)
      h:MoveVec(V(tx, ty, tz), "WALK")
    else
      speedMult(r, h, mult)
      h:MoveVec(V(mx, my, mz), GAITS[gait] or "WALK")
    end
  end
  if turn then h:SetDir(V(fx, fy, 0)) end
  -- the authority's pedestrian crouches in panic: ours too, while it stands (UNVERIFIED read and picture)
  if bit(flags, 4) and gait == 0 and M.now - (r.crouchT or -10) > 1.2 then
    r.crouchT = M.now
    pcall(h.PanicCrouch, h, 1.5)
  end
end

-- ---- the copies' spawner (player_sync.lua's puppet factory recipe, docs/game-api.md 3) --------------------------------
function P.FacReset(why)
  local f = P.fac
  if f.id then pcall(game.traffic.DespawnEntityImmediately, game.traffic, f.id) end  -- its copies go with it
  if f.drvId then M.PedOwned[f.drvId] = nil end
  for key, r in pairs(P.reps) do
    if r.how == "spawned" or r.how == "spawning" then
      if r.id then M.PedOwned[r.id] = nil end
      P.reps[key] = nil
      P.ev[#P.ev + 1] = (r.how == "spawned" and r.id) and ("PGL " .. r.id) or ("PSF " .. key .. " spawner_reset")
    end
  end
  P.queue = {}
  P.fac = { st = "none", t = -3 }
  if why then log("copy spawner reset: " .. why) end
end

function P.FacTick(dt, p)
  local f = P.fac
  f.t = f.t + dt
  local need = #P.queue > 0
  if f.st == "none" then
    if not need or f.t < 0 then return end
    local so, id = try("spawner ObtainCarWithCrew", game.traffic.ObtainCarWithCrew, game.traffic, FACTORY_CAR,
      { FACTORY_PROFILE })
    if so and id then f.st, f.so, f.id, f.t = "obtain", so, id, 0 else f.t = -5 end
  elseif f.st == "obtain" then
    if f.so:IsDone() then
      local q = p:GetPos()
      f.sp = game.traffic:SpawnCar(f.id, V(q.x - 3, q.y, q.z - 40), V(1, 0, 0))
      f.st, f.t = "spawn", 0
    elseif f.t > 6 then
      P.FacReset("obtain timeout")
    end
  elseif f.st == "spawn" then
    if f.sp:IsDone() then
      local car = game.traffic:GetSpawnedEntity(f.id)
      local drv = car and car:GetSeatOwner(0)
      if not valid(drv) then P.FacReset("no crew") return end
      pcall(car.FixCarPos, car, true)
      pcall(car.SetDespawnImmunity, car, true)
      if not drv:GetComponent("C_RuntimeSpawnerComponent") then drv:AddComponent("C_RuntimeSpawnerComponent") end
      local comp = drv:GetComponent("C_RuntimeSpawnerComponent")
      if not comp then P.FacReset("no spawner component") return end
      pcall(drv.SetDemigod, drv, true)
      pcall(drv.DespawnImmunity, drv, true)
      pcall(drv.SetReactionOnSound, drv, false)
      pcall(drv.InterruptCivilWander, drv)
      pcall(drv.LockControls, drv, true)
      f.car, f.drv, f.drvId, f.comp, f.st, f.t = car, drv, idOf(drv), comp, "ready", 0
      if f.drvId then M.PedOwned[f.drvId] = true end
      log("copy spawner ready")
    elseif f.t > 8 then
      P.FacReset("spawn timeout")
    end
  elseif f.st == "ready" then
    if not valid(f.drv) or not valid(f.car) or f.drv:IsDeath() then P.FacReset("copy spawner lost") return end
    if not f.loaded then
      if not f.load then
        local prof = mc_fnv and tostring(mc_fnv(COPY_PROFILE)) or COPY_PROFILE
        if not pcall(f.comp.SetSpawnProfile, f.comp, prof) then
          P.FacReset("profile refused")
          P.fac.t = -30
          return
        end
        f.load, f.loadT = f.comp:GetSpawnProfileLoadSyncObject(), M.now
      elseif f.load:IsDone() then
        f.loaded = true
        log("copy profile " .. COPY_PROFILE .. " loaded")
      elseif M.now - f.loadT > LOAD_TIMEOUT then
        log("copy profile " .. COPY_PROFILE .. " did not load in " .. LOAD_TIMEOUT .. " s")
        for _, rq in ipairs(P.queue) do P.ev[#P.ev + 1] = "PSF " .. rq.key .. " no_profile" end
        P.FacReset("the profile did not load")
        P.fac.t = -30
      end
      return
    end
    local rq = table.remove(P.queue, 1)
    if rq then
      local so = try("copy CreateObject", f.comp.CreateObject, f.comp)
      if not valid(so) then
        P.ev[#P.ev + 1] = "PSF " .. rq.key .. " create_failed"
        P.reps[rq.key] = nil
        return
      end
      so:SetPos(V(rq.x, rq.y, rq.z))
      so:SetDir(V(rq.hx, rq.hy, 0))
      so:Activate()
      pcall(so.SetDemigod, so, false)
      pcall(function() so.invulnerability = false end)
      control(so)
      local id = idOf(so)
      if id then M.PedOwned[id] = true end
      P.reps[rq.key] = { key = rq.key, how = "spawned", h = so, id = id }
      P.ev[#P.ev + 1] = "PSR " .. rq.key .. " " .. tostring(id or 0)
      P.made = (P.made or 0) + 1
      if P.made <= 30 or P.made % 100 == 0 then
        log(string.format("spawned a copy for pedestrian %d of player %d (our %s, #%d)", rq.key % 65536,
          math.floor(rq.key / 65536), tostring(id), P.made))
      end
      f.t = 0
    else
      -- idle: the spawner goes when it has no copy left for a while (its despawn would take its copies along)
      local any = false
      for _, r in pairs(P.reps) do
        if r.how == "spawned" then any = true break end
      end
      if any then f.t = 0 end
      if f.t > FACTORY_IDLE then P.FacReset("idle") P.fac.t = 0 return end
      if f.t % 3 < dt then
        local q, cp = p:GetPos(), f.car:GetPos()
        if (q.x - cp.x) ^ 2 + (q.y - cp.y) ^ 2 > 120 * 120 then f.car:SetPos(V(q.x - 3, q.y, q.z - 40)) end
      end
    end
  end
end

-- ---- our spawners near another's pedestrians --------------------------------------------------------------------------
-- BlockSpawnersInSphere(pos, r, bool) (static, docs/game-api.md 26: the spawners inside the sphere at that moment get
-- the flag; not a lasting zone) around us while another game's pedestrians show here, so fewer of ours appear only to
-- be hidden. UNVERIFIED that `true` blocks and that it reaches the pedestrian spawners.
function P.Block(on, p)
  if on then
    if M.now - (P.blockT or -10) < 1 then return end
    P.blockT = M.now
    local q = p:GetPos()
    for _, b in ipairs(P.blocks) do
      if (q.x - b.x) ^ 2 + (q.y - b.y) ^ 2 < BLOCK_STEP * BLOCK_STEP then return end
    end
    local ok = pcall(game.traffic.BlockSpawnersInSphere, game.traffic, V(q.x, q.y, q.z), BLOCK_RADIUS, true)
    table.insert(P.blocks, { x = q.x, y = q.y, z = q.z })
    if #P.blocks > BLOCK_MAX then
      local b = table.remove(P.blocks, 1)
      pcall(game.traffic.BlockSpawnersInSphere, game.traffic, V(b.x, b.y, b.z), BLOCK_RADIUS, false)
    end
    if #P.blocks == 1 then log("our pedestrian spawners blocked around us: " .. (ok and "ok" or "FAILED")) end
  elseif #P.blocks > 0 then
    for _, b in ipairs(P.blocks) do
      pcall(game.traffic.BlockSpawnersInSphere, game.traffic, V(b.x, b.y, b.z), BLOCK_RADIUS, false)
    end
    log(#P.blocks .. " blocked spots of our pedestrian spawners unblocked")
    P.blocks = {}
  end
end

-- Everything we hold goes back to the game (session end, a new world: `gone` = the entities went with it).
function P.DropAll(why, gone)
  local n, hn = 0, 0
  for key, r in pairs(P.reps) do
    if not gone then P.Release(key, r.how) end
    n = n + 1
  end
  for id, _ in pairs(P.hidden) do
    if not gone then P.Show(id) end
    hn = hn + 1
  end
  if not gone and P.fac.st ~= "none" then P.FacReset(nil) end
  if not gone then P.Block(false) end
  P.reps, P.hidden, P.queue, P.blocks = {}, {}, {}, {}
  P.fac = { st = "none", t = 0 }
  M.PedOwned = {}
  if n + hn > 0 then log(string.format("%d replicas and %d hidden humans let go: %s", n, hn, why)) end
end

-- ---- update -----------------------------------------------------------------------------------------------------------
function P.Update(dt, cmds)
  P.ev = {}
  local ev = P.ev
  local p = M.Player and M.Player()
  local wkey = p and (tostring(game.game:GetLoadedSaveId()) .. "|" .. tostring(M.inWorldAt)) or nil
  if wkey ~= P.worldKey then  -- another world: what we held went with the old one
    if P.worldKey ~= nil then ev[#ev + 1] = "PW" end
    P.worldKey = wkey
    P.DropAll("a new world", true)
    P.amb, P.notAmb = {}, {}
    P.scanT, P.roundT = -10, -10
  end
  local isActive = false
  local list = {}
  for line in string.gmatch(cmds, "[^\n]+") do
    local f = {}
    for w in string.gmatch(line, "%S+") do f[#f + 1] = w end
    if f[1] == "S" then
      isActive = f[2] == "1"
      P.localId = f[4] or P.localId
    else
      list[#list + 1] = f
    end
  end
  if not (isActive and p) then
    if P.wasActive and p then P.DropAll("the session ended", false) end
    P.wasActive = isActive and p ~= nil
    return table.concat(ev, "\n")
  end
  P.wasActive = true
  for _, f in ipairs(list) do
    local op, n = f[1], tonumber
    local good, err = true, nil
    if op == "D" then
      local r = P.reps[n(f[2]) or -1]
      if r and (r.how == "adopted" or r.how == "spawned") then good, err = pcall(P.Drive, r, f) end
    elseif op == "A" then
      good, err = pcall(P.Adopt, n(f[2]), n(f[3]), f)
    elseif op == "N" then
      local key = n(f[2])
      if key and not P.reps[key] then
        P.reps[key] = { key = key, how = "spawning" }
        P.queue[#P.queue + 1] = { key = key, x = n(f[3]) or 0, y = n(f[4]) or 0, z = n(f[5]) or 0, hx = n(f[6]) or 0,
          hy = n(f[7]) or 1 }
      end
    elseif op == "R" then
      good, err = pcall(P.Release, n(f[2]), f[3])
    elseif op == "H" then
      good, err = pcall(P.Hide, n(f[2]))
    elseif op == "W" then
      good, err = pcall(P.Show, n(f[2]))
    elseif op == "K" then
      local r = P.reps[n(f[2]) or -1]
      if r and r.h and valid(r.h) then kill(r.h) end
    elseif op == "KL" then
      local id = n(f[2])
      local e = id and P.amb[id]
      local h = (e and valid(e.h)) and e.h or (id and mc_entity(id))
      if h and valid(h) then
        kill(h)
        log("our pedestrian " .. tostring(id) .. " dies: a player killed his replica of it")
      end
    elseif op == "B" then
      good, err = pcall(P.Block, f[2] == "1", p)
    end
    if not good then
      P.errs = (P.errs or 0) + 1
      if P.errs <= 20 then log(op .. " error: " .. tostring(err)) end
    end
  end
  -- the spawner works through the spawn requests (one profile, copies one per tick)
  if #P.queue > 0 or P.fac.st ~= "none" then
    local good, err = pcall(P.FacTick, dt, p)
    if not good then log("spawner error: " .. tostring(err)) P.FacReset("error") end
  end
  -- replica reports (and gone ones)
  for key, r in pairs(P.reps) do
    if r.how == "adopted" or r.how == "spawned" then
      if not valid(r.h) then
        ev[#ev + 1] = "PGL " .. tostring(r.id or 0)
        if r.id then M.PedOwned[r.id] = nil end
        P.reps[key] = nil
      else
        local q, d = r.h:GetPos(), r.h:GetDir()
        local okD, dead = pcall(r.h.IsDeath, r.h)
        ev[#ev + 1] = string.format("PR %d %.3f %.3f %.3f %.4f %.4f %d", key, q.x, q.y, q.z, d.x, d.y,
          (okD and dead) and 1 or 0)
      end
    end
  end
  if M.now - (P.hiddenT or -10) >= 1 then
    P.hiddenT = M.now
    for id, x in pairs(P.hidden) do
      if not valid(x.h) then
        P.hidden[id] = nil
        M.PedOwned[id] = nil
        ev[#ev + 1] = "PGL " .. id
      end
    end
  end
  -- our ambient humans
  if M.now - (P.scanT or -10) >= SCAN_INTERVAL then
    P.scanT = M.now
    local good, err = pcall(P.Scan)
    if not good then log("scan error: " .. tostring(err)) end
  end
  if M.now - (P.roundT or -10) >= ROUND_INTERVAL then
    P.roundT = M.now
    local good, err = pcall(P.Round, p, ev)
    if not good then log("round error: " .. tostring(err)) end
  end
  if M.now - (P.stT or -10) >= 1 then
    P.stT = M.now
    local a, s, hid = 0, 0, 0
    for _, r in pairs(P.reps) do
      if r.how == "adopted" then a = a + 1 elseif r.how == "spawned" then s = s + 1 end
    end
    for _, _ in pairs(P.hidden) do hid = hid + 1 end
    ev[#ev + 1] = string.format("ST ambient %d (%d near players), replicas %d adopted / %d spawned (+%d queued), "
      .. "hidden %d, spawner %s, blocked spots %d", P.ambCount or 0, P.reported or 0, a, s, #P.queue, hid,
      P.fac.st, #P.blocks)
    trace("PDL", string.format("amb=%d near=%d adopted=%d spawned=%d queued=%d hidden=%d fac=%s blocks=%d",
      P.ambCount or 0, P.reported or 0, a, s, #P.queue, hid, P.fac.st, #P.blocks))
  end
  return table.concat(ev, "\n")
end

return P.version
