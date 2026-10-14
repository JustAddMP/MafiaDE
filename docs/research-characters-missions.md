# Research: characters/outfits (F1) and missions/chapters/cutscenes (B2, B5)

Static research only (2026-10-14). No game launch. Exe = Steam build, CRC32 3168979183. Sources: the exe
(disassembly of the Lua binding thunks with capstone), the game's own data (SDS archives unpacked with the game's
`oo2core_8_win64.dll`, the game's own HKS Lua bytecode), the shipped saves in `edit\saves`, and the ScriptHook
`PlayerModelIDs.lua` / trainer `Player.lua` as API reference. Clean-room: nothing from other multiplayer mods.
Everything here is **[static]** until the live-test scripts below have been run; tags as in `re-notes.md`
(**[confirmed]** several independent indicators, **[likely]** one strong indicator, **[guess]**).

Scratch tooling (not checked in) lives in `%TEMP%\claude\G--MafiaCoop\re2`: `sds.py` (SDS v20 unpacker), `split.py`
(splits `tables\scripts.sds` into its 122 HKS chunks and dumps their constants into `lua\NNN_<path>.txt`), `lib.py` +
`ad.py`/`reg.py` (disassembler, `luaL_Reg` table dumper), `profiles_recovered.txt` (198 spawn profile names with their
hashes). How to redo it is in "Method" at the end.

---------------------------------------------------------------------------------------------------------------

## F1. Customisable characters

### Summary

- **A character/outfit in this engine is a "spawn profile"**, a 64-bit name hash looked up in
  `/combinables/spawnprofiles` (`sds_retail\combinables\spawnprofiles.sds`, resource `C_SpawnProfileDB`). The hash is
  **FNV-1 64** (multiply, then xor; offset basis 0xcbf29ce484222325, prime 0x100000001b3) of the lower-case path. The
  DB holds exactly the 2040 hashes listed in ScriptHook's `PlayerModelIDs.lua` (all 2040 found in the unpacked DB).
  Names are stripped from the DB; **198 names were recovered** by dictionary attack (list in the appendix), including
  every story character: Paulie, Sam, Salieri, Frank, Vincenzo, Luigi, Ralph, Sarah, Morello, Sergio, Norman,
  Salvatore, Bertone, Biff, Billy, plus Tommy's 40 outfits and the free-ride wardrobe.
- **The local player's model can be swapped at runtime** [confirmed statically]: `C_WrapperPlayer` has four switches
  (`SwitchPlayerSpawnProfile`, `SwitchDefaultPlayerSpawnProfile`, `SwitchOverridePlayerSpawnProfile`,
  `ClearOverridePlayerSpawnProfile`). The game's own scripts use them for checkpoints (default) and free-ride /
  missions (override), and the ScriptHook trainer's "Change Model" uses the default switch with a decimal string.
  **Every hash argument accepts a plain string**: a path is hashed, and a string of decimal digits is taken as the
  raw 64-bit value (see Evidence 2). A Lua number is rejected.
- **Outfits** are a separate layer, `game.outfits` (`C_OutfitDBScriptWrapper`): ids from `/config/outfits`
  (`casual`, `gangster_suit_2_hat`, `old_1950s`, `vito`, ...). `SetCurrentOutfit(id)` only works for unlocked outfits
  and ends in the same "switch default player profile" call; every outfit id `X` has a matching player profile
  `characters/game/hero/wardrobe/game_hero_wardrobe_X` and a visual profile `visuals/hero/game_main_tommy_X`.
- **Hats/accessories**: there is no per-part API. `enums.ModelParts` exists (HEAD 0, TORSO 1, LEGS 2, SHOES 3, HAIR 4,
  GLASSES 5, HAT 6, MOUSTACHE 7, ACCESSORIES 8, BODY 9, LAST 10) but no binding in the classic tables and no game
  script uses it [likely orphaned]. What exists: `human:ShowHat(bool)`, `human:DropHat(...)`,
  `human:SetDirtBlend(0..1)`, and the hat/no-hat/dirty/bloody variants as separate spawn profiles. So "accessory"
  choices map to choosing a different profile.
- **Puppets**: the same names work for `C_RuntimeSpawnerComponent:SetSpawnProfile(name)` (what `player_sync.lua`
  already does with `visuals/...`). Most promising for puppets are the visual-only story profiles
  `characters/scotch/game/main/game_main_<name>` (counterparts of `visuals/hero/game_main_tommy`); the
  `characters/scotch/ai/ai_ally_<name>` profiles may carry an AI loadout (shotgun variants exist), which may also give
  the puppet a big-weapon slot (open question 2).

### Candidate APIs (prioritised)

| # | call | args (from the binding thunk) | returns | notes |
|---|---|---|---|---|
| 1 | `player:SwitchOverridePlayerSpawnProfile(p)` | `p` = profile path or decimal-hash string, or a `C_HashName` (`game.game:ToHashName(s)` / global `tohashname(s)`). Thunk 0x2395A00 reads it with the hash reader 0x3810D50 | `C_CntPtr<C_SyncObject>` (poll `IsDone()`, user data `IsSuccess()`) | Sets the override slot of the player model manager (`*(exe+0x6353128)`, +0x38) and swaps. Free-ride's debug `model` command and mission nodes use it. **Best candidate for "my co-op character"**: survives checkpoint scripts that re-set the *default* profile |
| 2 | `player:ClearOverridePlayerSpawnProfile()` | none (0x2395F40) | sync object | back to the default (story) profile, e.g. before cutscenes if a swap breaks them |
| 3 | `player:SwitchDefaultPlayerSpawnProfile(p)` | as 1 (0x2395340 -> 0x21E0960) | sync object | Sets the default slot (+0x30); swaps only if it changed **and no override is active**. Used by the checkpoint setup node (`Game.Game.SetMissionSpawnProfile`: `mm_spawn_profiles[quest][cp]`) and by the outfit DB. Likely saved with the game (`C_PlayerModelManager`, `PlayerModelManagerSaveBodyBlock`) [guess] |
| 4 | `player:SwitchPlayerSpawnProfile(p)` | as 1 (0x2394BB0 -> 0x221D190) | sync object | The raw swap, bypasses both slots: the next checkpoint/respawn reapplies default/override. This is what `player_sync.lua` `M.ModelTick` uses today |
| 5 | `player:IsPlayerUsingDefaultCostume()` | none (0x23965A0) | bool | asks the outfit DB |
| 6 | `game.outfits:SetCurrentOutfit(id)` | string #2 (0x22E44A0) | nothing | `C_OutfitDB::SetCurrentOutfit` 0x236BD40: returns false (no change) if `id` is not in the unlocked list; writes the story slot (+0x88) or the free-ride/replay slot (+0x90) depending on `C_Game->vf[0xC0]()->vf[0x28]()`; then switches the default player profile (0x2373300 -> 0x21E0960) and sends telemetry. **Writes save data** |
| 7 | `game.outfits:GetCurrentOutfit()` | none (0x22E4630) | `C_HashName` (compare with `game.game:ToHashName("casual")`) | |
| 8 | `game.outfits:IsLocked(id)` / `UnlockOutfit(id)` / `UnlockAll()` / `FreeRideStart()` | string / string / none / none | bool / - / - / - | `UnlockAll` permanently unlocks every outfit in the profile: **do not call on a player's real save**. `FreeRideStart` re-applies the stored outfit |
| 9 | `comp:SetSpawnProfile(name)` on `C_RuntimeSpawnerComponent` | C_String (verified in game-api.md 3) | - | puppets; any recovered name. Decimal-hash strings untested |
| 10 | `human:ShowHat(b)` | bool #2 (0x23B87A0) -> AI component (`[[h+0x10]+0x130]`) 0x2BA3170 | - | hide/show the hat of the current model; works through the AI component, so puppets should be fine; player untested |
| 11 | `human:DropHat(v)` | #2 read by 0x3810FF0 (same reader `SetRespawnPosDir` uses for its C_Vector args) | - | knocks the hat off (impulse/direction vector) [likely] |
| 12 | `human:SetDirtBlend(f)` | number #2 (0x23C45A0) -> entity vfunc +0x3C0 | - | dirt overlay 0..1 |
| 13 | `actor:OverrideMaterial(...)`, `actor:OverrideMaterialParam(...)` | not analysed | | tint/material swaps, a later polish option |
| 14 | in-world wardrobe menu: `enums.MenuId.E_MENUID_OUTFIT`, `/gui/in_world_menu_outfit.swf`, flash iface `OutfitIface` (`GetOutfitList`, `SelectOutfit`, `GetOutfitCategoryHashString`), node `Game.Game.CostumeChangeMenu` (`costumeChangeMenu`, `CostumeMenuAccept`, `CostumeMenuExit`) | | | the game's own outfit picker (free ride). Could be reused as a "Character" page later |

