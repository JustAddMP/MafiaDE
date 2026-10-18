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
| player | `game.game:GetActivePlayer()` | non-nil from boot: at the origin before the menu, at (-20, -20, 0) and invisible (`IsModelVisible()` false) in the main menu, whose 3D scene ticks scripts. "In world" = not at the origin, `not game.hud:IsLoadingScreenUp()` and `not game.game:GetIs3DMenu()` (true in the main menu, false in the world). `game.game:GetLoadedSaveId()` is `"!none!"` in the menu and e.g. `checkpoint_mm_110_omerta_cp_020` in the world |
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
| `npc:SetMovementSpeedMult(m)` | **UNVERIFIED (static, 2026-10-17)**: one number; the binding (exe+0x239D230) calls exe+0x2B9E8D0 on `[human + 0x130]`, which allocates a 0x48-byte AI command with `m` (a multiplier of the locomotion speed, by its name). Used by player puppets since 0.13.0, given right before each `MoveVec` |
| `npc:OverrideMoveSpeed(v)` / `ResetMoveSpeed()` | **UNVERIFIED (static)**: a plain store of `v` at `[human + 0x130] + 0xB0` (exe+0x2B98A70); Reset stores -1.0. Units not known (m/s or a factor). Not used |
| `npc:TurnAtDir(dir)`, `ClampMoveSpeed(...)`, `DbgTesting_Movement(...)`, `SetAnimationVariableFloat(name, v)` | exist (re-notes appendix); not tried. Puppets turn on the spot with `SetDir` (rate-limited by the controller) |

The tests move the *local* player by small `SetPos` steps from a game script thread (`StartThread` + `Sleep(0)`),
which the remote side sees as walking/running.

## 5. Invulnerable / non-hostile / persistent — set, behaviour partly verified

`npc:SetDemigod(true)`, `npc:EnableInjury(false)`, `npc.invulnerability = true` (the ScriptHook trainer uses the same
three for god mode), `npc:DespawnImmunity(true)` (returns a sync object). All calls succeed; the puppets are
civilian-archetype humans and in the tests nobody reacted to them (police and the mission NPC ignored them, they did
not flee or attack). A demigod human gets no DAMAGE events, so puppets are NOT demigods: they take damage like any NPC
and their health is topped up instead (section 11). Not tested: wanted levels, puppets holding a gun in front of police
for a long time.

## 6. Weapons — verified

| call | notes |
|---|---|
| `h:InventoryAddWeapon(name, ammo)` | names as in `WeaponIDs` of the ScriptHook scripts (`p_mast_a_v1`, `rev_alf_a_v1`, `smg_trench_a_v1`, ...). **Player only** (the binding checks the entity type; verified 2026-10-15: on an NPC it adds nothing, pistol included; the pistol a civilian puppet shows is the one its archetype already carries) |
| `h:DbgTesting_HelperInventory_AddWeapon(name, ammo)` | the same add without the player check (what the game's own `human.lua` uses to arm NPCs). Verified: an NPC took `rev_alf_a_v1`, `smg_trench_a_v1` and `sg_barker_a_v1`; a puppet armed this way draws, aims (`DbgTesting_AimAtDir`) and fires the Tommy gun (`DbgTesting_ShootReload`: its clip went 44 -> 42 per trigger press, a two-round burst). Used by `M.Weapon` |
| `h:InventoryHaveWeapon(name)` | |
| `h:InventorySelect(name, animate, forced)` | equips; `InventoryGetSelected()` reports it ~1 s later (draw animation) |
| `game.game:GetItemIDFromName(name)` | numeric id (84 for the Tommy gun); the inventory calls want the name. DAMAGE messages carry the same ids (`GetWeaponID`: 2 = `p_mast_a_v1`, 84 = `smg_trench_a_v1`, 9 = `rev_alf_a_v1`, 1 = a car) |

## 7. Vehicles — verified

| call | notes |
|---|---|
| `local so, id = game.traffic:ObtainSpecificCar(model)` | `id` is a small number; poll `so:IsDone()` |
| `local s = game.traffic:SpawnCar(id, pos, dir)` | poll `s:IsDone()`; `s:GetUserData():IsSuccess()` |
| `game.traffic:GetSpawnedEntity(id)` | the car (`GetType()` = 10, `GetVehicleModelName()`, `GetSeatCount()`) |
| `game.traffic:DespawnEntity(id)` / `DespawnEntityImmediately(id)` | the first waits until unseen; the second is instant and also removes the crew and anything spawned from it |
| `car:GetPos()`, `car:GetRot()` (quat), `car:GetDir()`, `car:GetSpeed()` (velocity vector), `car:GetSpeedFloat()` | |
| `car:SetPos(v)`, `car:SetRot(q)`, `car:SetDir(v)` | teleport; on a dynamic car physics continues from there |
| `car:FixCarPos(true)` | freezes the car (no gravity); `SetPos/SetRot` still move it. **Not usable for cars people sit in**, see "Seating" below. Only the puppet factory car (crew seated by the spawner before the freeze) uses it |
| `car:SetDespawnImmunity(true)`, `car:DisableExplosion(true)`, `car:Lock()` / `Unlock()` | |
| `car:LockPlayerEntryPoints()` / `UnlockPlayerEntryPoints()` | no args; the player cannot get in by E while locked (scripted `GetOnVehicle` still works) |
| `car:Deactivate()` (entity) | the car disappears (stays a valid object, `IsActive()` false). Used to hide a real car that is replaced by a copy; `car:Activate()` brings it back (0.15, UNVERIFIED; section 15 "Mission car back") |
| `car:GetSeatOwner(i)` | 0-based seat (0 = driver); the human or nil |
| `car:SetSpeed(n)` | takes a number (AI speed), not a velocity vector; there is no velocity setter |
| `carpls(model)` (global, game_structure_console.lua) | spawns a car next to the player and seats him as driver. Worked in some sessions and did nothing in others (cause not found); the tests now use the recipe in "Seating" |
| `game.traffic:ObtainCarWithCrew(model, {profile, ...})` | car + crew humans with those spawn profiles (`SpawnCar` as above) |
| `game.traffic:GetClosestPointOnRoad(pos, n)` | 2 args (vector, number); returns the road point and the road direction |
| `game.traffic:DespawnPlayerCarImmediately(n)` | wants a number (not the car); not used |
| `h:GetOffVehicle()` | NPC: animated exit; does nothing while the car is fixed or moved by `SetPos` every tick (exit after the car stops). Player: no effect from a plain chunk (the test presses E) |
| `h:GetOnVehicle(car, seat, animate [, moveStyle [, bool]])` | **seat is 1-based** (1 = driver, 2 = front passenger ...): the binding passes `seat - 1` to the engine, so seat 0 never works (that is why earlier tries failed). `animate = false` puts the human in the seat in the same tick. Works for our puppets and AI crew. For the **player** it only works with `player:LockControls(true)` first (unlock afterwards); used for "ride along" |
| `h:MakeCarOwnership(car)` | accepted; made no difference to seating |
| `car:SetSteer(x)`, `SetPower(x)`, `SetBrake(x)`, `SetHandbrake(x)` | **UNVERIFIED (static, 2026-10-17, docs/re-notes.md 9)**: one number each; they write the car's control wishes in its vehicle part (entity + 0xF8): steering -1..1 through a response curve, throttle 0..1 (below 0.02 = 0), brake 0..1 (below 0.04 = 0), handbrake 0..1 (below 0.02 = 0). Since 0.13.0 applied every tick to a remote driver's car copy |
| `car:GetPowerWish()` | **UNVERIFIED (static)**: reads the throttle wish (entity + 0xACC). The other controls have no getter; `mc_car_controls(id)` (ours) reads them |
| `car:GetGear()` / `SetGear(n)` | **UNVERIFIED (static)**: GetGear returns gearbox +0xB8 minus +0x19C (a gear relative to some base; negative taken as reverse). SetGear is not used (its meaning of `n` is not known); since 0.14.0 both the driver's and the copy's gear are in the sync trace (LR / CR) to compare |
| `car:SetEngineOn(on [, b])` / `IsEngineOn()` | **UNVERIFIED** (0.14.0): a car copy's engine is switched on when its driver's controls are first applied and whenever IsEngineOn says it is off (checked once a second; tried as `(true, true)`, then `(true)`), logged. In the 0.13.0 test the copies did not roll under their controls; a traffic car spawned without a driver standing with its engine off is the first suspect |
| `car:SetSpeed(v)` / `GetSpeed()` | **UNVERIFIED** (0.14.0): GetSpeed returns the velocity vector (used since 0.10.0). SetSpeed is tried with a vector, then with a number; whichever is accepted is logged and used to give a car copy its wanted velocity right after each position / rotation correction (if `SetPos` stops the body, the copy could never roll). Three probes per copy log the speed before and after SetPos, SetRot and SetSpeed |
| `car:IsCarPosFixed()`, `GetEngineRotations()`, `GetPowerInternal()`, `GetSpeedFloat()` | read for the sync trace only (CR / LR records) |
| `car:SetAddSteer(deg)` | **UNVERIFIED (static)**: degrees (x pi/180 in the binding), an extra steering angle. Not used |
| `car:SetImpulse(a, b[, part])` | **UNVERIFIED (static)**: two vectors (and a part index) to the vehicle part (exe+0x1C0A8B0); which is the impulse and which the point is not known. Not used |
| `mc_car_controls(id)` (ours, src/game/lua.cpp) | **UNVERIFIED**: `steer, power, brake, handbrake, steerLinearity, steerAngle` of car entity `id` (the number in its GUID) read from the fields above, or nil when a read fails or a value is out of range. The driver's game sends them in PlayerState |
| lights, horn, siren, indicators, beacon | **UNVERIFIED (static, 2026-10-18, docs/re-notes.md 9)**: no script getter exists; `mc_car_signals(id)` (ours) reads them from the car's vehicle part. Setters: `car:SetHornOn(on)` (horn byte; through the entity, which also plays the horn sound, at most one new sound per 2 s), `car:SetSirenOn(on)`, `car:SetBeaconLightOn(on)` (one bool each), `car:SetLightState(force, on)`: `(true, true)` forces both light groups on, `(true, false)` forces them off, `(false, x)` gives them back to the game (off, or the game's automatic mode). `SetSearchlightOn` sets light bit 0x400 (not synced). The player has no light or indicator key (no `E_CTRL_` for them; `E_CTRL_HORN` exists), so a player's lights are whatever the game switches (time of day, police car); the horn and a police car's siren are what remote players hear. Since this change the driver's game sends them in PlayerState (`signals`) and the copy gets them on change |
| `mc_car_signals(id)` / `mc_car_set_signals(id, sig, mask)` (ours, src/game/lua.cpp) | **UNVERIFIED**: the car signals byte of car entity `id` (src/game/car_signals.h: 1 park/side lights, 2 headlights, 4 indicator A, 8 indicator B, 16 beacon, 32 siren, 64 horn, +128 valid), or nil. `mc_car_set_signals` sets the light bits of `mask` (park, head, indicators) to those of `sig` with the game's own setters and returns the byte read back. Registered only when the game's setter patterns are found (their bytes confirm the field offsets) |
| `h:ShowModel(bool)`, `h:SetPhysState(0\|2)` | hide/show a human; 2 = DISABLED (no collision while hidden) |

**Seating (verified 2026-10-14, two instances, park in Omerta):**

- Puppet in a car: `npc:GetOnVehicle(car, seat0 + 1, false, "WALK")`. The car can be moved with `SetPos/SetRot` every
  tick and the human stays in (tested 4 s at 2 m/s and minutes of following a driven car).
- **Never seat a human in a car that is `FixCarPos(true)`**: the game crashes 4-60 s later on GameMainThread (call
  through a null vtable in an event dispatch, exe+0x38A50AA via +0x38A52FC/+0x38A5657, or a null write at
  +0x3855567 on the same path). Reproduced three times; seating first and fixing afterwards ran 90 s without a crash.
