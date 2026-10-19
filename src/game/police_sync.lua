-- MafiaCoop police glue (police in co-op). Runs in the game's Lua state on GameMainThread after player_sync.lua,
-- npc_sync.lua and traffic_sync.lua, called once per script tick by src/game/police_sync.cpp (MafiaCoop.Police.Update).
-- Embedded into the dll; a copy at <dll dir>\police_sync.lua overrides it. docs/game-api.md 22, docs/protocol.md
-- "Police".
--
-- Every player's wanted state is his own game's: his game keeps one police zone for its player (search = blue, hunt =
-- red, a level, a centre = the area he has to leave to lose them), his fines and his arrests. This glue reads ours
-- (GetActiveZones / GetZoneInfo, IsSeenByPolice, the police events, the HumanMessages our player gets) and reports it;
-- the C++ side broadcasts it. Nothing of another player's state is copied into our game, except:
--   - host: a wanted guest near us is a target of OUR police (his puppet here): while he is hunted, our police near
--     his puppet attack it (Attack); police that saw his puppet are counted (SEE_ENEMY) and reported, the C++ side
--     tells him (PoliceChase). When he is clean again the police we set on him forget it (unless we are wanted
--     ourselves). No offence is announced from his puppet: the game has one police zone, its player's, and an
--     offence announced from a puppet that a cop witnesses becomes OUR player's zone (in-game test 2026-10-18: the
--     host was wanted at the puppet's spot 12 s after AnnouncePoliceOffence). The "A" commands are only logged.
--   - guest: while the host's police chase us, our game holds our wanted level at least at the chase's level
--     (ForceMinEscalationLevel), keeps our zone from decaying (PreventZonesDecay(true, follow): the game's decay timer
--     is not counted down; follow: a red zone's centre follows us, as when our own police see us) and starts a zone of
--     its own here when the host's police see us and our game has none, when our zone decays while they are on us, or
--     when it decayed away while they were still on us (ZoneCreate on our own player); released when the chase is
--     over (the decay goes on from where it stood: we still have to lose them). Never on the host: his zone is his.
--     Far from the host nothing is touched: our own police spawn and work as always (traffic_sync.lua switches our
--     police spawning off only near the host).
-- traffic_sync.lua asks P.ChaseNear(x, y, z) for the sirens of police copies when a police car's siren cannot be read.
--
-- Input (cmds, one line per command):
--   S active host localId          session running, we host, our player id
--   T id level hunt                host: our police take on guest `id`'s puppet (this tick's full list)
--   A id level hunt                host: announce an offence from guest `id`'s puppet (once)
--   F level                        guest: ForceMinEscalationLevel(level) (0 releases)
--   Z level hunt why               guest: ZoneCreate(our position, hunt and HUNT or SEARCH, level); why 0 seen, 1 our
--                                  zone decays, 2 it decayed (police_model.h ZoneWhy)
--   D hold follow refresh          guest: PreventZonesDecay(hold, follow) (0 0 releases; refresh 1: set again, quiet)
-- Output (events, one per line):
--   PS flags level x y z           our state (5 times a second; flags: police_model.h Flags)
--   PE NAME arg                    a police event of ours (police_model.h EventName)
--   PH x y z                       host: where we are
--   PP id x y z                    host: where guest `id`'s puppet is
--   PC id cops seers nearest       host: our police near guest `id`'s puppet (a target), how many saw it lately
--   ST text                        status for the overlay (once a second)
MafiaCoop = MafiaCoop or {}
local M = MafiaCoop
M.Police = M.Police or {}
local P = M.Police
P.version = 1

local READ_INTERVAL = 0.1   -- s between reads of our police state (was 0.2)
local SCAN_INTERVAL = 1.0   -- s between scans for our police near targeted puppets (host)
local POS_INTERVAL = 0.5    -- s between puppet / host positions sent to the C++ side (host)
local COP_RANGE = 90        -- m from a targeted puppet: our police humans that count (and may be set on it)
local SEE_KEEP = 5          -- s a cop that saw a puppet counts as seeing it
local ATTACK_AGAIN = 8      -- s before the same cop is told again
local MAX_ATTACKERS = 6     -- cops set on one puppet at once
local CHASE_NEAR = 250      -- m: ChaseNear (siren fallback for police copies)
local DECAY_KEEP = 60       -- s a BLUEZONE_DECAY counts (unless the zone escalates again)