### Character list (recovered profile names, curated)

Story cast, visual profiles (counterparts of `visuals/hero/game_main_tommy`), prefix `characters/scotch/game/main/`:
`game_main_paulie`, `_paulie_suit`, `_paulie_dirty`, `_paulie_dirty_bloody`, `_paulie_drunk`, `_paulie_scarf`,
`game_main_sam`, `_sam_dirty`, `game_main_salieri`, `game_main_frank`, `_frank_suit`, `_frank_omerta`,
`game_main_vincenzo`, `game_main_luigi`, `game_main_ralph`, `game_main_sarah`, `game_main_morello`,
`game_main_sergio_suit`, `game_main_norman`, `game_main_bertone`, `game_main_billy`.

Story cast with AI archetype, prefix `characters/scotch/ai/`: `ai_ally_paulie` (+ `_gangster_suit_1_hat`,
`_gangster_suit_1_no_hat`, `_gangster_suit_2_hat`, `_gangster_suit_2_no_hat`, `_trenchcoat`, `_trenchcoat_dirty`,
`_bank_job`, `_drunk`), `ai_ally_sam` (+ `_gangster_suit_1/2/3_hat`, `_..._no_hat`, `_trenchcoat`, `_dirty`, `_beaten`,
`_shotgun`), `ai_ally_salieri` (+ `_shotgun`), `ai_ally_frank`, `ai_ally_vincenzo`, `ai_ally_luigi`, `ai_ally_ralph`,
`ai_ally_sarah` (+ `_late`), `ai_ally_salvatore`, `ai_ally_bertone`, `ai_ally_biff`, `ai_enemy_sam`,
`ai_enemy_morello_{triggerman, shotgunner, machinegunner, rifleman, knifeman, enforcer}`.

Tommy: `visuals/hero/game_main_tommy_<outfit>` for every outfit id below plus `bank_job_without_a_scarf`,
`bloddy_forearm_gngstr1`, `cab_driver_messy`, `gangster_suit_1_with_sarah-s_coat`, `gangster_trenchcoat_bloody`,
`gangster_trenchcoat_dirty`, `scarf`, `1938`; `characters/scotch/game/hero/game_main_tommy` (+ `_1938`, `_bloody`,
`_coat`, `_dirty`, `_gangster_suit_1`, `_racing_outfit`, `_scarf`).

Outfit ids (`/config/outfits` in `tables\tables.sds`, all have both `game_hero_wardrobe_X` and
`visuals/hero/game_main_tommy_X` except `alien`/`dogman` which have only the wardrobe one): `cab_driver_flatcap`
(default, unlocked), `cab_driver_no_flatcap`, `criminal_outfit_hat`, `criminal_outfit_no_hat`,
`gangster_suit_1_hat`, `gangster_suit_1_no_hat`, `gangster_suit_2_hat`, `gangster_suit_2_no_hat`,
`gangster_suit_2_vest`, `gangster_trenchcoat`, `trenchcoat_bloody_nohat`, `funeral`, `bank_job`, `casual`,
`racing_outfit`, `sailor`, `jail`, `reward_outfit_1..6, 9, 10`, `old_wedding`, `old_1950s`, `capone` (DLC
"chicago"), `blackcats`, `vito`, `lincoln`, `alien`, `dogman`.

Civilians (random appearance per spawn): `characters/traffic/traffic_{base_ambient_male, base_ambient_female,
base_ambient_combined, business, business_white, low_class, low_class_men, low_class_men_white, low_class_white,
low_class_black, black, irish, workers, workers_male, workers_female, party, drunk, junkie}`, and
`characters/game/game_{attractivemen, attractivegirls, blackmen, blackwomen}`. Police/gangsters: see appendix.
Already used by `player_sync.lua`: `visuals/temp/young_salieri`, `visuals/temp/young_morello`,
`visuals/temp/trailer_thug`.

None of the recovered names is in ScriptHook's "Crashy" list (106 hashes it hides).

### Evidence / how found

1. **Hash = FNV-1 64.** `0x3CCC820` (string -> hash, used by every hash argument) calls `0x3CBAE50`; the 20 Tommy
   profile names found in plain text in `data_missions.lua` (`mm_spawn_profiles`) hash with FNV-1 64 to entries of
   `PlayerModelIDs.lua`. All 2040 ids occur as 8-byte values in the unpacked `spawnprofiles.sds`. Story names were
   found by scanning the mission SDS (`sds_retail\script\mm_*.sds`, unpacked) for those 8-byte hashes and reading the
   spawner entity name next to each (`cp_160_paulie_spawner`, `spawner_sam_gangster_suit_3_hat_00`, ...), then
   brute-forcing `prefix + name + suffix` until the hashes matched.
2. **Hash argument reader** `0x3810D50 -> 0x3810200` (used by all four player switches, `QuestGetCustomState`, ...):
   Lua type 4 (string) -> `0x3CBAE50`, which returns the **number itself if the string is all decimal digits starting
   with 1-9** (`lea eax,[rcx-0x31]; cmp al,8` then `rbx = rbx*10 + c - '0'` with overflow check), otherwise the FNV
   hash; type 7 (userdata) must be a `C_LuaHashName`; anything else raises `bad argument #2 (C_LuaHashName expected,
   got number)`. This is why ScriptHook's `SwitchDefaultPlayerSpawnProfile("3480140893589570539")` works and why a
   Lua number (32-bit float) cannot be used.