- `SetPos` on a fixed car builds up a bogus velocity (`GetSpeed()` read 78 m/s on an idle copy); the moment it is
  unfixed the car jumps and throws its occupants out. Hence copies are never fixed: a normal car moved by
  `SetPos/SetRot` every tick has ~0 velocity and keeps its occupants.
- A car the **local player has driven** throws an NPC out of its driver seat a few frames after every
  `GetOnVehicle` (with or without `MakeCarOwnership`, `DestroyController`, `FakePlayerCar`, and also without any
  `SetPos`). Passenger seats are fine. Fix: replace that car by a fresh copy (`Deactivate` the real car).
  **These tests were made with civilian-brain puppets; section 18 explains the throw-outs by that brain** (a
  friend-brain puppet stayed seated where a civilian was thrown out).
- Making a hidden puppet DYNAMIC again (`SetPhysState(0)`) right before `GetOnVehicle` gets it thrown out; show it
  with `ShowModel(true)` only and let the seat command manage physics.
- A puppet that was walking (`MoveVec` pending) sometimes gets out again right after being seated; retrying a
  second later sticks.
- Player as passenger: `p:LockControls(true); p:GetOnVehicle(car, 2, false, "WALK")`, then `p:LockControls(false)`
  when the sync object is done; E gets out normally. With the driver seat empty the game puts the player at the
  wheel instead (0.13.0 two-PC test, section 18).

## 8. Misc

- `game.hud:IsLoadingScreenUp()`, `game.police:Enable()/Disable()`.
- **Weather and time (verified 2026-10-14, used by WorldEnv):** `game.gfx:GetCurrentWeatherSetName()` returns the active
  weather set (`mm_110_omerta_cp_010_cs_cs_park` in the Omerta park save; `_default_game_rainy` after setting it);
  `game.gfx:SetWeatherSet(name, 0, 0)` switches at once (names: `WeatherIDs.lua` of the ScriptHook scripts, e.g.
  `_default_game`, `_default_game_rainy`, `_default_game_foggy`, `day_cycle_01`). `game.gfx:GetTime()` is the time of
  day in hours (11 in that save), `game.gfx:SetTime(hours, 0)` (2 args; 1 arg = "Missing adaptor arguments"; returns a
  sync object) sets it. `GetTimeFlowSpeed()` / `SetTimeFlowSpeed(n)` (1 arg): 0 in missions (time stands still), the
  game's day cycle runs when it is > 0 (flow 20 = 1 game hour per ~3 s). The game's own node scripts
  (`Game.Weather.SetTime`, `Game.Game.SetMissionWeatherAndTime`) make the same calls.
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

