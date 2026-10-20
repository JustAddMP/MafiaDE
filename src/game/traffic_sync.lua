-- MafiaCoop traffic glue (B4: ambient traffic and police shared, by region). Runs in the game's Lua state on
-- GameMainThread after player_sync.lua and npc_sync.lua, called once per script tick by src/game/traffic_sync.cpp
-- (MafiaCoop.Traffic.Update). Embedded into the dll; a copy at <dll dir>\traffic_sync.lua overrides it.
--
-- Every game is the authority for its own traffic: it reports its moving traffic and police cars (a car whose driver
-- came from the traffic or police generators, docs/game-api.md 15: `behavior_object_prime-NN-` with an empty suffix)
-- near any other player, 20 times a second; the C++ side (game/traffic_model.h) tells each other player's game which
-- of them it gets. Every game shows a copy of every other game's cars near it (a car with a crew of its own,
-- ObtainCarWithCrew), driven every tick the way the C++ side's VehicleController and Corrector say (the authority
-- driver's controls, a correction hidden in the motion, pinned when its physics does not follow), with
-- player_sync.lua's remote-car helpers (M.RV). Whose region we are in (the C++ side's RegionFollow: near the host, or
-- near a lower-id guest far from the host) only switches our own ambient traffic and police spawning off (M 1): our own
-- cars already there keep driving and are shared like any of ours, and are hidden only once out of everyone's sight
-- (beyond 70 m of every player). Parked cars, mission cars and player cars are not traffic: they stay as they were
-- (parked cars local, mission cars from each game's own mission, player cars through player_sync).
-- When a player takes over a car that is a traffic copy here (player_sync adopts it: its authority or we jacked a
-- traffic car) the copy is handed over: its crew gets out and it is player_sync's car from then on.
--
-- A copy is known by its key = authority x 65536 + net (the host's cars: their net id, as in 0.15).
-- Input (cmds, one line per command):
--   S active host me                           session running, we host, our player id
--   B from net key err copySpeed hostSpeed     player `from`'s copy of our car `key` was knocked off (logged)
--   M 1|0                                      our own spawning off (another player's region) / on (ours)
--   A key kind model c1 c2 plate x y z qx qy qz qw flags
--                                              make a copy of car `key` (kind 0 traffic, 1 police)
--   R key grace                                its copy goes (grace 1: its authority stopped sending it)
--   C key fix nx ny nz qx qy qz qw power steer brake hb ctl vx vy vz tx ty tz flags
--                                              drive that copy (VehicleCommand, the authority car's controls)
--                                              (flags as TrafficStates, plus 32: the copy's crew may get out now, the
--                                              authority's car is driverless and both stand; CrewGetsOut)
-- Output (events, one per line):
--   TC key kind model c1 c2 plate x y z vx vy vz qx qy qz qw steer power brake hb flags
--                                              one of our traffic cars (flags: 2 siren (police: read, else a guess),
--                                              4 no living driver read at the wheel this round (the C++ side's
--                                              DriverWatch sends "driverless" only after 1 s of that while slow),
--                                              8 controls known, 16 reversing); then
--   TE                                         end of the round
--   TV id x y z                                where player `id` is (his puppet / his car here), every tick
--   TP x y z                                   where we are
--   TR key x y z vx vy vz qx qy qz qw          where our copy is, after this tick's commands
--   TH key                                     that copy was handed over to player_sync
--   TG key                                     that copy is gone (the game removed it, a new world)
--   TL hidden near                             our own cars hidden / still driving near us (while our spawning is off)
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
local ORPHAN_KEEP = 20      -- s a traffic car without a driver read, standing (under 2 m/s), is still shared
local MAX_OBTAINS = 3       -- copies loading at once (guest)
local LOAD_TIMEOUT = 8      -- s an obtain or spawn may take
local RETRY_WAIT = 5        -- s before a failed copy is tried again
local BAD_AFTER = 3         -- failures of a model before it is not tried again
local COPY_IDLE = 0.3       -- s without a drive command: the copy's controls are released
local NEAR_DESPAWN = 80     -- m: a copy this close goes when unseen (DespawnEntity), else at once
local CLEAR_WAIT = 6        -- s after switching our traffic off before our own moving cars are hidden
local CLEAR_DIST = 70       -- m: our own traffic cars farther than this from every player are hidden (until then
                            --    they drive on, shared with every player near them like all our traffic)
local CLEAR_INTERVAL = 1.0
-- Crews of the copies: spawn profile names (ObtainCarWithCrew takes the name; the game hashes it, FNV-1 64). The names
-- come from the game's own Lua (data_traffic.human_models, read from the order of its constants: `civilian` ->
-- traffic_base_ambient_combined, `civilian_white_man` -> game_attractivemen, `police_lvl1` -> ai_police_lvl1; the
-- crew presets name those keys) and its tables; each one below has its hash in
-- sds_retail/combinables/spawnprofiles.sds (checked statically, 2026-10-19; ai_police_lvl1 does NOT, the
-- police_scotch ones do). The old preset lookup found no profile path in data_traffic.crew_presets (those hold
-- preset names, not paths), so every crew was the fallback Tommy. UNVERIFIED in game: that these load as crews.
-- A profile that fails twice before it ever worked is not used again (the next one is tried).
local CIV_CREW = { "characters/traffic/traffic_base_ambient_combined", "characters/game/game_attractivemen",
  "characters/traffic/traffic_workers_male" }
local POLICE_CREW = { "characters/ai/police_scotch/ai_police_triggerman",
  "characters/ai/police_scotch/ai_police_shotgunner" }
local FALLBACK_PROFILE = "visuals/hero/game_main_tommy_gangster_trenchcoat"
local CREW_BAD_AFTER = 2    -- failures of a crew profile (that never worked) before it is not used again
local SEAT_CHECK = 0.5      -- s between checks that a copy's crew sits in its seats (guest)
local RESEAT_WAIT = 1.0     -- s between two put-backs of one crew member
local RESEAT_MAX = 8        -- put-backs of one crew member in a row (seated again resets it) before we give up
local RESEAT_NEAR = 8       -- m: no put-back while our player is this close to the copy (he may be taking the car)
local MISS_LOG = 40         -- "no driver read" lines logged (then every 50th)

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

T.list = T.list or {}      -- ours: entity id -> { e, id, model, kind, c1, c2, plate, drv, missT, slowMissT, sirenT }
T.copies = T.copies or {}  -- others': key -> { key, auth, net, kind, model, st, ent, qid, rv, ... }
T.hidden = T.hidden or {}  -- our own traffic cars hidden while in another's region: id -> { car, crew }
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
  -- ped_sync.lua's copy spawner (2026-10-20 two-PC test: it went out as traffic, a crewed copy 40 m under the other
  -- player whose crew "kept leaving seat 0")
  local P = M.Peds
  if P and P.fac and P.fac.car and same(P.fac.car, e) then return true end
  if mine and same(mine, e) then return true end
  -- every puppet / copy spawner is a crewed car fixed 40 m under its player (FixCarPos); one left over from a reset
  -- spawner is no traffic either. Ambient traffic is never fixed.
  local okf, fixed = pcall(e.IsCarPosFixed, e)
  if okf and fixed then return true end
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

-- Is human `h` the crew of one of our copies of another game's traffic (seated or got out)? For npc_sync: a copy's
-- crew is a picture of the authority's driver, never one of this game's own NPCs.
function T.IsCopyCrew(h)
  if not valid(h) then return false end
  for _, c in pairs(T.copies) do
    for _, m in ipairs(c.crew or {}) do
      if same(m, h) then return true end
    end
  end
  return false
end

-- How a copy is named in the log: the host's cars by their net id (as in 0.15), others as "player A's car N".
local function label(c)
  if (c.auth or 0) == 0 then return "host car " .. tostring(c.net) end
  return "player " .. tostring(c.auth) .. "'s car " .. tostring(c.net)
end

local function crewOf(car)
  local out, seats = {}, {}
  for s = 0, 3 do
    local good, h = pcall(car.GetSeatOwner, car, s)
    if good and valid(h) then
      out[#out + 1] = h
      seats[#seats + 1] = s
    end
  end
  return out, seats
end

local function speedOf(e)
  local good, s = pcall(e.GetSpeed, e)
  if not (good and s) then return 0 end
  return math.sqrt(s.x * s.x + s.y * s.y + s.z * s.z)
end

-- Why no living traffic driver is read at the wheel of car `e` now: nil when one is (and the driver), else a reason.
local function driverMiss(e)
  local good, drv = pcall(e.GetSeatOwner, e, 0)
  if not good then return "seat read failed" end
  if not valid(drv) then return "seat 0 empty" end
  if not generatorHuman(drv) then
    local okn, name = pcall(drv.GetName, drv)
    return "seat 0 holds " .. token(okn and name or "?", 48) .. " (no generator human)", drv
  end
  local okd, dead = pcall(drv.IsDeath, drv)
  if okd and dead then return "its driver is dead", drv end
  return nil, drv
end

-- ---- authority (every game: our own traffic) --------------------------------------------------------------------------
-- A car of the list without a driver read now: noted (the C++ side's DriverWatch decides when it is driverless: 1 s
-- of this while slow). Logged with the reason and how far the host's player is (game-side driver culling or level of
-- detail far from him was the suspicion after the 0.15.0 test: 118 of 179 cars flagged while moving).
function T.NoteMiss(c, why, e)
  if c.missT then return end
  c.missT, c.missWhy = M.now, why
  T.misses = (T.misses or 0) + 1
  local q = e:GetPos()
  local p = M.Player and M.Player()
  local me = p and p:GetPos()
  local dist = me and math.sqrt((q.x - me.x) ^ 2 + (q.y - me.y) ^ 2) or -1
  local spd = speedOf(e)
  local okd, drv = pcall(e.GetSeatOwner, e, 0)
  local act = (okd and valid(drv)) and (active(drv) and "active" or "inactive") or "-"
  if T.misses <= MISS_LOG or T.misses % 50 == 0 then
    log(string.format("our traffic car %d: no driver read (%s, %s), %.1f m/s, %.0f m from us; still shared, "
      .. "driverless only after 1 s of this while slower than 2 m/s (#%d)", c.id, why, act, spd, dist, T.misses))
  end
  if mc_trace then
    mc_trace("TRO", string.format("key=%d ev=miss why=%s drv=%s spd=%.1f dist=%.0f", c.id,
      token((why:gsub("%s+", "_")), 64), act, spd, dist))
  end
end

function T.NoteBack(c, how)
  if not c.missT then return end
  local gone = M.now - c.missT
  T.backs = (T.backs or 0) + 1
  if T.backs <= MISS_LOG or T.backs % 50 == 0 then
    log(string.format("our traffic car %d: its driver is read again after %.1f s (%s, #%d)", c.id, gone, how,
      T.backs))
  end
  if mc_trace then mc_trace("TRO", string.format("key=%d ev=back after=%.2f how=%s", c.id, gone, how)) end
  c.missT, c.missWhy, c.slowMissT = nil, nil, nil
end

function T.Scan(p)
  local seen = {}
  local mine = p:GetOwner()
  for s in string.gmatch(mc_entity_ids(10), "%d+") do
    local id = tonumber(s)
    local e = mc_entity(id)
    if e and e.GetVehicleModelName and active(e) and not ourCar(e, mine) then
      local c = T.list[id]
      local miss, drv = driverMiss(e)
      local traffic = miss == nil
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
              log(string.format("our traffic car %d: %s (%s, colours %d/%d, plate %s)", id, model,
                police and "police" or "traffic", c.c1, c.c2, c.plate))
            end
          end
        end
        if c then
          seen[id] = true
          if traffic then
            c.drv = drv
            T.NoteBack(c, "scan")
          else
            T.NoteMiss(c, miss, e)
            -- no driver and standing: shared a while longer, then it is a parked car (one still rolling or driven
            -- by someone we cannot read stays shared)
            if speedOf(e) <= 2 then c.slowMissT = c.slowMissT or M.now else c.slowMissT = nil end
            if c.slowMissT and M.now - c.slowMissT > ORPHAN_KEEP then
              T.list[id], seen[id] = nil, nil
              log(string.format("our traffic car %d: no driver for %.0f s and standing: a parked car now, not shared",
                id, M.now - c.missT))
            end
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
        log("our traffic car " .. id .. " is player " .. tostring(pc.driver) .. "'s car now (" .. tostring(key)
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

function T.AuthSend(ev, vps)
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
          -- a car without a driver at the last read is read again every round (the read flickers: a driver read
          -- again clears it at once); flag 4 is this round's read, the C++ side confirms it (DriverWatch)
          if c.missT then
            local miss, drv = driverMiss(e)
            if miss then flags = flags + 4 else c.drv = drv T.NoteBack(c, "round") end
          end
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

-- ---- our own spawning (off in another player's region) ---------------------------------------------------------------
-- T.localOn: nil = never switched (the game's own settings stand), true = ours on, false = off (another's region),
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
    log(string.format("another player's traffic runs here (%s): our ambient spawning %s, police spawning %s", why,
      a and "off" or "FAILED", b and "prevented" or "FAILED"))
  end
end

-- Our own moving traffic (generator drivers) left over in another player's region drives on (shared with every player
-- near it, like all our traffic: no pop) and is hidden only once out of everyone's sight: farther than CLEAR_DIST from
-- us and from every other player. UNVERIFIED: whether switching ambient spawning off removes the cars already there
-- (the hiding is the fallback).
function T.ClearLocal(p, vps)
  if M.now < (T.clearT or 0) or M.now - (T.clearScanT or -10) < CLEAR_INTERVAL then return end
  T.clearScanT = M.now
  local me, mine = p:GetPos(), p:GetOwner()
  local pts = { { x = me.x, y = me.y } }
  for _, v in ipairs(vps or {}) do pts[#pts + 1] = v end
  local left, hid = 0, 0
  for s in string.gmatch(mc_entity_ids(10), "%d+") do
    local id = tonumber(s)
    local e = mc_entity(id)
    if e and e.GetVehicleModelName and not T.hidden[id] and active(e) and not ourCar(e, mine) then
      local good, drv = pcall(e.GetSeatOwner, e, 0)
      if good and valid(drv) and generatorHuman(drv) then
        local q = e:GetPos()
        local seen = false
        for _, v in ipairs(pts) do
          if (q.x - v.x) ^ 2 + (q.y - v.y) ^ 2 <= CLEAR_DIST * CLEAR_DIST then seen = true break end
        end
        if not seen then
          local crew = crewOf(e)
          for _, m in ipairs(crew) do pcall(m.Deactivate, m) end
          pcall(e.Deactivate, e)
          T.hidden[id] = { car = e, crew = crew }
          hid = hid + 1
        elseif (q.x - me.x) ^ 2 + (q.y - me.y) ^ 2 <= CLEAR_DIST * CLEAR_DIST then
          left = left + 1
        end
      end
    end
  end
  T.localLeft = left
  T.hiddenCount = (T.hiddenCount or 0) + hid
  if hid > 0 and (T.hiddenCount <= 60 or hid > 3) then
    log(string.format("hid %d of our own traffic cars out of everyone's sight (%d near us still drive, shared; "
      .. "%d hidden so far)", hid, left,
      T.hiddenCount))
  end
end

-- ---- copies of the other games' cars ---------------------------------------------------------------------------------
-- The crew profile of a copy: the first of CIV_CREW / POLICE_CREW (police: then the civilian ones) that has not
-- failed, else the puppet factory's Tommy.
T.crewBad = T.crewBad or {}    -- profile -> true: not used again
T.crewFails = T.crewFails or {}
T.crewOk = T.crewOk or {}      -- profile -> true: a copy got a crew with it
function T.CrewProfile(kind)
  local prof = FALLBACK_PROFILE
  for _, list in ipairs(kind == 1 and { POLICE_CREW, CIV_CREW } or { CIV_CREW }) do
    for _, name in ipairs(list) do
      if not T.crewBad[name] then prof = name break end
    end
    if prof ~= FALLBACK_PROFILE then break end
  end
  T.crewUsed = T.crewUsed or {}
  local k = kind .. ":" .. prof
  if not T.crewUsed[k] then
    T.crewUsed[k] = true
    log("copy crew (" .. (kind == 1 and "police" or "traffic") .. "): " .. prof)
  end
  return prof
end

-- A copy made with crew profile `prof` failed (`why`): a profile that never worked is not used again after
-- CREW_BAD_AFTER failures. Returns true when the failure is put down to the profile (then not to the car model).
function T.CrewFailed(prof, why)
  if not prof or prof == FALLBACK_PROFILE or T.crewOk[prof] then return false end
  T.crewFails[prof] = (T.crewFails[prof] or 0) + 1
  if T.crewFails[prof] >= CREW_BAD_AFTER and not T.crewBad[prof] then
    T.crewBad[prof] = true
    log("crew profile " .. prof .. " failed " .. T.crewFails[prof] .. " times (last: " .. why .. "): not used again")
  end
  return true
end

local function setTransform(c)
  c.ent:SetPos(V(c.tx, c.ty, c.tz))
  local q = Math:newQuat()
  q.v = V(c.qx, c.qy, c.qz)
  q.s = c.qw
  c.ent:SetRot(q)
end

function T.Forget(c, tell)
  T.copies[c.key] = nil
  if tell then T.ev[#T.ev + 1] = "TG " .. c.key end
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
  -- with a crew profile that never worked yet, the profile is the suspect, not the model
  local crewBlame = c.prof and T.CrewFailed(c.prof, why)
  c.prof = nil
  T.fails = T.fails or {}
  if not crewBlame then T.fails[c.model] = (T.fails[c.model] or 0) + 1 end
  if (T.fails[c.model] or 0) >= BAD_AFTER and not T.badModels[c.model] then
    T.badModels[c.model] = true
    log("model " .. c.model .. " failed " .. BAD_AFTER .. " times: no copies of it any more")
  end
  log("copy of " .. label(c) .. " (" .. c.model .. ") failed: " .. why)
end

function T.CopyTick(c, p)
  if c.st == "failed" then
    if not T.badModels[c.model] and M.now >= c.retryT then c.st = "new" end
    return
  end
  if c.st == "new" then
    if obtaining() >= MAX_OBTAINS then return end
    local prof = T.CrewProfile(c.kind)
    local so, id = try("ObtainCarWithCrew", game.traffic.ObtainCarWithCrew, game.traffic, c.model, { prof })
    c.prof = so and prof or nil
    if not so then
      T.CrewFailed(prof, "ObtainCarWithCrew refused")
      so, id = try("ObtainSpecificCar", game.traffic.ObtainSpecificCar, game.traffic, c.model)
      if so then log("copy of " .. label(c) .. ": no crew (" .. c.model .. ", " .. prof .. " refused)") end
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
      -- an explosion here only would leave the authority's car driving on: copies do not explode
      try("DisableExplosion", car.DisableExplosion, car, true)
      c.crew, c.seats = crewOf(car)
      if c.prof then
        if #c.crew > 0 then
          if not T.crewOk[c.prof] then log("crew profile " .. c.prof .. " works (copy of " .. label(c) .. ")") end
          T.crewOk[c.prof] = true
        else
          T.CrewFailed(c.prof, "the copy came without a crew")
        end
      end
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
        log(string.format("copy of %s ready: %s, %s, crew %d (%s) (#%d)", label(c), c.model,
          c.kind == 1 and "police" or "traffic", #c.crew, tostring(c.prof or "none"), T.made))
      end
    elseif M.now - c.t0 > LOAD_TIMEOUT then
      T.Fail(c, "spawn timeout")
    end
  elseif c.st == "ready" then
    if not valid(c.ent) then
      log("copy of " .. label(c) .. " is gone (the game removed it)")
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
      -- player_sync's record takes the obtain id along: the car is a copy of another game's car, not ours, so
      -- player_sync despawns it like its own copies (session end, a lost connection, the record dropped) instead of
      -- leaving it in the world with despawn immunity (player_sync.lua M.RemoveAll, M.LeftCopiesTick)
      local _, pc = M.KeyOf(c.ent)
      if pc and not pc.qid and not pc.orig and c.qid then pc.qid, pc.fromTraffic = c.qid, c.key end
      log("copy of " .. label(c) .. " is car " .. tostring(key) .. " of player_sync now (a player took it): "
        .. "handed over" .. ((pc and pc.fromTraffic == c.key) and ", with its obtain id (player_sync despawns it when "
        .. "it is done with it)" or ""))
      T.ev[#T.ev + 1] = "TH " .. c.key
      T.copies[c.key] = nil
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
  T.copies[c.key] = nil
  T.removed = (T.removed or 0) + 1
  if T.removed <= 40 or T.removed % 50 == 0 then
    log(string.format("copy of %s removed (%s, #%d)", label(c), grace and "its authority no longer sends it" or
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
  cmd.rev = math.floor(flags / 16) % 2 == 1  -- the authority's car reverses: SetSpeed gets a negative km/h
  local good, err = pcall(M.RV.Drive, c.rv, c.ent, cmd, tx, ty, tz)
  if not good then
    c.errs = (c.errs or 0) + 1
    if c.errs <= 3 then log("copy " .. c.key .. " drive error: " .. tostring(err)) end
    return
  end
  c.cmdT = M.now
  local siren = math.floor(flags / 2) % 2 == 1
  if c.kind == 1 and siren ~= c.siren then
    c.siren = siren
    pcall(c.ent.SetSirenOn, c.ent, siren)
    pcall(c.ent.SetBeaconLightOn, c.ent, siren)
  end
  -- 32: the authority's car is driverless (confirmed there: no driver read for 1 s while slow) and both cars stand: our
  -- driver gets out. Never out of a moving copy; the flag gone (a driver read again, or it drives on): back in.
  local crewOut = math.floor(flags / 32) % 2 == 1
  if crewOut and not c.crewOut then
    c.crewOut = true
    T.outs = (T.outs or 0) + 1
    local okd, d = pcall(c.ent.GetSeatOwner, c.ent, 0)
    if okd and valid(d) and not d:IsDeath() then pcall(d.GetOffVehicle, d) end
    if T.outs <= 40 or T.outs % 50 == 0 then
      log(string.format("%s has no driver (for 1 s and standing): ours gets out (npc_sync kills him if the "
        .. "host's died) (#%d)", label(c), T.outs))
    end
    if mc_trace then mc_trace("TRO", string.format("a=%d net=%d ev=out", c.auth, c.net)) end
  elseif not crewOut and c.crewOut then
    c.crewOut = false
    log(string.format("%s is driven again: our crew gets back in", label(c)))
    if mc_trace then mc_trace("TRO", string.format("a=%d net=%d ev=in", c.auth, c.net)) end
  end
  if not c.crewOut and M.now - (c.seatT or -10) >= SEAT_CHECK then
    c.seatT = M.now
    local good, err = pcall(T.KeepSeated, c)
    if not good then
      c.errs = (c.errs or 0) + 1
      if c.errs <= 3 then log("copy " .. c.key .. " seat check error: " .. tostring(err)) end
    end
  end
  T.ev[#T.ev + 1] = "TR " .. c.key .. " " .. M.RV.Report(c.rv, c.ent)
end

-- A copy keeps its crew seated: one found out of its seat (its AI left the car: a bump, a sound, a car-jack started
-- and given up) is put back with a non-animated GetOnVehicle (docs/game-api.md 7, "Seating"), at most once a second
-- and RESEAT_MAX times in a row. Dead ones stay out (npc_sync killed him: the host's driver died); nobody is put back
-- while our player is near the copy (he may be getting in: player_sync then takes the car over) or while another
-- human holds the seat.
function T.KeepSeated(c)
  local ent = c.ent
  if not (c.crew and c.seats and valid(ent)) then return end
  local p = M.Player and M.Player()
  if p then
    local q, me = ent:GetPos(), p:GetPos()
    if (q.x - me.x) ^ 2 + (q.y - me.y) ^ 2 < RESEAT_NEAR * RESEAT_NEAR then return end
  end
  c.reseat = c.reseat or {}
  for i, m in ipairs(c.crew) do
    local seat = c.seats[i]
    local r = c.reseat[i] or { n = 0, t = -10 }
    c.reseat[i] = r
    if valid(m) and seat and not m:IsDeath() then
      local okh, holder = pcall(ent.GetSeatOwner, ent, seat)
      if okh and same(holder, m) then
        r.n = 0
      elseif okh and not valid(holder) and r.n < RESEAT_MAX and M.now - r.t >= RESEAT_WAIT then
        r.n, r.t = r.n + 1, M.now
        local okg = pcall(m.GetOnVehicle, m, ent, seat + 1, false, "WALK")
        T.reseats = (T.reseats or 0) + 1
        local mq, cq = m:GetPos(), ent:GetPos()
        if T.reseats <= 40 or T.reseats % 50 == 0 then
          log(string.format("copy of %s: crew member %d was out of seat %d (%.1f m from the car, car %.1f m/s): "
            .. "put back%s (try %d, #%d)", label(c), i, seat,
            math.sqrt((mq.x - cq.x) ^ 2 + (mq.y - cq.y) ^ 2 + (mq.z - cq.z) ^ 2), speedOf(ent),
            okg and "" or " (GetOnVehicle failed)", r.n, T.reseats))
        end
        if mc_trace then
          mc_trace("TRO", string.format("a=%d net=%d ev=reseat seat=%d try=%d ok=%d", c.auth, c.net, seat, r.n,
            okg and 1 or 0))
        end
        if r.n == RESEAT_MAX then
          log(string.format("copy of %s: crew member %d keeps leaving seat %d: no more put-backs", label(c), i,
            seat))
        end
      end
    end
  end
end

function T.DropAll(why, p)
  local n = 0
  for _, c in pairs(T.copies) do
    T.Remove(c, false, p)
    T.ev[#T.ev + 1] = "TG " .. c.key
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
    for k, _ in pairs(T.copies) do ev[#ev + 1] = "TG " .. k end
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
      isActive, host, T.me = f[2] == "1", f[3] == "1", f[4]
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
        log(string.format("player %s: his copy of our car %s (net %s) was knocked %s m off (copy %s m/s, ours %s m/s)%s; "
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
  -- where everyone is: us, and every other player (his puppet, or his car) for the C++ side's regions and interest
  local me = p:GetPos()
  ev[#ev + 1] = string.format("TP %.2f %.2f %.2f", me.x, me.y, me.z)
  local vps = viewpoints()
  for _, v in ipairs(vps) do ev[#ev + 1] = string.format("TV %s %.2f %.2f %.2f", tostring(v.id), v.x, v.y, v.z) end
  -- our own traffic (every game is the authority for the traffic its game spawned): scanned once a second, reported
  -- 20 times a second while another player is in the world
  if M.now - (T.scanT or -10) >= SCAN_INTERVAL then
    T.scanT = M.now
    local good, err = pcall(T.Scan, p)
    if not good then log("scan error: " .. tostring(err)) end
  end
  if M.now - (T.sendT or -10) >= SEND_INTERVAL then
    T.sendT = M.now
    local good, err = pcall(T.AuthSend, ev, vps)
    if not good then
      log("send error: " .. tostring(err))
      ev[#ev + 1] = "TE"
    end
  end
  -- our own spawning: off in another player's region (the host's game: always on)
  if mode ~= nil then T.LocalTraffic(not mode, mode and "in another player's region" or "our own region") end
  -- the other games' cars near us
  for _, f in ipairs(removes) do
    local c = T.copies[tonumber(f[2]) or -1]
    if c then
      local good, err = pcall(T.Remove, c, f[3] == "1", p)
      if not good then
        log("remove error: " .. tostring(err))
        T.copies[c.key] = nil
      end
    end
  end
  for _, f in ipairs(adds) do
    local k = tonumber(f[2])
    if k and not T.copies[k] then
      local n = tonumber
      local c = { key = k, auth = math.floor(k / 65536), net = k % 65536, kind = n(f[3]) or 0, model = f[4],
        c1 = n(f[5]) or -1, c2 = n(f[6]) or -1, plate = f[7], tx = n(f[8]) or 0, ty = n(f[9]) or 0, tz = n(f[10]) or 0,
        qx = n(f[11]) or 0, qy = n(f[12]) or 0, qz = n(f[13]) or 0, qw = n(f[14]) or 1,
        st = T.badModels[f[4]] and "failed" or "new", retryT = 1e9 }
      T.copies[k] = c
    end
  end
  for _, c in pairs(T.copies) do
    local good, err = pcall(T.CopyTick, c, p)
    if not good then
      c.errs = (c.errs or 0) + 1
      if c.errs <= 3 then log("copy " .. c.key .. " error: " .. tostring(err)) end
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
    local good, err = pcall(T.ClearLocal, p, vps)
    if not good then log("clear error: " .. tostring(err)) end
  else
    T.localLeft = 0
  end
  local ready, loading, ours, nd = 0, 0, 0, 0
  for _, c in pairs(T.copies) do
    if c.st == "ready" then ready = ready + 1 elseif c.st ~= "failed" then loading = loading + 1 end
  end
  for _, c in pairs(T.list) do
    ours = ours + 1
    if c.missT then nd = nd + 1 end
  end
  local hidden = 0
  for _, _ in pairs(T.hidden) do hidden = hidden + 1 end
  ev[#ev + 1] = string.format("TL %d %d", hidden, T.localLeft or 0)
  T.status = string.format("%s, ours %d (%d near others, %d without a driver read), copies %d (+%d loading), ours "
    .. "hidden %d, ours near %d, crews out %d, put back %d", T.localOn == false and "spawning off" or "spawning on",
    ours, T.hostSent or 0, nd, ready, loading, T.hiddenCount or 0, T.localLeft or 0, T.outs or 0, T.reseats or 0)
  if M.now - (T.stT or -10) >= 1 then
    T.stT = M.now
    ev[#ev + 1] = "ST " .. (T.status or "")
  end
  return table.concat(ev, "\n")
end

return T.version
