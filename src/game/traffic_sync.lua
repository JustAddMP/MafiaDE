-- MafiaCoop traffic glue (B4: ambient traffic and police shared). Runs in the game's Lua state on GameMainThread after
-- player_sync.lua and npc_sync.lua, called once per script tick by src/game/traffic_sync.cpp
-- (MafiaCoop.Traffic.Update). Embedded into the dll; a copy at <dll dir>\traffic_sync.lua overrides it.
--
-- The host's traffic is authoritative. The host reports its moving traffic and police cars (a car whose driver came
-- from the traffic or police generators, docs/game-api.md 15: `behavior_object_prime-NN-` with an empty suffix) near
-- any guest, 20 times a second; the C++ side (game/traffic_model.h) tells each guest which of them he gets. A guest
-- near the host (within 250 m; back to his own beyond 400 m) switches his own ambient traffic and police spawning off,
-- hides his own moving traffic cars once they are out of the way, and shows a copy of each host car near him (a car
-- with a crew of its own, ObtainCarWithCrew), driven every tick the way the C++ side's VehicleController says (the
-- host driver's controls, a blended correction, pinned when its physics does not follow), with player_sync.lua's
-- remote-car helpers (M.RV). Parked cars, mission cars and player cars are not traffic: they stay as they were
-- (parked cars local, mission cars from each game's own mission, player cars through player_sync).
-- When a player takes over a car that is a traffic copy here (player_sync adopts it: the host or this guest jacked a
-- traffic car) the copy is handed over: its crew gets out and it is player_sync's car from then on.
--
-- Input (cmds, one line per command):
--   S active host                              session running, we host
--   B from net key err copySpeed hostSpeed     host: guest `from`'s copy of our car `key` was knocked off (logged)
--   M 1|0                                      guest: host traffic here (1) or our own (0)
--   A net kind model c1 c2 plate x y z qx qy qz qw flags
--                                              guest: make a copy of host car `net` (kind 0 traffic, 1 police)
--   R net grace                                guest: its copy goes (grace 1: the host stopped sending it)
--   C net fix nx ny nz qx qy qz qw power steer brake hb ctl vx vy vz tx ty tz flags
--                                              guest: drive that copy (VehicleCommand, the host car's controls)
-- Output (events, one per line):
--   TC key kind model c1 c2 plate x y z vx vy vz qx qy qz qw steer power brake hb flags
--                                              host: one traffic car (flags: 2 siren (police: read, else a guess),
--                                              4 no driver, 8 controls known, 16 reversing); then
--   TE                                         host: end of the round
--   TV id x y z                                host: where guest `id` is (his puppet / his car here)
--   TP x y z known hx hy hz                    guest: where we are and the host is (known 0: not seen)
--   TR net x y z vx vy vz qx qy qz qw          guest: where our copy is, after this tick's commands
--   TH net                                     guest: that copy was handed over to player_sync
--   TG net                                     guest: that copy is gone (the game removed it, a new world)
--   ST text                                    status for the overlay (once a second)
MafiaCoop = MafiaCoop or {}
local M = MafiaCoop
M.Traffic = M.Traffic or {}
local T = M.Traffic
T.version = 1

local SCAN_INTERVAL = 1.0   -- s between scans of the car table (host)
local SEND_INTERVAL = 0.05  -- s between host rounds
local RANGE = 380           -- m from a guest: host cars reported (the C++ side admits within 260 m)
local SIREN_SPEED = 14      -- m/s: a police car this fast has its siren on (when its signals cannot be read)
local SIREN_HOLD = 8        -- s the siren stays on after that
local ORPHAN_KEEP = 20      -- s a traffic car that lost its driver is still shared
local MAX_OBTAINS = 3       -- copies loading at once (guest)
local LOAD_TIMEOUT = 8      -- s an obtain or spawn may take
local RETRY_WAIT = 5        -- s before a failed copy is tried again
local BAD_AFTER = 3         -- failures of a model before it is not tried again
local COPY_IDLE = 0.3       -- s without a drive command: the copy's controls are released
local NEAR_DESPAWN = 80     -- m: a copy this close goes when unseen (DespawnEntity), else at once
local CLEAR_WAIT = 6        -- s after switching our traffic off before our own moving cars are hidden
local CLEAR_DIST = 70       -- m: our own traffic cars farther than this are hidden (closer ones when they get there)
local CLEAR_INTERVAL = 1.0
local CIV_CREW = { "civilian_white_crew", "civilian_crew", "civilians" }
local POLICE_CREW = { "police_lvl1", "police" }
local FALLBACK_PROFILE = "visuals/hero/game_main_tommy_gangster_trenchcoat"

local function log(s) if mc_log then mc_log("traffic: " .. s) end end
local function valid(o) return o ~= nil and IsObjectValid(o) end
local function same(a, b) return valid(a) and valid(b) and a == b end
local function V(x, y, z) return Math:newVector(x, y, z) end
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
local function token(s, max)
  s = tostring(s or "")
  s = s:gsub("[^%w_%-]", "")
  if s == "" then return "-" end
  return s:sub(1, max)
end

T.list = T.list or {}      -- host: entity id -> { e, id, model, kind, c1, c2, plate, drv, orphanT, sirenT }
T.copies = T.copies or {}  -- guest: net -> { net, kind, model, st, ent, qid, rv, ... }
T.hidden = T.hidden or {}  -- guest: our own traffic cars hidden while the host's traffic shows: id -> { car, crew }
T.badModels = T.badModels or {}

-- A human the traffic or police generators made (not a mission spawner's, not one of our puppets).
local function generatorHuman(h)
  local good, name = pcall(h.GetName, h)
  return good and type(name) == "string" and name:match("^behavior_object_prime%-%x+%-$") ~= nil
end

local function aiOf(h)
  local good, a = pcall(h.GetAIType, h)
  return good and tonumber(a) or 0
end

local function active(e)
  local good, a = pcall(e.IsActive, e)
  return not good or a ~= false
end

-- Cars that are not ambient traffic on this game: player_sync's cars (players' cars and their copies), the puppet
-- factories, our traffic copies, the car our player sits in.
local function ourCar(e, mine)
  if M.KeyOf and M.KeyOf(e) then return true end
  if M.fac and M.fac.car and same(M.fac.car, e) then return true end
  local N = M.Npc
  if N and N.cfac and N.cfac.car and same(N.cfac.car, e) then return true end
  if mine and same(mine, e) then return true end
  for _, c in pairs(T.copies) do
    if c.ent and same(c.ent, e) then return true end
  end
  return false
end

-- Is car `e` ambient traffic of this game (one of the host's shared traffic cars, or a car a traffic/police generator
-- driver sits at the wheel of)? player_sync.lua asks when it adopts a car (M.CarTick): such a car gets its despawn
-- immunity back off when player_sync is done with it.
function T.IsTraffic(e)
  if not valid(e) then return false end
  local good, g = pcall(e.GetGUID, e)
  local id = good and tonumber(tostring(g):match("(%d+)%s*$")) or nil
  if id and T.list[id] then return true end
  local okd, drv = pcall(e.GetSeatOwner, e, 0)
  return okd and valid(drv) and generatorHuman(drv) or false
end

local function crewOf(car)
  local out = {}
  for s = 0, 3 do
    local good, h = pcall(car.GetSeatOwner, car, s)
    if good and valid(h) then out[#out + 1] = h end
  end
  return out
end

-- ---- host -------------------------------------------------------------------------------------------------------------
function T.Scan(p)
  local seen = {}
  local mine = p:GetOwner()
  for s in string.gmatch(mc_entity_ids(10), "%d+") do
    local id = tonumber(s)
    local e = mc_entity(id)
    if e and e.GetVehicleModelName and active(e) and not ourCar(e, mine) then
      local c = T.list[id]
      local good, drv = pcall(e.GetSeatOwner, e, 0)
      drv = good and valid(drv) and drv or nil
      local traffic = drv and generatorHuman(drv) and not drv:IsDeath()
      if traffic or (c and c.e and same(c.e, e)) then
        if not c or not same(c.e, e) then
          local okm, model = pcall(e.GetVehicleModelName, e)
          model = okm and token(model, 64) or "-"
          if model ~= "-" then
            local ok1, c1 = pcall(e.GetColor, e, 1)
            local ok2, c2 = pcall(e.GetColor, e, 2)
            local okp, plate = pcall(e.GetSPZText, e)
            local police = (drv and aiOf(drv) == 4) or model:lower():find("police") ~= nil
            c = { e = e, id = id, model = model, kind = police and 1 or 0,
              c1 = (ok1 and tonumber(c1)) and math.floor(tonumber(c1)) or -1,
              c2 = (ok2 and tonumber(c2)) and math.floor(tonumber(c2)) or -1,
              plate = okp and token(plate, 16) or "-" }
            T.list[id] = c
            T.found = (T.found or 0) + 1
            if T.found <= 30 then
              log(string.format("host traffic car %d: %s (%s, colours %d/%d, plate %s)", id, model,
                police and "police" or "traffic", c.c1, c.c2, c.plate))
            end
          end
        end
        if c then
          seen[id] = true
          if traffic then
            c.drv, c.orphanT = drv, nil
          elseif not c.orphanT then
            c.orphanT = M.now  -- its driver died or got out: shared a while longer, then it is a parked car
          end
          if c.orphanT and M.now - c.orphanT > ORPHAN_KEEP then
            T.list[id], seen[id] = nil, nil
          end
        end
      end
    elseif e and T.list[id] and M.KeyOf and M.KeyOf(e) then
      -- a player took this traffic car (player_sync has it now): a remote driver's puppet needs the seat
      local key, pc = M.KeyOf(e)
      local c = T.list[id]
      if pc and pc.driver and pc.driver ~= "local" and c.drv and valid(c.drv) and not c.ejected then
        c.ejected = true
        pcall(c.drv.GetOffVehicle, c.drv)
        log("host traffic car " .. id .. " is player " .. tostring(pc.driver) .. "'s car now (" .. tostring(key)
          .. "): its driver gets out")
      end
    end
  end
  for id, _ in pairs(T.list) do
    if not seen[id] then T.list[id] = nil end
  end
end

local function viewpoints()
  local v = {}
  for id, pp in pairs(M.pup or {}) do
    if pp.veh and pp.cx then
      v[#v + 1] = { id = id, x = pp.cx, y = pp.cy, z = pp.cz }
    elseif pp.tx then
      v[#v + 1] = { id = id, x = pp.tx, y = pp.ty, z = pp.tz }
    end
  end
  return v
end

function T.HostSend(ev)
  local vps = viewpoints()
  for _, v in ipairs(vps) do ev[#ev + 1] = string.format("TV %s %.2f %.2f %.2f", tostring(v.id), v.x, v.y, v.z) end
  local n = 0
  if #vps > 0 then
    for id, c in pairs(T.list) do
      local e = c.e
      if valid(e) then
        local q = e:GetPos()
        local near = false
        for _, v in ipairs(vps) do
          if (q.x - v.x) ^ 2 + (q.y - v.y) ^ 2 <= RANGE * RANGE then near = true break end
        end
        if near then
          local s, r = e:GetSpeed(), e:GetRot()
          local flags = 0
          local st, pw, br, hb = 0, 0, 0, 0
          if mc_car_controls then
            local a, b, d, h = mc_car_controls(id)
            if a then st, pw, br, hb, flags = a, b, d, h, 8 end
          end
          local spd = math.sqrt(s.x * s.x + s.y * s.y + s.z * s.z)
          if spd > 0.5 then
            local good, d = pcall(e.GetDir, e)
            if good and d and (d.x * s.x + d.y * s.y + d.z * s.z) / spd < -0.5 then flags = flags + 16 end
          end
          if c.kind == 1 then
            -- the car's own siren / beacon when its signals can be read (car_signals.h: 128 valid, 32 siren, 16
            -- beacon); else a guess: fast, or near a player our police hunt (police_sync.lua)
            local sig = mc_car_signals and mc_car_signals(id)
            sig = sig and math.floor(sig) or 0
            if sig >= 128 then
              if math.floor(sig / 32) % 2 == 1 or math.floor(sig / 16) % 2 == 1 then flags = flags + 2 end
            else
              local P = M.Police
              if spd > SIREN_SPEED or (P and P.ChaseNear and P.ChaseNear(q.x, q.y, q.z)) then c.sirenT = M.now end
              if c.sirenT and M.now - c.sirenT < SIREN_HOLD then flags = flags + 2 end
            end
          end
          if c.orphanT then flags = flags + 4 end
          ev[#ev + 1] = string.format("TC %d %d %s %d %d %s %.3f %.3f %.3f %.3f %.3f %.3f %.5f %.5f %.5f %.5f %.3f %.3f "
            .. "%.3f %.3f %d", id, c.kind, c.model, c.c1, c.c2, c.plate, q.x, q.y, q.z, s.x, s.y, s.z, r.v.x, r.v.y, r.v.z,
            r.s, st, pw, br, hb, flags)
          n = n + 1
        end
      end
    end
  end
  ev[#ev + 1] = "TE"
  T.hostSent = n
end

-- ---- guest: our own traffic -------------------------------------------------------------------------------------------
-- T.localOn: nil = never switched (the game's own settings stand), true = ours on, false = off (the host's shows),
-- "?" = off before a load (the new world may have reset it: switched again either way).
function T.LocalTraffic(on, why)
  if T.localOn == on then return end
  if T.localOn == nil and on then T.localOn = true return end  -- never touched: leave the game's (mission's) settings
  T.localOn = on
  local t = game.traffic
  local a = pcall(t.SetEnableAmbientTrafficSpawning, t, on)
  local b = pcall(game.police.PreventPoliceSpawning, game.police, not on)
  if on then
    local n = 0
    for _, h in pairs(T.hidden) do
      if valid(h.car) then pcall(h.car.Activate, h.car) n = n + 1 end
      for _, m in ipairs(h.crew) do
        if valid(m) then pcall(m.Activate, m) end
      end
    end
    T.hidden = {}
    log(string.format("our own traffic is back (%s): ambient spawning %s, police spawning %s, %d hidden cars shown",
      why, a and "on" or "FAILED", b and "allowed" or "FAILED", n))
  else
    T.clearT = M.now + CLEAR_WAIT
    log(string.format("the host's traffic shows here (%s): our ambient spawning %s, police spawning %s", why,
      a and "off" or "FAILED", b and "prevented" or "FAILED"))
  end
end

-- Our own moving traffic (generator drivers) is hidden once out of the way while the host's traffic shows here.
-- UNVERIFIED: whether switching ambient spawning off removes the cars already there (the hiding is the fallback).
function T.ClearLocal(p)
  if M.now < (T.clearT or 0) or M.now - (T.clearScanT or -10) < CLEAR_INTERVAL then return end
  T.clearScanT = M.now
  local me, mine = p:GetPos(), p:GetOwner()
  local left, hid = 0, 0
  for s in string.gmatch(mc_entity_ids(10), "%d+") do
    local id = tonumber(s)
    local e = mc_entity(id)
    if e and e.GetVehicleModelName and not T.hidden[id] and active(e) and not ourCar(e, mine) then
      local good, drv = pcall(e.GetSeatOwner, e, 0)
      if good and valid(drv) and generatorHuman(drv) then
        local q = e:GetPos()
        if (q.x - me.x) ^ 2 + (q.y - me.y) ^ 2 > CLEAR_DIST * CLEAR_DIST then
          local crew = crewOf(e)
          for _, m in ipairs(crew) do pcall(m.Deactivate, m) end
          pcall(e.Deactivate, e)
          T.hidden[id] = { car = e, crew = crew }
          hid = hid + 1
        else
          left = left + 1
        end
      end
    end
  end
  T.localLeft = left
  T.hiddenCount = (T.hiddenCount or 0) + hid
  if hid > 0 and (T.hiddenCount <= 60 or hid > 3) then
    log(string.format("hid %d of our own traffic cars (%d near us still drive, %d hidden so far)", hid, left,
      T.hiddenCount))
  end
end

-- ---- guest: copies of the host's cars ---------------------------------------------------------------------------------
-- The crew of a copy: a profile from the game's crew presets (data_traffic.crew_presets; their layout is not known, so
-- the first profile path found in the preset), else the puppet factory's profile.
local function findProfile(t, depth)
  if depth > 4 then return nil end
  if type(t) == "string" then
    if t:find("/") then return t end
    return nil
  end
  if type(t) ~= "table" then return nil end
  for _, v in pairs(t) do
    local r = findProfile(v, depth + 1)
    if r then return r end
  end
  return nil
end

function T.CrewProfile(kind)
  T.crew = T.crew or {}
  if T.crew[kind] then return T.crew[kind] end
  local prof
  local presets = data_traffic and data_traffic.crew_presets
  if type(presets) == "table" then
    for _, name in ipairs(kind == 1 and POLICE_CREW or CIV_CREW) do
      prof = findProfile(presets[name], 0)
      if prof then
        log("copy crew (" .. (kind == 1 and "police" or "traffic") .. "): " .. prof .. " from preset " .. name)
        break
      end
    end
  end
  if not prof then
    prof = FALLBACK_PROFILE
    log("copy crew (" .. (kind == 1 and "police" or "traffic") .. "): no crew preset found, " .. prof)
  end
  T.crew[kind] = prof
  return prof
end

local function setTransform(c)
  c.ent:SetPos(V(c.tx, c.ty, c.tz))
  local q = Math:newQuat()
  q.v = V(c.qx, c.qy, c.qz)
  q.s = c.qw
  c.ent:SetRot(q)
end

function T.Forget(c, tell)
  T.copies[c.net] = nil
  if tell then T.ev[#T.ev + 1] = "TG " .. c.net end
end

local function obtaining()
  local n = 0
  for _, c in pairs(T.copies) do
    if c.st == "obtain" or c.st == "spawn" then n = n + 1 end
  end
  return n
end

function T.Fail(c, why)
  if c.qid then pcall(game.traffic.DespawnEntityImmediately, game.traffic, c.qid) end
  c.qid, c.ent, c.so, c.sp = nil, nil, nil, nil
  c.st, c.retryT = "failed", M.now + RETRY_WAIT
  T.fails = T.fails or {}
  T.fails[c.model] = (T.fails[c.model] or 0) + 1
  if T.fails[c.model] >= BAD_AFTER and not T.badModels[c.model] then
    T.badModels[c.model] = true
    log("model " .. c.model .. " failed " .. BAD_AFTER .. " times: no copies of it any more")
  end
  log("copy of host car " .. c.net .. " (" .. c.model .. ") failed: " .. why)
end

function T.CopyTick(c, p)
  if c.st == "failed" then
    if not T.badModels[c.model] and M.now >= c.retryT then c.st = "new" end
    return
  end
  if c.st == "new" then
    if obtaining() >= MAX_OBTAINS then return end
    local so, id = try("ObtainCarWithCrew", game.traffic.ObtainCarWithCrew, game.traffic, c.model,
      { T.CrewProfile(c.kind) })
    if not so then
      so, id = try("ObtainSpecificCar", game.traffic.ObtainSpecificCar, game.traffic, c.model)
      if so then log("copy of host car " .. c.net .. ": no crew (" .. c.model .. ")") end
    end
    if not so then T.Fail(c, "obtain refused") return end
    c.so, c.qid, c.st, c.t0 = so, id, "obtain", M.now
  elseif c.st == "obtain" then
    if c.so:IsDone() then
      c.sp = game.traffic:SpawnCar(c.qid, V(c.tx, c.ty, c.tz + 0.3), V(1, 0, 0))
      c.st, c.t0 = "spawn", M.now
    elseif M.now - c.t0 > LOAD_TIMEOUT then
      T.Fail(c, "obtain timeout")
    end
  elseif c.st == "spawn" then
    if c.sp:IsDone() then
      local car = game.traffic:GetSpawnedEntity(c.qid)
      if not valid(car) then T.Fail(c, "no car") return end
      c.ent, c.st, c.cmdT = car, "ready", M.now
      try("SetDespawnImmunity", car.SetDespawnImmunity, car, true)
      -- an explosion here only would leave the host's car driving on: copies do not explode
      try("DisableExplosion", car.DisableExplosion, car, true)
      c.crew = crewOf(car)
      for _, m in ipairs(c.crew) do
        pcall(m.DespawnImmunity, m, true)
        pcall(m.SetReactionOnSound, m, false)
        pcall(m.InterruptCivilWander, m)
        pcall(m.LockControls, m, true)  -- its own AI must not drive (UNVERIFIED that this stops it)
      end
      if c.c1 >= 0 and c.c2 >= 0 then pcall(car.SetColor, car, c.c1, c.c2) end
      if c.plate ~= "-" then pcall(car.SetSPZText, car, c.plate, true) end
      setTransform(c)
      c.rv = { probes = 3 }  -- player_sync.lua's speed probes are for player cars
      T.made = (T.made or 0) + 1
      if T.made <= 40 or T.made % 50 == 0 then
        log(string.format("copy of host car %d ready: %s, %s, crew %d (#%d)", c.net, c.model,
          c.kind == 1 and "police" or "traffic", #crewOf(car), T.made))
      end
    elseif M.now - c.t0 > LOAD_TIMEOUT then
      T.Fail(c, "spawn timeout")
    end
  elseif c.st == "ready" then
    if not valid(c.ent) then
      log("copy of host car " .. c.net .. " is gone (the game removed it)")
      T.Forget(c, true)
      return
    end
    -- a player took it (player_sync adopted it as his car): handed over, its crew gets out
    local key = M.KeyOf and M.KeyOf(c.ent)
    if key then
      for s = 0, 3 do
        local good, h = pcall(c.ent.GetSeatOwner, c.ent, s)
        if good and valid(h) and not same(h, p) and generatorHuman(h) then pcall(h.GetOffVehicle, h) end
      end
      if c.rv and M.RV then M.RV.Idle(c.rv, c.ent) end
      pcall(c.ent.SetDespawnImmunity, c.ent, true)
      -- its crew got out: they belong to no copy any more, the game may clean them up like any pedestrian (they kept
      -- their despawn immunity and locked controls for good before)
      for _, m in ipairs(c.crew or {}) do
        if valid(m) then
          pcall(m.DespawnImmunity, m, false)
          pcall(m.LockControls, m, false)
        end
      end
      -- player_sync's record takes the obtain id along: the car is a copy of a host car, not one of this game, so
      -- player_sync despawns it like its own copies (session end, a lost connection, the record dropped) instead of
      -- leaving it in the world with despawn immunity (player_sync.lua M.RemoveAll, M.LeftCopiesTick)
      local _, pc = M.KeyOf(c.ent)
      if pc and not pc.qid and not pc.orig and c.qid then pc.qid, pc.fromTraffic = c.qid, c.net end
      log("copy of host car " .. c.net .. " is car " .. tostring(key) .. " of player_sync now (a player took it): "
        .. "handed over" .. ((pc and pc.fromTraffic == c.net) and ", with its obtain id (player_sync despawns it when "
        .. "it is done with it)" or ""))
      T.ev[#T.ev + 1] = "TH " .. c.net
      T.copies[c.net] = nil
      return
    end
    if M.now - (c.cmdT or 0) > COPY_IDLE and c.rv and c.rv.driven and M.RV then M.RV.Idle(c.rv, c.ent) end
  end
end

function T.Remove(c, grace, p)
  if c.ent and valid(c.ent) and M.KeyOf and M.KeyOf(c.ent) then
    T.CopyTick(c, p)  -- hands it over
    return
  end
  if c.qid then
    local near = false
    if c.ent and valid(c.ent) and p then
      local q, me = c.ent:GetPos(), p:GetPos()
      near = (q.x - me.x) ^ 2 + (q.y - me.y) ^ 2 < NEAR_DESPAWN * NEAR_DESPAWN
    end
    if c.rv and M.RV and valid(c.ent) then M.RV.Idle(c.rv, c.ent) end
    if near then
      pcall(game.traffic.DespawnEntity, game.traffic, c.qid)  -- when unseen
    else
      pcall(game.traffic.DespawnEntityImmediately, game.traffic, c.qid)
    end
  end
  T.copies[c.net] = nil
  T.removed = (T.removed or 0) + 1
  if T.removed <= 40 or T.removed % 50 == 0 then
    log(string.format("copy of host car %d removed (%s, #%d)", c.net, grace and "the host no longer sends it" or
      "out of range or stale", T.removed))
  end
end

function T.Drive(c, f)
  local n = tonumber
  local tx, ty, tz = n(f[19]), n(f[20]), n(f[21])
  if not (tx and ty and tz) then return end
  -- the target also places a copy that is still loading (it spawns where the host's car is by then)
  c.tx, c.ty, c.tz, c.qx, c.qy, c.qz, c.qw = tx, ty, tz, n(f[7]) or c.qx, n(f[8]) or c.qy, n(f[9]) or c.qz,
    n(f[10]) or c.qw
  if c.st ~= "ready" or not valid(c.ent) or not M.RV then return end
  local cmd = { fix = n(f[3]) or 2, nx = n(f[4]) or 0, ny = n(f[5]) or 0, nz = n(f[6]) or 0, qx = n(f[7]) or 0,
    qy = n(f[8]) or 0, qz = n(f[9]) or 0, qw = n(f[10]) or 1, power = n(f[11]) or 0, steer = n(f[12]) or 0,
    brake = n(f[13]) or 0, hb = n(f[14]) or 0, ctl = f[15] == "1", vx = n(f[16]) or 0, vy = n(f[17]) or 0,
    vz = n(f[18]) or 0 }
  local flags = n(f[22]) or 0
  cmd.rev = math.floor(flags / 16) % 2 == 1  -- the host's car reverses: SetSpeed gets a negative km/h (RV.GiveSpeed)
  local good, err = pcall(M.RV.Drive, c.rv, c.ent, cmd, tx, ty, tz)
  if not good then
    c.errs = (c.errs or 0) + 1
    if c.errs <= 3 then log("copy " .. c.net .. " drive error: " .. tostring(err)) end
    return
  end
  c.cmdT = M.now
  local siren = math.floor(flags / 2) % 2 == 1
  if c.kind == 1 and siren ~= c.siren then
    c.siren = siren
    pcall(c.ent.SetSirenOn, c.ent, siren)
    pcall(c.ent.SetBeaconLightOn, c.ent, siren)
  end
  local noDriver = math.floor(flags / 4) % 2 == 1
  if noDriver and not c.noDriver then
    c.noDriver = true
    local okd, d = pcall(c.ent.GetSeatOwner, c.ent, 0)
    if okd and valid(d) and not d:IsDeath() then pcall(d.GetOffVehicle, d) end
    log("host car " .. c.net .. " lost its driver: ours gets out (npc_sync kills him if the host's died)")
  end
  T.ev[#T.ev + 1] = "TR " .. c.net .. " " .. M.RV.Report(c.rv, c.ent)
end

function T.DropAll(why, p)
  local n = 0
  for _, c in pairs(T.copies) do
    T.Remove(c, false, p)
    T.ev[#T.ev + 1] = "TG " .. c.net
    n = n + 1
  end
  if n > 0 then log(n .. " copies removed: " .. why) end
end

-- ---- update -----------------------------------------------------------------------------------------------------------
function T.Update(dt, cmds)
  T.ev = {}
  local ev = T.ev
  local isActive, host, mode = false, false, nil
  local p = M.Player and M.Player()
  local key = p and (tostring(game.game:GetLoadedSaveId()) .. "|" .. tostring(M.inWorldAt)) or nil
  if key ~= T.worldKey then  -- another world: our copies and hidden cars went with the old one
    T.worldKey = key
    for net, _ in pairs(T.copies) do ev[#ev + 1] = "TG " .. net end
    T.copies, T.list, T.hidden = {}, {}, {}
    if T.localOn == false then T.localOn = "?" end
    T.scanT = -10
  end
  local adds, drives, removes = {}, {}, {}
  for line in string.gmatch(cmds, "[^\n]+") do
    local f = {}
    for w in string.gmatch(line, "%S+") do f[#f + 1] = w end
    local op = f[1]
    if op == "S" then
      isActive, host = f[2] == "1", f[3] == "1"
    elseif op == "M" then
      mode = f[2] == "1"
    elseif op == "A" then
      adds[#adds + 1] = f
    elseif op == "C" then
      drives[#drives + 1] = f
    elseif op == "R" then
      removes[#removes + 1] = f
    elseif op == "B" then
      T.bumps = (T.bumps or 0) + 1
      if T.bumps <= 40 or T.bumps % 50 == 0 then
        local c = T.list[tonumber(f[4]) or -1]
        local where = ""
        if c and valid(c.e) then
          local q = c.e:GetPos()
          where = string.format(", here at %.1f %.1f %.1f", q.x, q.y, q.z)
        end
        log(string.format("guest %s: his copy of our car %s (net %s) was knocked %s m off (copy %s m/s, ours %s m/s)%s; "
          .. "ours is not moved (#%d)", f[2], tostring(f[4]), tostring(f[3]), tostring(f[5]), tostring(f[6]),
          tostring(f[7]), where, T.bumps))
      end
    end
  end
  if not (isActive and p) then
    -- copies the C++ side asked for while we are not in a world are not made: it is told, and asks again later
    if isActive then
      for _, f in ipairs(adds) do ev[#ev + 1] = "TG " .. tostring(f[2]) end
    end
    if T.localOn ~= nil and T.localOn ~= true and p then T.LocalTraffic(true, "the session ended") end
    if next(T.copies) then T.DropAll("the session ended", p) end
    return table.concat(ev, "\n")
  end
  if host then
    if T.localOn ~= nil and T.localOn ~= true then T.LocalTraffic(true, "we host now") end
    if M.now - (T.scanT or -10) >= SCAN_INTERVAL then
      T.scanT = M.now
      local good, err = pcall(T.Scan, p)
      if not good then log("scan error: " .. tostring(err)) end
    end
    if M.now - (T.sendT or -10) >= SEND_INTERVAL then
      T.sendT = M.now
      local good, err = pcall(T.HostSend, ev)
      if not good then
        log("send error: " .. tostring(err))
        ev[#ev + 1] = "TE"
      end
    end
    local n = 0
    for _, _ in pairs(T.list) do n = n + 1 end
    T.status = string.format("%d traffic cars, %d near guests", n, T.hostSent or 0)
  else
    -- where we are and where the host is (his puppet, or his car)
    local me = p:GetPos()
    local hp = M.pup and M.pup["0"]
    local hx, hy, hz, known = 0, 0, 0, 0
    if hp and hp.veh and hp.cx then
      hx, hy, hz, known = hp.cx, hp.cy, hp.cz, 1
    elseif hp and hp.tx then
      hx, hy, hz, known = hp.tx, hp.ty, hp.tz, 1
    end
    ev[#ev + 1] = string.format("TP %.2f %.2f %.2f %d %.2f %.2f %.2f", me.x, me.y, me.z, known, hx, hy, hz)
    if mode ~= nil then T.LocalTraffic(not mode, mode and "near the host" or "far from the host") end
    for _, f in ipairs(removes) do
      local c = T.copies[tonumber(f[2]) or -1]
      if c then
        local good, err = pcall(T.Remove, c, f[3] == "1", p)
        if not good then
          log("remove error: " .. tostring(err))
          T.copies[c.net] = nil
        end
      end
    end
    for _, f in ipairs(adds) do
      local net = tonumber(f[2])
      if net and not T.copies[net] then
        local n = tonumber
        local c = { net = net, kind = n(f[3]) or 0, model = f[4], c1 = n(f[5]) or -1, c2 = n(f[6]) or -1, plate = f[7],
          tx = n(f[8]) or 0, ty = n(f[9]) or 0, tz = n(f[10]) or 0, qx = n(f[11]) or 0, qy = n(f[12]) or 0,
          qz = n(f[13]) or 0, qw = n(f[14]) or 1, st = T.badModels[f[4]] and "failed" or "new", retryT = 1e9 }
        T.copies[net] = c
      end
    end
    for _, c in pairs(T.copies) do
      local good, err = pcall(T.CopyTick, c, p)
      if not good then
        c.errs = (c.errs or 0) + 1
        if c.errs <= 3 then log("copy " .. c.net .. " error: " .. tostring(err)) end
      end
    end
    for _, f in ipairs(drives) do
      local c = T.copies[tonumber(f[2]) or -1]
      if c then
        T.Drive(c, f)
      elseif not T.told or T.told[f[2]] ~= M.now then
        -- the C++ side drives a copy we do not have (lost on the way): it is told, and adds it again
        T.told = T.told or {}
        T.told[f[2]] = M.now
        ev[#ev + 1] = "TG " .. f[2]
      end
    end
    if T.localOn == false then
      local good, err = pcall(T.ClearLocal, p)
      if not good then log("clear error: " .. tostring(err)) end
    end
    local ready, loading = 0, 0
    for _, c in pairs(T.copies) do
      if c.st == "ready" then ready = ready + 1 elseif c.st ~= "failed" then loading = loading + 1 end
    end
    T.status = string.format("%s, copies %d (+%d loading), ours hidden %d, ours near %d",
      T.localOn == false and "host traffic" or "own traffic", ready, loading, T.hiddenCount or 0, T.localLeft or 0)
  end
  if M.now - (T.stT or -10) >= 1 then
    T.stT = M.now
    ev[#ev + 1] = "ST " .. (T.status or "")
  end
  if mc_trace and not host and M.now - (T.trT or -10) >= 0.5 then
    T.trT = M.now
    local hidden = 0
    for _, _ in pairs(T.hidden) do hidden = hidden + 1 end
    mc_trace("TRL", string.format("own=%d hidden=%d near=%d", T.localOn == false and 0 or 1, hidden, T.localLeft or 0))
  end
  return table.concat(ev, "\n")
end

return T.version
