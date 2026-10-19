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
| player melee (key **Q**: tap = light punch, hold about 1 s = charged punch; Counter = Left Alt; the default keys also include Stalk = X, Cover = Left Ctrl, Interact = E) | **Only outside safehouse zones** (below). The swing sends `MELEE_LIGHT_ATTACK` (458809) or `MELEE_HEAVY_ATTACK` (458810) to the attacker himself, `GetOpponentGUID()` = the human the game aimed it at (0 = none). If that is an NPC, about 0.2 s later the NPC gets DAMAGE type 3 (MELEE) with the attacker as enemy: 10 for a light, 36 for a charged punch (17 and 61 health off a puppet), plus MELEE_HIT_REACT (458814) and MELEE_END (458816). Six punches in a row on the same puppet all landed (no guard). **The game only targets humans with AI type 0** (`h:GetAIType()`): spawn profiles `characters/scotch/game/main/game_main_*` give 0; `characters/scotch/ai/ai_ally_*`, `visuals/hero/*`, `characters/game/hero/*` give 1, and a swing at those always has opponent 0 and hurts nobody, even standing right in front of it. `SetCivilArchetype`/`SetBattleArchetype` (decimal hash strings accepted) change the archetypes but not the AI type. The co-op glue (`M.MeleeTick`) therefore sends a Hit for a swing with opponent 0 when a puppet stands within 1.9 m in front of the player at the impact (0.25 s), and gives that puppet a `DoDamage(1)` flinch. Friend-brain puppets (AI type 1) get no DAMAGE from our bullets either; unreleased lock-on and shot hit test: section 17 "Shooting and punching a puppet" |
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
| puppet alive, shown, on foot | the human icon on the puppet (`RegisterHumanEntityDirect(puppet, 1, 1)`, verified above), it follows the entity; checked once a second (`GetIdByScriptWrapper`) |
| puppet alive, shown, on foot, its friend brain switch still to come (first ~2 s, `M.Ally`) | at his position (the switch would remove an icon on the puppet, see below) |
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

**The friend brain switch removes the human icon (0.15.0 bug: the host's marker missing on the guest's minimap).**
In the 0.15.0 one-PC test the guest's logs said `map marker 23 made on his puppet` (and later 434, 484) but no orange
marker was on the guest's minimap with the host 12 m / 40 m away; the host's markers of the guest were fine. Every
guest run, and only on the guest, `could not move it off his puppet` followed as soon as the host got into a car:
`ChangeIconTargetToPos` returned false, which it does **only when the id is not in the module's id map** (0x2F40CE0 ->
0x2FA3470 -> id lookup 0x2FB2000, false at once when not found). So the icon was gone soon after it was made. The
cause, statically: `h:SwitchBrain(n)` (C_HumanScriptW, 0x2391B80) -> 0x2BA5820 -> 0x2B81D70 (the brain switch) ->
0x2B820D0, which, when the old brain holds a navigation id (brain +0x11C not the "none" value), calls 0x3060200: look
the human up in the module's entity map (+0x170, by the entity's GUID at +0xA8) and unregister the human icon on it
with 0x30602B0, the routine `UnregisterId` uses (0x2F5A500). It then registers a new icon only for entity type 9
(0x2B7FE10), not for our puppets (type 8). Our `M.Ally` switches every puppet to the friend brain 2 s after it spawns
(0.15: on the guests' games too), and in every 0.15.0 log the human icon had been made 0.04-2 s **before** that
switch. On the guest the host's puppet is a Tommy profile (AI type 1 at spawn: an ally brain, which holds a navigation id:
inferred from the logs, where that id is set was not found statically); the host's copy of a guest (Paulie, AI type 0, a civilian brain) kept its icon through the same
switch (its `ChangeIconTargetToPos` worked). The one guest icon made 44 ms **after** the switch (run 3, icon 60) was
not removed until the puppet went. Before 0.15 guests never switched brains, which is why the 0.5.0 check saw the
host's marker on the guest's map.

The fix (static, UNVERIFIED in game): a puppet whose switch is still to come on this game (`M.WantsBrain()` and not
yet `pp.ally`) carries no icon: his icon stays at his position (`Input::brainPending` in map_markers.h; a human icon
already on it is moved off first) and goes onto the puppet once the switch is done. And an icon on a puppet is
checked once a second, starting 1 s after it is put there, with `GetIdByScriptWrapper(puppet)`:

| call | arguments (static) | notes |
|---|---|---|
| `GetIdByScriptWrapper(entity)` | entity -> number | read only (0x2F5B0D0 -> 0x2FC6AE0 -> entity map lookup 0x2FC4160): the navigation id of the icon on that entity, **0 when it has none**. Our check: it must give our id (`mc_mapmark_probe`, `mapmark::CheckProbe`) |

A lost icon (0 or another id) is dropped (an `UnregisterId` of the old id just in case) and made anew at the next update;
after `kMaxLosses` (2) on the same puppet he keeps an icon at his position instead. An icon found gone while being
moved off a puppet (`ChangeIconTargetToPos` false) is now `LOST ... made anew` and does not mark the puppet as
"burnt". Static reading also says `UnregisterId` (0x30602B0) erases the icon from both maps (id map and the entity
link, 0x305FBC0 + 0x3080D10), so a later `RegisterHumanEntityDirect` on the same human should make a fresh icon: the
0.4 "never a second icon after UnregisterId" observation may have had another cause; `pp.mkBurnt` is kept for now
(UNVERIFIED either way). Logs to look for: `map marker waits at his position until his puppet's friend brain switch
is done`, `<id> made at his position (..., his puppet's brain switch first)`, `(position icon) removed: he has a puppet
now`, `<id> made on his puppet (brain switch done)`, `<id> checked on his puppet: the game has it (human icon)`; a
problem shows as `<id> LOST: the game has no icon on his puppet (...)`. Trace `MK` ids carry the check result
(`0:434/human/puppet/ok`) and `/brainwait`; `MKE ev=lost/give_up`.

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
| `ChangeIconTargetToPos(id, vector)` | number, C_Vector -> bool | moves an icon (any kind) to a position; unlinks it from its entity (0x305FBC0) first. The game's objective scripts switch icons between entities and positions with this pair. Returns false only when the id is unknown (0x2FA3470 -> 0x2FB2000): the icon no longer exists |
| `GetIdByScriptWrapper(entity)` | entity -> number | the id of the icon on the entity, 0 when none (read only; see "friend brain switch" above) |
| `ChangeIconTargetToEntityDirect(id, entity)` | number, entity -> bool | puts an icon on an entity (false when the entity is not usable or the id unknown) |
| `SetIconTextByNavId(id, text)` | number, string | the game sets e.g. `"%\nDEFENSE"` texts with it; where the full map shows it (hover popup?) is UNVERIFIED |
| `SetIconFarVisibility(id, bool)` | number, boolean | the flow node `Game:Hud` icon nodes feed it from their `inRadarEdge` input: most likely "stay on the minimap's edge when out of range" (UNVERIFIED) |
| `SetIconMapVisibilityByNavId(id, bool)` / `SetIconRadarVisibilityByNavId(id, bool)` | number, boolean | map / minimap visibility of one icon (not used) |
| `SetIndicatorHideRange(id, m)` | number, number | (not used) |
| `SetIconFakeGPSPositionOnEntity(id, vector)`, `SetIconOverrideIndicatorPosition(id, vector)`, `SelectIcon(id)` | | (not used) |

