-- MafiaCoop world items glue: weapons lying on the ground (what dead enemies drop) and the ground pickups (inventory
-- items such as medkits), shared between the games; collectibles reported. Runs in the game's Lua state on
-- GameMainThread after player_sync.lua, called once per script tick by src/game/item_sync.cpp
-- (MafiaCoop.Items.Update). Embedded into the dll; a copy at <dll dir>\item_sync.lua overrides it.
--
-- The decisions are made in C++ (src/game/world_items.h, unit-tested): this file only reads the world and carries
-- out commands. Every SCAN_INTERVAL it lists the item entities of the game's item manager (entity types 63
-- ITEM_WEAPON and 66 ITEM_INVENTORY, mc_entity_ids) near a viewpoint (our player; on the host also every remote
-- player's puppet), and reports those that vanished (the entity gone or switched off) since the last scan, with the
-- weapons our player gained in the last GAIN_WINDOW s (for a take the host refuses). The C++ side pairs them with the
-- host's items, decides who took what and tells us to hide or move ours, or to give back a weapon.
-- Collectibles (cigarette cards, pulp magazines, comics, postcards; the game's collectibles.lua: entities named
-- lh_pickup_<kind>_<id>_bv01): each player collects his own, for his own save, and they are never hidden on the
-- other games (a guest's saves are redirected away anyway, so nothing a guest does changes the host's save); when ours
-- goes (it is switched off by the game's own pickup) next to us, the others hear about it (Collectible).
--
-- Input (cmds, one line per command):
--   S active host localId                  session running, we host, our player id
--   HIDE id why                            hide our item id (it was taken on another game / the host has none)
--   MOVE id x y z                          put our item id there (where the host's item lies)
--   UNDO name,name,...                     a take of ours was refused: take these weapons away again
--   COL from name                          player `from` found that collectible on his game
-- Output (events, one per line):
--   IW                                     a new world (the books start over)
--   IR x y z age                           a round of our items starts: our player's position, s since our world loaded
--   IL id type x y z name                  one of our items (in this round)
--   IG id gained                           our item vanished (gained: weapons we got in the last GAIN_WINDOW s, or -)
--   CF name                                we collected that collectible
--   ST text                                status for the overlay (once a second)
MafiaCoop = MafiaCoop or {}
local M = MafiaCoop
M.Items = M.Items or {}
local I = M.Items
I.version = 1

local SCAN_INTERVAL = 0.25  -- s between item rounds (the entity table scan, mc_entity_ids; was 0.5)
local RANGE = 100           -- m from a viewpoint: items reported
local TYPES = { 63, 66 }    -- EntityType ITEM_WEAPON, ITEM_INVENTORY
local GAIN_WINDOW = 2.0     -- s: weapons gained this recently count for a take
local COL_INTERVAL = 1.0    -- s between collectible checks
local COL_NEAR = 4          -- m: a collectible switched off this close to us was collected by us
local HIDE_TRIES = 3        -- a hidden weapon still there: ClearWeaponsInRadius, at most this often

-- The game's collectible names (collectibles.lua): lh_pickup_<kind>_<id>_bv01
local COLLECTIBLES = {
  bm = {"1923_01", "1929_06", "1930_02", "1931_05", "1931_06", "1931_12", "1932_05", "1933_01", "1933_03", "1933_06",
    "1933_09", "1933_11", "1934_03", "1934_04", "1934_05", "1935_09", "1935_12", "1936_02", "1944_03", "1949_03"},
  cc = {"01_tommy_angelo", "02_paulie_lombardo", "03_sam_trapani", "04_ennio_salieri", "05_frank_colletti",
    "06_vincenzo_ricci", "07_luigi_marino", "08_marcu_morello", "09_sergio_morello", "10_felice_peppone",
    "11_tomaso_moretti", "12_carlo_falcone", "13_frank_vinci", "14_leo_galante", "15_fredo_clemente",
    "16_louie_romero", "17_giuseppe_carillo", "18_valerio_marcano", "19_lou_marcano", "20_sal_marcano",
    "21_lucio_marcano", "22_sammy_robinson"},
  comics = {"a_trip_to_the_country", "don_carillo", "hanoi_hangman", "jimmys_vendetta", "mafia_three", "moonlighting",
    "offer_you_cant_refuse", "racing_racketeer", "sarah_and_tom", "steam_boys"},
  dd = {"1941_02", "1941_05", "1941_07", "1941_08", "1941_10", "1942_04", "1942_12", "1943_02", "1943_03", "1943_04",
    "1943_11", "1944_03", "1944_09", "1944_12", "1945_05", "1946_10", "1946_11", "1947_03", "1947_07", "1947_10"},
  pc = {"disorder", "appolyon", "flame_spear", "manta", "trautenberg"},
  sss = {"1940_07", "1940_09", "1940_11", "1941_01", "1941_03", "1941_05", "1941_08", "1941_11", "1942_02", "1942_05",
    "1942_08", "1942_11", "1943_05", "1949_01", "1949_04", "1949_07", "1949_09", "1950_03", "1950_05", "1950_07"},
  tt = {"1934_09", "1934_11", "1935_01", "1935_03", "1935_09", "1935_11", "1936_05", "1936_09", "1937_07", "1937_09",
    "1938_05", "1938_07", "1939_03", "1939_05", "1939_07", "1939_11", "1940_03", "1940_05", "1940_11", "1941_03"},
}
local KIND_NAMES = { bm = "Black Mask magazine", cc = "cigarette card", comics = "comic", dd = "Dime Detective magazine",
  pc = "postcard", sss = "Super Science Stories magazine", tt = "Terror Tales magazine", fox = "fox" }

local function log(s) if mc_log then mc_log("items: " .. s) end end
local function trace(t, s) if mc_trace then mc_trace(t, s) end end
local function V(x, y, z) return Math:newVector(x, y, z) end
local function valid(o)
  if o == nil then return false end
  local good, v = pcall(IsObjectValid, o)
  return not good or v ~= false
end
local function active(o)
  local good, a = pcall(o.IsActive, o)
  return good and a ~= false
end
local function safe(s)
  s = tostring(s or "")
  s = string.gsub(s, "[^%w_%-%.]", "_")
  if s == "" then return "-" end
  return string.sub(s, 1, 40)
end

I.names = I.names or nil   -- every collectible name (built once)
I.ents = I.ents or {}      -- our items: id -> { o, id, type, name, x, y, z }
I.reported = I.reported or {}  -- ids reported present (IL) and not gone since
I.hidden = I.hidden or {}  -- ids we hid: id -> { t, tries, x, y, z, type }
I.gains = I.gains or {}    -- weapons our player got: { name, t }
I.col = I.col or {}        -- collectibles seen switched on: name -> { x, y, z }
I.n = I.n or { hides = 0, moves = 0, undos = 0, found = 0, theirs = 0, clears = 0 }

local function collectibleNames()
  if I.names then return I.names end
  local t = {}
  for kind, ids in pairs(COLLECTIBLES) do
    for _, id in ipairs(ids) do t[#t + 1] = { name = "lh_pickup_" .. kind .. "_" .. id .. "_bv01", kind = kind } end
  end
  for i = 0, 40 do t[#t + 1] = { name = string.format("lh_pickup_fox_a_bv01_%02d", i), kind = "fox" } end
  I.names = t
  return t
end

local function kindOf(name)
  local k = string.match(name or "", "^lh_pickup_(%a+)_")
  return KIND_NAMES[k or ""] or "collectible"
end

local function viewpoints(p, host)
  local q = p:GetPos()
  local v = { { q.x, q.y } }
  if host then
    for _, pp in pairs(M.pup or {}) do
      if pp.tx then v[#v + 1] = { pp.tx, pp.ty } end
    end
  end
  return v
end

local function near(vps, x, y)
  for _, v in ipairs(vps) do
    if (x - v[1]) ^ 2 + (y - v[2]) ^ 2 <= RANGE * RANGE then return true end
  end
  return false
end

-- The weapons our player got recently (a take the host refuses gives them back).
function I.TrackWeapons(p)
  local now = {}
  local good, list = pcall(function() return { p:GetInventoryWeapons() } end)
  if good and list then
    for _, w in ipairs(list) do now[tostring(w)] = true end
  end
  if I.lastWeapons then
    for w, _ in pairs(now) do
      if not I.lastWeapons[w] then I.gains[#I.gains + 1] = { name = w, t = M.now } end
    end
  end
  I.lastWeapons = now
  local keep = {}
  for _, g in ipairs(I.gains) do
    if M.now - g.t <= GAIN_WINDOW then keep[#keep + 1] = g end
  end
  I.gains = keep
end

local function gained()
  local t = {}
  for _, g in ipairs(I.gains) do t[#t + 1] = safe(g.name) end
  return #t > 0 and table.concat(t, ",") or "-"
end

function I.Scan(p, host, ev)
  if not mc_entity_ids then return end
  local q = p:GetPos()
  I.TrackWeapons(p)
  ev[#ev + 1] = string.format("IR %.2f %.2f %.2f %.1f", q.x, q.y, q.z, M.now - (M.inWorldAt or M.now))
  local vps = viewpoints(p, host)
  local seen, count = {}, 0
  for _, t in ipairs(TYPES) do
    for s in string.gmatch(mc_entity_ids(t), "%d+") do
      local id = tonumber(s)
      seen[id] = true
      local e = I.ents[id]
      if not e or not valid(e.o) then
        local o = mc_entity(id)
        if o then
          local good, n = pcall(o.GetName, o)
          e = { o = o, id = id, type = t, name = safe(good and n or "-") }
        else
          e = nil
        end
        I.ents[id] = e
      end
      if e and not I.hidden[id] and active(e.o) then
        local good, pos = pcall(e.o.GetPos, e.o)
        if good and pos then
          e.x, e.y, e.z = pos.x, pos.y, pos.z
          if near(vps, pos.x, pos.y) then
            ev[#ev + 1] = string.format("IL %d %d %.2f %.2f %.2f %s", id, t, pos.x, pos.y, pos.z, e.name)
            I.reported[id] = true
            count = count + 1
          end
        end
      end
    end
  end
  I.count = count
  local g = nil
  for id, _ in pairs(I.reported) do
    local e = I.ents[id]
    if not seen[id] or not e or not valid(e.o) or not active(e.o) then
      I.reported[id] = nil
      g = g or gained()
      local mine = e and e.type == 63 and g or "-"  -- only a weapon on the ground can have given us a weapon
      ev[#ev + 1] = "IG " .. id .. " " .. mine
      if e then
        local d = e.x and math.sqrt((e.x - q.x) ^ 2 + (e.y - q.y) ^ 2 + (e.z - q.z) ^ 2) or -1
        trace("ITL", string.format("ev=gone id=%d type=%d p=%.2f,%.2f,%.2f d=%.2f name=%s gained=%s", id, e.type,
          e.x or 0, e.y or 0, e.z or 0, d, e.name or "-", mine))
      end
    end
  end
  for id, _ in pairs(I.ents) do
    if not seen[id] then I.ents[id] = nil end
  end
  -- hidden weapons still lying there: the item manager's own clean-up at that spot
  for id, h in pairs(I.hidden) do
    local e = I.ents[id]
    if not e then
      I.hidden[id] = nil
    elseif active(e.o) and M.now - h.t > 0.4 and h.tries < HIDE_TRIES and e.type == 63 and e.x then
      h.tries = h.tries + 1
      local good, err = pcall(game.game.ClearWeaponsInRadius, game.game, V(e.x, e.y, e.z), 0.3)
      I.n.clears = I.n.clears + 1
      log(string.format("item %d still there after Deactivate: ClearWeaponsInRadius %s (try %d)", id,
        good and "done" or ("failed: " .. tostring(err)), h.tries))
      trace("ITL", string.format("ev=clear id=%d ok=%d try=%d", id, good and 1 or 0, h.tries))
    end
  end
end

function I.Hide(id, why)
  local e = I.ents[id]
  if not e or not valid(e.o) then
    log("hide " .. tostring(id) .. ": not in our world any more (" .. why .. ")")
    return
  end
  local good, err = pcall(e.o.Deactivate, e.o)
  I.hidden[id] = { t = M.now, tries = 0 }
  I.reported[id] = nil
  I.n.hides = I.n.hides + 1
  log(string.format("item %d (type %d, %s) at %.1f %.1f %.1f hidden: %s%s", id, e.type, e.name or "-", e.x or 0,
    e.y or 0, e.z or 0, why, good and "" or (", Deactivate failed: " .. tostring(err))))
  trace("ITL", string.format("ev=hide id=%d type=%d p=%.2f,%.2f,%.2f ok=%d active=%d why=%s", id, e.type, e.x or 0,
    e.y or 0, e.z or 0, good and 1 or 0, active(e.o) and 1 or 0, safe(why)))
end

function I.Move(id, x, y, z)
  local e = I.ents[id]
  if not e or not valid(e.o) or not x then return end
  local good, err = pcall(e.o.SetPos, e.o, V(x, y, z))
  local back = nil
  pcall(function() back = e.o:GetPos() end)
  I.n.moves = I.n.moves + 1
  local d = back and math.sqrt((back.x - x) ^ 2 + (back.y - y) ^ 2 + (back.z - z) ^ 2) or -1
  log(string.format("item %d (%s) moved from %.1f %.1f %.1f to the host's spot %.1f %.1f %.1f%s (read back %.2f m off)",
    id, e.name or "-", e.x or 0, e.y or 0, e.z or 0, x, y, z, good and "" or (": SetPos failed: " .. tostring(err)), d))
  trace("ITL", string.format("ev=move id=%d from=%.2f,%.2f,%.2f to=%.2f,%.2f,%.2f ok=%d off=%.2f", id, e.x or 0,
    e.y or 0, e.z or 0, x, y, z, good and 1 or 0, d))
end

function I.Undo(p, names)
  for w in string.gmatch(names or "", "[^,]+") do
    local had = nil
    pcall(function() had = p:InventoryHaveWeapon(w) end)
    local good, err = pcall(p.InventoryRemoveWeapon, p, w)
    local still = nil
    pcall(function() still = p:InventoryHaveWeapon(w) end)
    I.n.undos = I.n.undos + 1
    log(string.format("another player took that item first: %s taken away again (had %s, %s, still %s)", w,
      tostring(had), good and "removed" or ("failed: " .. tostring(err)), tostring(still)))
    trace("ITL", string.format("ev=undo weapon=%s had=%s ok=%d still=%s", w, tostring(had), good and 1 or 0,
      tostring(still)))
    if I.lastWeapons then I.lastWeapons[w] = nil end
  end
end

-- Collectibles: a switched-on one that goes off next to us was collected by us (the game's own pickup switches it
-- off); one that goes off far away was streamed out.
function I.Collectibles(p, ev)
  local q = p:GetPos()
  for _, c in ipairs(collectibleNames()) do
    local good, o = pcall(game.entitywrapper.GetEntityByName, game.entitywrapper, c.name)
    local on = good and o ~= nil and valid(o) and active(o)
    local was = I.col[c.name]
    if on then
      local ok, pos = pcall(o.GetPos, o)
      if ok and pos then I.col[c.name] = { x = pos.x, y = pos.y, z = pos.z } end
    elseif was then
      I.col[c.name] = nil
      local d = math.sqrt((was.x - q.x) ^ 2 + (was.y - q.y) ^ 2 + (was.z - q.z) ^ 2)
      if d <= COL_NEAR then
        I.n.found = I.n.found + 1
        ev[#ev + 1] = "CF " .. c.name
        log(string.format("we collected %s (%s, %.1f m from us)", c.name, kindOf(c.name), d))
        trace("ITL", string.format("ev=collect name=%s d=%.2f", c.name, d))
      end
    end
  end
end

function I.Update(dt, cmds)
  local ev = {}
  local isActive, host = false, false
  local todo = {}
  for line in string.gmatch(cmds, "[^\n]+") do
    local f = {}
    for w in string.gmatch(line, "%S+") do f[#f + 1] = w end
    if f[1] == "S" then
      isActive, host = f[2] == "1", f[3] == "1"
    else
      todo[#todo + 1] = f
    end
  end
  local p = M.Player and M.Player()
  local key = p and (tostring(game.game:GetLoadedSaveId()) .. "|" .. tostring(M.inWorldAt)) or nil
  if key ~= I.worldKey then  -- another world: nothing carries over
    I.worldKey = key
    I.ents, I.reported, I.hidden, I.gains, I.col = {}, {}, {}, {}, {}
    I.lastWeapons = nil
    I.scanT, I.colT = -10, M.now  -- the first collectible check a second in (the game switches off collected ones)
    if key then ev[#ev + 1] = "IW" end
  end
  if not (isActive and p) then return table.concat(ev, "\n") end
  for _, f in ipairs(todo) do
    local good, err = true, nil
    if f[1] == "HIDE" then
      good, err = pcall(I.Hide, tonumber(f[2]), table.concat(f, " ", 3))
    elseif f[1] == "MOVE" then
      good, err = pcall(I.Move, tonumber(f[2]), tonumber(f[3]), tonumber(f[4]), tonumber(f[5]))
    elseif f[1] == "UNDO" then
      good, err = pcall(I.Undo, p, f[2])
    elseif f[1] == "COL" then
      I.n.theirs = I.n.theirs + 1
      I.lastCol = string.format("player %s found a %s", f[2] or "?", kindOf(f[3]))
      log(string.format("player %s found %s (%s) on his game; ours stays for our own save", f[2] or "?",
        f[3] or "?", kindOf(f[3])))
    end
    if not good then log(f[1] .. " error: " .. tostring(err)) end
  end
  if M.now - (I.scanT or -10) >= SCAN_INTERVAL then
    I.scanT = M.now
    local good, err = pcall(I.Scan, p, host, ev)
    if not good then log("scan error: " .. tostring(err)) end
  end
  if M.now - (I.colT or -10) >= COL_INTERVAL then
    I.colT = M.now
    local good, err = pcall(I.Collectibles, p, ev)
    if not good then log("collectibles error: " .. tostring(err)) end
  end
  if M.now - (I.stT or -10) >= 1 then
    I.stT = M.now
    local n = I.n
    ev[#ev + 1] = string.format("ST items here %d, hidden %d, moved %d, given back %d; collectibles ours %d, theirs %d%s",
      I.count or 0, n.hides, n.moves, n.undos, n.found, n.theirs, I.lastCol and (" (" .. I.lastCol .. ")") or "")
  end
  return table.concat(ev, "\n")
end

return I.version
