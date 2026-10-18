-- MafiaCoop door glue: doors are the same on every game. Runs in the game's Lua state on GameMainThread after
-- player_sync.lua, called once per script tick by src/game/door_sync.cpp (MafiaCoop.Doors.Update). Embedded into the
-- dll; a copy at <dll dir>\door_sync.lua overrides it. docs/game-api.md 21, docs/protocol.md "Doors".
--
-- Doors are entities of type 21 (enums.EntityType.DOOR; the entity table, mc_entity_ids(21)). Their wrapper is
-- C_WrapperDoor (IsClosed, IsLocked, Open(pos), Close, Kick(pos), Lock, Unlock); scene objects with a
-- C_RuntimeDoorComponent (shops, safehouses: IsOpen, Open, Close, Lock, Unlock) are used through the component when
-- the wrapper lacks the calls. A door is the same door on every game by its scene name and position (the C++ side,
-- game/door_model.h); its entity id is only ours.
-- This glue: keeps the table of loaded doors (a scan every few seconds, new ones read a few per tick), polls the
-- doors near our player (and any door that just got a door message or that we changed), works out who changed one
-- (our player: a door message he sent, his door kick / lock pull / door open action from player_sync.lua, or he
-- stood at it; else our world: scripts, NPCs) and carries out the changes the C++ side asks for.
--
-- Input (cmds, one line per command):
--   S active host localId                      session running, we host, our player id
--   A id act state fx fy fz by why             bring door `id` to `state` (1 open, 2 locked) the way `act` says
--                                              (door_model.h Act: 1 open 2 close 3 kick 4 unlock 5 lockpick 6 lock
--                                              7 set); fx fy fz: where the one who did it stood
-- Output (events, one per line):
--   DW key                                     a new world (or none: "-"); everything below starts again
--   DD id x y z state name                     a door in our table (first seen)
--   DX id                                      it is gone (not in the entity table any more)
--   DF id id ...                               these were polled within the last second, unchanged
--   DS id state ours act px py pz how          a door changed (ours 1: our player did it, act how; how: msg, act,
--                                              near, - for our world)
--   DP x y z                                   where our player is
--   ST text                                    status for the overlay (once a second)
MafiaCoop = MafiaCoop or {}
local M = MafiaCoop
M.Doors = M.Doors or {}
local D = M.Doors
D.version = 1

local DOOR_TYPE = 21         -- enums.EntityType.DOOR
local SCAN_INTERVAL = 4.0    -- s between scans of the entity table
local NEW_PER_TICK = 120     -- new doors read per tick (the first scan of a city world finds many)
local NEAR_RADIUS = 35       -- m: doors this close to our player are polled
local NEAR_INTERVAL = 1.0    -- s between recomputing the near list
local POLL_INTERVAL = 0.2    -- s between polls of the near doors
local FRESH_INTERVAL = 1.0   -- s between DF lines
local WATCH_TIME = 4.0       -- s a door that got a message or that we changed is polled wherever it is
local OURS_WINDOW = 2.0      -- s after our player's door action (player_sync M.doorActT) a change near him is his
local OURS_DIST = 3.5        -- m from him for that
local HAND_DIST = 1.8        -- m: a door opened or closed this close to our player on foot is his
local MSG_WINDOW = 1.5       -- s a door message explains a change of that door
local ACT = { open = 1, close = 2, kick = 3, unlock = 4, lockpick = 5, lock = 6, set = 7 }
local ACT_NAMES = { [1] = "open", [2] = "close", [3] = "kick", [4] = "unlock", [5] = "lockpick", [6] = "lock",
  [7] = "set" }
-- player_sync.lua action kinds (player_actions.h) -> the door act they stand for
local FROM_ACTION = { [18] = ACT.kick, [19] = ACT.lockpick, [20] = ACT.open }
-- enums.DoorMessages (base 0x150000, re3 enums dump at exe 0x5AE1600): what a door's message says happened
local DOOR_MESSAGES = { "OPEN", "CLOSE", "KICK", "LOCKPICK", "TOGGLELOCK", "TOGGLEOPEN" }

local function log(s) if mc_log then mc_log("doors: " .. s) end end
local function trace(kind, text) if mc_trace then mc_trace(kind, text) end end
local function valid(o)
  if o == nil then return false end
  local good, v = pcall(IsObjectValid, o)
  return not good or v ~= false