UNVERIFIED (needs the in-game test): that with the brain switch first the host's marker shows on the guest's minimap
and stays (and survives getting into a car: `at his position (in a car)` instead of `could not move it off`); that
`GetIdByScriptWrapper` returns our id for a working icon (if it gave 0 for a visible one, every icon would be "lost"
twice and end as a position icon: look for `checked on his puppet: the game has it`); that a human icon moved with `ChangeIconTargetToPos` is still drawn (on the
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
- Both players in their own copy of one mission car at one spot (0.15.0 test, after the guest's reload at Taxi
  cp_020: the guest's taxi 2 cm from the host's, the host in his): the guest's puppet stayed `veh-hidden` for minutes
  on the host (trace `VEH ... have=0`, the log said "another car stands where it is" once). Since this fix the copy
  waits 2 s, then is spawned beside his car (src/game/seats.h `PlaceCopy` / `AsideSpot`: 6 m to his right or left,
  7.5 m behind or ahead, 9 m out, no car within 5.6 m, ground and wall checked with `mc_raycast`) and his puppet is
  seated in it; it is held there (not driven onto his car) until his car is 4.5 m clear of every car here, then the
  vehicle controller takes it to his car. A parked copy of his earlier car standing on his spot (his car's id changed
  with a reload or a rejoin) is dropped first, and on a new world every car record except copies and hidden real cars
  is looked up again (a guest whose taxi had stood for the host's car went on reporting the host's car key after a
  reload when the new car compared equal to the old entity; UNVERIFIED that it does). All of it UNVERIFIED in game:
  the ray checks, that SpawnCar takes the spot, that the snap onto his car looks right.

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
| CarParked says the car was destroyed (new flags byte, docs/protocol.md) | ours comes back there and is set exploded (`car:SetExploded()`, the wreck without a second blast; since 0.15.0, section 23: the blast itself is shown as a picture) so this game's mission sees what the driver's saw. The driver's game takes a car as destroyed when `car:GetDamage()` is 100 or more: its scale is 0..100 percent and **100 = exploded** (read from the exe, section 24 / re-notes 11; the raw `GetDamage` / `GetMotorDamage` are still logged with every CarParked). Since the car damage sync (section 24) ours also gets the car's damage when it comes back, and its explosion if it blew up meanwhile |
| the copy could not be spawned, or the game removed it | ours comes back at once at the driver's last transform (no further swaps of that car if spawning failed) |
| a new world (checkpoint load, reload after a death or a failed mission, following the host, quit) | ours, if the entity survived the load (same GUID), is shown where it is (`Activate`), else forgotten; the old world's copy is not touched (its traffic id may belong to a new car). A copy that is still valid means it was only a loading screen in the same world: nothing happens |
| the session ends | ours comes back at the copy's place at once; if we sit in the copy it stays (a normal car) and ours comes back when we got out (`M.leftCars`) |
| a car record is dropped (model changed, trimmed) | ours comes back first; copies with ours hidden behind them are never trimmed |

Log lines: `car K: replaced by a fresh copy for the new driver (why), ours (id at x y z) hidden meanwhile`,
`swapped car K: parking, a puppet is still in the copy (...)` (every change of the machine's state, from the dll),
`car K: our own car id is back at the copy's place (x y z; hidden at ...), activate ok, active true, copy despawned:
parked`. Sync trace: `SWP` (machine) and `SWR` (swap / restore / forget), docs/sync-trace.md.

Unverified: `Activate()` on a deactivated car (and a teleport of it) showing it again where we put it, with its
mission state; `DespawnEntityImmediately` of a copy right after its occupants got out; `GetDamage`'s scale in a
running game (read from the exe since: 0..100, 100 = exploded, section 24); whether a deactivated car survives a checkpoint load deactivated (it is shown again then); a checkpoint **save**
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
| `DOOR_KICK` / `LOCKPICK_START`, `PLAYER_CUSTOMLOCKPICK` / `DOOR_OPEN` | 458857 / 458792, 458833 / 458858 | door kick (18), lock pull (19), door opened by hand (20); the door itself: section 21 | no |

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
| `h:DesirePlayAnimation(name, desirability)` / `PlayAnimation(...)` | the other script ways; now tried before `AnimPlay`: see "Why no punch showed" below |

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

**Why no punch showed (0.15.0 test; static analysis 2026-10-19, the fix NOT verified in game).** The test: the
actions arrive and the trace has `AP ok=1`, but every `AE` is `done=0 why=time_up`, no punch pose shows; from the
console `AnimPlay(punch)` shows nothing within 5 s, `AnimPlay("hero_nw_calm_idle_fidget_cracknuckles")` plays but ~2 s
after the call every time, and the throw animation is over after 0.14 s. What the exe and the game's scripts say
(docs/re-notes.md 12):

- **`AnimPlay` goes through the AI, not to the animation.** Its native call builds an AI command (0x1B8 bytes, ctor
  0x27DC880) and appends it to the command buffer of the human's AI (`+0x138`, 0x722090) - the same queue as `MoveVec`.
  Commands in that buffer run when the AI's current *desire* lets script commands run. The puppets have the FRIEND
  brain (`SwitchBrain(1)` 2 s after spawn, section 18) with desires of its own. The game's own script that plays
  `AnimPlay` on an NPC (`scripts/common/searchactionpoints/example.lua`) first calls
  `npc:ScriptControl(true, "DesireBattleScriptPoint")`, waits for the `HumanMessages.SCRIPT_CONTROL` (458783) message,
  and only then `AnimPlay`s; at the end `AnimStop()` and `ScriptControl(false, ...)`. The designers' node
  `Human.scriptControlActivate` is `ScriptControl(true, "DesireMainScript")`. So the brain's state machine does stand
  between `AnimPlay` and the animation: most likely cause of both the ~2 s start (the command waits for the brain to
  get round to it; the glue also kept steering the puppet during the console test, `SetDir`/`SetPos`/`MoveVec` are AI
  commands or touch it) and of the punch that never started within its 0.7 s (UNVERIFIED).
- **`PlayAnimation(name, loop)` has a direct path.** Binding 0x23A2560 (2 args -> 0x23D32C0 -> native 0x2BA2590 /
  0x2BA25E0): when the human's animation component (`+0x1D8 -> +0x78`) knows the animation (0x2B4EAD0 looks the name
  up in its animation set and checks it is loaded), it is played by that component at once (0x2C87160, through
  the human's vfunc `+0x2C8`), without the AI's command buffer; otherwise it falls back to an AI command (ctor
  0x28A42F0, the same buffer). It returns a sync object; `StopAnimation(so)` stops it.
- **`DesirePlayAnimation(name, d)`** (0x239F700 -> 0x2B96D70) is the same AI command as `AnimPlay` with a
  desirability float at `+0x1B8`: the AI weighs it against its own desires (the scale is not known; the glue gives 100).
- **Animation layer / blend.** No layer or blend argument on any of these. The only related switch is
  `QueueFullbodyNoTransition(bool)` (0x2390310): sets bit 1 of `+0x1D4` of that same animation component, by its name
  "queue a full-body animation without a transition" (UNVERIFIED what it does; the reader was not found statically).
- **Is the melee set loaded?** Yes: `/sds/basic_anim/hitmelee_anim.sds` is in the `Persistent` group of the game's
  streaming table (tables.sds), with basic_anim, takedowns_anim and script_anim. The punch names are in the animation
  project (`anim_project.sds`, e.g. `hero_nw_alert_melee_attack_light_1_v1.hkx`), like the fidget.
- **Fight / combat mode, the game's melee entry.** The player's punch is the melee controller driven by input; the
  only script doors into it are `AttackMelee` (does nothing on a puppet, section 11) and `EnableCloseCombat` +
  `AttackMelee` (crashed). `SetCombatMode(bool)` (0x23C2630) acts on the human's `+0x2C8` component (0x2C2D920,
  0x2C3C3C0, 0x2C304D0): taken to switch the alert/combat stance the melee animations belong to (UNVERIFIED).
  `IsMeleeFighting()` reads the state. The game's AI fighters use `enemy_` animations (`enemy_fight_punch1_v1`,
  `enemy_1h_attack1_v1a`, `enemy_1h_aim_to_gremade_throw` (sic)), now at the end of the lists.
- **The throw over after 0.14 s**: `hero_1h_alert_grenade_throw_cf` is a 1h (gun-hand) animation with an
  `_autoadditive` twin, i.e. one layered over an aim pose; on an unarmed puppet the command seems to end at once.
  Same remedy: the ways below, and the enemy grenade throw and the molotov throw as further names.

**What the glue does now (unreleased; UNVERIFIED which way works).** Each action is played one of six ways, tried in this
order (`M.ANIM_METHODS`, src/game/anim_methods.h), one order per action group (melee 1-4 and 6, throw 5, traversal
moves 7-14, other 15-20):

| # | way | calls | undone after |
|---|---|---|---|
| 1 | `direct` | `PlayAnimation(name, false)` | `StopAnimation(so)` + `AnimStop()` if not done |
| 2 | `direct_notrans` | `QueueFullbodyNoTransition(true)`, `PlayAnimation(name, false)` | the flag back to false |
| 3 | `desire` | `DesirePlayAnimation(name, 100)` | `AnimStop()` |
| 4 | `script` | `ScriptControl(true, "DesireMainScript")`, `AnimPlay(name, false)` | `AnimStop()`, `ScriptControl(false, ...)` |
| 5 | `script_combat` | `SetCombatMode(true)` + 4 | 4, `SetCombatMode(false)` |
| 6 | `animplay` | `AnimPlay(name, false)` (0.13.0-0.15.0) | `AnimStop()` |

Every try is measured: right before the call and every tick while it plays, `GetBonePos("RightHand")` /
`("LeftHand")` (bone names from skeletons.sds) are read in the puppet's own frame (forward, right, up from its
position). The farther hand's largest distance from its rest decides: 0.20 m or more is "moved" (a punch or throw
moves it 0.4-0.7 m), under 0.08 m "still"; in between, or when the puppet itself moved over 0.35 m (a walk swings
the arms; not counted for traversal moves, which are drawn by `SetPos`), or the bones cannot be read, "unknown". Two
"still" tries move on to the next way, three "unknown" ones too (so a test without readable bones still sees each
way); a "moved" one keeps that way for the group (log `ANIM: melee actions SHOW with 'direct' ...`); a kept way that
then shows nothing three times in a row is dropped. After the last way it starts again with the first and the next
animation name of the list first (`round`). While a group's way is not confirmed, an action is watched (and the
puppet left alone) 2.5 s instead of its own time, so a late start is seen and measured (`start=` in `AE`). Puppets'
`SCRIPT_CONTROL` / `AI_CONTROL` messages are logged with the time since the request (trace `SC`). From the dev
console: `MafiaCoop.TestAction(kind[, way[, id]])` plays an action on a puppet now (way forced for its group),
`mc_anim_set("melee" | "throw" | "move" | "other" | "all", way)` forces a way; `MafiaCoop.actionAnims` as before.
The ways' order is kept for the whole run of the game (not reset by a load). The thresholds are guesses (UNVERIFIED);
the `AE` numbers show what a played and an unplayed animation really measure.

**No damage from the picture.** The animation is not an attack order, so it should not hit anyone; to be sure, a
puppet swinging within 3 m makes the local player a demigod for the swing (+0.3 s; `SetDemigod`, which also stops
DAMAGE events, section 5), the Hit of a real punch still applies (`M.HurtScript` switches the demigod off around its
`DoDamage`), and a melee DAMAGE with a puppet as enemy is given back (logged "swing hit us here"). The demigod moment
also blocks other damage for that time (an enemy NPC's bullet during a puppet's swing next to us).

Not carried yet (seen while looking, not cheap with this mechanism): takedowns with their paired victim animation.
Thrown grenades and molotovs: their flight (a marker and trail drawn by our overlay) and their explosion and fire,
section 23. Medkits, bodies, vaults, ladders, jumps and the sneak walk: "Traversal" below. Door kicks, the lock
pull and doors opened by hand: section 21.

**Shooting and punching a puppet (unreleased; static analysis 2026-10-19, NOT verified in game).** The 0.15.0
two-PC test: the host fired about 20 shots at the guest's puppet and no Hit went out (no `we hit puppet` from a
bullet), and every punch logged opponent `C_EntityGuid: 0` (damage only through the 1.9 m swing fallback, no
lock-on). Both have the same cause: every puppet gets the FRIEND brain (`SwitchBrain(1)`, section 18, `M.Ally`), which
makes it AI type 1, the player's own side. What the exe says about sides:

