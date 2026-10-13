# Game Lua API: what we use, verified live

Everything here was tried in the running game (Steam build, exe CRC32 3168979183) through the dll's Lua bridge and
dev console (`tools\devlua.ps1`, README-dev.md). "Verified" means: called on GameMainThread, result checked in the log
and/or in a window capture. Static facts (RVAs, patterns) are in `re-notes.md`; this file is about the script API the
game's own Lua uses. Lua is Havok Script, Lua 5.1 syntax, **numbers are 32-bit floats**.

## 1. How the bindings behave (read this first)

- **Two binding styles.**
  1. *Classic* C functions from the `luaL_Reg` tables in `re-notes.md` (appendix), e.g. `C_WrapperHuman::SetDemigod`.
     They fetch `self` with the adaptor 0x3811340(L, minArgs, ...): too few arguments gives **"Missing adaptor
     arguments."** (it does NOT mean wrong `self`). Wrong types are often silently coerced.
  2. *Reflected* methods (auto-generated): their metatables have `[0]` (methods), `[1]` (getters), `[2]` (setters)
     tables and `__classname`. Wrong calls give precise errors: `bad argument #2 to 'MoveVec' (C_Vector expected, got
     table)`, `wrong number of arguments (got 2, expected 3)`. **Probing a method with a wrong argument is the
     fastest way to learn its signature.**
- `game.game:GetActivePlayer()` returns a **`C_SceneObjectLuaWrapper`** (`<SceneObject: player_archetype-2->`). Its
  `__index` falls through to the entity wrapper (`C_WrapperPlayer` -> `C_WrapperHuman` -> `C_WrapperActor` ->
  `C_WrapperEntityPos` -> `C_WrapperEntity`), so scene-object methods (`AddComponent`, `GetComponent`, `GetTransform`,
  `Activate`, `GetName`) and entity methods (`GetPos`, `InventorySelect`, `MoveVec`) work on the same value.
  Humans/cars from spawners are the same kind of value.
- **Dead references** print as `<SceneObject: nil>` and their methods become nil. Test with the global
  `IsObjectValid(o)`.
- **Async work returns `C_CntPtr<C_SyncObject>`**: `IsDone()`, `GetUserData()`. User data is
  `C_SyncObjectUserDataSuccess` (`IsSuccess()`) or `C_AICommand_SyncObjectUserData` (`IsSuccess/IsFail/IsDone/
  Terminate`). From a plain chunk you cannot `Wait()`; poll `IsDone()` on later ticks (our glue keeps state machines).
  Inside a game script thread (`StartThread(fn)`), `Wait(so)` and `Sleep(s)` work.
- **u64 parameters** (hashes, ids) accept only a Lua *number* (so only small values are exact); strings, `C_HashName`
  and HKS ui64 values are rejected ("u64 expected, got ui64"). Where a name is wanted, prefer the overload that takes
  a `C_String`.
- **Enums:** `enums.HumanMoveMode.RUN` etc. are enum objects (tostring gives the number). Some bound methods want
  the *name string* instead (MoveVec, below).
- Vectors: `Math:newVector(x, y, z)` (`.x .y .z`). Quaternions: `local q = Math:newQuat(); q.v = Math:newVector(x, y,
  z); q.s = w` (the 4-argument constructor and `Set` ignore/reject components). `GetRot()` returns one (`q.v.x ...
  q.s`). `GetTransform()` returns a 3x4 matrix (`SetPos`, `SetDir`, ...).
- Time: `os.clock()` has 1 s resolution here and the global `GetTime` is nil. Our glue accumulates the dll's tick dt
  (`MafiaCoop.now`).
- `io.open` works (handy for dumping), `string.dump(f)` returns HKS bytecode: its constant strings list the methods a
  game script calls, which is how most of the recipes below were found (e.g. `Game.Traffic.SpawnProfileHuman`).

## 2. Local player (sampling) — verified