3. **Player model manager** (`*(exe+0x6353128)`; `[TM]C_PlayerModelManager::RegGameSaveCb`): +0x28 player present,
   +0x30 default profile, +0x38 override profile, +0x50 a ready "done" sync object returned when there is no player.
   `0x21E0960` (default): stores +0x30 and calls the swap `0x221D190` only when the value changed and +0x38 == 0.
   `0x21E09B0` (override) / `0x21E0FF0` (clear) follow the same shape.
4. **Game scripts** (`tables\scripts.sds`, HKS chunks): `game.lua` checkpoint node calls
   `getp():SwitchDefaultPlayerSpawnProfile(game.game:ToHashName(mm_spawn_profiles[quest][cp]))` then
   `ClearOverridePlayerSpawnProfile()`, and in replay/test drive uses `game.outfits:GetCurrentOutfit()`;
   `player.lua` has nodes for all four switches (each `Wait(so)` then `so:GetUserData():IsSuccess()`); `freeride.lua`
   has a debug `model` command using `SwitchOverridePlayerSpawnProfile` + `ClearOverridePlayerSpawnProfile`.
5. **Outfit DB**: `C_OutfitDBScriptWrapper` table 0x5AD1220 registered as `game.outfits` (0x236F340, name "outfits",
   parent "game"). `C_OutfitDB` global `*(exe+0x6361DA8)`: unlocked list +0x28..+0x30, entry map +0x58, current
   story outfit +0x88, current free-ride/replay outfit +0x90. Profile path format string
   `characters/game/hero/wardrobe/game_hero_wardrobe_%s` (exe string), and `/config/outfits` in `tables.sds`.
6. **ModelParts**: enum `C_WrapE_ModelParts`, entries at 0x5AD6D50 (name, getter) with float values read from the
   getters. No xref outside its own enum class; no use in the 122 game scripts.

### Live-test script (dev console: F8 `out\dev.lua` or `tools\devlua.ps1 -Tag host -File <file>`)

Back up the save folder first; never call `game.outfits:UnlockAll()` or `SetCurrentOutfit` on a real save without a
backup. Run each block separately; each returns a string.

```lua
-- F1-1: hash reader accepts strings and decimal strings; reject numbers. Expect "ok ok err".
local p = game.game:GetActivePlayer()
local a = pcall(p.SwitchOverridePlayerSpawnProfile, p, "characters/scotch/game/main/game_main_paulie")
local b = pcall(p.SwitchOverridePlayerSpawnProfile, p, "16922695808871587265")   -- same profile as its hash
local c, e = pcall(p.SwitchOverridePlayerSpawnProfile, p, 1)
return tostring(a) .. " " .. tostring(b) .. " " .. tostring(c) .. " " .. tostring(e)
```

```lua
-- F1-2: swap the local player to Paulie with the override slot, poll the sync object, then check controls/camera.
-- Re-run with the visual Tommy profile to compare. Look at: model visible, walk/run/sprint anims, enter a car, shoot.
local p = game.game:GetActivePlayer()
MafiaCoop.f1so = p:SwitchOverridePlayerSpawnProfile("characters/scotch/game/main/game_main_paulie")
return tostring(MafiaCoop.f1so)
```
```lua
-- (next tick) result of F1-2; also check the player wrapper is still valid after the swap
local so = MafiaCoop.f1so
local p = game.game:GetActivePlayer()
return "done=" .. tostring(so:IsDone()) .. " ok=" .. tostring(so:IsDone() and so:GetUserData():IsSuccess())
  .. " player=" .. tostring(p) .. " valid=" .. tostring(IsObjectValid(p)) .. " default=" .. tostring(p:IsPlayerUsingDefaultCostume())
```

```lua
-- F1-3: override survives a checkpoint-style default switch (what the story does); expect the model to stay Paulie
local p = game.game:GetActivePlayer()
p:SwitchDefaultPlayerSpawnProfile("visuals/hero/game_main_tommy_casual")
return "sent; look: still Paulie? then run ClearOverridePlayerSpawnProfile() and expect casual Tommy"
```

```lua
-- F1-4: weapons per character (A6 gate). Run after each swap; read the log line.
local p = game.game:GetActivePlayer()
local out = {}
for _, w in ipairs({"p_mast_a_v1", "rev_alf_a_v1", "sg_barker_a_v1", "smg_trench_a_v1", "sn_mayw_a_v1"}) do
  pcall(p.InventoryAddWeapon, p, w, 50)
  out[#out + 1] = w .. "=" .. tostring(p:InventoryHaveWeapon(w))
end
p:InventorySelect("smg_trench_a_v1", true, true)
return table.concat(out, " ") .. " (check in ~1 s: InventoryGetSelected = " .. tostring(p:InventoryGetSelected()) .. ")"
```

```lua
-- F1-5: puppet profiles. Uses the running factory of player_sync (M.fac.comp); spawns one human per profile
-- 3 m in front of the camera, one per call. Check: model, idle anim, MoveVec, pistol and Tommy gun acceptance.
local names = { "characters/scotch/game/main/game_main_sam", "characters/scotch/ai/ai_ally_sam_shotgun",
  "characters/traffic/traffic_business", "16922695808871587265" }
MafiaCoop.f1i = (MafiaCoop.f1i or 0) % #names + 1
local comp = MafiaCoop.fac and MafiaCoop.fac.comp   -- player_sync.lua: M == MafiaCoop
if not comp then return "no factory component (is a session running and the factory ready?)" end
comp:SetSpawnProfile(names[MafiaCoop.f1i])
MafiaCoop.f1load = comp:GetSpawnProfileLoadSyncObject()
return "loading " .. names[MafiaCoop.f1i]
```
```lua
-- (when MafiaCoop.f1load:IsDone()) create it
local comp = MafiaCoop.fac.comp
local cam = game.cameramanager:GetPlayerCamera()
local pos, d = cam:GetPos(), cam:GetDir()
local npc = comp:CreateObject()
npc:SetPos(Math:newVector(pos.x + d.x * 3, pos.y + d.y * 3, pos.z)); npc:Activate()
local big = pcall(npc.InventoryAddWeapon, npc, "smg_trench_a_v1", 50) and npc:InventoryHaveWeapon("smg_trench_a_v1")
MafiaCoop.f1npc = npc
return tostring(npc) .. " tommygun=" .. tostring(big) .. " ai=" .. tostring(npc:GetAIType())
```

```lua
-- F1-6: hats and dirt on the player and on the last puppet
local p = game.game:GetActivePlayer()
pcall(p.ShowHat, p, false); pcall(p.SetDirtBlend, p, 1.0)
if MafiaCoop.f1npc then pcall(MafiaCoop.f1npc.ShowHat, MafiaCoop.f1npc, false) end
local ok, e = pcall(p.DropHat, p, Math:newVector(0, 0, 1))
return "hat hidden, dirt 1; DropHat: " .. tostring(ok) .. " " .. tostring(e)
```

