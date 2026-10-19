-- MafiaCoop car look glue: a copy of a car shows that car's look - colours, plate, window tint, interior, dirt, rust,
-- painting and its tuning items (the body variant: open / closed, ...) - (docs/game-api.md 24 "Car look",
-- docs/protocol.md "Car look"). Runs in the game's Lua state on GameMainThread after player_sync.lua and
-- traffic_sync.lua, called once per script tick by src/game/car_look_sync.cpp (MafiaCoop.CarLook.Update). Embedded
-- into the dll; a copy at <dll dir>\car_look.lua overrides it for development.
--
-- The dll decides (src/game/car_look.h Book): which game sends a car's look (the one that has the car itself), what a
-- copy lacks and when to try again. This glue reads the looks and sets them.
--
-- Input (cmds, one line per command):
--   S active host local                        session running, we host, our player id
--   LW ref                                     host: read our traffic car `ref` ("h<entity>") this round: a guest
--                                              asked for its look
--   LA ent what ids look...                    set on our copy `ent`: `what` = comma list of color, plate, tint,
--                                              interior, dirt, rust, painting, tuning; `ids` = the tuning items to
--                                              install ("-" none); then the look (the line form below). The tuning
--                                              is left out while someone sits in the copy (InstallTuningItems
--                                              rebuilds the car: he would be put out); the dll sets it later
--   LH ent ent ...                             after each round: the copies a puppet is not seated in yet (their
--                                              look is still to come; player_sync M.Seat asks K.Holds)
-- Output (events, one per line; a round every ROUND s while a session runs):
--   LC ref ent role owner look...              one car: ref = "o:g" (a player car's key), "t<net>" (a host traffic
--                                              car's copy here), "h<id>" (our own traffic car as host); role S (we
--                                              have the car itself: we send its look), C (our copy: it takes the
--                                              look) or B (our copy, someone sits in it: its tuning waits); owner =
--                                              the player whose game sends it (- unknown)
--   LR ent                                     tuning items were installed on our copy `ent`: it was rebuilt (the
--                                              dll tells car damage)
--   LE                                         end of the round
--   ST text                                    status for the overlay (once a second)
-- The look (game/car_look.h ToLine): model c1 c2 plate tint interior dirt rust painting tuning; strings escaped
-- (%XX for anything outside letters, digits and _ . : -), "-" for empty / unknown, tuning "-" unknown, "." none, else
-- the item ids joined by ",". Dirt and rust are always sent as -1 (unknown): GetDirty reads entity + 0xD58, GetRust a
-- native, both floats of a scale not known (UNVERIFIED), and a car gets dirtier while it drives; the fields are on the
-- wire for later (0..250 = x 250 of a 0..1 scale).
MafiaCoop = MafiaCoop or {}
local M = MafiaCoop
M.CarLook = M.CarLook or {}
local K = M.CarLook
K.version = 1

local ROUND = 0.5          -- s between rounds
local TRAFFIC_EVERY = 3.0  -- s between two reads of a host traffic copy once it is older than FRESH s
local FRESH = 10.0
K.read = K.read or {}      -- traffic net -> last read time
K.born = K.born or {}      -- traffic net -> first seen
K.want = K.want or {}      -- host: "h<id>" asked for this round
K.hold = K.hold or {}      -- entity id -> true: a puppet is not seated in this copy yet ("LH")
local HOLD_FRESH = 1.5     -- s an "LH" list counts (rounds come every ROUND s)
local SEATS = 6            -- seats looked at for an occupant (0-based; a car has 2-4, a bus more)

local function log(s) if mc_log then mc_log("look: " .. s) end end
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

local function esc(s)
  if s == nil then return "-" end
  s = tostring(s)
  if s == "" then return "-" end
  if s == "-" then return "%2D" end
  return (s:gsub("[^%w_%.:%-]", function(ch) return string.format("%%%02X", ch:byte()) end))