| what | call | notes |
|---|---|---|
| player | `game.game:GetActivePlayer()` | non-nil already in the main menu (at the origin): treat "x²+y² < 1" or `game.hud:IsLoadingScreenUp()` as "not in world" |
| position / heading | `p:GetPos()`, `p:GetDir()` | dir is a unit vector (z ~ 0 on foot) |
| set position / heading | `p:SetPos(v)`, `p:SetDir(v)` | instant (teleport). Works for the player, humans and cars |
| velocity | not exposed for humans | `p:GetMoveDir()` is zero when not driven by input; we differentiate positions |
| sprint / crouch | `p:IsSprinting()`, `p:IsCoverCrouch()`, `p:IsInCover()` | booleans |
| health | `p.health` (property, 0..100) | `GetHealth` does not exist on the wrapper |
| weapon in hand | `p:InventoryGetSelected()` | weapon name (`"p_mast_a_v1"`), `"empty"` when unarmed |
| inventory | `p:GetInventoryWeapons()` | returns several strings (varargs), e.g. `exp_grenade, exp_molotov, rev_alf_a_v1, smg_trench_a_v1` |
| vehicle | `p:GetOwner()` | the car (`<SceneObject: smith_v12>`) or nil on foot |
| seat | `car:GetSeatOwner(i) == p` (i = 0 driver .. 3) | `car:GetActualPlayerSeatIndex()` gives the same 1-based |
| physics state | `p:GetPhysState()` | 0 DYNAMIC, 1 KINEMATIC, 2 DISABLED (2 while seated) |
| camera | `game.cameramanager:GetPlayerCamera()` -> `:GetPos()`, `:GetDir()` | used by the tests to place things in view |

## 3. Spawning a human with a given model — verified

There is no "spawn human" function. Humans come from a `C_RuntimeSpawnerComponent` on an **archetype** scene object;
the spawned human inherits the archetype's AI. Recipe used by `player_sync.lua`:

```lua
-- 1. a source of a civilian AI archetype: a crewed traffic car (spawn profile of the crew is ours to choose)
local so, id = game.traffic:ObtainCarWithCrew("shubert_e_six", {"visuals/hero/game_main_tommy_gangster_trenchcoat"})
-- poll so:IsDone(), then:
local spawn = game.traffic:SpawnCar(id, Math:newVector(x, y, z - 40), Math:newVector(1, 0, 0))   -- hidden below the map
-- poll spawn:IsDone(), then:
local car = game.traffic:GetSpawnedEntity(id); car:FixCarPos(true); car:SetDespawnImmunity(true)
local drv = car:GetSeatOwner(0)                                  -- a civilian with full AI ("factory")
drv:AddComponent("C_RuntimeSpawnerComponent")
local comp = drv:GetComponent("C_RuntimeSpawnerComponent")
-- 2. per puppet
comp:SetSpawnProfile("visuals/hero/game_main_tommy_gangster_suit_2_hat")   -- C_String, not a hash
local load = comp:GetSpawnProfileLoadSyncObject()                  -- poll load:IsDone()
local npc = comp:CreateObject()                                    -- <SceneObject: behavior_object_prime-NN-...>
npc:SetPos(pos); npc:SetDir(dir); npc:Activate()
-- 3. removal
comp:ReleaseObjectRaw(npc)                                         -- deletes it immediately
game.traffic:DespawnEntityImmediately(id)                          -- deletes the factory car, crew AND everything they spawned
```

- Spawn profile names: `mm_spawn_profiles` (global table) lists the hero outfits, e.g.
  `visuals/hero/game_main_tommy_{casual, gangster_suit_1_hat, gangster_suit_2_hat, gangster_trenchcoat,
  cab_driver_flatcap, racing_outfit, sailor, bank_job, ...}`. An unknown name "loads" instantly and must not be used.
- Spawned humans have entity type 8 (`HUMAN`); the player is 9 (`PLAYER`). `GetAIType()` = 1 (`FRIEND`).
- **Dead ends (do not repeat):** a spawner component on the *player* creates a human with the player archetype — it
  renders and accepts commands but never moves (no AI locomotion). `objectduplicator:DuplicateSceneObject(player,
  matrix, 0)` (3 args: scene object, 3x4 matrix, u64) starts a duplication that **crashes the game** on a worker
  thread (exe+0x328BD42). `game.entitywrapper:GetEntityByGUID(number)` returns nil (it wants a `C_EntityGuid`, which
  scripts cannot construct); `GetSceneObjectByName` needs exact SDS names.

## 4. Moving / turning a human (locomotion) — verified

| call | notes |
|---|---|
| `npc:MoveVec(pos, "WALK" \| "RUN" \| "SPRINT")` | AI path-follows to `pos` with the walk/run/sprint animation. **The mode is a string** (an enum object is accepted but read as an unknown name, i.e. no movement). Returns an AI-command sync object; `IsFail()` becomes true when the target is unreachable or blocked (e.g. by the player), the human still moves as far as it can. Re-issuing it every ~0.3 s toward a moving target gives smooth following |
| `npc:SetPos(v)`, `npc:SetDir(v)` | instant; used for > 10 m corrections and for the heading while standing |
| `npc:ScriptControl(true, "DesireMainScript")` | 2 args; test scenes use it before scripted commands. Not needed for MoveVec |
| `npc:SwitchBrain(n)` | changes `GetAIType()`; did not make a player-archetype human move |
| `player:MoveVec(...)` | returns a done sync object immediately: the player is not AI-driven |

