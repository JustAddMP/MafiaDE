-- MafiaCoop NPC glue (B3 NPC mirroring, enemies reacting to guests). Runs in the game's Lua state on GameMainThread
-- after player_sync.lua, called once per script tick by src/game/npc_sync.cpp (MafiaCoop.Npc.Update). Embedded into
-- the dll; a copy at <dll dir>\npc_sync.lua overrides it.
--
-- Every game runs the chapter's own mission scripts at the host's checkpoint, so the mission NPCs exist on every
-- game, spawned by the same scripts at the same places. The host is authoritative for them: it sends the state of the
-- NPCs near any player (position, heading, health, dead, AI type, weapon); each guest pairs every host NPC with its
-- own copy (same spot when first seen, docs/game-api.md 15) and keeps that copy in line: dead when the host's is dead,
-- hostile when the host's is, pulled back when it wanders off, never killed by the guest's own shots (those are
-- sent to the host, whose NPC takes them; its death comes back). The host's NPCs react to a guest's shots because the
-- host replays them as real shots from the guest's puppet.
--
-- Input (cmds, one line per command):
--   S active host localId                        session running, we host, our player id
--   N key x y z hx hy health flags ai weapon     guest: a host NPC (flags: 1 dead, 2 in a car)
--   D key                                        guest: that host NPC died
--   H from key amount dtype zone weapon          host: guest `from` hit host NPC `key` on his screen
-- Output (events, one per line):
--   NS key x y z hx hy health flags ai weapon    host: NPC state (10 Hz, nearest first, at most MAX_SEND)
--   ND key                                       host: NPC died
--   NH key amount dtype zone weapon              guest: we hit (our copy of) host NPC key
--   ST text                                      status for the overlay (once a second)
MafiaCoop = MafiaCoop or {}
local M = MafiaCoop
M.Npc = M.Npc or {}
local N = M.Npc
N.version = 1

local SCAN_INTERVAL = 1.0   -- s between scans of the entity table (mc_entity_ids)
local SEND_INTERVAL = 0.1   -- s between host state rounds (10 Hz; ~10 KB/s for 48 NPCs)
local RANGE = 120           -- m from the host or a puppet: NPCs sent
local MAX_SEND = 48         -- NPCs per round
local MATCH_DIST = 1.5      -- m: a guest NPC this close to a host NPC (when first paired) is its copy
local PULL_DIST = 6         -- m: a copy farther than this from the host's NPC for PULL_TIME is put back
local PULL_TIME = 1.5
local PULL_DIST_FIGHT = 12  -- m: the same for hostile NPCs (they run and take cover on their own in a fight)
local STALE = 3             -- s without a state: the host NPC is out of range (pairing kept, nothing enforced)
local REPLAY_WAIT = 0.35    -- s a replayed guest bullet has to land on the host's NPC before DoDamage is used
local COPY_HEALTH = 5000    -- a guest copy's health while the host's NPC lives (our shots never kill it here)

local function log(s) if mc_log then mc_log("npc: " .. s) end end
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

N.ents = N.ents or {}      -- local humans: id -> { h = wrapper, id }
N.host = N.host or {}      -- guest: host key -> { x, y, z, hx, hy, hp, dead, car, ai, weapon, t, local = id }
N.byLocal = N.byLocal or {}  -- guest: local id -> host key
N.sent = N.sent or {}      -- host: key -> last dead flag sent
N.replays = N.replays or {}
N.hitq = N.hitq or {}      -- guest: hits from the DAMAGE callbacks, sent with the next update

-- Humans that are ours, not mission NPCs: the puppets and the puppet factory's driver.
local function ours(id)
  for _, pp in pairs(M.pup or {}) do
    if pp.so and valid(pp.so) and idOf(pp.so) == id then return true end
  end
  local f = M.fac
  if f and f.drv and valid(f.drv) and idOf(f.drv) == id then return true end
  return false
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
      if key and N.host[key] then N.host[key].loc = nil end
      N.byLocal[id] = nil
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