-- flags (police_model.h)
local F_WANTED, F_HUNT, F_SEEN, F_DECAY, F_FINE, F_ARREST, F_OFF, F_WORLD = 1, 2, 4, 8, 16, 32, 64, 128

local EVENTS = { "PLAYER_ENTER_BLUEZONE", "PLAYER_EXIT_BLUEZONE", "PLAYER_ENTER_REDZONE", "PLAYER_EXIT_REDZONE",
  "BLUEZONE_DECAY", "POLICE_ZONE_DECAYED", "PLAYER_FINE", "POLICE_FINE_STARTED", "POLICE_FINE_FINISHED",
  "POLICE_ARREST_STARTED", "POLICE_ARREST_ESCAPED", "POLICE_ARREST_ARRESTED", "PLAYER_AREST", "PLAYER_DEATH",
  "POLICE_ACTIVITY_ENDED", "POLICE_WITNESS_STATE_STARTED", "POLICE_REDZONE_STARTED", "POLICE_BLUEZONE_STARTED" }
local HUMAN_EVENTS = { "OFFENDER", "OFFENDER_UPDATE", "RECOGNIZED_BY_POLICE", "POLICE_SEARCH", "ARRESTED" }

local function log(s) if mc_log then mc_log("police: " .. s) end end
local function valid(o) return o ~= nil and IsObjectValid(o) end
local errs = {}
local function try(what, f, ...)
  local good, a, b, c = pcall(f, ...)
  if not good then
    errs[what] = (errs[what] or 0) + 1
    if errs[what] <= 5 then log(what .. " failed: " .. tostring(a)) end
    return nil
  end
  return a, b, c
end
local function idOf(o)
  local good, g = pcall(o.GetGUID, o)
  if not good then return nil end
  return tonumber(tostring(g):match("(%d+)%s*$"))
end
local function guidNum(g) return tonumber(tostring(g):match("(%d+)%s*$")) end
local function num(v) return tonumber(v) or tonumber(tostring(v)) end
local function bit(v, b) return math.floor(v / b) % 2 == 1 end

P.evq = P.evq or {}          -- event lines queued by the callbacks, sent with the next update
P.targets = P.targets or {}  -- host: guest id (string) -> { level, hunt, seen = { copId = t }, told = { copId = t }, ... }
P.forced = P.forced or 0     -- guest: the level we hold (ForceMinEscalationLevel), 0 none
P.held = P.held or false     -- guest: our zone's decay is held (PreventZonesDecay(true, ...))
P.follow = P.follow or false -- guest: ... and a red zone follows us
P.st = P.st or { flags = 0, level = 0, x = 0, y = 0, z = 0 }

local function queue(name, arg)
  if #P.evq < 64 then P.evq[#P.evq + 1] = "PE " .. name .. " " .. tostring(math.floor(arg or 0)) end
end

-- ---- watching our police ---------------------------------------------------------------------------------------------
function P.OnPoliceEvent(name, e)
  P.nEvents = (P.nEvents or 0) + 1
  if P.nEvents <= 20 then log("police event " .. name .. " (" .. tostring(e) .. ")") end
  if name == "BLUEZONE_DECAY" or name == "POLICE_ZONE_DECAYED" then
    P.decayT = M.now
  elseif name == "PLAYER_ENTER_BLUEZONE" or name == "PLAYER_ENTER_REDZONE" or name == "POLICE_REDZONE_STARTED"
      or name == "POLICE_BLUEZONE_STARTED" then
    P.decayT = nil
  elseif name == "POLICE_FINE_STARTED" then
    P.fining = true
  elseif name == "POLICE_FINE_FINISHED" or name == "PLAYER_FINE" then
    P.fining = false
  elseif name == "POLICE_ARREST_STARTED" then
    P.arresting = true
  elseif name == "POLICE_ARREST_ESCAPED" or name == "POLICE_ARREST_ARRESTED" or name == "PLAYER_AREST" then
    P.arresting = false
  elseif name == "POLICE_ACTIVITY_ENDED" or name == "PLAYER_DEATH" then
    P.fining, P.arresting = false, false
  end
  queue(name, 0)
