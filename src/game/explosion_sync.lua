-- MafiaCoop explosion glue (0.16: grenades, molotovs, dynamite, exploding barrels and cars on every game). Runs in the
-- game's Lua state on GameMainThread after the other glue files, called once per script tick by
-- src/game/explosion_sync.cpp (MafiaCoop.Boom.Update). Embedded into the dll; a copy at <dll dir>\explosion_sync.lua
-- overrides it. docs/game-api.md 23, docs/protocol.md "Explosions".
--
-- Damage is decided by ONE game, the picture is shown on all (src/game/explosion_model.h):
--   - Here, the game tells us about explosions with HEAR_SOUND messages (every human that hears one gets one, with
--     the sound's position, type and owner: EXPLOSION 11, BURNING_FIRE 12, EXPLOSIVE_BOUNCE 10), our player's throws
--     (WEAPON_THROWN, already counted by player_sync.lua as action kind 5), PLAYER_GRENADE_EXPLOSION to our player and
--     our shots. The C++ detector (mc_boom_sound / mc_boom_throw / mc_boom_grenade / mc_boom_shot) decides which ones
--     are ours (or, on the host, our NPCs') and sends them (Explosion 0x0240).
--   - The others' explosions arrive as "E" commands: shown with game.shotmgr:CreateScriptExplosion(nil, pos, weapon,
--     0 damage, no stun, ranges 0, 1.5, fire) (exe 0x239D8F0, docs/game-api.md 23; UNVERIFIED that it hurts nobody),
--     a remote zone marked first (mc_boom_mark), so that whatever damage the picture still does here is undone.
--   - Every explosion / fire DAMAGE goes through B.OnDamage first (player_sync.lua M.OnDamageEvent): mc_boom_judge
--     says keep, undo (a picture of another game's explosion hurt someone here) or ours (our explosion, its damage
--     names no attacker: counted as our player's, so the usual hit paths send it on).
--   - Cars: a car that blows up here (CAR_HAVE_PROBLEM with GetCarProblem() == EXPLODE, UNVERIFIED) and that we are
--     authoritative for (we drive it, or our own explosion did it; on the host a shared traffic car) is sent
--     (CarExploded 0x0241, "CS" events); the others' copies of it are set exploded (SetExploded: the wreck without a
--     blast, exe 0x2540380 with -1, UNVERIFIED) with a picture of the blast.
--
-- Input (cmds, one line per command):
--   S active host localId                          session running, we host, our player id
--   E from seq kind flags x y z weapon npc object  player `from`'s explosion (kind: 0 unknown 1 grenade 2 molotov
--                                                  3 dynamite 4 object 5 car; flags: 1 fire, 2 one of his NPCs threw
--                                                  it, npc = its key there; weapon / object "-" = none)
--   C from what key x y z                          player `from`'s car blew up (what 0: shared car key "o:g";
--                                                  1: host traffic car, key = its traffic net id)
-- Output (events, one per line):
--   CS what key x y z                              a car we are authoritative for blew up (what 1: key = its
--                                                  entity id here, the C++ side turns it into the traffic net id)
--   ST text                                        status for the overlay (once a second)
-- Sync trace: "XV" (a picture shown), "XCV" (a car set exploded), "XU" (an undo carried out), docs/sync-trace.md.
MafiaCoop = MafiaCoop or {}
local M = MafiaCoop
M.Boom = M.Boom or {}
local B = M.Boom
B.version = 1

local FAR = 400            -- m: explosions farther than this from us are not shown (nobody would see them)
local HEAR_DEDUP = 0.5     -- s: the same sound (type, metre cell) heard by many humans counts once
local SNAP_RADIUS = 20     -- m around a picture: NPC health remembered (an undo puts it back)
local SNAP_KEEP = 40       -- s a remembered health is kept
local CAR_RADIUS = 6       -- m: a car whose explosion our own blast caused stands this close to it (mc_boom_own)
local SHOW_IGNORE = 1.5    -- the picture's 9th argument (the game's own nodes pass 1.5 when they have none)

