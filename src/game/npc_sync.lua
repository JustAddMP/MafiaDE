-- MafiaCoop NPC glue (B3 NPC mirroring, one authority per NPC). Runs in the game's Lua state on GameMainThread after
-- player_sync.lua, called once per script tick by src/game/npc_sync.cpp (MafiaCoop.Npc.Update). Embedded into the dll;
-- a copy at <dll dir>\npc_sync.lua overrides it.
--
-- Every game runs the chapter's own mission scripts at the host's checkpoint, so the mission NPCs exist on every game,
-- spawned by the same scripts at the same places. Each NPC has ONE authority (docs/protocol.md "NPCs",
-- src/game/npc_replica.h): the host for its NPCs, a guest for the NPCs only his game has. Only the authority's AI
-- decides what the NPC does; every other game shows a *replica* driven from the authority's states every tick:
--   - Host: every round (15 Hz) the NPCs near any guest that are not ambient pedestrians (those are ped_sync.lua's,
--     the `ambient-peds` package; npc_replica.h IsAmbientHuman, MafiaCoop.PedOwned) go to the C++ side (NS), which
--     picks per guest what he gets and how often (interest by distance) and sends compact time-stamped states.
--   - Guest: pairs each host NPC with its own NPC (same spot when first seen, docs/game-api.md 15; after a reload or for
--     NPCs that moved, the nearest free one of the same kind within LOOSE_DIST). The paired NPC's own AI is stopped
--     (N.Freeze: InterruptBattle and friends, UNVERIFIED that it holds) and it is DRIVEN: the C++ side interpolates the
--     host's states, runs a locomotion controller + correction for it and sends a command per tick (R): walk to a
--     point with a gait and speed, a small nudge, a snap when far off, a turn. A paired NPC that does not follow (its
--     AI keeps walking off: three snaps in 20 s) is hidden and a spawned copy shows the host's NPC instead. A host NPC
--     without a partner here gets a spawned copy (the same spawn profile; driven the same way). Health and death
--     follow the host's NPC; its shots are pictures; its damage to our player comes from the host as Hit source 1
--     (a local hit by a replica is given back: N.IsPicture). Our hits on a replica hurt it here and go to the host
--     (NpcHit), where they are replayed as real shots from our puppet; a hit that killed our replica is lethal there.
--     Mission scripts keep running here: a mission NPC is never despawned, only driven, hidden or killed.
--   - Guest-owned NPCs: an NPC with a job that only the guest's game has (he got somewhere first), unpaired for 5 s,
--     is the guest's own (src/game/npc_owner.h): its states go to the host the same compact way, the host shows each as
--     a spawned copy (a "mirror", keyed by its mirror id) driven like the guest's replicas; the host's hits on it go to
--     the guest, deaths go both ways; when the host's own game spawns that NPC it is paired again (NpcClaim) and the
--     host is its authority. Copies are never reported or sent (no loops).
--
-- Input (cmds, one line per command):
--   S active host                                session running, we host
--   N key x y z hx hy health flags ai tag weapon profile shots vx vy gait age
--                                                guest: a host NPC where it is now (its replica target; flags: 1 dead,
--                                                2 in a car; tag: archetype; profile: spawn profile hash, 0 = unknown;
--                                                shots: fired, mod 256; vx vy: m/s; gait 0-3; age: s since its last
--                                                state), 10 Hz for every host NPC the C++ side has states of
--   R key kind gait mult reissue mx my mz fix nx ny turn fx fy tx ty tz hx hy refuse
--                                                both: drive replica `key` this tick (kind 1 a paired NPC, 2 a copy;
--                                                gait 0-3; mult: walk speed multiplier; reissue 1: walk to mx my mz now
--                                                (gait 0: walk to tx ty tz and stand); fix 1: move by nx ny, 2: put it at
--                                                tx ty tz facing hx hy; turn 1: face fx fy; refuse 1: it does not follow,
--                                                hide it and show a copy). On the host `key` is a mirror id
--   D key                                        guest: that host NPC died
--   H from key amount dtype zone weapon lethal   host: guest `from` hit host NPC `key` on his screen (lethal 1: his
--                                                replica died of it)
--   GN mid from key x y z hx hy health flags ai tag weapon profile shots
--                                                host: a state of guest `from`'s own NPC `key` (mirror id mid)
--   GD mid                                       host: that mirror died on the guest's game
--   GX mid reason hostKey health                 host: the mirror ends (reason 1 handed back to our NPC hostKey,
--                                                which takes the guest's health; 2 gone): its copy goes
--   GK hostKey mid                               host: our NPC hostKey is the guest's dead mirror mid: it dies
--   O key 1|0                                    guest: our NPC key is (no longer) our own, sent to the host
--   C key hostKey dead                           guest: the host claims our own NPC key as its NPC hostKey
--   X key amount dtype zone weapon lethal        guest: the host hit (his replica of) our own NPC key
-- Output (events, one per line):
--   NSR, then NS cls key x y z hx hy health flags ai tag weapon profile shots
--                                                host: a round (15 Hz) of our NPCs near any guest (cls: name class,
--                                                npc_replica.h NameClass), nearest first, at most MAX_SEND
--   ND key                                       host: NPC died
--   NR key x y z hx hy kind busy                 both: a replica driven this tick, where it is now (after the
--                                                commands; kind 1 paired, 2 copy). Only drivable ones are reported
--   NX key                                       both: that replica is no longer driven (unpaired, copy gone)
--   VPR, then VP id x y z                        both: where the other players are (puppets), every round
--   NH key amount dtype zone weapon lethal       guest: we hit (our replica of) host NPC key
--   GH mid amount dtype zone weapon lethal       host: we hit (our replica of) a guest's own NPC (mirror mid)
--   GOR, then GO dist hostKey key x y z hx hy health flags ai tag weapon profile shots
--                                                guest: a round (15 Hz) of our NPCs with a job near us (hostKey:
--                                                the host NPC it is paired with, 0 = none)
--   GCS id id ...                                both: our spawned copies (never reported or sent)
--   GR key hostKey                               guest: a claim we could not carry out
--   ST text                                      status for the overlay (once a second)
MafiaCoop = MafiaCoop or {}
local M = MafiaCoop
M.Npc = M.Npc or {}
local N = M.Npc
N.version = 4

local SCAN_INTERVAL = 1.0   -- s between scans of the entity table (mc_entity_ids)
local SEND_INTERVAL = 0.066 -- s between host rounds (15 Hz; the C++ side sends each NPC as often as its distance says)
local RANGE = 140           -- m from a guest (his puppet): NPCs in the round (the C++ side admits them within 120 m)
local MAX_SEND = 128        -- NPCs per round, nearest to a guest first
local MATCH_DIST = 1.5      -- m: a guest NPC this close to a host NPC (when first paired) is its copy
local LOOSE_DIST = 40       -- m: NPCs with a job (enemies, allies, police) that moved apart (a fight after a reload, a
                            -- late join, NPCs streamed in late): the nearest free one of the same kind this close
local LOOSE_WAIT = 2        -- s a host NPC stays unpaired before the loose match is tried
local RELEASE = 5           -- s without a state: a pairing is given up (the local NPC is free to pair again)
local LETHAL_WAIT = 0.8     -- s after a guest's lethal hit: the host's NPC still alive is killed
local OUR_HIT_TIME = 1.5    -- s: our replica that dies this soon after our hit died of our hit
local STALE = 3             -- s without a state: the host NPC is out of range (pairing kept, not driven)
local REPLAY_WAIT = 0.35    -- s a replayed guest bullet has to land on the host's NPC before DoDamage is used
local FIRE_SUM = 0.5        -- s over which our fire damage on one NPC is summed into one hit
local CLASS_EVERY = 2       -- s between two looks at a human's AI type, car and name (ambient or not)
local REFREEZE = 2          -- s between two InterruptBattle orders to a driven paired NPC (its mission may restart it)
-- spawned copies (guest)
local COPY_WAIT = 3         -- s a host NPC with a job stays unpaired (after the LOOSE_WAIT pairing) before it gets a copy
local COPY_RANGE = 90       -- m from our player: host NPCs farther away get no copy (and lose theirs at COPY_RANGE + 20)
local MAX_COPIES = 10       -- copies at once (each is a full human on the guest's game)
local COPY_DEAD_KEEP = 12   -- s a dead copy lies before it is released
local COPY_LOAD_TIMEOUT = 12  -- s a spawn profile may take to load before it counts as bad (the spawner is then reset)
local COPY_AFTER_LOAD = 10  -- s after our world loaded before copies are spawned (our own mission spawns its NPCs then)
local FACTORY_CAR = "shubert_e_six"
local FACTORY_PROFILE = "visuals/hero/game_main_tommy_gangster_trenchcoat"
-- Spawn profiles for a copy of an NPC whose own is not known (profile "0": police and civilians of the generators, and
-- every NPC no mission spawner made): by its AI type, police (4) a police profile, anyone else a generic civilian.
-- The spawner component takes a profile as its FNV-1 64 hash (mc_fnv); both names have their hash in
-- sds_retail/combinables/spawnprofiles.sds (checked statically 2026-10-19, as traffic_sync.lua's crews). 0.15.0 two-PC
-- test: 12 guest-owned cops (AI type 4, profile 0) were never shown on the host. UNVERIFIED in game.
local FALLBACK_POLICE = "characters/ai/police_scotch/ai_police_triggerman"
local FALLBACK_CIVILIAN = "characters/traffic/traffic_base_ambient_combined"
local FAST_NPC = 11         -- m/s: a host NPC moving this fast is in a vehicle, whatever its car flag says
local FAST_HOLD = 2         -- s it counts as in a vehicle after that
local UNPAIR_WAIT = 2       -- s a host NPC in a vehicle stays paired with our NPC on foot more than LOOSE_DIST away
local GAITS = { [1] = "WALK", [2] = "RUN", [3] = "SPRINT" }  -- the controller's gait -> MoveVec mode

local function log(s) if mc_log then mc_log("npc: " .. s) end end
local function valid(o) return o ~= nil and IsObjectValid(o) end
-- The host paused his game, or one of the two games resumed less than a second ago (mc_peer_quiet, src/game/
-- peer_pause.h): his NPCs stood while ours did not (or the other way round), so a copy far off is no reason to pull
local function hostQuiet() return mc_peer_quiet ~= nil and (mc_peer_quiet(0) or 0) ~= 0 end
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

N.ents = N.ents or {}      -- local humans: id -> { h = wrapper, id }
N.host = N.host or {}      -- guest: host key -> { x, y, z, hx, hy, hp, dead, car, ai, weapon, t, local = id }
N.byLocal = N.byLocal or {}  -- guest: local id -> host key
N.sent = N.sent or {}      -- host: key -> last dead flag sent
N.replays = N.replays or {}
N.hitq = N.hitq or {}      -- guest: hits from the DAMAGE callbacks, sent with the next update
N.fireq = N.fireq or {}    -- key -> { t, amount, weapon }: our fire on (our copy of) that NPC, summed (N.FireFlush)
N.lethal = N.lethal or {}  -- host: guests' lethal hits waiting for the NPC to die
N.copies = N.copies or {}  -- guest: host key -> spawned copy { key, so, id, st, prof, ... } (also a puppet-like table)
N.copyIds = N.copyIds or {}  -- guest: local id of a spawned copy -> host key
N.cfac = N.cfac or { st = "none", t = 0 }  -- guest: the copies' spawner (a factory like player_sync.lua's)
N.cgrave = N.cgrave or {}  -- guest: burning copies waiting to be released
N.shots = N.shots or {}    -- host: NPC id -> shots fired (mod 256), from the ON_SHOOT events
N.spawners = N.spawners or {}  -- host: spawner name -> { {x, y, z, prof}, ... } (mission spawners, entity type 65)
N.badProfiles = N.badProfiles or {}
N.owned = N.owned or {}    -- guest: local id -> true while it is our own NPC (sent to the host)
N.tcrew = N.tcrew or {}    -- guest: local ids of traffic copy crews (traffic_sync.lua): never our own NPCs
N.cmd = N.cmd or {}        -- both: this tick's drive commands (R lines), key -> fields
N.hidden = N.hidden or {}  -- guest: local id -> host key: a paired NPC that did not follow, hidden (a copy shows it)
N.nx = N.nx or {}          -- both: replicas no longer driven since the last update (NX lines)

-- Relay keys (several guests, src/game/npc_owner.h "relay"): the host sends a guest the other guests' own NPCs in its
-- NpcStates under a relay key: the owner's entity id with its type byte (8, a human) replaced by 240 + the owner's
-- player id, a value no entity id has, so it never is a host NPC's key, and below 2^24, so our 32-bit float numbers
-- hold it exactly. They are host NPCs here like any other (paired, copied, hit: the host forwards hits to the owner).
local RELAY_TYPE, TYPE_HUMAN, BYTE, MAXKEY = 240, 8, 256, 16777216

-- Is the NPC of state `s` in a vehicle on its owner's game? Its car flag (GetOwner there), or it moves faster than a
-- human runs: the 0.15.0 two-PC test had host cops chasing the guest at 12-25 m/s with the car flag 0 (37 samples, our
-- paired cop on foot 20-46 m off and put back every 2 s: NPC 7090184). UNVERIFIED why GetOwner reads nil for them.
function N.InVehicle(s)
  return s.car == true or (s.fastT ~= nil and M.now - s.fastT < FAST_HOLD)
end

-- The spawn profile for a copy of the NPC of state `s` (nil: none): its own, else the fallback for its AI type.
function N.CopyProfile(s)
  if s.prof and s.prof ~= "0" then return s.prof end
  if not mc_fnv then return nil end
  N.fbHash = N.fbHash or {}
  for _, name in ipairs(s.ai == 4 and { FALLBACK_POLICE, FALLBACK_CIVILIAN } or { FALLBACK_CIVILIAN }) do
    local h = N.fbHash[name]
    if not h then
      h = tostring(mc_fnv(name))
      N.fbHash[name] = h
      log("fallback spawn profile " .. name .. " = " .. h)
    end
    if not N.badProfiles[h] then return h, name end
  end
  return nil
end
function N.RelayKey(from, key)
  local f, k = tonumber(from), tonumber(key)
  if not f or not k or k < 0 or k >= MAXKEY or k % BYTE ~= TYPE_HUMAN or f < 1 or f > 15 then return nil end
  return k - TYPE_HUMAN + RELAY_TYPE + f
end
function N.FromRelay(key)  -- -> owner id (string), his key; nil for a plain host key
  key = tonumber(key)
  if not key or key >= MAXKEY or key % BYTE <= RELAY_TYPE then return nil end
  local t = key % BYTE
  return tostring(t - RELAY_TYPE), key - t + TYPE_HUMAN
end

-- Who a pairing key stands for in the log: on a guest a host NPC (or another guest's, relayed); on the host a guest's
-- own NPC (its mirror id).
function N.Who(key)
  local s = N.host[key]
  if N.isHost and s then return string.format("guest %s's NPC %s (mirror %s)", tostring(s.from), tostring(s.gkey),
    tostring(key)) end
  local of, ok = N.FromRelay(key)
  if of then return string.format("guest %s's own NPC %.0f (relayed by the host as %.0f)", of, ok, key) end
  return "host NPC " .. tostring(key)
end
local function owner() return N.isHost and "the guest" or "the host" end

-- The hit line for the C++ side: on a guest an NpcHit for a host NPC, on the host a HostNpcHit for a mirror.
function N.HitLine(key, amount, dtype, zone, weapon, lethal)
  return string.format("%s %.0f %.2f %d %d %.0f %d", N.isHost and "GH" or "NH", key, amount or 0, dtype or 0, zone or 0,
    weapon or 0, lethal and 1 or 0)
end

-- Humans that are ours, not mission NPCs: the puppets and the puppet factory's driver.
local function ours(id)
  for _, pp in pairs(M.pup or {}) do
    if pp.so and valid(pp.so) and idOf(pp.so) == id then return true end
  end
  -- an old puppet entity on its way out or left over after its release (player_sync.lua M.Release): never paired,
  -- sent or owned as an NPC (a friend-brain human would otherwise become a guest-owned NPC on the host)
  if M.IsRetired and M.IsRetired(id) then return true end
  local f = M.fac
  if f and f.drv and valid(f.drv) and idOf(f.drv) == id then return true end
  if N.copyIds[id] then return true end
  local c = N.cfac
  if c and c.drvId == id then return true end
  return false
end

-- The crews of another game's traffic copies here (traffic_sync.lua, every game shows the others' traffic): that
-- game's NPCs, never ours to send or own. Ids collected at most twice a second.
local crewIds, crewT = {}, -10
local function copyCrew(id)
  if M.now - crewT >= 0.5 then
    crewT, crewIds = M.now, {}
    local tr = M.Traffic
    for _, c in pairs(tr and tr.copies or {}) do
      for _, m in ipairs(c.crew or {}) do
        local mid = valid(m) and idOf(m)
        if mid then crewIds[mid] = true end
      end
    end
  end
  return crewIds[id] ~= nil
end

-- ped_sync.lua's humans (the `ambient-peds` package; a missing table is empty) and other games' traffic crews: never
-- ours to send, pair or drive.
local function pedOwned(id)
  local t = M.PedOwned
  if t ~= nil and id ~= nil and t[id] ~= nil then return true end
  return id ~= nil and copyCrew(id)
end

-- Is local human `e` an ambient pedestrian (src/game/npc_replica.h IsAmbientHuman, through mc_npc_class: AI type 0,
-- on foot, not named after a mission spawner)? Looked at every CLASS_EVERY s. Ambient humans are ped_sync.lua's.
function N.Ambient(e)
  if e.clsT and M.now - e.clsT < CLASS_EVERY then return e.amb end
  e.clsT = M.now
  local h = e.h
  local goodA, ai = pcall(h.GetAIType, h)
  local goodC, car = pcall(h.GetOwner, h)
  local goodN, name = pcall(h.GetName, h)
  ai = goodA and tonumber(ai) or 0
  name = goodN and tostring(name or "") or ""
  if mc_npc_class then
    local amb, cls = mc_npc_class(name, ai, (goodC and car ~= nil) and 1 or 0, 0)
    e.amb, e.cls = amb == 1, tonumber(cls) or 0
  else
    e.amb, e.cls = false, 0
  end
  return e.amb
end

-- A replica of another game's NPC (a paired NPC driven here, a hidden one, a spawned copy): its shots on our player
-- are only pictures (player_sync.lua M.OnLocalDamage gives them back); its authority's real hits come as Hits.
function N.IsPicture(id)
  if not id then return false end
  if N.copyIds[id] or N.hidden[id] then return true end
  return not N.isHost and N.byLocal[id] ~= nil
end

-- The entity table scan (mc_entity_ids, src/game/lua.cpp): every human of this world.
function N.Scan()
  if not mc_entity_ids then return end
  local seen = {}
  for s in string.gmatch(mc_entity_ids(8), "%d+") do
    local id = tonumber(s)
    seen[id] = true
    local e = N.ents[id]
    if ours(id) then  -- checked on every scan: the factory's driver changes whenever the factory is rebuilt
      N.ents[id] = nil
    elseif not e or not valid(e.h) then
      local h = mc_entity(id)
      N.ents[id] = h and { h = h, id = id } or nil
    end
  end
  for id, e in pairs(N.ents) do
    if not seen[id] or not valid(e.h) then
      N.ents[id] = nil
      local key = N.byLocal[id]
      local s = key and N.host[key]
      if s and s.loc == id then N.Unlink(s, "gone from our world") end
      if s and s.hid == id then s.hid = nil end
      N.byLocal[id], N.hidden[id] = nil, nil
    end
  end
end

local function weaponOf(h)
  local good, w = pcall(h.InventoryGetSelected, h)
  w = good and w and tostring(w) or ""
  if w == "" or w == "empty" then return "-" end
  return w
end

local function aiOf(h)
  local good, a = pcall(h.GetAIType, h)
  return good and tonumber(a) or 0
end

-- The NPC's battle archetype as a u32 (0 = none): the same mission NPC has the same one on every game, so it tells
-- apart NPCs of different kinds when pairing them by position is not enough.
local function tagOf(h)
  local good, a = pcall(h.GetBattleArchetype, h)
  if not good or a == nil then return 0 end
  local n = tonumber(a)
  if not n then
    n = 5381
    for i = 1, #tostring(a) do n = (n * 33 + string.byte(tostring(a), i)) % 4294967296 end
  end
  return math.floor(n) % 4294967296
end

local function healthOf(h)
  local hp = tonumber(h.health) or 0
  if hp <= 0 then return 0 end
  return math.max(1, math.min(65535, math.ceil(hp)))
end

-- Deactivated humans (the game keeps some in the world switched off: not simulated, they take no damage from anyone,
-- bullets, DoDamage or `health = 0`, and do not move) are no mission NPCs to share: such a "copy" was what looked
-- like a passer-by that takes no damage on the guest's game.
local function active(h)
  local good, a = pcall(h.IsActive, h)
  return not good or a ~= false
end

local function invulnerable(h)
  local good, v = pcall(function() return h.invulnerability end)
  return good and v == true
end

-- ---- host -------------------------------------------------------------------------------------------------------------
-- An NPC's spawn profile. There is no getter on a human; but a human that a mission spawner made is named after it
-- (`behavior_object_prime-NN-<spawner>`), and the spawners are scene objects (entity type 65) whose runtime spawner
-- component tells its profile (a decimal hash; docs/game-api.md 15). "0" when it is not known (traffic, police from
-- the generators, our puppets).
function N.SpawnerScan()
  N.spT = M.now
  local t, n = {}, 0
  for s in string.gmatch(mc_entity_ids(65), "%d+") do
    local o = mc_entity(tonumber(s))
    local good, name = pcall(function() return o and o:GetName() end)
    if good and name and name ~= "" then
      local prof
      for _, cn in ipairs({ "C_RuntimeSpawnerComponent", "C_SpawnerComponent" }) do
        local ok, c = pcall(o.GetComponent, o, cn)
        if ok and c and c.GetSpawnProfile then
          local ok2, pr = pcall(c.GetSpawnProfile, c)
          pr = ok2 and tostring(pr) or ""
          if pr:match("^%d+$") and pr ~= "0" then prof = pr break end
        end
      end
      if prof then
        local q = o:GetPos()
        t[name] = t[name] or {}
        table.insert(t[name], { x = q.x, y = q.y, z = q.z, prof = prof })
        n = n + 1
      end
    end
  end
  N.spawners = t
  if n ~= N.spawnerCount then
    N.spawnerCount = n
    log("mission spawners with a profile: " .. n)
  end
end

function N.ProfileOf(e)
  if e.prof then return e.prof end
  local good, name = pcall(e.h.GetName, e.h)
  local sp = good and tostring(name):match("^behavior_object_prime%-%x+%-(.+)$")
  if not sp or sp:match("^behavior_object_prime") then e.prof = "0" return e.prof end
  local list = N.spawners[sp]
  if not list and M.now - (N.spT or -10) > 3 then
    pcall(N.SpawnerScan)
    list = N.spawners[sp]
  end
  if not list then
    e.profTries = (e.profTries or 0) + 1
    if e.profTries >= 5 then
      e.prof = "0"
      log("NPC " .. e.id .. " comes from spawner " .. sp .. ", which was not found: no profile")
    end
    return "0"
  end
  local q = e.h:GetPos()
  local best, bd
  for _, c in ipairs(list) do
    local d = (c.x - q.x) ^ 2 + (c.y - q.y) ^ 2 + (c.z - q.z) ^ 2
    if not bd or d < bd then best, bd = c, d end
  end
  e.prof = best.prof
  N.profKnown = (N.profKnown or 0) + 1
  if N.profKnown <= 40 then log("NPC " .. e.id .. " comes from spawner " .. sp .. ": profile " .. e.prof) end
  return e.prof
end

-- Host: the NPCs' shots, counted from their ON_SHOOT events (one subscription per world, like player_sync.lua's
-- DAMAGE watch); the guests' copies fire as many.
function N.WatchShots()
  local key = N.worldKey
  if N.shotWatch and N.shotWatch.key == key then return end
  N.UnwatchShots()
  local good, id = pcall(game.game.RegisterBlanketEvent, game.game, {}, function(ctx, m)
    pcall(function()
      local r = tonumber(tostring(m:GetReceiverGUID()):match("(%d+)%s*$"))
      if r and N.ents[r] then
        N.shots[r] = ((N.shots[r] or 0) + 1) % 256
        N.shotEvents = (N.shotEvents or 0) + 1
        if N.shotEvents <= 5 then log("NPC " .. r .. " fired (shot event #" .. N.shotEvents .. ")") end
      end
    end)
  end, enums.EventType.HUMAN, enums.HumanMessages.ON_SHOOT)
  if good and tonumber(id) and tonumber(id) >= 0 then
    N.shotWatch = { key = key, id = id }
    log("watching NPC shots")
  else
    N.shotWatch = { key = key }
    log("could not watch NPC shots (" .. tostring(id) .. ")")
  end
end

function N.UnwatchShots()
  if N.shotWatch and N.shotWatch.id then pcall(game.game.UnregisterBlanketEvent, game.game, N.shotWatch.id) end
  N.shotWatch = nil
end

-- Where the other players are (their puppets on our game): the C++ side's interest (host: per guest; guest: what the
-- host and the other guests are near, for our own NPCs). Every round.
function N.Views(ev)
  ev[#ev + 1] = "VPR"
  for id, pp in pairs(M.pup or {}) do
    if pp.tx and pp.ty and tonumber(id) then
      ev[#ev + 1] = string.format("VP %d %.2f %.2f %.2f", tonumber(id), pp.tx, pp.ty, pp.tz or 0)
    end
  end
end

-- The NPCs of this round: every one within RANGE of a guest that is shared (not ambient, not ped_sync's), at most
-- MAX_SEND, nearest to a guest first. The C++ side decides per guest (interest).
function N.PickSend()
  local vps = {}
  for _, pp in pairs(M.pup or {}) do
    if pp.tx and pp.ty then vps[#vps + 1] = { pp.tx, pp.ty } end
  end
  local list = {}
  if #vps == 0 then return list, 0 end
  for id, e in pairs(N.ents) do
    local h = e.h
    if valid(h) and active(h) and not pedOwned(id) then
      local q = h:GetPos()
      local best
      for _, v in ipairs(vps) do
        local d = (q.x - v[1]) ^ 2 + (q.y - v[2]) ^ 2
        if not best or d < best then best = d end
      end
      if best <= RANGE * RANGE and not N.Ambient(e) then list[#list + 1] = { e = e, q = q, d = best } end
    end
  end
  table.sort(list, function(a, b) return a.d < b.d end)
  local all = #list
  while #list > MAX_SEND do table.remove(list) end
  return list, all
end

function N.HostSend(p, ev)
  local list, all = N.PickSend()
  if all > MAX_SEND and (N.cutLogT == nil or M.now - N.cutLogT > 30) then
    N.cutLogT = M.now
    log(string.format("%d NPCs near the guests, the %d nearest in each round", all, MAX_SEND))
  end
  ev[#ev + 1] = "NSR"
  for i = 1, #list do
    local e, q, h = list[i].e, list[i].q, list[i].e.h
    local d = h:GetDir()
    local dead = h:IsDeath()
    local flags = (dead and 1 or 0) + (h:GetOwner() and 2 or 0)
    if not e.tagT or M.now - e.tagT > 2 then e.tag, e.tagT = tagOf(h), M.now end
    local prof = "0"
    if not dead then
      local good, pr = pcall(N.ProfileOf, e)
      prof = good and pr or "0"
    end
    ev[#ev + 1] = string.format("NS %d %d %.2f %.2f %.2f %.3f %.3f %d %d %d %.0f %s %s %d", e.cls or 0, e.id, q.x, q.y,
      q.z, d.x, d.y, dead and 0 or healthOf(h), flags, aiOf(h), e.tag or 0, weaponOf(h), prof, N.shots[e.id] or 0)
    if dead and not N.sent[e.id] then
      N.sent[e.id] = true
      ev[#ev + 1] = "ND " .. e.id
      log("host NPC " .. e.id .. " died")
    elseif not dead then
      N.sent[e.id] = nil
    end
  end
  N.hostCount = #list
end

-- A guest's hit on our NPC: a bullet is replayed as a real shot from his puppet (the NPC's own hit reaction, damage
-- and AI response: it turns on the puppet); anything else, or a replay that did not land, is DoDamage.
function N.HostHit(from, key, amount, dtype, zone, weapon, lethal)
  local e = N.ents[key]
  if not e or not valid(e.h) or e.h:IsDeath() then return end
  local h = e.h
  local pp = M.pup and M.pup[from]
  N.hitsIn = (N.hitsIn or 0) + 1
  if lethal then
    -- the guest's copy died of this hit: the host's NPC dies too (below, if the hit itself does not kill it)
    N.lethal[#N.lethal + 1] = { t = M.now, key = key, from = from }
    log(string.format("player %s killed his copy of our NPC %d (health here %.0f)", from, key, tonumber(h.health) or -1))
    if pp then N.Engage(h, pp) end
    return
  end
  if dtype == 0 and pp and pp.so and valid(pp.so) and pp.st == "alive" and not pp.veh then
    local a = pp.so:GetBonePos("Head")
    local b = h:GetPos()
    local tz = b.z + ({ [0] = 0.95, [1] = 1.3, [2] = 1.62, [3] = 1.3, [4] = 1.3, [5] = 0.5, [6] = 0.5, [7] = 1.7 })[zone] or 1.3
    local dx, dy, dz = b.x - a.x, b.y - a.y, tz - a.z
    local l = math.sqrt(dx * dx + dy * dy + dz * dz)
    if l > 0.3 and l < 200 then
      local f = math.min(0.4, l * 0.5)
      local w = (pp.weapon and pp.weapon ~= "") and pp.weapon or "p_mast_a_v1"
      local ok, err = pcall(game.shotmgr.CreateShots, game.shotmgr, pp.so,
        V(a.x + dx / l * f, a.y + dy / l * f, a.z + dz / l * f), V(b.x, b.y, tz), w, 0, 0, false, false)
      if not ok then log("CreateShots failed: " .. tostring(err)) end
      if ok then
        N.replays[#N.replays + 1] = { t = M.now, key = key, from = from, amount = amount, h0 = tonumber(h.health) or 0,
          shooter = idOf(pp.so) }
        if N.hitsIn <= 30 then log(string.format("player %s shot our NPC %d: replayed from his puppet", from, key)) end
        N.Engage(h, pp)
        return
      end
    end
  end
  pcall(h.DoDamage, h, amount)
  if N.hitsIn <= 30 then log(string.format("player %s hit our NPC %d: %.1f type %d, DoDamage", from, key, amount, dtype)) end
  N.Engage(h, pp)
end

-- Enemies only shoot at the player's side. On the host each guest's puppet gets the FRIEND brain (player_sync.lua
-- M.Ally), so the host's enemies target it and their hits reach the guest (N.OnDamage); each guest's own copies of
-- the enemies fight him on his game as well. An NPC that a guest attacked fights back at the guest's puppet (hostile
-- NPCs only; civilians flee on their own).
function N.Engage(h, pp)
  if not (pp and pp.so and valid(pp.so)) or h:IsDeath() then return end
  local ai = aiOf(h)
  if ai ~= 2 and ai ~= 4 then return end
  pcall(h.SetShootTargetPriority, h, pp.so:GetGUID(), 100, true, true)
  try("Attack", h.Attack, h, pp.so)
end

-- DAMAGE events (player_sync.lua forwards every human's): a replayed bullet that landed, and on guests our hits on
-- copies of host NPCs.
function N.OnDamage(m, receiverId)
  local enemyId = tonumber(tostring(m:GetEnemyGUID()):match("(%d+)%s*$"))
  if not M.isHost and enemyId and N.owned[enemyId] then
    -- our own NPC (the host's game has none of it) hit a player's puppet, the host's or another guest's (every puppet
    -- has the friend brain, so our NPCs fire at all of them): that player takes it (M.HurtByNpc on his game; another
    -- guest skips it when his own version of this NPC, paired with the host's relay of ours, fights him there)
    for id, pp in pairs(M.pup or {}) do
      if pp.so and valid(pp.so) and idOf(pp.so) == receiverId then
        N.ownHits = (N.ownHits or 0) + 1
        local amount = tonumber(m:GetDamageAmount()) or 0
        if N.ownHits <= 20 or N.ownHits % 50 == 0 then
          log(string.format("our own NPC %d hit player %s's puppet (%.1f, type %s; #%d): sent to him", enemyId, id,
            amount, tostring(m:GetDamageType()), N.ownHits))
        end
        if pp.st == "alive" and M.SendNpcHit then
          M.SendNpcHit(pp, amount, tonumber(m:GetDamageType()) or 0, tonumber(m:GetHitZone()) or 0,
            tonumber(m:GetWeaponID()) or 0, enemyId)
        end
        return true
      end
    end
  end
  if not M.isHost then
    -- the host's hit on our own NPC, replayed from his puppet here, landed
    for _, r in ipairs(N.replays) do
      if not r.landed and r.key == receiverId and r.shooter == enemyId then r.landed = true return true end
    end
  end
  if M.isHost and not N.copyIds[receiverId] then
    local enemy = enemyId
    if enemy and N.ents[enemy] then  -- one of our NPCs hit a guest's puppet: the guest takes the hit on his game
      for id, pp in pairs(M.pup or {}) do
        if pp.so and valid(pp.so) and idOf(pp.so) == receiverId then
          N.puppetHits = (N.puppetHits or 0) + 1
          local amount = tonumber(m:GetDamageAmount()) or 0
          if N.puppetHits <= 20 or N.puppetHits % 50 == 0 then
            log(string.format("NPC %d hit guest %s's puppet (%.1f, type %s; #%d)", enemy, id, amount,
              tostring(m:GetDamageType()), N.puppetHits))
          end
          if pp.st == "alive" and M.SendNpcHit then
            M.SendNpcHit(pp, amount, tonumber(m:GetDamageType()) or 0, tonumber(m:GetHitZone()) or 0,
              tonumber(m:GetWeaponID()) or 0, enemy)
          end
          return true
        end
      end
    end
    for _, r in ipairs(N.replays) do
      if not r.landed and r.key == receiverId and r.shooter == enemy then r.landed = true return true end
    end
    return false
  end
  -- a copy of an NPC of the other game (on a guest: of a host NPC; on the host: a mirror's copy), hit
  local key = N.byLocal[receiverId]
  if M.isHost then key = N.copyIds[receiverId] end
  if not key then return false end
  local cc = N.copyIds[receiverId] and N.copies[N.copyIds[receiverId]]
  if cc and tonumber(m:GetDamageType()) == 8 then cc.burnT = M.now end  -- FIRE: see N.ReleaseCopy
  local enemy = tostring(m:GetEnemyGUID())
  if not (M.IsLocalAttacker and M.IsLocalAttacker(enemy)) then return true end  -- our copy, hit by someone else
  local amount = tonumber(m:GetDamageAmount()) or 0
  local dtype, zone = tonumber(m:GetDamageType()) or 0, tonumber(m:GetHitZone()) or 0
  local weapon = tonumber(m:GetWeaponID()) or 0
  if amount > 0 then
    local s = N.host[key]
    if s then s.ourHitT, s.ourHit, s.ourType, s.ourZone, s.ourWeapon = M.now, amount, dtype, zone, weapon end
    if dtype == 8 then
      -- FIRE burns in many small steps (our molotov, docs/game-api.md 23): summed over FIRE_SUM s into one hit
      local f = N.fireq[key]
      if not f then f = { t = M.now, amount = 0, weapon = weapon }; N.fireq[key] = f end
      f.amount = f.amount + amount
      return true
    end
    N.hitq[#N.hitq + 1] = N.HitLine(key, amount, dtype, zone, weapon, false)
    N.hitsOut = (N.hitsOut or 0) + 1
    if N.hitsOut <= 30 or N.hitsOut % 50 == 0 then
      log(string.format("we hit %s (our %d): %.1f type %d zone %d (hit #%d)", N.Who(key), receiverId, amount, dtype,
        zone, N.hitsOut))
    end
  end
  return true
end

-- Guest: NPC `key` of player `from`'s game hit our puppet there (the host's NPC, or another guest's own NPC, known here
-- by its relay key). True when its hit already happened on our game, so the relayed one is skipped (player_sync.lua
-- M.ApplyHit). The rule is src/game/npc_replica.h SkipRelayedHit (mc_npc_skip_hit): only where our game really runs that
-- NPC (our own NPC, alive and hostile). A replica (a paired NPC driven here, a hidden one, a copy) never fights here:
-- its authority's hits always count (before the one-authority rule a hostile paired copy skipped them).
function N.FightsUs(key, from)
  if N.isHost then return false end  -- (the host's N.host holds mirrors: a guest's NPC that hit us never fights here)
  if from and tostring(from) ~= "0" then key = N.RelayKey(from, key) end
  local s = key and N.host[key]
  local id = s and (s.loc or s.hid)
  local role = 0
  if id then role = N.owned[id] and 2 or 1 end
  local e = id and N.ents[id]
  local alive = e ~= nil and valid(e.h) and not e.h:IsDeath()
  local hostile = alive and aiOf(e.h) == 2
  if not mc_npc_skip_hit then return false end
  return mc_npc_skip_hit(role, alive and 1 or 0, hostile and 1 or 0) == 1
end

function N.ReplayTick()
  local waiting = {}
  for _, l in ipairs(N.lethal) do
    local e = N.ents[l.key]
    if not (e and valid(e.h)) or e.h:IsDeath() then
      if e and valid(e.h) then
        N.guestKills = (N.guestKills or 0) + 1
        log(string.format("our NPC %d is dead: player %s's kill (#%d)", l.key, l.from, N.guestKills))
      end
    elseif M.now - l.t >= LETHAL_WAIT then
      -- an enemy the mission keeps invulnerable here dies too: the guest's game let him kill it (its scripts made it
      -- vulnerable where the guest's player is); allies and civilians it protects are left alone
      if invulnerable(e.h) and aiOf(e.h) ~= 2 then
        log("our NPC " .. l.key .. " is invulnerable here (the mission): player " .. l.from .. "'s kill is not repeated")
      else
        if invulnerable(e.h) then
          pcall(function() e.h.invulnerability = false end)
          log("our NPC " .. l.key .. " is invulnerable here (the mission): made vulnerable for player " .. l.from .. "'s kill")
        end
        pcall(e.h.DoDamage, e.h, 100000)
        N.guestKills = (N.guestKills or 0) + 1
        log(string.format("our NPC %d killed: player %s's kill (#%d, health then %.0f)", l.key, l.from, N.guestKills,
          tonumber(e.h.health) or -1))
      end
    else
      waiting[#waiting + 1] = l
    end
  end
  N.lethal = waiting
  local keep = {}
  for _, r in ipairs(N.replays) do
    if not r.landed and M.now - r.t > REPLAY_WAIT then
      local e = N.ents[r.key]
      if e and valid(e.h) and not e.h:IsDeath() then pcall(e.h.DoDamage, e.h, r.amount) end
      N.replayMisses = (N.replayMisses or 0) + 1
      if N.replayMisses <= 10 then log("replayed shot on NPC " .. r.key .. " did not land: DoDamage") end
    elseif not r.landed then
      keep[#keep + 1] = r
    end
  end
  N.replays = keep
end

-- ---- guest ------------------------------------------------------------------------------------------------------------
-- Pairing: every host NPC with a state gets one local human. First the strict match (the same spot: the same mission
-- script spawned both, MATCH_DIST); a host NPC with a job (AI type not 0) that found none within LOOSE_WAIT s gets the
-- nearest free local human of the same kind (the same archetype tag, or the same AI type) within LOOSE_DIST: after a
-- reload of the guest, a late join or NPCs that streamed in late the fight has moved on, and the two games' NPCs are
-- no longer at the same spots. Dead host NPCs are paired too, so the guest's copy dies (enemies the host already
-- killed die on a guest who reloaded). Pairs are taken nearest first over all candidates.
-- The paired NPC's own AI is stopped while it is a replica (docs/research-world-sync.md B3 "Freeze / AI off"; no brain
-- switch: that crashed the game, docs/game-api.md 15). Which of these holds is UNVERIFIED in game: a paired NPC that
-- still walks off is given up (three snaps in 20 s: hidden, a copy shown, N.Refuse).
local FREEZE = { "InterruptBattle", "InterruptCivilWander", "InterruptPatrol", "InterruptAvoidingCar" }
N.freezeLog = N.freezeLog or {}
function N.Freeze(h, id)
  for _, name in ipairs(FREEZE) do
    local f = h[name]
    local good, err = false, "missing"
    if f then good, err = pcall(f, h) end
    local fl = N.freezeLog[name] or { ok = 0, bad = 0 }
    N.freezeLog[name] = fl
    if good then fl.ok = fl.ok + 1 else fl.bad = fl.bad + 1 end
    if (good and fl.ok == 1) or (not good and fl.bad <= 3) then
      log(string.format("freeze %s on our %s: %s", name, tostring(id), good and "ok" or ("failed: " .. tostring(err))))
    end
  end
  pcall(h.SetReactionOnSound, h, false)
  N.frozen = (N.frozen or 0) + 1
end

function N.Unfreeze(h, id)
  pcall(h.ResumeBattle, h)
  pcall(h.SetReactionOnSound, h, true)
  N.unfrozen = (N.unfrozen or 0) + 1
  if N.unfrozen <= 20 then log("our " .. tostring(id) .. " is no replica any more: its own AI again") end
end

-- The local NPC `s.loc` is no longer the replica of host NPC `s` (the pairing is given up; a copy goes elsewhere).
function N.Unlink(s, why)
  local id = s.loc
  if id then
    if N.byLocal[id] == s.key then N.byLocal[id] = nil end
    if not s.copy then
      local e = N.ents[id]
      if e and valid(e.h) and not e.h:IsDeath() then N.Unfreeze(e.h, id) end
    end
    N.nx[#N.nx + 1] = s.key
    N.unlinks = (N.unlinks or 0) + 1
    if why and N.unlinks <= 60 then log(string.format("%s: our %d no longer its replica (%s)", N.Who(s.key), id, why)) end
  end
  s.loc, s.copy = nil, nil
end

local function link(key, s, id, d, how)
  s.copy = N.copyIds[id] ~= nil
  s.loc, s.ourHitT, s.deadHere, s.killT, s.kills, s.hidden, s.notedHostile = id, nil, nil, nil, 0, nil, nil
  N.byLocal[id] = key
  if not s.copy then
    local e = N.ents[id]
    if e and valid(e.h) and not e.h:IsDeath() then N.Freeze(e.h, id) end
    s.frzT = M.now
  end
  N.paired = (N.paired or 0) + 1
  local loose = how ~= "same spot"
  if loose then N.pairedLoose = (N.pairedLoose or 0) + 1 end
  if N.paired <= 80 or (loose and N.pairedLoose <= 80) then
    log(string.format("%s is our %d (%.2f m apart, %s%s)", N.Who(key), id, d, how,
      s.dead and (", dead on " .. owner() .. "'s game") or ""))
  end
end

function N.PairAll()
  local free = {}
  for id, e in pairs(N.ents) do
    if not N.byLocal[id] and not N.hidden[id] and not pedOwned(id) and valid(e.h) and not e.h:IsDeath()
        and active(e.h) and not N.Ambient(e) then
      local q = e.h:GetPos()
      local okc, car = pcall(e.h.GetOwner, e.h)
      free[#free + 1] = { id = id, x = q.x, y = q.y, z = q.z, ai = aiOf(e.h), tag = tagOf(e.h),
        car = okc and car ~= nil or false }
    end
  end
  if #free == 0 then return end
  local cands = {}
  for key, s in pairs(N.host) do
    -- a host NPC shown by a spawned copy is paired again as soon as our own game has that NPC (our mission spawned
    -- it after all): our own takes over and the copy goes
    if (not s.loc or s.copy) and not s.hid and M.now - s.t <= STALE then
      s.unpairedSince = s.unpairedSince or M.now
      local loose = s.ai ~= 0 and (s.copy or M.now - s.unpairedSince >= LOOSE_WAIT)
      for i, f in ipairs(free) do
        local d = math.sqrt((f.x - s.x) ^ 2 + (f.y - s.y) ^ 2 + (f.z - s.z) ^ 2)
        -- the same spot and the same kind (0.12.0: another of our NPCs that stands there, e.g. our kitchen rifleman
        -- where the host's kitchen machine gunner came in, is not it; a spawned copy gives way only to its own kind)
        local kindOk = (s.tag or 0) == 0 or f.tag == 0 or f.tag == s.tag
        if d <= MATCH_DIST and not s.dead and kindOk then
          cands[#cands + 1] = { d = d, dist = d, s = s, i = i, how = "same spot" }
        elseif loose and d <= LOOSE_DIST and f.ai ~= 0 and math.abs(f.z - s.z) < 8 and f.car == N.InVehicle(s) then
          -- (a host NPC in a vehicle is not ours on foot, nor the other way round: 0.15.0 paired a host cop driving
          -- after the guest with our cop on foot, put back 30-50 m every 2 s)
          local sameTag = (s.tag or 0) ~= 0 and f.tag == s.tag
          local sameAi = f.ai == s.ai
          if sameTag or (sameAi and ((s.tag or 0) == 0 or f.tag == 0) and not s.copy) then
            -- the same archetype counts double: a near NPC of another kind loses to a farther one of this kind
            cands[#cands + 1] = { d = sameTag and d * 0.5 or d, dist = d, s = s, i = i,
              how = sameTag and "same kind, moved apart" or "same AI type, moved apart" }
          end
        end
      end
    end
  end
  table.sort(cands, function(a, b)
    if (a.how == "same spot") ~= (b.how == "same spot") then return a.how == "same spot" end
    return a.d < b.d
  end)
  local used = {}
  for _, c in ipairs(cands) do
    if not used[c.i] and (not c.s.loc or c.s.copy) then
      used[c.i] = true
      if c.s.copy then
        N.DropCopy(c.s.key, "our own game has this NPC now (our " .. free[c.i].id .. ", " .. c.how .. ")")
      end
      if c.dist > 5 then  -- a loose pair: it starts where the host's NPC is (the drive takes it from there)
        local e = N.ents[free[c.i].id]
        if e and valid(e.h) then pcall(e.h.SetPos, e.h, V(c.s.x, c.s.y, c.s.z)) end
      end
      link(c.s.key, c.s, free[c.i].id, c.dist, c.how)
    end
  end
end

-- A paired NPC that does not follow its states (the C++ side counted three snaps in 20 s: its own AI keeps walking
-- off): hidden where the host's NPC is (no model, no collision, no damage), kept paired so it is not paired again (it
-- dies with the host's NPC), and a spawned copy shows the host's NPC instead.
function N.Refuse(s, key)
  local id = s.loc
  local e = id and N.ents[id]
  if not (e and valid(e.h)) or s.copy then return end
  local h = e.h
  pcall(h.ShowModel, h, false)
  pcall(h.SetPhysState, h, 2)
  pcall(h.SetDemigod, h, true)
  pcall(h.InterruptBattle, h)
  N.hidden[id] = key
  s.hid, s.loc, s.copy = id, nil, nil
  s.unpairedSince = M.now - 100  -- a copy at once
  N.nx[#N.nx + 1] = key
  N.refused = (N.refused or 0) + 1
  log(string.format("our %d does not follow %s (its own AI walks off): hidden, a copy shows it (#%d)", id,
    N.Who(key), N.refused))
end

-- A hidden paired NPC stays near the host's NPC (out of sight) and dies with it; when the host's NPC is forgotten, it is
-- shown again with its own AI.
function N.HiddenTick(s, key)
  local id = s.hid
  local e = id and N.ents[id]
  if not (e and valid(e.h)) then
    if id then N.hidden[id] = nil end
    s.hid = nil
    return
  end
  local h = e.h
  if s.dead and not h:IsDeath() then N.Kill(h) return end
  if M.now - (s.hidT or -10) >= 1 then
    s.hidT = M.now
    local q = h:GetPos()
    if (q.x - s.x) ^ 2 + (q.y - s.y) ^ 2 > 4 then pcall(h.SetPos, h, V(s.x, s.y, s.z)) end
  end
end

function N.Unhide(s, key)
  local id = s.hid
  local e = id and N.ents[id]
  if e and valid(e.h) and not e.h:IsDeath() then
    pcall(e.h.ShowModel, e.h, true)
    pcall(e.h.SetPhysState, e.h, 0)
    pcall(e.h.SetDemigod, e.h, false)
    N.Unfreeze(e.h, id)
  end
  if id then N.hidden[id], N.byLocal[id] = nil, nil end
  s.hid = nil
end

function N.Kill(h)
  pcall(function() h.invulnerability = false end)
  pcall(h.SetDemigod, h, false)
  pcall(h.DoDamage, h, 100000)
end

-- Keep our copy `e` of host NPC `s` in line.
function N.Enforce(s, e, p)
  local h = e.h
  if s.dead then
    if not h:IsDeath() and not s.hidden and M.now - (s.killT or -10) >= 1 then
      s.killT, s.kills = M.now, (s.kills or 0) + 1
      if s.kills <= 3 then
        N.Kill(h)
        if s.kills == 1 then log(N.Who(s.key) .. " is dead: our " .. e.id .. " dies") end
      else
        -- some humans take no damage from scripts (or anyone's bullets) on this game: out of sight instead
        s.hidden = true
        pcall(h.ShowModel, h, false)
        pcall(h.SetPhysState, h, 2)
        log(string.format("our %d does not die (health %.0f, invulnerable %s): hidden like the dead %s",
          e.id, tonumber(h.health) or -1, tostring(invulnerable(h)), N.Who(s.key)))
      end
    end
    return
  end
  if h:IsDeath() then
    if not s.deadHere then
      s.deadHere = true
      if M.now - (s.ourHitT or -10) <= OUR_HIT_TIME then
        -- we killed it: the host's NPC dies too (a lethal hit, N.HostHit)
        N.hitq[#N.hitq + 1] = N.HitLine(s.key, s.ourHit, s.ourType, s.ourZone, s.ourWeapon, true)
        N.kills = (N.kills or 0) + 1
        log(string.format("we killed our %d: %s dies too (our kill #%d)", e.id, N.Who(s.key), N.kills))
      else
        log("our copy " .. e.id .. " of " .. N.Who(s.key) .. " died here while " .. owner() .. "'s lives")
      end
    end
    return
  end
  -- health follows the host's NPC down (his shots and everyone else's there), never up: our own hits hurt our copy
  -- here at once, and the host's NPC may not take them (the mission keeps it invulnerable on his game until his
  -- player gets somewhere, Bon Appetit cp_040; or the replayed shot missed): when our hits kill our copy, the host's
  -- NPC is killed too (the lethal hit above)
  local hp, here = tonumber(s.hp) or 0, tonumber(h.health) or 0
  if hp > 0 and here > hp + 0.5 then pcall(function() h.health = hp end) end
  if s.copy then return end  -- a spawned copy: its drive and pictures in N.DriveCopy
  -- (while the host's game is paused or just resumed, hostQuiet: his NPCs stood, their last speed and distance say
  -- nothing; no unpairing then)
  local inVeh, ourCar = N.InVehicle(s), h:GetOwner()
  -- the host's NPC drives (or rides), ours walks far from it: not the same NPC (a loose pair made while both stood,
  -- or before the host's got in): unpaired, the pairing finds one of the right kind (or none)
  if inVeh and not ourCar and s.ai ~= 0 and not hostQuiet() then
    local q = h:GetPos()
    local d = math.sqrt((q.x - s.x) ^ 2 + (q.y - s.y) ^ 2)
    if d > LOOSE_DIST then
      s.vehOffSince = s.vehOffSince or M.now
      if M.now - s.vehOffSince > UNPAIR_WAIT then
        N.unpairs = (N.unpairs or 0) + 1
        if N.unpairs <= 40 then
          log(string.format("%s is in a vehicle (%s), our %d on foot %.0f m off: unpaired (#%d)", N.Who(s.key),
            s.car and "car flag" or string.format("%.0f m/s", s.spd or 0), e.id, d, N.unpairs))
        end
        N.Unlink(s, nil)
        s.vehOffSince, s.unpairedSince = nil, M.now
        return
      end
    else
      s.vehOffSince = nil
    end
  else
    s.vehOffSince = nil
  end
  -- its mission may order it into a fight again: interrupted again now and then while it is driven
  if M.now - (s.frzT or 0) >= REFREEZE then
    s.frzT = M.now
    pcall(h.InterruptBattle, h)
  end
end

function N.GuestTick(p)
  local matched, live = 0, 0
  for key, s in pairs(N.host) do
    if s.loc and not s.copy and M.now - s.t > RELEASE then  -- out of the host's range (or gone): ours is free again
      N.Unlink(s, "no states for " .. RELEASE .. " s")
    end
    if s.hid then N.HiddenTick(s, key) end
    if M.now - s.t > 30 then
      if s.hid then N.Unhide(s, key) end
      N.host[key] = nil
    elseif M.now - s.t <= STALE then
      live = live + 1
      local e = s.loc and (N.ents[s.loc] or (s.copy and N.copies[key] and N.copies[key].ent))
      if s.loc and not (e and valid(e.h) and active(e.h)) then
        if s.copy then
          N.byLocal[s.loc] = nil
          s.loc = nil
        else
          N.Unlink(s, "it is gone or switched off")
        end
        e = nil
      end
      if e then
        s.unpairedSince = nil
        matched = matched + 1
        local good, err = pcall(N.Enforce, s, e, p)
        if not good then log("enforce error: " .. tostring(err)) end
      end
    end
  end
  if M.now - (N.pairT or -10) >= 0.5 then
    N.pairT = M.now
    local good, err = pcall(N.PairAll)
    if not good then log("pairing error: " .. tostring(err)) end
  end
  local good, err = pcall(N.CopiesTick, p)
  if not good then log("copies error: " .. tostring(err)) end
  local nc = 0
  for _, c in pairs(N.copies) do if c.st == "alive" then nc = nc + 1 end end
  N.status = string.format("NPCs: %d of the host's %d driven (%d spawned copies, %d hidden), our kills %d", matched,
    live, nc, N.refused or 0, N.kills or 0)
end

-- ---- spawned copies (guest) -------------------------------------------------------------------------------------------
-- The copies' spawner: a crewed traffic car parked 40 m below our player whose driver carries a runtime spawner
-- component (the recipe of player_sync.lua's puppet factory, docs/game-api.md 3); one profile at a time. Despawning
-- it removes every copy it made.
-- wait: a burning copy holds the reset back (the despawn would release it); a lost spawner is despawned anyway (its
-- driver is gone: whatever it made goes with its car).
function N.FacReset(why, wait)
  local f = N.cfac
  if wait then
    for _, c in pairs(N.copies) do
      if c.burnT and M.now - c.burnT < 8 then
        if not f.burnWait then log("copy spawner reset (" .. tostring(why) .. ") waits: a copy burns") end
        f.burnWait = true
        return false
      end
    end
  end
  if f.id then pcall(game.traffic.DespawnEntityImmediately, game.traffic, f.id) end
  if why then log("copy spawner reset: " .. tostring(why)) end
  for key, c in pairs(N.copies) do N.Forget(key, c) end
  N.cgrave = {}
  N.cfac = { st = "none", t = -1 }
  return true
end

function N.FacTick(dt, need, p)
  local f = N.cfac
  f.t = f.t + dt
  if f.st == "none" then
    if not need or f.t < 0 then return end
    local so, id = try("copy spawner ObtainCarWithCrew", game.traffic.ObtainCarWithCrew, game.traffic, FACTORY_CAR,
      { FACTORY_PROFILE })
    if so and id then
      f.st, f.so, f.id, f.t = "obtain", so, id, 0
    else
      f.t = -5
    end
  elseif f.st == "obtain" then
    if f.so:IsDone() then
      local q = p:GetPos()
      f.sp = game.traffic:SpawnCar(f.id, V(q.x + 3, q.y, q.z - 40), V(1, 0, 0))
      f.st, f.t = "spawn", 0
    elseif f.t > 6 then
      N.FacReset("obtain timeout", true)
    end
  elseif f.st == "spawn" then
    if f.sp:IsDone() then
      local car = game.traffic:GetSpawnedEntity(f.id)
      local drv = car and car:GetSeatOwner(0)
      if not valid(drv) then N.FacReset("no crew", true) return end
      pcall(car.FixCarPos, car, true)
      pcall(car.SetDespawnImmunity, car, true)
      if not drv:GetComponent("C_RuntimeSpawnerComponent") then drv:AddComponent("C_RuntimeSpawnerComponent") end
      local comp = drv:GetComponent("C_RuntimeSpawnerComponent")
      if not comp then N.FacReset("no spawner component", true) return end
      pcall(drv.SetDemigod, drv, true)
      pcall(drv.DespawnImmunity, drv, true)
      pcall(drv.SetReactionOnSound, drv, false)
      pcall(drv.InterruptCivilWander, drv)
      pcall(drv.LockControls, drv, true)
      f.car, f.drv, f.drvId, f.comp, f.st, f.t, f.busy = car, drv, idOf(drv), comp, "ready", 0, nil
      N.ents[f.drvId] = nil
      log("copy spawner ready")
    elseif f.t > 8 then
      N.FacReset("spawn timeout", true)
    end
  elseif f.st == "ready" then
    if not valid(f.drv) or not valid(f.car) or f.drv:IsDeath() then
      N.FacReset("copy spawner lost", false)
      return
    end
    if f.t > 3 then
      f.t = 0
      local q, cp = p:GetPos(), f.car:GetPos()
      if (q.x - cp.x) ^ 2 + (q.y - cp.y) ^ 2 > 120 * 120 then f.car:SetPos(V(q.x + 3, q.y, q.z - 40)) end
    end
  end
end

-- A copy's entity is gone (released, with the spawner or the world): forget it.
function N.Forget(key, c)
  if c.id then N.copyIds[c.id] = nil end
  local s = N.host[key]
  if s and s.copy then
    if s.loc then N.byLocal[s.loc] = nil end
    s.loc, s.copy = nil, nil
    N.nx[#N.nx + 1] = key
  end
  N.copies[key] = nil
end

-- Release a copy (one that burns is hidden first and released once the fire is out: the game's fire update keeps a
-- list of burning humans and read a released one, docs/game-api.md 15). Copies carry no map marker.
function N.ReleaseCopy(c)
  if not (c.so and valid(c.so)) then return end
  if c.burnT and M.now - c.burnT < 8 then
    pcall(c.so.ShowModel, c.so, false)
    pcall(c.so.SetPhysState, c.so, 2)
    N.cgrave[#N.cgrave + 1] = { so = c.so, burnT = c.burnT }
    return
  end
  local f = N.cfac
  if f.comp then pcall(f.comp.ReleaseObjectRaw, f.comp, c.so) end
end

function N.DropCopy(key, why)
  local c = N.copies[key]
  if not c then return end
  if c.so then N.ReleaseCopy(c) end
  if N.cfac.busy == key then N.cfac.busy = nil end
  N.Forget(key, c)
  N.dropped = (N.dropped or 0) + 1
  if N.dropped <= 80 then log("copy of " .. N.Who(key) .. " removed: " .. why) end
end

function N.DropAllCopies(why)
  local n = 0
  for key, _ in pairs(N.copies) do
    N.DropCopy(key, why)
    n = n + 1
  end
  return n
end

-- The host NPCs that should have a copy: alive, with a job (AI type not 0), a known profile, on foot, near us, without
-- a partner for LOOSE_WAIT + COPY_WAIT s; nearest first.
function N.CopyWanted(p)
  local q = p:GetPos()
  local list = {}
  for key, s in pairs(N.host) do
    if not s.loc and not N.copies[key] and not s.dead and not N.InVehicle(s) and s.ai ~= 0 and M.now - s.t <= 1
        and N.CopyProfile(s) and s.unpairedSince
        and M.now - s.unpairedSince >= LOOSE_WAIT + COPY_WAIT then
      local d = math.sqrt((s.x - q.x) ^ 2 + (s.y - q.y) ^ 2)
      if d <= COPY_RANGE and math.abs(s.z - q.z) < 30 then list[#list + 1] = { s = s, d = d } end
    end
  end
  table.sort(list, function(a, b) return a.d < b.d end)
  return list
end

-- One copy at a time through the spawner: profile, load, create.
function N.SpawnCopyTick(p)
  local f = N.cfac
  if f.st ~= "ready" then return end
  if f.busy then
    local c = N.copies[f.busy]
    local s = N.host[f.busy]
    if not c or not s or s.loc or s.dead then
      if c and not c.so then N.copies[f.busy] = nil end
      f.busy = nil
      return
    end
    if not f.load:IsDone() then
      if M.now - f.loadT > COPY_LOAD_TIMEOUT then
        N.badProfiles[c.prof] = true
        log("copy profile " .. c.prof .. " did not load in " .. COPY_LOAD_TIMEOUT .. " s: not used again")
        N.copies[f.busy] = nil
        f.busy = nil
        N.FacReset("a profile did not load", true)
      end
      return
    end
    f.busy = nil
    local so = try("copy CreateObject", f.comp.CreateObject, f.comp)
    if not valid(so) then N.copies[c.key] = nil return end
    so:SetPos(V(s.x, s.y, s.z))
    so:SetDir(V(s.hx, s.hy, 0))
    so:Activate()
    pcall(so.SetDemigod, so, false)
    pcall(function() so.invulnerability = false end)
    pcall(so.DespawnImmunity, so, true)
    -- a civilian: it runs no fight of its own (never a brain switch, never a melee order: both crashed the game,
    -- docs/game-api.md 11/15); it does not run from gunfire or cars, it only shows the host's NPC
    pcall(so.SetReactionOnSound, so, false)
    pcall(so.InterruptCivilWander, so)
    pcall(so.InterruptAvoidingCar, so)
    local hp = tonumber(s.hp) or 0
    if hp > 0 then pcall(function() so.health = hp end) end
    c.so, c.id, c.st, c.spawnT = so, idOf(so), "alive", M.now
    c.ent = { h = so, id = c.id }
    c.tx, c.ty, c.tz, c.hx, c.hy, c.lastMove, c.moving = s.x, s.y, s.z, s.hx, s.hy, -1, false
    c.shots, c.wantWeapon, c.weapon = s.shots or 0, nil, nil
    c.noShield = true
    N.copyIds[c.id] = c.key
    N.ents[c.id] = nil
    link(c.key, s, c.id, 0, "spawned copy")
    N.spawned = (N.spawned or 0) + 1
    log(string.format("%s had no partner here: spawned a copy (our %d, profile %s, AI type here %s, " ..
      "weapon %s, #%d)", N.Who(c.key), c.id, c.prof, tostring(aiOf(so)), tostring(s.weapon), N.spawned))
    return
  end
  local n = 0
  for _, _ in pairs(N.copies) do n = n + 1 end
  if n >= MAX_COPIES then return end
  local w = N.CopyWanted(p)[1]
  if not w then return end
  local s = w.s
  local prof, fallback = N.CopyProfile(s)
  if not prof then return end
  if fallback then
    N.fallbacks = (N.fallbacks or 0) + 1
    if N.fallbacks <= 40 then
      log(string.format("%s has no spawn profile (AI type %s): its copy gets %s", N.Who(s.key), tostring(s.ai),
        fallback))
    end
  end
  local c = { key = s.key, st = "loading", prof = prof }
  local ok = pcall(f.comp.SetSpawnProfile, f.comp, prof)
  if not ok then N.badProfiles[prof] = true return end
  N.copies[s.key] = c
  f.load, f.loadT, f.busy = f.comp:GetSpawnProfileLoadSyncObject(), M.now, s.key
end

-- A copy's pictures (its walk comes from the C++ side, N.DriveAll): its weapon and its shots (player_sync.lua M.Combat)
-- fired along its heading, and past us when it faces us: a copy's bullet is only a picture (the host's NPC shot at our
-- puppet there and that hit reaches us). Also for a driven paired NPC (N.Pictures).
function N.Picture(c, s, so, p)
  c.hx, c.hy = s.hx, s.hy
  c.wantWeapon = (s.weapon and s.weapon ~= "-") and s.weapon or ""
  c.shots = s.shots or 0
  -- the profile's own loadout may take its gun back out (its AI picked its pistol): the host's weapon again
  if c.wantWeapon ~= "" and M.now - (c.wpT or 0) > 2 then
    c.wpT = M.now
    local good, sel = pcall(so.InventoryGetSelected, so)
    if good and tostring(sel) ~= c.wantWeapon then c.weapon = nil end
  end
  local q = so:GetPos()
  local ax, ay, az = c.hx, c.hy, 0
  local me = p:GetPos()
  local mx, my, mz = me.x - q.x, me.y - q.y, me.z - q.z
  local ml = math.sqrt(mx * mx + my * my)
  if ml > 0.5 and (mx * c.hx + my * c.hy) / ml > 0.8 then
    -- it faces us: aim past us (1.6 m beside and 2 m above), so its bullets fly by but do not hit us
    local sx, sy = -my / ml, mx / ml
    local tx, ty, tz = mx + sx * 1.6, my + sy * 1.6, mz + 2.2
    local l = math.sqrt(tx * tx + ty * ty + tz * tz)
    ax, ay, az = tx / l, ty / l, tz / l
  end
  c.ax, c.ay, c.az, c.aim, c.crouch, c.reload = ax, ay, az, false, false, false
  if M.Weapon then M.Weapon(c) end
  if M.Combat then M.Combat(c) end
end

function N.DriveCopy(c, s, p)
  local so = c.so
  if so:IsDeath() then
    c.deadT = c.deadT or M.now
    if M.now - c.deadT > COPY_DEAD_KEEP then N.DropCopy(c.key, "dead for " .. COPY_DEAD_KEEP .. " s") end
    return
  end
  c.tx, c.ty, c.tz = s.x, s.y, s.z
  N.Picture(c, s, so, p)
end

function N.CopiesTick(p)
  local f = N.cfac
  local want = M.now - (M.inWorldAt or M.now) > COPY_AFTER_LOAD and not M.cutscene
  -- copies go during a cutscene (puppets are hidden then too) and come back afterwards
  if M.cutscene and next(N.copies) then N.DropAllCopies("a cutscene plays") end
  local any = false
  for key, c in pairs(N.copies) do
    any = true
    local s = N.host[key]
    if c.st == "alive" then
      if not (c.so and valid(c.so)) then
        N.Forget(key, c)
      elseif not s or M.now - s.t > RELEASE then
        N.DropCopy(key, owner() .. " no longer sends it (out of range or gone)")
      elseif N.InVehicle(s) then
        N.DropCopy(key, "it got into a car on " .. owner() .. "'s game" .. (s.car and "" or " (it moves too fast)"))
      elseif s.loc ~= c.id then
        N.DropCopy(key, "paired otherwise")
      else
        local q, me = c.so:GetPos(), p:GetPos()
        if (q.x - me.x) ^ 2 + (q.y - me.y) ^ 2 > (COPY_RANGE + 20) ^ 2 and not c.so:IsDeath() then
          N.DropCopy(key, "far from us")
        elseif M.now - s.t <= STALE then
          local good, err = pcall(N.DriveCopy, c, s, p)
          if not good then
            c.errs = (c.errs or 0) + 1
            if c.errs <= 5 then log("copy " .. key .. " drive error: " .. tostring(err)) end
          end
        end
      end
    end
  end
  if #N.cgrave > 0 then
    local keep = {}
    for _, g in ipairs(N.cgrave) do
      if not valid(g.so) then
        -- gone already
      elseif M.now - g.burnT > 8 then
        if f.comp then pcall(f.comp.ReleaseObjectRaw, f.comp, g.so) end
      else
        keep[#keep + 1] = g
      end
    end
    N.cgrave = keep
  end
  local need = want and (any or #N.CopyWanted(p) > 0)
  if f.st ~= "none" or need then N.FacTick(M.dt or 0.033, need, p) end
  if want then N.SpawnCopyTick(p) end
end

-- ---- driving (both roles) ------------------------------------------------------------------------------------------
-- Every replica is driven from its authority's states: the C++ side (src/game/npc_replica.h ReplicaSet) interpolates
-- them, runs a locomotion controller + correction per replica and sends one command per tick (R); the replica walks
-- with the game's own locomotion (MoveVec: AI path following with the walk / run / sprint animation) at the speed the
-- controller asks for (SetMovementSpeedMult, docs/game-api.md 4, UNVERIFIED on NPCs), and is nudged or put back only
-- by the correction. Where it is afterwards goes back (NR), so the next command knows. Not driven (not reported): a
-- dead or burning one, one in a car or whose authority NPC is in a car, during a cutscene, while the host's game is
-- paused, in our first 3 s in the world.
function N.Drivable(s, so)
  if so:IsDeath() or s.dead or N.InVehicle(s) or M.cutscene or hostQuiet() then return false end
  if not M.inWorldAt or M.now - M.inWorldAt < 3 or M.now - s.t > STALE then return false end
  local good, car = pcall(so.GetOwner, so)
  return not (good and car)
end

function N.SpeedMult(r, so, m)
  if r.noMult or math.abs(m - (r.mult or 1)) < 0.02 then return end
  local good, err = pcall(so.SetMovementSpeedMult, so, m)
  if not good then
    r.noMult = true
    N.multFails = (N.multFails or 0) + 1
    if N.multFails <= 5 then log("SetMovementSpeedMult failed on an NPC (" .. tostring(err) .. "): its own speed") end
    return
  end
  r.mult = m
end

-- "R key kind gait mult reissue mx my mz fix nx ny turn fx fy tx ty tz hx hy refuse" (fields after the key)
function N.ApplyDrive(r, so, rec, aiming)
  if r.fix == 2 then
    so:SetPos(V(r.tx, r.ty, r.tz))
    if not aiming then so:SetDir(V(r.hx, r.hy, 0)) end
    N.snaps = (N.snaps or 0) + 1
  elseif r.fix == 1 then
    local q = so:GetPos()
    so:SetPos(V(q.x + r.nx, q.y + r.ny, q.z))
  end
  if r.reissue then
    if r.gait == 0 then  -- stop: walk the last bit to its spot and stand there
      N.SpeedMult(rec, so, 1)
      so:MoveVec(V(r.tx, r.ty, r.tz), "WALK")
    else
      N.SpeedMult(rec, so, r.mult)
      so:MoveVec(V(r.mx, r.my, r.mz), GAITS[r.gait] or "RUN")
    end
  end
  if r.turn and not aiming then so:SetDir(V(r.fx, r.fy, 0)) end
end

function N.ParseDrive(f)
  local n = function(i) return tonumber(f[i]) or 0 end
  return { kind = n(3), gait = n(4), mult = n(5), reissue = f[6] == "1", mx = n(7), my = n(8), mz = n(9), fix = n(10),
    nx = n(11), ny = n(12), turn = f[13] == "1", fx = n(14), fy = n(15), tx = n(16), ty = n(17), tz = n(18),
    hx = n(19), hy = n(20), refuse = f[21] == "1" }
end

function N.DriveAll(p, ev)
  N.rec = N.rec or {}
  for key, s in pairs(N.host) do
    local so, kind, c
    if s.loc and s.copy then
      c = N.copies[key]
      so = c and c.st == "alive" and c.so or nil
      kind = 2
    elseif s.loc then
      local e = N.ents[s.loc]
      so = e and e.h
      kind = 1
    end
    if so and valid(so) and N.Drivable(s, so) then
      local r = N.cmd[key]
      local rec = N.rec[key] or {}
      N.rec[key] = rec
      if r and r.refuse and kind == 1 then
        N.Refuse(s, key)
        so = nil
      elseif r then
        local good, err = pcall(N.ApplyDrive, r, so, rec, c and c.cb and c.cb.aim)
        if not good then
          N.driveErrs = (N.driveErrs or 0) + 1
          if N.driveErrs <= 10 then log("drive error on " .. N.Who(key) .. ": " .. tostring(err)) end
        end
        N.drives = (N.drives or 0) + 1
      end
      if so then
        local q, d = so:GetPos(), so:GetDir()
        ev[#ev + 1] = string.format("NR %.0f %.3f %.3f %.3f %.4f %.4f %d 0", key, q.x, q.y, q.z, d.x, d.y, kind)
      end
      -- a paired NPC with a gun shows the authority's shots as pictures too
      if so and kind == 1 and s.ai ~= 0 and (s.weapon or "-") ~= "-" then
        s.pic = s.pic or { id = "npc " .. tostring(s.loc), noShield = true }
        s.pic.so = so
        pcall(N.Picture, s.pic, s, so, p)
      end
    end
  end
  N.cmd = {}
  for _, key in ipairs(N.nx) do ev[#ev + 1] = string.format("NX %.0f", key) end
  N.nx = {}
end

-- ---- guest-owned NPCs ----------------------------------------------------------------------------------------------
-- Guest: every round (10 Hz) our NPCs with a job near us go to the C++ side (GO lines), which decides which are our
-- own (unpaired for 5 s: no host NPC is them) and sends those to the host. None in the first COPY_AFTER_LOAD s after
-- our world loaded (the host's states and our pairing need that long).
local OWN_RANGE = 100       -- m from our player
local MAX_OWN_REPORT = 64   -- NPCs per round, nearest first

-- The crews of traffic_sync.lua's copies of the host's traffic and police cars are copies of host NPCs, never our
-- own: reported (police crews have a job, AI type 4) they were mirrored onto the host next to his own drivers, and a
-- dead one could kill a host cop of the same archetype nearby through a claim. Every human seen in a copy's seat is
-- remembered for this world (N.tcrew), so one that got out (its host car lost its driver) stays excluded.
function N.TrafficCrews()
  local T = M.Traffic
  for _, c in pairs(T and T.copies or {}) do
    if c.ent and valid(c.ent) then
      for s = 0, 3 do
        local good, h = pcall(c.ent.GetSeatOwner, c.ent, s)
        if good and valid(h) then
          local id = idOf(h)
          if id and not N.tcrew[id] then
            N.tcrew[id] = true
            N.tcrewN = (N.tcrewN or 0) + 1
            if N.tcrewN <= 20 then log("human " .. id .. " is a traffic copy's crew: never one of our own NPCs") end
          end
        end
      end
    end
  end
end

function N.OwnRound(p, ev)
  if M.now - (M.inWorldAt or M.now) <= COPY_AFTER_LOAD or M.cutscene then return end
  local q0 = p:GetPos()
  local list = {}
  pcall(N.TrafficCrews)
  for id, e in pairs(N.ents) do
    local h = e.h
    if not N.copyIds[id] and not N.tcrew[id] and not N.hidden[id] and not pedOwned(id) and valid(h) and active(h) then
      local q = h:GetPos()
      local d2 = (q.x - q0.x) ^ 2 + (q.y - q0.y) ^ 2
      if d2 <= OWN_RANGE * OWN_RANGE and math.abs(q.z - q0.z) < 30 then
        if not e.aiT or M.now - e.aiT > 2 then e.ai, e.aiT = aiOf(h), M.now end
        if e.ai ~= 0 then list[#list + 1] = { e = e, d = math.sqrt(d2), q = q } end
      end
    end
  end
  table.sort(list, function(a, b) return a.d < b.d end)
  ev[#ev + 1] = "GOR"
  for i = 1, math.min(#list, MAX_OWN_REPORT) do
    local e, q, h = list[i].e, list[i].q, list[i].e.h
    local d = h:GetDir()
    local dead = h:IsDeath()
    if not e.tagT or M.now - e.tagT > 2 then e.tag, e.tagT = tagOf(h), M.now end
    local prof = "0"
    if not dead then
      local good, pr = pcall(N.ProfileOf, e)
      prof = good and pr or "0"
    end
    ev[#ev + 1] = string.format("GO %.1f %.0f %d %.2f %.2f %.2f %.3f %.3f %d %d %d %.0f %s %s %d", list[i].d,
      N.byLocal[e.id] or 0, e.id, q.x, q.y, q.z, d.x, d.y, dead and 0 or healthOf(h),
      (dead and 1 or 0) + (h:GetOwner() and 2 or 0), e.ai, e.tag or 0, weaponOf(h), prof, N.shots[e.id] or 0)
  end
end

-- Guest: the host claims our own NPC `key` as its NPC `hostKey` (its game spawned it: the host got there): paired
-- (any spawned copy of that host NPC goes); the C++ side sees the pairing in the next round and hands it back.
function N.Claimed(key, hostKey, dead)
  local e, s = N.ents[key], N.host[hostKey]
  local why
  if not (e and valid(e.h)) then
    why = "our NPC is gone"
  elseif N.byLocal[key] and N.byLocal[key] ~= hostKey then
    why = "ours is paired with host NPC " .. tostring(N.byLocal[key])
  elseif not s then
    why = "no state of that host NPC here yet"
  elseif s.loc and not s.copy and s.loc ~= key then
    why = "that host NPC is paired with our " .. tostring(s.loc)
  end
  if why then
    log(string.format("the host claims our own NPC %d as its NPC %d: refused (%s)", key, hostKey, why))
    if not dead then N.hitq[#N.hitq + 1] = "GR " .. key .. " " .. hostKey end
    return
  end
  N.owned[key] = nil
  if s.loc == key then return end
  if s.copy then N.DropCopy(hostKey, "the host claimed our own NPC " .. key .. " for it") end
  local q = e.h:GetPos()
  local d = math.sqrt((q.x - s.x) ^ 2 + (q.y - s.y) ^ 2 + (q.z - s.z) ^ 2)
  link(hostKey, s, key, d, dead and "claimed by the host, ours is dead" or "claimed by the host")
  N.claimsDone = (N.claimsDone or 0) + 1
end

-- Host: a state of a guest's own NPC (GN): kept like a guest keeps host NPC states (N.host, keyed by its mirror id),
-- so the spawned-copy machinery above shows it.
function N.MirrorState(mid, from, f)
  local s = N.host[mid]
  if not s then
    s = { key = mid, from = from, gkey = tonumber(f[4]), unpairedSince = M.now - 100 }  -- the guest waited already
    N.host[mid] = s
    N.mirrorsSeen = (N.mirrorsSeen or 0) + 1
    if N.mirrorsSeen <= 60 then
      log(string.format("guest %s's own NPC %s (no NPC of ours is it): mirror %d, AI type %s, profile %s", from,
        tostring(f[4]), mid, tostring(f[12]), tostring(f[15])))
    end
  end
  return s
end

-- Host: a mirror ends (GX): its copy goes; handed back (reason 1): our own NPC hostKey is it now and takes the
-- guest's health (never more than ours).
function N.MirrorEnd(mid, reason, hostKey, hp)
  local s = N.host[mid]
  if N.copies[mid] then
    N.DropCopy(mid, reason == 1 and ("our own NPC " .. hostKey .. " is it now") or (owner() .. "'s NPC went"))
  end
  N.host[mid] = nil
  if reason == 1 and hostKey and hostKey > 0 then
    local e = N.ents[hostKey]
    if e and valid(e.h) and not e.h:IsDeath() and hp and hp > 0 then
      local here = tonumber(e.h.health) or 0
      if here > hp + 0.5 then pcall(function() e.h.health = hp end) end
      log(string.format("guest %s handed his NPC %s back: our NPC %d (health %.0f -> %d)", s and tostring(s.from) or "?",
        s and tostring(s.gkey) or "?", hostKey, here, hp))
    end
  end
end

function N.KillNative(hostKey, mid)
  local e = N.ents[hostKey]
  if e and valid(e.h) and not e.h:IsDeath() then
    N.Kill(e.h)
    N.lateKills = (N.lateKills or 0) + 1
    log(string.format("our NPC %d spawned where a guest had killed his (mirror %d): it dies (#%d)", hostKey, mid,
      N.lateKills))
  end
end

-- Host: keep the mirrors' copies in line (health down, deaths) and spawn/drive them (N.CopiesTick).
function N.MirrorTick(p)
  local live, shown = 0, 0
  for mid, s in pairs(N.host) do
    if s.dead then
      s.deadT = s.deadT or M.now
      if M.now - s.deadT < COPY_DEAD_KEEP then s.t = M.now end  -- its copy lies there (no states come any more)
    end
    if M.now - s.t > 30 then
      N.host[mid] = nil
    elseif M.now - s.t <= STALE then
      live = live + 1
      local c = N.copies[mid]
      local e = s.loc and c and c.ent
      if s.loc and not (e and valid(e.h)) then
        N.byLocal[s.loc] = nil
        s.loc, e = nil, nil
      end
      if e then
        shown = shown + 1
        local good, err = pcall(N.Enforce, s, e, p)
        if not good then log("mirror enforce error: " .. tostring(err)) end
      end
    end
  end
  N.mirrorLive, N.mirrorShown = live, shown
  local good, err = pcall(N.CopiesTick, p)
  if not good then log("mirror copies error: " .. tostring(err)) end
end

-- Both: our spawned copies for the C++ side (never reported, never sent: no feedback loops).
function N.CopyList(ev)
  local ids = {}
  for id, _ in pairs(N.copyIds) do ids[#ids + 1] = tostring(id) end
  ev[#ev + 1] = "GCS " .. table.concat(ids, " ")
end

-- ---- update -----------------------------------------------------------------------------------------------------------
-- Our fire on NPCs of the other game, summed: one hit line per NPC every FIRE_SUM s.
function N.FireFlush()
  for key, f in pairs(N.fireq) do
    if M.now - f.t >= FIRE_SUM then
      N.fireq[key] = nil
      if f.amount > 0 then
        N.hitq[#N.hitq + 1] = N.HitLine(key, f.amount, 8, 0, f.weapon, false)
        N.hitsOut = (N.hitsOut or 0) + 1
        if N.hitsOut <= 30 or N.hitsOut % 50 == 0 then
          log(string.format("our fire burnt %s: %.1f (hit #%d)", N.Who(key), f.amount, N.hitsOut))
        end
      end
    end
  end
end

function N.Update(dt, cmds)
  pcall(N.FireFlush)
  local ev = N.hitq
  N.hitq = {}
  local active, host = false, false
  for line in string.gmatch(cmds, "[^\n]+") do
    local f = {}
    for w in string.gmatch(line, "%S+") do f[#f + 1] = w end
    if f[1] == "S" then
      active, host = f[2] == "1", f[3] == "1"
      N.isHost = host
    elseif f[1] == "R" then
      local key = tonumber(f[2])
      if key then N.cmd[key] = N.ParseDrive(f) end
    elseif f[1] == "N" or f[1] == "GN" then
      local s
      local vxIn, vyIn, gaitIn, ageIn
      if f[1] == "N" then
        local key = tonumber(f[2])
        s = N.host[key] or { key = key }
        N.host[key] = s
        -- "N key ... shots vx vy gait age": the velocity of its replica target (the C++ side's interpolation)
        vxIn, vyIn, gaitIn, ageIn = tonumber(f[15]), tonumber(f[16]), tonumber(f[17]), tonumber(f[18])
      else
        -- "GN mid from key ...": the same fields as N from f[4] on (shifted by two)
        s = N.MirrorState(tonumber(f[2]), f[3], f)
        table.remove(f, 2)
        table.remove(f, 2)
      end
      local px, py, pt = s.x, s.y, s.t
      s.x, s.y, s.z, s.hx, s.hy = tonumber(f[3]), tonumber(f[4]), tonumber(f[5]), tonumber(f[6]), tonumber(f[7])
      if vxIn and vyIn then
        s.vx, s.vy, s.gait = vxIn, vyIn, gaitIn
        s.spd = math.sqrt(vxIn * vxIn + vyIn * vyIn)
        if s.spd > FAST_NPC then
          if not s.fastT and not s.car then
            N.fastSeen = (N.fastSeen or 0) + 1
            if N.fastSeen <= 20 then
              log(string.format("%s moves at %.0f m/s without its car flag: taken as in a vehicle", N.Who(s.key),
                s.spd))
            end
          end
          s.fastT = M.now
        end
      elseif pt and M.now - pt > 0.03 and M.now - pt < 0.6 then
        -- a mirror (GN, raw states): its velocity from the last two states, smoothed
        local vx, vy = (s.x - px) / (M.now - pt), (s.y - py) / (M.now - pt)
        -- its speed, smoothed (a vehicle's speed too: N.InVehicle; a jump over 80 m/s is a teleport, left out)
        local raw = math.sqrt(vx * vx + vy * vy)
        if raw < 80 then s.spd = 0.6 * (s.spd or raw) + 0.4 * raw end
        if (s.spd or 0) > FAST_NPC then
          if not s.fastT and not s.car then
            N.fastSeen = (N.fastSeen or 0) + 1
            if N.fastSeen <= 20 then
              log(string.format("%s moves at %.0f m/s without its car flag: taken as in a vehicle", N.Who(s.key),
                s.spd))
            end
          end
          s.fastT = M.now
        end
        if vx * vx + vy * vy > 100 then vx, vy = 0, 0 end
        s.vx, s.vy = 0.5 * (s.vx or vx) + 0.5 * vx, 0.5 * (s.vy or vy) + 0.5 * vy
      elseif pt then
        s.vx, s.vy = 0, 0
      end
      s.hp, s.ai, s.tag, s.weapon = tonumber(f[8]), tonumber(f[10]), tonumber(f[11]) or 0, f[12] or "-"
      s.prof, s.shots = f[13] or "0", tonumber(f[14]) or 0
      local flags = tonumber(f[9]) or 0
      s.dead = s.dead or (flags % 2) == 1
      s.car = math.floor(flags / 2) % 2 == 1
      s.t = M.now - math.min(ageIn or 0, 2)
    elseif f[1] == "D" or f[1] == "GD" then
      local s = N.host[tonumber(f[2])]
      if s then s.dead = true end
    elseif f[1] == "GX" then
      local good, err = pcall(N.MirrorEnd, tonumber(f[2]), tonumber(f[3]), tonumber(f[4]), tonumber(f[5]))
      if not good then log("mirror end error: " .. tostring(err)) end
    elseif f[1] == "GK" then
      local good, err = pcall(N.KillNative, tonumber(f[2]), tonumber(f[3]))
      if not good then log("late kill error: " .. tostring(err)) end
    elseif f[1] == "O" then
      N.owned[tonumber(f[2])] = f[3] == "1" or nil
    elseif f[1] == "C" then
      local good, err = pcall(N.Claimed, tonumber(f[2]), tonumber(f[3]), f[4] == "1")
      if not good then log("claim error: " .. tostring(err)) end
    elseif f[1] == "X" then
      -- the host hit his copy of our own NPC, or another guest hit his copy of it (the host relays it to him): replayed
      -- from that player's puppet here (N.HostHit; f[8] the attacker, "0" = the host)
      local by = (f[8] and f[8] ~= "0" and M.pup and M.pup[f[8]]) and f[8] or "0"
      local good, err = pcall(N.HostHit, by, tonumber(f[2]), tonumber(f[3]) or 0, tonumber(f[4]) or 0,
        tonumber(f[5]) or 0, tonumber(f[6]) or 0, f[7] == "1")
      if not good then log("host hit error: " .. tostring(err)) end
    elseif f[1] == "H" then
      local good, err = pcall(N.HostHit, f[2], tonumber(f[3]), tonumber(f[4]) or 0, tonumber(f[5]) or 0,
        tonumber(f[6]) or 0, tonumber(f[7]) or 0, f[8] == "1")
      if not good then log("hit error: " .. tostring(err)) end
    end
  end
  local p = M.Player and M.Player()
  local key = p and (tostring(game.game:GetLoadedSaveId()) .. "|" .. tostring(M.inWorldAt)) or nil
  if key ~= N.worldKey then  -- another world: nothing carries over
    N.worldKey = key
    local n = 0
    for _, _ in pairs(N.copies) do n = n + 1 end
    if n > 0 then log(n .. " spawned copies went with the old world") end
    N.ents, N.host, N.byLocal, N.sent, N.replays, N.lethal = {}, {}, {}, {}, {}, {}
    N.copies, N.copyIds, N.cgrave, N.shots, N.spawners = {}, {}, {}, {}, {}
    N.owned, N.tcrew, N.fireq, N.cmd, N.hidden, N.nx, N.rec = {}, {}, {}, {}, {}, {}, {}
    N.cfac = { st = "none", t = 0 }  -- its car went with the world
    N.shotWatch, N.spT = nil, nil
    N.scanT = -10
  end
  M.dt = dt
  if not active and p and (next(N.copies) or N.cfac.st ~= "none") then  -- the session ended: our copies go
    N.DropAllCopies("the session ended")
    N.FacReset("session ended", true)
  end
  if not (active and p) then return table.concat(ev, "\n") end
  if M.now - (N.scanT or -10) >= SCAN_INTERVAL then
    N.scanT = M.now
    local good, err = pcall(N.Scan)
    if not good then log("scan error: " .. tostring(err)) end
  end
  pcall(N.WatchShots)  -- (on guests too: the shots of our own NPCs go to the host's copies of them)
  if M.now - (N.viewT or -10) >= SEND_INTERVAL then
    N.viewT = M.now
    pcall(N.Views, ev)
  end
  if host then
    if M.now - (N.sendT or -10) >= SEND_INTERVAL then
      N.sendT = M.now
      local good, err = pcall(N.HostSend, p, ev)
      if not good then log("send error: " .. tostring(err)) end
    end
    pcall(N.ReplayTick)
    local good, err = pcall(N.MirrorTick, p)
    if not good then log("mirror error: " .. tostring(err)) end
    good, err = pcall(N.DriveAll, p, ev)
    if not good then log("drive error: " .. tostring(err)) end
    N.status = string.format("NPCs: sending %d, guest hits %d, guest kills %d; guests' own %d (copies %d)",
      N.hostCount or 0, N.hitsIn or 0, N.guestKills or 0, N.mirrorLive or 0, N.mirrorShown or 0)
  else
    local good, err = pcall(N.GuestTick, p)
    if not good then log("guest error: " .. tostring(err)) end
    good, err = pcall(N.DriveAll, p, ev)
    if not good then log("drive error: " .. tostring(err)) end
    pcall(N.ReplayTick)  -- the host's hits on our own NPCs
    if M.now - (N.ownT or -10) >= SEND_INTERVAL then
      N.ownT = M.now
      good, err = pcall(N.OwnRound, p, ev)
      if not good then log("own round error: " .. tostring(err)) end
    end
    local no = 0
    for _, _ in pairs(N.owned) do no = no + 1 end
    N.status = (N.status or "") .. string.format(", our own %d", no)
  end
  if M.now - (N.cpT or -10) >= 0.5 then
    N.cpT = M.now
    N.CopyList(ev)
  end
  if M.now - (N.stT or -10) >= 1 then
    N.stT = M.now
    ev[#ev + 1] = "ST " .. (N.status or "")
  end
  if mc_trace and M.now - (N.trT or -10) >= 0.5 then
    N.trT = M.now
    pcall(N.Trace, host)
  end
  return table.concat(ev, "\n")
end

-- Sync trace (docs/sync-trace.md), twice a second: host "NHS" (how many NPCs it sends); guest "NP" for every host
-- NPC it has a state of (where the host's is, where ours is, how far apart) and "NGS" (totals).
function N.Trace(host)
  if host then
    mc_trace("NHS", string.format("sending=%d hits=%d kills=%d mirrors=%d shown=%d lateKills=%d", N.hostCount or 0,
      N.hitsIn or 0, N.guestKills or 0, N.mirrorLive or 0, N.mirrorShown or 0, N.lateKills or 0))
    for mid, s in pairs(N.host) do
      if M.now - s.t <= STALE then
        local c = N.copies[mid]
        local h = c and c.so
        local q = h and valid(h) and h:GetPos()
        mc_trace("NM", string.format("mid=%d from=%s key=%s loc=%s st=%s ai=%s guest=%.2f,%.2f,%.2f our=%s d=%.2f "
          .. "hp=%s/%s dead=%d/%d age=%.2f", mid, tostring(s.from), tostring(s.gkey), tostring(s.loc),
          c and tostring(c.st) or "-", tostring(s.ai), s.x, s.y, s.z,
          q and string.format("%.2f,%.2f,%.2f", q.x, q.y, q.z) or "-",
          q and math.sqrt((q.x - s.x) ^ 2 + (q.y - s.y) ^ 2) or -1, tostring(s.hp),
          q and string.format("%.0f", tonumber(h.health) or -1) or "-", s.dead and 1 or 0,
          (q and h:IsDeath()) and 1 or 0, M.now - s.t))
      end
    end
    return
  end
  local own = 0
  for id, _ in pairs(N.owned) do
    own = own + 1
    local e = N.ents[id]
    local h = e and e.h
    if h and valid(h) then
      local q = h:GetPos()
      mc_trace("NO", string.format("key=%d p=%.2f,%.2f,%.2f hp=%.0f dead=%d ai=%d", id, q.x, q.y, q.z,
        tonumber(h.health) or -1, h:IsDeath() and 1 or 0, aiOf(h)))
    end
  end
  local matched, live, sumD, maxD = 0, 0, 0, 0
  for key, s in pairs(N.host) do
    if M.now - s.t <= STALE then
      live = live + 1
      local e = s.loc and (N.ents[s.loc] or (s.copy and N.copies[key] and N.copies[key].ent))
      local h = e and e.h
      local q = h and valid(h) and h:GetPos()
      local d = q and math.sqrt((q.x - s.x) ^ 2 + (q.y - s.y) ^ 2) or -1
      if d >= 0 then
        matched = matched + 1
        sumD, maxD = sumD + d, math.max(maxD, d)
      end
      mc_trace("NP", string.format("key=%.0f loc=%s copy=%d ai=%s car=%d spd=%.1f host=%.2f,%.2f,%.2f our=%s d=%.2f "
        .. "hp=%s/%s dead=%d/%d age=%.2f", key, tostring(s.loc or (s.hid and ("hidden:" .. s.hid))), s.copy and 1 or 0,
        tostring(s.ai),
        s.car and 1 or (N.InVehicle(s) and 2 or 0), s.spd or 0, s.x, s.y,
        s.z, q and string.format("%.2f,%.2f,%.2f", q.x, q.y, q.z) or "-", d, tostring(s.hp),
        h and valid(h) and string.format("%.0f", tonumber(h.health) or -1) or "-", s.dead and 1 or 0,
        (h and valid(h) and h:IsDeath()) and 1 or 0, M.now - s.t))
    end
  end
  local nc = 0
  for _, c in pairs(N.copies) do if c.st == "alive" then nc = nc + 1 end end
  mc_trace("NGS", string.format("live=%d matched=%d copies=%d avg=%.2f max=%.2f kills=%d snaps=%d own=%d claims=%d "
    .. "unpairs=%d refused=%d fallbacks=%d frozen=%d drives=%d", live, matched, nc, matched > 0 and sumD / matched or 0,
    maxD, N.kills or 0, N.snaps or 0, own, N.claimsDone or 0, N.unpairs or 0, N.refused or 0, N.fallbacks or 0,
    N.frozen or 0, N.drives or 0))
end

return N.version