end

-- HumanMessages of every human: ours are reported; one on a puppet (host) is logged (did AnnouncePoliceOffence
-- make it an offender?).
function P.OnHumanEvent(name, m)
  local r = guidNum(m:GetReceiverGUID())
  if not r then return end
  if r == P.localGuid then
    local arg = 0
    if name == "ARRESTED" then
      local good, fin = pcall(m.GetArrestFinished, m)
      arg = (good and fin) and 1 or 0
      if good and fin then P.arresting = false end
    end
    queue(name, arg)
  elseif P.isHost then
    for id, pp in pairs(M.pup or {}) do
      if valid(pp.so) and idOf(pp.so) == r then
        P.nPupEvents = (P.nPupEvents or 0) + 1
        if P.nPupEvents <= 30 then log("guest " .. id .. "'s puppet got " .. name .. " on our game") end
      end
    end
  end
end

-- SEE_ENEMY of every human (host, while a guest is a target): our cops that saw a targeted puppet.
function P.OnSee(m)
  if not next(P.targets) then return end
  local good, eg = pcall(m.GetEnemyGUID, m)
  if not good then return end
  local enemy, r = guidNum(eg), guidNum(m:GetReceiverGUID())
  if not enemy or not r then return end
  for _, t in pairs(P.targets) do
    if t.pupId == enemy then t.seen[r] = M.now end
  end
end

local function worldKey() return tostring(game.game:GetLoadedSaveId()) .. "|" .. tostring(M.inWorldAt) end

function P.Unwatch()
  if not P.reg then return end
  for _, id in ipairs(P.reg.ids) do pcall(game.police.UnregisterForPoliceEvent, game.police, id) end
  for _, id in ipairs(P.reg.blanket) do pcall(game.game.UnregisterBlanketEvent, game.game, id) end
  P.reg = nil
end