local function log(s) if mc_log then mc_log("boom: " .. s) end end
local function trace(kind, text) if mc_trace then mc_trace(kind, text) end end
local function valid(o) return o ~= nil and IsObjectValid(o) end
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
local function idOf(g) return tonumber(tostring(g or ""):match("(%d+)%s*$")) or 0 end
local function guidStr(o)
  if not valid(o) then return nil end
  local good, g = pcall(o.GetGUID, o)
  return good and tostring(g) or nil
end
local function tok(s) s = tostring(s or ""); s = s:gsub("[^%w_%-%.:/]", ""); return s ~= "" and s or "-" end
local function f3(x, y, z) return string.format("%.2f,%.2f,%.2f", x, y, z) end

-- entity type from the low byte of an entity id (docs/game-api.md 15: 8 human, 9 player, 10 car; 12 crash object as
-- in the ActorMessages entity types, UNVERIFIED for ids)
local TYPE_CAR, TYPE_CRASH = 10, 12

B.heard = B.heard or {}
B.exploded = B.exploded or {}   -- entity id -> true: this car blew up here (sent or set exploded)
B.hp = B.hp or {}               -- NPC id -> { hp, t }: its health before a picture went off near it
B.restore = B.restore or {}     -- NPC id -> true: give it its remembered health back (next tick)
B.resnap = B.resnap or {}       -- NPC id -> true: its health changed for real, remember it anew
B.stats = B.stats or { shown = 0, failed = 0, far = 0, npcSkip = 0, cars = 0, undoLocal = 0, undoNpc = 0, lost = 0,
  sounds = 0, carsSent = 0, hearAll = 0 }
B.events = {}

-- ---------------------------------------------------------------------------------------------------------------
-- Who is who (GUID strings as tostring(C_EntityGuid) prints them)
-- ---------------------------------------------------------------------------------------------------------------
local function N() return M.Npc end

-- the damage message's attacker, for mc_boom_judge: us (our player or the car he sits in), npc (a real NPC of this
-- game), puppet, copy (a spawned NPC copy), none (no attacker), other
function B.Classify(g)
  local id = idOf(g)
  if id == 0 then return "none", 0 end
  if g == M.localGuid then return "us", id end
  local p = M.Player and M.Player()
  if p then
    if g == guidStr(p) then return "us", id end
    local good, car = pcall(p.GetOwner, p)
    if good and car and g == guidStr(car) then return "us", id end
  end
  for _, pp in pairs(M.pup or {}) do
    if pp.guid and pp.guid == g then return "puppet", id end
  end
  local n = N()
  if n and n.copyIds and n.copyIds[id] then return "copy", id end
  if n and n.ents and n.ents[id] then return "npc", id end
  return "other", id
end

-- the damage message's victim: local, puppet, copy or npc, its id and the human (nil for puppets: never touched)
local function victimOf(g)
  local id = idOf(g)
  if g == M.localGuid then return "local", id, M.Player and M.Player() end
  for _, pp in pairs(M.pup or {}) do
    if pp.guid and pp.guid == g then return "puppet", id, pp.so end
  end
  local n = N()
  if n and n.copyIds and n.copyIds[id] then return "copy", id, nil end
  local e = n and n.ents and n.ents[id]
  return "npc", id, e and e.h or (mc_entity and mc_entity(id))
end

-- Does this game show the sender's NPC `key` with a spawned copy (no fight of its own here)? Then its explosions are
-- shown here; an NPC this game runs itself (paired: its own copy of the mission NPC) throws its own grenades here.
function B.ShowsNpc(from, key)
  local n = N()
  if not n or not n.host then return false end
  if not M.isHost then
    -- another guest's own NPC is known here by the host's relay key for it (npc_sync.lua N.RelayKey, several guests);
    -- the host's NPCs (from "0") by their own key
    local s
    if tostring(from) ~= "0" and n.RelayKey then
      local rk = n.RelayKey(from, key)
      s = rk and n.host[rk]
    end
    s = s or n.host[key]
    return s ~= nil and s.copy ~= nil and s.copy ~= false
  end
  for mid, s in pairs(n.host) do
    if tostring(s.from) == tostring(from) and tonumber(s.gkey) == key then return n.copies and n.copies[mid] ~= nil end
  end
  return false
