-- MafiaCoop pedestrian glue (ambient pedestrians shared). Runs in the game's Lua state on GameMainThread after
-- player_sync.lua, npc_sync.lua and traffic_sync.lua, called once per script tick by src/game/ped_sync.cpp
-- (MafiaCoop.Peds.Update). Embedded into the dll; a copy at <dll dir>\ped_sync.lua overrides it.
--
-- Host authority (docs/host-authority.md WP-C): the host's game owns every ambient pedestrian. On the host this glue
-- reads its ambient humans (MafiaCoop.IsAmbientHuman) near any player 20 times a second for the C++ side, which sends
-- each guest the ones within 80 m of him. On a guest it carries out what the C++ side (game/ped_model.h PedReceiver)
-- decides: every one of the host's pedestrians is shown by a copy from a pool - a new copy is made by a spawner of
-- our own (the recipe of player_sync.lua's puppet factory) with one of six civilian profiles (one profile load at a
-- time; requests for the loaded profile first, a request that waited 2 s gets the loaded look), a copy whose
-- pedestrian went is hidden (ShowModel false, Deactivate)
-- and kept, and a kept copy is put at the next new pedestrian's spot and shown again. A copy is walked like a puppet
-- (MoveVec with the host's gait, speed multiplier, nudges and snaps of core's LocomotionController). Deaths follow
-- the host (a copy is killed); a copy that dies here by our hand is reported (PedKill). The guest's own pedestrians
-- are not this glue's business any more: guest_world (WP-A) switches their spawning off and hides leftovers; nothing
-- here adopts, hides or blocks the guest's own humans.
--
-- Contract with npc_sync.lua and guest_world: an ambient human is not a player / puppet / any copy, not in a vehicle,
-- AI type 0, and not named after a mission spawner (`behavior_object_prime-NN-<suffix>` with a non-empty suffix,
-- docs/game-api.md 15). npc_sync skips them (MafiaCoop.IsAmbientHuman(h)). MafiaCoop.PedOwned (entity id -> true)
-- holds every human this glue controls - every copy of the pool, shown or hidden, and the copy spawner's driver -
-- and is kept complete at every change: npc_sync never pairs or steers one, guest_world never hides one.
-- MafiaCoop.Peds.fac.car is the copy spawner's car (traffic_sync and guest_world must leave it alone).
--
-- Input (cmds, one line per command):
--   S active host localId                  session running, we host, our player id
--   N key x y z hx hy prof name            spawn a copy for pedestrian `key` (host << 16 | net) there, with profile
--                                          `name` (index `prof` of game/ped_model.h kCopyProfiles)
--   U key id x y z hx hy                   pool copy `id` is `key`'s copy from now on: put there, shown
--   H id                                   copy `id` has no pedestrian now: hidden, kept in the pool
--   R key id                               copy `id` goes for good (ReleaseObjectRaw); id 0: `key`'s request dropped
--   D key gait mult reissue mx my mz fix nx ny turn fx fy tx ty tz hx hy flags
--                                          drive `key`'s copy (core LocomotionCommand; t = the target, flags as
--                                          PedStates: 1 dead 2 panic 4 crouch 8 running)
--   K key id                               `key`'s copy dies (the host's died)
--   KL id                                  (host) our own pedestrian `id` dies (a guest killed his copy of it)
--   FR why                                 the copy spawner is broken (C++: CreateObject failing / every copy gone
--                                          at once): a new one now
-- Output (events, one per line):
--   PP id x y z inWorld                    every player (us too): where he is (his car while he drives)
--   PC x y z dx dy dz                      our camera
--   PH id x y z hx hy flags look           (host) an ambient human of ours near a player (flags: 1 dead, 2 panic,
--                                          4 crouch)
--   PE                                     (host) end of a round of PH lines
--   PR key x y z hx hy dead                where a pedestrian's copy is, after this tick's commands
--   PSR key id prof / PSF key why          a spawned copy is in the world (its id, the look it got) / failed (why:
--                                          create_failed, no_profile, spawner_reset)
--   PGL id                                 a copy of ours (bound or in the pool) is gone (the game, or the spawner)
--   PFR                                    our copy spawner was reset (its copies went with it: the PGLs before)
--   PW                                     a new world: everything of the old one is gone
--   ST text                                status for the overlay (once a second)
MafiaCoop = MafiaCoop or {}
local M = MafiaCoop
M.Peds = M.Peds or {}
local P = M.Peds
P.version = 2
M.PedOwned = M.PedOwned or {}