end
local function unesc(s)
  if s == nil or s == "-" then return "" end
  return (s:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
end
local function int(v, lo, hi)
  v = tonumber(v)
  if not v or v ~= v or v < lo or v > hi then return -1 end
  return math.floor(v + 0.5)
end
local function tenth(v)  -- 0..1 -> 0..250 in steps of 25; -1 unknown
  v = tonumber(v)
  if not v or v ~= v then return -1 end
  v = math.max(0, math.min(1, v))
  return math.floor(v * 10 + 0.5) * 25
end

-- the look of car `e` (line form), or nil
function K.Read(e)
  if not (e and valid(e)) then return nil end
  local model = try("GetVehicleModelName", e.GetVehicleModelName, e)
  if not model or model == "" then return nil end
  local c1 = try("GetColor", e.GetColor, e, 1)
  local c2 = try("GetColor", e.GetColor, e, 2)
  local plate = try("GetSPZText", e.GetSPZText, e)
  local tint = try("GetWindowTint", e.GetWindowTint, e)
  local inter = try("GetInteriorColorsSet", e.GetInteriorColorsSet, e)
  local dirt, rust = nil, nil  -- not read (scale UNVERIFIED, see above)
  local paint = try("GetPainting", e.GetPainting, e)
  local tun = "-"
  local id = entId(e)
  if mc_car_tuning and id then tun = mc_car_tuning(id) or "-" end
  if type(plate) ~= "string" then plate = nil end
  if type(paint) ~= "string" then paint = nil end
  return string.format("%s %d %d %s %d %d %d %d %s %s", esc(model), int(c1, -1, 32767), int(c2, -1, 32767),
    esc(plate), int(tint, -1, 32767), int(inter, -1, 32767), tenth(dirt), tenth(rust), esc(paint), tun)
end

-- a player's human: ours or a puppet of another player's (not a traffic crew: their copies take the tuning as before)
local function isPlayer(h)
  local good, g = pcall(h.GetGUID, h)
  if not good then return false end
  g = tostring(g)
  local p = M.Player and M.Player()
  if p and valid(p) then
    local gp, pg = pcall(p.GetGUID, p)
    if gp and tostring(pg) == g then return true end
  end
  for _, pp in pairs(M.pup or {}) do
    if pp.guid == g then return true end
  end
  return false
end

-- Which player sits in car `e` (a puppet, our player): the first one found (human, seat), or nil
function K.Occupant(e)
  if not (e and valid(e) and e.GetSeatOwner) then return nil end
  for s = 0, SEATS - 1 do
    local good, h = pcall(e.GetSeatOwner, e, s)
    if not good then break end
    if h ~= nil and valid(h) and isPlayer(h) then return h, s end
  end
  return nil
end

-- True while the dll holds copy entity `id` back from a puppet: its look (tuning) is still to come
function K.Holds(id)
  id = tonumber(id)
  return id ~= nil and K.hold[id] == true and M.now - (K.holdT or -99) < HOLD_FRESH
end

-- "LA ent what ids look...": set what the dll says our copy lacks
function K.Apply(f, ev)
  local e = mc_entity and mc_entity(tonumber(f[2]) or -1)
  if not (e and valid(e) and e.GetVehicleModelName) then return end
  local what = {}
  for w in string.gmatch(f[3] or "", "[^,]+") do what[w] = true end
  local ids = {}
  if f[4] and f[4] ~= "-" then
    for s in string.gmatch(f[4], "%d+") do ids[#ids + 1] = tonumber(s) end
  end
  -- the look: f[5] model, f[6] c1, f[7] c2, f[8] plate, f[9] tint, f[10] interior, f[11] dirt, f[12] rust, f[13] painting
  local res = {}
  local function put(name, fn, ...)
    if not fn then res[#res + 1] = name .. " (no binding)"; return end
    local good, err = pcall(fn, e, ...)
    res[#res + 1] = name .. (good and "" or (" FAILED " .. tostring(err)))
  end
  if what.color then put("colours", e.SetColor, tonumber(f[6]), tonumber(f[7])) end
  if what.plate then put("plate", e.SetSPZText, unesc(f[8]), true) end
  if what.tint then put("tint", e.SetWindowTint, tonumber(f[9])) end
  if what.interior then put("interior", e.SetInteriorColorsSet, tonumber(f[10])) end
  if what.dirt then put("dirt", e.SetDirty, (tonumber(f[11]) or 0) / 250) end
  if what.rust then put("rust", e.SetRust, (tonumber(f[12]) or 0) / 250) end
  if what.painting then put("painting", e.SetPainting, unesc(f[13])) end
  if what.tuning and #ids > 0 then
    local occ, seat = K.Occupant(e)
    if occ then
      -- someone got in since the dll's round: InstallTuningItems rebuilds the car (docs/re-notes.md 13) and would
      -- put him out. Left out: the next rounds report the copy busy and the dll sets it once it is empty.
      res[#res + 1] = "tuning " .. table.concat(ids, ",") .. " deferred (seat " .. tostring(seat) .. " taken)"
    else
      -- one call with every item (the binding takes them as separate numbers: InstallTuningItems(a, b, ...))
      local before = #res
      put("tuning " .. table.concat(ids, ","), e.InstallTuningItems, (table.unpack or unpack)(ids))
      if e.InstallTuningItems and res[before + 1] and not res[before + 1]:find("FAILED", 1, true) and ev then
        ev[#ev + 1] = "LR " .. tostring(f[2])  -- rebuilt: the dll tells car damage
      end
    end
  end
  log(string.format("car entity %s: %s", tostring(f[2]), table.concat(res, ", ")))
  if mc_trace then mc_trace("CLW", string.format("ent=%s what=%s ids=%s res=%s", tostring(f[2]), tostring(f[3]),
    tostring(f[4]), (table.concat(res, ";"):gsub(" ", "_")))) end
end

local function emit(ev, ref, e, role, owner)
  local id = entId(e)
  local look = id and K.Read(e)
  if not look then return false end
  if role == "C" and K.Occupant(e) then role = "B" end  -- a copy someone sits in: its tuning waits
  ev[#ev + 1] = string.format("LC %s %d %s %s %s", ref, id, role, owner or "-", look)
  return true
end

function K.Round(ev)
  local me = tonumber(M.localId or "0") or 0
  local n, src, cop = 0, 0, 0
  for key, c in pairs(M.cars or {}) do
    -- a motorbike (player_sync.lua c.bike, docs/game-api.md 25): not a C_WrapperCar, its look is not synced
    if (c.st == "ready" or c.st == "lost") and not (c.bike or c.bikeHint) then
      local owner = tonumber(key:match("^(%d+):"))
      if owner == me then
        -- our car (the key is ours: we drove it first): its look goes out from the car itself, also while it is
        -- hidden behind a copy (another player drives it); that copy here takes it like any copy
        local real = (c.orig and valid(c.orig) and c.orig) or (not c.qid and valid(c.ent) and c.ent) or nil
        if real and emit(ev, key, real, "S", me) then n, src = n + 1, src + 1 end
        if c.qid and c.st == "ready" and valid(c.ent) and emit(ev, key, c.ent, "C", me) then n, cop = n + 1, cop + 1 end
      elseif c.qid and c.st == "ready" and valid(c.ent) then
        -- a copy we spawned of another game's car (an adopted car of ours is real: never changed)
        if emit(ev, key, c.ent, "C", owner) then n, cop = n + 1, cop + 1 end
      end
    end
  end
  local T = M.Traffic
  if T and not K.host then
    local alive = {}
    for net, c in pairs(T.copies or {}) do
      if c.st == "ready" and valid(c.ent) then
        alive[net] = true
        K.born[net] = K.born[net] or M.now
        local every = (M.now - K.born[net] < FRESH) and ROUND or TRAFFIC_EVERY
        if M.now - (K.read[net] or -99) >= every - 0.01 then
          K.read[net] = M.now
          if emit(ev, "t" .. net, c.ent, "C", 0) then n, cop = n + 1, cop + 1 end
        end
      end
    end
    for net in pairs(K.born) do
      if not alive[net] then K.born[net], K.read[net] = nil, nil end
    end
  end
  if K.host then
    for ref in pairs(K.want) do
      local e = mc_entity and mc_entity(tonumber(ref:sub(2)) or -1)
      if e and valid(e) and e.GetVehicleModelName and emit(ev, ref, e, "S", me) then n, src = n + 1, src + 1 end
    end
  end
  K.want = {}
  ev[#ev + 1] = "LE"
  K.cars, K.src, K.cop = n, src, cop
end

function K.Update(dt, cmds)
  local ev = {}
  for l in string.gmatch(cmds or "", "[^\n]+") do
    local f = {}
    for w in string.gmatch(l, "%S+") do f[#f + 1] = w end
    if f[1] == "S" then
      K.active, K.host = f[2] == "1", f[3] == "1"
    elseif f[1] == "LW" and f[2] then
      K.want[f[2]] = true
    elseif f[1] == "LA" then
      local good, err = pcall(K.Apply, f, ev)
      if not good then log("apply error: " .. tostring(err)) end
    elseif f[1] == "LH" then
      local h = {}
      for i = 2, #f do
        local id = tonumber(f[i])
        if id then h[id] = true end
      end
      K.hold, K.holdT = h, M.now
    end
  end
  if not K.active then
    K.read, K.born, K.want, K.hold = {}, {}, {}, {}
    return ""
  end
  K.t = (K.t or 0) + (dt or 0)
  if K.t >= ROUND then
    K.t = 0
    local good, err = pcall(K.Round, ev)
    if not good then
      errs.round = (errs.round or 0) + 1
      if errs.round <= 5 or errs.round % 500 == 0 then log("round error #" .. errs.round .. ": " .. tostring(err)) end
    end
    K.stT = (K.stT or 0) + ROUND
    if K.stT >= 1 then
      K.stT = 0
      ev[#ev + 1] = string.format("ST cars %d (ours %d, copies %d)", K.cars or 0, K.src or 0, K.cop or 0)
    end
  end
  return table.concat(ev, "\n")
end

return K.version