end

-- ---------------------------------------------------------------------------------------------------------------
-- The game's messages (per loaded world: blanket subscriptions, re-registered after a load)
-- ---------------------------------------------------------------------------------------------------------------
B.subs = B.subs or {}

function B.Unwatch()
  for _, id in ipairs(B.subs.ids or {}) do pcall(game.game.UnregisterBlanketEvent, game.game, id) end
  B.subs = {}
end

local function sub(ids, et, msg, name, fn)
  if not (et and msg) then
    log("could not watch " .. name .. " (no such enum in this build)")
    return
  end
  local good, id = pcall(game.game.RegisterBlanketEvent, game.game, {}, function(ctx, m)
    local ok, err = pcall(fn, m)
    if not ok then
      errs[name] = (errs[name] or 0) + 1
      if errs[name] <= 5 then log(name .. " event error: " .. tostring(err)) end
    end
  end, et, msg)
  if good and tonumber(id) and tonumber(id) >= 0 then ids[#ids + 1] = id else log("could not watch " .. name .. " (" .. tostring(id) .. ")") end
end

function B.Watch()
  local key = tostring(game.game:GetLoadedSaveId()) .. "|" .. tostring(M.inWorldAt)
  if B.subs.key == key then return end
  B.Unwatch()
  local ids = {}
  local hm, cm = enums.HumanMessages or {}, enums.CarMessages or {}
  sub(ids, enums.EventType.HUMAN, hm.HEAR_SOUND, "HEAR_SOUND", B.OnHear)
  sub(ids, enums.EventType.HUMAN, hm.PLAYER_GRENADE_EXPLOSION, "PLAYER_GRENADE_EXPLOSION", B.OnGrenade)
  sub(ids, enums.EventType.CAR, cm.CAR_HAVE_PROBLEM, "CAR_HAVE_PROBLEM", B.OnCarProblem)
  B.subs = { key = key, ids = ids }
  B.heard, B.exploded, B.hp, B.restore, B.resnap = {}, {}, {}, {}, {}
  log("watching sounds, our grenades and car problems (" .. #ids .. " subscriptions)")
end

-- HEAR_SOUND (C_HearSoundMessage: GetSoundOwnerGUID, GetSoundPos, GetSoundType, GetSecondEntityGUID): only the
-- explosive ones, once per sound.
function B.OnHear(m)
  B.stats.hearAll = (B.stats.hearAll or 0) + 1  -- every HEAR_SOUND callback (the cost of this subscription, in the status)
  local st = tonumber(m:GetSoundType())
  if st ~= 10 and st ~= 11 and st ~= 12 then return end
  local pos = m:GetSoundPos()
  local x, y, z = pos.x, pos.y, pos.z
  local k = st .. ":" .. math.floor(x) .. ":" .. math.floor(y) .. ":" .. math.floor(z)
  local seen = B.heard[k]
  if seen and M.now - seen < HEAR_DEDUP then return end
  B.heard[k] = M.now
  B.stats.sounds = B.stats.sounds + 1
  local og = tostring(m:GetSoundOwnerGUID())
  local cls, oid = B.Classify(og)
  local owner, npc = "unknown", 0
  if cls == "us" then
    owner = "us"
  elseif cls == "npc" then
    -- the host's NPCs are the host's to tell; on a guest only the NPCs that are his own (npc_sync.lua N.owned)
    local n = N()
    if M.isHost or (n and n.owned and n.owned[oid]) then owner, npc = "npc", oid else owner = "other" end
  elseif cls ~= "none" then
    owner = "other"
  end
  -- the crash object that went off (a barrel): the sound's second entity or its owner, when that is one
  local object = ""
  local good, sg = pcall(m.GetSecondEntityGUID, m)
  for _, cand in ipairs({ good and tostring(sg) or "", og }) do
    local id = idOf(cand)
    if id ~= 0 and id % 256 == TYPE_CRASH and mc_entity then
      local e = mc_entity(id)
      local g2, name = pcall(function() return e:GetName() end)
      if g2 and name then object = tok(name) break end
    end
  end
  if object == "-" then object = "" end
  if st == 10 and owner == "us" then  -- our grenade bouncing: points of its flight (not shown elsewhere)
    trace("XB", string.format("p=%s", f3(x, y, z)))
  end
  if st == 11 and B.stats.sounds <= 40 then
    log(string.format("explosion heard at %.1f %.1f %.1f, owner %s (%s), second %s%s", x, y, z, og, cls,
      good and tostring(sg) or "?", object ~= "" and (", object " .. object) or ""))
  end
  mc_boom_sound(st, x, y, z, owner, npc, object)
end

function B.OnGrenade(m)
  if tostring(m:GetReceiverGUID()) ~= M.localGuid then return end
  mc_boom_grenade()
  B.grenadeMsgs = (B.grenadeMsgs or 0) + 1
  if B.grenadeMsgs <= 20 then log("PLAYER_GRENADE_EXPLOSION for us (sender " .. tostring(m:GetSenderGUID()) .. ")") end
end

-- A car problem (C_* message with GetCarProblem(), common_vehicle_events.lua OnCarProblem): EXPLODE = 1.
function B.OnCarProblem(m)
  local good, prob = pcall(m.GetCarProblem, m)
  if not good then
    if not B.noProblem then B.noProblem = true; log("CAR_HAVE_PROBLEM without GetCarProblem: " .. tostring(prob)) end
    return
  end
  local explode = (enums.CarProblem and enums.CarProblem.EXPLODE) or 1
  if tonumber(prob) ~= explode then return end
  local id = idOf(tostring(m:GetReceiverGUID()))
  if id == 0 or B.exploded[id] then return end
  B.exploded[id] = true
  local car = mc_entity and mc_entity(id)
  if not valid(car) then return end
  local p = car:GetPos()
  local key, c = nil, nil
  if M.KeyOf then key, c = M.KeyOf(car) end
  if key then
    local ours = c.driver == "local" or (not c.driver and mc_boom_own(p.x, p.y, p.z))
    log(string.format("shared car %s blew up here at %.1f %.1f %.1f (driver %s)%s", key, p.x, p.y, p.z,
      tostring(c.driver or "none"), ours and ": sent to the others" or ": not ours to tell"))
    if ours then
      mc_boom_own_blast(p.x, p.y, p.z)
      B.events[#B.events + 1] = string.format("CS 0 %s %.3f %.3f %.3f", key, p.x, p.y, p.z)
      B.stats.carsSent = B.stats.carsSent + 1
    end
    return
  end
  local T = M.Traffic
  local traffic = M.isHost and T and T.list and T.list[id]
  if traffic then  -- one of our traffic cars (traffic_sync.lua): the guests near it have copies (the C++ side knows
                   -- which are shared; the rest are dropped there)
    mc_boom_own_blast(p.x, p.y, p.z)
    B.events[#B.events + 1] = string.format("CS 1 %d %.3f %.3f %.3f", id, p.x, p.y, p.z)
    B.stats.carsSent = B.stats.carsSent + 1
  end
  log(string.format("car %d blew up here at %.1f %.1f %.1f%s", id, p.x, p.y, p.z,
    traffic and " (our traffic: sent)" or " (this game's own car: nobody else's to show)"))
end

-- ---------------------------------------------------------------------------------------------------------------
-- The picture of another game's explosion
-- ---------------------------------------------------------------------------------------------------------------
-- NPCs near it: their health now (an undo puts it back)
local function snap(x, y, z)
  local n = N()
  if not (n and n.ents) then return end
  for id, e in pairs(n.ents) do
    if valid(e.h) and not (n.copyIds and n.copyIds[id]) then
      local good, q = pcall(e.h.GetPos, e.h)
      if good and q and (q.x - x) ^ 2 + (q.y - y) ^ 2 + (q.z - z) ^ 2 <= SNAP_RADIUS * SNAP_RADIUS then
        B.hp[id] = { hp = tonumber(e.h.health) or 0, t = M.now }
      end
    end
  end
end

-- Shows a visual-only explosion at x y z. Returns ok, how/err.
function B.Picture(x, y, z, weapon, fire, from, seq, kind)
  mc_boom_mark(x, y, z, fire and 1 or 0, tonumber(from) or 255, tonumber(seq) or 0, tonumber(kind) or 0)
  snap(x, y, z)
  local w = (weapon and weapon ~= "-" and weapon ~= "") and weapon or "exp_grenade"
  -- (positionEntity, positionVector, weapon, damage, stun, fullDamageRange, damageRange, ignoreColl, createFire):
  -- argument order from the game's own CreateScriptExplosion node (game.lua; docs/game-api.md 23)
  local good, err = pcall(game.shotmgr.CreateScriptExplosion, game.shotmgr, nil, V(x, y, z), w, 0, false, 0, 0,
    SHOW_IGNORE, fire and true or false)
  if good then return true, "script" end
  return false, tostring(err)
end

function B.OnRemote(f)
  local from, seq, kind, flags = f[2], tonumber(f[3]) or 0, tonumber(f[4]) or 0, tonumber(f[5]) or 0
  local x, y, z = tonumber(f[6]), tonumber(f[7]), tonumber(f[8])
  local weapon, npc, object = f[9], tonumber(f[10]) or 0, f[11] or "-"
  local p = M.Player and M.Player()
  if not (p and x and y and z) then return end
  local q = p:GetPos()
  local d = math.sqrt((q.x - x) ^ 2 + (q.y - y) ^ 2 + (q.z - z) ^ 2)
  local fire = flags % 2 == 1
  local act, how = "shown", "-"
  if d > FAR then
    act = "far"
    B.stats.far = B.stats.far + 1
  elseif math.floor(flags / 2) % 2 == 1 and not B.ShowsNpc(from, npc) then
    -- his NPC: our own copy of it runs its own fight here (and throws its own grenades), or we do not have it
    act = "npc_here"
    B.stats.npcSkip = B.stats.npcSkip + 1
  else
    local ok
    ok, how = B.Picture(x, y, z, weapon, fire, from, seq, kind)
    if ok then
      B.stats.shown = B.stats.shown + 1
    else
      act = "failed"
      B.stats.failed = B.stats.failed + 1
    end
    if object ~= "-" and game.entitywrapper then
      -- the barrel he set off: gone here too (hidden, not exploded: its own blast would hurt people here)
      local good, o = pcall(game.entitywrapper.GetEntityByName, game.entitywrapper, object)
      if good and valid(o) then try("Hide " .. object, o.Hide, o) end
    end
  end
  -- the cars whose damage we hold take its effect here (car_damage.lua D.Blast), far or near; not when our own copy of
  -- his NPC threw it here (then that blast was real on our game and hit our cars already)
  if act ~= "npc_here" and M.CarDamage and M.CarDamage.Blast then
    local good, err = pcall(M.CarDamage.Blast, x, y, z, kind, from, seq)
    if not good then log("car blast error: " .. tostring(err)) end
  end
  local n = B.stats.shown + B.stats.failed + B.stats.far + B.stats.npcSkip
  if n <= 60 or n % 50 == 0 then
    log(string.format("player %s's %s at %.1f %.1f %.1f (%.0f m from us): %s%s", tostring(from),
      ({ [0] = "explosion", "grenade", "molotov", "dynamite", "object", "car blast" })[kind] or "explosion", x, y, z, d,
      act, act == "failed" and (" (" .. how .. ")") or ""))
  end
  trace("XV", string.format("from=%s seq=%d kind=%d act=%s how=%s d=%.1f p=%s fire=%d", tostring(from), seq, kind, act,
    (how:gsub("%s", "_")):sub(1, 80), d, f3(x, y, z), fire and 1 or 0))
end

-- Another game's car blew up: our entity for it (a copy, or the real car) is set exploded, with a picture.
function B.OnRemoteCar(f)
  local from, what, key = f[2], tonumber(f[3]) or 0, f[4]
  local x, y, z = tonumber(f[5]), tonumber(f[6]), tonumber(f[7])
  local ent, label
  if what == 0 then
    local c = M.cars and M.cars[key]
    ent, label = c and c.ent, "shared car " .. tostring(key)
    if c then c.boomShown = true end  -- our hidden car (if any) is set exploded without a blast when it comes back
  else
    local T = M.Traffic
    local tc = T and T.copies and T.copies[tonumber(key)]
    ent, label = tc and tc.ent, "traffic copy " .. tostring(key)
  end
  local act = "none"
  local D = M.CarDamage
  if valid(ent) and D and D.Explode then
    -- the one explode routine (car_damage.lua D.Explode): once per entity, whether this or car damage's exploded
    -- flag came first; SetExploded and the picture with its damage guard zone
    local good, ok, how = pcall(D.Explode, ent, true, false, from)
    act = good and tostring(how) or ("failed:" .. tostring(ok):gsub("%s", "_"):sub(1, 60))
    if good and ok then B.stats.cars = B.stats.cars + 1 end
  elseif valid(ent) then
    local id = idOf(guidStr(ent))
    if not B.exploded[id] then
      B.exploded[id] = true
      try("DisableExplosion", ent.DisableExplosion, ent, false)  -- copies have it disabled (it also blocks this)
      local good, err = pcall(ent.SetExploded, ent)
      local q = ent:GetPos()
      local ok = B.Picture(q.x, q.y, q.z, "exp_grenade", false, from, 0, 5)
      act = good and "set_exploded" or ("failed:" .. tostring(err):gsub("%s", "_"):sub(1, 60))
      if not ok then act = act .. "+no_picture" end
      B.stats.cars = B.stats.cars + 1
    else
      act = "already"
    end
  elseif x then
    B.Picture(x, y, z, "exp_grenade", false, from, 0, 5)  -- no car here: at least the blast
    act = "no_car"
  end
  -- its blast reaches the cars we hold nearby (car_damage.lua D.Blast; the car itself is exploded already)
  local bx, by, bz = x, y, z
  if valid(ent) then
    local good, q = pcall(ent.GetPos, ent)
    if good and q then bx, by, bz = q.x, q.y, q.z end
  end
  if bx and M.CarDamage and M.CarDamage.Blast then pcall(M.CarDamage.Blast, bx, by, bz, 5, from, 0) end
  log(string.format("player %s's %s blew up: %s", tostring(from), label, act))
  trace("XCV", string.format("from=%s what=%d key=%s act=%s", tostring(from), what, tostring(key), act))
end

-- ---------------------------------------------------------------------------------------------------------------
-- The damage guard (player_sync.lua M.OnDamageEvent calls this first for every DAMAGE): nil = carry on as usual,
-- "undo" = a picture hurt someone here (given back, nothing else happens with it), "ours" = count it as our player's
-- ---------------------------------------------------------------------------------------------------------------
function B.OnDamage(m, g)
  if not mc_boom_judge then return nil end
  local dtype = tonumber(m:GetDamageType()) or -1
  if dtype ~= 1 and dtype ~= 8 then
    local id = idOf(g)
    if B.hp[id] then B.resnap[id] = true end  -- a real hit on an NPC near a picture: remember its health anew
    return nil
  end
  local victim, vid, h = victimOf(g)
  local enemy = B.Classify(tostring(m:GetEnemyGUID()))
  local x, y, z
  if valid(h) then
    local good, q = pcall(h.GetPos, h)
    if good and q then x, y, z = q.x, q.y, q.z end
  end
  if not x then return nil end
  local amount = tonumber(m:GetDamageAmount()) or 0
  local verdict = mc_boom_judge(victim, enemy, dtype, x, y, z, amount)
  if verdict == "undo" then
    B.Undo(victim, vid, h, amount, dtype)
    return "undo"
  end
  if victim == "npc" and B.hp[vid] then B.resnap[vid] = true end
  if verdict == "ours" then return "ours" end
  return nil
end

function B.Undo(victim, vid, h, amount, dtype)
  if victim == "local" then
    -- player_sync.lua M.CopyUndoTick gives our health back on the next tick (a real Hit taken meanwhile lowers the
    -- target by what it took, M.HurtScript)
    if M.lastHealth then M.undoTo = math.max(M.undoTo or 0, M.lastHealth) end
    B.stats.undoLocal = B.stats.undoLocal + 1
    if B.stats.undoLocal <= 20 then
      log(string.format("another game's %s hurt us here (%.1f): given back", dtype == 8 and "fire" or "explosion", amount))
    end
  elseif victim == "npc" then
    if B.hp[vid] then B.restore[vid] = true end
    B.stats.undoNpc = B.stats.undoNpc + 1
  end
  -- puppets (health topped up anyway) and spawned copies (health from their game) need nothing
end

-- Next tick: NPCs hurt by a picture get their health back; NPCs hurt for real are remembered anew.
function B.RestoreTick()
  local n = N()
  for id, _ in pairs(B.restore) do
    local s = B.hp[id]
    local e = n and n.ents and n.ents[id]
    local h = e and e.h
    if s and valid(h) then
      if h:IsDeath() then
        B.stats.lost = B.stats.lost + 1
        log(string.format("NPC %d died of another game's explosion here: cannot be undone (#%d)", id, B.stats.lost))
        trace("XU", string.format("id=%d act=died", id))
      else
        local cur = tonumber(h.health) or 0
        if cur < s.hp then
          pcall(function() h.health = s.hp end)
          if B.stats.undoNpc <= 30 then log(string.format("NPC %d: health %.0f -> %.0f (another game's explosion)", id, cur, s.hp)) end
          trace("XU", string.format("id=%d act=health from=%.1f to=%.1f", id, cur, s.hp))
        end
      end
    end
  end
  B.restore = {}
  for id, _ in pairs(B.resnap) do
    local e = n and n.ents and n.ents[id]
    if e and valid(e.h) then B.hp[id] = { hp = tonumber(e.h.health) or 0, t = M.now } end
  end
  B.resnap = {}
  for id, s in pairs(B.hp) do
    if M.now - s.t > SNAP_KEEP then B.hp[id] = nil end
  end
end

-- ---------------------------------------------------------------------------------------------------------------
-- Our throws and shots (player_sync.lua counts them: M.actSeq / M.actKind 5 = throw, M.shots)
-- ---------------------------------------------------------------------------------------------------------------
local function explosiveName(p)
  local good, w = pcall(p.InventoryGetSelectedExplosive, p)
  if not good then return "", "error: " .. tostring(w) end
  if type(w) == "number" then
    local g2, n = pcall(game.game.GetWeaponNameFromID, game.game, w)
    w = g2 and n or w
  end
  w = tostring(w or "")
  return w, w
end

function B.LocalTick(p)
  if M.actSeq ~= B.actSeq then
    local first = B.actSeq == nil
    B.actSeq = M.actSeq
    if not first and M.actKind == 5 then
      local w, raw = explosiveName(p)
      local q = p:GetPos()
      if w == "" or w:find("^exp_") then
        mc_boom_throw(w:find("^exp_") and tok(w) or "", q.x, q.y, q.z)
        B.throws = (B.throws or 0) + 1
        if B.throws <= 30 then log(string.format("we threw %s at %.1f %.1f %.1f", w ~= "" and w or "something (" .. raw .. ")", q.x, q.y, q.z)) end
      elseif (B.knives or 0) < 5 then
        B.knives = (B.knives or 0) + 1
        log("we threw " .. w .. ": not an explosive")
      end
    end
  end
  if M.shots ~= B.shots then
    local first = B.shots == nil
    B.shots = M.shots
    if not first then
      local q = p:GetPos()
      mc_boom_shot(q.x, q.y, q.z)
    end
  end
end

-- ---------------------------------------------------------------------------------------------------------------
-- Entry point
-- ---------------------------------------------------------------------------------------------------------------
function B.Update(dt, cmds)
  B.events = {}
  local active = false
  local remote, cars = {}, {}
  for line in string.gmatch(cmds, "[^\n]+") do
    local f = {}
    for w in string.gmatch(line, "%S+") do f[#f + 1] = w end
    if f[1] == "S" then
      active = f[2] == "1"
    elseif f[1] == "E" then
      remote[#remote + 1] = f
    elseif f[1] == "C" then
      cars[#cars + 1] = f
    end
  end
  local p = M.Player and M.Player()
  if not (active and p) then
    if B.subs.key then
      if p then pcall(B.Unwatch) else B.subs = {} end  -- session over: unsubscribed; world gone: they went with it
    end
    return ""
  end
  local good, err = pcall(B.Watch)
  if not good then log("watch error: " .. tostring(err)) end
  pcall(B.RestoreTick)
  good, err = pcall(B.LocalTick, p)
  if not good then log("local error: " .. tostring(err)) end
  for _, f in ipairs(remote) do
    good, err = pcall(B.OnRemote, f)
    if not good then log("explosion error: " .. tostring(err)) end
  end
  for _, f in ipairs(cars) do
    good, err = pcall(B.OnRemoteCar, f)
    if not good then log("car error: " .. tostring(err)) end
  end
  -- the dedup table: old cells go
  if M.now - (B.cleanT or -10) >= 5 then
    B.cleanT = M.now
    for k, t in pairs(B.heard) do
      if M.now - t > HEAR_DEDUP then B.heard[k] = nil end
    end
  end
  if M.now - (B.stT or -10) >= 1 then
    B.stT = M.now
    local s = B.stats
    B.events[#B.events + 1] = string.format("ST shown %d (failed %d, far %d, NPC's own %d), cars %d/%d, undone us %d / NPCs %d (died %d), sounds %d of %d heard",
      s.shown, s.failed, s.far, s.npcSkip, s.cars, s.carsSent, s.undoLocal, s.undoNpc, s.lost, s.sounds, s.hearAll)
  end
  return table.concat(B.events, "\n")
end

-- Tests and the dev console: MafiaCoop.Boom.Test(x, y, z, weapon, fire) shows a picture next to us (weapon default
-- exp_grenade), as if another player's explosion had arrived; MafiaCoop.Boom.TestThrow() throws a real grenade from
-- 10 m in front of us with game.game:ThrowProjectileWeapon (docs/game-api.md 23: real damage, for checking the
-- signature only).
function B.Test(x, y, z, weapon, fire)
  local p = M.Player()
  local q = p:GetPos()
  local d = p:GetDir()
  x = x or q.x + d.x * 8
  y = y or q.y + d.y * 8
  z = z or q.z
  local ok, how = B.Picture(x, y, z, weapon or "exp_grenade", fire, 254, 0, 0)
  return string.format("picture at %.1f %.1f %.1f: %s (%s)", x, y, z, ok and "ok" or "failed", tostring(how))
end

function B.TestThrow(weapon)
  local p = M.Player()
  local q, d = p:GetPos(), p:GetDir()
  local from = V(q.x + d.x * 2, q.y + d.y * 2, q.z + 1.5)
  local to = V(q.x + d.x * 12, q.y + d.y * 12, q.z)
  local good, r = pcall(game.game.ThrowProjectileWeapon, game.game, weapon or "exp_grenade", from, to, 30, p)
  return "ThrowProjectileWeapon: " .. tostring(good) .. " " .. tostring(r)
end

return B.version