**How the co-op entries are added** (src/game/menu.cpp, core's external/core/src/swf/swf_patch.cpp): the dll hooks the movie loader
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
  (JOIN FRIEND > STEAM FRIENDS LIST; JOIN FRIEND itself is core's friends panel, core docs/protocol.md "JOIN FRIEND")
  open over the game (gameoverlayrenderer64.dll is loaded in game processes started by MafiaCoop.exe;
  with two instances on one account the dialog showed in both windows).

**Button prompts (footer) and the gamepad** (static, from the ActionScript of `main_menu_main.gfx` and the shared
`scripts.infobar` classes; how they look with a pad NOT checked in game):
- The menus' footer is `scripts.infobar::Infobar` (CLIK component, laid out bottom right). A page builds a
  `Vector.<infobar_item>` of `infobar_item_new(game, padButton, pcType, label, command, keyStringId)` and hands it to
  `menu_parent.setupInfoBar(vec)`. Pad buttons are strings (`Infobar.CTRL_A` = "a", `CTRL_B` "b", `CTRL_X`, `CTRL_Y`,
  `CTRL_LB`/`RB`, `CTRL_LT`/`RT`, `CTRL_LS`/`RS`, `_CLICK`, `CTRL_BACK`), `pcType` `CTRL_KEY` = "key" with a key
  name string id (`ENTER_STRING_ID` `$59280027`, `ESC_STRING_ID` `$59280000`, `X_STRING_ID` `$59280044`,
  `Y_STRING_ID` `$59280020`, `H_STRING_ID`, `UP`/`DOWN`/`LEFT`/`RIGHT_STRING_ID`), `command(page, this.Select)`.
  `Infobar.onKeyEvent` hands every input event to its items, so the footer's buttons are what run the commands.
  Glyphs come from `prompt_icons.swf` (`UEIcon` frames such as `enter-gamepad_A`, `escape-gamepad_B`, `gamepad_X`,
  `gamepad_L1`, ...); the device switch is `UEIcon.deviceType`: on PC it follows the engine's data binding
  `CONTROLLER_DEVICE` / `ControllerDeviceType` (`controlSettings.PlayerControllerDeviceType`: `KEYBOARD_MOUSE` or
  `JOYSTICK_CLASS`), i.e. the C++ input layer decides and every prompt redraws.
- `main_menu_page_main.setupInfoBar()` = A / Enter `$menu_select` -> `Select()`; `setupInfoBarBack()` = that plus
  B / Esc `$menu_back` -> `Back()`. `UpdateMenu()` calls `setupInfoBar()` only for ROOT_MENU; every sub page gets
  `setupInfoBarBack()` from `ActivateButton` before it switches `menuLocation`, and `Back()` restores `setupInfoBar()`
  when it returns to ROOT. Our CO-OP page does exactly that (the `open` answer calls `setupInfoBarBack`, BACK and the
  pad's B run the game's `Back()`), so on the native page the footer, its glyph switching and A / B are the game's
  own; nothing else is needed there (a "refresh" rebuild goes through `UpdateMenu`, whose default case leaves the
  footer alone). The pause menu's GAME page keeps its own footer; our fifth entry only adds a `Select()` case.
- **Our panels** (core `dll/coop_ui.cpp`: LOBBY, CO-OP SETTINGS, JOIN FRIEND, JOIN BY IP / PASSWORD / NAME boxes,
  PLAYER / KICK dialogs, the pause CO-OP panel) are drawn by our overlay, so they cannot use the Infobar; they copy
  its behaviour (core `dll/ui_prompts.h`): the footer shows pad glyphs after pad input and key names after a key, a
  click or a real mouse move (core `ui_input` follows every XInput read and the window messages all the time, so a
  panel opens with the device the game used last; log `ui_input: prompts now for gamepad (pad buttons 0x1000)` /
  `keyboard/mouse (key 0xD)`). Glyphs are drawn Xbox style (coloured A/B/X/Y discs, LB/RB bars, a d-pad cross for
  Left/Right), not the game's `prompt_icons.swf` art (UNVERIFIED how close they look). One prompt list per panel
  drives both the footer and the pad, so a button does what its prompt says and nothing without one:
  A / Enter Select (Join, Connect, Save, Set, Yes), B / Esc Back (Cancel, No), d-pad / Left-Right Change on
  `< VALUE >` rows; lobby: X Start (host) / Ready or Join game (guest), Y Invite (host); pause CO-OP panel while
  hosting: Y Invite; JOIN FRIEND: Y / F5 Refresh; text boxes: X / Backspace Delete, Y / Ctrl+V Paste. X / Y are pad
  shortcuts for list entries and are not shown with the keyboard. The panel-open log line and the test `dump` print
  the footer (`footer gamepad:  [A] Select  [X] Start  [Y] Invite  [B] Back`).

## 10. Combat on a human: aim, shoot, crouch, reload — verified 2026-10-14

What the puppets use (src/game/player_sync.lua `M.Combat`). The puppet is a civilian-archetype AI human with a pistol.

| call | notes |
|---|---|
| `h:DbgTesting_AimAtDir(dir)` | aims the weapon in hand along a world direction (C_Vector, any length; z tilts it); the body turns with it. Holds until `h:DbgTesting_AimAtStop()`. Used by the game's `Human.NPC.DBG.DebugAimAtDir` node. Re-issue to change direction |
| `h:DbgTesting_ShootReload(trigger, reload, 0)` | presses (true) or releases the trigger / reload button of an AI human. The AI reads it on its next update, so a press must stay until `Wait(h:DbgTesting_SemaphoreNextFrame())` (a sync object; poll `IsDone()` outside a script thread) before the release; pressing and releasing within 1-4 frames often does nothing. One press = one real shot (bullet, muzzle flash, sound), also while not aiming. `human.ShootWithParams(h, maxShots, shotsPerSecond)` (game Lua, needs a script thread) does the same in a loop |
| `h:DbgTesting_HelperFire_GetAmmoCountInEquipedWeapon()` | rounds in the clip of the weapon in hand (player too). The player's shots are counted from drops of this value |
| `h:SetStalkMove(bool)` / `IsStalkMove()` | crouch (head bone ~0.94 m above the feet instead of ~1.6 m); a puppet that is still walking (MoveVec pending) stays up until it stops. Aiming stands it up |
| `h:IsReloading()` | true during the reload animation (AI: right after a reload press). `p:InventoryReload()` on the player refills the clip instantly without animation |
| `h:GetBonePos("Head")` | world position of the head bone (other names, numbers: the entity position) |
| `h:DoDamage(n)` | hurts the human (player: `p.health` 100 -> 37 after `DoDamage(35)`, armour/difficulty scaling); `p.health = x` is ignored |
| `h:AimAt(target, offset, bool)` / `h:ShootAt(target, offset, n)` / `h:Aim(bool)` | AI combat commands (3 args each; target = a human, offset a C_Vector). `AimAt(self, worldPos, true)` aimed the puppet in the wrong direction (offset is not a world position) and the command reports `IsFail()`; `AimAt(otherHuman, (0,0,0), true)` aims at that human. Not used |
| `h:ShootEffect(from, to, dir, n, n)` | 5 args, returned a done sync object, no visible effect. Not used |
| `h:PanicCrouch(seconds)` | a civilian panic crouch (hands over the head); `DbgTesting_SetPose(n)` did nothing visible |

Local player: there is no script getter for "aiming" (`GetAimDiff()` stayed (0,0,0) and `IsButtonDown(n)` takes gamepad
button enums only: `enums.E_Script_GamepadButtons.BUTT_A/B/X/Y` = 6..9), so the dll reads the aim input itself (right
mouse button / pad left trigger in the focused game window) and only reports it while `HaveFireWeaponInHand()`. The
aim direction is the player camera's `GetDir()`. `p:ShootWeapon(true)` makes the player fire, about 1 s later and not
every time (unreliable; used only by tests). `DbgTesting_ShootReload` has no effect on the player.

Test notes: synthetic mouse **button** events (`mouse_event`/SendInput) are ignored by the game (mouse movement and
keyboard with scan codes work), so in tests the guest sets `MafiaCoop.testAim = true` (or `{x, y, z}` for a fixed
direction) from the dev console instead of holding the right button, and fires with `p:ShootWeapon(true)`. The default
crouch key (C) did nothing in the Omerta park save (crouch may be mission-gated); `SetStalkMove(true)` on the player
crouches him and is reported.

## 11. Damage, death and health — verified 2026-10-15

Two instances on one PC, story save (Omerta park, `mm_110_omerta_cp_010`) and Free Ride (`freeroam_test`). What the
co-op damage uses (src/game/player_sync.lua "Damage between players"; message flow in docs/protocol.md "Hit").

| call / event | notes |
|---|---|
| `game.game:RegisterBlanketEvent(ctx, fn, enums.EventType.HUMAN, enums.HumanMessages.DAMAGE)` | one subscription for every human; `fn(ctx, msg)`; returns an id (`UnregisterBlanketEvent(id)`). `msg:GetReceiverGUID()` is the victim: compare `tostring()` with `tostring(h:GetGUID())`. Also `DEATH` (`GetKillerGUID`, `GetDamageType`). A per-human `h:RegisterEvent(ctx, fn, et, msg)` works the same way. Enum values as in research-world-sync.md 0.1 (EventType.HUMAN 3, DAMAGE 458772, DEATH 458777, ON_BULLET_HIT 458756, DamageType.SHOT 0, Hitzone.HEAD 2) |
| `C_DamageMessage` getters | `GetEnemyGUID` (the attacker: the player for his bullets, the player for rams with the car he drives, the shooting NPC for `CreateShots` from an NPC), `GetDamageType` (0 SHOT, 9 SCRIPT, 11 COLLISION, 12 FALL), `GetDamageAmount` (10 for a pistol or Tommy gun bullet on an NPC; 0.02-0.8 per frame of car contact), `GetHitZone`, `GetWeaponID`, `GetCanHeadShot` |
| `ON_BULLET_HIT` | sent to the **shooter**; `GetHitObject()` is the human or object hit (a bench: `lh_bench_a_v1_bv01_13`) |
| player GUID | the local player is `C_EntityGuid: 24585` in every instance (the same entity id in each game) |
| player melee (key **Q**: tap = light punch, hold about 1 s = charged punch; Counter = Left Alt; the default keys also include Stalk = X, Cover = Left Ctrl, Interact = E) | **Only outside safehouse zones** (below). The swing sends `MELEE_LIGHT_ATTACK` (458809) or `MELEE_HEAVY_ATTACK` (458810) to the attacker himself, `GetOpponentGUID()` = the human the game aimed it at (0 = none). If that is an NPC, about 0.2 s later the NPC gets DAMAGE type 3 (MELEE) with the attacker as enemy: 10 for a light, 36 for a charged punch (17 and 61 health off a puppet), plus MELEE_HIT_REACT (458814) and MELEE_END (458816). Six punches in a row on the same puppet all landed (no guard). **The game only targets humans with AI type 0** (`h:GetAIType()`): spawn profiles `characters/scotch/game/main/game_main_*` give 0; `characters/scotch/ai/ai_ally_*`, `visuals/hero/*`, `characters/game/hero/*` give 1, and a swing at those always has opponent 0 and hurts nobody, even standing right in front of it. `SetCivilArchetype`/`SetBattleArchetype` (decimal hash strings accepted) change the archetypes but not the AI type. The co-op glue (`M.MeleeTick`) therefore sends a Hit for a swing with opponent 0 when a puppet stands within 1.9 m in front of the player at the impact (0.25 s), and gives that puppet a `DoDamage(1)` flinch |
| `h:DoDamage(n)` | one number. SCRIPT damage (type 9, attacker GUID 0, amount 0 in the message), self as sender. On the player: a short flinch and the red damage vignette; the game scales it (`p.health` 100 -> 93.5 for `DoDamage(10)`: 0.65 on this difficulty). A big value kills (YOU DIED) |
| `game.shotmgr:CreateShots(shooter, from, to, weaponName, scatter, timeout, particle, sound)` | real bullets owned by `shooter`. `timeout` 0 = one shot, 1 = two or three (the weapon's rate over 1 s). Player as shooter: the target NPC gets SHOT damage with `GetEnemyGUID` = player, the zone where it hit and its own hit reaction. **NPC (puppet) as shooter at the player works**: the player gets a SHOT DAMAGE with `GetEnemyGUID` = that NPC and his real hit reaction (health 100 -> 74 for 10 damage: NPC bullets are scaled about 2.6x on the player). **Unreliable**: in some game instances every `CreateShots` call (any shooter, any target) did nothing for the whole run, in others it worked every time; not explained (not window focus, not the model swap, not old sync objects). The glue replays bullets with it and falls back to `DoDamage` when the replay did not land within 0.3 s |
| `h.health` | health of the player and of NPCs; **writable on NPCs** (`so.health = 5000` sticks; `healthmax` stays 105), not on the player. A puppet knocked down by a car came back with `healthmax` 100 and health 100 |
| `h:HealByMedkit()` | refills the player's health (a frame later) |
| `h:SetMaxHealth()` | no args, accepted; no visible change on a player at full health |
| `h:ApplyRagdollRadialSpeed(center, speed, radius)` | accepted on the player, no visible effect |
| `h:AttackMelee(target)` | on a puppet (FRIEND AI) aimed at the player: accepted, nothing happens |
| `p:AttackMelee(puppet)` | on the player: accepted, nothing happens |
| `h:DbgTesting_HumanCombatAction(n, 0)` | on the player, n = 0..5: accepted, nothing visible |
| `game.savesystem:Retry()` | during the death camera (before YOU DIED appears): loads the last checkpoint at once, like "Load Last Checkpoint". **Scripts do not run while the YOU DIED menu is up**, so it has to be called before. Used for guests |
| `game.savesystem:RequestCheckpointSave()` | writes a checkpoint save at once (save0.sav of the active slot) |
| `p:IsInputDisabled(enums.DisabledInput.X)` / `p:DisableInput(lockName, disable, input, ...)` | **safehouse zones**: around Salieri's bar (where Free Ride starts, about (-943, -233)) the game locks (the Omerta park story checkpoint has a mission lock with the same flags) SPRINT, MELEE, AIM, WEAPON, COVER, STALK and GRENADE (`IsInputDisabled` true; MOVE, SHOOT, ACTION, CAMERA false): Q, the pad buttons and scripted melee do nothing there. The lock comes from the game's safehouse script (`data_input_presets.input_presets.safehouse`, 21 inputs); `DisableInput("safehouse", false, ...)` does not lift it. Teleported to the Omerta park (-583.5, 121.5) in Free Ride, every flag read false and punches worked. Walking 20 m from the Free Ride start did not leave the zone |

Learned the hard way:
- A player can be killed on his own screen by a remote player's **car copy** teleported through him: the copy follows
  the driver's transform, a teleport of the real car was lerped across 30 m and ran the guest over (DEATH, type 12).
  The interpolation now snaps instead of lerping when two snapshots are more than 20 m apart. Checked 2026-10-15: the host's
  car teleported 40 m with the guest standing in the middle of the line; on the guest's game the copy jumped between
  two ticks (closest 19.7 m), no damage.
- Puppets (civilian AI) **step out of the way of cars** (HUMAN message 458774 several times as the car comes), so a
  ram missed them. `h:InterruptAvoidingCar()` (no args) stops that; the glue calls it on every puppet. Then a car
  pushing a puppet gives one COLLISION DAMAGE (type 11, enemy = the driver) per frame of contact, 0.014-0.024 each;
  summed over 0.25 s they become Hits of 1.2-1.5 (three Hits for one push at low speed).
- Teleporting the player next to a car on the first tick after a checkpoint load crashed the game in the HUD
  (`ShowPrompt` on a null movie, exe+0x3024566). Wait a few seconds after the load before moving him.
- `npc:SwitchBrain(2)` + `EnableCloseCombat(true)` + `AttackMelee(puppet)` on a spawned NPC crashed the game a second
  later on GameMainThread (null read at exe+0x733e7d in the AI update). Do not script NPC fist fights this way.
- Synthetic mouse clicks (SendInput, mouse_event, posted WM_LBUTTONDOWN) do nothing in game. The keyboard is the only
  DirectInput device the game polls (logged by a test build); the mouse comes another way (GetCursorPos, messages).
  Keyboard keys with the scan-code flag work. Tests fire with `p:ShootWeapon(true)` (along the camera's crosshair,
  about one call in seven hit a puppet 7 m away) or with `CreateShots`.
- The game reads the gamepad through `XInputGetState` (xinput9_1_0.dll), which core already hooks. With `test_pad = 1`
  / `--test_pad 1` the dll adds a virtual pad on user 0 that the game uses like a real one (left stick walked the
  player 4.7 m in 1.5 s); tests press it from Lua with `mc_pad(buttons, holdMs, lt, rt, lx, ly, rx, ry)` (XINPUT bits,
  e.g. `mc_pad(0, 1500, 0, 0, 0, 30000)` = stick forward). Dev Lua does not run while the pause menu is open (scripts
  are paused), so it cannot press the pad inside our panels; core 0.7's `test_input` does (README-dev.md "Testing the
  co-op panels"). The game itself ignored the test pad in some sessions (no walking, START did not open the pause
  menu) while our panels, which read XInput through the same hook, always saw it.

## 12. Player model (character) — verified 2026-10-15

| call | notes |
|---|---|
| `p:SwitchOverridePlayerSpawnProfile(profile)` | profile = path string (`"characters/scotch/game/main/game_main_paulie"`) or a decimal-hash string (`"16922695808871587265"`, the same profile); a Lua number is rejected (`C_LuaHashName expected, got number`). Returns a sync object, done within a few ticks. **The player wrapper survives**: same `GetGUID()` (24585), still valid, inventory kept. Used by `M.ModelTick` |
| `p:SwitchDefaultPlayerSpawnProfile(profile)` | what the checkpoint scripts call. While an override is set the model stays the override (default switched to `tommy_casual`: Paulie stayed) |
| `p:ClearOverridePlayerSpawnProfile()` | back to the default slot (casual Tommy after the switch above) |
| `p:SwitchPlayerSpawnProfile(tohashname(profile))` | the raw swap. Did not visibly change the guest at session start (the checkpoint setup applies the slots again); not used |
| `p:IsPlayerUsingDefaultCostume()` | false with an override and also after clearing it; not useful |
| save files | **the override is written into the save**: after `RequestCheckpointSave()` with Paulie as override, save0.sav holds the default (Tommy suit) and the Paulie hash next to each other in the player model block. A single-player Continue of that save still loaded Tommy (the checkpoint setup clears the override), so a guest's own story is not changed by it |
| weapons | Paulie as the player took every weapon (`p_mast_a_v1`, `rev_alf_a_v1`, `sg_barker_a_v1`, `smg_trench_a_v1`, `sn_mayw_a_v1`) and drew and fired the Tommy gun |
| puppets | `comp:SetSpawnProfile("characters/scotch/game/main/game_main_paulie" / "..._sam")` spawns the same characters with the civilian AI. AI type (`GetAIType()`): 0 for `characters/scotch/game/main/*`, 1 for `characters/scotch/ai/ai_ally_*`, `visuals/hero/*` and `characters/game/hero/*` (matters for punches, section 11). A **decimal hash string** works too (`"11254217090474872233"` = `visuals/hero/game_main_tommy_casual`) |
| story outfits | a `characters/scotch/game/main/game_main_<name>` profile picks its clothes from the loaded story point, the same for the player and a puppet in the same game: at Omerta cp_020 Paulie wore a long grey coat and hat and Sam a grey suit, both as player and as puppet; in Free Ride Paulie wore a cap. So the "Sam's puppet wears a coat, his player model a suit" mismatch seen earlier is two games at different story points, not player vs puppet. The `characters/scotch/ai/ai_ally_<name>_<outfit>` profiles (`paulie_gangster_suit_1_no_hat`, `_gangster_suit_2_hat`, `_trenchcoat`, same for Sam) look the same everywhere |
| checked list (2026-10-15) | as the local player (override, `IsSuccess()` true and seen in a capture) and as a puppet (spawned, seen): `game_main_` paulie, paulie_suit, paulie_scarf, sam, sam_dirty, salieri, frank, frank_suit, frank_omerta, vincenzo, luigi, ralph, sarah, morello, sergio_suit, norman, bertone, billy; `ai_ally_` paulie / sam outfits above, salieri; puppets also ai_ally_ frank, vincenzo, luigi, ralph, sarah, salvatore, bertone, biff; Tommy's `characters/game/hero/wardrobe/game_hero_wardrobe_` casual, cab_driver_flatcap, gangster_suit_2_hat, gangster_trenchcoat, racing_outfit, sailor (player and puppet). The list in the CO-OP panel is src/game/characters.cpp. Sailor Tommy drew and fired the Tommy gun; Sam (`game_main_sam`) drove a car |
| unknown profile | player: `SwitchOverridePlayerSpawnProfile("characters/scotch/ai/ai_does_not_exist_xyz")` returns a sync object that never finishes; the model stays; a later valid switch works. Puppet: `SetSpawnProfile` with it never finishes loading and the spawner component stays stuck on it (a valid profile set afterwards never loads either) until the factory car is despawned and made again. The glue gives both 12 s, then marks the profile bad and uses the default character for that player id |
| model manager | the default slot (story / Free Ride outfit) is readable from the dll (docs/re-notes.md 4): in Free Ride it held `visuals/hero/game_main_tommy_cab_driver_flatcap`. The host as "story Tommy" reports that hash, so the others' puppet of him wears his real Free Ride clothes instead of the trench coat |
| `h:ShowHat(false)`, `h:SetDirtBlend(1)` | accepted on the player, no visible change |
| `game.outfits:GetCurrentOutfit()` | a `C_HashName` that compared unequal to `ToHashName("casual")` and `("cab_driver_flatcap")` at the Omerta checkpoint; `IsLocked("vito")` and `IsLocked("casual")` both false |
| `enums.ModelParts` | HEAD 0 .. BODY 9, LAST 10, as in the static research. `C_ModelComponent` (SetSubdivType, SetDynamic, reflections, resource id) and `C_ClothComponent` exist on the player, neither switches parts; `C_SpawnedObjectComponent` is nil |
| Free Ride | `GetLoadedSaveId()` = `freeroam_test`; `mm_spawn_profiles` has no entry for it. Resolving "tommy" locally therefore gives the trench coat; since 0.4.0 the host's own game sends its model-manager profile instead (row above) |
| `game.outfits:GetCurrentOutfit()` in Free Ride | `tostring` gives a number like 140737180990744 that matches no `ToHashName(<outfit id>)` (23 ids tried); not usable |

## 13. Screen projection, minimap markers, starting a mode — verified 2026-10-15

Two instances on one PC, Omerta park story checkpoint and Free Ride. Used by the name tags, the map markers and the
lobby (src/game/player_sync.lua "Name tags and map markers", src/game/module.cpp, src/game/menu.cpp).

| call | notes |
|---|---|
| `game.game:ProjectPointToScreen(v)` | returns **three values** `x, y, onScreen`: x and y in -1..1 from the screen centre, **y up** (a point 10 m above the head gave y 10.3), `onScreen` false for points behind the camera or outside the view. Pixel = ((x + 1) / 2 * w, (1 - y) / 2 * h). A point 20 m along the camera's direction gave (0, 0) |
| `game.cameramanager:GetPlayerCamera():GetPos()` / `GetDir()` | camera position and direction (distance for the tag fade) |
| `game.hud:GetScreenResolution()` / `GetScreenAspectRatio()` | 1280 800 / 1.6 (the game's render resolution; the dll draws in backbuffer pixels instead) |
| `h:IsVisibleFromCamera()` | true for humans far off screen and behind the camera too: **not** a visibility or occlusion test |
| `game.physics:StartCollisionQuery(from, to, x)` | 3 arguments (2 or 4 = wrong number), returns a sync object whose user data is a `C_WrapperCollisionResult` (`HasHit`, `GetHitEntity`, `GetHitPos`, `GetHitNormal`, `GetHitFraction`). With the third argument 0, 1, -1, 65535, 2, 4, 8 or 16 a ray from the camera 20 m straight down never hit anything; `MakeCollLayers(...)` returns a `C_EntityGuid: 0` for every argument tried. Also with the sync object awaited (`Wait(so)`, then `so:UserData()`), 16 argument combinations: never a hit. The ray cast behind `GetCollisionPointBelow` works, see `mc_raycast` |
| `game.physics:GetCollisionPointBelow(v)` | a vector: the first collision below `v` (a real physics ray: the floor 1.2 m under the camera) |
| `mc_raycast(x1, y1, z1, x2, y2, z2)` (ours, src/game/lua.cpp) | `hit, x, y, z`: the game's own ray cast with any from/to (the query `GetCollisionPointBelow` builds, re-notes "Pattern table" `RayCast`); nil when not available. Walls, floors and cars hit, a ray through an open courtyard did not. **Name tags use it for occlusion** (0.10.0): five times a second a ray from the camera to the head (not counting hits at the camera, at our own player or at the head itself); two hits in a row hide the tag. Checked in Free Ride: a guest 14 m away in the open had his tag, behind a building none (`tag-visible2.png`, `tag-hidden1.png`) |
| `game.navigation:RegisterHumanEntityDirect(human, 1, 1)` | **minimap/map marker on a human**: returns an id (11, 12, ...); an orange marker appeared on the minimap at the human's position and followed it (also on the guest's map for the host's puppet in Free Ride). `RegisterHumanEntity(name, type, state)` wants the entity **name** (string) as 2nd argument. Types 0, 2-6 (state 1) and states 0, 2, 3 (type 1) showed nothing, but they were tried on a human that had had an icon before: after `UnregisterId(id)` the **same entity never gets a second icon** (every later register returns a new id and shows nothing; a fresh human gets one at once). The game's own node `Game.Hud.CreateHumanIcon` uses this call (`inEntity`, `inHumanNavType`, `inHumanNavState`); other icons: `RegisterIconEntityDirect(entity, icon, textId, desc, showText, showIcon, picture[, color])` (`Game.Hud.CreateMapIconOnEntity`), not used |
| `game.navigation:UnregisterId(id)` | removes the marker |

**Every other player on the map (static only, NOT verified in game).** Since this change every other player in the
session has exactly one navigation icon while we are in a world, whatever he does (src/game/player_sync.lua "Map
markers", the decision in src/game/map_markers.h, unit-tested by tests/map_markers_test.cpp):

| his state | his icon |
|---|---|
| puppet alive, shown, on foot | the human icon on the puppet (`RegisterHumanEntityDirect(puppet, 1, 1)`, verified above), it follows the entity |
| driving or riding (seat 0-3) | the same icon at his car's spot from his latest state (`cx cy cz` of the `T` command), moved with `ChangeIconTargetToPos` when he is more than 1 m from it, at most 5 times a second |
| puppet hidden (cutscene, no car copy yet), dead and respawning, re-created, model swapped, factory reset, puppet never spawned, far away | the same icon at his position from his latest state (the C++ side sends every remote player's state every tick, near or far) |
| no puppet yet in this world (just joined, far away, his model still loading) | a position icon (`RegisterIconPos`, type `freeride-generic`, UNVERIFIED look), replaced by the human icon once his puppet exists |
| left / session over / map markers off / our world goes (loading screen, mission_sync's quit) | `UnregisterId` |

The icon is moved off a puppet (to his position) **before** the puppet is released (`M.Unmark`: the factory reset and
`M.PuppetLife`'s respawn; `M.Remove` unregisters it), so the navigation module never reads a released puppet through
an icon (the crash in section 15). Because the game gives an entity no second human icon after `UnregisterId`, the
icon is kept and moved (`ChangeIconTargetToEntityDirect` onto a new puppet) instead of being made again; a puppet
whose icon was unregistered is remembered (`pp.mkBurnt`) and only gets an icon moved onto it. The name of the player
(the `N id name` command, once a second from player_sync.cpp) is set with `SetIconTextByNavId`; every icon of ours gets
`SetIconFarVisibility(id, true)`. Log: `sync: player <id>: map marker <nav id> made on his puppet / at his position /
off his puppet / on his puppet / removed (<why>)`; trace `MK` (counts, 1 Hz) and `MKE` (each change), sync-trace.md.

What the exe and the game's own scripts say about `game.navigation` (`C_NavigationScriptW`, method table RVA
0x5AF9720: 106 methods plus __gc and __eq; re-notes lists the first 88; the argument types are read from the classic binding's checks:
`isnumber` 0x785B30, `isstring` 0x1254DD0, `isboolean` 0x1AC6B10, entity 0x1ABD980, vehicle 0x1ABD570, vector
0x3810FF0, self adaptor 0x3811340; usage from the game's compiled scripts in `sds_retail/tables/scripts.sds` and
`script/vi_flownodes.sds`, unpacked with the game's own `oo2core_8_win64.dll`):

| call | arguments (static) | notes |
|---|---|---|
| `RegisterHumanEntityDirect(human, type, state)` | entity (must be a human: entity type byte 8), number, number -> id | looks the entity up first (map by GUID at manager +0x170): a human icon already on it is **updated and its id returned**; `type` is `script_enums.HumanNavType` (names SQUEALER, BOSS, UNFRIENDLY, FRIEND, POLICE, WINGMAN; the numbers are not in the data we read; 1 = the orange one we use, likely SQUEALER), `state` `HumanNavState` (DEFAULT, SEARCH, RECOGNIZE, ANGRY) |
| `RegisterIconPos(...)` | 5 to 11 arguments: `(x, y, icon, text, showText)`, `(x, y, icon, text, showText, desc)`, `(+ picture)`, `(+ colour number)`, `(x, y, icon, text, showText, desc, picture, colour, s, s)`, `(x, y, z, icon, text, showText, desc, picture, colour, s, s)` -> id | the shorter forms put z = 0, colour 0xFFFFFF, desc "0". The game calls e.g. `RegisterIconPos(x, y, "relay_collectable", "UI_Text_home", .., "UI_Text_homeDescription", "/gui/pop/home.dds")` and `RegisterIconPos(x, y, z, "race_checkpoint", "racing_checkpoint", .., tonumber("FFFFFF", 16), ..)`. `icon` is a name from the navigation config in `ingame_gui.sds` (e.g. `freeride-generic`, `freeride-challenge`, `car_player`, `car_hostile`, `race_checkpoint`, `relay_collectable`, `collectable_*`, `health_station`, `objective_warning`, `informant`); `text`/`desc` are string table ids |
| `RegisterIconEntityDirect(entity, ...)` | same overload scheme with an entity first | used by the game for informants, pickups (`grow_house_pickup`), health stations |
| `RegisterVehicleDirect(car, type)` | vehicle, string -> id | `type` from the vehicle list `enemy police_car taxi_car enemy_squeeler enemy_enforcer enemy_boat police_boat player_car player_boat vehicle taxi_target`; the game uses `"enemy"` (node `Game.Hud.CreateVehicleIcon`, default `enemy`). Not used: a car copy is despawned in many places (M.Swap, M.Restore, M.DropCar), and an icon on a released car is the same crash risk as on a puppet |
| `ChangeIconTargetToPos(id, vector)` | number, C_Vector -> bool | moves an icon (any kind) to a position; unlinks it from its entity (0x305FBC0) first. The game's objective scripts switch icons between entities and positions with this pair |
| `ChangeIconTargetToEntityDirect(id, entity)` | number, entity -> bool | puts an icon on an entity (false when the entity is not usable or the id unknown) |
| `SetIconTextByNavId(id, text)` | number, string | the game sets e.g. `"%\nDEFENSE"` texts with it; where the full map shows it (hover popup?) is UNVERIFIED |
| `SetIconFarVisibility(id, bool)` | number, boolean | the flow node `Game:Hud` icon nodes feed it from their `inRadarEdge` input: most likely "stay on the minimap's edge when out of range" (UNVERIFIED) |
| `SetIconMapVisibilityByNavId(id, bool)` / `SetIconRadarVisibilityByNavId(id, bool)` | number, boolean | map / minimap visibility of one icon (not used) |
| `SetIndicatorHideRange(id, m)` | number, number | (not used) |
| `SetIconFakeGPSPositionOnEntity(id, vector)`, `SetIconOverrideIndicatorPosition(id, vector)`, `SelectIcon(id)` | | (not used) |

UNVERIFIED (needs the in-game test): that a human icon moved with `ChangeIconTargetToPos` is still drawn (on the
minimap and the full map) and moves when moved again; that `freeride-generic` gives a visible icon; that
`ChangeIconTargetToEntityDirect` puts a moved icon back on a (new) puppet; where the name shows; what
`SetIconFarVisibility` does; whether the navigation module keeps or drops icons across a world load (we unregister
ours on the first tick without a world, and forget, without unregistering, any id made in an earlier world).

**Starting Free Ride / Continue from the main menu.** The main menu's AS3 (`pages::main_menu_page_main`) starts a
game with `menu_parent.LoadGame(name)` (`main_menu` class): `"free_ride"` (the FREE RIDE entry, and the Free Ride
info page's START), `"free_ride_extreme"`, `"continue_game"` (CONTINUE), `"new_game"`. `LoadGame` fades to black and
after a short timer calls the C++ menu object: `game.MainMenu.OpenDebugLoadChapter2(["02_lost_heaven",
"lh_freeride_extreme.sav"])` for Free Ride, `game.MainMenu.ContinueGame()` for CONTINUE, `StartNewGame()` for a new
game. The co-op lobby calls `menu_parent.LoadGame("free_ride")` from our patched page (src/game/menu.cpp,
"poll"): both games went to the Free Ride loading screen (sailor Tommy) at once and, after Enter on each loading
screen ("Enter CONTINUE"; the game waits for it, the dll does not press it), both players were in Free Ride at the
Free Ride start (`GetLoadedSaveId()` = `freeroam_test`) and saw each other. While the loading screen waits for that
key, scripts run and `game.hud:IsLoadingScreenUp()` is true. Not tried: a guest whose Free Ride is still locked.

**Main menu live refresh.** The patched `BuildMenu` registers `this.QuitGameResponse` as an `enterFrame` listener
(Flash keeps one registration per listener, so every rebuild adds it once). `QuitGameResponse(answer:String)` is the
quit dialog's callback ("a" / "b"); called as a listener it gets the Event (coerced to a string) and asks C++
(`"poll"`, menuLocation) every frame: `refresh` -> `UpdateMenu()`, `free_ride` / `continue_game` ->
`menu_parent.LoadGame(...)`. Checked: the host's CO-OP page switched to LOBBY (1 PLAYER) when hosting started and to
LOBBY (2 PLAYERS) when the guest joined, without a key press. The main menu page is loaded and polls behind the title
screen too ("PRESS ENTER TO START"); it builds HOME there, and again whenever the window is resized (`--window`), so
HOME builds do not tell the title from the menu (a guest who joined with `--join` got the lobby over the title, and a
lobby START then loaded from behind the title and hung on a black screen). The game's scripts tick only from the
moment the title is passed (first `C_ScriptMachine::Tick`, never on the title), so "at the main menu" = a script tick
seen and a poll within the last second (`menu::AtMainMenu`, which `RequestLoad` needs), and "the menu is really on
screen" (`menu::MainMenuShown`, the lobby may open) also needs no requested load pending or started in the last 3 s.

**Quitting.** Closing the game window (WM_CLOSE, the X button) ends the process through `ExitProcess` on the game's
main thread right away (no dialog); the dll's hook leaves the co-op session first (core docs/protocol.md "Leaving
cleanly").

## 14. Story: chapters, checkpoints, objectives, cutscenes — verified 2026-10-15

Two instances on one PC, Omerta (`mm_110_omerta`, cp_010 and cp_020) and Taxi (`mm_20_taxi`, cp_010). Used by the
story sync (src/game/mission_sync.cpp + mission_sync.lua; messages in docs/protocol.md "Phase 4"; the loader in
docs/re-notes.md 6).

| call | notes |
|---|---|
| `GetActiveQuest()` (global) | the running chapter, e.g. `"mm_110_omerta"`; `""` in Free Ride and while the quest of a loaded save is not started |
| `game.director:QuestGetCustomState(q)` | the checkpoint: `"cp_020"`. `QuestGetCustomStatesCount(q)` / `QuestGetCustomStateByIndex(q, i)` list them (Omerta: cp_010 .. cp_120, 13) |
| `game.director:QuestIsAvailable(q)` / `QuestSetAvailable(q)` / `QuestSetCustomState(q, cp)` | a save loaded through the menu's chapter loader comes without its quest running (false); setting the checkpoint and making the quest available starts the chapter's flow there: objective, mission NPCs, cutscene objects. From the chapter's cp_010 save at cp_010 the intro cutscene played and the flow reached cp_020 on its own; a later checkpoint from the cp_010 save crashed (re-notes 6), so a guest only ever starts a later checkpoint from the host's save of it |
| `game.game:GetLoadedSaveId()` | `"checkpoint_mm_110_omerta_cp_020"`, `"checkpoint_mm_20_taxi_cp_010"`; `"!none!"` at the main menu and while a world is torn down after a quit |
| `game.savesystem:IsInReplayOrTestDrive()` | true after a load with the replay flags (MafiaCoop's loads) |
| globals `active_objective`, `override_objective`, `active_obj_description` | the objective shown: `active_objective` is its **id** (`"mm_110_omerta_cp_020"`); the HUD looks the text up from the id ("Talk to Vincenzo") |
| `HUD_AddQuestObjective(id)` (global, various_utils.lua) | shows objective `id` (sets `active_objective`, calls `game.hud:UpdateSimpleObjective`); with a quest id it shows the chapter name ("Omerta"). `HUD_AddQuestOverrideObjective(id)` / `HUD_RemoveQuestOverrideObjective()` for the override. The flows' own calls of these globals were not seen by a wrapper installed after the load (they hold the functions) |
| `game.hud:QuitToMainMenu()` | leaves the world for the main menu at once (no dialog, no save written). Called while a loading screen waits for Enter it hung the game on a black screen with the spinner (2026-10-16) |
| failed mission | leaving the mission area 250 m (Taxi cp_010, Omerta cp_020 reached by its own flow): in a chapter-loader load the game loads `freeroam_before_quest_activated` at once (no screen; re-notes 7); in a CONTINUE load it shows "YOU ABANDONED THE OBJECTIVE" with Load Last Checkpoint / Restart Chapter / Return to Main Menu (scripts paused). Omerta started at cp_020 by `QuestSetCustomState` (after a reload) did not fail even 900 m away, and the host appeared at -943 -232 (the Free Ride start spot) instead of the park: a chapter-loader load never puts the player where the save has him (re-notes 7); since 0.10.0 the story sync moves him there before the quest starts, and then the check works (Omerta cp_020 failed again from every reloaded checkpoint). A destroyed mission car (Taxi cp_010, the parked taxi blown up with `car:Explode()`) fails the same way (the same `freeroam_before_quest_activated` load, about 20 s later); with the player in it he dies instead. Timer failures exist (`mm_50_race_go_time_out`, a later drive of the Race chapter) but were not reached. `timer_helper.StartTimer(s, f)` / `SimpleFail()` (timer.lua) showed nothing in a co-op chapter |
| `game.director:GameOver("0059310003")` | (the id `timer.lua` uses) returned without error in Taxi cp_010 and showed nothing |
| `game.savesystem:FreeRoamRetry()` | (static, re-notes 7) the game's own answer to a failure or a death in a chapter-loader load: asks for the load of `freeroam_before_quest_activated` and returns true. The game-over controller calls the same C++ function (exe+0x21E8B20) when a checkpoint retry is not possible. MafiaCoop skips it in a co-op chapter (0.15, below) |
| `skipobjective()`, `savemission()` | debug globals that post events to a "temp_mission_flow" kit: nothing happens in this build |
| cutscene object `game.entitywrapper:GetSceneObjectByName("cine_1100_omerta_cs_object")` -> `GetComponent("C_RuntimeCutsceneComponent")` | exists only while the chapter's quest runs. `IsCutscenePlaybackInProgress()` true while it plays (also when the mission flow plays it); `LoadCutscene()` then (when `IsCutsceneLoadFinished()`) `PlayCutscene2("", "", "", "")` (**four** strings; three = "wrong number of arguments") plays it on demand; `StopCutscene()` ends it (the play sync object is then done), `UnloadCutscene()`. After a stop the screen stays black: `HUD_FaderFadeIn(0.5)` (global) fades back in. `GetCutsceneId()` -1 before a load, 50001 after |
| `game.gfx:GetCurrentWeatherSetName()` | after chapter loads it can name another chapter's set (`mm_160_harbor_cp_080_harbour_entrance_cutscene` in Omerta) |

**How the co-op story works** (src/game/mission_sync.*):
- Host: twice a second its chapter, checkpoint, objective and override objective (MissionState) and the cutscene
  that plays (polling the chapter's `cine_*_cs_object`s, docs/research-world-sync.md B5 list); every 2 s the newest
  `checkpoint_*` save in `Documents\My Games\Mafia Definitive Edition\Data\<id>\gamesaves` of the point it is at
  (HostSave, ~35-47 KB).
- Guest following the host (lobby STORY / CHAPTER, or JOIN GAME): at the main menu it loads the host's point through
  the menu's FREE RIDE load with the save bytes swapped (the host's save, or for a chapter start the shipped
  `edit\saves\00_mission_replay` save), then starts the quest at that checkpoint and is put behind the host's puppet.
  When the host is at another checkpoint for more than 8 s (`follow_grace`) and no cutscene plays, the guest
  removes the puppets, quits to the main menu and loads the host's save of that checkpoint (waiting for it when it is
  not there yet). Objectives: the host's ids through `HUD_AddQuestObjective`. Cutscenes: the guest plays its own copy
  of the host's cutscene object (or its own flow already does), stops it when the host's ends or is skipped, and is
  held (controls locked, "THE HOST IS WATCHING A CUTSCENE") if it skipped or cannot play it; puppets are hidden
  while a cutscene plays.
- **Failed missions (0.15, src/game/fail_flow.h; static, UNVERIFIED in game):** every failure kind and a death the
  game handles itself end in FreeRoamRetry in a co-op chapter. A chapter host or a following guest skips it: the
  screen fades out, he leaves for the main menu at once and loads the checkpoint (one load instead of the Free Ride
  stop plus the reload). The host's failure goes to the guests (FailVerdict HOST_FAILED: they leave at once and load
  his checkpoint once he is back in his world); a guest's failure goes to the host (MissionFail), who decides:
  REJOIN (he plays on: the guest loads his point) or WAIT (he fails or loads too: the guest follows his reload).
  A STORY host keeps his fail screen; his GameOver tells the guests. docs/protocol.md "Failed missions".
- Checked: lobby STORY from the Omerta cp_020 save (guest at cp_020 with the quest running and the objective);
  lobby CHAPTER 10: OMERTA (both at cp_010, intro cutscene on both, both flows reached cp_020 together, the host's
  cp_020 save went to the guest); lobby CHAPTER 2: TAXI (both in the chapter with objective markers); host cutscene
  played on the guest, host skip stopped it, guest skip held him until the host's ended; a late joiner (host already at Omerta cp_020) pressing JOIN GAME came in at cp_020 next to him;
  the guest reloading the host's checkpoint from the world (crashed 2 of 5 before the fix in re-notes 7; after it 9 of 9
  in a CHAPTER session and 11 of 11 in STORY sessions).

## 16. Remote players' animation and car controls (0.13.0) - static only, NOT verified in game

Found by reading the exe (docs/re-notes.md 9) while the game could not be started; everything here needs an
in-game check (README-dev.md "Test checklist: animation and car controls").

- Puppets on foot no longer get pulled every tick. The C++ side (core `net/remote_motion.h` LocomotionController)
  picks gait, speed multiplier and walk point; the glue (`M.Walk`) issues `SetMovementSpeedMult(m)` then
  `MoveVec(point, "WALK"|"RUN"|"SPRINT")`, nudges only the error beyond 0.9 m and snaps when far off. To check: that
  `SetMovementSpeedMult` changes the walk speed and its animation together (the controller measures the result:
  the debug status shows the calibrated walk/run/sprint speeds; if the multiplier does nothing they stay where they are
  and the puppet is a little slower or faster than its player, corrected by nudges) and that it does not stop a
  running walk.
- A remote driver's car copy gets his controls (`SetSteer` / `SetPower` / `SetBrake` / `SetHandbrake`) and is moved
  by its own physics with a blended correction. To check: the front wheels turn with his, brake lights show when he
  brakes, the copy does not wobble or fight; whether `SetPos` on a dynamic car keeps its velocity (if not, the
  controller pins the copy after a second, logged, and it behaves as before 0.13.0 with steering wheels).
- The puppet driver is an AI human in the driver seat: if the game gives it an AI driving controller, that may write
  the same controls; then the steering flickers between ours and its (not seen statically; `DestroyController` exists).
- **Car signals** (static, 2026-10-18): the driver's lights (two light groups, taken to be side lights and
  headlights), indicators, horn, police siren and beacon are read from his car (`mc_car_signals`) and set on the copy
  when they change (`RV.Signals`). To check: honk (the copy honks on the other game: the sound comes from
  `SetHornOn`), drive a police car and switch its siren on, drive at night (the copy's headlights follow his, also
  when the copy's own game would switch them differently: CR `sigre` counts how often it had to set them again), and
  read LR `sig` against CR `sig` in the two traces. If the light bits turn out to be something else, the copy shows
  the same bits anyway (they are copied as they are); only the names in the logs would be wrong.
- **First test (0.13.0, two PCs, 2026-10-17):** every car copy was pinned within ~4 s on both games (copy 0.2 m/s,
  target 4.4 m/s), with the driver's steering and throttle arriving. 0.14.0 starts the copy's engine, gives it its
  wanted speed after each correction (SetSpeed, probed), retries its physics after 5-40 s, and traces what the copy
  really has (controls, wheel angle, engine, gear) against the driver's car. On foot the calibrated sprint was only
  ~3.3 m/s (the AI's), so sprinting players got 1.3-3.2 m ahead: the shortfall is now carried.

## 15. Mission NPCs and cars on two games, the entity table - checked 2026-10-15

Two instances on one PC; chapters Taxi (`mm_20_taxi` cp_010/cp_020) and Bon Appetit (`mm_140_salieri` cp_020). Used by
src/game/npc_sync.* (messages in docs/protocol.md "NPCs") and the shared cars in player_sync.lua.

| call | notes |
|---|---|
| `mc_entity_ids(type)` (ours, src/game/lua.cpp) | every entity id of that type (low byte of the id: 8 human, 9 player, 10 car), space separated. The game keeps its entities in a table indexed by `id >> 8` (the lookup behind `GetEntityByGUID`, exe+0x233AC10, ignores the low byte); the scan stops after 65536 empty slots (~13 000 slots in a story world; after several reloads in one game new entities sat beyond a gap of more than 8192 slots, the old limit, and the scan found 2 of 168 cars). There is no "humans/cars in a radius" call for scripts; this is it |
| `mc_entity(id)` (ours) | the wrapper of entity `id` (the number `tostring(guid)` ends with), nil when that slot holds another entity now. Same steps as `game.entitywrapper:GetEntityByGUID`, which only accepts a `C_EntityGuid` object |
| `GetPlayersEnemies()` | the enemies the AI module lists for the player; police in sight showed up there outside any fight |
| blanket `ON_ACTIVATE` | never fired for humans (only `ON_DEACTIVATE` of passing pedestrians): no spawn feed |
| `h:GetName()` | `behavior_object_prime-NN-` for spawned humans: generated, **different on each game**; a car's name is its model name. Entity ids differ between the games too |
| positions | the same mission scripts at the same checkpoint put mission NPCs and parked mission cars at **the same spots on both games** (the Taxi depot's civilians matched to 0.0-0.3 m, the mission taxi to 0.5-1.1 m): the key for pairing (npc_sync: nearest within 1.5 m, then kept). Ambient pedestrians and traffic differ per game (31 of the host's 66 humans paired at the depot, 26 of 37 at Salieri's) |
| `h:SwitchBrain(t)` | changed a civilian to `AI_TYPE` 2 (enemy, it then shot at the player) or a puppet to 1 (friend: the host's enemies then shot the puppet), **but the game crashed seconds later** in a once-a-second update over a list of humans (exe+0x243A915, a stale entry; since 0.9.0 guarded, re-notes 8, and used for puppets), every time in Bon Appetit and in two of five Taxi tests. Not used by MafiaCoop; some civilians refuse it (type stays 0) |
| civilians and damage | some paired civilian copies on the guest took no damage at all (`DoDamage`, `health = 0`, bullets from the player, from a puppet or from themselves); their host counterparts died from the host's bullet. npc_sync hides such a copy when the host's dies. **Cause found 2026-10-16**: those were deactivated humans (`h:IsActive()` false; Bon Appetit cp_050: 3 of 16 humans on each game, phys state 2, `DoDamage(20)` left them at 100 on both games). They are no longer sent or paired (0.11.0) |
| `h:IsActive()` | false for humans the game keeps in the world switched off (not simulated, no damage, no movement). Cheap; npc_sync checks it on every send and pairing |
| `h.invulnerability` | readable (true/false) and writable on NPCs. Missions set it: in Bon Appetit cp_040/cp_050 ("Circle round and ambush attackers") the four attackers in front of Pepe's are `true` on each game from the start of cp_040 (their bullets keep the players pinned), only the waves that come round the back (kitchen, alley) are `false`. Teleporting the host behind them, into `cp_050_dummy_radius_1` or next to them did not clear it; the route the mission waits for was not found. Our shots and `DoDamage` do nothing to them (the "no damage on either game" of 0.10.0's test); `invulnerability = false` then `DoDamage` kills them |
| `h:GetBattleArchetype()` | a 64-bit hash (prints as a long decimal), the same for the same mission NPC on every game (Bon Appetit: the front attackers and waves 13908013266033645159, the kitchen/alley shotgun men 5086849525089173067, Salieri 10438535588825573133). npc_sync sends it hashed to a u32 as the pairing tag |
| scene objects by type | `mc_entity_ids(35)` / `(65)` list dummies and spawners with names (`dummy_cp_050_obj_search_for_salieri`, `cp_050_dummy_radius_1`, `dummy_cp_060_salieri_escape_car`): a way to find a checkpoint's places without the mission scripts (visual scripts, not readable from Lua) |
| mission waves per game | each game spawns a checkpoint's waves for its own player's progress: the host walked into the kitchen and his game spawned an enemy there that the guest's game (guest elsewhere) never did (unpaired: it shot the guest's puppet, those hits reached the guest). The waves that both games spawned paired at once |
| spawn profile of an NPC (0.12.0) | there is no getter on a human, and its profile hash is not stored in the human's own object or two pointer levels below it (searched at runtime with a known puppet profile). But a human that a mission spawner made is **named after it**: `behavior_object_prime-NN-<spawner name>` (Bon Appetit: `...-spawner_cp_010_salieri`, `...-machinegunner_Kitchen`, `...-triggerman_yard_00`); puppets are `...-behavior_object_prime-NN-` (the factory driver's name), traffic and police from the generators `behavior_object_prime-NN-` (empty), ambient pedestrians from area spawners (`civ_spawner`, `bartender_spawner`) are not found as scene objects. The spawners are entities of type 65 (`mc_entity_ids(65)`, ~200 in a story world, names repeat across areas): `GetComponent("C_RuntimeSpawnerComponent"):GetSpawnProfile()` (also `C_SpawnerComponent`) gives the profile as a decimal hash string, usable with `SetSpawnProfile`. npc_sync takes the nearest spawner of that name |
| profile hashes | FNV-1 64 of the lower-case path (`mc_fnv(text)` in the dll: `visuals/hero/game_main_tommy_cab_driver_flatcap` = 335218123840277515, `characters/scotch/game/main/game_main_paulie` = 16922695808871587265) |
| `ON_SHOOT` (HumanMessages 458755) | a blanket subscription gets one event per shot of every NPC (`GetReceiverGUID` = the shooter), a Tommy gun burst one per bullet: the host counts them per NPC for the guests' copies |
| enemy profile on the puppet factory | `SetSpawnProfile(<an enemy's profile>)` on the factory's runtime spawner (a civilian driver) makes a human with **AI type 2** (the profile's brain, not the factory's). Spawned alone 4-12 m from the player, armed, even firing its gun by script, it did not attack him in 15 s (three tries); as a copy in a running fight it took out its own pistol and some of its bullets hit the guest although it aims past him (8 hits in one 22-min run): those hits are given back on the guest (player_sync.lua `M.CopyUndoTick`) and the host's gun is put back every 2 s. No brain switch is used on copies |
| `CreateShots(puppet, ...)` at an NPC | a guest's bullet replayed on the host from his puppet: the NPC took it (its health and death on the host), its DAMAGE event names the puppet |
| `game.traffic:FindNearestSpawnedVehicle(slot, pos, radius)` -> sync object, then `GetNearestFoundSpawnedEntity(slot)` | the nearest *traffic* car only (a culver 40 m away, not the mission taxi next to the player) |
| `game.traffic:GetLastPlayerCar()` / `GetLastPlayerVehicle()` / `GetLastObtainedPlayerCar()` | nil at Taxi cp_010 (the mission taxi is not "the player's car" there) |
| `FIRE` damage (type 8) | an enemy with the triggerman archetype threw fire; burning humans then took 0.1 damage per frame. A burning puppet is not released by MafiaCoop until 8 s after the fire (the fire update keeps a list of burning humans) |
| map markers | `RegisterHumanEntityDirect` markers must be unregistered **before** the human is released (MafiaCoop did it a tick later) |

**Fights in 0.11.0 (checked in Bon Appetit cp_040 and cp_050, two instances).** A guest's copy is an ordinary NPC
on his game: its health follows the host's NPC down, his own bullets hurt and kill it, and the host's NPC dies with it
(the replayed bullet from his puppet when it lands, else the lethal NpcHit; 10 kills, 5 each way). The host's
`DoDamage(30)` on an enemy took it 100 -> 64 and the guest's copy followed to 64 within a state round. With the host
not shooting, the guest cleared both cp_050 waves alone (4 enemies each run). After a guest reload mid-fight the
moved-on enemies paired again by archetype at 5-8 m ("same kind, moved apart"), after the host's death reload the
guest reloaded himself and all 6 enemies paired. `CreateShots` from the player at an enemy behind a wall does not hit
(no DAMAGE); the tests teleport the player 3 m behind the target first.

**Enemies and guests (0.9.0).** What decides whether an enemy shoots at a human is its brain, not the AI type its
spawn profile reports: a puppet spawned from an `ai_ally_` profile reads `GetAIType()` 1 but enemies ignored it,
even with `enemy:Attack(puppet)`; a car crew spawned with an enemy profile (`ObtainCarWithCrew(model,
{"characters/ai/morello/ai_enemy_morello_enforcer", ...})`) is AI type 2 and hostile from the start, and a crew with
an `ai_ally_` profile is not spawned at all (empty seats). After `SwitchBrain(1)` the host's enemies shot the guest's
puppet (four and three NPC bullets of 10 in two runs, Brothel cp_015 in Salieri's yard, the host out of sight inside
the bar). The switch itself is safe; freeing a living friend-brain human crashed the game (re-notes 8), which the
dll's guard now prevents. Fights in these tests were set up with such enemy crews spawned on both games at the same
spot (they paired like mission NPCs); the chapters' own shootouts were not reached by the automated tests (they all
begin with a drive or a walk the tests cannot follow).

**How it plays** (verified unless noted):
- Pairing: on a guest the host's NPCs near either player are paired with his own (logs "host NPC k is our l (0.01 m
  apart)"); host NPCs that died on the host died on the guest at once (Taxi: a civilian shot by a hostile NPC on the
  host, a hostile NPC); one civilian copy took no damage and stayed alive (since then it is hidden after 3 tries, not
  checked in game); a guest's shots on his copy of a hostile NPC went to the host,
  were replayed from his puppet there, the host's NPC died and the guest's copy with it (Taxi, twice).
- Enemies and guests: on each game the enemies fight that game's own player (each game runs the mission's AI); since
  0.9.0 the host's enemies also target the guests' puppets (friend brain, above) and their hits reach the guest: in
  the first run he went 100 -> 74 -> 39 -> 13 health from three relayed bullets, was downed and respawned next to the
  host; with his own hostile copies alive the relayed hits are skipped ("our copy fights us here"). Pairing in that
  fight: 29-35 of the host's 48-53 NPCs; both host NPC deaths repeated on the guest at once. A guest's kill of his copy
  was not re-checked here (the guest's own spawned copies were despawned by his game); it was checked in Taxi before.
  Not checked in a chapter's own scripted shootout.
- Mission cars: the host got into the Taxi chapter's taxi; the guest's game took its own taxi standing there as the
  host's car ("our own shubert_six_taxi stands there (0.5 m): it is this car here") instead of spawning a copy on top
  of it. Before, the copy shoved the guest's taxi, his mission failed and the game loaded the free roam state from
  before the quest. When both stood in their own taxis (the guest after loading the host's cp_020 save), the host's game
  spawned no copy into its own taxi ("another car stands where it is") until the guest's car had moved off. The NPC
  driver puppet is thrown out of a mission taxi repeatedly (the car does not keep an NPC at its wheel), so the host
  was seen next to his car, not in it, on the guest's game. Since 0.9.0 the guest's mission taxi is swapped for a copy
  after the second throw-out and the puppet drives that (Taxi cp_010: thrown out once, swapped, seated, stayed at the
  wheel while the host drove, `B4-taxi-swap-guest.png`; after the guest followed to cp_020 the host's taxi is a copy
  and the puppet stayed seated there too). The mission car coming back when the host parks was not checked (the
  Taxi mission does not let the player out). In chapters that start with each player in his own mission car (Lost
  Heaven chase, Taxi cp_020) the guest sits in his own car where the host's would be: the host's puppet waits hidden
  until the cars have parted, as before.

**Mission car back (0.15; built from the code paths above, NOT checked in game).** The swap keeps the real car
(`c.orig` in player_sync.lua): this game's mission scripts reference that entity, not the copy. Since 0.15 every
real car is kept this way, not only an adopted mission car: the host's own mission car, swapped on the host's game
when a guest drove it, used to stay hidden for good. When it comes back is decided by a small machine in C++
(`src/game/car_swap.h`, unit-tested in `tests/car_swap_test.cpp`; Lua `mc_carswap(key, now, bits, speed, parkedAt)`
-> action, lock, phase, reason), fed every tick by `M.SwapTick` with what the game shows:

| situation | what happens |
|---|---|
| somebody drives the copy (the remote player, or we ourselves) | nothing; the copy is the car |
| nobody drives it (the driver got out) | the copy is locked for the local player (`LockPlayerEntryPoints`; he would get into the copy, not his mission's car) until ours is back |
| the copy is empty (no puppet, not us, no remote player getting in/out or riding along; NPCs alone are waited for 8 s) and the last driver's **CarParked** arrived (the copy is put at that final transform first) | ours comes back at the copy's place: the copy is despawned, ours `SetPos`/`SetRot` (before and after `Activate`, since it is not known whether a deactivated car takes a teleport), `Activate()`; the driver's puppet climbs out of the copy before that (the restore waits for it) and stands beside the car |
| no CarParked (lost, or the driver left the session) | ours comes back once the copy has stood still (< 0.5 m/s) for 1 s, 10 s after the driver got out; after 20 s at the latest |
| a CarParked up to 3 s before the driver's state shows him out | counts (CarParked is reliable, the state is not: either may come first); an older one does not |
| the driver gets back in before ours is back | the copy is driven again, nothing restored |
| a remote player drives ours after it came back | swapped for a copy at once (`c.noNpc`: it threw his puppet out before) instead of after two throw-outs |
| we drive the copy ourselves (we rode along, the lock did not hold) | it is our car until we park it; our own CarParked counts like his, and ours comes back where we left the copy once we are out |
| CarParked says the car was destroyed (new flags byte, docs/protocol.md) | ours comes back there and is blown up (`car:Explode()`) so this game's mission sees what the driver's saw. The driver's game takes a car as destroyed when `car:GetDamage()` is 100 or more: **its scale is not known** (the raw `GetDamage` / `GetMotorDamage` are logged with every CarParked to calibrate it); a 0..1 scale never says destroyed, and then the mission's own failure reload still brings everyone back |
| the copy could not be spawned, or the game removed it | ours comes back at once at the driver's last transform (no further swaps of that car if spawning failed) |
| a new world (checkpoint load, reload after a death or a failed mission, following the host, quit) | ours, if the entity survived the load (same GUID), is shown where it is (`Activate`), else forgotten; the old world's copy is not touched (its traffic id may belong to a new car). A copy that is still valid means it was only a loading screen in the same world: nothing happens |
| the session ends | ours comes back at the copy's place at once; if we sit in the copy it stays (a normal car) and ours comes back when we got out (`M.leftCars`) |
| a car record is dropped (model changed, trimmed) | ours comes back first; copies with ours hidden behind them are never trimmed |

Log lines: `car K: replaced by a fresh copy for the new driver (why), ours (id at x y z) hidden meanwhile`,
`swapped car K: parking, a puppet is still in the copy (...)` (every change of the machine's state, from the dll),
`car K: our own car id is back at the copy's place (x y z; hidden at ...), activate ok, active true, copy despawned:
parked`. Sync trace: `SWP` (machine) and `SWR` (swap / restore / forget), docs/sync-trace.md.

Unverified: `Activate()` on a deactivated car (and a teleport of it) showing it again where we put it, with its
mission state; `DespawnEntityImmediately` of a copy right after its occupants got out; `GetDamage`'s scale;
whether a deactivated car survives a checkpoint load deactivated (it is shown again then); a checkpoint **save**
made while a car is hidden (the host's own car while a guest drives it, or a guest's while the host does): the save
may store it deactivated; mission NPCs that board the hidden car meanwhile (Taxi customers) were not looked at.

## 17. Remote players' punches and other actions (melee sync) - static only, NOT verified in game

Found by reading the exe, its Lua bindings and the game's animation and script archives while the game could not be
started (no game launch for this work). Used by src/game/player_sync.lua "Visible actions" and the action block of
PlayerState (docs/protocol.md). Everything below needs the in-game check.

**Detecting our player's actions.** The swing messages of section 11 are sent to the attacker himself; the same
`HumanMessages` table (enums dump, `C_WrapE_FightHint` at 0x5AEDF10 in re3/enums.txt) has the other melee moves and a
throw, and the glue subscribes to each with one blanket HUMAN subscription, keeping only those whose receiver is our
player:

| message | value | action | checked |
|---|---|---|---|
| `MELEE_LIGHT_ATTACK` | 458809 | light punch (Q tapped) | yes (section 11) |
| `MELEE_HEAVY_ATTACK` | 458810 | charged punch (Q held) | yes (section 11) |
| `MELEE_COUNTER_ATTACK` | 458811 | counter (Left Alt) | no: assumed sent to the attacker like the two above; ignored when its opponent is our player |
| `MELEE_COMBAT_TAKEDOWN` / `MELEE_STEALTH_TAKEDOWN` | 458812 / 458813 | takedown | no (same assumption) |
| `WEAPON_THROWN` | 458830 | grenade / molotov / knife thrown | no: who receives it is not known |
| `MELEE_HIT_REACT` | 458814 | an NPC punched our player | no: seen on NPC victims only (section 11) |

There is no getter for "blocking": the player's defence in this game is the counter (Left Alt), so a block shows as a
counter. Aiming (`flags` bit3), reloading (bit4), crouch and cover were already carried and shown (section 10).

**Playing it on a puppet.** None of the script orders makes an AI human punch: `AttackMelee(target)` does nothing on
a puppet (section 11), and `npc:SwitchBrain(2)` + `EnableCloseCombat(true)` + `AttackMelee` crashed the game.
`DbgTesting_HumanCombatAction(n, 0)` is no melee either: its enum (`DbgTesting_HumanCombatActions`, 0x5AD8210) is
NONE, LOOKAROUND_1..3, EVADE_1..3. What the glue uses instead is a plain animation:

| call | notes |
|---|---|
| `h:AnimPlay(name, loop)` | the call of the game's `Human.Anim.Play` node (`common_human.lua`: `inEntity:AnimPlay(animation, loop)`, result kept as `anim_so`, then `Wait`). Binding `C_WrapperHuman::AnimPlay` (exe 0x23B1DA0) dispatches on the argument count: 2 (string, bool) -> 0x23D3000, also 5 and 6 arguments (other overloads, not used). The native call (0x2BA1F00) builds a 0x1B8-byte AI command and queues it on the human's AI (`+0x138`; for a human without one, a dead one or the player in some states it returns a sync object without queuing anything), like `MoveVec`: so a later walk, turn, aim or weapon command can replace it. Returns a sync object (`IsDone()`). UNVERIFIED in game |
| `h:AnimStop()` | the `Human.Anim.Stop` node's call; used when the time for an action is up and the animation still runs |
| `h:DesirePlayAnimation(name, desirability)` / `PlayAnimation(...)` | the other script ways (`Human.DesirePlayAnimation` node: inAnimation, inDesirability; `PlayAnimation` takes docking object, bone, loop). Not used; next to try if `AnimPlay` does nothing on a puppet |

Animation names: from the game's own archives, `sds_retail/basic_anim/hitmelee_anim.sds` (melee attacks, hit
reactions) and `basic_anim.sds` (throws, door kicks, the 1h punches), unpacked with re2/sds.py (Oodle). The scripted
nodes use names from `basic_anim` the same way (`hero_nw_calm_idle_fidget_cracknuckles` in the Omerta park script),
so names from these sets should be playable; whether the melee set is loaded outside a fight is not known. The glue
tries, per kind (first accepted wins; a name `AnimPlay` rejects, or whose animation is over within 0.1 s, is dropped
after two tries, logged):

| kind | names (in order) | time left alone |
|---|---|---|
| light punch | combo step 1/2/3 when punches come less than 1.2 s apart: `hero_nw_alert_melee_attack_light_<1/2/3>_v1`, `hero_nw_alert_melee_punch_<1/2/3>`, `hero_nw_alert_melee_punch1_v1` / `punch2_v5` / `punch_3_v2`; with a gun in hand `hero_1h_alert_melee_punch_<1/2/3>` first | 0.7 s |
| heavy punch | `hero_nw_alert_melee_attack_heavy_v2_01_release_hero`, `hero_nw_alert_melee_charge_punch3_v6`, `hero_nw_alert_melee_punch3_ko_l_v2` (armed: `hero_1h_alert_melee_attack_heavy_charge`, `hero_1h_alert_melee_punch2`) | 1.0 s |
| counter | `hero_nw_alert_melee_dodge_down_l_hero`, `hero_nw_alert_melee_punch_2_v2`, `hero_nw_alert_melee_punch2_v5` | 0.9 s |
| takedown | `hero_nw_alert_melee_punch3_ko_l_v2`, `hero_nw_alert_melee_attack_kick`, the heavy release | 1.4 s |
| throw | `hero_1h_alert_grenade_throw_cf`, `trig_1h_idle_to_grenade_throw`, `hero_nw_alert_throw_knife_f` | 1.1 s |
| hit reaction | `hero_nw_alert_melee_hitreact2_v1`, `hero_nw_alert_melee_hitreact2_v2`, `enemy_nw_alert_melee_hitreact1_v1` | 0.6 s |

Other names in those sets for later: `hero_nw_alert_melee_attack_kick`, `hero_nw_alert_kick`, `hero_1h_alert_doorkick`,
`hero_nw_idle_block_1` / `_enter_1` / `_exit_1`, `hero_nw_alert_melee_dodge_*`, `hero_2h_alert_melee_punch1` (rifle),
`hero_knife_alert_melee_attack_light_1..3`, `hero_bat_alert_melee_attack_light_*`, `hero_1h_alert_grenade_throw_cl/cr`.
The lists are a table in the glue, `MafiaCoop.actionAnims[kind]` (`dur`, `list`, `armed`, `combo`), and can be
changed from the dev console to try others.

While a puppet plays one: it is turned to its player's heading first and its aim dropped; nothing else is asked of it
(no walk, turn, weapon or aim command, they would replace the animation) and its `PS` report says busy, so the C++
locomotion controller treats it as "no copy" (no re-aims, nudges or snaps, its standing still is not measured as a
walk speed; the calibration is kept) and walks it again right after. It ends when the sync object reports done or
the time above is up (then `AnimStop()`).

**No damage from the picture.** The animation is not an attack order, so it should not hit anyone; to be sure, a
puppet swinging within 3 m makes the local player a demigod for the swing (+0.3 s; `SetDemigod`, which also stops
DAMAGE events, section 5), the Hit of a real punch still applies (`M.HurtScript` switches the demigod off around its
`DoDamage`), and a melee DAMAGE with a puppet as enemy is given back (logged "swing hit us here"). The demigod moment
also blocks other damage for that time (an enemy NPC's bullet during a puppet's swing next to us).

Not carried yet (seen while looking, not cheap with this mechanism): door kicks (`DOOR_KICK` 458857, the door's state
is not synced either), the explosion of a thrown grenade or molotov on the other games (the puppet only shows the
throw; `game.game:ThrowProjectileWeapon` exists but would hurt people on the receiver's game), medkit use
(`PLAYER_USED_MEDKIT`), carrying bodies (`TakeBody` / `DropBody`), lockpicking, vaults and ladders (`USE_FENCE`,
`USE_LADDER`; the puppet's walk takes its own way over them or snaps), jumping (only the in-air flag, the puppet does
not jump), stalk (sneak) walking beyond crouch, and takedowns with their paired victim animation.

## 18. Puppets thrown out of car seats: the civilian brain (0.15) - static + 0.13.0 logs, the fix NOT verified in game

**What the logs show** (first two-PC test of 0.13.0, 2026-10-17, Free Ride, ~5 min; host `mafiacoop-adam.log`,
guest `mafiacoop.log`):

| | host's game: the guest's puppet | guest's game: the host's puppet |
|---|---|---|
| brain | switched to FRIEND 2 s after it spawned (`friend brain set (AI type 1)`, M.Ally, host only then) | the factory's civilian brain (never switched on guests) |
| at the wheel of a fresh copy, car under its own physics, then pinned | seated at once, `seat0` in every status line for 20 s of own physics and 2 min pinned; two more copies (one a swap) the same | `seated` at 20.39, 21.39, 22.40 and `keeps getting thrown out` at 22.50 (thrown out 0.1-1 s after each GetOnVehicle); the same 41.6-43.7 for the second copy; it stayed only after the copy was pinned and the slow (4 s) retries came round |
| passenger seat of a car the local player drives | - (not tried) | thrown out of seat 1 of the guest's own car again and again while he drove (hidden / seated alternating for 70 s) |

Same game build, same factory, same seating code (GetOnVehicle(car, seat + 1, false, "WALK")), same copies; the
only difference is the brain. Earlier findings fit: the mission taxi and "a car the local player has driven" threw
out civilian-brain puppets (section 7, 15); the one civilian that stayed was in 0.10.0, at the wheel of the guest's
own Salieri car (adopted, moved pinned, as every copy was before 0.13.0) with Salieri beside it.

**Why (static, from the exe's AI class names):** the brain is a set of desires (`C_Desire*`, `/config/ai/brains`,
`brain_friend` / `brain_enemy` / `brain_police`; `SwitchBrain(1)` = friend). The civilian set includes
`C_DesireUseCar`, `C_DesirePanicInCar`, `C_DesireCivilPlayerReaction`, `C_DesireReactionToPlayer`,
`C_DesireCivilHornReaction`, `C_DesireAvoidingPlayerCollision`, `C_DesireWander`, `C_DesireThrownPassengerReaction`,
`C_DesireReactionToThrownFromCar` - a civilian decides for itself whether to stay in a car, and it leaves one the
player drives, drove or that moves without it driving (our controls and corrections, bumps). The friend set is built
around riding with the player (`C_DesireFriend`, `C_DesireFollowCar`, `C_DesireFollowGetInCar`,
`C_DesireFollowGetOutCar`, `C_DesireFollowOnFoot`); the follow desires need a leader set by script (`Follow`), which
puppets never get. Which civilian desire fires exactly was not traced (UNVERIFIED); the fix does not depend on it.

**What MafiaCoop does since 0.15** (player_sync.lua M.Seat, M.Ally, M.RideAlong):

- Every puppet gets the friend brain on **every** game where the game-sectors guard is in (the C++ side's `G` line,
  last field = `sectors_guard::Active()`; re-notes 8: a freed friend-brain human crashed the game without it), not only
  the guests' puppets on the host. It is switched 2 s after the spawn whenever the puppet is not in a car (hidden
  included), and a puppet about to be seated waits for it (0.3 s after the switch). Side effects: the guest's own
  enemies now target the host's puppet too (their hits on a puppet are ignored on a guest, as before); a friend-brain
  puppet cannot be punched by the game's melee targeting (AI type 1, section 11: the swing fallback covers it, as on
  the host since 0.9.0). With `enemies_target_guests = 0` the host still switches the brain (for the seats) but does
  not send its enemies' hits to the guest.
- The seat is checked every tick (`car:GetSeatOwner(seat)` against the puppet, not only `GetOwner()`): a puppet
  still in the car but not in its seat (the AI starting to leave, or another seat) is put back with a non-animated
  GetOnVehicle at once (every 0.5 s while it lasts; if the seat still reads wrong after 2.5 s the check is switched
  off for that seat and GetOwner is trusted, as before). Whether an AI exit gives up the seat before GetOwner changes
  is **UNVERIFIED** (logged: `puppet N is in car K but not in seat S (... leaving ...): put back at once`).
- A puppet that did get out is put back on the next tick (0.2 s after the last command) for the first three
  ejections of a seat, then once a second, then every 4 s hidden in between (the old pace).
- Without the guard (another game build) the civilian is kept calm for its seat instead: `SetReactionOnSound(false)`
  and `InterruptCivilWander()` (what keeps the factory driver in his car).
- Ride along (E next to a car another player drives): the game slides a player who gets in beside an empty driver
  seat over to the wheel (0.13.0: the guest got in while the host's puppet had just been thrown out and sat at the
  wheel of the host's car: "we took the wheel while player 0 drove it", the host's game then saw two drivers). The
  driver's puppet is now put at the wheel first (`ride along: put player N's puppet at the wheel ... first`); if it
  cannot be, there is no ride (logged, press E again). If the game still seats us at the wheel, `p:SlideToSeat(seat
  + 1)` is tried (below) and the seat we report stays "getting in" (9) until the ride is done, so nobody's car is
  taken over; still at the wheel after 2 s, we drive it (as before).
- Three or four players in one car (0.15 multi-guest, NOT checked in game): the ride-along seat is the first
  passenger seat that is free here and that no other player's state names (`mc_rideseat(seatCount, freeMask,
  "id:seat ...")`, src/game/seats.h); a passenger seat that two players took at once is kept by the lower player id,
  the other slides over (`mc_yieldseat(localId, seat, seatCount, freeMask, claims)` -> seat, -1 stay, -2 none;
  `SlideToSeat(seat + 1)`, UNVERIFIED like below). Logged as `ride along: seat S of car K (... the others' seats:
  1:1 3:2)` and `seats: ... we slide to seat S`; trace `SEA`.

**Bindings looked at for this (static, exe):**

| call | notes |
|---|---|
| `h:SlideToSeat(n)` | **UNVERIFIED**: one number; the binding (exe+0x23CCF90) calls the human's AI (`[human + 0x130]`, exe+0x2BA48C0), which returns a failed sync object for `n <= 0`, so `n` is 1-based like GetOnVehicle's seat. Used once when a ride along put the player at the wheel |
| `h:PlayerShouldRemainPassenger(bool)` | player only: exe+0x2B99140 does nothing unless the entity type is 9 (player); writes a flag at `+0xB0` of a player-side object. Not for puppets |
| `h:DisableAutoExitFromExplodedCar(bool)` | exists (exe+0x238FE30 -> exe+0x23E6E00); not used (copies have `DisableExplosion`) |
| `h:SetHumanEscapeEnabled(bool)` | a byte at `+4` of an AI sub-object (exe+0x2BA4210); meaning unknown, not used |
| `h:SwitchBrain(n)` | exe+0x2391B80 -> exe+0x2BA5820: skipped for the player (type 9) and for humans with the byte `+0x1F9` set |
| `GetOnVehicle` 6-argument form | the 5th script argument (a bool) is passed through unchanged (exe+0x2B97A40); meaning unknown |

**Logs and trace:** every throw-out is a `puppet N thrown out of car K seat S (#n, m this session): ...` line (time
since seated, car speed, fix / pinned, phys state, who holds the seat now, friend brain yes/no, put-backs, distance
from the car, who drives the car, adopted / we drove it); `puppet N sits in car K seat S (... friend brain ...)` when
a seat holds; `puppet N: seat S of car K is taken here by ...` once per seat. The `PR` record has `sown` (who holds
the puppet's seat: ours / free / other / ?), `brain` (friend brain set), `ai` (GetAIType), `back` (put-backs),
`leaving` and `ejAll` (docs/sync-trace.md); `tools/sync_compare.py` lists the throw-outs, the brain / put-back lines
and the ride-along lines per player.

## 19. Traffic and police on two games (B4) - static only, NOT verified in game

Used by src/game/traffic_sync.* (messages in docs/protocol.md "Traffic"). Nothing here has been run in the game yet;
each call is wrapped in `pcall` and its failure logged (`traffic: ... failed`).

| call | notes |
|---|---|
| traffic car = generator driver | a car whose seat 0 holds a human named `behavior_object_prime-NN-` (empty suffix, section 15) is moving traffic or police from the generators; the host shares those. **UNVERIFIED** that every generator car has such a driver and that no mission car does |
| police | the driver's `GetAIType()` is 4 (POLICE), or the model name contains `police`. **UNVERIFIED** for police cars of the generators |
| `car:GetColor(1)`, `car:GetColor(2)` / `car:SetColor(c1, c2)` | the copy gets the host car's two colour numbers (only when both read). **UNVERIFIED**: slot meaning (research-world-sync B4-3) |
| `car:GetSPZText()` / `car:SetSPZText(text, true)` | the plate, copied (letters, digits, `-`, `_` only). **UNVERIFIED** |
| `game.traffic:SetEnableAmbientTrafficSpawning(false/true)` | guest near the host: his ambient traffic spawning off, back on when he leaves (and when the session ends). **UNVERIFIED**: whether it removes the cars already there (the glue hides them itself, below) and whether it touches mission traffic (it is the narrower switch; `SwitchGenerators` is not used) |
| `game.police:PreventPoliceSpawning(true/false)` | the same moments: no police cars of the guest's own while the host's are shown. **UNVERIFIED**; a mission that sets it itself is overridden only on those switches |
| `car:Deactivate()` / `Activate()` on a traffic car and its crew | the guest's own moving traffic cars farther than 70 m are hidden this way while the host's traffic shows, shown again afterwards. **UNVERIFIED** for crewed traffic cars (verified only for an empty mission car, section 7) |
| `ObtainCarWithCrew(model, {profile})` for a copy | crew profile: the first profile path found in `data_traffic.crew_presets.civilian_white_crew` (police: `police_lvl1`), else `visuals/hero/game_main_tommy_gangster_trenchcoat`. **UNVERIFIED** layout of the presets (the log names the profile used); `ObtainSpecificCar(model)` (no crew) when it is refused |
| crew `LockControls(true)` | so the copy's own AI driver does not steer against the host car's controls. **UNVERIFIED** that it stops a traffic AI (if not, the VehicleController pins the copy, as for player car copies) |
| `car:SetSirenOn(b)`, `car:SetBeaconLightOn(b)` | police copies, from the host's siren flag. There is no siren getter, so the host's flag is a guess: on while a police car goes faster than 14 m/s and 8 s after |
| `car:DisableExplosion(true)` on copies | an explosion on one game only would leave the host's car driving on |
| `h:GetOffVehicle()` | the copy's crew when a player takes the car (handed over to player_sync), the host's traffic driver when a guest took the car, the copy's driver when the host's car lost its driver. Does nothing while the car is moved by `SetPos` every tick (section 7) |
| `game.traffic:DespawnEntity(id)` / `DespawnEntityImmediately(id)` | copies within 80 m of the guest go when unseen, others at once |