local SCAN_INTERVAL = 0.25   -- s between scans of the human table (host)
local ROUND_INTERVAL = 0.05  -- s between rounds of positions for the C++ side
local REPORT_RANGE = 100     -- m from any player: our ambient humans reported (the C++ side sends within 80 / 90 m)
local MAX_REPORT = 200       -- humans per round
local RECHECK = 5            -- s: a human found not ambient is checked again after this long
local FACTORY_CAR = "shubert_e_six"
local FACTORY_PROFILE = "visuals/hero/game_main_tommy_gangster_trenchcoat"
local LOOK_WAIT = 2         -- s: a request that waited this long is made with the look loaded now (a crowd that comes
                             -- at once is not kept waiting for six loads; tests/ped_test.cpp SpawnerSim)
local LOAD_TIMEOUT = 8       -- s a copy profile may take to load (a look that does not load stops the spawner that
                             -- long; npc_sync's mission profiles get 12 s. UNVERIFIED how long a civilian look takes)
local FACTORY_IDLE = 30      -- s without a copy (pool included) or request: the spawner goes
local FACTORY_FOLLOW = 50    -- m: the spawner car is put under us again when we are this far from it (ground)
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

P.amb = P.amb or {}        -- (host) our ambient humans: id -> { h, id, look, flags }
P.notAmb = P.notAmb or {}  -- id -> time it was found not ambient (checked again after RECHECK)
P.copies = P.copies or {}  -- (guest) the pool: id -> { h, id, key (nil: free), shown, prof, phys }
P.reps = P.reps or {}      -- key -> its copy (an entry of P.copies), or { key, how = "spawning" } while asked for
P.queue = P.queue or {}    -- spawn requests: { key, x, y, z, hx, hy, prof, name }
P.fac = P.fac or { st = "none", t = 0 }

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
  -- one rule with npc_sync: src/game/npc_replica.h ClassOfName / IsAmbientHuman through mc_npc_class
  if mc_npc_class then
    local amb = mc_npc_class(okN and tostring(name or "") or "", 0, 0, 0)
    return amb == 1
  end
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

function P.Round(p, ev, host)
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
  if not host then return end  -- a guest shares no pedestrians
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

-- ---- copies -----------------------------------------------------------------------------------------------------------
-- A copy under our control: its own wandering and reactions are interrupted (UNVERIFIED which of these the game's
-- civilian AI honours; MoveVec orders are given every few tenths of a second anyway).
local function control(h)
  pcall(h.InterruptCivilWander, h)
  pcall(h.InterruptAvoidingCar, h)
  pcall(h.SetReactionOnSound, h, false)
  pcall(h.DespawnImmunity, h, true)
end

local function unbind(c)
  if c.key and P.reps[c.key] == c then P.reps[c.key] = nil end
  c.key = nil
end

-- Pool copy `id` stands for pedestrian `key` from now on: put at its spot, shown (UNVERIFIED that Activate +
-- ShowModel(true) bring back a copy hidden with Deactivate + ShowModel(false), as 0.16.0 did with hidden ambient
-- humans).
function P.Reuse(key, id, f)
  local c = P.copies[id]
  if not (c and valid(c.h)) then
    if c then P.copies[id] = nil end
    M.PedOwned[id] = nil
    P.ev[#P.ev + 1] = "PGL " .. id
    return
  end
  local n = tonumber
  local old = P.reps[key]
  if old and old ~= c then
    if old.how == "spawning" then P.reps[key] = nil else unbind(old) end
  end
  unbind(c)
  c.key = key
  c.mult, c.noMult, c.crouchT = nil, nil, nil
  P.reps[key] = c
  local h = c.h
  if not c.shown then
    pcall(h.Activate, h)
    pcall(h.ShowModel, h, true)
    if c.phys ~= nil then pcall(h.SetPhysState, h, c.phys) end
    c.shown = true
  end
  local x, y, z = n(f[4]), n(f[5]), n(f[6])
  if x and y and z then
    h:SetPos(V(x, y, z))
    h:SetDir(V(n(f[7]) or 0, n(f[8]) or 1, 0))
  end
  pcall(h.SetMovementSpeedMult, h, 1)
  control(h)
  P.reused = (P.reused or 0) + 1
  if P.reused <= 30 or P.reused % 100 == 0 then
    log(string.format("pool copy %d is the copy of the host's pedestrian %d now (#%d)", id, key % 65536, P.reused))
  end
end

-- Copy `id` has no pedestrian now: hidden and kept for the next one.
function P.HideCopy(id)
  local c = P.copies[id]
  if not c then return end
  unbind(c)
  if not valid(c.h) then
    P.copies[id] = nil
    M.PedOwned[id] = nil
    P.ev[#P.ev + 1] = "PGL " .. id
    return
  end
  if c.shown then
    local h = c.h
    pcall(h.SetMovementSpeedMult, h, 1)
    local okp, phys = pcall(h.GetPhysState, h)
    c.phys = okp and phys or nil
    pcall(h.ShowModel, h, false)
    pcall(h.Deactivate, h)
    c.shown = false
  end
  P.hides = (P.hides or 0) + 1
end

-- Copy `id` goes for good; id 0: pedestrian `key`'s spawn request is dropped (a copy already made for it stays: the
-- C++ side got its PSR and keeps it in the pool).
function P.Remove(key, id)
  for i = #P.queue, 1, -1 do
    if P.queue[i].key == key then table.remove(P.queue, i) end
  end
  local r = key and P.reps[key]
  if r and r.how == "spawning" then P.reps[key] = nil end
  if not id or id == 0 then return end
  local c = P.copies[id]
  P.copies[id] = nil
  M.PedOwned[id] = nil
  if not c then return end
  unbind(c)
  if valid(c.h) and P.fac.comp then pcall(P.fac.comp.ReleaseObjectRaw, P.fac.comp, c.h) end
  P.removed = (P.removed or 0) + 1
  if P.removed <= 30 or P.removed % 100 == 0 then
    log(string.format("copy %d removed (#%d)", id, P.removed))
  end
end

local function speedMult(r, h, m)
  if r.noMult or math.abs(m - (r.mult or 1)) < 0.02 then return end
  local good = pcall(h.SetMovementSpeedMult, h, m)
  if not good then r.noMult = true return end
  r.mult = m
end

-- Walk a copy as the C++ side's LocomotionController says (player_sync.lua M.Walk does the same for puppets).
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
  -- the host's pedestrian crouches in panic: ours too, while it stands (UNVERIFIED read and picture)
  if bit(flags, 4) and gait == 0 and M.now - (r.crouchT or -10) > 1.2 then
    r.crouchT = M.now
    pcall(h.PanicCrouch, h, 1.5)
  end
end

-- ---- the copies' spawner (player_sync.lua's puppet factory recipe, docs/game-api.md 3) --------------------------------
local function anyCopy()
  for _, _ in pairs(P.copies) do return true end
  return false
end

function P.FacReset(why)
  local f = P.fac
  if f.id then pcall(game.traffic.DespawnEntityImmediately, game.traffic, f.id) end  -- its copies go with it
  if f.drvId then M.PedOwned[f.drvId] = nil end
  for id, c in pairs(P.copies) do
    M.PedOwned[id] = nil
    P.ev[#P.ev + 1] = "PGL " .. id
  end
  for key, r in pairs(P.reps) do
    if r.how == "spawning" then P.ev[#P.ev + 1] = "PSF " .. key .. " spawner_reset" end
  end
  P.copies, P.reps, P.queue = {}, {}, {}
  P.fac = { st = "none", t = -3 }
  P.ev[#P.ev + 1] = "PFR"  -- after the PGLs: the C++ side does not take them for a spawner that took its copies along
  if why then log("copy spawner reset: " .. why) end
end

-- A profile did not load (or was refused): every request for it fails (the C++ side counts it once and may drop it
-- from the rotation); the spawner itself is fine.
local function profileFailed(f, name, why)
  log("copy profile " .. tostring(name) .. " " .. why)
  for i = #P.queue, 1, -1 do
    local rq = P.queue[i]
    if rq.name == name then
      P.ev[#P.ev + 1] = "PSF " .. rq.key .. " no_profile"
      if P.reps[rq.key] and P.reps[rq.key].how == "spawning" then P.reps[rq.key] = nil end
      table.remove(P.queue, i)
    end
  end
  f.load, f.loadName, f.loadedName = nil, nil, nil
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
      f.car, f.drv, f.drvId, f.comp, f.st, f.t, f.readyT = car, drv, idOf(drv), comp, "ready", 0, M.now
      if f.drvId then M.PedOwned[f.drvId] = true end
      log("copy spawner ready")
    elseif f.t > 8 then
      P.FacReset("spawn timeout")
    end
  elseif f.st == "ready" then
    if not valid(f.drv) or not valid(f.car) or f.drv:IsDeath() then P.FacReset("copy spawner lost") return end
    -- deactivated (2026-10-20 two-PC test: the guest's traffic sync hid it as one of its own traffic cars - its copies
    -- went with it and every CreateObject failed for 30 s): a new one
    if not active(f.car) or not active(f.drv) then
      P.FacReset("copy spawner deactivated")
      P.fac.t = 0
      return
    end
    if M.now - (f.followT or 0) >= 1 then
      f.followT = M.now
      local q, cp = p:GetPos(), f.car:GetPos()
      if (q.x - cp.x) ^ 2 + (q.y - cp.y) ^ 2 > FACTORY_FOLLOW * FACTORY_FOLLOW then
        f.car:SetPos(V(q.x - 3, q.y, q.z - 40))
      end
    end
    -- a profile load under way: nothing is made until it is done (the component has one profile at a time)
    if f.load then
      if f.load:IsDone() then
        f.loadedName, f.loadedProf, f.load = f.loadName, f.loadProf, nil
        P.loadedOnce = P.loadedOnce or {}
        if not P.loadedOnce[f.loadedName] then
          P.loadedOnce[f.loadedName] = true
          log("copy profile " .. f.loadedName .. " loaded (" .. string.format("%.1f", M.now - f.loadT) .. " s)")
        end
      elseif M.now - f.loadT > LOAD_TIMEOUT then
        profileFailed(f, f.loadName, "did not load in " .. LOAD_TIMEOUT .. " s")
      end
      return
    end
    if #P.queue == 0 then
      -- idle: the spawner goes when it has no copy left for a while, the pool's included (its despawn would take
      -- its copies along)
      if anyCopy() then f.t = 0 end
      if f.t > FACTORY_IDLE then P.FacReset("idle") P.fac.t = 0 end
      return
    end
    -- a request for the loaded profile first (or the oldest one with it, once it waited LOOK_WAIT); else the look
    -- most requests wait for is loaded (ties: the oldest request's): one load per look, not one per copy
    local pick
    for i, rq in ipairs(P.queue) do
      if rq.name == f.loadedName then pick = i break end
    end
    if not pick and f.loadedName and M.now - (P.queue[1].t or M.now) >= LOOK_WAIT then pick = 1 end
    if not pick then
      local count, name = {}, P.queue[1].name
      for _, rq in ipairs(P.queue) do count[rq.name] = (count[rq.name] or 0) + 1 end
      for _, rq in ipairs(P.queue) do
        if count[rq.name] > count[name] then name = rq.name end
      end
      local prof = mc_fnv and tostring(mc_fnv(name)) or name
      if not pcall(f.comp.SetSpawnProfile, f.comp, prof) then
        profileFailed(f, name, "refused by SetSpawnProfile")
        return
      end
      local okL, so = pcall(f.comp.GetSpawnProfileLoadSyncObject, f.comp)
      if not okL or not so then
        profileFailed(f, name, "gave no load sync object")
        return
      end
      local idx = 0
      for _, rq in ipairs(P.queue) do
        if rq.name == name then idx = rq.prof break end
      end
      f.load, f.loadName, f.loadProf, f.loadedName, f.loadT = so, name, idx, nil, M.now
      return
    end
    local rq = table.remove(P.queue, pick)
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
    if id then
      M.PedOwned[id] = true
      local c = { h = so, id = id, key = rq.key, shown = true, prof = f.loadedProf or rq.prof }
      P.copies[id] = c
      P.reps[rq.key] = c
    else
      P.reps[rq.key] = nil
    end
    P.ev[#P.ev + 1] = "PSR " .. rq.key .. " " .. tostring(id or 0) .. " " .. tostring(f.loadedProf or rq.prof)
    P.made = (P.made or 0) + 1
    if P.made <= 30 or P.made % 100 == 0 then
      log(string.format("spawned a copy for the host's pedestrian %d (our %s, look %s, #%d)", rq.key % 65536,
        tostring(id), tostring(f.loadedName), P.made))
    end
    f.t = 0
  end
end

-- Everything we hold goes back to the game (session end, a new world: `gone` = the entities went with it).
function P.DropAll(why, gone)
  local n = 0
  for id, c in pairs(P.copies) do
    if not gone and valid(c.h) and P.fac.comp then pcall(P.fac.comp.ReleaseObjectRaw, P.fac.comp, c.h) end
    n = n + 1
  end
  if not gone and P.fac.st ~= "none" then P.FacReset(nil) end
  P.copies, P.reps, P.queue = {}, {}, {}
  P.fac = { st = "none", t = 0 }
  M.PedOwned = {}
  if n > 0 then log(string.format("%d copies let go: %s", n, why)) end
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
  local isActive, isHost = false, false
  local list = {}
  for line in string.gmatch(cmds, "[^\n]+") do
    local f = {}
    for w in string.gmatch(line, "%S+") do f[#f + 1] = w end
    if f[1] == "S" then
      isActive = f[2] == "1"
      isHost = f[3] == "1"
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
  P.isHost = isHost
  for _, f in ipairs(list) do
    local op, n = f[1], tonumber
    local good, err = true, nil
    if op == "D" then
      local r = P.reps[n(f[2]) or -1]
      if r and r.h then good, err = pcall(P.Drive, r, f) end
    elseif op == "N" then
      local key = n(f[2])
      if key and not P.reps[key] and f[9] then
        P.reps[key] = { key = key, how = "spawning" }
        P.queue[#P.queue + 1] = { key = key, x = n(f[3]) or 0, y = n(f[4]) or 0, z = n(f[5]) or 0, hx = n(f[6]) or 0,
          hy = n(f[7]) or 1, prof = n(f[8]) or 0, name = f[9], t = M.now }
      end
    elseif op == "U" then
      good, err = pcall(P.Reuse, n(f[2]), n(f[3]), f)
    elseif op == "H" then
      good, err = pcall(P.HideCopy, n(f[2]))
    elseif op == "R" then
      good, err = pcall(P.Remove, n(f[2]), n(f[3]))
    elseif op == "K" then
      local r = P.reps[n(f[2]) or -1]
      if r and r.h and valid(r.h) then kill(r.h) end
    elseif op == "KL" then
      local id = n(f[2])
      local e = id and P.amb[id]
      local h = (e and valid(e.h)) and e.h or (id and mc_entity(id))
      if h and valid(h) then
        kill(h)
        log("our pedestrian " .. tostring(id) .. " dies: a guest killed his copy of it")
      end
    elseif op == "FR" then
      -- only a spawner that has been ready a while (one being made, or made just now after our own reset, is new)
      if P.fac.st == "ready" and M.now - (P.fac.readyT or M.now) >= 1 then
        good, err = pcall(P.FacReset, "asked (" .. tostring(f[2]) .. ")")
        P.fac.t = 0
      end
    end
    if not good then
      P.errs = (P.errs or 0) + 1
      if P.errs <= 20 then log(op .. " error: " .. tostring(err)) end
    end
  end
  -- the spawner works through the spawn requests (one profile load at a time, copies one per tick)
  if #P.queue > 0 or P.fac.st ~= "none" then
    local good, err = pcall(P.FacTick, dt, p)
    if not good then log("spawner error: " .. tostring(err)) P.FacReset("error") end
  end
  -- copy reports (and gone ones)
  for key, c in pairs(P.reps) do
    if c.h then
      if not valid(c.h) then
        ev[#ev + 1] = "PGL " .. tostring(c.id or 0)
        if c.id then
          M.PedOwned[c.id] = nil
          P.copies[c.id] = nil
        end
        P.reps[key] = nil
      else
        local q, d = c.h:GetPos(), c.h:GetDir()
        local okD, dead = pcall(c.h.IsDeath, c.h)
        ev[#ev + 1] = string.format("PR %d %.3f %.3f %.3f %.4f %.4f %d", key, q.x, q.y, q.z, d.x, d.y,
          (okD and dead) and 1 or 0)
      end
    end
  end
  if M.now - (P.poolT or -10) >= 1 then
    P.poolT = M.now
    for id, c in pairs(P.copies) do
      if not c.key and not valid(c.h) then
        P.copies[id] = nil
        M.PedOwned[id] = nil
        ev[#ev + 1] = "PGL " .. id
      end
    end
  end
  -- our ambient humans (the host only)
  if isHost and M.now - (P.scanT or -10) >= SCAN_INTERVAL then
    P.scanT = M.now
    local good, err = pcall(P.Scan)
    if not good then log("scan error: " .. tostring(err)) end
  end
  if M.now - (P.roundT or -10) >= ROUND_INTERVAL then
    P.roundT = M.now
    local good, err = pcall(P.Round, p, ev, isHost)
    if not good then log("round error: " .. tostring(err)) end
  end
  if M.now - (P.stT or -10) >= 1 then
    P.stT = M.now
    local bound, free, hidden = 0, 0, 0
    for _, c in pairs(P.copies) do
      if c.key then bound = bound + 1 else free = free + 1 end
      if not c.shown then hidden = hidden + 1 end
    end
    if isHost then
      ev[#ev + 1] = string.format("ST ambient %d (%d near players)", P.ambCount or 0, P.reported or 0)
    else
      ev[#ev + 1] = string.format("ST copies %d + %d in the pool (%d hidden, +%d queued), spawner %s, look %s",
        bound, free, hidden, #P.queue, P.fac.st, tostring(P.fac.loadedName or "-"))
    end
    trace("PDL", string.format("amb=%d near=%d copies=%d pool=%d hidden=%d queued=%d fac=%s prof=%s",
      P.ambCount or 0, P.reported or 0, bound, free, hidden, #P.queue, P.fac.st,
      tostring(P.fac.loadedName or "-")))
  end
  return table.concat(ev, "\n")
end

return P.version
