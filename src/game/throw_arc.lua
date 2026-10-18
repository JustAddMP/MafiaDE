-- MafiaCoop throw glue (0.15.0 "grenade arc": the flight of a thrown grenade / molotov / dynamite on every game). Runs in
-- the game's Lua state on GameMainThread, called once per script tick by src/game/throw_sync.cpp
-- (MafiaCoop.Arc.Update). Embedded into the dll; a copy at <dll dir>\throw_arc.lua overrides it.
-- docs/game-api.md 23 "The flight", docs/protocol.md "Explosions" (Throw 0x0242).
--
-- Here (the thrower):
--   - while our player lines up a throw (an explosive in hand, or the aim input held / PLAYER_AIM_ENTER with an
--     explosive selected and no gun in hand) every tick: mc_arc_aim(...) with where we stand, our head, our hand (a
--     hand bone if GetBonePos knows one of HAND_BONES, else the head + an offset), the camera and its direction, and
--     the aim model's arc (camera direction turned up AIM_PITCH_UP, AIM_SPEED m/s, gravity 9.81: UNVERIFIED guesses)
--     with where it lands (mc_raycast along it, twice a second). The C++ side writes GA trace records from it.
--   - on the release (WEAPON_THROWN, explosion_sync.lua B.LocalTick calls A.OnThrown): mc_arc_begin gives the throw its
--     number (the explosion detector gets it with the throw: the Explosion sent for it names it), mc_arc_model the
--     aim model's launch; then the real projectile is looked for (mc_entity_ids_of PROJ_TYPES: a grenade 25, molotov
--     26, projectile explosive 88 or satchel charge 90 entity within SEARCH_REACH of our hand, new since the aim
--     started or moved since) for SEARCH_TIME and, once found, its position goes to mc_arc_sample every tick only
--     while the C++ side measures the launch (3 positions or 0.3 s: the one Throw message). Then it is followed
--     cheaply: read again when mc_arc_sample says (every 0.2 s, or the next tick after a bounce / turn), and at each of
--     our EXPLOSIVE_BOUNCE sounds (explosion_sync.lua B.OnHear -> A.OnBounce -> mc_arc_bounce); the C++ Tracker sends
--     a ThrowFix at a bounce, a sharp turn, when it has settled and when it is gone (mc_arc_gone), at most 8.
-- On the other games:
--   - "F" commands: each flight of another game, where it is now (the C++ Book simulates it: arc, ray casts against
--     this world, bounces, rolling, settled; at the explosion's true position once that came). Shown with a
--     marker and a fading trail that our overlay draws ("D" events: kind, head, alpha, screen x y, distance) at
--     game.game:ProjectPointToScreen of those points. Nothing is spawned in the world: it cannot hit or hurt anyone.
--     The explosion itself is explosion_sync.lua's picture when the Explosion arrives (the flight ends then, "X").
--
-- Input (cmds, one line per command):
--   S active                                        session running
--   F from seq kind flags x y z held age src        a flight of player `from` (kind as Explosion: 1 grenade 2 molotov
--                                                   3 dynamite; flags 1 fire 2 measured; src sim / roll /
--                                                   settled / boom / arc)
--   X from seq why                                  that flight is over without an explosion (fuse, timeout, left)
-- Output (events, one per line):
--   D kind head alpha x y dist                      one overlay dot (head 1: the marker, 0: a trail point)
--   ST text                                         status for the overlay (once a second)
-- Sync trace: "GS" (projectile search result), "GV" (a flight shown, 2 Hz), docs/sync-trace.md.
MafiaCoop = MafiaCoop or {}
local M = MafiaCoop
M.Arc = M.Arc or {}
local A = M.Arc
A.version = 1

local AIM_SPEED = 16.0       -- m/s: the aim model's throw speed (UNVERIFIED; compare GE model_speed vs speed)
local AIM_PITCH_UP = 0.26    -- rad (15 deg) the throw goes above the camera direction (UNVERIFIED)
local GRAVITY = 9.81
local ARC_STEP = 0.05        -- s per segment of the predicted arc
local ARC_MAX = 4.0          -- s of predicted arc at most
local LAND_INTERVAL = 0.5    -- s between two landing predictions while aiming (ray casts)
local HAND_BONES = { "RightHand", "R_Hand", "Bip01 R Hand", "Bip01_R_Hand", "hand_r", "r_hand", "RHand", "Hand_R",
  "RightHandMiddle1" }
local PROJ_TYPES = "25 26 88 90"
local SEARCH_INTERVAL = 0.1  -- s between two projectile searches after the release
local SEARCH_TIME = 0.3      -- s after the release the projectile is looked for (the launch is measured by then)
local SEARCH_REACH = 8       -- m from our hand
local MOVED = 0.3            -- m: an entity that was there already counts if it moved this much (a pooled one)
local TRAIL_STEP = 0.05      -- s between two trail points
local TRAIL_N = 14
local FAR = 250              -- m from the camera: not drawn beyond

local function log(s) if mc_log then mc_log("arc: " .. s) end end
local function trace(kind, text) if mc_trace then mc_trace(kind, text) end end
local function valid(o) return o ~= nil and IsObjectValid(o) end
local function V(x, y, z) return Math:newVector(x, y, z) end
local function f3(x, y, z) return string.format("%.2f,%.2f,%.2f", x, y, z) end
local function dist(a, b) return math.sqrt((a.x - b.x) ^ 2 + (a.y - b.y) ^ 2 + (a.z - b.z) ^ 2) end
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
-- math.atan2 if this VM has it (Lua 5.1 does; not checked in HKS), else from math.atan
local atan2 = math.atan2 or function(y, x)
  if x > 0 then return math.atan(y / x) end
  if x < 0 then return math.atan(y / x) + (y >= 0 and math.pi or -math.pi) end
  return y > 0 and math.pi / 2 or (y < 0 and -math.pi / 2 or 0)
end
local function tok(s) s = tostring(s or ""); s = s:gsub("[^%w_%-%.:/]", ""); return s ~= "" and s or "-" end

A.fl = A.fl or {}         -- other games' flights shown here: key "from:seq" -> { kind, x, y, z, held, trail, ... }
A.subs = A.subs or {}
A.stats = A.stats or { throws = 0, found = 0, notFound = 0, shown = 0, ended = 0, aims = 0 }
A.events = {}

-- ---------------------------------------------------------------------------------------------------------------
-- What we hold
-- ---------------------------------------------------------------------------------------------------------------
function A.Explosive(p)
  local good, w = pcall(p.InventoryGetSelectedExplosive, p)
  if not good then return "" end
  if type(w) == "number" then
    local g2, n = pcall(game.game.GetWeaponNameFromID, game.game, w)
    w = g2 and n or w
  end
  return tostring(w or "")
end

function A.KindOf(w)
  if w == "exp_grenade" then return 1 end
  if w == "exp_molotov" then return 2 end
  if w == "exp_satchel" then return 3 end
  return 0
end

-- our hand: a bone GetBonePos knows (it answers the entity's position for a name it does not know), else the head
-- moved to where a right hand holding something up would be. Returns the vector and whether it is a bone.
function A.Hand(p)
  local q = p:GetPos()
  local names = A.handBone and { A.handBone } or (A.noHandBone and {} or HAND_BONES)
  for _, n in ipairs(names) do
    local good, b = pcall(p.GetBonePos, p, n)
    if good and b and dist(b, q) > 0.3 then
      if A.handBone ~= n then A.handBone = n; log("hand bone: " .. n) end
      return b, true
    end
  end
  if not A.handBone and not A.noHandBone then
    A.noHandBone = true
    log("no hand bone known to GetBonePos (" .. table.concat(HAND_BONES, ", ") .. "): the head + an offset is used")
  end
  local good, h = pcall(p.GetBonePos, p, "Head")
  if not good or not h or dist(h, q) < 0.3 then h = V(q.x, q.y, q.z + 1.65) end
  local d = p:GetDir()
  local l = math.sqrt(d.x * d.x + d.y * d.y)
  if l < 1e-3 then d, l = V(0, 1, 0), 1 end
  local fx, fy = d.x / l, d.y / l
  return V(h.x + fy * 0.3 + fx * 0.25, h.y - fx * 0.3 + fy * 0.25, h.z + 0.1), false
end

-- the aim model's launch velocity from the camera direction
function A.ModelLaunch(cd)
  local h = math.sqrt(cd.x * cd.x + cd.y * cd.y)
  local hx, hy = cd.x, cd.y
  local pitch = atan2(cd.z, h) + AIM_PITCH_UP
  if h < 1e-4 then hx, hy, h = 0, 1, 1 end
  if pitch > 1.48 then pitch = 1.48 elseif pitch < -1.48 then pitch = -1.48 end
  local c = math.cos(pitch)
  return V(AIM_SPEED * c * hx / h, AIM_SPEED * c * hy / h, AIM_SPEED * math.sin(pitch))
end

-- where an arc from o with velocity v lands (the first ray cast hit along it, not next to o): landing, time; or nil
function A.Predict(o, v)
  if not mc_raycast then return nil end
  local px, py, pz = o.x, o.y, o.z
  local t = 0
  while t < ARC_MAX do
    t = t + ARC_STEP
    local x, y, z = o.x + v.x * t, o.y + v.y * t, o.z + v.z * t - 0.5 * GRAVITY * t * t
    local hit, hx, hy, hz = mc_raycast(px, py, pz, x, y, z)
    if hit and hx and ((hx - o.x) ^ 2 + (hy - o.y) ^ 2 + (hz - o.z) ^ 2) > 1.44 then
      return V(hx, hy, hz), t
    end
    px, py, pz = x, y, z
  end
  return nil
end

-- every projectile entity now: id -> position
function A.Scan()
  local out, n = {}, 0
  if not (mc_entity_ids_of and mc_entity) then return out, 0 end
  for s in string.gmatch(mc_entity_ids_of(PROJ_TYPES), "%d+") do
    local id = tonumber(s)
    local e = mc_entity(id)
    if valid(e) then
      local good, q = pcall(e.GetPos, e)
      if good and q then out[id] = q; n = n + 1 end
    end
  end
  return out, n
end

-- ---------------------------------------------------------------------------------------------------------------
-- The game's messages: the player's aim mode (per loaded world, re-registered after a load)
-- ---------------------------------------------------------------------------------------------------------------
function A.Unwatch()
  for _, id in ipairs(A.subs.ids or {}) do pcall(game.game.UnregisterBlanketEvent, game.game, id) end
  A.subs = {}
end

function A.Watch()
  local key = tostring(game.game:GetLoadedSaveId()) .. "|" .. tostring(M.inWorldAt)
  if A.subs.key == key then return end
  A.Unwatch()
  local ids = {}
  local hm = enums.HumanMessages or {}
  for _, it in ipairs({ { hm.PLAYER_AIM_ENTER, true, "PLAYER_AIM_ENTER" }, { hm.PLAYER_AIM_LEAVE, false, "PLAYER_AIM_LEAVE" } }) do
    if it[1] then
      local on, name = it[2], it[3]
      local good, id = pcall(game.game.RegisterBlanketEvent, game.game, {}, function(ctx, m)
        local ok, err = pcall(function()
          if tostring(m:GetReceiverGUID()) ~= M.localGuid then return end
          A.aimMsg = on
          A.aimMsgs = (A.aimMsgs or 0) + 1
          if A.aimMsgs <= 10 then log(name .. " for us") end
        end)
        if not ok then
          errs[name] = (errs[name] or 0) + 1
          if errs[name] <= 5 then log(name .. " event error: " .. tostring(err)) end
        end
      end, enums.EventType.HUMAN, it[1])
      if good and tonumber(id) and tonumber(id) >= 0 then ids[#ids + 1] = id else log("could not watch " .. name) end
    else
      log("could not watch " .. it[3] .. " (no such enum in this build)")
    end
  end
  A.subs = { key = key, ids = ids }
  A.aimMsg = false
end

-- ---------------------------------------------------------------------------------------------------------------
-- The thrower: aim, release, the projectile
-- ---------------------------------------------------------------------------------------------------------------
function A.Aiming(p)
  local sel = tostring(p:InventoryGetSelected() or "")
  local w = A.Explosive(p)
  local gun = false
  local good, g = pcall(p.HaveFireWeaponInHand, p)
  if good then gun = g and true or false end
  local how = {}
  if sel:find("^exp_") then how[#how + 1] = "hand"; w = sel end
  if not gun and w ~= "" then
    if M.aimHeld then how[#how + 1] = "input" end
    if A.aimMsg then how[#how + 1] = "msg" end
  end
  if #how == 0 or p:GetOwner() then return false end
  return true, w, table.concat(how, "+")
end

function A.AimTick(p)
  local on, w, how = A.Aiming(p)
  if not on then
    A.aimOn = false
    return
  end
  if not A.aimOn then  -- a new aim: the projectiles that exist now are not ours
    A.aimOn = true
    A.base = A.Scan()
    A.landT = nil
    A.stats.aims = A.stats.aims + 1
  end
  local q = p:GetPos()
  local good, h = pcall(p.GetBonePos, p, "Head")
  if not good or not h then h = V(q.x, q.y, q.z + 1.65) end
  local hand, bone = A.Hand(p)
  local cam = game.cameramanager:GetPlayerCamera()
  local cp, cd = cam and cam:GetPos(), cam and cam:GetDir()
  if not (cp and cd) then cp, cd = h, p:GetDir() end
  if M.now - (A.landT or -10) >= LAND_INTERVAL then
    A.landT = M.now
    A.land, A.landTime = A.Predict(hand, A.ModelLaunch(cd))
  end
  local l = A.land
  mc_arc_aim(tok(w), how, q.x, q.y, q.z, h.x, h.y, h.z, hand.x, hand.y, hand.z, bone and 1 or 0, cp.x, cp.y, cp.z,
    cd.x, cd.y, cd.z, l and 1 or 0, l and l.x or 0, l and l.y or 0, l and l.z or 0, A.landTime or 0)
end

-- WEAPON_THROWN of our player (explosion_sync.lua B.LocalTick): returns the throw number (a string) for the
-- explosion detector
function A.OnThrown(p, w)
  if not mc_arc_begin then return nil end
  local q = p:GetPos()
  local kind = A.KindOf(w)
  local seq = mc_arc_begin(tok(w), kind, q.x, q.y, q.z)
  local hand = A.Hand(p)
  local cam = game.cameramanager:GetPlayerCamera()
  local cd = cam and cam:GetDir() or p:GetDir()
  local v = A.ModelLaunch(cd)
  mc_arc_model(seq, hand.x, hand.y, hand.z, v.x, v.y, v.z)
  local base = A.aimOn and A.base or A.Scan()
  A.cur = { seq = seq, t0 = M.now, hand = hand, base = base or {}, nextScan = M.now, scans = 0, w = w }
  A.stats.throws = A.stats.throws + 1
  return seq
end

function A.SearchTick()
  local c = A.cur
  if not c or c.id or c.gaveUp or M.now < c.nextScan then return end
  if M.now - c.t0 > SEARCH_TIME then
    c.gaveUp = true
    A.stats.notFound = A.stats.notFound + 1
    if A.stats.notFound <= 20 then
      log(string.format("throw #%s: no projectile entity found near our hand (%d scans, %d candidates seen); the aim model's launch is sent if it was not measured",
        c.seq, c.scans, c.seen or 0))
    end
    trace("GS", string.format("seq=%s found=0 scans=%d seen=%d", c.seq, c.scans, c.seen or 0))
    return
  end
  c.nextScan = M.now + SEARCH_INTERVAL
  c.scans = c.scans + 1
  local now = A.Scan()
  local best, bestD, bestP = nil, SEARCH_REACH, nil
  local seen = 0
  for id, q in pairs(now) do
    seen = seen + 1
    local was = c.base[id]
    local d = dist(q, c.hand)
    if d <= bestD and (was == nil or dist(was, q) >= MOVED) then best, bestD, bestP = id, d, q end
  end
  c.seen = math.max(c.seen or 0, seen)
  if not best then return end
  c.id, c.type = best, best % 256
  A.stats.found = A.stats.found + 1
  if A.stats.found <= 30 then
    log(string.format("throw #%s: projectile entity %d (type %d) found %.1f m from our hand, %.2f s after the release",
      c.seq, best, best % 256, bestD, M.now - c.t0))
  end
  trace("GS", string.format("seq=%s found=1 id=%d type=%d d=%.2f after=%.3f scans=%d p=%s", c.seq, best, best % 256,
    bestD, M.now - c.t0, c.scans, f3(bestP.x, bestP.y, bestP.z)))
  mc_arc_sample(c.seq, bestP.x, bestP.y, bestP.z)
end

-- the projectile's position now, or nil when it cannot be read (gone)
local function readProjectile(c)
  local e = mc_entity(c.id)
  local q = valid(e) and try("projectile GetPos", e.GetPos, e)
  if q then
    local good, act = pcall(e.IsActive, e)
    if good and act == false then q = nil end
  end
  return q
end

function A.TrackTick()
  local c = A.cur
  if not c then return end
  if not mc_arc_active(c.seq) then  -- settled, gone, exploded, timed out: let go
    A.cur = nil
    return
  end
  if not c.id or c.sampledAt == M.now or M.now < (c.nextRead or 0) then return end
  local q = readProjectile(c)
  if not q then
    if A.stats.found <= 30 then log(string.format("throw #%s: projectile gone after %.2f s", c.seq, M.now - c.t0)) end
    mc_arc_gone(c.seq)
    A.cur = nil
    return
  end
  c.sampledAt = M.now
  local nextIn = mc_arc_sample(c.seq, q.x, q.y, q.z) or 0.2
  if nextIn < 0 then A.cur = nil else c.nextRead = M.now + nextIn end
end

-- an EXPLOSIVE_BOUNCE sound of ours (explosion_sync.lua B.OnHear): read the projectile now (its velocity comes from
-- the next tick's reading); without one the sound's position is the fix
function A.OnBounce(x, y, z)
  local c = A.cur
  if not (c and mc_arc_active(c.seq)) then return end
  local q = c.id and readProjectile(c)
  local nextIn
  if q then
    nextIn = mc_arc_bounce(c.seq, q.x, q.y, q.z, 1)
    c.sampledAt = M.now
  else
    nextIn = mc_arc_bounce(c.seq, x, y, z, 0)
  end
  if nextIn and nextIn >= 0 then c.nextRead = M.now + nextIn end
end

-- ---------------------------------------------------------------------------------------------------------------
-- The other games' flights: marker + trail on our overlay
-- ---------------------------------------------------------------------------------------------------------------
local function project(x, y, z)
  local good, sx, sy, on = pcall(game.game.ProjectPointToScreen, game.game, V(x, y, z))
  if good and on and sx and sy then return sx, sy end
  return nil
end

function A.ShowTick(flights)
  local cam = game.cameramanager:GetPlayerCamera()
  local c = cam and cam:GetPos()
  if not c then return end
  for key, f in pairs(flights) do
    local s = A.fl[key]
    if not s then
      s = { trail = {}, t = M.now, kind = f.kind }
      A.fl[key] = s
      A.stats.shown = A.stats.shown + 1
      if A.stats.shown <= 40 then
        log(string.format("showing player %s's %s (#%s) from %.1f %.1f %.1f (%s)", f.from,
          ({ [0] = "throw", "grenade", "molotov", "dynamite" })[f.kind] or "throw", f.seq, f.x, f.y, f.z, f.src))
      end
    end
    local tr = s.trail
    if M.now - (s.trailT or -10) >= TRAIL_STEP and not (f.held and s.held) then
      s.trailT = M.now
      tr[#tr + 1] = { f.x, f.y, f.z }
      if #tr > TRAIL_N then table.remove(tr, 1) end
    end
    s.held = f.held
    local d = math.sqrt((f.x - c.x) ^ 2 + (f.y - c.y) ^ 2 + (f.z - c.z) ^ 2)
    local onScreen = false
    if d <= FAR then
      for i, p in ipairs(tr) do
        if i < #tr then
          local sx, sy = project(p[1], p[2], p[3])
          if sx then
            A.events[#A.events + 1] = string.format("D %d 0 %.2f %.4f %.4f %.1f", f.kind, 0.15 + 0.6 * i / #tr, sx, sy, d)
          end
        end
      end
      local sx, sy = project(f.x, f.y, f.z)
      if sx then
        onScreen = true
        A.events[#A.events + 1] = string.format("D %d 1 1 %.4f %.4f %.1f", f.kind, sx, sy, d)
      end
    end
    if M.now - (s.trT or -10) >= 0.5 then
      s.trT = M.now
      trace("GV", string.format("from=%s seq=%s p=%s src=%s held=%d age=%.2f d=%.1f screen=%d", f.from, f.seq,
        f3(f.x, f.y, f.z), f.src, f.held and 1 or 0, f.age, d, onScreen and 1 or 0))
    end
  end
  for key, _ in pairs(A.fl) do
    if not flights[key] then A.fl[key] = nil end
  end
end

-- ---------------------------------------------------------------------------------------------------------------
-- Entry point
-- ---------------------------------------------------------------------------------------------------------------
function A.Update(dt, cmds)
  A.events = {}
  local active = false
  local flights = {}
  for line in string.gmatch(cmds, "[^\n]+") do
    local f = {}
    for w in string.gmatch(line, "%S+") do f[#f + 1] = w end
    if f[1] == "S" then
      active = f[2] == "1"
    elseif f[1] == "F" then
      local key = f[2] .. ":" .. f[3]
      flights[key] = { from = f[2], seq = f[3], kind = tonumber(f[4]) or 0, flags = tonumber(f[5]) or 0,
        x = tonumber(f[6]), y = tonumber(f[7]), z = tonumber(f[8]), held = f[9] == "1", age = tonumber(f[10]) or 0,
        src = f[11] or "-" }
      if not (flights[key].x and flights[key].y and flights[key].z) then flights[key] = nil end
    elseif f[1] == "X" then
      local key = (f[2] or "") .. ":" .. (f[3] or "")
      A.fl[key] = nil
      A.stats.ended = A.stats.ended + 1
    end
  end
  local p = M.Player and M.Player()
  if not (active and p) then
    if A.subs.key then
      if p then pcall(A.Unwatch) else A.subs = {} end
    end
    A.fl, A.cur, A.aimOn = {}, nil, false
    return ""
  end
  local good, err = pcall(A.Watch)
  if not good then log("watch error: " .. tostring(err)) end
  good, err = pcall(A.AimTick, p)
  if not good then log("aim error: " .. tostring(err)) end
  good, err = pcall(A.SearchTick)
  if not good then log("search error: " .. tostring(err)) end
  good, err = pcall(A.TrackTick)
  if not good then log("track error: " .. tostring(err)); A.cur = nil end
  good, err = pcall(A.ShowTick, flights)
  if not good then log("show error: " .. tostring(err)) end
  if M.now - (A.stT or -10) >= 1 then
    A.stT = M.now
    local s = A.stats
    A.events[#A.events + 1] = string.format("ST aims %d, throws %d (projectile found %d, not %d), shown %d, ended %d%s",
      s.aims, s.throws, s.found, s.notFound, s.shown, s.ended, A.handBone and (", hand " .. A.handBone) or "")
  end
  return table.concat(A.events, "\n")
end

-- Tests and the dev console (tools\devlua.ps1), in a session:
--   MafiaCoop.Arc.TestFlight()  shows a made-up throw of "player 254" from 2 m in front of us (as if his Throw had
--                               arrived; the marker and trail on our overlay; it ends after 15 s or TestEnd())
--   MafiaCoop.Arc.TestEnd(seq)  ends it as if his explosion had arrived
--   MafiaCoop.Arc.Probe()       lists every projectile entity now (types 25, 26, 88, 90) with its distance from us:
--                               run it right after throwing a grenade to see whether the game makes one we can read
--   MafiaCoop.Arc.TestHand(on)  experimental: the first puppet holds a grenade model (ModelToHands(0, "exp_grenade",
--                               ""); TestHand(false) gives ModelToHands(0, "", "")); not used by the sync
function A.TestFlight(kind)
  local p = M.Player()
  local q, d = p:GetPos(), p:GetDir()
  local v = A.ModelLaunch(V(d.x, d.y, 0.2))
  local seq = mc_arc_test_flight(q.x + d.x * 2, q.y + d.y * 2, q.z + 1.6, v.x, v.y, v.z, kind or 1)
  A.testSeq = seq
  return "test flight #" .. tostring(seq)
end

function A.TestEnd(seq)
  local p = M.Player()
  local q = p:GetPos()
  mc_arc_test_end(tostring(seq or A.testSeq or 0), q.x, q.y, q.z)
  return "ended"
end

function A.Probe()
  local p = M.Player()
  local q = p:GetPos()
  local s, n = A.Scan()
  local out = {}
  for id, at in pairs(s) do
    out[#out + 1] = string.format("%d(type %d) %.1f m at %.1f %.1f %.1f", id, id % 256, dist(at, q), at.x, at.y, at.z)
  end
  return n .. " projectile(s): " .. table.concat(out, "; ")
end

function A.TestHand(on)
  for _, pp in pairs(M.pup or {}) do
    if valid(pp.so) then
      local good, err
      if on == false then
        good, err = pcall(pp.so.ModelToHands, pp.so, 0, "", "")
      else
        good, err = pcall(pp.so.ModelToHands, pp.so, 0, "exp_grenade", "")
      end
      return "ModelToHands on puppet " .. tostring(pp.id) .. ": " .. tostring(good) .. " " .. tostring(err)
    end
  end
  return "no puppet"
end

return A.version