```lua
-- F1-7: outfit DB read-only probes (safe)
local o = game.outfits
local cur = o:GetCurrentOutfit()
local function is(id) return tostring(cur == game.game:ToHashName(id)) end
return "outfits=" .. tostring(o) .. " cur=" .. tostring(cur) .. " casual?" .. is("casual") .. " cab?" .. is("cab_driver_flatcap")
  .. " lockedVito=" .. tostring(o:IsLocked("vito")) .. " lockedCasual=" .. tostring(o:IsLocked("casual"))
```

```lua
-- F1-8: ModelParts and component probing (find any reflected method that takes a model part)
local s = {}
for k, v in pairs(enums.ModelParts or {}) do s[#s + 1] = tostring(k) .. "=" .. tostring(v) end
local p = game.game:GetActivePlayer()
for _, c in ipairs({ "C_ModelComponent", "C_ClothComponent", "C_SpawnedObjectComponent" }) do
  local ok, comp = pcall(p.GetComponent, p, c)
  local mt = ok and comp and getmetatable(comp)
  local m = {}
  if mt and type(mt[0]) == "table" then for k in pairs(mt[0]) do m[#m + 1] = k end end
  s[#s + 1] = c .. ":" .. tostring(comp) .. "{" .. table.concat(m, ",") .. "}"
end
return table.concat(s, " ")
```

### Risks / open questions

1. Does a player model swap keep the `GetActivePlayer()` wrapper valid, the inventory, the car seat and the camera?
   (F1-2.) If the swap re-creates the entity, `player_sync.lua` must re-fetch the player after the sync object is done.
2. Which profiles accept the Tommy gun on **puppets**: the civilian factory gives no big-weapon slot (game-api.md 6);
   `ai_ally_*` profiles might bring their own loadout/slots (F1-5). Fallback: hide the gun on the puppet and only show
   pistols.
