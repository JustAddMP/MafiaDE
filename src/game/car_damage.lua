-- MafiaCoop car damage glue: the same car damage on every game (docs/game-api.md 24, docs/protocol.md "Car damage").
-- Runs in the game's Lua state on GameMainThread after player_sync.lua and traffic_sync.lua, called once per script
-- tick by src/game/car_damage_sync.cpp (MafiaCoop.CarDamage.Update). Embedded into the dll; a copy at
-- <dll dir>\car_damage.lua overrides it for development.
--
-- The C++ side reads and sets the damage itself (the car's deform parts, its wheels, the fire and the explosion,
-- found statically in the exe, docs/re-notes.md 11); this glue only says which cars there are, which game holds each
-- one's damage, and does what needs a script binding (Repair, and the explosion when the native call is missing).
--
-- Input (cmds, one line per command):
--   S active host local          session running, we host, our player id
--   RP ent                       repair car entity `ent` (C_WrapperCar::Repair): a copy whose car was repaired
--   EX ent pic claimed           blow car entity `ent` up through D.Explode (pic 1: a visual-only picture of the
--                                blast, a copy; claimed 1: mc_car_explode_first was asked already, our own car)
--   DX ent                       a copy's explosion switched off again (after a repair or parts set on it)
-- Output (events, one per line; a round every ROUND s while a session runs):
--   CD ref ent role auth quiet gd md
--                                one car: ref = its network name ("o:g" player car key, "t<net>" a host traffic car's
--                                copy here, "h<id>" our own traffic car as host), ent = this game's entity id, role
--                                A (our game holds its damage: we drive it, or drove it last) or C (a copy here),
--                                auth = the player whose game holds it (- unknown), quiet 1 = the copy was put in
--                                place by SetPos in the last second (pinned / snapped: its own bumps are no damage),
--                                gd / md = GetDamage / GetMotorDamage (0..100, for the logs; - unknown)
--   CE                           end of the round
--   BL ref ent dist fire seq from another game's explosion `dist` m from our car `ent` (we hold its damage): the dll
--                                puts the blast's effect on it (car_damage.h BlastOn); fire 1 = a molotov's fire only
--   ST text                      status for the overlay (once a second)
MafiaCoop = MafiaCoop or {}
local M = MafiaCoop
M.CarDamage = M.CarDamage or {}
local D = M.CarDamage
D.version = 1

local ROUND = 0.15        -- s between rounds (was 0.25; a round sends only what changed)
local QUIET = 1.0         -- s after a SetPos (pin / snap) a copy's own damage is not reported
D.last = D.last or {}     -- player car key -> id of its last driver (the game that holds its damage while parked)
D.expl = D.expl or {}     -- player car key -> true: our copy's explosion was switched back on (we hold the car)
D.off = D.off or {}       -- entity id -> true: a car another game holds, its explosion switched off by us
D.blown = D.blown or {}   -- entity id -> true: blown up by D.Explode (nothing to switch off any more)
D.held = D.held or {}     -- the cars we hold (this round): { ref, e }
D.blasts = D.blasts or {} -- "BL ..." lines for the next round
local BLAST_REACH = 10.5  -- m: cars farther from another game's explosion are not touched (car_damage.h kBlastReach)

local function log(s) if mc_log then mc_log("damage: " .. s) end end
local function valid(o) return o ~= nil and IsObjectValid(o) end
local function entId(o)
  local good, g = pcall(o.GetGUID, o)
  if not good then return nil end
  return tonumber(tostring(g):match("(%d+)%s*$"))
end
local errs = {}
local function try(what, f, ...)
  local good, a = pcall(f, ...)
  if not good then
    errs[what] = (errs[what] or 0) + 1
    if errs[what] <= 5 then log(what .. " failed: " .. tostring(a)) end
    return nil, false
  end
  return a, true
end
local function num(v)
  v = tonumber(v)
  if not v or v ~= v then return "-" end
  return string.format("%.1f", v)
end

-- the entity (a live car) for id `ent`, or nil
local function carOf(ent)
  if not mc_entity then return nil end
  local e = mc_entity(tonumber(ent) or -1)
  if e and valid(e) and e.GetVehicleModelName then return e end
  return nil
end

