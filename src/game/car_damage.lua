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
--   EX ent                       blow car entity `ent` up (SetExploded; only when the dll has no native explosion)
-- Output (events, one per line; a round every ROUND s while a session runs):
--   CD ref ent role auth quiet gd md
--                                one car: ref = its network name ("o:g" player car key, "t<net>" a host traffic car's
--                                copy here, "h<id>" our own traffic car as host), ent = this game's entity id, role
--                                A (our game holds its damage: we drive it, or drove it last) or C (a copy here),
--                                auth = the player whose game holds it (- unknown), quiet 1 = the copy was put in
--                                place by SetPos in the last second (pinned / snapped: its own bumps are no damage),
--                                gd / md = GetDamage / GetMotorDamage (0..100, for the logs; - unknown)
--   CE                           end of the round
--   ST text                      status for the overlay (once a second)
MafiaCoop = MafiaCoop or {}
local M = MafiaCoop
M.CarDamage = M.CarDamage or {}
local D = M.CarDamage
D.version = 1

local ROUND = 0.25        -- s between rounds
local QUIET = 1.0         -- s after a SetPos (pin / snap) a copy's own damage is not reported
D.last = D.last or {}     -- player car key -> id of its last driver (the game that holds its damage while parked)
D.expl = D.expl or {}     -- player car key -> true: our copy's explosion was switched back on (we hold the car)

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

function D.Repair(ent)
  local e = carOf(ent)
  if not e then return end
  local _, ok = try("Repair", e.Repair, e)
  local g, d = pcall(e.GetDamage, e)
  log(string.format("car %s repaired (%s): GetDamage now %s", tostring(ent), ok and "ok" or "failed",
    g and num(d) or "?"))
end

function D.Explode(ent)
  local e = carOf(ent)
  if not e then return end
  pcall(e.DisableExplosion, e, false)
  local _, ok = try("SetExploded", e.SetExploded, e)
  if not ok then try("Explode", e.Explode, e) end
  log("car " .. tostring(ent) .. " blown up through the script binding (" .. (ok and "SetExploded" or "Explode") .. ")")
end

-- A copy we spawned for a player car (c.qid) has its explosion switched off (player_sync M.CarTick: a copy must not
-- blow up on one game only). While our game holds the car's damage (we drive it, or drove it last) it must be able
-- to blow up like any car; when another game holds it again, off again.
local function explosionFor(key, c, ours)
  local id = c.qid and entId(c.ent)
  if not id then return end
  if ours and D.expl[key] ~= id then
    D.expl[key] = id  -- by entity: a fresh copy (M.Swap) starts with its explosion off again
    try("DisableExplosion", c.ent.DisableExplosion, c.ent, false)
    log("car " .. key .. ": we hold its damage, our copy " .. id .. " can blow up now")
  elseif not ours and D.expl[key] then
    if D.expl[key] == id then
      try("DisableExplosion", c.ent.DisableExplosion, c.ent, true)
      log("car " .. key .. ": another game holds its damage, our copy does not blow up by itself again")
    end
    D.expl[key] = nil
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

function D.Round(ev)
  local me = M.localId or "0"
  local n, own, copies = 0, 0, 0
  for key, c in pairs(M.cars or {}) do
    if c.driver then D.last[key] = (c.driver == "local") and me or c.driver end
    if c.st == "ready" and valid(c.ent) then
      local holder = (c.driver == "local") and me or c.driver or D.last[key] or key:match("^(%d+):")
      local ours = holder == me
      explosionFor(key, c, ours)
      line(ev, key, c.ent, ours and "A" or "C", holder, ours and "0" or quietOf(c.rv))
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
          n, own = n + 1, own + 1
        end
      end
    else
      for net, c in pairs(T.copies or {}) do
        if c.st == "ready" and valid(c.ent) then
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
      D.Explode(f[2])
    end
  end
  if not D.active then
    if next(D.last) or next(D.expl) then D.last, D.expl = {}, {} end
    return ""
  end
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
