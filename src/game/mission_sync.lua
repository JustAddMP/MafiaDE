-- MafiaCoop mission glue (B2 chapters/checkpoints/objectives, B5 cutscenes). Runs in the game's Lua state on
-- GameMainThread after player_sync.lua, called once per script tick by src/game/mission_sync.cpp
-- (MafiaCoop.Mission.Update). Embedded into the dll; a copy at <dll dir>\mission_sync.lua overrides it.
--
-- Input (cmds, one line per command):
--   S active host following        session running, we host, we (guest) follow the host's story (1/0 each)
--   H quest cp obj override inWorld  the host's chapter, checkpoint, objective and override objective ("-" = none)
--   C name                            the cutscene playing on the host ("-" = none)
--   Q                                 guest: quit to the main menu (the host reached a checkpoint we load next)
--                                     (also: a failed mission, host or guest: straight back to the checkpoint)
--   QC                                forget a Q not carried out yet (the game loads another world itself)
--   B                                 fade the screen to black (a failed mission: this world is left at once)
--   T quest cp [x y z dx dy]          the chapter and checkpoint our last chapter load is for (guest following the
--                                     host, or the host's lobby chapter start): its quest is started once in the world,
--                                     after the player was put where the save has him (x y z, heading dx dy)
-- Output: events, one per line:
--   "GS quest cp hold cutscene"       our chapter and checkpoint, 1 = held while the host watches a cutscene we
--                                     cannot play, 1 = a cutscene plays here (twice a second)
--   "MS quest cp obj override"        host: our story state (twice a second; the dll sends it on change)
--   "CS name"                         host: the cutscene that plays here ("-" = none)
--   "AC quest cp"                     the T quest was started (or already ran): the dll forgets it
--   "LS 1|0"                          a loading screen is up (twice a second, also outside the world)
-- Sets MafiaCoop.cutscene (true while a cutscene plays here or the host's holds us): player_sync.lua hides the
-- puppets meanwhile.
MafiaCoop = MafiaCoop or {}
local M = MafiaCoop
M.Mission = M.Mission or {}
local MS = M.Mission
MS.version = 1

local function log(s) if mc_log then mc_log("mission: " .. s) end end
local function valid(o) return o ~= nil and IsObjectValid(o) end
local function dash(s) return (s == nil or s == "") and "-" or tostring(s) end
local function undash(s) return (s == nil or s == "-") and "" or s end

-- Cutscene objects per chapter (<name>_cs_object in the chapter's primary SDS; docs/research-world-sync.md B5).
local CUTSCENES = {
  mm_10_chase = { "cine_0005_prologue_bik", "cine_0100_chase_bik", "cine_0110_chase_reward" },
  mm_20_taxi = { "cine_0200_taxi", "cine_0270_taxi_salireris" },
  mm_30_molotov = { "cine_0300_molotov_intro", "cine_0310_molotov_get_guns", "cine_0320_molotov_get_car",
    "cine_0330_molotov_outro" },
  mm_40_motel = { "cine_0400_motel_intro", "cine_0420_motel_get_car", "cine_0450_motel_arrive",
    "cine_0460_motel_gorilla", "cine_0490_intermission_one", "cine_0495_intermission_one_morello_bik",
    "cine_0497_intermission_one" },
  mm_50_race = { "cine_0500_race_intro", "cine_0510_race_ralphy", "cine_0540_race_tuning", "cine_0560_race_driver",
    "cine_0565_race_finish_bik", "cine_0570_race_win", "cine_0592_race_paulie" },
  mm_60_sarah = { "cine_0600_sarah_intro", "cine_0610_sarah_punks", "cine_0620_sarah_fight",
    "cine_0625_sarah_fight_win", "cine_0630_sarah_outro" },
  mm_70_hoodlums = { "cine_0700_hoodlums", "cine_0705_hoodlums_get_car", "cine_0730_hoodlums_backalley",
    "cine_0750_hoodlums_chase", "cine_0760_hoodlums_kill" },
  mm_80_brothel = { "cine_0800_brothel", "cine_0830_brothel_michelle", "cine_0840_brothel_escape",
    "cine_0847_brothel_jump", "cine_0850_brothel_funeral", "cine_0860_brothel_priest", "cine_0995_intermission_two",
    "cine_0997_intermission_two_frank_bik" },
  mm_100_farm = { "cine_1000_farm", "cine_1010_farm_get_car", "cine_1020_farm_trucks", "cine_1030_farm_ambush",
    "cine_1040_farm_search", "cine_1050_farm_sam", "cine_1060_farm_company", "cine_1062_farm_chase_split",
    "cine_1063_farm_chase_dead1", "cine_1064_farm_chase_stuck", "cine_1065_farm_chase_dead2",
    "cine_1066_farm_chase_free", "cine_1067_farm_chase_turret_down", "cine_1068_farm_chase_molotovs",
    "cine_1069_farm_chase_victory", "cine_1070_farm_outro", "cine_1071_farm_chase_dead3", "cine_1075_farm_warehouse",
    "cine_1095_farm_sarah" },
  mm_110_omerta = { "cine_1100_omerta", "cine_1130_omerta_cops", "cine_1140_omerta_cops_arrive",
    "cine_1160_omerta_confrontation", "cine_1195_omerta_funeral_bik" },
  mm_120_mansion = { "cine_1200_mansion_bik", "cine_1250_mansion_safe" },
  mm_130_parking = { "cine_1300_parking", "cine_1320_parking_ambush", "cine_1325_parking_leave",
    "cine_1330_parking_outro", "cine_1395_intermission_three" },
  mm_140_salieri = { "cine_1400_salieri", "cine_1410_salieri_ambush1_bik", "cine_1415_salieri_ambush1_bik",
    "cine_1420_salieri_ambush2", "cine_1430_salieri_carlos", "cine_1440_salieri_outro", "cine_1495_morello" },
  mm_150_boat = { "cine_1500_boat", "cine_1530_boat_sailor_costume", "cine_1550_boat_depart",
    "cine_1575_boat_get_gun", "cine_1580_boat_speech_bik", "cine_1590_boat_outro_bik" },
  mm_160_harbor = { "cine_1600_harbor_bik", "cine_1602_harbor_bomb", "cine_1630_harbor_dame",
    "cine_1640_harbor_restaurant", "cine_1650_harbor_bike", "cine_1670_harbor_port", "cine_1680_harbor_kill_sergio" },
  mm_170_plane = { "cine_1700_plane_bik", "cine_1710_plane_plane_late", "cine_1750_plane_airport",
    "cine_1760_plane_shoot", "cine_1770_plane_crash", "cine_1790_plane_end", "cine_1795_intermission_four" },
  mm_180_sniper = { "cine_1800_sniper", "cine_1810_sniper_get_gun", "cine_1890_sniper_sarah" },
  mm_190_cigars = { "cine_1900_cigars", "cine_1930_cigars_little_job_bik", "cine_1950_cigars_hide",
    "cine_1960_cigars_load_crates", "cine_1990_cigars_outro", "cine_1995_cigar_train_paulie_bik" },
  mm_200_bank = { "cine_2000_bank_intro", "cine_2050_bank_robbery", "cine_2060_bank_safe", "cine_2070_bank_success" },
  mm_210_gallery = { "cine_2100_gallery_sarah", "cine_2110_gallery_pauli_dead", "cine_2120_gallery_round_one",
    "cine_2130_gallery_round_two", "cine_2150_gallery_outro", "cine_2200_the_end", "cine_2210_the_end_prison_bik" },
}
MS.CUTSCENES = CUTSCENES

-- The C_RuntimeCutsceneComponent of a cutscene object (nil when its chapter part is not loaded here).
function MS.Comp(name)
  local good, o = pcall(game.entitywrapper.GetSceneObjectByName, game.entitywrapper, name .. "_cs_object")
  if not (good and valid(o)) then return nil end
  local ok, c = pcall(o.GetComponent, o, "C_RuntimeCutsceneComponent")
  return ok and c or nil
end

local function playing(c)
  local good, v = pcall(c.IsCutscenePlaybackInProgress, c)
  return good and v == true
end

-- Our chapter (quest) and checkpoint (the quest's custom state).
function MS.Point()
  local good, q = pcall(GetActiveQuest)
  if not good or not q or q == "" then return nil, nil end
  local ok, cp = pcall(game.director.QuestGetCustomState, game.director, q)
  return tostring(q), ok and cp and tostring(cp) or nil
end

-- The cutscene of chapter `q` that plays here, or nil.
function MS.Playing(q)
  for _, n in ipairs(CUTSCENES[q or ""] or {}) do
    local c = MS.Comp(n)
    if c and playing(c) then return n end
  end
  return nil
end

-- ---- guest: objectives ----------------------------------------------------------------------------------------------
-- The game shows an objective from its id (the HUD looks its text up: "mm_110_omerta_cp_020" = "Talk to Vincenzo"),
-- so the host's ids are applied with the game's own HUD_* helpers (they keep active_objective etc. consistent).
function MS.Objectives(h)
  if M.now - (MS.objT or -10) < 1 then return end
  MS.objT = M.now
  if h.obj ~= "" and tostring(active_objective or "") ~= h.obj then
    local good, err = pcall(HUD_AddQuestObjective, h.obj)
    log("objective -> " .. h.obj .. (good and "" or (" failed: " .. tostring(err))))
  end
  local ov = tostring(override_objective or "")
  if ov ~= h.ov then
    local good, err
    if h.ov == "" then good, err = pcall(HUD_RemoveQuestOverrideObjective)
    else good, err = pcall(HUD_AddQuestOverrideObjective, h.ov) end
    log("override objective -> " .. (h.ov == "" and "none" or h.ov) .. (good and "" or (" failed: " .. tostring(err))))
  end
end

-- ---- guest: cutscenes -------------------------------------------------------------------------------------------------
-- The host's cutscene is played on our own copy of its object (same chapter part loaded); if we cannot (other part,
-- not loaded within 15 s) or we skipped ours, we are held: controls locked until the host's ends. When it ends (or
-- the host skips it) ours is stopped, the screen faded in, and we are put next to the host.
function MS.Hold(p, on)
  if (MS.held or false) == on then return end
  MS.held = on
  if p then pcall(p.LockControls, p, on) end
  log(on and "held: the host watches a cutscene we do not play" or "released")
end

function MS.CutsceneGuest(p, name)
  local cs = MS.cs
  if name ~= "" and (not cs or cs.name ~= name) then
    if cs then MS.EndCutscene(p) end
    cs = { name = name, st = "held", t = M.now }
    MS.cs = cs
    local c = MS.Comp(name)
    if c then
      cs.c = c
      if playing(c) then
        cs.st = "playing"  -- our own mission flow started it too
        log("host cutscene " .. name .. ": already playing here")
      else
        local good, err = pcall(c.LoadCutscene, c)
        cs.st = good and "loading" or "held"
        log("host cutscene " .. name .. ": loading ours" .. (good and "" or (" failed: " .. tostring(err))))
      end
    else
      log("host cutscene " .. name .. ": not in our world, holding")
    end
  elseif name == "" and cs then
    MS.EndCutscene(p)
    return
  end
  if not cs then return end
  if not cs.c then
    -- not in our world yet (our quest starts a moment after the load): look again; our own mission flow may start
    -- the same cutscene itself
    local c = MS.Comp(name)
    if c then
      cs.c, cs.t = c, M.now
      if playing(c) then
        cs.st = "playing"
        log("host cutscene " .. name .. ": playing here too")
      elseif pcall(c.LoadCutscene, c) then
        cs.st = "loading"
        log("host cutscene " .. name .. ": loading ours")
      end
    end
  end
  if cs.st == "loading" then
    local good, done = pcall(cs.c.IsCutsceneLoadFinished, cs.c)
    if good and done then
      local ok, so = pcall(cs.c.PlayCutscene2, cs.c, "", "", "", "")
      cs.st, cs.ours = ok and "playing" or "held", ok
      log("host cutscene " .. name .. ": " .. (ok and "playing ours" or ("play failed: " .. tostring(so))))
    elseif M.now - cs.t > 15 then
      cs.st = "held"
      log("host cutscene " .. name .. ": not loaded in 15 s, holding")
    end
  elseif cs.st == "playing" and not playing(cs.c) and M.now - cs.t > 2 then
    cs.st = "held"  -- skipped or finished before the host's: wait for him
  end
  MS.Hold(p, cs.st == "held")
end

function MS.EndCutscene(p)
  local cs = MS.cs
  MS.cs = nil
  if not cs then return end
  if cs.c then
    if playing(cs.c) then pcall(cs.c.StopCutscene, cs.c) end
    if cs.ours then pcall(cs.c.UnloadCutscene, cs.c) end
  end
  pcall(HUD_FaderFadeIn, 0.5)
  MS.Hold(p, false)
  MS.placeAt = M.now + 1.5  -- then next to the host
  log("host cutscene " .. cs.name .. " ended: ours stopped")
end

-- ---- guest: start the host's mission ---------------------------------------------------------------------------------
-- The chapter loader brings the host's save in without its quest running (QuestIsAvailable false: no mission flow,
-- objectives, mission NPCs or cutscene objects). Selecting the checkpoint (the quest's custom state) and making the
-- quest available starts the chapter's flow at that checkpoint, like the game's own debug "unlockquest".
-- A load through the chapter loader starts the player at the Free Ride start, not where the save has him
-- (mission_sync.cpp SaveSpotIn): a checkpoint whose setup does not move him (Omerta cp_020) then began at Salieri's bar,
-- outside its mission area, and its leave-the-area check never armed. So first he is put where the save has him (not
-- on the first ticks after a load: that crashed), then the quest is started there a second later.
function MS.Activate(t, p)
  if not M.inWorldAt or M.now - M.inWorldAt < 1 then return end  -- not on the first ticks after the load
  local key = tostring(M.inWorldAt) .. "|" .. t.quest .. "|" .. t.cp
  if MS.activeKey == key then return end
  if t.spot then
    if MS.spotKey ~= key then
      if M.now - M.inWorldAt < 3 then return end
      MS.spotKey, MS.spotT = key, M.now
      local s = t.spot
      local from = p:GetPos()
      local okOwner, car = pcall(p.GetOwner, p)
      if okOwner and car then  -- seated in a car the load gave us: that is where the save has us anyway
        log("in a car after the load: not moved")
        return
      end
      local good, err = pcall(function()
        p:SetPos(Math:newVector(s[1], s[2], s[3] + 0.1))
        p:SetDir(Math:newVector(s[4], s[5], 0))
      end)
      log(string.format("put where the save has us: %.1f %.1f %.1f (were at %.1f %.1f)%s", s[1], s[2], s[3], from.x,
        from.y, good and "" or (" failed: " .. tostring(err))))
      return
    end
    if M.now - (MS.spotT or 0) < 1 then return end
  end
  MS.activeKey = key
  local d = game.director
  MS.events[#MS.events + 1] = "AC " .. t.quest .. " " .. dash(t.cp)
  local good, avail = pcall(d.QuestIsAvailable, d, t.quest)
  if good and avail then return end
  local ok1, e1 = true, nil
  if t.cp ~= "" then ok1, e1 = pcall(d.QuestSetCustomState, d, t.quest, t.cp) end
  local ok2, e2 = pcall(d.QuestSetAvailable, d, t.quest)
  log("started the host's mission " .. t.quest .. " at " .. t.cp .. ((ok1 and ok2) and "" or
    (" failed: " .. tostring(e1) .. " " .. tostring(e2))))
end

-- ---- guest: next to the host after every load and cutscene --------------------------------------------------------------
function MS.Place(p)
  if M.inWorldAt and M.now - M.inWorldAt > 3 then
    local key = tostring(game.game:GetLoadedSaveId()) .. "|" .. tostring(M.inWorldAt)
    if MS.placedKey ~= key and M.NearHost and M.NearHost(p) then
      MS.placedKey = key
      log("placed next to the host after the load")
    end
  end
  if MS.placeAt and M.now >= MS.placeAt and M.NearHost then
    MS.placeAt = nil
    if M.NearHost(p) then log("placed next to the host after the cutscene") end
  end
end

function MS.Update(dt, cmds)
  local ev = {}
  MS.events = ev
  local active, host, following, h, hostCut, quit, target = false, false, false, nil, nil, false, nil
  local blackout, cancelQuit = false, false
  for line in string.gmatch(cmds, "[^\n]+") do
    local f = {}
    for w in string.gmatch(line, "%S+") do f[#f + 1] = w end
    if f[1] == "S" then
      active, host, following = f[2] == "1", f[3] == "1", f[4] == "1"
    elseif f[1] == "H" then
      h = { quest = undash(f[2]), cp = undash(f[3]), obj = undash(f[4]), ov = undash(f[5]), inWorld = f[6] == "1" }
    elseif f[1] == "C" then
      hostCut = undash(f[2])
    elseif f[1] == "Q" then
      quit = true
    elseif f[1] == "QC" then
      cancelQuit = true
    elseif f[1] == "B" then
      blackout = true
    elseif f[1] == "T" then
      target = { quest = f[2], cp = undash(f[3]) }
      local s = { tonumber(f[4]), tonumber(f[5]), tonumber(f[6]), tonumber(f[7]), tonumber(f[8]) }
      if s[1] and s[2] and s[3] and s[4] and s[5] then target.spot = s end
    end
  end
  if blackout and MS.blackKey ~= tostring(M.inWorldAt) then
    -- a failed mission (the world we leave in a moment: the failed one, or the Free Ride stop of the game's own
    -- fail load): the player is not meant to see more of it
    MS.blackKey = tostring(M.inWorldAt)
    local good, err = pcall(HUD_FaderFadeOut, 0.2)
    log("failed mission: screen faded out" .. (good and "" or (" failed: " .. tostring(err))))
  end
  if cancelQuit and MS.quitAt then
    MS.quitAt = nil
    log("quit to the main menu cancelled (the game loads another world itself)")
  end
  if quit and not MS.quitAt then
    -- Leave the world clean first: puppets, their map markers, the puppet factory's car and our car copies are
    -- removed and kept away (M.leaving) until the main menu is up; quitting with them still there crashed the next
    -- chapter load in the save system (exe+0x2e81ef8).
    pcall(M.RemoveAll)
    M.leaving = true
    MS.quitAt = M.now + 1
    log("leaving the world to load the host's checkpoint")
  end
  if MS.quitAt and M.now >= MS.quitAt then
    MS.quitAt = nil
    local good, err = pcall(game.hud.QuitToMainMenu, game.hud)
    log("quit to the main menu" .. (good and "" or (" failed: " .. tostring(err))))
  end
  if M.now - (MS.lsT or -10) >= 0.5 then
    MS.lsT = M.now
    local good, up = pcall(game.hud.IsLoadingScreenUp, game.hud)
    ev[#ev + 1] = "LS " .. ((good and up) and "1" or "0")
  end
  local p = M.Player and M.Player()
  if not p then
    if MS.held then MS.held = nil end  -- the world (and our lock) went away
    MS.cs = nil
    M.cutscene = nil
    return table.concat(ev, "\n")
  end
  if active and target then
    local good, err = pcall(MS.Activate, target, p)
    if not good then log("activate error: " .. tostring(err)) end
  end
  if M.now - (MS.pollT or -10) >= 0.25 then
    MS.pollT = M.now
    MS.q, MS.cp = MS.Point()
    MS.localCut = MS.Playing(MS.q)
  end
  if active and not host and following and h and h.inWorld then
    if hostCut then
      local good, err = pcall(MS.CutsceneGuest, p, hostCut)
      if not good then log("cutscene error: " .. tostring(err)) end
    end
    if h.quest == MS.q then
      local good, err = pcall(MS.Objectives, h)
      if not good then log("objective error: " .. tostring(err)) end
    end
    if not MS.cs and not target then pcall(MS.Place, p) end  -- after our quest start (it may move us first)
  elseif MS.cs or MS.held then
    MS.EndCutscene(p)
  end
  M.cutscene = (MS.localCut ~= nil) or (MS.cs ~= nil) or nil
  if M.now - (MS.evT or -10) >= 0.5 then
    MS.evT = M.now
    ev[#ev + 1] = "GS " .. dash(MS.q) .. " " .. dash(MS.cp) .. " " .. (MS.held and "1" or "0") .. " " .. (MS.localCut and "1" or "0")
    if active and host then
      ev[#ev + 1] = "MS " .. dash(MS.q) .. " " .. dash(MS.cp) .. " " .. dash(active_objective) .. " " .. dash(override_objective)
      ev[#ev + 1] = "CS " .. dash(MS.localCut)
    end
  end
  return table.concat(ev, "\n")
end

return MS.version