| address | what | notes |
|---|---|---|
| 0x2BA2470 | `GetAIType` native | `[[ai] + 0x298]`: the AI type (the binding 0x23B34A0 reads it through the human's AI at `+0x130`) |
| 0x25E0700 | "is friend" (a, b) | `a +0x1FD` set (MimicFriend) -> true; else both AI types are mapped to a faction (type 0 -> 3, 1 -> 0, 2 -> 1, 4 -> 2, 17 -> 3, others none) and a float table (0x2496130, presumably `config/game/faction_definitions`) is read at [faction a][faction b]: true when > 0. 12 callers |
| 0x24AFB90 | "is enemy" (a, b) | the same lookup: true when < 0; `+0x1FD` set -> false. 34 callers, one of them the hit-marker code (`enable_hit_marker`, 0x2C0A9B0) |
| 0x23B3340 | `MimicFriend(bool)` binding | writes byte `+0x1FD` of the human's AI (0x2B98730): the human then reads as a friend to everyone (the opposite of what we need) |

So a friend-brain puppet is in the player's faction (both AI type 1): the game's melee targeting (only AI type 0
humans, section 11) and its damage of player bullets skip it. Which caller is the bullet filter and which the melee
target picker was not found statically (UNVERIFIED that they use 0x25E0700). Options that were weighed:

- **A non-ally brain or AI type on foot** (friend brain only for car seats): no. Brain switches are what crashed the
  game before the game-sectors guard (section 15), the seat fix needs the friend brain before every seat, and the
  host's enemies only target the puppet because of it (`enemies_target_guests`). Switching back and forth would bring
  those problems back. Not done.
- **MimicFriend / a per-entity "can be targeted" flag**: there is no such flag in the human bindings
  (re2/human_methods.txt: `SetVisibleByPlayerAim` / `IsVisibleByPlayerAim` exist, by name about the aim assist, not
  looked into; UNVERIFIED); MimicFriend makes a human more friendly, not less.
- **A hostile relationship to the local player only**: possible natively (a hook on 0x25E0700 answering "not a
  friend" for the pair (our player, a puppet) only; enemies keep their relation to the puppet). Not done: a hook on a
  hot AI function with 12 callers, all unverified, and the friend brain's own decisions use the same function.
  Kept as the next step if the fallbacks below are not good enough.
- **Our own hit test** (done): the mod decides what we hit, the game's own word first.

What the glue does (src/game/pvp_hit.h, unit-tested by tests/pvp_hit_test.cpp; player_sync.lua M.ShotFired /
M.ShotTick / M.MeleeLock / M.MeleeTick):

| step | what | notes |
|---|---|---|
| a shot | `countShots` (an ammo drop of the weapon in hand) | the camera's position and direction (`GetPlayerCamera():GetPos()/GetDir()`, the crosshair ray) are tested at once against each puppet's body: a vertical capsule of 0.35 m radius from the feet to 0.15 m above the head bone (`GetBonePos("Head")`, else 1.62 m), up to 150 m (`mc_pvp_ray`); the nearest one hit; the zone from the height (head, body, lower body, leg). A wall: `mc_raycast` from the camera to 0.6 m before the puppet (a hit next to our own player does not count, as for the name tags) |
| 0.3 s later | the verdict (`mc_pvp_verdict`) | 1. the game's own DAMAGE of our bullet on a puppet within -0.2..+0.3 s of the shot: the Hit was already sent (M.OnPuppetDamage), nothing more (a puppet whose brain switch is still to come, AI type 0, gets them); 2. an `ON_BULLET_HIT` to us naming a puppet: a Hit; 3. our ray hit, not blocked, the puppet alive, shown and on foot: a Hit. Each piece of evidence counts for one shot only |
| self-check | `rep` | once the game reports our bullet on a **friend-brain** puppet (1 or 2), the ray alone no longer hits (`ray_unconfirmed`): the game's bullets (with their spread) decide from then on. Logged once |
| the Hit | `M.SendHit(pp, amount, 0 = SHOT, zone, weapon id)` | the amount: what our bullets of that weapon did to any human this session (learned from our DAMAGE events), else 10 (pistol, revolver, SMG; measured), 30 shotgun, 25 rifle, 12 MG (guessed). The victim's game replays it as a real bullet from our puppet (section 11), so his game's own damage and hit reaction apply. The puppet here gets `DoDamage(1)` for a flinch |
| an orphan `ON_BULLET_HIT` | 0.6 s with no counted shot | a Hit by itself (`bullet_alone`), unless the game's DAMAGE came with it |
| a punch | `M.MeleeLock` at the swing message | only when the game aimed it at no puppet; the nearest puppet within 2.6 m and 70 degrees of our facing (else of the camera's direction) is locked: our player is turned to it (`p:SetDir`, UNVERIFIED that the player's melee keeps it); at the impact (0.25 s) it is hit within 2.3 m and 60 degrees, else the plain swing test (1.9 m, 45 degrees) |
| friendly fire | `friendly_fire` (CO-OP SETTINGS, each player's own ini) | off: no shot or punch of ours becomes a Hit (verdict `off`), and Hits from others are not applied (as before) |

Unchanged on purpose: the puppet's brain, its AI type and its seats; the map marker's wait for the brain switch
(brainPending) and the script control gate (nothing here touches the puppet's AI: `SetDir` is on our own player).
Logs: `our bullet on puppet N: the game's DAMAGE / ON_BULLET_HIT`, `our shot (...) at puppet N: <verdict>`,
`our light punch: locked on to puppet N (...): turned to it (SetDir ok)`, `lock-on on puppet N at the impact: X degrees
off our facing ...: the turn took / did NOT take`; trace `PV` / `PVM` (docs/sync-trace.md).
UNVERIFIED: whether `ON_BULLET_HIT` comes for a friend, whether the camera ray matches where the bullets go (spread,
hip fire, the muzzle offset), whether `SetDir` turns the player during a swing, the non-pistol damage values.

### Traversal: jumps, vaults, ledges, drops, ladders, cover slides, sneaking, bodies, medkits - static only, NOT verified in game

Found the same way (exe Lua bindings re2/human_methods.txt, the enums dump re3/enums.txt, the game's script and
animation archives); no game launch. Code: src/game/traversal.h/.cpp (pure, tests/traversal_test.cpp), the action
kinds 7..17 and the pose block in player_actions.h (docs/protocol.md "PlayerState"), player_sync.lua
"Traversal" / `M.Follow` / `M.LadderHold` / `M.CarryTick`.

**Detecting it on our player.** `HumanMessages` has these (C_WrapE_FightHint enum, re3/enums.txt line 97); the glue
subscribes to each with one blanket HUMAN subscription, keeps those whose receiver is our player (UNVERIFIED that he
is the receiver, as for the melee messages) and only collects them as bits for the next sample; the C++ side
(`traversal::LocalDetector`, fed with every sample: height, smoothed vertical speed, ground speed, cover flag and
the getters below) decides:

| source | value | becomes |
|---|---|---|
| `USE_FENCE` | 458865 | climb up (kind 9) when he rose 0.6 m within 0.3 s of it, else vault (8) after 0.3 s |
| `USE_LADDER` | 458864 | ladder up (12) / down (13) by the first 0.15 m he moved within 0.3 s (else up); pose bit "ladder" until his vertical speed stays under 0.3 m/s for 0.6 s (at least 0.8 s, at most 15 s) |
| no message: vertical speed above 2 m/s up, no fence / ladder within 1.2 s | - | jump (7) |
| no message: falling faster than 2.5 m/s without a jump | - | drop down (10); a hard landing (11) when he fell faster than 5 m/s for 0.3 s or more |
| `IsInCover()` going on at 2.5 m/s or more | - | slide into cover (14) |
| `PLAYER_USED_MEDKIT` | 458802 | medkit (15) (player.lua's OnMedkitUsed node registers this one on the player) |
| `PICKUP_BODY` / `DROP_BODY`, or `IsCarryingBody()` going on / off | 458853 / 458854 | body picked up (16) / dropped (17), message and getter edge within 1 s count once |
| `IsStealthMove()` | getter | pose bit "sneaking" |
| `IsCarryingBody()`, `GetCarriedBody()` | getters (player.lua TakeBody / IsCarryingBody nodes) | pose bit "carrying" + the body's key (npc_sync numbering) |

There is no jump message or getter in the bindings (none in `human_methods.txt`, none in the enum); the flags' "in
air" bit stays as it was. `USE_CORNER` (458866), `PLAYER_COVER_LEAN_*` and `IN_DEEP_WATER` were seen and are not used.
The thresholds are guesses for a 60 Hz sample with the C++ side's smoothed speed: the trace (`TL`, `TE`, docs/sync-trace.md)
shows what was detected with which height and speed, to tune them.

Crouching was already shown (flags bit1 from `IsStalkMove() or IsCoverCrouch()`, `SetStalkMove` on the puppet,
section 10); the sneak walk is `SetStealthMove` / `IsStealthMove`, a separate pair in the bindings: what it looks like
on an AI human is not known.

**Playing it on the puppet.** The same `AnimPlay(name, false)` as the punches, from `MafiaCoop.actionAnims[kind]`
(changeable from the dev console). Names from `basic_anim.sds` / `anim_project.sds` (unpacked with re2/sds.py):

| kind | names (in order; armed: tried first with a gun in hand) | time |
|---|---|---|
| jump | `hero_nw_alert_jump_over_ledge_medium_r`, `hero_nw_alert_jump_land_soft` (no plain jump-off animation in the sets: the jump is probably a state of the animation graph) | 0.9 s |
| vault | `hero_nw_alert_move_run_vault_wall_thin_140cm`, `hero_nw_alert_move_run_vault_wall_140cm`, `hero_nw_cov_vault_wall_90cm`; armed `hero_1h_alert_move_run_vault_wall_thin_90cm_step_up`, `hero_rifle_alert_move_run_vault_wall_thin_90cm` | 1.1 s |
| climb up | `hero_trench_alert_idle_climb_up_180cm`, `hero_rifle_alert_idle_climb_up_180cm`, `hero_trench_alert_idle_climb_up_130cm` (no `hero_nw_` climb-up in the sets) | 1.6 s |
| drop down | `hero_nw_alert_move_walk_lp_to_fall`, `enemy_nw_alert_idle_climb_down_130cm` | 0.8 s |
| hard landing | `hero_nw_alert_jump_land_medium`, `hero_rifle_alert_fall_land_hard`, `hero_nw_alert_jump_land_soft` | 0.8 s |
| ladder up / down | `hero_nw_alert_climb_up_ladders` (`_low`, `_veryhigh`) / `hero_nw_alert_climb_down_ladders_285cm` (`_forward`) | 3.0 / 2.5 s |
| cover slide | `hero_nw_cov_vault_wall_deep_90cm` (no slide animation found; a stand-in) | 0.8 s |
| medkit | `hero_nw_stalk_idle_fidget_lookaround` (no medkit animation found in the sets) | 1.0 s |
| body pickup / drop | `hero_nw_alert_idle_carry_pickup_male` (`_fast`, stalk) / `hero_nw_alert_idle_carry_drop_male` (stalk) | 1.6 / 1.4 s |

Others for later: the `_s`/`_fem` carry variants, the trunk / van body drops (`hero_nw_alert_idle_carry_drop_trunk_*`),
`hero_rifle_alert_move_run_vault_wall_*cm` per height, `hero_rifle_alert_fall_loop`, `hero_nw_stalk_move_walk`.

While a move (7..14) plays, or while his pose says "ladder" after the climb animation (at most 15 s), the puppet's
`PS` report says busy 2: the C++ locomotion controller is paused as for a punch (it sees no copy) and
`traversal::Catchup` sends a nudge toward his position every tick instead, height included (10 x dt of the gap);
the glue applies it with `SetPos` and nothing else (no walk, turn, aim). Whether `SetPos` during `AnimPlay` keeps
the animation playing is UNVERIFIED (nudges during a walk do not stop the walk). A move whose animation `AnimPlay`
refused is still followed for its time. For 1.5 s after a move a snap the controller asks for (it starts from
"stand" with the puppet maybe metres off) becomes a blend (6 x dt) when the error is under 8 m and 6 m in height, the
part of the error beyond 0.9 m is blended away faster than the controller's own nudge, and a height difference over
0.25 m is blended too (the walk cannot climb a ledge); the T line carries that height part (`nz`). Farther off it
still snaps. Logged ("plays a traversal move", "move over, x m off: catching up", "snap after a move turned into a
blend") and traced (`TVC`, `PUP busy= tm= rec=`).

Every puppet also gets `SetCanTraverse(PathObjectType.ALL, true)` at spawn (`C_WrapE_PathObjectType`: CLIMB 1,
CAR_ROOF 3, CAR 5, DOOR 11, LADDER 13, ALL 15; human.lua's SetCanTraverse node takes inPathObjectType,
inCanTraverse), so its own walk (`MoveVec`, AI path following) may climb and use ladders where its player went
instead of stopping at the fence. UNVERIFIED what the default is and whether the AI's traversal then plays its own
vault (it would make the walk the better picture of a vault).

**Sneaking**: pose bit 1 -> `SetStealthMove(true/false)` on the puppet when it changes (dropped while it aims, like
the crouch). **Carrying a body**: pose bit 2 with the body's key. The body is a mission NPC in the entity table
(npc_sync.lua): on the host its own id, on a guest the host key its NPC is paired with (`N.byLocal`); the receiver
finds its own copy of that NPC (`N.ents`, on a guest via `N.host[key].loc`) and calls `puppet:TakeBody(body)`
(player.lua's TakeBody node calls it on the player with inEntity after `CanCarryBody`); 1.5 s later
`IsCarryingBody()` tells whether it took (logged, traced `CB`). If it took, the game carries the body along with the
puppet and `DropBody()` puts it down when the pose bit goes; if not (an AI human may not carry, the body is not
known here, key 0), only the pickup / drop animations show it, and the body is put at the puppet's feet when he
drops it. UNVERIFIED: all of it, and whether `DropBody` needs an argument (the node has inDropAnim). The dead NPC's
position is not synced otherwise (npc_sync keeps live copies in line).

**Medkit**: `HealByMedkit` exists on the human, but the health already comes with the state; only the action is
shown.

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
| `car:SetSirenOn(b)`, `car:SetBeaconLightOn(b)` | police copies, from the host's siren flag: the host reads its police car's siren / beacon bits (`mc_car_signals`, section 22); only when they cannot be read the flag is a guess: on while a police car goes faster than 14 m/s (or is near a hunted player) and 8 s after |
| `car:DisableExplosion(true)` on copies | an explosion on one game only would leave the host's car driving on. Since the car damage sync (section 24) the host's car's fire and explosion reach the copy through that sync instead, and its dents, tyres and parts too |
| `h:GetOffVehicle()` | the copy's crew when a player takes the car (handed over to player_sync), the host's traffic driver when a guest took the car, the copy's driver when the host's car lost its driver. Does nothing while the car is moved by `SetPos` every tick (section 7) |
| `game.traffic:DespawnEntity(id)` / `DespawnEntityImmediately(id)` | copies within 80 m of the guest go when unseen, others at once |

## 20. Things lying in the world: dropped weapons, pickups, collectibles - static only, NOT verified in game

Used by src/game/item_sync.* and src/game/world_items.h (messages in docs/protocol.md "World items"). Found
statically in the exe (class registrations, enum tables, strings; the method of research-world-sync.md "Method") and
the game's own scripts (`collectibles.lua`, `player.lua`, `game.lua` nodes). Nothing here has been run in the game;
every call is wrapped in `pcall` and its failure logged (`items: ...`).

**What the game has.** The item manager (`ItemManager`, `ue::C_Singleton<I_ItemManager,...>`) owns item entities:
`C_ItemWeapon` (EntityType ITEM_WEAPON 63; data `C_ItemDataWeapon`, `S_ItemWeaponInitProps`), `C_ItemInventory`
(ITEM_INVENTORY 66; data `C_ItemDataInventoryItem`, `C_ItemDataMedkitItem`), `C_ItemScriptAB` (64), `C_ItemUpgrade`
(68), all `C_ItemBase`. The Lua wrapper `C_WrapperItemWeapon` (parent `C_WrapperActor`) has **no methods of its own**:
what a script can do with an item is the entity/actor part (`GetPos`, `SetPos`, `GetName`, `Activate`, `Deactivate`,
`IsActive`, `GetActionByType(enums.ActorActions.ITEM_PICK_UP = 8)`, `GetOwner`). A string `ItemWeaponDespawnDistance`
says dropped weapons are removed at a distance. `C_Pinup` / `C_WrapperPinup` (`GetPinupID`, EntityType PINUP 54) is a
Mafia II-era leftover; no Mafia DE script uses it.

| call / fact | notes |
|---|---|
| `mc_entity_ids(63)`, `mc_entity_ids(66)` (ours, section 15) | every weapon on the ground / inventory item of this world. **UNVERIFIED**: that `mc_entity(id)` wraps items (it does humans, cars, dummies, spawners), that a dead enemy's weapon is such an entity (and not, say, still a child of the corpse until touched), what `GetName()` returns for a dropped weapon (we send it and pair by it when both names are known) |
| `item:Deactivate()` | how an item is hidden (a guest's copy of an item taken elsewhere, the host's for a guest). **UNVERIFIED** for items (verified for cars and humans) |
| `game.game:ClearWeaponsInRadius(pos, radius)` (thunk 0x267E370: vec, number) | the game's own clean-up of dropped weapons around a point (a visual-script node, `game.lua` `gameClearWeaponsInRadius`); used when a hidden weapon is still active a moment after `Deactivate`, radius 0.3 m. **UNVERIFIED** what exactly it clears |
| `item:SetPos(v)` | a guest's paired item put where the host's lies. **UNVERIFIED** for a physics item (the read-back distance is logged) |
| picking up | no event names the item. `HumanMessages.PLAYER_WEAPON_PICKUP` (458800, class `C_HumanMessagePlayerWeaponPickup`: GetWeaponID, GetWeaponName; `player.lua` registers it on the player), `PLAYER_WEAPON_ADDED_ON_EMPTY_SLOT` (458801, GetSlotType), `PLAYER_USED_MEDKIT` (458802), `LOOT_CASH` / `LOOT_GRENADE` / `LOOT_MOLOTOV` (458875-7, `C_HumanMessageLoot`: GetPos, GetAmount) exist. MafiaCoop detects a take by the item entity vanishing (gone or switched off) within 2.5 m of the player instead, which works for every item kind. **UNVERIFIED**: that a picked item's entity goes away (and is not just hidden and kept) and that walking over a weapon only for its ammo leaves it lying (then it is no take) |
| a weapon swap | picking a weapon with a full slot drops the old one at the player's feet (a new item there; **UNVERIFIED**). It is his own drop: never hidden on his game (within 2.5 m of him when it appears); not shown on the other games (no spawn) |
| spawning an item | no binding. `h:InventoryDrop()` (no args, the human drops what it holds) would be the way: give a puppet the weapon (`DbgTesting_HelperInventory_AddWeapon`) and drop it. Not used: a host item without a partner on a guest's game is not shown there |
| `p:InventoryRemoveWeapon(name)` | a guest whose take the host refused (another player was first) gives the weapons he gained in the last 2 s back. **UNVERIFIED** for the player (research-world-sync A6 lists it) |
| `p:GetInventoryWeapons()` | the player's weapons (varargs, section 2): polled each round to know what a take gave him |
| medkits | `InventoryGetMedkitCount/Capacity`, `HealByMedkit`, `InventoryAddItem("Medkit")` (the shop test node). Wall first-aid boxes in missions (`lh_wall_medkit_a_scripted_v1`, interaction `interact_first_aid_box`) are mission-script interactions, no item entities: per player |
| money | `InventoryGetMoney/AddMoney/RemoveMoney`, `LOOT_CASH`, `hud:MoneyShow` are Mafia III-era bindings; Mafia DE has no money pickups (**UNVERIFIED**). Not shared |

**Collectibles** (`/scripts/common/commands_library/system/game/collectibles.lua`, module `collectibles`). Each is a
scene entity named `lh_pickup_<kind>_<id>_bv01`: kinds `bm` (Black Mask), `cc` (cigarette cards, 22), `comics` (10),
`dd` (Dime Detective), `pc` (postcards = hidden car clues), `sss` (Super Science Stories), `tt` (Terror Tales), plus
`lh_pickup_fox_a_bv01_NN`. The script gives each an actor action (`game.game:CreateScriptActorAction`, ACTION_HELD /
ACTION_USABLE events); when it is picked (`CollectiblePicked`) it sets the save group in the datastore
(`DatastoreSet`), shows the HUD (`OnShowCollectableCollected`), calls `getp():AddToCollection(name, bool)` and
`Deactivate()`s the entity; at load (`InitCollectible`) a collected one is deactivated at once (`DatastoreExists`).
MafiaCoop looks each name up once a second (`game.entitywrapper:GetEntityByName(name)`) and takes "was active, now
switched off, within 4 m of us" as our pickup. **Decision: per player.** Each player collects his own copy for his own
save; nothing is hidden on the other games, no other game's datastore or collection is written (writing the host's
save from a guest's action, or deciding for a guest, is not safe without knowing the datastore layout). The others
only hear about it (Collectible message, log, status line). Note: a guest's saves (and a CHAPTER host's) go to the
scratch tree during co-op (save_guard.h), so what he collects there is not kept in his real save. **UNVERIFIED**:
that the entities have exactly these names in the world (they are in the mission SDS string tables, e.g.
`lh_pickup_cc_04_ennio_salieri_bv01` in mm_120_mansion), that `GetEntityByName` finds them, the fox count.

**Choices for shared pickups.** Weapons on the ground and inventory items: one game's take removes them on every game
(the host decides who was first). The game may respawn some pickups (**UNVERIFIED**); a respawned one is a new entity,
paired again on its own. Physics props (crates, barrels) stay local to each game.

Log lines: `items: guest N took his copy of our item K ...: his; ours is hidden`, `items: item I (type T, name) at
x y z hidden: <why>`, `items: item I (...) moved from ... to the host's spot ... (read back D m off)`,
`items: our item I (host item K) vanished: taken by us, asking the host`, `items: the host's item K: taken by player
P (ours: I)`, `items: another player took that item first: W taken away again (...)`, `items: we collected <name>`,
`items: player P found <name> (<kind>) on his game; ours stays for our own save`. Sync trace: `ITH`, `ITG`, `ITE`,
`ITL`, `COL` (docs/sync-trace.md).

## 21. Doors on every game (unreleased) - static only, NOT verified in game

Found by reading the exe, its Lua bindings, the game's own door scripts (`tables\scripts.sds`:
`/scripts/common/commands_library/entity/door.lua`, `.../system/game/persistent_location.lua`) and its animation
archives, without launching the game. Used by src/game/door_sync.lua (rules in src/game/door_model.h, messages in
docs/protocol.md "Doors"). Every call is wrapped in `pcall`; failures are logged (`doors: ... failed`) and traced
(`DA calls=...!`).

**What a door is.** Two script faces of one thing (UNVERIFIED that they are the same object for every door):

| face | calls (re-notes appendix) | notes |
|---|---|---|
| entity, `C_WrapperDoor` : `C_WrapperActorDeform` (table 0x5AD8480) | `Lock()`, `Unlock()`, `AILock`, `AIUnlock`, `IsLocked()`, `StartLockpick`, `StopLockpick(bool)`, `IsLockpicking`, `Kick(vec)`, `Open(vec)` / `Open(vec, bool)` (the thunk 0x23C5120 dispatches on the argument count: 0x23DA650 vec, 0x23DA710 vec + bool), `Close()`, `ToggleOpen(vec[, bool])`, `IsClosed()`, `EnableAction` / `DisableAction`, `PlayAnim`, `StopAnim` | `Lock` (0x23C0050) takes no argument (self only, then a native call on `[self + 0x10]`). The `vec` of Open / Kick / ToggleOpen is a position: the game's `SetState` door node passes `inPos:GetPos()` (the door swings away from it, a guess from the name). Entity type 21 (`enums.EntityType.DOOR`) |
| scene object component `C_RuntimeDoorComponent` (table 0x5ACF860) | `IsOpen`, `IsClosed`, `Open`, `Close`, `Lock`, `Unlock`, `EnableForPlayer` / `DisableForPlayer`, `EnableForNPCs` / `DisableForNPCs`, `DisableLockPulling`, `RegisterCallback(enums.DoorEventType.X, fn)` (OPEN 2, CLOSE 4, KICK 8, STEALTH_OPEN 18 = the lock pull), `RegisterOnKickedCallback` | what the game's newer scripts use: `door.lua`'s `Lock2.doorLock` calls `Lock` through `CallObjectComponentFunction(entity, "C_RuntimeDoorComponent", "Lock", difficulty, kickable)`; the shops and safehouses (`persistent_location.lua` `InitDoors`) find their doors as subobjects named `door_front_*`, `door_back_*`, `door_interior_*`, `door_front_safehouse_*`, `door_back_safehouse_*`, `door_alarm_*` and open / disable them per location state |

The glue lists doors with `mc_entity_ids(21)` (the entity table, section 15) every 4 s and gets each one with
`mc_entity(id)`; it uses the wrapper's calls when the value has `IsClosed` and `Open`, else the component
(`GetComponent("C_RuntimeDoorComponent")`; the lock bit then from `IsLocked` if the value has it). How many doors a
city world has, and whether gates, garage doors and car-park barriers are type-21 doors, is UNVERIFIED (the status
line and the `ST` text count doors and which face they use; a door without either face is logged once as
`entity N (type 21) has no door calls we know`).

**The same door on two games.** Entity ids differ between games (section 15). A door is its scene name (`GetName()`,
from the scene file, so it should be the same on every game; UNVERIFIED) and its position when first seen, matched
within 1.0 m (door_model.h `Same`). UNVERIFIED: whether `GetPos()` of a door moves while it swings (if it is the
leaf's centre, a door first seen open on one game and closed on the other is up to ~0.5 m apart: still within the
match distance for a normal door).

**Story doors.** The mission scripts lock, unlock, open and close doors themselves: `door.lua` has nodes for all of
them (`doorLock`, `doorUnlock`, `doorOpen2`, `doorClose2`, `SetState`, `doorEnableForPlayer`, ...) and for the door
events (`OnOpen`, `OnClose`, `OnKick`, `OnLockPick`, by `RegisterEvent(..., enums.EventType.DOOR,
enums.DoorMessages.X)`), and the shops set their doors from the location state. Each game runs its own copy of
those scripts, so the host's game decides: its world's changes go to the guests, a guest's own world's lock changes
of doors the host decided are put back (docs/protocol.md "Doors").

**Messages.**

| what | value | used for |
|---|---|---|
| `enums.DoorMessages` (base 0x150000, enum at exe 0x5AE1600) | OPEN 1376257, CLOSE 1376258, KICK 1376259, TOGGLELOCK 1376260, LOCKPICK 1376261, OPENING 1376262, CLOSING 1376263, TOGGLEOPEN 1376264 | blanket subscriptions (`RegisterBlanketEvent(ctx, fn, enums.EventType.DOOR, msg)`, EventType.DOOR = 6) to OPEN, CLOSE, KICK, LOCKPICK, TOGGLELOCK, TOGGLEOPEN: the receiver GUID is the door (polled at once, wherever it is), the sender GUID is compared with our player's. UNVERIFIED that blanket events reach doors and who the sender is; the poll works without them |
| `HumanMessages.DOOR_KICK` 458857, `DOOR_OPEN` 458858, `LOCKPICK_START` 458792, `PLAYER_CUSTOMLOCKPICK` 458833 | on our player | player_sync.lua's action subscription (section 17): action kinds 18 door kick, 19 lock pull, 20 door open, and `M.doorActKind` / `M.doorActT` for door_sync's attribution. UNVERIFIED that the game sends them to the player who acts (as the melee ones); `LOCKPICK_START` may come for a car's lock as well (the puppet then plays the lock pull too) |
| `LOCKPICK_END` 458793, `PICKUP_BODY`, ... | | not used |

The lockpick of a door in this game is the **lock pull** (`C_WrapperPlayer:StartLockPull`, `LockPullGetSyncObjects`,
the `lockpull` animations, `DisableLockPulling` on the component); the poll sees its result (the door unlocked, and
opened).

**Applying a change** (door_sync.lua `D.Apply`): from the door's current state to the wanted one: `Unlock()` when
it has to be unlocked; `Kick(from)` for a kick (wrapper only), else `Open(from)` (then `Open(from, false)`,
`ToggleOpen(from)` if that fails; the component's `Open()`); `Close()`; `Lock()`. `from` is where the player who did
it stood. UNVERIFIED: every one of these on a door from a script outside the door's own scripts, that `Kick` does
no damage, that `Lock` right after `Close` locks (the door may still be swinging: a door that does not reach the
state in 3 s is traced `DR ev=unmet`).

**The puppet** (player_sync.lua `M.actionAnims`, section 17), names from `basic_anim` / `anim_project` (UNVERIFIED
that they play on a puppet):

| kind | names (in order) | time left alone |
|---|---|---|
| 18 door kick | `hero_1h_alert_doorkick`, `hero_1h_alert_doorkick_left`, `hero_trench_alert_doorkick`, `hero_rifle_alert_doorkick` | 1.1 s |
| 19 lock pull | `hero_nw_calm_idle_to_lockpull_begin_idle`, `hero_lockpull_break_attempt`, `hero_lockpull_break_success` | 2.5 s |
| 20 door open | `hero_nw_alert_door_open_push_far_handle`, `hero_nw_alert_door_open_pull_near_handle`, `hero_nw_cov_door_open_push_far_handle_160cm` | 0.8 s |

Other door animations seen: `hero_nw_alert_door_open_pull_far_handle(_horizontal/_vertical/_prison/_farm)`,
`hero_nw_stalk_door_open_*`, `hero_lockpull_begin_idle_loop`, `hero_lockpull_break_idle_loop`,
`hero_lockpull_hit_exit`, `hero_lockpull_exit_to_nw_calm_idle`, `hero_trench_alert_doorkick_left`,
`hero_rifle_alert_doorkick_left`, `sc_garage_door_large_open` (a garage door's own animation).

**Cost.** One entity table scan every 4 s (as the traffic scan), new doors read 120 per tick, the doors within 35 m
polled 5 times a second (`IsClosed` + `IsLocked`), one 33-byte reliable message per change.

## 22. Police and wanted levels on two games - static only, NOT verified in game

Used by src/game/police_sync.* (messages in docs/protocol.md "Police", the pure logic in src/game/police_model.h,
tests/police_test.cpp). Found statically (docs/re-notes.md 10, the game's flow scripts in scripts.sds); nothing here
has been run in the game yet. Each call is wrapped in `pcall` and its failure logged (`police: ... failed`).

How the game's police work (from the bindings): **one police zone for the player** (`game.police` keeps it): a
SEARCH (blue) or HUNT (red) zone with a centre and an escalation level. It is the wanted state: wanted = a zone
exists; "lose the cops" = leave it until it decays (`BLUEZONE_DECAY`, `POLICE_ZONE_DECAYED`); the HUD shows it.
Fines and arrests are police activities with their own events. There is no wanted-level getter and no bribe API.

| call | notes |
|---|---|
| `game.police:GetActiveZones()` | varargs: `0` when our player has a zone, nothing otherwise (re-notes 10). Read 5 times a second |
| `game.police:GetZoneInfo(id)` | `pos, level, zone_type` (vector, number, number; zone_type `enums.PoliceZoneType` SEARCH 0 / HUNT 1). **UNVERIFIED**: the level scale (logged: `police: zone 0: GetZoneInfo -> ...`) |
| `p:IsSeenByPolice()` | bool, our player only |
| `game.police:AreOffencesDisabled()` | bool (a mission switched offences off); reported as a flag |
| `game.police:RegisterForPoliceEvent(ctx, {enums.PoliceEventType.X}, fn)` / `UnregisterForPoliceEvent(id)` | one subscription per event type (so the callback knows which one fired), once per world. **UNVERIFIED**: what `fn` gets (logged for the first 20 events) and that every event listed in police_sync.lua fires for the player |
| `game.game:RegisterBlanketEvent(ctx, fn, enums.EventType.HUMAN, enums.HumanMessages.X)` | OFFENDER, OFFENDER_UPDATE, RECOGNIZED_BY_POLICE, POLICE_SEARCH, ARRESTED (`GetArrestFinished()`) on our player are reported; on a guest's puppet (host) they are logged (`guest N's puppet got OFFENDER on our game`): that shows whether AnnouncePoliceOffence made the puppet an offender. SEE_ENEMY (`GetEnemyGUID`) counts the cops that saw a targeted puppet |
| `puppet:AnnouncePoliceOffence(enums.OffenceType.X)` | **Not used (in-game test 2026-10-18)**: announced on the host from a wanted guest's puppet (RUNNING_FROM_POLICE), a cop witnessed it (`POLICE_WITNESS_STATE_STARTED` 1 s later) and 12 s later the host's game made **its own player** wanted, a zone centred on the puppet's spot (`we are wanted search level 1 at -110.1,-199.9`): the game has one police zone, its player's, and an offence from any human ends up there. With no cop near nothing happened. The host's police now only attack a hunted guest's puppet (next row) |
| `cop:Attack(puppet)` | host: police humans (`GetAIType() == 4`, alive) within 90 m of a guest's puppet who is hunted (any level; was level >= 3 while offences were announced): the nearest 6, again every 8 s. **UNVERIFIED**: the level that means "shoot" (ATTACK_LEVEL = 3 in police_sync.lua, a guess); their hits on the puppet reach the guest like any NPC's (docs/protocol.md "Phase 6") |
| `cop:Forget()` | host: the cops we set on a guest when he is no target any more; not while the host is wanted himself (Forget drops every target) |
| `game.police:ForceMinEscalationLevel(n)` | guest: while the host's police chase him, his own level is held at least at the chase's level; `0` when the chase is over (4 s later) and when the session ends. **UNVERIFIED**: that 0 is "no minimum" and whether forcing a level without a zone does anything |
| `game.police:ZoneCreate(pos, type, level)` | guest, at most every 5 s, only while the host's police are on him (an active PoliceChase): (seen) they see him (SEE_ENEMY of a host cop on his puppet) and his game has no zone: one at his position (HUNT or SEARCH as the host's chase says); (refresh) his zone decays (BLUEZONE_DECAY) while host cops are near his puppet: a fresh one at his position, once per decay; (revive) his zone decayed away while they are still near (he was wanted in this chase, not fined / arrested / dead in the last 30 s, offences not off). **UNVERIFIED**: what ZoneCreate does when a zone exists (the game has PLAYER_ZONE_MERGED, so a merge is likely). Never used for another player (the zone is always the player's own) |
| `game.police:PreventZonesDecay(hold [, follow])` | guest (decay fix, 2026-10-19): while the host's police are on him (active PoliceChase, and 4 s after it ended or went silent for 3 s) `PreventZonesDecay(true, follow)`, set again every 2 s; `(false, false)` when the chase is over, when the session ends and when he becomes the host. Static (docs/re-notes.md 10): `hold` stops the zone's decay timer (zone `+0x40`) and keeps it from being marked decaying; `follow` moves a HUNT zone's centre to the player each tick, as the game does when its own police see him. follow = his zone is red, the chase is a hunt and a host cop saw his puppet (5 s) or is within 40 m. The decay goes on from where it stood once released (he still has to lose them). Never called on the host (his zone is his own). **UNVERIFIED**: in game at all; whether a world load resets it (hence the 2 s refresh); a mission's own hold is released by ours |
| `car:SetSirenOn(b)` / `SetBeaconLightOn(b)` on police copies | the host now reads its police cars' real siren and beacon (`mc_car_signals`, car_signals.h bits 32 / 16) for the copies' flag 2; only when that cannot be read the old guess stays (faster than 14 m/s), plus "near a hunted player" (`MafiaCoop.Police.ChaseNear`) |

Who decides what:

- **Every player's wanted state is his own game's**: his crimes raise it on his game (his pedestrians, his police
  and - near the host - the police copies' crews, which are police humans on his game), he loses it, is fined or
  arrested there. A guest's arrest in a co-op story is his failed mission (`mm_000_go_arrest`, docs/protocol.md
  "Failed missions"); in Free Ride the game's own answer. Fines take his own money. Nothing of this is copied.
- **Near the host (250 m, released beyond 400 m: the same ranges as the traffic)** the guest's game spawns no police
  of its own (docs/game-api.md 19), so the host's police are the ones that react to him: his puppet on the host's game
  becomes their target (above), and the host tells him (PoliceChase) while some of them are near his puppet, how many
  and how many saw it; the police NPCs and cars he sees are the host's (npc_sync / traffic_sync), so they chase where
  the host's chase his puppet.
- **Far from the host** nothing is touched: his own police spawn and work as always; the host's police ignore his
  puppet.
- The host's own wanted state is his; the guests' games do not make their police go after the host's puppet.

Dev console checks (host or guest; each returns a string):

```lua
-- our police state as the glue reads it
local pol, p = game.police, game.game:GetActivePlayer()
local z = { pcall(pol.GetActiveZones, pol) }
local s = "zones " .. tostring(z[1]) .. " n=" .. (#z - 1)
if z[2] ~= nil then local ok, pos, lvl, zt = pcall(pol.GetZoneInfo, pol, z[2]); s = s .. string.format(" info %s %s %s %s", tostring(ok), tostring(pos), tostring(lvl), tostring(zt)) end
return s .. " seen=" .. tostring(select(2, pcall(p.IsSeenByPolice, p))) .. " off=" .. tostring(select(2, pcall(pol.AreOffencesDisabled, pol)))
```

```lua
-- host: make guest 1's puppet an offender and see what our police do (watch the log for "puppet got OFFENDER")
local pp = MafiaCoop.pup["1"]
return tostring(pcall(pp.so.AnnouncePoliceOffence, pp.so, enums.OffenceType.STREET_SHOOTING))
```

## 23. Explosions, fires and blown-up cars on every game (0.15.0) - static only, NOT verified in game

Found by reading the exe, its Lua binding thunks and the game's own scripts (`tables/scripts.sds`, HKS bytecode)
without running the game. Used by src/game/explosion_sync.lua (MafiaCoop.Boom), src/game/explosion_model.h and
docs/protocol.md "Explosions". Every call and event below needs the in-game check.

**How the game reports an explosion (on the game where it happens).**

| what | notes |
|---|---|
| `HumanMessages.HEAR_SOUND` (458774), `C_HearSoundMessage` | sent to every human that hears a sound (the puppets' "avoiding car" messages of section 11 were these). Getters: `GetSoundType()` (`enums.ActorSoundType`, 0x5A93E30: SHOT 5, EXPLOSIVE_BOUNCE 10, EXPLOSION 11, BURNING_FIRE 12, ...), `GetSoundPos()` (a vector), `GetSoundOwnerGUID()`, `GetSecondEntityGUID()`. One blanket HUMAN subscription gets them all; the glue drops everything but types 10-12 at once and the copies of one sound (same type and metre cell within 0.5 s). Who the owner of an explosion's sound is (the thrower, the projectile, nobody) is NOT known; it is logged for the first 40 explosions. The cost of this subscription (it also fires for every footstep and shot every human hears) is counted in the status line ("sounds N of M heard") |
| `HumanMessages.WEAPON_THROWN` (458830) | our player's throw (player_sync.lua counts it as action kind 5, section 17); the glue reads `p:InventoryGetSelectedExplosive()` (no arguments, thunk 0x23BE430; a name or an id, `GetWeaponNameFromID` then) right after: only `exp_*` names (and unreadable ones) count as an explosive throw |
| `HumanMessages.PLAYER_GRENADE_EXPLOSION` (458831) | by its name sent when the player's grenade explodes; to whom and with which message class is not known (the glue takes it when the receiver is our player and logs the sender) |
| DAMAGE type `EXPLOSION` (1) / `FIRE` (8) | `enums.DamageType`; who the enemy GUID is for a grenade's blast and for a molotov's fire is not known (fire may name nobody) |
| `CarMessages.CAR_HAVE_PROBLEM` (655365) + `m:GetCarProblem()` | the game's `common_vehicle_events.lua` (OnCarProblem) reads `GetCarProblem()` from this message; `enums.CarProblem`: EXPLODE 1, MOTOR_CRASH 2, POSITION 3, FUEL, TYRE_CRASH, AI_LOST. The glue subscribes for all cars (EventType.CAR) |
| entity ids | the low byte of an entity id is its type (section 15: 8 human, 9 player, 10 car); 12 = crash object as in the `ActorMessages` entity type enum (UNVERIFIED for ids). A crash object (a barrel) has its scene name (`GetName()`), the same on every game: `game.entitywrapper:GetEntityByName(name)` finds it there |

**A visual-only explosion.**

| call | notes |
|---|---|
| `game.shotmgr:CreateScriptExplosion(entity, pos, weapon, damage, stun, fullDamageRange, damageRange, n, createFire)` | thunk 0x239D8F0: adaptor with 9 arguments, then #2 an entity wrapper (fetched with 0x1ABD980, which returns null for nil/none), #3 vector, #4 string, #5 number, #6 bool, #7 #8 #9 numbers, #10 bool; native 0x23E59E0. **Argument order** from the game's own `CreateScriptExplosion` nodes in `game.lua` (their HKS bytecode: the parameters nodeID, inWeapon, inPositionEntity, inPositionVector, inDamage, inStun, inDamageRange, inFullDamageRange, inIgnoreColl, inCreateFire are R0..R9 and the call moves R2, R3, R1, R4, R5, **R7, R6**, R8, R9 into the argument slots): entity, position, weapon, damage, stun (bool), **full damage range, then damage range**, the "ignore collision" number (the node variants without that input pass the constant **1.5**), create fire. The entity is both the position anchor and the explosion's owner: for an entity of one type (the check against 0xC) the vector is relative to it, for two other types (2, 4) the entity's own position is used, nil = no owner and the vector is absolute. 0x23E5740 turns the weapon name into its id (0x23EA260), builds the explosion descriptor (position, weapon id, owner twice, the owner's GUID, damage, stun, the two ranges, the 9th number, create fire, a constant 300/50/10 block) and hands it to the explosion manager (0x26A1FB0 -> 0x269CEE0). **Used** for every picture: `(nil, pos, weapon, 0, false, 0, 0, 1.5, fire)`. UNVERIFIED: that damage 0 with both ranges 0 hurts nobody and still shows the weapon's effect and sound, that physics (cars, ragdolls, glass) is not pushed, that `createFire` makes a molotov's fire |
| `game.shotmgr:CreateExplosion(pos, weapon)` | thunk 0x239CCE0 (vector, string); the game's "Grenade" node calls it with `"Grenade"`. A real explosion with the weapon's damage: not used |
| `game.shotmgr:ScriptSetExplosionsEnabled(b)` | thunk 0x239E1F0; the nodes explosionsEnable / explosionsDisable. Global: not used |
| `game.game:ThrowProjectileWeapon(weapon, fromVec, toVec, angle, owner)` | thunk 0x267E4C0 (string, vector, vector, number, entity wrapper); node inputs inWeapon, inEntity, inStartPos, inEndEnt, inEndPos, inThrowAngle, "entity not created" on failure. A real projectile that explodes with real damage, so it is **not** used for the receivers' picture (the flight is drawn by our overlay instead, "The flight" below). The node returns the projectile entity ("entity not created" on failure), so projectile entities have script wrappers [likely]. `MafiaCoop.Boom.TestThrow()` calls it in front of us for a signature check |
| particles | `C_WrapperParticle` (Run, Stop, StopImmediately) works on particle entities placed in a scene; no script call spawns an effect by name |

**Cars.**

| call | notes |
|---|---|
| `car:Explode()` / `car:SetExploded()` | thunks 0x239B550 / 0x239BCC0; both call 0x2540380(car, t, true): Explode with a tunable value (read from a config once), SetExploded with **-1**. In 0x2540380 a positive t arms a timer (the car blows up t later); t <= 0 sets the wrecked state at once, and only t >= 0 runs the block that builds an explosion at the car (the blast with its damage): so SetExploded is the wreck **without the blast** [likely, from the code]. The first test there (car+0x1280, bit 16) returns at once; probably the DisableExplosion flag [guess], so the glue calls `DisableExplosion(false)` before `SetExploded()` on copies. Used: a receiver's copy of a car that blew up on its driver's game, and a hidden real car that comes back wrecked (CarParked, section 15; before 0.15.0 that was `Explode()`, a second real blast) |
| `car:DisableExplosion(true)` | copies (player car copies, traffic copies) keep it: a copy never blows up by itself; so a remote player's grenade cannot wreck a car the other game drives (not carried) |

**The damage guard** (explosion_model.h Judge; player_sync.lua M.OnDamageEvent asks `MafiaCoop.Boom.OnDamage` first
for every DAMAGE): explosion or fire damage inside a remote zone (where another game's explosion is shown, marked
right before the picture) whose attacker is nobody, a puppet, a spawned NPC copy or something that is not a human of
ours is undone: the local player through M.CopyUndoTick (health back to what it was at the end of the last tick,
lowered by whatever a real Hit took meanwhile), an NPC by writing back the health it had just before the picture
(`h.health` is writable on NPCs, section 11; the healths of NPCs within 20 m are remembered when the picture goes off,
and anew after any real hit on them). Inside an own zone (where our explosion went off) such damage without a named
attacker counts as our player's (`M.boomOurs` makes M.IsLocalAttacker say yes for that one dispatch), so puppets and
NPC copies hurt by our fire still send Hit / NpcHit. FIRE damage on puppets and NPC copies is summed over 0.5 s into
one hit (it comes in many small steps, UNVERIFIED how many).

**Dev console** (`tools\devlua.ps1`): `return MafiaCoop.Boom.Test()` shows a picture 8 m in front of us
(`Test(x, y, z, "exp_molotov", true)` for a fire); watch whether it is seen and heard, whether the player and NPCs near
it take damage (the log says "another game's explosion hurt us here: given back" if so) and whether cars get pushed.
`return MafiaCoop.Boom.TestThrow()` checks ThrowProjectileWeapon's signature (a real grenade: step back).

**The flight (grenade arc)** - static only, NOT verified in game. Used by src/game/throw_arc.lua (MafiaCoop.Arc),
src/game/throw_model.h and docs/protocol.md "Explosions" (Throw 0x0242). The thrower's game measures the real
projectile's launch in its first 0.3 s (or sends the aim model's guess) and sends it once; every other game
simulates the flight itself against its own world with `lua::RayCast` (the physics ray cast behind `mc_raycast`,
src/game/lua.cpp: one segment per script tick per flight, a probe on a hit; bounce 35 %, friction 25 %, rolling,
settling) until the Explosion naming the throw arrives. The thrower keeps following its projectile cheaply (a
position read every 0.2 s and at each of our EXPLOSIVE_BOUNCE sounds, which explosion_sync.lua forwards) and sends a
25-byte ThrowFix (0x0243) at each bounce, sharp turn, when it settles and when it is gone (at most 8): the receivers
re-anchor their simulation there and slide the marker over 0.1 s. The simulation's numbers are ours, not the game's
grenade physics (UNVERIFIED: compare the GE `off` between the simulated and the true explosion position, and the GX
`jump` of each applied fix).

| what | notes |
|---|---|
| projectile entities | the native entity type enum (its name table is registered at exe 0x176E5C0..: `E_ENTITY_<name>` strings 0x4CC9240.. with their values): **GRENADE 25**, **MOLOTOV 26**, BED 87, **PROJECTILE_EXPLOSIVE 88**, **SATCHEL_CHARGE 90** (LAST_ID 91; the script enum `ActorMessages` has GRENADE = 25 too). Script wrapper classes `C_WrapperEntityGrenade`, `C_WrapperEntityMolotov`, `C_WrapperEntityProjectileExplosive`, `C_WrapperEntitySatchel`, all `: C_WrapperActor` with no methods of their own (GetPos, GetDir, SetPos, IsActive, Activate/Deactivate from `C_WrapperEntity`/`C_WrapperEntityPos`). Which type a player's thrown grenade / molotov gets, and whether it has a script wrapper while it flies (mc_entity needs one: entity +0xA8), is NOT known: `MafiaCoop.Arc.Probe()` right after a throw lists what exists |
| finding them | `mc_entity_ids_of("25 26 88 90")` (src/game/lua.cpp): one walk over the entity table for several types. The table lookup (exe 0x233AC10) shifts the id right by 8 and ignores the type byte, so one walk sees every type; the low byte of the found entity's id (+0x18) is its type. The glue takes an entity within 8 m of the hand that is new since the aim started (or moved 0.3 m: a pooled one), every 0.1 s for 1.2 s after WEAPON_THROWN |
| the aim | `HumanMessages.PLAYER_AIM_ENTER` 458824 / `PLAYER_AIM_LEAVE` 458825 (receiver = our player; also sent for guns, so only counted with no gun in hand and an explosive selected), the aim input (RMB / left trigger, player_sync.lua `M.aimHeld`), `p:InventoryGetSelected()` starting with `exp_` (the explosive in hand). Controls `E_CTRL_WEAPON_GRENADE_AIM` / `_CANCEL` exist (and vehicle variants), so throwing may have an aim of its own; which of the three signals fires is logged (`how` in GA) |
| the game's arc | the arc the HUD shows while aiming is a mesh built by `C_CharacterStateHandlerGrenade::C_GrenadeArcJob` (strings `GrenadeArc`, `GrenadeArcVB_%llu`, `GrenadeArcIB_%llu`, `GrenadeArcMat`, `GrenadeTargetDecal%u`): not readable from script. The throw's numbers come from data: player tunables `grenadepitch`, `grenadespeed` (and `...car`, `...boat`, `...cargospace`, `...tap...` variants, `throwyawtospeedmult...`) parsed into curves by exe 0x2B2B2A0, and the projectile tables (`S_ProjectileWeaponSettings`: `m_ThrowVelocity`, `m_ProjectileModelName`, `m_GrenadePhysicsConfig`, `GetProjectileSettingsGRD`). Not extracted: so the aim model's speed (16 m/s) and pitch-up (15 deg) are guesses, and the real projectile's launch is **measured** instead (the first two positions after the release; the gravity from three, logged in GE) |
| hand | `h:GetBonePos(name)` answers the entity's own position for a name it does not know; the glue tries `RightHand`, `R_Hand`, `Bip01 R Hand`, `Bip01_R_Hand`, `hand_r`, `r_hand`, `RHand`, `Hand_R`, `RightHandMiddle1` once and keeps the first that answers something else (logged "hand bone: ..."), else the head + 0.3 m right, 0.25 m forward |
| a visible object on the other games | nothing harmless that can be spawned and moved was found: `ThrowProjectileWeapon` makes a real, damaging projectile; `CreateScriptExplosion` bursts along the arc would be loud and make NPCs react; `objectduplicator:DuplicateSceneObject` crashes (section 3); spawner components make humans; `CreateCleanEntity` is a cleaner zone (node inputs radius, block pedestrians / traffic); `Register3DIcon(iconObject, target)` and the HUD's `AddEntityIndicator` need an existing object; particles exist only as scene-placed `C_WrapperParticle` entities; the weapon list (`/xbin/human_items.xlaybin`: `exp_grenade` model `grenad`, `exp_molotov` model `wep_molotov`, `car_explosion`, `car_bomb`, `script_explosive`, `script_flame`, melee weapons) has no harmless thrown item. **Used:** our overlay draws a marker (grenade olive, molotov orange, dynamite red) and a fading trail at `ProjectPointToScreen` of the simulated positions (drawn over walls, like a HUD marker). **Next step for authenticity:** a real grenade model, put into the thrower puppet's hand with ModelToHands and then moved along the simulated path; no moveable harmless model entity was found statically (the list above), so only the hand part can be tried today: `h:ModelToHands(n, rightItem, leftItem)` (thunk 0x23B1BE0 -> 0x23EBA90: both names go through the weapon-id lookup 0x23EA260, then the human's +0x148 part, 0x2BB88A0, keeps them at +0x194 / +0x198; n < 0 does nothing): a grenade model in the thrower puppet's hand during his throw animation; `MafiaCoop.Arc.TestHand()` tries it on a puppet |

Dev console: `return MafiaCoop.Arc.TestFlight()` (in a session) simulates and shows a made-up throw of "player 254" from
2 m in front of us with the marker and trail (watch it bounce and settle); `MafiaCoop.Arc.TestEnd()` ends it as an explosion would; `return MafiaCoop.Arc.Probe()`
right after throwing a grenade lists the projectile entities (id, type, distance).

Not carried (known): NPCs' throws (only their explosions); fire spreading (each
picture's fire burns by itself on its game); mission dynamite and scripted car bombs (each game's own mission sets
them off).

## 24. Car damage on every game - static only, NOT verified in game

Found by reading the exe (docs/re-notes.md 11) without running the game. Used by src/game/car_damage_sync.cpp,
car_damage.lua and the game-free rules in src/game/car_damage.h (unit-tested in tests/car_damage_test.cpp); messages
in docs/protocol.md "Car damage". Everything below needs the in-game check.

**What the script API has** (C_WrapperCar, docs/re-notes.md appendix):

| call | notes |
|---|---|
| `car:GetDamage()` | **0..100 percent**: the weighted damage of the car's deform parts and doors, and **exactly 100 once the car has exploded** (re-notes 11). So "destroyed = GetDamage >= 100" (CarParked, section 15) is right |
| `car:GetMotorDamage()` / `SetMotorDamage(x)` | 0..100: the engine part's (type 13) lost energy; 100 when exploded. Set goes through the same part energy setter the collision code uses |
| `car:GetPartCount()` | the number of deform parts (entity + 0x100). There is no per-part getter or setter on cars: `GetPartEnergy` / `SetPartEnergy` / `GetPartVersion` are `C_WrapperActorDeform`'s (crash objects), on the same kind of part list |
| `car:Repair()` / `RepairAndClear()` | repair (and clean). Used on a copy whose car was repaired on the driver's game |
| `car:Explode()` / `SetExploded()` | Explode: the car catches fire and blows up after the game's delay; SetExploded: at once |
| `car:DisableExplosion(b)` / `IsDisableExplosion()` | entity + 0x1EF0 bit 0x1000000; copies have it on (they must not blow up on one game only). Whether it also stops Explode / the native explosion is not known |
| `car:SetCarDamage(x)` | random damage (its argument is not used): parts, lights, glass, doors, a tyre unless `SetWheelsProtected(true)`. Not used |
| `car:SetTyre` / `SetWheel` / `GetWheel` | tyre and rim models (tuning), not damage |
| `Get/SetBrakeDamage`, `Get/SetHandbrakeDamage` | not carried |
| `C_EntityMessageVehicleDamaged` | a car's damage event (type, incoming, final, attacker, local position); not used |

There is no script call for a single part, a dent, a tyre burst or a lost wheel, so the dll does those itself with the
game's own functions (re-notes 11): the **part energy setter** (what SetMotorDamage, the collisions and bullets
call: it plays the part's effects, swaps its damage model when its stage changes - dented, broken glass, a broken
light, a bumper, door or hood hanging or off - and drives its morph deformation), the **tyre burst** and **wheel
off** functions (what SetCarDamage's random damage and the exploded-car code call), and the **explosion** function
(with a delay > 0 the car burns and blows up when the time is out: the fire; with -1 at once). The patterns'
bytes contain the offsets each one uses, and two more (GetMotorDamage's native, the game's loop over all wheels)
confirm the part list, the part fields, the exploded bit and the wheel layout, so a different build disables the feature instead of reading garbage (logged `damage: car layout
not confirmed`).

**One car's damage as a snapshot** (`car_damage::Snapshot`): per deform part 0 intact .. 254 no energy left (1 -
energy / maximum, quantized) or 255 gone; per wheel ok / tyre burst / wheel off; exploded; burning with the seconds
left. Same model = same part list: a layout hash of the part types tells whether two cars can exchange part indexes
(if not, only tyres, the fire and the explosion are synced, logged once per car).

**Who holds a car's damage** (the authority): a player car's driver's game, and while it is parked its last driver's
(the car key's first driver if nobody drove it in this session); the host for his traffic. Every game reads its cars
4 times a second:

- **Authority:** a change (a part by 2 quanta or more, a tyre, the fire, the explosion) goes to everyone at once as
  a CarDamage with only the parts that changed; a damaged car's whole state again every 15 s, and to anyone who
  asks (CarDamageRequest, sent by a game that has a new copy or entity for the car). A **repair** (a part 24 quanta
  or more better, a gone part back, a tyre back, the explosion undone: a garage, Repair by a script, a respawned car)
  moves the car's epoch on and sends the whole state.
- **Copy:** only ever **raises** its damage to the authority's (raise-only; a part the game will not take is not set
  again until the authority's rises further); a newer epoch makes it `Repair()` first and set the authority's damage
  0.6 s later. A copy's **own** damage (more than the authority's by more than 3 quanta: a guest shooting the host's
  car, ramming it on his game) is reported after 0.4 s (CarDamageReport); the authority puts it on the real car
  (raise-only) and its next change brings it back to everyone. While a copy is moved by SetPos (pinned / snapped
  by the car controller, the last second) its own part damage is **not** reported (those are bumps from being put in
  place, not real damage); once such damage adds up (30 quanta) the copy is repaired and set to the authority's
  again (at most every 20 s). Tyres and the fire of a copy are reported even then.
- **Echoes never come back** (0.15.0 one-PC test: `car:Explode()` on the host's car; the guest's copy was blown up
  through the one explode routine, its wreck - 3 wheels off - came back as a CarDamageReport, the host put it on his
  wreck (`damage another game's copy took is put on our car (0 parts, 3 wheels)`) and read the drop that followed as
  a repair (`repaired on our game (was ... exploded; now parts 18/33 ...)`), so every copy repaired itself: the
  explosion undid the host's damage). Only damage a copy really took on its own game is reported now:
  - a copy never reports anything while it or the holder's car is exploded (a wreck shows what the explosion did to
    it on this game; the explosion is the holder's), and never the exploded flag (a copy never blows up by itself);
  - for 1.5 s after we wrote cascading damage on a copy (an explosion, a fire, a wheel taken off, a part broken off)
    whatever new damage it shows is the game's reaction to our writes: taken as reported (logged `an echo of our own
    writes, not reported`, traced `CDX ... echo=wreck|write`), so only damage beyond it is ever sent;
  - the holder drops every report of a car that is exploded on its game and the exploded flag in any report (older
    builds' copies still send them; counted `echoesDropped`);
  - a car that stays exploded is never "repaired" (its wreck changes by itself), and for 1 s after the holder wrote
    damage on its own car (a report, another game's blast, a new entity) a drop is taken as the game's reaction, not
    a repair (logged `taken as the game's reaction, no repair`, traced `CDT kind=drop-after-write`). A real repair in
    that second is missed (the car's next change still goes out; copies only ever raise). UNVERIFIED that the game's
    own reactions all come within these windows.
  Tests: `TestExplosionEcho`, `TestWriteEcho` in tests/car_damage_test.cpp.
- **A new entity for a car** (our own car back from behind a copy, section 15 "Mission car back"; a fresh copy for
  a new driver): it gets the authority's damage at once; if it has damage the car does not (the driver had it
  repaired while ours was hidden), it is repaired first. The authority's own new entity gets what everyone was told.
- **The same parts:** a copy of another body variant has another part list (0.15.0 test: `our copy's parts differ
  from the authority's (... layout 7d84a644 vs fe918d3f): only tyres and the explosion are synced`, the copy was the
  open smith_v12, the car the closed one). The car look sync below gives the copy the car's tuning items (its body);
  the damage book reads the layout every round, so once the copy's body matches, its parts are synced too
  (UNVERIFIED that the tuning items make the part lists equal).
- **The fire and the explosion - one explosion, on the holder's game only.** A car blows up for real only on the
  game that holds its damage. On every other game (a copy, or a real car another game holds) it never blows up by
  itself:
  - its explosion is switched off (`DisableExplosion(true)`), re-asserted every round (4 times a second), after a
    `Repair()` and after parts were set on it (car_damage.lua `keepOff`; traffic copies too);
  - a copy of a burning car burns (the native explosion function with the holder's seconds left, at least 3 s: the
    flames show), but its countdown (entity + 0x1FB8) is held at 3 s or more every tick while it burns
    (car_damage.h `HoldFire`, car_damage_sync.cpp `HoldFireGuarded`), so the countdown never ends in a blast of its
    own. UNVERIFIED: that +0x1FB8 counts down to the blast and that raising it keeps the flames;
  - it blows up only when the holder's game says so: car damage's exploded flag (CarDamage) or the explosions sync's
    CarExploded (section 23), whichever arrives first, or a wrecked car of ours coming back (CarParked / car_swap's
    `restore_wrecked`, section 15). All of them call one routine, car_damage.lua `D.Explode`: it asks the dll's
    one-explosion book (`mc_car_explode_first(entity)`, car_damage.h `ExplodeOnce`: the first ask per entity goes
    through, every later one is refused; cleared with a new world, `mc_car_explode_reset`), then `DisableExplosion(false)`
    + `SetExploded()` (the wreck; never `Explode()`, which is a real blast with the game's countdown) and a visual-only
    picture of the blast with the damage guard's remote zone (explosion_sync.lua `B.Picture`; none when CarExploded's
    picture was shown for that car already). Traced `CDB` (the book's answer) and `CDE` (what was done).
  On the holder's game (the driver's, the last driver's while parked, the host for his traffic) the car blows up the
  game's own way; a player car copy we drive ourselves (we hold its damage now) has its explosion switched back on,
  and off again when another game holds it.
- **Another game's explosion on the cars we hold.** Cars stay destroyable by anyone's explosion: a blast on any game
  reaches the holder's game as Explosion (section 23; a damage-free picture there), and the holder puts the blast's
  effect on every car it holds within 10 m itself (explosion_sync.lua `B.OnRemote` / `B.OnRemoteCar` -> car_damage.lua
  `D.Blast` -> "BL" -> car_damage_sync.cpp `ApplyBlast`, the falloff in car_damage.h `BlastOn`): within 4 m the car
  explodes (native, after `ExplodeOnce`); 4..10 m every part takes up to 200 quanta, linear to 0 at 10 m; within 6 m
  every tyre bursts; within 5 m it catches fire (8 s to its own explosion); a molotov's fire (Explosion kind 2): within
  3 m it catches fire, parts up to 40 quanta, never an explosion. The game's own explosion damage curve was not found
  statically: these constants are guesses (UNVERIFIED). The usual CarDamage then brings it to every copy. Not when
  our own copy of an NPC threw it here (its blast was real on our game). Players and NPCs are not touched by this
  (their damage is decided once, by the sender's game). The copy on the game where the blast really happened takes
  its own part damage there (its explosion is off) and reports it 0.4 s later: the holder drops the copies' reports of
  that car for 1.5 s after it put a remote blast on it (`Book::OnRemoteBlast`), so the blast counts once (the holder's
  version). Traced `CDK`.
- **Traffic:** the host's traffic cars (traffic_sync, section 19) are named by his entity id on his game and by their
  net id on the wire and on the guests' games; a guest's copy is a copy like any other (it asks the host, reports
  its own damage to him). A traffic car a player takes over becomes a player car (its damage goes on with it: the
  entity keeps it).

**Logs** (main log, `damage: ...`): `car layout confirmed ...` at start; per car `its damage from our game`,
`damage changed on our game: parts 3/41 damaged (worst 47%, 0 gone), tyres 1 burst ...`, `damage set on our copy
(...)`, `car entity N: 2 of 2 parts set (0 refused by the game, 0 failed), 1 of 1 wheels, fire started / blown up`,
`our copy took damage of its own (...): reported to player N's game`, `damage another game's copy took is put on our
car`, `repaired on our game ...: epoch N`, `our copy is repaired (why ...)`, `our game holds its damage now`,
`its damage cannot be read (why)`, `an echo of our own writes, not reported`, `taken as the game's reaction, no
repair`. Sync trace: CDS, CDT, CDR, CDA, CDX, CDQ (docs/sync-trace.md);
`tools/sync_compare.py` lists them and each player car's last state on both games side by side.

**UNVERIFIED (all of it, static only):** the field offsets and flag meanings (part list, energy and maximum, gone
flag, wheels and their bits, exploded / burning bits, the fire timer); that the same model has the same part order
on two games (the layout hash guards the indexes, not the meaning); that setting a part's energy from outside a
collision shows the same dents and broken / hanging / lost parts (the morph deformation and the stage models come
from the energy, the free mesh deformation of a crash does not: a copy's dents are the part's morph targets, not the
exact shape); that the tyre and wheel functions are safe to call with r8 = false outside their callers; whether
`DisableExplosion` stops the native fire / explosion on a copy (read back and logged; Lua fallback for the
explosion); that a part the game refuses (inactive, already gone) is reported as refused, not crashed (guarded);
GetDamage's scale (read from the code: 100 = exploded) in a running game; which part type is which beyond the engine.

### Car look: a copy looks like the car - static only, NOT verified in game

0.15.0 one-PC test: the host drove a black closed smith_v12 sedan (plate ZK-299); the guest's copy was a light-blue
open smith_v12 (plate NY 858). A player car's copy was spawned with `ObtainSpecificCar(model)` and nothing else, so
the game picked its own variant, colours and plate (TrafficSpawn carried colours and plate for traffic copies only).
Now the game that has the car itself sends its look once (and on change) in CarLook (0x0253, docs/protocol.md "Car
look"), and every copy we spawned takes it. Code: src/game/car_look.{h,cpp} (wire, line form, diff, the Book; tests in
tests/car_look_test.cpp), src/game/car_look_sync.cpp (messages, the native tuning read), src/game/car_look.lua (which
cars, the reads and setters).

**What makes a car's look** (found statically, docs/re-notes.md 13):

| what | how it is read / set | notes |
|---|---|---|
| model | `car:GetVehicleModelName()` | the vehicles table row's name (entity + 0x1FA8 -> the row, its name at +0x14; `GetCarTableId()` is the row's id at +0). The vehicles table has **one row per model** (tables.sds: `smith_v12` once), so the variant is not a table row; `ObtainSpecificCar` takes only the model name (one argument) |
| look flags | `car:HasVehicleLookFlag({enums...})` | `C_WrapE_TrafficVehicleLookFlags` CONVERTIBLE_CLOSED, CONVERTIBLE_OPENED, HARDTOP, ROOFRACK, PICKUP, ... are read from the **table row** (row + 4, 16 bytes of flags), the same for every car of a model: what the model can be, not what this car is. Not used |
| body variant: tuning items | `mc_car_tuning(id)` (ours) / `car:InstallTuningItems(id, id, ...)` | **the per-car part**: entity + 0x1B20 is the car's tuning; +0xF0 / +0xF8 its slots (0x18 bytes: +0 the tuning table item installed there or null, +0x10 u16 the slot kind). The global tuning table: 0xD0-byte items, +0 u16 item id, +2 u16 slot kind. `InstallTuningItems` takes the item ids as separate numbers (variadic, read as u16) and puts each in the car's slot of its kind (exe+0x2586E90), then rebuilds the car (exe+0x256CC30). The traffic generator picks a car's tuning (`car_traffic_tuning` in tables.sds); `IsInstalledTuning(id)` reads one. That the open / closed roof is a tuning slot is **UNVERIFIED** (the strings `E_SE_CAR_CUSTOMIZATION_PURCHASE_ROOF`, `Car_No_Roof`, `CarRoof` point there) |
| colours | `car:GetColor(1)`, `GetColor(2)` / `car:SetColor(c1, c2)` | GetColor passes `slot - 1` to its native (exe+0x254C0C0): the slots are 1 and 2 (section 19's question answered). What the number is (a colour table index) is not known; `SetColor` takes two numbers (it also has a string form) |
| plate | `car:GetSPZText()` / `car:SetSPZText(text, true)` | a string; may hold a space (`NY 858`): escaped on the line form, never cut to letters and digits |
| window tint, interior | `GetWindowTint()` / `SetWindowTint(n)`, `GetInteriorColorsSet()` / `SetInteriorColorsSet(n)` | numbers |
| painting | `GetPainting()` / `SetPainting(s)` | a string (entity + 0x1FC0, or the game's default) |
| dirt, rust | `GetDirty()` (entity + 0xD58), `GetRust()` | floats of an unknown scale, and a car gets dirtier while it drives: **not carried** (always sent as unknown; the wire has the fields) |

**Who sends, who takes:** the game that has the car itself: a player car's first driver (the car key's owner: his
real car, also while it is hidden behind a copy because another player drives it), the host for his traffic (only to
a guest who asks: a guest's traffic copy asks once it is ready). Only copies we spawned (player_sync's copies,
traffic_sync's copies) ever take a look: a real car (ours, an adopted mission or traffic car) is never changed, and
the car's game never takes a look back, so a look cannot echo. A copy without a look asks every 3 s; a look that
does not take is tried 3 times (2 s apart), then given up and logged with what still differs; a new entity for the
car (a fresh copy) is tried again. A copy of another model (it should not happen: player_sync spawns the driver's
model) gets what can be set.

**Logs** (main log, `look: ...`): `car tuning layout confirmed` at start, per car `its look from our game (#n):
smith_v12 colours 3/7 plate 'ZK-299' ... tuning 12,40,77`, `player N's car K (#n): ...` (received), `its look set on
our copy (entity E, try n: colours, plate, tuning 1 item(s) 40; it had ...)`, `car entity E: colours, plate, tuning
40` (what the setters said), `our copy (entity E) shows its look` (the read back matches), `does not take its look
after 3 tries: still differs in ...`, `car entity E: tuning slots (slot:item) 3:12,5:40,9:77` (each car once). Sync
trace: CLS, CLR, CLQ, CLA, CLW, CLM, CLF (docs/sync-trace.md). The overlay: `Car looks: N cars (ours, copies: match,
waiting, not taken); tuning read`.

**UNVERIFIED (all of it):** the tuning layout (offsets above; the dll checks the install function's and the
InstallTuningItems loop's bytes, which hold them, and disables the tuning read otherwise: `car tuning layout not
found`); that the open / closed body (and the other visible variant parts) are tuning items; that `InstallTuningItems`
on a spawned car changes its body (and its deform part list: car damage's layout then matches); that `SetColor(c1,
c2)` with `GetColor`'s numbers gives the same paint; `SetSPZText`'s bool; whether a traffic copy's colours / tuning
are already what TrafficSpawn set (the copy is checked against the look either way).