end
local function V(x, y, z) return Math:newVector(x, y, z) end
local errs = {}
local function try(what, f, ...)
  local good, a, b = pcall(f, ...)
  if not good then
    errs[what] = (errs[what] or 0) + 1
    if errs[what] <= 5 then log(what .. " failed: " .. tostring(a)) end
    return false, a
  end
  return true, a, b
end
local function isOpen(st) return st % 2 == 1 end
local function isLocked(st) return math.floor(st / 2) % 2 == 1 end
local function field(o, k)  -- o[k] without an error for wrappers that throw on unknown keys
  local good, v = pcall(function() return o[k] end)
  return good and v or nil
end
local function dist(ax, ay, az, bx, by, bz) return math.sqrt((ax - bx) ^ 2 + (ay - by) ^ 2 + (az - bz) ^ 2) end
local function idOfGuid(g)  -- the entity id a C_EntityGuid prints as (the number tostring ends with)
  local s = tostring(g or "")
  return tonumber(s:match("(%d+)%D*$") or "")
end

D.doors = D.doors or {}  -- id -> { e, id, name, x, y, z, st, api ("w" wrapper, "c" component), comp }
D.todo = D.todo or {}    -- ids found by the scan, not read yet
D.bad = D.bad or {}      -- ids that are no usable door (no state readable)
D.near = D.near or {}    -- ids polled every POLL_INTERVAL
D.watch = D.watch or {}  -- id -> until: polled wherever it is
D.msgs = D.msgs or {}    -- id -> { kind, sender, t }: the latest door message of that door
D.subs = D.subs or {}

-- ---- reading a door ---------------------------------------------------------------------------------------------------
-- The calls of one door: its wrapper's (C_WrapperDoor) when it has them, else its C_RuntimeDoorComponent's.
local function bind(d)
  local e = d.e
  if field(e, "IsClosed") and field(e, "Open") then
    d.api = "w"
    return true
  end
  local gc = field(e, "GetComponent")
  if gc then
    local good, c = pcall(gc, e, "C_RuntimeDoorComponent")
    if good and c and field(c, "IsOpen") then
      d.api, d.comp = "c", c
      return true
    end
  end
  return false
end

local function readState(d)
  local e = d.e
  if not valid(e) then return nil end
  local open, locked
  if d.api == "w" then
    local good, closed = pcall(e.IsClosed, e)
    if not good then return nil end
    open = closed == false
  else
    local good, o = pcall(d.comp.IsOpen, d.comp)
    if not good then return nil end
    open = o == true
  end
  local il = field(e, "IsLocked")
  if il then
    local good, l = pcall(il, e)
    locked = good and l == true
  end
  return (open and 1 or 0) + (locked and 2 or 0)
end