3. Default vs override persistence: `C_PlayerModelManager` registers a save callback; whether the override slot is
   saved is unknown (if it is, a guest's character would stick in their own save, fine; the host must not get it).
   Test: override, checkpoint save, reload, `IsPlayerUsingDefaultCostume()`.
4. Cutscenes put the player entity into the cutscene (`C_CutsceneGameObjectComponent:SetCutsceneObject`): a
   non-Tommy model may look wrong or break facial animation. PLAN F's fallback (clear the override for cutscenes) is
   supported by `ClearOverridePlayerSpawnProfile`.
5. Mission scripts that call `SwitchOverridePlayerSpawnProfile` themselves (racing outfit, disguises) will overwrite
   our override; re-apply after `ClearOverride...` or after a checkpoint (watch `game.game:GetLoadedSaveId()` and the
   quest custom state, B2).
6. `SetCurrentOutfit`, `UnlockOutfit`, `UnlockAll` change the profile/save; use the profile switches instead for co-op.
7. Some recovered profiles (e.g. `visuals/temp/*`, `characters/scotch/ai/mission/*`) are mission-specific and may need
   their SDS (textures) loaded; `GetSpawnProfileLoadSyncObject()` handles that for puppets; for the player switch it
   is part of the returned sync object [guess].

---------------------------------------------------------------------------------------------------------------

## B2 (+B5). Missions, chapters, checkpoints, objectives, cutscenes

### Summary

- **Chapters are "quests"** with ids `mm_10_chase` ... `mm_210_gallery` (global `mm_ids` in `data_missions.lua`;
  `mm_ids_replay` for the replay list), and **checkpoints are the quest's "custom state"** (`cp_010`, `cp_020`, ...),
  read with `game.director:QuestGetCustomState(questId)` and keyed the same way as `mm_spawn_profiles[quest][cp]`.
  The game's own helper `GetActiveQuest()` (global, `various_utils.lua`) returns the first quest of `mm_ids` for which
  `game.director:QuestIsAvailable(id)` is true, and caches it in the global `active_quest`.
  `game.game:GetLoadedSaveId()` (already verified, e.g. `checkpoint_mm_110_omerta_cp_020`) names the save that was
  loaded. Together these are the host's chapter/checkpoint state [likely; verify with B2-1].
- **E_MissionAction is telemetry only**: `telemetry_enums.mission_actions` in `game.lua` (`E_MA_START`, `E_MA_FAILED`,
  `E_MA_SUCCESS`) and `E_MissionFailedReason` (`E_MFR_PLAYERDIED`, `NPCDIED`, `DOCUMENTARY_ENDING`, `TIMEOUT`,
  `ABANDONED`). The node functions that receive them (`missionEvent(nodeID, inMissionName, inMissionAction)`,
  `missionFailedReason(nodeID, inReason)`) are now empty stubs, but the mission flow graphs still call them, so
  **wrapping these Lua functions on the host is a free mission start/fail/success hook** [likely].
- **Loading a chapter is C++ (main menu), not Lua.** The main menu's AS3 calls the `C_MainMenu` flash iface
  (`_root.game.MainMenu`): `MissionReplay(index)` (chapter select) picks entry `index` of the mission replay list and
  calls **`OpenDebugLoadChapterString(name, false, true)`** (0x3020350), which loads `/saves/<name>` (the shipped
  replay saves `edit\saves\00_mission_replay\checkpoint_mm_<nnn>_<name>_cp_010.sav`, one per chapter) and starts the
  game from it. `LoadSaveId(id)` loads one of the player's own saves; `ContinueGame()` continues. All of these are
  registered in the iface table built at 0x30323D0 / 0x3034E10. A guest at the main menu can be sent into the host's
  chapter by having our patched main-menu AS3 (menu.cpp already patches `main_menu_main.gfx`) call
  `_root.game.MainMenu.MissionReplay(i)` or `OpenDebugLoadChapterString("...")`, or from C++ by calling 0x3020350 on
  the captured iface `this` [likely; B2-5].
- **Jumping to a later checkpoint** has no direct API. Options: (a) the host's save id names the checkpoint, and the
  checkpoint kits read the quest custom state, so `QuestSetCustomState(quest, "cp_050")` before the mission flow
  starts may select it [guess]; (b) send the host's checkpoint save file to the guest and load it with `LoadSaveId`
  (user saves are `Documents\My Games\Mafia Definitive Edition\Data\<steamid>\gamesaves\default\{a,b,c}slot\saveN.sav`,
  partly compressed, save id in plain text near the end) [guess]; (c) start every guest at the chapter's `cp_010` and
  teleport them to the host (simplest, works with "guests mirror the host's world").
- **Objectives**: mission flows call Lua nodes `Game.Hud.Objective.Missions.*` (`CreateObjective`, `FinishObjective`,
  `CreateOverrideObjective`, ...) which call globals `HUD_AddQuestObjective`, `HUD_AddQuestOverrideObjective`,
  `HUD_RemoveQuestObjective`, `HUD_SetQuestObjectiveState`, ... (`various_utils.lua`), which end in
  `game.hud:UpdateSimpleObjective(...)` and keep the state in globals `active_objective`, `override_objective`,
  `active_obj_description` and table `game_missions[questId]` (`title`, `objectives`, `objectives_order`, `subs`,
  `finished`, ...). Host: read those globals (or wrap the `HUD_*` globals); guest: call the same `HUD_*` globals or
  `game.hud:UpdateSimpleObjective` [likely].
- **Fail path**: `game.director:GameOver(reasonId)` (0x2245970) takes a **numeric string** (`strtoul(s, 0, 10)`, e.g.
  `"0059310003"` in `timer.lua`, a string-table id) and shows the mission-failed screen; flows call it through the Lua
  node `gameOver(nodeID, game_over_reason)` (`game_director/other.lua`). "You abandoned the objective" is an English
  string-table entry (fail ids per mission: `mm_20_taxi_go_taxi_abandoned`, `mm_30_molotov_go_paulie_abandoned`, ...).
  Retry: `game.savesystem:IsRetryPossible()` / `Retry()` / `FreeRoamRetry()`; HUD: `ShowMissionRestart`,
  `AcceptMissionRestart`, `QuitToMainMenu`.
- **Cutscenes (B5)**: `game.game:PlayCutscene(name [, bool])` is a **stub** in this build (reads its arguments and
  returns). Real cutscenes are scene objects named `cine_<nnnn>_<name>_cs_object` (e.g. `cine_1100_omerta_cs_object`)
  with a `C_RuntimeCutsceneComponent`: `LoadCutscene`, `IsCutsceneLoadStarted`, `IsCutsceneLoadFinished`,
  `PlayCutscene`, `PlayCutscene2`, `GetVisualStartSyncObject`, `IsCutscenePlaybackInProgress`, `StopCutscene`,
  `UnloadCutscene`, `AllowPlayerCamera`. While one plays, the cutscene node sets datastore `cutscene_active` in group
  `DefaultGroup`. Skippable scripted scenes: `game.skipscriptscene:BeginSkipableScene()` / `EndSkipableScene()`
  (`C_SkipScrCutsceneModule`); the skip input is `E_CTRL_SKIP_CUTSCENE` (C++).

### Candidate APIs (prioritised)

Detecting the host's state:

| # | call | args | returns | notes |
|---|---|---|---|---|
| 1 | `GetActiveQuest()` (global Lua) | none | quest id string (`"mm_110_omerta"`) or nil | iterates `mm_ids` with `game.director:QuestIsAvailable` |
| 2 | `game.director:QuestGetCustomState(q)` | `q` string or `C_HashName` (hash reader) (0x2246D20) | string (`"cp_020"`) | the checkpoint; `QuestGetCustomStatesCount(q)`, `QuestGetCustomStateByIndex(q, i)` list the states |
| 3 | `game.game:GetLoadedSaveId()` | none | `"checkpoint_mm_110_omerta_cp_020"`, `"!none!"` in the menu | verified (game-api.md 2); changes only on load |
| 4 | `game.game:GetLoadedSaveType()` | none (0x26820E0) | number = `*(int*)(C_Game->vf[0xC0]() + 0x20)` | values unknown (story / free ride / replay?) |
| 5 | `game.savesystem:IsInReplayOrTestDrive()` | none | bool | |
| 6 | globals `active_quest`, `active_objective`, `override_objective`, `active_obj_description`, `game_missions`, `game_chapters`, `mission_to_chapter_map` | | | objective state as the game's own Lua keeps it |
| 7 | wrap `missionEvent` / `missionFailedReason` (`game.lua`, telemetry nodes; table path to confirm with B2-2) | `(nodeID, inMissionName, inMissionAction)` / `(nodeID, inReason)` | | mission start/fail/success events |
| 8 | wrap `gameOver` node (`game_director/other.lua`) or `HUD_*` objective globals | | | fail screen / objective changes as events |
| 9 | `game.director:GetCurrentAct()` / `GetCurrentSubAct()` | none | number | game-director act (Mafia III heritage; may be constant) |
| 10 | datastore `cutscene_active` (`DatastoreGet("cutscene_active", "DefaultGroup")`, `DatastoreExists`) | | | a cutscene is playing |
| 11 | `hud:GetSelectedObjective()` / `GetLastSelectedObjective()` | | | |

Making a guest follow:

| # | call | args | notes |
|---|---|---|---|
| 1 | `_root.game.MainMenu.MissionReplay(i)` (AS3, C++ handler 0x3016800) | `i` = index into the mission replay list (`GetMissionReplayList()`) | the chapter-select path; resets the replay outfit (0x236D360) first, then `OpenDebugLoadChapterString(entry, false, true)` |
| 2 | `_root.game.MainMenu.OpenDebugLoadChapterString(name)` (handler 0x3020350; C++ signature `(this, C_String* name, bool, bool)`, C_String from `char*` via 0x689AA0) | save name relative to `/saves/` (e.g. `00_mission_replay/checkpoint_mm_110_omerta_cp_010` [guess on the folder part]) | loads a save file by name and starts the game. Also `OpenDebugLoadChapter`, `OpenDebugLoadChapter2`, `GetMissionList`, `GetPartList` (dev chapter loader, handlers 0x30201D0 / 0x301FCC0 / 0x2FE0110 / 0x2FE5AE0) |
| 3 | `_root.game.MainMenu.LoadSaveId(id)` (handler 0x3016300), `ContinueGame()`, `CanContinueGame()`, `GetSaveList()` | | player's own saves |
| 4 | `game.director:QuestSetCustomState(q, cp)` (0x2246E60) | quest (hash reader), string | may select the checkpoint the next mission start uses [guess] |
| 5 | `game.savesystem:RequestCheckpointSave([name])` (0x2E56EF0; 0 or 1 string arg), `RequestMissionSave()`, `RequestFreeroamSave()`, `Retry()`, `IsRetryPossible()`, `FreeRoamRetry()`, `SetRespawnPosDir(pos, dir)`, `SetRespawnDefaultSpawnProfile(p)`, `SetRespawnOverrideSpawnProfile(p)`, `EnableSaveLogic(b)` | | `Retry()` = load the last checkpoint (what the fail screen does). `EnableSaveLogic(false)` on guests may stop them writing saves of a mirrored world [guess] |
| 6 | `game.hud:UpdateSimpleObjective(s2, s3, b4, b5, s6)` (0x2F44180, needs all 5 args) | two strings, two bools, a string (names unknown; `HUD_AddQuestObjective(inQuestID, inObjectiveID, inObjectiveText)` calls it with `active_obj_description`) | mirror the objective text |
| 7 | globals `HUD_AddQuestObjective`, `HUD_AddQuestOverrideObjective`, `HUD_RemoveQuestOverrideObjective`, `HUD_RemoveQuestObjective`, `HUD_SetQuestObjectiveState`, `HUD_AddQuestSubObjective`, `HUD_AddChapter`, `HUD_AddQuest` | quest/objective ids + text ids | safer than calling `UpdateSimpleObjective` directly: they keep the game's globals consistent |
| 8 | `game.director:GameOver(id)` | numeric string id | force a guest's fail screen when the host fails |
| 9 | `game.hud:ShowTitleCard(text, desc)` / `HideTitleCard()` | `$title_card_mm_110_title` style ids (`data_missions.lua` `title_cards`) | chapter title cards |
| 10 | cutscene: `obj = game.entitywrapper:GetSceneObjectByName("cine_1100_omerta_cs_object")`; `c = obj:GetComponent("C_RuntimeCutsceneComponent")`; `c:LoadCutscene()`; poll `c:IsCutsceneLoadFinished()`; `so = c:PlayCutscene2(...)`; `c:StopCutscene()`; `c:UnloadCutscene()` | PlayCutscene2 args unknown (the node passes title/desc/texture/fade/unload flags) | B5: play the host's cutscene on the guest, or show a "cutscene playing" panel while `cutscene_active` is set on the host |
| 11 | `game.skipscriptscene:BeginSkipableScene()` / `EndSkipableScene()` | none | mark a skippable scripted scene |
| 12 | `game.streammap:ChangeScenario(s)`, `game.sds:GetSyncObjectForLoadSDS(name)` / `GetSyncObjectForUnloadSDS` / `IsSDSLoaded` | | mission streaming pieces (`mm_110_omerta_p`, `..._s_park`), not a chapter loader |

### Evidence / how found

1. `tables\scripts.sds` unpacked (Oodle) contains the game's 122 Lua modules as HKS bytecode; their constant tables
   list the methods each node calls. Relevant modules: `various_utils.lua` (`GetActiveQuest`, `HUD_*` globals,
   objective globals), `data_missions.lua` (`mm_ids`, `mm_ids_replay`, `mm_settings[...].start_pos/start_dir`,
   `mm_spawn_profiles`, `mm_loadouts`, `mm_world_state`, `mm_rewards.outfits`, `title_cards`, per-checkpoint objective
   descriptions), `game.lua` (checkpoint setup: `GetActiveQuest`, `game.director:QuestGetCustomState(...)`,
   `QuestGetCustomStatesCount`, `QuestGetCustomStateByIndex`, `game.savesystem:SetRespawnPosDir`,
   `SetRespawnDefaultSpawnProfile`, `SetRespawnTrafficSeason`; telemetry enums; `skipscriptscene`), `savesystem.lua`,
   `objective_missions.lua`, `cutscene.lua`, `game_director/other.lua` (`gameOver`), `timer.lua`
   (`GameOver("0059310003")`), `streammap.lua`, `game_structure_console.lua` (debug commands `skipobjective`,
   `skipquest`, `savemission`, `getqueststate`, `setqueststate`: they post events to "Temp Mission Flow" kits).
2. `game.*` adaptor names come from the registration calls `(g_ScriptGameMachine, obj, "<name>", "game")`: `director`,
   `savesystem`, `outfits`, `skipscriptscene`, `streammap`, `sds`, `datastore`, `perchars`, `capture`, `racemanager`,
   ... (xrefs of the string "game" at 0x4CE3C44).
3. `C_MainMenu` iface tables (0x30323D0, 0x3034E10): each entry `{vtable, owner, name, member fn, flags}`; handler
   addresses read from the `lea` preceding each name. `MissionReplay` (0x3016800) disassembly: bounds check against
   the replay list (`*(exe+0x6359CC0)`), `0x236D360` on the outfit DB, `0x25CDB30(*(exe+0x637F670))` (same tail as
   `SetCurrentOutfit`), entry string -> C_String (0x689AA0) -> `0x3020350(this, &name, 0, 1)`. `0x3020350` builds
   `"/saves/" + name`, reads the file through the VFS (`[this+0x2C8]` vf+0x40/+0x88) and starts a game session.
4. Shipped saves: `edit\saves\00_mission_replay\checkpoint_mm_{010_chase, 020_taxi, 030_molotov_cp_010_cinematic_molotov,
   040_motel, 050_race, 060_sarah, 070_hoodlums, 080_brothel, 100_farm, 110_omerta, 120_mansion, 130_parking,
   140_salieri, 150_boat, 160_harbor, 170_plane, 180_sniper, 190_cigars, 200_bank, 210_gallery}_cp_010.sav` and
   `mm_090_funeral_init.sav` (format `GENR`, readable: they contain the quest ids, `<mission>_finished_objectives`,
   the player spawn profile, e.g. `visuals/hero/game_main_tommy_gangster_suit_1_hat`).
5. `PlayCutscene` (0x2671D50) dispatches on argc to 0x2688D30 / 0x2688C70; both only read/check their arguments and
   return 0. `GameOver` (0x2245970): `lua_tostring(2)` -> `strtoul(s, NULL, 10)` (import at 0x4A6FD98) -> director
   vfunc +8.
6. `UpdateSimpleObjective` (0x2F44180): self adaptor with min 5, then `isstring(2)`, `isstring(3)`, `isboolean(4)`,
   `isboolean(5)`, `isstring(6)` -> 0x306CD60(hud, s2, s3, b4, b5, s6).
7. Cutscene component method names are adjacent strings at 0x4BAB548..0x4BAB620 (reflected class
   `C_RuntimeCutsceneComponent`); the entity names `cine_1100_omerta_cs_object`, ... are in `mm_110_omerta_p.sds`.

### Live-test script

```lua
-- B2-1: host state. Run in a mission save (e.g. Omerta park) and again after reaching the next checkpoint.
local q = GetActiveQuest and GetActiveQuest()
local s = { "quest=" .. tostring(q), "save=" .. tostring(game.game:GetLoadedSaveId()),
  "type=" .. tostring(game.game:GetLoadedSaveType()),
  "replay=" .. tostring(game.savesystem:IsInReplayOrTestDrive()),
  "act=" .. tostring(game.director:GetCurrentAct()) .. "/" .. tostring(game.director:GetCurrentSubAct()) }
if q then
  s[#s + 1] = "cp=" .. tostring(game.director:QuestGetCustomState(q))
  local n = game.director:QuestGetCustomStatesCount(q)
  local all = {}
  for i = 0, (tonumber(n) or 0) - 1 do all[#all + 1] = tostring(game.director:QuestGetCustomStateByIndex(q, i)) end
  s[#s + 1] = "states(" .. tostring(n) .. ")=" .. table.concat(all, ",")
end
s[#s + 1] = "obj=" .. tostring(active_objective) .. " override=" .. tostring(override_objective)
  .. " desc=" .. tostring(active_obj_description)
return table.concat(s, " | ")
```

```lua
-- B2-2: where the node tables are, and hook the telemetry/objective/fail functions (host). Logs via mc_log.
local found = {}
local function scan(t, path, depth)
  if depth > 4 or type(t) ~= "table" then return end
  for k, v in pairs(t) do
    if type(v) == "function" and (k == "missionEvent" or k == "missionFailedReason" or k == "gameOver"
        or k == "objectiveMissionsCreateObjective" or k == "objectiveMissionsFinishObjective") then
      found[#found + 1] = path .. "." .. k
      if not MafiaCoop["hooked_" .. path .. k] then
        MafiaCoop["hooked_" .. path .. k] = true
        local orig = v
        t[k] = function(...)
          local a = { ... }
          local parts = {}
          for i = 1, select("#", ...) do parts[#parts + 1] = tostring(a[i]) end
          if mc_log then mc_log("B2 hook " .. path .. "." .. k .. "(" .. table.concat(parts, ", ") .. ")") end
          return orig(...)
        end
      end
    elseif type(v) == "table" and type(k) == "string" then scan(v, path .. "." .. k, depth + 1) end
  end
end
scan(Game, "Game", 0)
for _, g in ipairs({ "HUD_AddQuestObjective", "HUD_AddQuestOverrideObjective", "HUD_RemoveQuestObjective",
    "HUD_SetQuestObjectiveState", "HUD_AddChapter" }) do
  if type(_G[g]) == "function" and not MafiaCoop["hooked_" .. g] then
    MafiaCoop["hooked_" .. g] = true
    local orig = _G[g]
    _G[g] = function(...)
      local parts = {}
      for i = 1, select("#", ...) do parts[#parts + 1] = tostring((select(i, ...))) end
      if mc_log then mc_log("B2 hook " .. g .. "(" .. table.concat(parts, ", ") .. ")") end
      return orig(...)
    end
    found[#found + 1] = g
  end
end
return "hooked: " .. table.concat(found, " ")
```
Then play until an objective changes, die once (fail screen), restart, and read `logs\mafiacoop.log`. Note: a flow node
may hold a reference to the original function (captured at load); if nothing is logged, the hook must wrap earlier (at
`onGameRun`) or the fields are read through a different table.

```lua
-- B2-3: objective mirroring on a second instance (guest); text ids from data_missions.lua
local q = GetActiveQuest and GetActiveQuest() or "mm_110_omerta"
local ok, e = pcall(HUD_AddQuestObjective, q, "coop_test", "mm_110_omerta_obj_desc_02")
local ok2, e2 = pcall(game.hud.UpdateSimpleObjective, game.hud, "mm_110_omerta_obj_desc_02", "", true, true, "")
return "HUD_AddQuestObjective " .. tostring(ok) .. " " .. tostring(e) .. " | UpdateSimpleObjective " .. tostring(ok2) .. " " .. tostring(e2)
```

```lua
-- B2-4: fail screen ids. Prints the global objective state first; GameOver shows the fail UI (needs Retry/Quit after).
local id = MafiaCoop.goid or "0059310003"
local ok, e = pcall(game.director.GameOver, game.director, id)
return "GameOver(" .. id .. ") " .. tostring(ok) .. " " .. tostring(e)
  .. " retryPossible=" .. tostring(game.savesystem:IsRetryPossible())
```

```lua
-- B2-5: chapter loading from the main menu (run while the MAIN MENU is shown; uses the hud bridge to the movie).
-- First list what the MainMenu iface reports, then try MissionReplay(index) through the movie's own AS3 object.
-- SendMessageMovie only calls _root functions, so this needs a tiny AS3 helper patched into main_menu_main.gfx by
-- menu.cpp (e.g. our BuildMenu prologue answering "replay|<i>" or "load|<name>" by calling
-- _root.game.MainMenu.MissionReplay(i) / OpenDebugLoadChapterString(name)). Test sequence for the later agent:
--   1. patch: on our CO-OP entry id 199, call _root.game.MainMenu.GetMissionReplayList() and send the result back
--      through AttachIface("MafiaCoop", "dump", result) -> log it (gives the entry names and their order);
--   2. on id 198, call _root.game.MainMenu.MissionReplay(10) -> expect Omerta (index of mm_110 in mm_ids_replay);
--   3. on id 197, call _root.game.MainMenu.OpenDebugLoadChapterString("00_mission_replay/checkpoint_mm_110_omerta_cp_010")
--      and, if that fails, the same without the folder.
-- From Lua, the replay order is visible already:
local t = {}
for i, id in ipairs(mm_ids_replay or {}) do t[#t + 1] = (i - 1) .. "=" .. tostring(id) end
return "mm_ids_replay: " .. table.concat(t, " ")
```

```lua
-- B2-6: checkpoint selection by custom state (in a chapter's cp_010 save; back up saves first)
local q = GetActiveQuest()
local before = game.director:QuestGetCustomState(q)
game.director:QuestSetCustomState(q, "cp_050")
return "before=" .. tostring(before) .. " after=" .. tostring(game.director:QuestGetCustomState(q))
  .. " -- now try game.savesystem:Retry() and see which checkpoint loads"
```

```lua
-- B5-1: cutscene state on the host and cutscene objects in the loaded mission
local s = { "active=" .. tostring(DatastoreExists and DatastoreExists("cutscene_active", "DefaultGroup")) }
for _, n in ipairs({ "cine_1100_omerta_cs_object", "cine_1130_omerta_cops_cs_object", "cine_1195_omerta_funeral_bik_cs_object" }) do
  local ok, o = pcall(game.entitywrapper.GetSceneObjectByName, game.entitywrapper, n)
  local c = ok and o and o.GetComponent and o:GetComponent("C_RuntimeCutsceneComponent")
  local playing = c and select(2, pcall(c.IsCutscenePlaybackInProgress, c))
  s[#s + 1] = n .. "=" .. tostring(o) .. " comp=" .. tostring(c) .. " playing=" .. tostring(playing)
end
s[#s + 1] = "skip=" .. tostring(game.skipscriptscene)
return table.concat(s, " | ")
```

### Risks / open questions

1. `OpenDebugLoadChapterString` is a debug-named entry: it is registered in the retail iface table and used by
   `MissionReplay`, but whether the AS3 side may call it directly (and the exact relative name it expects) is
   untested. `MissionReplay(i)` is the safe path; it also resets the replay outfit and needs the chapter to be in the
   replay list (unlocked) [guess: the list may only contain chapters the guest's profile has unlocked].
2. Mission replay mode differs from the story (`IsInReplayOrTestDrive()` true; separate outfit slot; saves may be
   disabled). For co-op that is probably good (guest progress does not overwrite the guest's story save) but the host
   plays the real story; mission scripts may branch on replay mode.
3. Later checkpoints: no direct loader; B2-6 or transferring save files are guesses. The fallback (guest starts the
   chapter at `cp_010`, then mirrors the host's world and gets teleported) is always available.
4. Hooks on node functions only work if the flow graph looks the function up at call time; HKS nodes are usually
   looked up by name per call, but this must be confirmed (B2-2).
5. `UpdateSimpleObjective` argument meaning is inferred from the call sites; use the `HUD_*` globals where possible.
6. `GameOver` ids are numeric string-table ids; the id for "You abandoned the objective" was not resolved statically
   (string-table index format not decoded). B2-2 logs the real ids when a mission fails.
7. `game.game:PlayCutscene` is a dead end (stub). Cutscene objects only exist while their mission SDS is loaded on
   that machine, so a guest can only replay a host cutscene if it runs the same mission part.

---------------------------------------------------------------------------------------------------------------

## Method (to redo after a game update)

1. **SDS v20 unpack**: header `SDS\0`, version 20, `PC`; after the `UEzl` magic skip 9 bytes (magic + u32 + u8), then
   blocks `{u32 size, u8 compressed}`; a compressed block starts with `{u32 uncompressedSize, u32 headerSize(0x80),
   ...}` and the data after the 0x80-byte header is Oodle: `OodleLZ_Decompress(src, n, dst, usize, 1, 0, 0, NULL, 0,
   NULL, NULL, NULL, 0, 3)` from the game's `oo2core_8_win64.dll` (ctypes). Concatenate the blocks.
2. **HKS chunks**: in the unpacked `tables\scripts.sds`, every chunk starts with `\x1bLua`; the chunk's source name
   (`@/scripts/...lua`) and all its string constants are plain text.
3. **luaL_Reg tables**: `{char* name, lua_CFunction fn}` pairs (VA = RVA + 0x140000000); tables listed in the
   re-notes appendix. Thunks: `0x3811340(L, minArgs, flags)` = self adaptor, `0x1254DD0` isstring, `0x785B30`
   isnumber, `0x1AC6B10` isboolean, `0xE4DF10` tostring, `0x785BF0` tonumber, `0xE4DE40` toboolean, `0x3810D50` hash
   reader (string/decimal string/`C_LuaHashName`), `0x3810FF0` vector reader [likely], `0x1ABD820` push sync object,
   `0x3819C20` "wrong number of arguments (got %d)".
4. **Spawn profile names**: FNV-1 64 of the path; candidate names from data strings and spawner names, checked against
   the 2040 ids of `PlayerModelIDs.lua` (all present in `combinables\spawnprofiles.sds`).

## Appendix: all 198 recovered spawn profile names

`visuals/hero/`: game_main_tommy, game_main_tommy_1938, _bank_job, _bank_job_without_a_scarf, _blackcats,
_bloddy_forearm_gngstr1, _cab_driver_flatcap, _cab_driver_messy, _cab_driver_no_flatcap, _capone, _casual,
_criminal_outfit_hat, _criminal_outfit_no_hat, _funeral, _gangster_suit_1_hat, _gangster_suit_1_no_hat,
_gangster_suit_1_with_sarah-s_coat, _gangster_suit_2_hat, _gangster_suit_2_no_hat, _gangster_suit_2_vest,
_gangster_trenchcoat, _gangster_trenchcoat_bloody, _gangster_trenchcoat_dirty, _jail, _lincoln, _old_1950s,
_old_wedding, _racing_outfit, _reward_outfit_1, _reward_outfit_2, _reward_outfit_3, _reward_outfit_4,
_reward_outfit_5, _reward_outfit_6, _reward_outfit_9, _reward_outfit_10, _sailor, _scarf, _trenchcoat_bloody_nohat,
_vito (`_x` = `game_main_tommy_x`)

`visuals/temp/`: trailer_thug, young_morello, young_salieri

`characters/game/hero/`: game_hero; `characters/game/hero/wardrobe/game_hero_wardrobe_` + alien, bank_job, blackcats,
cab_driver_flatcap, cab_driver_no_flatcap, capone, casual, criminal_outfit_hat, criminal_outfit_no_hat, dogman,
funeral, gangster_suit_1_hat, gangster_suit_1_no_hat, gangster_suit_2_hat, gangster_suit_2_no_hat,
gangster_suit_2_vest, gangster_trenchcoat, jail, lincoln, old_1950s, old_wedding, racing_outfit, reward_outfit_1,
reward_outfit_2, reward_outfit_3, reward_outfit_4, reward_outfit_5, reward_outfit_6, reward_outfit_9,
reward_outfit_10, sailor, trenchcoat_bloody_nohat, vito

`characters/scotch/game/hero/`: game_main_tommy, game_main_tommy_1938, _bloody, _coat, _dirty, _gangster_suit_1,
_racing_outfit, _scarf

`characters/scotch/game/main/`: game_main_bertone, game_main_billy, game_main_frank, game_main_frank_omerta,
game_main_frank_suit, game_main_luigi, game_main_morello, game_main_norman, game_main_paulie, game_main_paulie_dirty,
game_main_paulie_dirty_bloody, game_main_paulie_drunk, game_main_paulie_scarf, game_main_paulie_suit, game_main_ralph,
game_main_salieri, game_main_sam, game_main_sam_dirty, game_main_sarah, game_main_sergio_suit, game_main_vincenzo

`characters/scotch/ai/`: ai_ally_bertone, ai_ally_biff, ai_ally_frank, ai_ally_luigi, ai_ally_paulie,
ai_ally_paulie_bank_job, ai_ally_paulie_drunk, ai_ally_paulie_gangster_suit_1_hat, ai_ally_paulie_gangster_suit_1_no_hat,
ai_ally_paulie_gangster_suit_2_hat, ai_ally_paulie_gangster_suit_2_no_hat, ai_ally_paulie_trenchcoat,
ai_ally_paulie_trenchcoat_dirty, ai_ally_ralph, ai_ally_salieri, ai_ally_salieri_shotgun, ai_ally_salvatore,
ai_ally_sam, ai_ally_sam_beaten, ai_ally_sam_dirty, ai_ally_sam_gangster_suit_1_hat, ai_ally_sam_gangster_suit_1_no_hat,
ai_ally_sam_gangster_suit_2_hat, ai_ally_sam_gangster_suit_2_no_hat, ai_ally_sam_gangster_suit_3_hat,
ai_ally_sam_gangster_suit_3_no_hat, ai_ally_sam_shotgun, ai_ally_sam_trenchcoat, ai_ally_sarah, ai_ally_sarah_late,
ai_ally_vincenzo, ai_enemy_morello_enforcer, ai_enemy_morello_knifeman, ai_enemy_morello_machinegunner,
ai_enemy_morello_rifleman, ai_enemy_morello_shotgunner, ai_enemy_morello_triggerman, ai_enemy_sam;
`mission/mm_050_race/`: ai_neutral_mm_050_race_driver_1, ai_neutral_mm_050_race_driver_2; `mission/mm_080_brothel/`:
ai_enemy_mm_080_manager; `mission/mm_110_omerta/`: ai_enemy_biff, ai_enemy_frank, ai_enemy_joe

`characters/traffic/`: traffic_base_ambient_combined, traffic_base_ambient_female, traffic_base_ambient_male,
traffic_black, traffic_business, traffic_business_white, traffic_drunk, traffic_irish, traffic_junkie,
traffic_low_class, traffic_low_class_black, traffic_low_class_men, traffic_low_class_men_white,
traffic_low_class_white, traffic_party, traffic_workers, traffic_workers_female, traffic_workers_male

`characters/game/`: game_attractivegirls, game_attractivemen, game_blackmen, game_blackwomen,
dlc03_opera_house/dlc03_opera_high_priest, garbagedump/game_garbagedump_crew1

`characters/ai/`: ai_enemy_italian_enforcer_lvl1; detective/ ai_detective_{machinegunner, shotgunner, triggerman},
ai_enemy_detective_{machinegunner, shotgunner, triggerman}; morello/ ai_enemy_morello_{batter, blade, enforcer,
knifeman, machinegunner, rifleman, rusher, shotgunner, sniper, triggerman}; police_scotch/ ai_police_{machinegunner,
rifleman, shotgunner, triggerman}; streetthug/ ai_enemy_streetthug_{machinegunner, shotgunner, triggerman}