-- ---- host -------------------------------------------------------------------------------------------------------------
-- Viewpoints: the host and every puppet (the guests).
local function viewpoints(p)
  local v = {}
  local pos = p:GetPos()
  v[1] = { pos.x, pos.y }
  for _, pp in pairs(M.pup or {}) do
    if pp.tx then v[#v + 1] = { pp.tx, pp.ty } end
  end
  return v
end

function N.HostSend(p, ev)
  local vps = viewpoints(p)
  local list = {}
  for id, e in pairs(N.ents) do
    local h = e.h
    if valid(h) then
      local q = h:GetPos()
      local best
      for _, v in ipairs(vps) do
        local d = (q.x - v[1]) ^ 2 + (q.y - v[2]) ^ 2
        if not best or d < best then best = d end
      end
      if best and best <= RANGE * RANGE then list[#list + 1] = { e = e, d = best, q = q } end
    end
  end
  table.sort(list, function(a, b) return a.d < b.d end)
  for i = 1, math.min(#list, MAX_SEND) do
    local e, q, h = list[i].e, list[i].q, list[i].e.h
    local d = h:GetDir()
    local dead = h:IsDeath()
    local flags = (dead and 1 or 0) + (h:GetOwner() and 2 or 0)
    local hp = math.max(0, math.min(255, math.floor((tonumber(h.health) or 0) + 0.5)))
    ev[#ev + 1] = string.format("NS %d %.2f %.2f %.2f %.3f %.3f %d %d %d %s", e.id, q.x, q.y, q.z, d.x, d.y, hp, flags,
      aiOf(h), weaponOf(h))
    if dead and not N.sent[e.id] then
      N.sent[e.id] = true
      ev[#ev + 1] = "ND " .. e.id
      log("host NPC " .. e.id .. " died")
    elseif not dead then
      N.sent[e.id] = nil
    end
  end
  N.hostCount = math.min(#list, MAX_SEND)
end

-- A guest's hit on our NPC: a bullet is replayed as a real shot from his puppet (the NPC's own hit reaction, damage
-- and AI response: it turns on the puppet); anything else, or a replay that did not land, is DoDamage.
function N.HostHit(from, key, amount, dtype, zone, weapon)
  local e = N.ents[key]
  if not e or not valid(e.h) or e.h:IsDeath() then return end
  local h = e.h
  local pp = M.pup and M.pup[from]
  N.hitsIn = (N.hitsIn or 0) + 1
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
        if N.hitsIn <= 30 then log(string.format("guest %s shot NPC %d: replayed from his puppet", from, key)) end
        N.Engage(h, pp)
        return
      end
    end
  end
  pcall(h.DoDamage, h, amount)
  if N.hitsIn <= 30 then log(string.format("guest %s hit NPC %d: %.1f type %d, DoDamage", from, key, amount, dtype)) end
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
  if M.isHost then
    local enemy = tonumber(tostring(m:GetEnemyGUID()):match("(%d+)%s*$"))
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
  local key = N.byLocal[receiverId]
  if not key then return false end
  local enemy = tostring(m:GetEnemyGUID())
  if not (M.IsLocalAttacker and M.IsLocalAttacker(enemy)) then return true end  -- our copy, hit by someone else
  local amount = tonumber(m:GetDamageAmount()) or 0
  local dtype, zone = tonumber(m:GetDamageType()) or 0, tonumber(m:GetHitZone()) or 0
  local weapon = tonumber(m:GetWeaponID()) or 0
  if amount > 0 then
    N.hitq[#N.hitq + 1] = string.format("NH %d %.2f %d %d %.0f", key, amount, dtype, zone, weapon)
    N.hitsOut = (N.hitsOut or 0) + 1
    if N.hitsOut <= 30 or N.hitsOut % 50 == 0 then
      log(string.format("we hit host NPC %d (our %d): %.1f type %d zone %d (hit #%d)", key, receiverId, amount, dtype,
        zone, N.hitsOut))
    end
  end
  return true
end

-- Guest: host NPC `key` hit our puppet on the host's game. True when our copy of it is alive and hostile here: then it
-- fights us on our own game and its hits land here already (player_sync.lua M.ApplyHit skips the relayed one).
function N.FightsUs(key)
  local s = key and N.host[key]
  local e = s and s.loc and N.ents[s.loc]
  if not (e and valid(e.h)) or e.h:IsDeath() then return false end
  return aiOf(e.h) == 2
end

function N.ReplayTick()
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
-- Pair host NPC `k` with the nearest free local human within MATCH_DIST.
local function pair(key, s)
  local best, bestD
  for id, e in pairs(N.ents) do
    if not N.byLocal[id] and valid(e.h) then
      local q = e.h:GetPos()
      local d = (q.x - s.x) ^ 2 + (q.y - s.y) ^ 2 + (q.z - s.z) ^ 2
      if d <= MATCH_DIST * MATCH_DIST and (not bestD or d < bestD) then best, bestD = id, d end
    end
  end
  if best then
    s.loc = best
    N.byLocal[best] = key
    local h = N.ents[best].h
    pcall(function() h.health = COPY_HEALTH end)
    N.paired = (N.paired or 0) + 1
    if N.paired <= 60 then log(string.format("host NPC %d is our %d (%.2f m apart)", key, best, math.sqrt(bestD))) end
  end
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
        if s.kills == 1 then log("host NPC " .. s.key .. " is dead: our " .. e.id .. " dies") end
      else
        -- some civilians take no damage from scripts (or anyone's bullets) on this game: out of sight instead
        s.hidden = true
        pcall(h.ShowModel, h, false)
        pcall(h.SetPhysState, h, 2)
        log("our " .. e.id .. " does not die: hidden like the host's dead NPC " .. s.key)
      end
    end
    return
  end
  if h:IsDeath() then
    if not s.deadHere then
      s.deadHere = true
      log("our copy " .. e.id .. " of host NPC " .. s.key .. " died here while the host's lives")
    end
    return
  end
  -- never killed by our own shots: the host decides (its death comes back)
  if (tonumber(h.health) or COPY_HEALTH) < COPY_HEALTH * 0.5 then pcall(function() h.health = COPY_HEALTH end) end
  -- hostile on the host but not here: only noted (our own mission flow makes its enemies hostile itself; switching
  -- a civilian's brain from a script crashed the game in tests)
  if s.ai == 2 and aiOf(h) ~= 2 and not s.notedHostile then
    s.notedHostile = true
    log("host NPC " .. s.key .. " is hostile, our " .. e.id .. " is not (yet)")
  end
  -- pulled back when it walked off (not in cars: they drive their own way)
  -- (only NPCs with a job: enemies, allies, police, scripted ones; plain civilians wander their own way here)
  if s.ai ~= 0 and not s.car and not h:GetOwner() and M.inWorldAt and M.now - M.inWorldAt > 3 then
    local q = h:GetPos()
    local d = math.sqrt((q.x - s.x) ^ 2 + (q.y - s.y) ^ 2)
    -- enemies in a fight run and take cover differently on each game: only one far off (12 m) is put back
    if d > (s.ai == 2 and PULL_DIST_FIGHT or PULL_DIST) then
      s.offSince = s.offSince or M.now
      if M.now - s.offSince > PULL_TIME then
        s.offSince = nil
        pcall(h.SetPos, h, V(s.x, s.y, s.z))
        pcall(h.SetDir, h, V(s.hx, s.hy, 0))
        N.pulls = (N.pulls or 0) + 1
        if N.pulls <= 20 then log(string.format("our %d was %.1f m off host NPC %d: put back", e.id, d, s.key)) end
      end
    else
      s.offSince = nil
    end
  end
end

function N.GuestTick(p)
  local matched, live = 0, 0
  for key, s in pairs(N.host) do
    if M.now - s.t > 30 then
      if s.loc then N.byLocal[s.loc] = nil end
      N.host[key] = nil
    elseif M.now - s.t <= STALE then
      live = live + 1
      if not s.loc and not s.dead then pair(key, s) end
      local e = s.loc and N.ents[s.loc]
      if e and valid(e.h) then
        matched = matched + 1
        local good, err = pcall(N.Enforce, s, e, p)
        if not good then log("enforce error: " .. tostring(err)) end
      end
    end
  end
  N.status = string.format("NPCs: %d of the host's %d paired", matched, live)
end

-- ---- update -----------------------------------------------------------------------------------------------------------
function N.Update(dt, cmds)
  local ev = N.hitq
  N.hitq = {}
  local active, host = false, false
  for line in string.gmatch(cmds, "[^\n]+") do
    local f = {}
    for w in string.gmatch(line, "%S+") do f[#f + 1] = w end
    if f[1] == "S" then
      active, host = f[2] == "1", f[3] == "1"
    elseif f[1] == "N" then
      local key = tonumber(f[2])
      local s = N.host[key] or { key = key }
      N.host[key] = s
      s.x, s.y, s.z, s.hx, s.hy = tonumber(f[3]), tonumber(f[4]), tonumber(f[5]), tonumber(f[6]), tonumber(f[7])
      s.hp, s.ai, s.weapon = tonumber(f[8]), tonumber(f[10]), f[11] or "-"
      local flags = tonumber(f[9]) or 0
      s.dead = s.dead or (flags % 2) == 1
      s.car = math.floor(flags / 2) % 2 == 1
      s.t = M.now
    elseif f[1] == "D" then
      local s = N.host[tonumber(f[2])]
      if s then s.dead = true end
    elseif f[1] == "H" then
      local good, err = pcall(N.HostHit, f[2], tonumber(f[3]), tonumber(f[4]) or 0, tonumber(f[5]) or 0,
        tonumber(f[6]) or 0, tonumber(f[7]) or 0)
      if not good then log("hit error: " .. tostring(err)) end
    end
  end
  local p = M.Player and M.Player()
  local key = p and (tostring(game.game:GetLoadedSaveId()) .. "|" .. tostring(M.inWorldAt)) or nil
  if key ~= N.worldKey then  -- another world: nothing carries over
    N.worldKey = key
    N.ents, N.host, N.byLocal, N.sent, N.replays = {}, {}, {}, {}, {}
    N.scanT = -10
  end
  if not (active and p) then return table.concat(ev, "\n") end
  if M.now - (N.scanT or -10) >= SCAN_INTERVAL then
    N.scanT = M.now
    local good, err = pcall(N.Scan)
    if not good then log("scan error: " .. tostring(err)) end
  end
  if host then
    if M.now - (N.sendT or -10) >= SEND_INTERVAL then
      N.sendT = M.now
      local good, err = pcall(N.HostSend, p, ev)
      if not good then log("send error: " .. tostring(err)) end
    end
    pcall(N.ReplayTick)
    N.status = string.format("NPCs: sending %d, guest hits %d", N.hostCount or 0, N.hitsIn or 0)
  else
    local good, err = pcall(N.GuestTick, p)
    if not good then log("guest error: " .. tostring(err)) end
  end
  if M.now - (N.stT or -10) >= 1 then
    N.stT = M.now
    ev[#ev + 1] = "ST " .. (N.status or "")
  end
  return table.concat(ev, "\n")
end

return N.version