The tests move the *local* player by small `SetPos` steps from a game script thread (`StartThread` + `Sleep(0)`),
which the remote side sees as walking/running.

## 5. Invulnerable / non-hostile / persistent — set, behaviour partly verified

`npc:SetDemigod(true)`, `npc:EnableInjury(false)`, `npc.invulnerability = true` (the ScriptHook trainer uses the same
three for god mode), `npc:DespawnImmunity(true)` (returns a sync object). All calls succeed; the puppets are
civilian-archetype humans and in the tests nobody reacted to them (police and the mission NPC ignored them, they did
not flee or attack). Not tested: damage (shooting/ramming a puppet), wanted levels, puppets holding a gun in front
of police for a long time.

## 6. Weapons — verified

| call | notes |
|---|---|
| `h:InventoryAddWeapon(name, ammo)` | names as in `WeaponIDs` of the ScriptHook scripts (`p_mast_a_v1`, `rev_alf_a_v1`, `smg_trench_a_v1`, ...). For the player any weapon works; **a civilian puppet accepts pistols (`p_mast_a_v1`) but not the Tommy gun** (no big-weapon slot) |
| `h:InventoryHaveWeapon(name)` | |
| `h:InventorySelect(name, animate, forced)` | equips; `InventoryGetSelected()` reports it ~1 s later (draw animation) |
| `game.game:GetItemIDFromName(name)` | numeric id (84 for the Tommy gun); the inventory calls want the name |

## 7. Vehicles — verified

| call | notes |
|---|---|
| `local so, id = game.traffic:ObtainSpecificCar(model)` | `id` is a small number; poll `so:IsDone()` |
| `local s = game.traffic:SpawnCar(id, pos, dir)` | poll `s:IsDone()`; `s:GetUserData():IsSuccess()` |
| `game.traffic:GetSpawnedEntity(id)` | the car (`GetType()` = 10, `GetVehicleModelName()`, `GetSeatCount()`) |
| `game.traffic:DespawnEntity(id)` / `DespawnEntityImmediately(id)` | the first waits until unseen; the second is instant and also removes the crew and anything spawned from it |
| `car:GetPos()`, `car:GetRot()` (quat), `car:GetDir()`, `car:GetSpeed()` (velocity vector), `car:GetSpeedFloat()` | |
| `car:SetPos(v)`, `car:SetRot(q)`, `car:SetDir(v)` | teleport; on a dynamic car physics continues from there |
| `car:FixCarPos(true)` | freezes the car (no gravity); `SetPos/SetRot` still move it. Our copy cars are fixed and moved every tick |
| `car:SetDespawnImmunity(true)`, `car:DisableExplosion(true)`, `car:Lock()` | |
| `carpls(model)` (global, game_structure_console.lua) | spawns a car next to the player and puts the player in the driver seat: used by the tests |
| `game.traffic:ObtainCarWithCrew(model, {profile, ...})` | car + crew humans with those spawn profiles (`SpawnCar` as above) |
| `h:GetOffVehicle()` | works for AI crew; for the player it returned a sync object but the player stayed in (the test pressed E instead) |
| `h:GetOnVehicle(car, seat, bool [, "WALK"])` / `h:MakeCarOwnership(car)` | **did not seat our puppets** in any variant tried (3/4 args, seat 0/1, unlocked car, open plaza): the command reports success or stays pending and the human stays outside. Hence puppets are hidden while their player is in a vehicle |
| `h:ShowModel(bool)`, `h:SetPhysState(0\|2)` | hide/show a human; 2 = DISABLED (no collision while hidden) |

## 8. Misc

- `game.hud:IsLoadingScreenUp()`, `game.police:Enable()/Disable()`, `game.gfx:SetWeatherSet(name, 0, 0)`.
- Globals useful for exploring: `scripts`, `mm_spawn_profiles`, `enums`, `Game.*` / `Human.*` / `Vehicle.*` (viscript
  node tables: their functions show, via `string.dump`, which wrapper methods a node calls).
- `package.loaded.traffic.GetSpawnProfiles(...)`, `Game.Traffic.SpawnProfileHuman`, `Testscenes.SpawnEnemies`
  (uses `entitywrapper:GetEntityByName("enemy%02d")` on pre-placed test-scene entities).

## 9. Menus and notifications — verified

