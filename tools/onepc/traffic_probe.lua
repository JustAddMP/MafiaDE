-- Traffic drivers and copies right now (tools/onepc run_test.ps1 "traffic"): on the host (the only traffic authority),
-- his traffic cars by distance from his player and whether a driver sits at the wheel (the game streams drivers out
-- beyond ~50 m: traffic_sync.lua DRIVER_LOD); on a guest, his copies of the host's cars: driver seated, crew out (and
-- the totals of crews sent out / put back), the copies within 40 m apart (beyond, his game streams their crews out as
-- its own traffic's drivers: not put back, onepc-fixes); a guest's "ours" must be 0 everywhere (he shares no traffic).
-- One line back.
local M = MafiaCoop
local T = M and M.Traffic
if not T then return "no MafiaCoop.Traffic" end
local p = game.game:GetActivePlayer()
local me = p and p:GetPos()
if not me then return "no player" end
local function d2(q) return math.sqrt((q.x - me.x) ^ 2 + (q.y - me.y) ^ 2) end
-- ours: [band] = {cars, without a driver}
local bands = { { 0, 40 }, { 40, 60 }, { 60, 1e9 } }
local own = {}
for i = 1, #bands do own[i] = { 0, 0 } end
for _, c in pairs(T.list or {}) do
  if c.e and IsObjectValid(c.e) then
    local d = d2(c.e:GetPos())
    local okd, drv = pcall(c.e.GetSeatOwner, c.e, 0)
    for i, b in ipairs(bands) do
      if d >= b[1] and d < b[2] then
        own[i][1] = own[i][1] + 1
        if not (okd and drv and IsObjectValid(drv)) then own[i][2] = own[i][2] + 1 end
      end
    end
  end
end
local copies, seated, out, near, nearSeated = 0, 0, 0, 0, 0
for _, c in pairs(T.copies or {}) do
  if c.st == "ready" and c.ent and IsObjectValid(c.ent) then
    copies = copies + 1
    local okd, drv = pcall(c.ent.GetSeatOwner, c.ent, 0)
    local sits = okd and drv and IsObjectValid(drv)
    if sits then seated = seated + 1 end
    if d2(c.ent:GetPos()) < 40 then
      near = near + 1
      if sits then nearSeated = nearSeated + 1 end
    end
    if c.crewOut then out = out + 1 end
  end
end
local parts = {}
for i, b in ipairs(bands) do
  parts[#parts + 1] = string.format("%s-%s m %d (no driver %d)", b[1], b[2] < 1e9 and b[2] or "", own[i][1], own[i][2])
end
return string.format("ours: %s; copies %d: driver seated %d (within 40 m: %d of %d), crew out %d; crews out so far %d, "
  .. "put back %d", table.concat(parts, ", "), copies, seated, nearSeated, near, out, T.outs or 0, T.reseats or 0)