-- a car another game holds: its explosion off (re-asserted every round, after a repair and after parts were set:
-- only the holder's game has a real explosion of it, docs/game-api.md 24 "One explosion")
local function keepOff(e, id)
  if not id or D.blown[id] then return end
  try("DisableExplosion", e.DisableExplosion, e, true)
  D.off[id] = true
end

function D.Repair(ent)
  local e = carOf(ent)
  if not e then return end
  local _, ok = try("Repair", e.Repair, e)
  keepOff(e, entId(e))  -- repairs are only ever done on copies
  local g, d = pcall(e.GetDamage, e)
  log(string.format("car %s repaired (%s): GetDamage now %s", tostring(ent), ok and "ok" or "failed",
    g and num(d) or "?"))
end

-- The one way a car is blown up here because another game says so (docs/game-api.md 24 "One explosion"): car
-- damage's exploded flag for a copy ("EX"), the explosions sync's CarExploded (explosion_sync.lua B.OnRemoteCar) and a
-- wrecked car of ours coming back (player_sync.lua M.Restore) all call this. mc_car_explode_first lets the first one
-- per entity through and refuses the rest, whichever comes first. `ent`: an entity id (string or number) or the car.
-- picture: show the blast as a visual-only explosion (CreateScriptExplosion without damage, with the damage guard's
-- remote zone: explosion_sync.lua B.Picture); claimed: the dll asked mc_car_explode_first already (our own car).
-- Never Explode(): that is a real blast with the game's own countdown. Returns ok, what ("set_exploded", "already",
-- "no_car", "failed:...").
function D.Explode(ent, picture, claimed, from)
  local e = ent
  if type(ent) ~= "userdata" and type(ent) ~= "table" then e = carOf(ent) end
  if not (e and valid(e)) then return false, "no_car" end
  local good, g = pcall(e.GetGUID, e)
  local idText = good and tostring(g):match("(%d+)%s*$") or nil
  local id = idText and tonumber(idText)
  if not claimed and mc_car_explode_first and not mc_car_explode_first(idText or "0") then
    log("car " .. tostring(idText) .. ": blown up already (one explosion per car)")
    return false, "already"
  end
  if id then
    D.blown[id] = true
    -- its CAR_HAVE_PROBLEM here is not ours to send on (explosion_sync.lua B.OnCarProblem skips known ones)
    if M.Boom and M.Boom.exploded then M.Boom.exploded[id] = true end
  end
  pcall(e.DisableExplosion, e, false)  -- (switched off on copies; it may block SetExploded too)
  local _, ok = try("SetExploded", e.SetExploded, e)
  local shown = false
  if picture and M.Boom and M.Boom.Picture then
    local q = e:GetPos()
    shown = M.Boom.Picture(q.x, q.y, q.z, "exp_grenade", false, from or 255, 0, 5) and true or false
    if M.KeyOf then
      local _, c = M.KeyOf(e)
      if c then c.boomShown = true end  -- our hidden real car of it comes back set exploded without a second picture
    end
  end
  log(string.format("car %s set exploded (%s%s)", tostring(idText), ok and "SetExploded" or "SetExploded failed",
    picture and (shown and ", with a picture" or ", picture failed") or ""))
  if mc_trace then
    mc_trace("CDE", string.format("ent=%s ok=%d picture=%d claimed=%d from=%s", tostring(idText), ok and 1 or 0,
      shown and 1 or 0, claimed and 1 or 0, tostring(from or "-")))
  end
  return ok, ok and "set_exploded" or "failed:SetExploded"
end

-- A copy we spawned for a player car (c.qid) has its explosion switched off (player_sync M.CarTick: a copy must not
-- blow up on one game only), and so has every car another game holds (keepOff, every round). While our game holds
-- the car's damage (we drive it, or drove it last) it must be able to blow up like any car; when another game holds
-- it again, off again.
local function explosionFor(key, c, ours)
  local id = entId(c.ent)
  if not id then return end
  if ours then
    if (c.qid and D.expl[key] ~= id) or D.off[id] then
      D.expl[key] = id  -- by entity: a fresh copy (M.Swap) starts with its explosion off again
      D.off[id] = nil
      if not D.blown[id] then
        try("DisableExplosion", c.ent.DisableExplosion, c.ent, false)
        log("car " .. key .. ": we hold its damage, our entity " .. id .. " can blow up now")
      end
    end
  else
    if D.expl[key] == id then
      log("car " .. key .. ": another game holds its damage, our entity does not blow up by itself again")
    end
    D.expl[key] = nil
    keepOff(c.ent, id)
  end
end

local function quietOf(rv)
  return rv and rv.setPosT and M.now - rv.setPosT < QUIET and "1" or "0"
end

local function line(ev, ref, e, role, auth, quiet)
  local id = entId(e)
  if not id then return end
  local g1, gd = pcall(e.GetDamage, e)
  local g2, md = pcall(e.GetMotorDamage, e)
  ev[#ev + 1] = string.format("CD %s %d %s %s %s %s %s", ref, id, role, auth or "-", quiet or "0",
    g1 and num(gd) or "-", g2 and num(md) or "-")
end