-- One subscription per police event (the event tells which by its callback) and per HumanMessage, once per world.
function P.Watch(p)
  local key = worldKey()
  if P.reg and P.reg.key == key then return end
  P.Unwatch()
  local reg = { key = key, ids = {}, blanket = {} }
  P.reg = reg
  P.localGuid = idOf(p)
  P.decayT, P.fining, P.arresting = nil, false, false
  local pet = enums and enums.PoliceEventType
  local bad = {}
  for _, name in ipairs(EVENTS) do
    local flag = pet and pet[name]
    local good, id = false, "no enums.PoliceEventType." .. name
    if flag ~= nil then
      local fn = function(e) local ok, err = pcall(P.OnPoliceEvent, name, e) if not ok then log("event: " .. tostring(err)) end end
      good, id = pcall(game.police.RegisterForPoliceEvent, game.police, reg, { flag }, fn)
      if not (good and num(id) and num(id) >= 0) and num(flag) then  -- an enum object: try its number
        good, id = pcall(game.police.RegisterForPoliceEvent, game.police, reg, { num(flag) }, fn)
      end
    end
    if good and num(id) and num(id) >= 0 then reg.ids[#reg.ids + 1] = id else bad[#bad + 1] = name .. " (" .. tostring(id) .. ")" end
  end
  local hm = enums and enums.HumanMessages
  local subs = {}
  for _, name in ipairs(HUMAN_EVENTS) do
    subs[#subs + 1] = { name, function(ctx, m) local ok, err = pcall(P.OnHumanEvent, name, m) if not ok then log("human event: " .. tostring(err)) end end }
  end
  subs[#subs + 1] = { "SEE_ENEMY", function(ctx, m) pcall(P.OnSee, m) end }
  for _, s in ipairs(subs) do
    local msg = hm and hm[s[1]]
    local good, id = false, "no HumanMessages." .. s[1]
    if msg then good, id = pcall(game.game.RegisterBlanketEvent, game.game, {}, s[2], enums.EventType.HUMAN, msg) end
    if good and num(id) and num(id) >= 0 then reg.blanket[#reg.blanket + 1] = id else bad[#bad + 1] = s[1] .. " (" .. tostring(id) .. ")" end
  end
  log(string.format("watching our police: %d police events, %d human messages%s", #reg.ids, #reg.blanket,
    #bad > 0 and (", not watched: " .. table.concat(bad, ", ")) or ""))
end

-- Our state: GetActiveZones() gives the zone ids (our game keeps one: 0), GetZoneInfo(id) its centre, level and type
-- (docs/game-api.md 22).
function P.Read(p)
  local pol = game.police
  local flags, level, x, y, z = F_WORLD, 0, 0, 0, 0
  local res = { pcall(pol.GetActiveZones, pol) }
  if not res[1] then
    try("GetActiveZones", error, res[2])
  elseif res[2] ~= nil then
    local good, pos, lvl, zt = pcall(pol.GetZoneInfo, pol, res[2])
    if good and pos and pos.x then
      flags = flags + F_WANTED
      level = math.max(0, math.floor(num(lvl) or 0))
      local hunt = num(enums and enums.PoliceZoneType and enums.PoliceZoneType.HUNT) or 1
      if num(zt) == hunt then flags = flags + F_HUNT end
      x, y, z = pos.x, pos.y, pos.z
      local sig = tostring(res[2]) .. "/" .. tostring(lvl) .. "/" .. tostring(zt)
      if sig ~= P.zoneSig then
        P.zoneSig = sig
        P.nZoneLogs = (P.nZoneLogs or 0) + 1
        if P.nZoneLogs <= 40 then
          log(string.format("zone %s: GetZoneInfo -> %.1f %.1f %.1f, level %s, type %s (%d zone ids)", tostring(res[2]),
            x, y, z, tostring(lvl), tostring(zt), #res - 1))
        end
      end
    elseif not good then
      try("GetZoneInfo", error, pos)
    end
  else
    P.zoneSig = nil
  end
  local good, seen = pcall(p.IsSeenByPolice, p)
  if good and seen then flags = flags + F_SEEN end
  if bit(flags, F_WANTED) and P.decayT and M.now - P.decayT < DECAY_KEEP then flags = flags + F_DECAY end
  if not bit(flags, F_WANTED) then P.decayT = nil end
  if P.fining then flags = flags + F_FINE end
  if P.arresting then flags = flags + F_ARREST end
  local good2, off = pcall(pol.AreOffencesDisabled, pol)
  if good2 and off then flags = flags + F_OFF end
  P.st = { flags = flags, level = level, x = x, y = y, z = z }
  return P.st
end

-- ---- host: our police and the guests' puppets ------------------------------------------------------------------------
local function pupPos(pp)
  if pp.veh and pp.cx then return pp.cx, pp.cy, pp.cz end
  if valid(pp.so) then
    local good, q = pcall(pp.so.GetPos, pp.so)
    if good and q then return q.x, q.y, q.z end
  end
  if pp.tx then return pp.tx, pp.ty, pp.tz end
  return nil
end

-- An offence would be announced from guest `id`'s puppet here. Not done: AnnouncePoliceOffence on a puppet makes OUR
-- player wanted once a cop witnesses it (the game's one police zone is its player's; see the header). Logged only.
function P.Announce(id, level, hunt)
  P.nAnnounce = (P.nAnnounce or 0) + 1
  log(string.format("guest %s is wanted near us (level %d, %s): no offence announced from his puppet (it would make "
    .. "us wanted); our police %s", id, level, hunt and "hunt" or "search",
    hunt and "near his puppet attack it" or "leave him to his own game while he is only searched"))
end

local function isCop(h)
  local good, a = pcall(h.GetAIType, h)
  if not (good and num(a) == 4) then return false end
  local good2, dead = pcall(h.IsDeath, h)
  return good2 and not dead
end

-- Police near each target's puppet; set some of them on it when he is hunted at a high level.
function P.Scan(ev)
  local cops = {}
  for s in string.gmatch(mc_entity_ids(8), "%d+") do
    local e = mc_entity(tonumber(s))
    if e and e.GetAIType and isCop(e) then cops[#cops + 1] = { id = tonumber(s), h = e, pos = e:GetPos() } end
  end
  for id, t in pairs(P.targets) do
    local pp = M.pup and M.pup[id]
    local px, py, pz = nil, nil, nil
    if pp then px, py, pz = pupPos(pp) end
    local n, seers, nearest, told = 0, 0, 0, 0
    if px and valid(pp.so) then
      t.pupId = idOf(pp.so)
      local near = {}
      for _, c in ipairs(cops) do
        local d = math.sqrt((c.pos.x - px) ^ 2 + (c.pos.y - py) ^ 2 + (c.pos.z - pz) ^ 2)
        if d <= COP_RANGE then near[#near + 1] = { c = c, d = d } end
      end
      table.sort(near, function(a, b) return a.d < b.d end)
      for i, nc in ipairs(near) do
        n = n + 1
        if i == 1 then nearest = nc.d end
        if t.seen[nc.c.id] and M.now - t.seen[nc.c.id] < SEE_KEEP then seers = seers + 1 end
        if t.hunt and i <= MAX_ATTACKERS then
          if not t.told[nc.c.id] or M.now - t.told[nc.c.id] >= ATTACK_AGAIN then
            local good, err = pcall(nc.c.h.Attack, nc.c.h, pp.so)
            t.told[nc.c.id] = M.now
            t.attacks = (t.attacks or 0) + 1
            if t.attacks <= 10 or t.attacks % 50 == 0 then
              log(string.format("our cop %d (%.0f m) set on guest %s's puppet: %s (#%d)", nc.c.id, nc.d, id,
                good and "ok" or ("FAILED " .. tostring(err)), t.attacks))
            end
          end
          told = told + 1
        end
      end
    end
    t.cops, t.seers, t.nearest = n, seers, nearest
    ev[#ev + 1] = string.format("PC %s %d %d %.1f", id, n, seers, nearest)
    if mc_trace then
      mc_trace("POT", string.format("id=%s lvl=%d hunt=%d cops=%d seers=%d near=%.1f set=%d attacks=%d", id, t.level,
        t.hunt and 1 or 0, n, seers, nearest, told, t.attacks or 0))
    end
  end
end

-- A target ended: the cops we set on him forget (all their targets: not while we are wanted ourselves).
function P.Release(id, t, why)
  local n = 0
  if not bit(P.st.flags or 0, F_WANTED) then
    for copId, _ in pairs(t.told) do
      local e = mc_entity(copId)
      if e and e.Forget then
        pcall(e.Forget, e)
        n = n + 1
      end
    end
  end
  log(string.format("guest %s is no target of our police any more (%s): %d cops told to forget%s", id, why, n,
    (next(t.told) and n == 0) and " (none: we are wanted ourselves)" or ""))
end

function P.HostTick(p, ev, tl, ann)
  local now = M.now
  -- targets: this tick's list
  for id, f in pairs(tl) do
    local t = P.targets[id]
    if not t then
      t = { seen = {}, told = {}, since = now }
      P.targets[id] = t
      log(string.format("guest %s is wanted near us: our police take him on (level %d, %s)", id, f.level,
        f.hunt and "hunt" or "search"))
    end
    t.level, t.hunt = f.level, f.hunt
  end
  for id, t in pairs(P.targets) do
    if not tl[id] then
      P.Release(id, t, "clean, far or gone")
      P.targets[id] = nil
    end
  end
  for _, a in ipairs(ann) do P.Announce(a[1], a[2], a[3]) end
  if now - (P.posT or -10) >= POS_INTERVAL then
    P.posT = now
    local q = p:GetPos()
    ev[#ev + 1] = string.format("PH %.2f %.2f %.2f", q.x, q.y, q.z)
    for id, pp in pairs(M.pup or {}) do
      if id ~= "0" then
        local x, y, z = pupPos(pp)
        if x then ev[#ev + 1] = string.format("PP %s %.2f %.2f %.2f", id, x, y, z) end
      end
    end
  end
  if next(P.targets) and mc_entity_ids and mc_entity and now - (P.scanT or -10) >= SCAN_INTERVAL then
    P.scanT = now
    local good, err = pcall(P.Scan, ev)
    if not good then log("scan error: " .. tostring(err)) end
  end
end

-- traffic_sync.lua (host): is a hunted player (we, or a guest our police chase) near this point?
function P.ChaseNear(x, y, z)
  if not P.active then return false end
  local r2 = CHASE_NEAR * CHASE_NEAR
  local s = P.st
  if bit(s.flags or 0, F_HUNT) and P.me and (P.me.x - x) ^ 2 + (P.me.y - y) ^ 2 <= r2 then return true end
  for id, t in pairs(P.targets) do
    local pp = t.hunt and M.pup and M.pup[id]
    if pp then
      local px, py = pupPos(pp)
      if px and (px - x) ^ 2 + (py - y) ^ 2 <= r2 then return true end
    end
  end
  return false
end

-- ---- guest: the host's police chase us -------------------------------------------------------------------------------
function P.Force(level, why)
  if level == P.forced then return end
  local good, err = pcall(game.police.ForceMinEscalationLevel, game.police, level)
  log(string.format("ForceMinEscalationLevel(%d) %s (%s)", level, good and "ok" or ("FAILED " .. tostring(err)), why))
  if good then P.forced = level end
end

-- The decay of our zone (docs/re-notes.md 10, PreventZonesDecay): hold = the game does not count our zone's decay
-- timer down (and does not mark it decaying); follow = a red zone's centre is put where we are (the game does this
-- itself while our own police see us). Module-wide, so it is released (false, false) when the chase is over and when
-- the session ends. UNVERIFIED in game; a mission that holds the decay itself is released too.
function P.Hold(hold, follow, refresh, why)
  local good, err = pcall(game.police.PreventZonesDecay, game.police, hold and true or false, follow and true or false)
  local changed = hold ~= P.held or follow ~= P.follow
  if good then P.held, P.follow = hold, follow end
  if changed or not good then
    P.nHoldLogs = (P.nHoldLogs or 0) + 1
    if not good or P.nHoldLogs <= 60 then
      log(string.format("PreventZonesDecay(%s, %s) %s (%s)", tostring(hold), tostring(follow),
        good and "ok" or ("FAILED " .. tostring(err)), why))
    end
  end
  if refresh and not good then try("PreventZonesDecay", error, err) end
end

local ZONE_WHY = { [0] = "the host's police see us", [1] = "our zone decays while the host's police are on us",
  [2] = "our zone decayed while the host's police are still on us" }

function P.Zone(p, level, hunt, why)
  local q = p:GetPos()
  local zt = hunt and (enums.PoliceZoneType and enums.PoliceZoneType.HUNT or 1)
    or (enums.PoliceZoneType and enums.PoliceZoneType.SEARCH or 0)
  local good, err = pcall(game.police.ZoneCreate, game.police, q, zt, level)
  P.nZones = (P.nZones or 0) + 1
  log(string.format("ZoneCreate(here %.1f %.1f %.1f, %s, %d) %s: %s", q.x, q.y, q.z,
    hunt and "HUNT" or "SEARCH", level, good and "ok" or ("FAILED " .. tostring(err)), ZONE_WHY[why or 0] or "?"))
end

-- ---- update ----------------------------------------------------------------------------------------------------------
function P.Update(dt, cmds)
  local ev = {}
  local isActive, host = false, false
  local tl, ann, force, zones, hold = {}, {}, nil, {}, nil
  for line in string.gmatch(cmds or "", "[^\n]+") do
    local f = {}
    for w in string.gmatch(line, "%S+") do f[#f + 1] = w end
    local c = f[1]
    if c == "S" then
      isActive, host = f[2] == "1", f[3] == "1"
    elseif c == "T" then
      tl[f[2]] = { level = tonumber(f[3]) or 1, hunt = f[4] == "1" }
    elseif c == "A" then
      ann[#ann + 1] = { f[2], tonumber(f[3]) or 1, f[4] == "1" }
    elseif c == "F" then
      force = tonumber(f[2]) or 0
    elseif c == "Z" then
      zones[#zones + 1] = { tonumber(f[2]) or 1, f[3] == "1", tonumber(f[4]) or 0 }
    elseif c == "D" then
      hold = { f[2] == "1", f[3] == "1", f[4] == "1" }
    end
  end
  P.active, P.isHost = isActive, host
  local p = M.Player and M.Player() or nil
  if not isActive then
    if P.forced ~= 0 then P.Force(0, "the session ended") end
    if P.held or P.follow then P.Hold(false, false, false, "the session ended") end
    for id, t in pairs(P.targets) do P.Release(id, t, "the session ended") end
    P.targets = {}
    P.evq = {}
    return ""
  end
  if force ~= nil then P.Force(force, force > 0 and "the host's police chase us" or "the host's police stopped") end
  if hold ~= nil and not host then
    P.Hold(hold[1], hold[2], hold[3], hold[1] and (hold[2] and "the host's police are on us and see us / are close"
      or "the host's police are on us") or "the host's police are not on us any more")
  elseif host and (P.held or P.follow) then
    P.Hold(false, false, false, "we are the host now: our zone is our own")
  end
  if not p then
    P.evq = {}
    if next(P.targets) then
      for id, t in pairs(P.targets) do P.Release(id, t, "we are not in a world") end
      P.targets = {}
    end
    return table.concat(ev, "\n")
  end
  local good, err = pcall(P.Watch, p)
  if not good then log("watch error: " .. tostring(err)) end
  if M.now - (P.readT or -10) >= READ_INTERVAL then
    P.readT = M.now
    local good2, s = pcall(P.Read, p)
    if good2 and s then
      ev[#ev + 1] = string.format("PS %d %d %.2f %.2f %.2f", s.flags, s.level, s.x, s.y, s.z)
      local q = p:GetPos()
      P.me = { x = q.x, y = q.y, z = q.z }
      if mc_trace and (s.flags ~= (P.trF or -1) or s.level ~= P.trL or (bit(s.flags, F_WANTED) and M.now - (P.trT or -10) >= 1)) then
        P.trF, P.trL, P.trT = s.flags, s.level, M.now
        mc_trace("POL", string.format("f=%d w=%d hunt=%d lvl=%d seen=%d decay=%d fine=%d arrest=%d off=%d zone=%.1f,%.1f,%.1f p=%.1f,%.1f,%.1f held=%d follow=%d forced=%d",
          s.flags, bit(s.flags, F_WANTED) and 1 or 0, bit(s.flags, F_HUNT) and 1 or 0, s.level,
          bit(s.flags, F_SEEN) and 1 or 0, bit(s.flags, F_DECAY) and 1 or 0, bit(s.flags, F_FINE) and 1 or 0,
          bit(s.flags, F_ARREST) and 1 or 0, bit(s.flags, F_OFF) and 1 or 0, s.x, s.y, s.z, q.x, q.y, q.z,
          P.held and 1 or 0, P.follow and 1 or 0, P.forced or 0))
      end
    elseif not good2 then
      log("read error: " .. tostring(s))
    end
  end
  for _, l in ipairs(P.evq) do ev[#ev + 1] = l end
  P.evq = {}
  if host then
    local good3, err3 = pcall(P.HostTick, p, ev, tl, ann)
    if not good3 then log("host error: " .. tostring(err3)) end
  else
    if next(P.targets) then
      for id, t in pairs(P.targets) do P.Release(id, t, "we are a guest now") end
      P.targets = {}
    end
    for _, z in ipairs(zones) do pcall(P.Zone, p, z[1], z[2], z[3]) end
  end
  if M.now - (P.stT or -10) >= 1 then
    P.stT = M.now
    local n = 0
    for _, _ in pairs(P.targets) do n = n + 1 end
    ev[#ev + 1] = string.format("ST lua: %d events, %d announced, %d targets%s%s%s", P.nEvents or 0, P.nAnnounce or 0,
      n, P.forced ~= 0 and (", level held " .. P.forced) or "",
      P.held and (P.follow and ", decay held (zone follows us)" or ", decay held") or "",
      (P.nZones or 0) > 0 and (", " .. P.nZones .. " zones started") or "")
  end
  return table.concat(ev, "\n")
end

return P.version