local function readNew(id, ev)
  local e = mc_entity(id)
  if not e then
    D.bad[id] = true
    return
  end
  local d = { e = e, id = id }
  if not bind(d) then
    D.bad[id] = true
    D.unbound = (D.unbound or 0) + 1
    if D.unbound <= 5 then log("entity " .. id .. " (type 21) has no door calls we know: skipped") end
    return
  end
  local good, pos = pcall(e.GetPos, e)
  if not good or not pos then
    D.bad[id] = true
    return
  end
  local st = readState(d)
  if not st then
    D.bad[id] = true
    return
  end
  local name = "-"
  local gn = field(e, "GetName")
  if gn then
    local ok, n = pcall(gn, e)
    if ok and type(n) == "string" and n ~= "" then name = n:gsub("[^%w_%-%.]", "_"):sub(1, 80) end
  end
  d.name, d.x, d.y, d.z, d.st = name, pos.x, pos.y, pos.z, st
  D.doors[id] = d
  D.apis = D.apis or {}
  D.apis[d.api] = (D.apis[d.api] or 0) + 1
  ev[#ev + 1] = string.format("DD %d %.3f %.3f %.3f %d %s", id, d.x, d.y, d.z, st, name)
end

-- ---- the table --------------------------------------------------------------------------------------------------------
function D.Scan(ev)
  local list = mc_entity_ids(DOOR_TYPE) or ""
  local seen, n = {}, 0
  for s in string.gmatch(list, "%d+") do
    local id = tonumber(s)
    seen[id] = true
    n = n + 1
    if not D.doors[id] and not D.bad[id] and not D.queued[id] then
      D.todo[#D.todo + 1] = id
      D.queued[id] = true
    end
  end
  for id, _ in pairs(D.doors) do
    if not seen[id] then
      D.doors[id] = nil
      ev[#ev + 1] = "DX " .. id
    end
  end
  for id, _ in pairs(D.bad) do if not seen[id] then D.bad[id] = nil end end
  D.found = n
end

function D.ReadSome(ev)
  local k = 0
  while #D.todo > 0 and k < NEW_PER_TICK do
    local id = table.remove(D.todo)
    D.queued[id] = nil
    k = k + 1
    local good, err = pcall(readNew, id, ev)
    if not good then
      D.bad[id] = true
      errs.read = (errs.read or 0) + 1
      if errs.read <= 5 then log("reading door " .. id .. " failed: " .. tostring(err)) end
    end
  end
end

function D.Near(p)
  local q = p:GetPos()
  local near = {}
  for id, d in pairs(D.doors) do
    if dist(d.x, d.y, d.z, q.x, q.y, q.z) <= NEAR_RADIUS then near[#near + 1] = id end
  end
  D.near = near
end

-- Who changed door d (old -> st): ours 1/0, the act, how we know.
local function attribute(d, old, st, p)
  local q = p:GetPos()
  local dd = dist(d.x, d.y, d.z, q.x, q.y, q.z)
  local opened, closed = not isOpen(old) and isOpen(st), isOpen(old) and not isOpen(st)
  local unlocked = isLocked(old) and not isLocked(st)
  local m = D.msgs[d.id]
  if m and M.now - m.t < MSG_WINDOW and M.localGuid and m.sender == M.localGuid then
    local a = (m.kind == "KICK" and ACT.kick) or (m.kind == "LOCKPICK" and ACT.lockpick) or nil
    return 1, a or (opened and ACT.open) or (closed and ACT.close) or (unlocked and ACT.unlock) or ACT.set, "msg", dd
  end
  local kind = M.doorActKind
  if kind and M.doorActT and M.now - M.doorActT < OURS_WINDOW and dd <= OURS_DIST and FROM_ACTION[kind] then
    local a = FROM_ACTION[kind]
    if closed then a = ACT.close end
    return 1, a, "act", dd
  end
  local inCar = false
  local good, owner = pcall(p.GetOwner, p)
  if good and owner then inCar = true end
  if not inCar and dd <= HAND_DIST and (opened or closed) then
    return 1, opened and ACT.open or ACT.close, "near", dd
  end
  return 0, 0, "-", dd
end

function D.Poll(p, ev, ids)
  local q = p:GetPos()
  for _, id in ipairs(ids) do
    local d = D.doors[id]
    if d then
      local st = readState(d)
      if st == nil then
        if not valid(d.e) then
          D.doors[id] = nil
          ev[#ev + 1] = "DX " .. id
        end
      elseif st ~= d.st then
        local old = d.st
        d.st = st
        local ours, act, how, dd = attribute(d, old, st, p)
        D.changes = (D.changes or 0) + 1
        ev[#ev + 1] = string.format("DS %d %d %d %d %.3f %.3f %.3f %s", id, st, ours, act, q.x, q.y, q.z, how)
        local m = D.msgs[id]
        trace("DL", string.format("ev=change id=%d name=%s p=%.2f,%.2f,%.2f old=%d state=%d ours=%d act=%s how=%s "
          .. "d=%.2f msg=%s", id, d.name, d.x, d.y, d.z, old, st, ours, ACT_NAMES[act] or "-", how, dd,
          m and (m.kind .. "@" .. string.format("%.2f", M.now - m.t)) or "-"))
        if D.changes <= 60 or D.changes % 50 == 0 then
          log(string.format("door %d (%s at %.1f %.1f %.1f) %s%s -> %s%s, %s (%s, %.1f m from us)", id, d.name, d.x,
            d.y, d.z, isOpen(old) and "open" or "closed", isLocked(old) and "+locked" or "",
            isOpen(st) and "open" or "closed", isLocked(st) and "+locked" or "",
            ours == 1 and ("by us: " .. (ACT_NAMES[act] or "?")) or "by our world", how, dd))
        end
      else
        D.fresh[id] = true
      end
    end
  end
end

-- ---- door messages ----------------------------------------------------------------------------------------------------
function D.OnDoorMessage(m, kind)
  local id = idOfGuid(m:GetReceiverGUID())
  if not id then return end
  local good, s = pcall(m.GetSenderGUID, m)
  local sender = good and tostring(s) or "-"
  D.msgs[id] = { kind = kind, sender = sender, t = M.now }
  D.watch[id] = M.now + WATCH_TIME
  D.msgCount = (D.msgCount or 0) + 1
  local d = D.doors[id]
  trace("DL", string.format("ev=msg id=%d kind=%s sender=%s ours=%d known=%d", id, kind, sender,
    (M.localGuid and sender == M.localGuid) and 1 or 0, d and 1 or 0))
end

function D.Unwatch()
  for _, id in ipairs(D.subs) do pcall(game.game.UnregisterBlanketEvent, game.game, id) end
  D.subs = {}
end

-- One blanket subscription per door message (enums.EventType.DOOR), per world. UNVERIFIED that blanket events reach
-- doors (game.lua uses them for humans); without them the poll still sees every change near us, just without the
-- sender.
function D.Watch()
  D.Unwatch()
  local dm = enums and enums.DoorMessages
  local et = enums and enums.EventType and enums.EventType.DOOR
  if not dm or not et then
    log("no enums.DoorMessages / EventType.DOOR in this build: no door messages, the poll alone")
    return
  end
  local n = 0
  for _, kind in ipairs(DOOR_MESSAGES) do
    local msg = dm[kind]
    if msg then
      local good, id = pcall(game.game.RegisterBlanketEvent, game.game, {}, function(ctx, m)
        local ok, err = pcall(D.OnDoorMessage, m, kind)
        if not ok then
          errs.msg = (errs.msg or 0) + 1
          if errs.msg <= 5 then log("door message error: " .. tostring(err)) end
        end
      end, et, msg)
      if good and tonumber(id) and tonumber(id) >= 0 then
        D.subs[#D.subs + 1] = id
        n = n + 1
      else
        log("could not watch door " .. kind .. " messages (" .. tostring(id) .. ")")
      end
    end
  end
  log("watching door messages (" .. n .. " subscriptions)")
end

-- ---- applying ---------------------------------------------------------------------------------------------------------
local function call(d, what, ...)
  local o = d.api == "w" and d.e or d.comp
  local f = field(o, what)
  if not f and d.api == "c" then  -- the component lacks it (IsLocked, Kick): the wrapper's, if any
    o = d.e
    f = field(o, what)
  end
  if not f then return false, "no " .. what end
  local good, r = pcall(f, o, ...)
  return good, r
end

function D.Apply(f)
  local n = tonumber
  local id, act, want = n(f[2]), n(f[3]) or 7, n(f[4]) or 0
  local from = V(n(f[5]) or 0, n(f[6]) or 0, n(f[7]) or 0)
  local by, why = f[8] or "?", f[9] or "-"
  local d = id and D.doors[id]
  if not d or not valid(d.e) then
    log("apply to door " .. tostring(id) .. ": not in our table any more")
    return
  end
  local cur = readState(d) or d.st
  local calls, ok = {}, true
  local function step(what, ...)
    local good, r = call(d, what, ...)
    calls[#calls + 1] = what .. (good and "" or "!")
    if not good then
      ok = false
      errs[what] = (errs[what] or 0) + 1
      if errs[what] <= 5 then log("door " .. id .. ": " .. what .. " failed: " .. tostring(r)) end
    end
    return good
  end
  if isLocked(cur) and not isLocked(want) then step("Unlock") end
  if not isOpen(cur) and isOpen(want) then
    local done = false
    if act == ACT.kick and d.api == "w" then done = step("Kick", from) end
    if not done then
      if d.api == "w" then
        done = step("Open", from)
        if not done then done = step("Open", from, false) end
      else
        done = step("Open")
      end
    end
    if not done then step("ToggleOpen", from) end
  end
  if isOpen(cur) and not isOpen(want) then step("Close") end
  if not isLocked(cur) and isLocked(want) then step("Lock") end
  D.watch[id] = M.now + WATCH_TIME
  D.applied = (D.applied or 0) + 1
  trace("DA", string.format("id=%d name=%s act=%s want=%d cur=%d by=%s why=%s calls=%s ok=%d", id, d.name,
    ACT_NAMES[act] or "?", want, cur, by, why, #calls > 0 and table.concat(calls, ",") or "-", ok and 1 or 0))
  if D.applied <= 60 or D.applied % 50 == 0 then
    log(string.format("door %d (%s): %s -> %s (%s, %s of %s): %s", id, d.name, tostring(cur), tostring(want),
      ACT_NAMES[act] or "?", why, by, #calls > 0 and table.concat(calls, ", ") or "nothing to do"))
  end
end

-- ---- update -----------------------------------------------------------------------------------------------------------
local function reset(ev, key)
  D.doors, D.todo, D.queued, D.bad, D.near, D.watch, D.msgs, D.apis = {}, {}, {}, {}, {}, {}, {}, {}
  D.scanT, D.nearT = -10, -10
  ev[#ev + 1] = "DW " .. key
end

function D.Update(dt, cmds)
  local ev = {}
  local isActive, host = false, false
  local applies = {}
  for line in string.gmatch(cmds, "[^\n]+") do
    local f = {}
    for w in string.gmatch(line, "%S+") do f[#f + 1] = w end
    if f[1] == "S" then
      isActive, host = f[2] == "1", f[3] == "1"
    elseif f[1] == "A" then
      applies[#applies + 1] = f
    end
  end
  if not mc_entity_ids or not mc_entity then
    if not D.noEntities then log("no mc_entity_ids / mc_entity: doors are not synced") end
    D.noEntities = true
    return ""
  end
  local p = M.Player and M.Player()
  local key = p and (tostring(game.game:GetLoadedSaveId()) .. "|" .. tostring(M.inWorldAt)) or "-"
  if not isActive then
    if D.wasActive then
      D.Unwatch()
      D.wasActive = false
    end
    return ""
  end
  if not D.wasActive or key ~= D.worldKey then  -- a new session or a new world: our table starts again
    D.wasActive, D.worldKey = true, key
    D.queued = D.queued or {}
    reset(ev, key)
    D.Unwatch()
    if p then D.Watch() end
  end
  if not p then return table.concat(ev, "\n") end
  D.queued = D.queued or {}
  D.fresh = D.fresh or {}
  for _, f in ipairs(applies) do
    local good, err = pcall(D.Apply, f)
    if not good then log("apply error: " .. tostring(err)) end
  end
  if M.now - (D.scanT or -10) >= SCAN_INTERVAL then
    D.scanT = M.now
    local good, err = pcall(D.Scan, ev)
    if not good then log("scan error: " .. tostring(err)) end
  end
  D.ReadSome(ev)
  if M.now - (D.nearT or -10) >= NEAR_INTERVAL then
    D.nearT = M.now
    pcall(D.Near, p)
  end
  if M.now - (D.pollT or -10) >= POLL_INTERVAL then
    D.pollT = M.now
    local ids, inList = {}, {}
    for _, id in ipairs(D.near) do ids[#ids + 1] = id; inList[id] = true end
    for id, untilT in pairs(D.watch) do
      if M.now > untilT then D.watch[id] = nil elseif not inList[id] then ids[#ids + 1] = id end
    end
    local good, err = pcall(D.Poll, p, ev, ids)
    if not good then log("poll error: " .. tostring(err)) end
  end
  if M.now - (D.freshT or -10) >= FRESH_INTERVAL then
    D.freshT = M.now
    local list = {}
    for id, _ in pairs(D.fresh) do
      list[#list + 1] = tostring(id)
      if #list >= 200 then break end
    end
    D.fresh = {}
    if #list > 0 then ev[#ev + 1] = "DF " .. table.concat(list, " ") end
    local q = p:GetPos()
    ev[#ev + 1] = string.format("DP %.2f %.2f %.2f", q.x, q.y, q.z)
  end
  if M.now - (D.stT or -10) >= 1 then
    D.stT = M.now
    local n = 0
    for _, _ in pairs(D.doors) do n = n + 1 end
    local a = D.apis or {}
    ev[#ev + 1] = string.format("ST %d doors (%d near, %d to read; wrapper %d, component %d), %d msgs, %d applied",
      n, #D.near, #D.todo, a.w or 0, a.c or 0, D.msgCount or 0, D.applied or 0)
  end
  return table.concat(ev, "\n")
end

return D.version