-- Another game's explosion at x y z (explosion_sync.lua B.OnRemote / B.OnRemoteCar; a damage-free picture here):
-- cars stay destroyable by anyone's explosion, so the cars we hold within reach take its effect on our game (the dll
-- writes it; docs/game-api.md 24 "Another game's explosion"). kind: Explosion kind (2 molotov: fire only).
function D.Blast(x, y, z, kind, from, seq)
  x, y, z = tonumber(x), tonumber(y), tonumber(z)
  if not (x and y and z) then return 0 end
  local n = 0
  for _, h in ipairs(D.held) do
    if valid(h.e) then
      local good, q = pcall(h.e.GetPos, h.e)
      if good and q then
        local d = math.sqrt((q.x - x) ^ 2 + (q.y - y) ^ 2 + (q.z - z) ^ 2)
        if d <= BLAST_REACH then
          local id = entId(h.e)
          if id then
            D.blasts[#D.blasts + 1] = string.format("BL %s %d %.2f %d %s %s", h.ref, id, d,
              tonumber(kind) == 2 and 1 or 0, tostring(seq or 0), tostring(from or "-"))
            n = n + 1
          end
        end
      end
    end
  end
  if n > 0 then log(string.format("player %s's explosion at %.1f %.1f %.1f: %d of our cars within reach", tostring(from), x, y, z, n)) end
  return n
end

function D.Round(ev)
  local me = M.localId or "0"
  local n, own, copies = 0, 0, 0
  D.held = {}
  for key, c in pairs(M.cars or {}) do
    if c.driver then D.last[key] = (c.driver == "local") and me or c.driver end
    if c.st == "ready" and valid(c.ent) then
      local holder = (c.driver == "local") and me or c.driver or D.last[key] or key:match("^(%d+):")
      local ours = holder == me
      explosionFor(key, c, ours)
      line(ev, key, c.ent, ours and "A" or "C", holder, ours and "0" or quietOf(c.rv))
      if ours then D.held[#D.held + 1] = { ref = key, e = c.ent } end
      n = n + 1
      if ours then own = own + 1 else copies = copies + 1 end
    end
  end
  local T = M.Traffic
  if T then
    if D.host then
      for id, c in pairs(T.list or {}) do
        if valid(c.e) then
          line(ev, "h" .. id, c.e, "A", me, "0")
          D.held[#D.held + 1] = { ref = "h" .. id, e = c.e }
          n, own = n + 1, own + 1
        end
      end
    else
      for net, c in pairs(T.copies or {}) do
        if c.st == "ready" and valid(c.ent) then
          keepOff(c.ent, entId(c.ent))  -- the host's traffic car: only the host's game blows it up for real
          line(ev, "t" .. net, c.ent, "C", "0", quietOf(c.rv))
          n, copies = n + 1, copies + 1
        end
      end
    end
  end
  ev[#ev + 1] = "CE"
  D.cars, D.own, D.copies = n, own, copies
end

function D.Update(dt, cmds)
  local ev = {}
  for l in string.gmatch(cmds or "", "[^\n]+") do
    local f = {}
    for w in string.gmatch(l, "%S+") do f[#f + 1] = w end
    if f[1] == "S" then
      D.active, D.host = f[2] == "1", f[3] == "1"
    elseif f[1] == "RP" then
      D.Repair(f[2])
    elseif f[1] == "EX" then
      local good, err = pcall(D.Explode, f[2], f[3] == "1", f[4] == "1")
      if not good then log("explode error: " .. tostring(err)) end
    elseif f[1] == "DX" then
      local e = carOf(f[2])
      if e then keepOff(e, entId(e)) end
    end
  end
  -- a new world: entity ids start over (the dll's one-explosion book too)
  local world = tostring(M.inWorldAt)
  if D.world ~= world then
    D.world = world
    D.off, D.blown = {}, {}
    if mc_car_explode_reset then mc_car_explode_reset() end
  end
  if not D.active then
    if next(D.last) or next(D.expl) or next(D.off) or next(D.blown) then D.last, D.expl, D.off, D.blown = {}, {}, {}, {} end
    D.held, D.blasts = {}, {}
    return ""
  end
  for _, l in ipairs(D.blasts) do ev[#ev + 1] = l end
  D.blasts = {}
  D.t = (D.t or 0) + (dt or 0)
  if D.t >= ROUND then
    D.t = 0
    local good, err = pcall(D.Round, ev)
    if not good then
      errs.round = (errs.round or 0) + 1
      if errs.round <= 5 or errs.round % 500 == 0 then log("round error #" .. errs.round .. ": " .. tostring(err)) end
    end
    D.stT = (D.stT or 0) + ROUND
    if D.stT >= 1 then
      D.stT = 0
      ev[#ev + 1] = string.format("ST cars %d (ours %d, copies %d)", D.cars or 0, D.own or 0, D.copies or 0)
    end
  end
  return table.concat(ev, "\n")
end

return D.version