The menus are not scripted in Lua: they are Scaleform AS3 movies whose entries are built by their own ActionScript
(docs/re-notes.md 5). What the script side offers, and what we found:

| what | notes |
|---|---|
| `game.hud:SendMessageMovie(movie, fn, args...)` | calls `_root.<fn>` of a loaded movie instance (`"HUD"`, `"MainMenu"`, ...); string/number args. Returns true even when the function does not exist |
| `game.hud:OpenMovie(path)` / `AttachMovie` / `CloseMovie` | open extra movies (test scenes use `/gui/scripted_test.swf` with `OnCreateRootMenu`, `OnSetMenuTitleText`, `OnSelectMenuItem`; not shipped as a usable menu) |
| `game.hud:RegisterScriptedMenuEvent(movie, enums.ScriptedMenuEvent.X, fn)` | menu events to Lua (`ACCEPT`, `BACK`, `MENU_SELECT_ITEM` with `selectedIndex`, ...); `Game.Hud.MainMenuEvent` registers on `"MainMenu"`. The stock main menu does not need it |
| `enums.MenuId` | E_MENUID_SERVICES / WEAPONSTORE / HIDEOUTASSIGNMENT / OUTFIT / HERBALISM: in-world (Mafia III style) menus |
| HUD banners | `SendMessageMovie("HUD", "OnShowWarningBanner", title, text)` (used by `persistent_location` with plain text), `"OnShowUnlockBanner"`, `game.hud:ShowWarning(text)`: all returned true in the world but **showed nothing** in this build, so co-op notifications are our own overlay banners in the menu style |

**How the co-op entries are added** (src/game/menu.cpp, src/game/swf_patch.cpp): the dll hooks the movie loader
(`LoadProcess::BeginSWFLoading`), and for `/gui/main_menu_main.gfx`, `/gui/pause_menu.gfx` and
`/gui/pause_menu_game.gfx` reads the file, appends a few constants to the DoABC block and prepends short bytecode
prologues to existing methods, then hands the loader a memory copy. Nothing on disk changes; if a movie does not have
the expected classes/methods the patch is skipped (logged) and the menu loads unchanged.

- Main menu, `pages::main_menu_page_main`:
  - `BuildMenu(vec)` prologue asks C++ `ExternalInterface.call("AttachIface", "MafiaCoop", "build", menuLocation)`
    and applies the answer (lines `T|title`, `P|id|label` push, `A|afterId|id|label` insert after an entry). Root
    menu: `A|9|100|CO-OP` puts CO-OP after FREE RIDE (label shows `(HOSTING)` / `(CONNECTED)`); location 50 is our
    CO-OP page (HOST CO-OP GAME / JOIN FRIEND / JOIN BY IP, or INVITE FRIENDS / STOP HOSTING, or LEAVE CO-OP; BACK).
  - `ActivateButton(data)` prologue: ids >= 100 are ours; plays the menu's select sound, sends `"select", id` and
    follows the answer: `open` (enter page 50 the way the game enters sub pages), `back` (`Back()`), `refresh`
    (`UpdateMenu()`).
- Pause menu GAME page, `pages::pause_menu_page_game`: `PopulateNavigation()` prologue shows the hidden fifth clip
  (`entry_4_mc`) as CO-OP on PC (not on Xbox, where it is "switch account"), `Select()` prologue sends `"pause"`.
- The `ExternalInterface.call("AttachIface", "MafiaCoop", ...)` calls reach the game's `AttachIface` handler, which we
  hook; anything else goes to the original. Answers are strings; the engine keeps the char pointer until it has
  converted the value, so replies are kept in a small ring buffer.
- Everything a native entry can do runs on GameMainThread and only queues a command; the render thread
  (dll/coop_ui.cpp) executes it (Coop::Host/JoinAddress/Leave, Steam overlay calls) and shows our two panels: the
  pause CO-OP panel (players with ping, actions) and the JOIN BY IP box, both drawn by our overlay in the menu style.
  While one is open the game gets no keyboard/mouse/gamepad input (DirectInput state zeroed, presses dropped, releases
  kept; XInput zeroed; window key/mouse messages swallowed; key releases always pass so Scaleform sees balanced
  key up/down) and blocking continues until the confirm/back keys are released plus 250 ms.
- Steam: `ISteamFriends::ActivateGameOverlayInviteDialog(lobby)` (INVITE FRIENDS) and `ActivateGameOverlay("Friends")`
  (JOIN FRIEND) open over the game (gameoverlayrenderer64.dll is loaded in game processes started by MafiaCoop.exe;
  with two instances on one account the dialog showed in both windows).
