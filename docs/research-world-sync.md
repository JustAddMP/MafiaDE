# Research: world sync — damage (A5), weapons (A6), NPC mirroring (B3), traffic (B4), cutscenes (B5)

Static research only (2026-10-14). The game was not launched. Exe = Steam build, CRC32 3168979183. Same method as
`research-characters-missions.md`:
- disassembly of the Lua binding thunks (capstone);
- the game's own data, unpacked with its own `oo2core_8_win64.dll`: the 122 HKS Lua chunks in `tables\scripts.sds`,
  the mission SDS in `sds_retail\script`, and `tables\tables.sds`;
- the ScriptHook (MPlus) trainer scripts in the game folder, used as API reference only.

Clean-room: nothing from other multiplayer mods. Tags as in `re-notes.md`: **[confirmed]** means several independent
indicators, **[likely]** one strong indicator, **[guess]** inference only. Everything below is static until its
live test has run. RVAs are relative to the image base 0x140000000.

Scratch tooling (not checked in) is in `%TEMP%\claude\G--MafiaCoop\re3`. It reuses the re2 tools (`lib.py`, `reg.py`,
`sds.py`, the `lua\NNN_*.txt` constant dumps). Use `python.exe` directly: the pyenv shim mangles arguments with `|`.
- `enumlua.py`: Lua enum name -> `C_WrapE_*` class.
- `enummap.py` / `enums_dump.py` -> `enums.txt`: every enum table with its values.
- `classes.py`: every `Lua_RegisterClass` call (class : parent, method table, method names).
- `sig.py` / `sigf.py` -> `sigs.txt`: argument types of 1500 binding thunks (see "Method").
- `typechk.py`: thunks that check the entity type.
- `c.py`: context search in the Lua constant dumps.
- `rstr.py` / `rgrep.py`: raw string search by RVA.
- `csobj.py`: cutscene objects per mission SDS.
- `ad.py` / `xs.py` / `xr.py`: disassembly and xrefs.

---------------------------------------------------------------------------------------------------------------

## 0. Cross-cutting findings (read first)

1. **Lua enum names differ from the C++ class names.** [confirmed] The `enums.X` registration objects (vtable +
   `"enums"` + name) map as follows. The values come from the enum getters.

   | `enums.` | values |
   |---|---|
   | `EventType` (= `C_WrapE_EntityMessageType`) | BASEMESSAGE 0, SCRIPTWAKE 1, ACTOR 2, **HUMAN 3**, CAR 4, BOAT 5, DOOR 6, QUEST 8, AISTATE 9, FIRETARGET 10, SYNCOBJECTEVENT 11, TELEPHONE 12, ACTORDETECTOR 13, PINUP 14, SKIPSCRIPTSCENE 15, FSMEVENT 16, TAILENTITY 17, ENTITYSENSOR 18 |
   | `HumanMessages` (base 0x70000 = 458752) | ON_ACTIVATE 458753, ON_DEACTIVATE 458754, ON_SHOOT 458755, ON_BULLET_HIT 458756, ON_RELOAD 458757, ON_OWNER_CHANGE 458758, SEE_ENEMY 458759, LOST_ENEMY 458760, SEE_PLAYER 458761, **DAMAGE 458772**, STUN 458773, AIM 458775, **DEATH 458777**, PANIC_START/STOP 458781/2, SCRIPT_CONTROL 458783, AI_CONTROL 458784, ENTER/LEAVE_VEHICLE 458788/9 (+ _DONE 458790/1), MELEE_LIGHT_ATTACK 458809, MELEE_HEAVY_ATTACK 458810, MELEE_COUNTER_ATTACK 458811, MELEE_COMBAT_TAKEDOWN 458812, MELEE_STEALTH_TAKEDOWN 458813, MELEE_HIT_REACT 458814, MELEE_STUNNED 458815, MELEE_END 458816, TELEPORT 458844, AI_PERCEPTION_EVENT 458848, OFFENDER 458859, SHOT 458872, ATTACK 458878, ... (full list: `enums.txt` line `5AEDF10`) |
   | `ActorMessages` (base 0x40000) | DAMAGE 262145, ACTIVATE 262148, SCRIPT 262149, HITINFO 262154, ... |
   | `CarMessages` (base 0xA0000) | HEAR_SOUND 655361, CARCRASH 655362, **CAR_DAMAGED 655363**, CARCRASH_WITH 655374, PULLED_OVER 655378, DRIVING_EVENT 655379 |
   | `DamageType` | UNDEFINED -1, SHOT 0, EXPLOSION 1, MELEE 3, TAKEDOWN_SPRINT 4, TAKEDOWN_STEALTH 5, TAKEDOWN_STUN 6, TAKEDOWN_WEAPON 7, FIRE 8, **SCRIPT 9**, IN_CAR 10, COLLISION 11, FALL 12, CINEMATICDEADZONE 13, ANIMEVENT 14, PASSOUT 15 |
   | `Hitzone` (note the lower-case z) | LOWER_BODY 0, BODY 1, HEAD 2, LEFT_ARM 3, RIGHT_ARM 4, LEFT_LEG 5, RIGHT_LEG 6, HELMET 7 |
   | `EntityType` | HUMAN 8, PLAYER 9, CAR 10, CRASH_OBJECT 12, DOOR 21, CUTSCENE 43, BOAT 27, ITEM_WEAPON 63, ... |
   | `AI_TYPE` (= `HAIS_TYPE`, what `GetAIType()` returns) | CIVILIAN 0, FRIEND 1, ENEMY 2, POLICE 4, SCRIPTED 12, DEBUG 17 |
   | `AI_SIDE` (= `RelationshipSide`) | PLAYER 0, ENEMY 1, POLICE 2, NEUTRAL 3 |
   | `AI_Aggressivity` | SeeEnemy 0, SeeEnemyWithWeapon 1, HearGunshot 2, OnAttack 4 |
   | `AI_Courage` | Funk 0, Normal 1, Courageous 2 |
   | `InventorySlotType` | SMAL_WEP 0, BIG_WEP 1, MELEE_WEP 4, PLACED_EXPLOSIVES 6, GRENADES 7, MOLOTOVES 8, VOODOO 9, PROXIMITY_MINES 10, SNIPER_SERVICE 11, THROWING_KNIFE 12, COIN 13 |
   | `PhysicsState` | DYNAMIC 0, KINEMATIC 1, DISABLED 2 |
   | `PoliceEventType` (= `C_WrapE_EventType`!) | PLAYER_ENTER_BLUEZONE 1, ... NPC_KILLED 512, PLAYER_DEATH 131072, POLICE_ARREST_* ... (bit flags) |
   | `PoliceZoneType` | SEARCH 0, HUNT 1 |
   | `OffenceType` | SIDEWALK_DRIVING 0, FISTFIGHT 4, STREET_SHOOTING 7, CIVIL_KILL_ATTACK 18, DISCIPLINARY_KILL 19, SHOOT_AT_CIVIL 24, ... |

   So `player_sync.lua`'s `RegisterEvent(ctx, fn, enums.EventType.HUMAN, enums.HumanMessages.DAMAGE)` uses the right
   names (EventType.HUMAN = 3). The C++ class `C_WrapE_EventType` is exposed as `enums.PoliceEventType`.
2. **Global event subscription exists: `game.game:RegisterBlanketEvent(ctx, fn, eventType, message)`.**
   [confirmed from the thunk and from game scripts]
   - Thunk 0x2673CB0 has the same callback construction as `RegisterEvent` (0x3C04160). It reads #4 and #5 with
     0x23F2640 (to int) and returns an id, or -1 on failure. Undo it with `UnregisterBlanketEvent(id)`.
   - `game.lua` uses it to get **every human's DEATH**: `RegisterBlanketEvent(on_death, ..., EventType.HUMAN,
     HumanMessages.DEATH)`. Its handler recovers the human with
     `game.entitywrapper:GetEntityByGUID(event:GetReceiverGUID())`.
   - A test node uses it for every `ON_SHOOT`.
   - This is the basis for host-side NPC observation (B3) and global damage/death tracking (A5).
3. **`game.entitywrapper:GetEntityByGUID(guid)` works with a `C_EntityGuid`.** [confirmed] The thunk 0x239E510 reads
   the argument with 0x170E430 (checks the `C_EntityGuid` metatable, then reads the u32 id). A `C_EntityGuid` comes
   from `entity:GetGUID()` or from message getters (`GetEnemyGUID`, `GetKillerGUID`, `GetReceiverGUID`,
   `GetSenderGUID`, `GetOpponentGUID`, ...). A Lua number never works (game-api.md 3 only tried numbers).
   - The u32 inside a GUID is the entity id. **Its low byte is the entity type**: thunks test
     `cmp byte [entity+0x18], 9` (9 = PLAYER).
4. **Bound message classes and their getters** (from all `Lua_RegisterClass(..., "C_EntityMessage", table)` calls).
   [confirmed] The getters are what an event handler `fn(ctx, msg)` can read:
   - `C_EntityMessage` (base of all): GetType, GetMessage, HasName, GetName, GetSenderGUID, GetReceiverGUID, Clone.
   - `C_DamageMessage`: GetEnemyGUID, GetWeaponID, GetDamageType, GetDamageAmount, GetHitZone, GetCanHeadShot.
   - `C_HumanMessageDeath`: GetKillerGUID, GetDamageType, GetWeaponID, GetHitZone, WasStunned, WasReloading.
   - `C_HumanMessageOnBulletHit`: **GetHitObject, GetPartIndex**. Likely sent to the *shooter* (ON_BULLET_HIT).
   - `C_HumanMessageShoot`: GetWeaponID, GetWeaponName (ON_SHOOT).
   - `C_ShotMessage`: GetShotPos. `C_ShotEnemyMessage`: no getters.
   - `C_SeeEnemyMessage`: GetEnemyGUID. `C_AimMessage`: GetAimerGUID, GetState (`enums.AimMessageState` BEGIN 0,
     CHANGE_HIT_ZONE 1, END 2).
   - `C_HumanMessageMelee{LightAttack, HeavyAttack, CounterAttack, CombatTakedown, StealthTakedown, HitReact,
     Stunned}`: GetOpponentGUID.
   - `C_HumanMessageInjuryStun`: GetAgonyStunFlag.
   - `C_AIPerceptionEventMessage`: GetEvent, GetPosition. `C_AIControlMessage`: GetSelectorName, GetObjectName,
     GetReason. `C_ScriptControlMessage`: GetState, GetObjectName.
   - `C_HumanMessageOffenceDetected`: GetOffenderPlayer.
   - `C_EntityMessageVehicleDamaged`: GetDamageType, GetDamageIncoming, GetDamageFinal, GetAttackerGUID,
     GetLocalPos. `C_EntityMessageVehicleCrash`: GetCrashSpeed. `C_EntityMessageVehicleCrashWith`:
     GetSecondEntityGUID, GetCrashSpeed. `C_EntityMessageCarDrivingEvent`: GetEvent.
   - `C_FireTargetMessageHit`: GetHitPos, GetHitNormal.

   Which HumanMessages value delivers which class is inferred from the names [likely]. Test A5-1 prints the getters
   that succeed.
5. **Reflected component methods** (from the strings next to the class name, and from the registration code that
   stores `{vtable, owner, "name", memberFn}`):
   - `C_SceneObjectLuaWrapper` (any scene object, including humans): Show, Hide, GetName, GetTransform, AddComponent,
     GetComponent, **GetSubobjectsWithComponent(compName)**, **GetSubobjectsCount()**, **GetSubobjectByIndex(i)**,
     **GetSubobjectByName(name)**, **GetSubobjectsWithNamePrefix(prefix)**, HasTag, HasTagWithValue,
     GetSingleTagValue, AttachObject, DetachObject, GetGroupObjects, ...
     The game wraps the list returns with `table.c2lua(...)`, e.g.
     `table.c2lua(obj:GetSubobjectsWithComponent("C_RuntimeInteractionComponent"))`.
   - `C_RuntimeSpawnerComponent`:
     - CreateObject (member 0x2D8B090), CreateObjectAsync (0x2D8B100), ReleaseObjectRaw, ReleaseAllObjects,
       GetSpawnProfileLoadSyncObject, TransferObjectToTrafficRaw, Set/GetTransferActorsToTrafficOnDespawn;
     - inherited from `C_SpawnerComponent`: SetSpawnProfile, GetSpawnProfile, Set/GetShutdownBehavior,
       Set/GetSpawnOnAddToScene, SpawnOnActivate, AddTag, RemoveTag, ...
     - The async result is `Wait(so)` then `so:GetUserData():GetCreatedObject()` (`C_SpawnerUserData`).
   - `C_SpawnerDataComponent`: SwitchDataSet, SwitchDataSetString(name), GetCurrentDataSetName.
   - `C_RuntimeProgressionSpawnComponent`: UpdateSpawners (the mission wave spawners).
   - `C_RuntimeCutsceneComponent`: GetCutsceneId, Set/GetPlayOption, LoadCutscene, UnloadCutscene,
     IsCutsceneLoadStarted, IsCutsceneLoadFinished, GetVisualStartSyncObject, PlayCutscene, PlayCutscene2,
     StopCutscene, IsCutscenePlaybackInProgress, AllowPlayerCamera.
6. **Global module names** seen in the game scripts:
   - `game.shotmgr` (C_WrapperShotManager), `game.battle`, `game.physics`, `game.police`, `game.traffic`,
     `game.entitywrapper`, `game.skipscriptscene`;
   - the **globals** `relationships` (C_RelationShipsScriptWrapper) and `wingmanmanager`.

   [likely: confirm with A5-0]

---------------------------------------------------------------------------------------------------------------

## A5. Damage, death, invulnerability, hits

**Live-tested 2026-10-15** (A5-0 to A5-6): results in game-api.md 11. Confirmed: the enum values, RegisterBlanketEvent,
the DAMAGE getters, `DoDamage` = SCRIPT damage with a flinch, `CreateShots` from an NPC hurting the player with a real
hit reaction (but unreliable per game instance), car rams as COLLISION damage with the driver as enemy. Refuted: the
globals `relationships` and `wingmanmanager` (nil), `ApplyRagdollRadialSpeed` on the player (no effect), `AttackMelee`
from a puppet, melee DAMAGE messages (a punch sends none).

### Summary

- **Observe**: per human with `h:RegisterEvent(ctx, fn, enums.EventType.HUMAN, enums.HumanMessages.DAMAGE | DEATH)`
  (what `player_sync.lua` already does), or for all humans at once with `game.game:RegisterBlanketEvent(ctx, fn,
  EventType.HUMAN, HumanMessages.DAMAGE | DEATH)`. The handler gets `(ctx, msg)`. `msg:GetReceiverGUID()` is the
  victim; `GetEnemyGUID` / `GetKillerGUID` is the attacker. Cars: `EventType.CAR`, `CarMessages.CAR_DAMAGED`
  (`GetAttackerGUID`, `GetDamageFinal`) for rams and shots on cars.
- **Apply, the simple way**: `h:DoDamage(amount)` takes **one number only**. Native 0x23E6F10 builds a damage
  message with **type SCRIPT (9), attacker = the victim itself**, at the victim's own position and direction, and
  sends it to itself. There is no hit zone, no direction and no weapon, so it probably gives at most a generic flinch
  or a death, not the bullet or punch reaction [likely].
- **Apply, the real way**: **`game.shotmgr:CreateShots(shooter, originVec, targetVec, weaponName, scatter, timeout,
  particle, sound)`** (8 args; thunk 0x23A0EA0 -> 0x23D74A0). It returns a sync object. The game's `CreateShots`
  node (`game.lua`, inputs inEntity inOrigin inTarget inWeapon inScatter inTimeout inParticle inSound) does
  `Wait(game.shotmgr:CreateShots(...))`.
  - It fires real bullets owned by `shooter`. The victim should get a SHOT damage message with
    `GetEnemyGUID() == shooter`, the hit zone and the reaction or ragdoll for where the bullet lands, and its AI
    sees `shooter` as the attacker [likely].
  - An 11-argument overload (0x23D7730) takes (shooter, originEntity, originOffset, targetEntity, targetOffset,
    weapon, scatter, timeout, num, bool, bool). It aims at an entity plus an offset (e.g. a bone height).
  - Explosions: `game.shotmgr:CreateExplosion(pos, weaponName)` (0x239CCE0), and `CreateScriptExplosion(...)` with
    damage, stun and range parameters.
- **Ragdoll and knock-down**: `h:ActivateRagdoll(bool)`, `h:ApplyRagdollRadialSpeed(centerVec, speed, radius)`,
  `h:SetFullBodyInjuryReactionThreshold(n)`, `h:SetCustomDeathAnimation(name)` / `ClearCustomDeathAnimation()`,
  `h:SetNoRagdollNoAnimAtDeath()`, `h:KillAndThrowNPCOutOfCar()`.
  - Melee: `h:AttackMelee(target)` makes an AI human start a real fist fight with `target`. A guest's punch could
    be replayed on the host by its puppet [guess].
- **Detect hits on a specific human (puppet hits)**:
  - On the victim: DAMAGE (`C_DamageMessage`): GetEnemyGUID, GetDamageType (SHOT/MELEE/IN_CAR/COLLISION/...),
    GetHitZone, GetWeaponID (numeric, `game.game:GetWeaponNameFromID(id)` gives the name), GetDamageAmount.
    Already used by `M.OnPuppetDamage`.
  - On the attacker (the local player): `ON_BULLET_HIT` (`C_HumanMessageOnBulletHit:GetHitObject()`) and
    MELEE_LIGHT/HEAVY_ATTACK (`GetOpponentGUID`). These give "my shot or punch hit X" without watching every copy
    [guess].
  - Victim-side melee: `MELEE_HIT_REACT` / `MELEE_STUNNED` (GetOpponentGUID).
- **Invulnerability**: human properties (getter/setter pairs at table 0x5AD44B0) [confirmed]:
  - `invulnerability` (bool), `health`, `healthrel`, `healthmax`, `healthmaxvar`, `healthfall`, `healthrestore`,
    `healthrestoredown`, `shield`;
  - **`nonplayerdamage`** (number; the setter 0x2BA4360 stores `value * const` at AI+0x1E8, so it is probably a
    percent multiplier for damage from non-player attackers), `nonplayerdamagedist`.
  - Methods: `SetDemigod(bool)`, `EnableInjury(bool)`, `EnableInjuryDiff(bool)`, `SwitchHealthSystem(n)`,
    `SetMaxHealth()` (no args: heal to max, as `game.lua` uses it after a respawn) [likely], `HealByMedkit()`,
    `GiveArmor(n)`, `GiveFullArmor()`, `RemoveAllArmor()`, `ReviveDownedHuman()`.
  - The game's own nodes `invulnerability(nodeID, inEntity, invul)` and `nonplayerdamage(nodeID, inEntity, damage)`
    (`common_human.lua`) set exactly these properties.
- **Player death -> checkpoint reload is not Lua.** The C++ game-over manager runs the flow in
  `script\game_over_manager.sds` (strings `mm_000_go_death`, `mm_000_go_arrest`, `mission_failed`,
  `MS:MissionFailed`, `global_mission_disengage`). That shows the fail screen; Retry is `game.savesystem:Retry()`
  (B2 notes).
  - Scripts that need to know: `RegisterEvent(... HUMAN, DEATH)` on the player (test-node `OnEventPlayerDeath`),
    and the police event flag `PoliceEventType.PLAYER_DEATH` via `game.police:RegisterForPoliceEvent`.
  - For guests the existing "downed instead of dead" approach (`M.DownTick`: low health -> demigod + LockControls ->
    HealByMedkit) avoids the fail screen. A cleaner guard is `p.nonplayerdamage = 0` on guests (see A5-5).

### Candidate APIs

| # | call | args (thunk) | notes / confidence |
|---|---|---|---|
| 1 | `h:RegisterEvent(ctx, fn, et, msg)` | 0x239FB40; #4/#5 numbers | returns an id (>= 0). Undo with `UnregisterEvent(id)`. [confirmed by game scripts] |
| 2 | `game.game:RegisterBlanketEvent(ctx, fn, et, msg)` / `UnregisterBlanketEvent(id)` | 0x2673CB0 | all entities. [confirmed] |
| 3 | `h:DoDamage(n)` | 0x23C5EC0 -> 0x23E6F10 | SCRIPT damage, self as attacker. [confirmed from code] |
| 4 | `game.shotmgr:CreateShots(shooter, from, to, weapon, scatter, timeout, particle, sound)` | 0x23D74A0 | real bullets; returns a sync object. [likely] |
| 4b | `game.shotmgr:CreateShots(shooter, fromEnt, fromOfs, toEnt, toOfs, weapon, scatter, timeout, n, b, b)` | 0x23D7730 | entity-relative version. [likely] |
| 5 | `game.shotmgr:CreateExplosion(pos, weapon)` / `CreateScriptExplosion(...)` / `ScriptSetExplosionsEnabled(b)` | 0x239CCE0 / 0x239D8F0 | |
| 6 | `h.invulnerability`, `h.health`, `h.healthmax`, `h.nonplayerdamage`, ... | properties | the setters run through the AI component (`[[h+0x10]+0x130]`) |
| 7 | `h:SetDemigod(b)`, `h:EnableInjury(b)`, `h:EnableInjuryDiff(b)` | bool #2 | game-api.md 5 |
| 8 | `h:ActivateRagdoll(b)`, `h:ApplyRagdollRadialSpeed(v, n, n)`, `h:SetFullBodyInjuryReactionThreshold(n)` | | knock-down / push |
| 9 | `h:AttackMelee(target)`, `h:Attack(target [, b])` | wrapper #2 | AI attack orders |
| 10 | `h:IsDeath()`, `h:IsStunned()`, `h.health` | | state |
| 11 | `car` CAR_DAMAGED: `GetAttackerGUID()`, `GetDamageFinal()`; `car:SetCarDamage(n)`, `car:Explode()`, `car:SetExploded()` | | rams and car damage |
| 12 | `game.game:GetWeaponNameFromID(id)`, `GetItemIDFromName(name)`, `GetIsMeleeWeapon(name, b)`, `GetAmmoCategory(name)` | | weapon ids from damage messages |

### Live tests (dev console; run each block on its own, each returns a string)

```lua
-- A5-0: module names and the enum values used below
local s = {}
for _, k in ipairs({ "shotmgr", "battle", "physics", "police", "traffic", "entitywrapper", "skipscriptscene" }) do
  s[#s + 1] = k .. "=" .. tostring(game[k])
end
s[#s + 1] = "relationships=" .. tostring(relationships) .. " wingmanmanager=" .. tostring(wingmanmanager)
s[#s + 1] = "EventType.HUMAN=" .. tostring(enums.EventType.HUMAN) .. " DAMAGE=" .. tostring(enums.HumanMessages.DAMAGE)
  .. " DEATH=" .. tostring(enums.HumanMessages.DEATH) .. " ON_BULLET_HIT=" .. tostring(enums.HumanMessages.ON_BULLET_HIT)
  .. " AI_SIDE.ENEMY=" .. tostring(enums.AI_SIDE and enums.AI_SIDE.ENEMY) .. " AI_TYPE.ENEMY=" .. tostring(enums.AI_TYPE and enums.AI_TYPE.ENEMY)
  .. " DamageType.SHOT=" .. tostring(enums.DamageType.SHOT) .. " Hitzone.HEAD=" .. tostring(enums.Hitzone and enums.Hitzone.HEAD)
return table.concat(s, " | ")
```

```lua
-- A5-1: global damage/death/bullet-hit log. Then shoot, punch and run over some NPCs and read the log.
-- dump(m) tries every known getter and prints the ones that work, so the message class is visible.
local G = { "GetType", "GetMessage", "GetReceiverGUID", "GetSenderGUID", "GetEnemyGUID", "GetKillerGUID",
  "GetDamageType", "GetDamageAmount", "GetHitZone", "GetWeaponID", "GetCanHeadShot", "GetHitObject", "GetPartIndex",
  "GetOpponentGUID", "GetWeaponName", "GetShotPos" }
local function dump(m)
  local o = {}
  for _, g in ipairs(G) do
    local f = m[g]
    if f then local ok, v = pcall(f, m); if ok then o[#o + 1] = g .. "=" .. tostring(v) end end
  end
  return table.concat(o, " ")
end
MafiaCoop.a5ids = MafiaCoop.a5ids or {}
for _, name in ipairs({ "DAMAGE", "DEATH", "ON_BULLET_HIT", "MELEE_HIT_REACT", "MELEE_LIGHT_ATTACK", "ON_SHOOT" }) do
  local id = game.game:RegisterBlanketEvent({ n = name }, function(ctx, m)
    local ok, e = pcall(function()
      local who = game.entitywrapper:GetEntityByGUID(m:GetReceiverGUID())
      mc_log("A5-1 " .. ctx.n .. " on " .. tostring(who) .. ": " .. dump(m))
    end)
    if not ok then mc_log("A5-1 handler error " .. tostring(e)) end
  end, enums.EventType.HUMAN, enums.HumanMessages[name])
  MafiaCoop.a5ids[#MafiaCoop.a5ids + 1] = id
end
return "registered " .. table.concat(MafiaCoop.a5ids, ",")
-- clean up later: for _, id in ipairs(MafiaCoop.a5ids) do game.game:UnregisterBlanketEvent(id) end MafiaCoop.a5ids = {}
```

```lua
-- A5-2: DoDamage on an NPC (the last puppet or a spawned test NPC, see F1-5 in research-characters-missions.md):
-- watch the reaction, read the A5-1 log line (expect DamageType 9 = SCRIPT, attacker = itself)
local n = MafiaCoop.f1npc
if not n then return "spawn a test NPC first (MafiaCoop.f1npc)" end
local h0 = n.health
n:DoDamage(20)
return "health " .. tostring(h0) .. " -> " .. tostring(n.health)
```

```lua
-- A5-3: real shots from the local player at the test NPC's chest (attacker = player). Expect a SHOT damage event
-- with GetEnemyGUID == player, a real hit reaction, and the NPC reacting to the player.
local p, n = game.game:GetActivePlayer(), MafiaCoop.f1npc
local a, b = p:GetPos(), n:GetPos()
local from = Math:newVector(a.x, a.y, a.z + 1.5)
local to = Math:newVector(b.x, b.y, b.z + 1.2)
local ok, so = pcall(game.shotmgr.CreateShots, game.shotmgr, p, from, to, "p_mast_a_v1", 0, 1, true, true)
MafiaCoop.a5so = ok and so or nil
return "CreateShots " .. tostring(ok) .. " " .. tostring(so)
  .. " (also try shooter = a puppet: MafiaCoop.pup['1'].so, and the 11-arg form with entities)"
```

```lua
-- A5-4: ragdoll push and melee order
local n, p = MafiaCoop.f1npc, game.game:GetActivePlayer()
local b = n:GetPos()
local r1 = { pcall(n.ApplyRagdollRadialSpeed, n, Math:newVector(b.x + 1, b.y, b.z), 6, 3) }
local r2 = { pcall(n.ActivateRagdoll, n, true) }
return "radial " .. tostring(r1[1]) .. " " .. tostring(r1[2]) .. " | ragdoll " .. tostring(r2[1]) .. " " .. tostring(r2[2])
  .. " | next: n:AttackMelee(p) and check MELEE_* events"
```

```lua
-- A5-5: guest-side guard. nonplayerdamage = 0 on the player; then let an NPC shoot him (expect no damage), and
-- check DoDamage still works (attacker = self, a "player" source?).
local p = game.game:GetActivePlayer()
local before = p.nonplayerdamage
pcall(function() p.nonplayerdamage = 0 end)
local h0 = p.health
p:DoDamage(5)
return "nonplayerdamage " .. tostring(before) .. " -> " .. tostring(p.nonplayerdamage) .. "; DoDamage(5): "
  .. tostring(h0) .. " -> " .. tostring(p.health) .. " (restore with p.nonplayerdamage = " .. tostring(before) .. ")"
```

```lua
-- A5-6: car damage events (ram something with the car you sit in)
local car = game.game:GetActivePlayer():GetOwner()
if not car then return "sit in a car first" end
MafiaCoop.a5car = car:RegisterEvent({}, function(ctx, m)
  mc_log(string.format("A5-6 car damaged: type %s in %s final %s by %s", tostring(m:GetDamageType()),
    tostring(m:GetDamageIncoming()), tostring(m:GetDamageFinal()), tostring(m:GetAttackerGUID())))
end, enums.EventType.CAR, enums.CarMessages.CAR_DAMAGED)
return "car event id " .. tostring(MafiaCoop.a5car)
```

### Risks / open questions

1. Is a blanket handler called for *every* human, and how often? Watch the frame time with many NPCs. Keep handlers
   cheap: queue, do not call into the game.
2. Does `CreateShots` damage only what is on the line, or the target regardless? Do the scatter/timeout units mean
   degrees and seconds? Does the victim's DAMAGE carry `GetEnemyGUID == shooter`? (A5-3.)
3. `DoDamage` as SCRIPT damage: is there a hit reaction, a ragdoll on death, does `nonplayerdamage` apply to it
   (attacker = itself)?
4. `nonplayerdamage` scale: the setter multiplies by a constant (0x4B2491C); 0 vs 100 semantics to confirm.
5. `GetWeaponID` is numeric; `GetWeaponNameFromID` should map it back (84 = Tommy gun per game-api.md 6).

---------------------------------------------------------------------------------------------------------------

## A6. Weapons: why puppets refuse the Thompson; how enemies are armed

**Live-tested 2026-10-15** (A6-1, A6-2): confirmed. `InventoryAddWeapon` adds nothing to an NPC, the helper adds the
pistol, revolver, Tommy gun and shotgun, and a civilian-archetype puppet draws, aims and fires the Tommy gun without an
archetype switch (game-api.md 6).

### Summary

- **`InventoryAddWeapon` is player-only.** [confirmed from code]
  - Thunk 0x23B6A20 reads (name, ammo). Then it checks `cmp byte [entity+0x18], 9` (entity type PLAYER) and only
    then calls the inventory add 0x2BADA00. **For a human (type 8) it does nothing at all**, whatever the weapon.
  - The scan of all 478 human/player thunks found this type-9 check only in `InventoryAddWeapon`,
    `InventorySelectExplosive`, `InventoryGetSelectedExplosive`, `ClearInventory` and four input functions
    (`typechk.py`).
  - So on a puppet `InventoryAddWeapon` fails for the pistol too. If a pistol appeared on a puppet, it came from the
    `DbgTesting_HelperInventory_AddWeapon` fallback in `M.Weapon`, or the civilian already had it.
- **`h:DbgTesting_HelperInventory_AddWeapon(name, ammo)` is the same add without the player check.**
  [confirmed from code]
  - Thunk 0x23C31D0: name, ammo -> `0x23EA260(name)` (name -> id) -> 0x2BADA00(inventory, id, ammo).
  - **The shipped `human.lua` node uses exactly this** (`DbgTesting_HelperInventory_AddWeapon` + `InventorySelect`)
    to arm NPCs from mission flows.
- **NPCs have no slot limits.** [confirmed from code]
  - 0x2BADA00 asks 0x2B192F0 "can add?". For a non-player owner that returns true at once (`jne 0x2B19455 ->
    mov al,1`).
  - The placement 0x2B18A80 also skips the slot logic for non-players (`cmp byte [owner+0x18],9; jne 0x2B18D78`)
    and adds the item straight to the item list (0x25C03F0).
  - Slots (`[inventory+0xA8]`, 24-byte entries: item, locked flag, type mask) only apply to the player:
    `UnlockPlayerWeaponSlot(slotType)` / `LockPlayerWeaponSlot(slotType)`.
  - **So a civilian puppet can carry the Tommy gun through the helper.**
- What can still go wrong is *using* it.
  - The puppet's battle archetype / animation set may have no two-handed aim and fire.
  - Mission enemies get their weapon behaviour from the spawn profile plus a **battle archetype**.
    `tables.sds` pairs `archetype_triggerman_base`, `archetype_machinegunner_base`, `archetype_shotgunner_base`,
    `archetype_sniper_base`, `archetype_policeman` (`_mg`, `_shotgunner`, `_sniper`) with `characters/ai/*` profiles.
  - Test scenes do `enemy:SetBattleArchetype(arch)` and `SetAggressivity(enums.AI_Aggressivity.OnAttack)`.
  - Candidate fix for puppets: `puppet:SetBattleArchetype("archetype_machinegunner_base")` before selecting the SMG,
    and/or spawn the puppet from an armed AI profile (`characters/scotch/ai/ai_ally_sam_shotgun`,
    `characters/ai/morello/ai_enemy_morello_machinegunner`) [guess].
- **How story enemies are equipped**: by their spawn profile (the `ai_enemy_*_{triggerman, machinegunner,
  shotgunner, rifleman, ...}` names encode the loadout) and their archetype. Mission flows add or swap weapons with
  the helper node. The player's mission loadout is `mm_loadouts` (`data_missions.lua`: `weapons`, `equip`,
  `clear_inventory`, `explosives`, `molotov`, `grenade`, `melee`, `forced`, `small`/`large`). `game.lua` applies it
  with `ClearInventory`, `GetInventoryWeaponsInSlotType`, `InventoryRemoveWeapon`, `InventoryAddWeapon`,
  `InventorySelect`, `InventoryAddAmmoByCategory`, `InventoryEnsureMinCheckpointAmmo`.

### Candidate APIs

| call | args | notes |
|---|---|---|
| `h:DbgTesting_HelperInventory_AddWeapon(name, ammo)` | str, num | **NPC weapon add** (any weapon) |
| `h:InventoryAddWeapon(name, ammo)` | str, num | player only (type check) |
| `h:InventorySelect(name, animate, forced)` / `(name, animate)` | | NPC and player |
| `h:InventoryHaveWeapon(name)`, `h:InventoryHasWeaponInSlot(slot)`, `h:GetInventoryWeapons()`, `h:GetInventoryWeaponsInSlotType(slot)` | | `slot` = `enums.InventorySlotType.*` |
| `h:InventoryRemoveWeapon(name)`, `h:InventoryHolster(b)`, `h:InventoryDrop()`, `h:ClearInventory()` (player only) | | |
| `h:InventoryAddAmmoByCategory(cat, n [, fill])`, `h:InventoryGetAmmo(cat)`, `h:InventoryGetAmmoCategoryForWeapon(name)`, `game.game:GetAmmoCategory(name)` | | ammo |
| `h:InventorySetUnlimitedAmmo(b)` | bool | useful for puppets (they never need to reload unless told to) |
| `h:SetBattleArchetype(str)` / `GetBattleArchetype()` / `GetBattleArchetypeBase()`, `h:SetCivilArchetype(str)` / `GetCivilArchetype()` | | archetype names above |
| `h:GetWeaponMagazineSize(name)`, `h:GetEquippedWeaponSceneObject()` | | |
| `p:UnlockPlayerWeaponSlot(slot)` / `LockPlayerWeaponSlot(slot)` | num | player slots |

Weapon names (ScriptHook `WeaponIDs.lua`): `p_mast_a_v1`, `p_mast_a_v2`, `rev_alf_a_v1`, `rev_alf_b_v1`,
`rev_mast_special_a_v1/2`, `sg_barker_a_v1/2`, `sg_lupara_a_v1`, `rif_mayw_a_v1`, `sn_mayw_a_v1`, `smg_trench_a_v1/2`,
`wep_baseball_bat_a_v1`, `wep_knife_a_v1`, `wep_knuckle_a_v1`, `wep_wooden_plank_a_v1`, `wep_crowbar_a_v1`,
`wep_metal_bar_a_v1`; throwables `exp_grenade`, `exp_molotov`, `exp_satchel`.

### Live tests

```lua
-- A6-1: the static claim. On the test NPC: InventoryAddWeapon does nothing, the helper adds anything.
local n = MafiaCoop.f1npc
if not n then return "spawn a test NPC first" end
local out = {}
for _, w in ipairs({ "p_mast_a_v1", "smg_trench_a_v1", "sg_barker_a_v1" }) do
  pcall(n.InventoryAddWeapon, n, w, 60)
  local a = n:InventoryHaveWeapon(w)
  pcall(n.DbgTesting_HelperInventory_AddWeapon, n, w, 60)
  out[#out + 1] = w .. " add=" .. tostring(a) .. " helper=" .. tostring(n:InventoryHaveWeapon(w))
end
return table.concat(out, " | ") .. " | inv=" .. table.concat({ n:GetInventoryWeapons() }, ",")
```

```lua
-- A6-2: can the puppet hold/aim/fire the Tommy gun? Try with and without a machinegunner archetype.
local n = MafiaCoop.f1npc
local arch0 = tostring(n:GetBattleArchetype())
if MafiaCoop.a6arch then pcall(n.SetBattleArchetype, n, "archetype_machinegunner_base") end
pcall(n.InventorySelect, n, "smg_trench_a_v1", true, true)
local cam = game.cameramanager:GetPlayerCamera():GetDir()
pcall(n.DbgTesting_AimAtDir, n, cam)
return "archetype " .. arch0 .. " -> " .. tostring(n:GetBattleArchetype()) .. "; in ~1 s check InventoryGetSelected="
  .. tostring(n:InventoryGetSelected()) .. ", then fire with DbgTesting_ShootReload(true,false,0) / next frame false"
```

```lua
-- A6-3: same with an armed AI profile as the puppet's model (F1-5 recipe, profile below), no helper needed?
-- names: characters/ai/morello/ai_enemy_morello_machinegunner, characters/scotch/ai/ai_ally_sam_shotgun
local n = MafiaCoop.f1npc
return "inventory of the spawned AI-profile NPC: " .. table.concat({ n:GetInventoryWeapons() }, ",")
  .. " selected=" .. tostring(n:InventoryGetSelected()) .. " ai=" .. tostring(n:GetAIType())
```

### Risks / open questions

1. Civilian archetype + SMG: the weapon may be in the inventory but the draw/aim animation may be missing. A6-2
   decides between "archetype switch" and "AI profile with its own loadout" (A6-3).
2. An armed AI profile may come with its own AI type and archetype. The puppet must stay passive: test
   `ScriptControl(true, ...)` and `SetAggressivity`, and check it does not start fights on its own.
3. The helper's name says "DbgTesting". It is in the retail binding table and the shipped `human.lua` uses it, so
   it is safe to call; the only risk is that it bypasses mission loadout bookkeeping.

---------------------------------------------------------------------------------------------------------------

## B3. NPC mirroring: find, read, spawn, target, freeze

### Summary

- **There is no "humans in radius" query bound to Lua.** [confirmed: checked every bound class and `GetXxx` name.]
  The building blocks are:
  1. **`game.game:GetPlayersEnemies()`**: varargs of entity wrappers. Native 0x2673300 copies the u32 id list of
     the player's current enemies from the AI module `*(exe+0x63B9928)` (0x2A7F4F0) and pushes one wrapper per id.
     The flow node `GetPlayersEnemies` (`player.lua`) outputs it as `enemies`. Covers hostile NPCs in combat.
     [confirmed from code]
  2. **Blanket events** (§0.2): DEATH, DAMAGE, SEE_ENEMY, AI_PERCEPTION_EVENT, ON_SHOOT, ENTER/LEAVE_VEHICLE, and
     maybe ON_ACTIVATE / ON_DEACTIVATE (458753/4) for every human. The receiver GUID gives the entity. If
     ON_ACTIVATE fires on spawn, this is a complete "spawned / despawned" feed [guess, B3-1].
  3. **Scene-object trees**: mission objects are named in the SDS, e.g. `cp_050_spawner_frank_00`,
     `cp_070_spawner_paulie`, `Spawners_NMEs_in_Gardens`, `Spawn_Car_01`, `cp_060_haz4_spawner_police`.
     `game.entitywrapper:GetSceneObjectByName(name)` then `GetSubobjectsCount/GetSubobjectByIndex` or
     `table.c2lua(o:GetSubobjectsWithComponent("C_RuntimeSpawnerComponent"))` lists the spawners of a group.
     A runtime spawner has **no getter for the humans it spawned**.
  4. `game.game:GetCorpseCountAroundPos(pos, r)` (count only), `IsDeadHumanInside(volume)`.
     `game.traffic:FindNearestSpawnedVehicle(...)` / `GetNearestFoundSpawnedEntity(n)` cover traffic.
     `game.physics:StartCollisionQuery(from, to [, ?, layers])` -> `Wait`, then
     `UserData:HasHit()/GetHitEntity()` is a ray, not an area.
  5. Native fallback: hook the runtime spawner's internal spawn 0x2D8B470 (called by `CreateObject` 0x2D8B090;
     `CreateObjectAsync` 0x2D8B100 follows a different path) to see every spawn, including C++ flow spawns [guess].
- **Reading an NPC**: `GetPos/GetDir/GetRot`, `health`, `IsDeath()`, `GetAIType()` (AI_TYPE), `GetOwner()` (car or
  nil), `InventoryGetSelected()`, `IsInCover()`, `IsStunned()`, `IsReloading()`, `GetBattleArchetype()`,
  `GetEnemyPerceptionState()`, `GetSeePlayer()`, `IsModelVisible()`, `GetName()` (scene object name, e.g.
  `behavior_object_prime-NN-...`).
  - **No Lua getter for a human's spawn profile or model.** The only `GetSpawnProfile` is on the spawner
    (`C_SpawnerComponent`). To mirror an NPC's look: (a) read the spawner's profile (B3-3), (b) take it from the
    mission data (`mm_spawn_profiles`, spawner names), or (c) read it natively from the entity [no offset known].
  - Its current target: watch SEE_ENEMY / LOST_ENEMY (`GetEnemyGUID`) and AIM (`GetAimerGUID` on the victim).
- **How missions spawn enemies**: SDS spawner objects carry `C_SpawnerComponent` data (`m_SpawnProfile`,
  `m_SpawnerType` HUMAN/BASIC_OBJECT, `m_ShutdownBehavior` IMMEDIATE_DESPAWN / QUICK_TRAFFIC_DESPAWN /
  DEFAULT_DESPAWN, `m_SpawnOnAddToScene`) and the runtime component, plus `C_RuntimeProgressionSpawnComponent` for
  waves (`UpdateSpawners`).
  - The compiled C++ flow graphs (names stripped to hashes in the SDS) start them.
  - Lua helper nodes (`traffic.lua`): `SetSpawnerData*` -> `GetComponent("C_RuntimeSpawnerComponent"):
    SetSpawnProfile(p)`, `SetShutdownBehavior`, `SpawnProfileHuman*`.
  - `persistent_location.lua`: `GetComponent("C_SpawnerDataComponent"):SwitchDataSetString(...)`, then
    `Wait(comp:CreateObjectAsync())`, then `so:GetUserData():GetCreatedObject()`.
  - Test scenes: pre-placed `enemy%02d` + `SetBattleArchetype` + `SetAggressivity`.
  - Not bound in this build, though the scripts reference them (Mafia III leftovers): `SetSpawnerObjectRaw`,
    `GetSpawnPointObjectArchetype` (absent from the exe strings).
- **Making AI target a specific human** (so enemies react to guests' puppets on the host):
  - `enemy:Attack(target [, bool])` (0x239C1E0), `enemy:AttackMelee(target)`, `enemy:AimAt(target, ofs, b)`,
    `enemy:ShootAt(target, ofs, n)` (game-api.md 10).
  - `game.battle:SetTargetPriority(human, prio)` / `ResetTargetPriorities()`.
  - `enemy:SetShootTargetPriority(targetGuid, prio, b, b)`: #2 is a **C_EntityGuid**.
  - `enemy:AddBattleFakeTarget(fake, b)` with `game.game:NewBattleFakeTarget(pos, n, b, b, b)`.
  - `game.battle:SendEnemiesIntoBattleSearch(pos, r)`.
  - `relationships:SetRelationship(side1, type1, side2, type2, value)` (sides = `enums.AI_SIDE`, types =
    `enums.AI_TYPE`; value meaning unknown).
  - `relationships:CreateGangGroup(type, name)`, then `relationships:AddGroupMember(type, name, human:GetGUID())`.
    The args are (number, string, C_EntityGuid), read from the thunk 0x2A41FD0, matching the node
    `AddGroupMember(gType, gName, inEntity:GetGUID())`.
  - `relationships:IgnorePlayer(b)`, `GroupForgetHuman(...)`, `GroupForgetAll()`.
  - `wingmanmanager:RegisterWingman(puppet)`: allies of the player are legal enemy targets in missions.
  - `puppet:SwitchBrain(enums.AI_TYPE.X)` changes its AI type.
  - **Most promising**: the guest's shots are replayed on the host with `game.shotmgr:CreateShots(guestPuppet, ...)`
    (A5). The NPC is then really shot *by the puppet* and its own AI turns on the puppet. Add
    `game.battle:SetTargetPriority(puppet, high)` and `enemy:Attack(puppet)` when needed.
- **Police**: `h:AnnouncePoliceOffence(enums.OffenceType.X)` reports an offence by that human [guess: lets police
  chase a puppet]. Also `game.police:SquealerCallPolice(h)`, wanted level `game.hud:PoliceWantedSet(n)`, police
  zones (B4).
- **Freeze / AI off for mirrored copies on guests** (copies are driven like player puppets):
  - `h:ScriptControl(true, "DesireMainScript")`; `h:InterruptBattle()` / `ResumeBattle()`; `h:SuspendBattle(t)`;
  - `h:InterruptCivilWander()`, `InterruptPatrol()`, `InterruptAvoidingCar()`;
  - `h:PreventAIScriptThisPrioAndLower(n)` / `ResetPreventAIScript()`;
  - `h:SetReactionOnSound(false)`, `h:SetAggressivity(n)`, `h:SetPerceptionConfig(name)`,
    `h:DbgTesting_SetAiUpdateFrequency(n)`;
  - physics `h:SetPhysState(enums.PhysicsState.KINEMATIC)`; whole entity `Deactivate()` /
    `game.game:MassDeactivate(e)`.
  - The guest's own local mission NPCs (if the guest runs the chapter) can be suppressed with
    `game.traffic:BlockSpawnersInSphere(pos, r, true)` / `BlockSpawnersInArea`,
    `game.traffic:SetMaxHumanElements(0)`, or `Deactivate()` / `ReleaseAllObjects()` on their spawners.

### Candidate APIs (state + spawn + targeting)

| call | args | notes |
|---|---|---|
| `game.game:GetPlayersEnemies()` | none | varargs of wrappers. [confirmed] |
| `game.game:RegisterBlanketEvent(ctx, fn, et, msg)` | | discovery feed (§0.2) |
| `game.entitywrapper:GetEntityByGUID(guid)` | C_EntityGuid | from messages / GetGUID. [confirmed] |
| `o:GetSubobjectsWithComponent(c)`, `GetSubobjectsWithNamePrefix(p)`, `GetSubobjectsCount()`, `GetSubobjectByIndex(i)` | | wrap lists with `table.c2lua` |
| `spawnerObj:GetComponent("C_RuntimeSpawnerComponent")`: `SetSpawnProfile(p)`, `GetSpawnProfileLoadSyncObject()`, `CreateObject()`, `CreateObjectAsync()` -> `GetUserData():GetCreatedObject()`, `ReleaseObjectRaw(o)`, `ReleaseAllObjects()`, `SetShutdownBehavior(n)` | | the same component `player_sync.lua` uses |
| `GetComponent("C_SpawnerDataComponent"):SwitchDataSetString(name)` | | data-set variant of a spawner |
| `h:SetBattleArchetype(s)`, `h:SetAggressivity(n)`, `h:SwitchBrain(n)`, `h:MimicFriend(b)`, `h:NeverLoseTrackOfPlayer(b)`, `h:Forget()` | | |
| `h:Attack(t[, b])`, `h:AttackMelee(t)`, `h:AimAt(t, ofs, b)`, `h:ShootAt(t, ofs, n)`, `h:TakeCover(...)`, `h:BattleSearch(...)`, `h:CalmSearch(...)` | | |
| `game.battle:SetTargetPriority(h, n)`, `ResetTargetPriorities()`, `SendEnemiesIntoBattleSearch(pos, r)`, `SetArchetypeCountLimit(s, n)` | | |
| `h:SetShootTargetPriority(guid, n, b, b)` | C_EntityGuid, num, bool, bool | |
| `relationships:SetRelationship(s1, t1, s2, t2, v)`, `CreateGangGroup(t, name)`, `AddGroupMember(t, name, guid)`, `RemoveGroupMember(...)`, `IgnorePlayer(b)`, `IsPlayerAttacked()` | | global `relationships` |
| `wingmanmanager:RegisterWingman(h)` / `UnregisterWingman(h)` / `GetWingmenCount()` / `GetWingman(i)` / `AllowTeleportationIntoCar(h, b)` | | |
| `h:ScriptControl(b, s)`, `InterruptBattle()`, `SuspendBattle(t)`, `PreventAIScriptThisPrioAndLower(n)`, `DbgTesting_SetAiUpdateFrequency(n)`, `SetPhysState(n)` | | freeze |

### Live tests

```lua
-- B3-1: discovery feed. Log every human that activates/deactivates/sees an enemy for 30 s, plus the enemy list.
MafiaCoop.b3seen = {}
MafiaCoop.b3ids = MafiaCoop.b3ids or {}
for _, name in ipairs({ "ON_ACTIVATE", "ON_DEACTIVATE", "SEE_ENEMY", "AI_PERCEPTION_EVENT" }) do
  MafiaCoop.b3ids[#MafiaCoop.b3ids + 1] = game.game:RegisterBlanketEvent({ n = name }, function(ctx, m)
    pcall(function()
      local g = m:GetReceiverGUID()
      local e = game.entitywrapper:GetEntityByGUID(g)
      local k = ctx.n .. ":" .. tostring(g)
      if not MafiaCoop.b3seen[k] then
        MafiaCoop.b3seen[k] = true
        local extra = (ctx.n == "SEE_ENEMY") and (" enemy=" .. tostring(m:GetEnemyGUID())) or ""
        mc_log("B3-1 " .. ctx.n .. " " .. tostring(e) .. " type=" .. tostring(e and e:GetType()) .. " name="
          .. tostring(e and e.GetName and e:GetName()) .. extra)
      end
    end)
  end, enums.EventType.HUMAN, enums.HumanMessages[name])
end
local en = { game.game:GetPlayersEnemies() }
local s = {}
for _, e in ipairs(en) do s[#s + 1] = tostring(e) .. "@" .. tostring(e:GetPos()) end
return "enemies(" .. #en .. "): " .. table.concat(s, " ") .. " | blanket ids " .. table.concat(MafiaCoop.b3ids, ",")
```

```lua
-- B3-2: GUID round trip and per-NPC state of the enemies (run during a fight, e.g. Omerta cp or a test enemy)
local s = {}
for _, e in ipairs({ game.game:GetPlayersEnemies() }) do
  local g = e:GetGUID()
  local back = game.entitywrapper:GetEntityByGUID(g)
  s[#s + 1] = string.format("%s guid=%s back=%s hp=%s dead=%s ai=%s arch=%s car=%s wpn=%s perc=%s", tostring(e:GetName()),
    tostring(g), tostring(back == e), tostring(e.health), tostring(e:IsDeath()), tostring(e:GetAIType()),
    tostring(e:GetBattleArchetype()), tostring(e:GetOwner()), tostring(e:InventoryGetSelected()),
    tostring(e:GetEnemyPerceptionState()))
end
return table.concat(s, "\n")
```

```lua
-- B3-3: mission spawners. In a mission save, list the spawners of a known group/object and read their profile.
-- Names from the mission SDS: e.g. Omerta "cp_050_spawner_frank_00"; Mansion "Spawners_NMEs_in_Gardens".
local name = MafiaCoop.b3name or "cp_050_spawner_frank_00"
local o = game.entitywrapper:GetSceneObjectByName(name)
if not o then return "no object " .. name end
local s = { "obj=" .. tostring(o) .. " sub=" .. tostring(o:GetSubobjectsCount()) }
for _, cn in ipairs({ "C_RuntimeSpawnerComponent", "C_SpawnerComponent", "C_SpawnerDataComponent", "C_RuntimeProgressionSpawnComponent" }) do
  local ok, c = pcall(o.GetComponent, o, cn)
  local prof = ok and c and c.GetSpawnProfile and select(2, pcall(c.GetSpawnProfile, c))
  s[#s + 1] = cn .. "=" .. tostring(ok and c) .. (prof and (" profile=" .. tostring(prof)) or "")
end
local ok, l = pcall(function() return table.c2lua(o:GetSubobjectsWithComponent("C_RuntimeSpawnerComponent")) end)
s[#s + 1] = "children with spawner: " .. tostring(ok and #l or l)
return table.concat(s, " | ")
```

```lua
-- B3-4: make an enemy target a puppet. Needs an enemy (MafiaCoop.b3enemy, e.g. first of GetPlayersEnemies) and a
-- puppet (MafiaCoop.pup["1"].so on the host). Try the cheapest first, read the reaction.
local e = MafiaCoop.b3enemy or ({ game.game:GetPlayersEnemies() })[1]
local pp = MafiaCoop.pup and MafiaCoop.pup["1"]
local t = pp and pp.so or MafiaCoop.f1npc
if not (e and t) then return "need an enemy and a target" end
local r = {}
r[#r + 1] = "prio " .. tostring(pcall(game.battle.SetTargetPriority, game.battle, t, 100))
r[#r + 1] = "shootprio " .. tostring(pcall(e.SetShootTargetPriority, e, t:GetGUID(), 100, true, true))
r[#r + 1] = "attack " .. tostring(pcall(e.Attack, e, t))
return table.concat(r, " ") .. " | watch: does the enemy turn and shoot the puppet? (A5-1 log shows DAMAGE on it)"
```

```lua
-- B3-5: a hostile test enemy from the puppet factory (no mission needed): civilian -> enemy brain + archetype
local n = MafiaCoop.f1npc
if not n then return "spawn a test NPC first (use profile characters/ai/morello/ai_enemy_morello_triggerman)" end
pcall(n.DbgTesting_HelperInventory_AddWeapon, n, "p_mast_a_v1", 60)
pcall(n.InventorySelect, n, "p_mast_a_v1", true, true)
local a = pcall(n.SwitchBrain, n, enums.AI_TYPE.ENEMY)
local b = pcall(n.SetBattleArchetype, n, "archetype_triggerman_base")
local c = pcall(n.SetAggressivity, n, enums.AI_Aggressivity.SeeEnemy)
local d = pcall(n.Attack, n, game.game:GetActivePlayer())
return "brain " .. tostring(a) .. " arch " .. tostring(b) .. " aggr " .. tostring(c) .. " attack " .. tostring(d)
  .. " ai now " .. tostring(n:GetAIType())
```

```lua
-- B3-6: freeze an NPC (mirrored-copy behaviour): it should stop wandering/fighting but still obey MoveVec
local n = MafiaCoop.f1npc
local r = {}
r[#r + 1] = "sc " .. tostring(pcall(n.ScriptControl, n, true, "DesireMainScript"))
r[#r + 1] = "ib " .. tostring(pcall(n.InterruptBattle, n))
r[#r + 1] = "icw " .. tostring(pcall(n.InterruptCivilWander, n))
r[#r + 1] = "snd " .. tostring(pcall(n.SetReactionOnSound, n, false))
local p = game.game:GetActivePlayer():GetPos()
MafiaCoop.b3mv = n:MoveVec(Math:newVector(p.x + 3, p.y, p.z), "RUN")
return table.concat(r, " ") .. " | now fire a gun next to it: it should not flee"
```

### Risks / open questions

1. ON_ACTIVATE / ON_DEACTIVATE as blanket events are guesses. If they do not fire, discovery falls back to
   `GetPlayersEnemies` + DAMAGE/DEATH/SEE_ENEMY blankets + known spawner names, or a native hook on the spawner
   (0x2D8B470).
2. **Identity across machines**: entity ids differ per machine. If guests run the same chapter, the SDS spawner name
   is a stable key, but the link from a spawned human to its spawner is not readable in Lua. The host can tag
   humans itself (key = host GUID string) and guests spawn their own copies (player-puppet recipe with the profile
   the host reports). The profile still has to come from somewhere (B3-3, or a native read).
3. `SetRelationship` value range and `AI_SIDE`/`AI_TYPE` use are unknown; the game only calls it from a few nodes.
4. `CreateShots` with a puppet as shooter is the key unknown for "enemies react to guests" (A5-3 / B3-4).
5. Blanket handlers run inside the game's message dispatch: keep them non-blocking. Avoid spawning or despawning
   from inside one; queue the work for the next tick.

### Guest-owned NPCs (the reverse direction; 0.15.0, not yet checked in game)

Each game spawns a checkpoint's waves for its own player, so the guest's game can have enemies the host's has not
spawned yet ("he got somewhere first"). Design (docs/protocol.md "Guest-owned NPCs"): the guest's job NPCs that stay
unpaired for 5 s are his own; their states go to the host, which shows them as spawned copies (the 0.12.0 copy
recipe, reused: profile from the spawner name, `C_RuntimeSpawnerComponent` on a parked traffic car's driver, driven
like a puppet). Ownership, caps, claims and timeouts are pure C++ (`src/game/npc_owner.h`, unit-tested); the copies
are registered there by entity id so a copy is never reported or sent (no loops). The host's own late spawn of the
same NPC is claimed (same spot / same archetype tag within 40 m) and handed back: authority is always the host's
once both games have the NPC.

UNVERIFIED (static design, nothing of it run in game yet):
1. That the guest's mission NPC is named `behavior_object_prime-NN-<spawner>` on the guest's game too (the profile
   comes from there; without one the host shows no copy, the hits and deaths still sync for nothing visible).
2. That the host's puppet on the guest's game with the FRIEND brain (`SwitchBrain(1)`, so far only used on the host)
   draws the guest's enemies' fire and stays crash-free (the sectors guard covers it as on the host). The guest's
   copies of host enemies may now shoot at it as well (harmless: puppets do not die, those hits are not relayed).
3. That `CreateShots` from the host's puppet on the guest's game lands on the guest's NPC like the guest's replays do
   on the host (else `DoDamage`).
4. That the host's late spawn really comes up within 40 m of where the guest's NPC is (the same spawn points, but
   the guest's enemies may have moved): farther, it is not claimed and both games show a copy of the other's NPC
   (doubles) until one dies; the guest's loose pairing has the same 40 m limit.
5. Health hand-over at the hand-back (`h.health = hp` on the host's NPC) as in the existing enforce code.
6. Other guests do not see a guest's own NPCs (a host copy is never sent on).

---------------------------------------------------------------------------------------------------------------

## B4. Traffic, parked cars, mission cars, police

### Summary

- **`game.traffic` exposes two tables** (`C_ScriptWrapperTraffic` 0x5AE44A0 and the spawn table 0x5AE4650)
  [confirmed]:
  - `SwitchGenerators(bool)`: all traffic generators on/off. Node `TrafficSwitch`; the ScriptHook trainer
    "Disable Population".
  - `SetEnableAmbientTrafficSpawning(bool)`: ambient only. Node `AmbientTrafficSwitch`.
  - `GetIsAmbientTrafficOn()`.
  - `Populate()`: **no arguments** (thunk 0x26807D0 reads none; ScriptHook's `Populate(100)` ignores the 100).
    Returns a sync object.
  - `SetMaxHumanElements(n)`: pedestrian budget.
  - `SetMaxVehicleElements(moving_cars, parked_cars, translocated_cars, translocated_boats, moving_boats)`: 5
    numbers. The node takes them from `data_traffic.max_vehicle_tuning_profiles` (`default`, `truck_chase`,
    `sm_ef_080`).
  - `GetGlobalCarDensityMultiplier()`, `SetTrainDensity(n)`.
  - `BlockSpawnersInSphere(pos, r, bool)` / `BlockSpawnersInArea(?, ?, bool)`: stop spawning near a point.
  - `AddSuppressionToVolume(...)`, `SwitchRoad(pos, b [, b])`, `OpenSeason(n)` / `CloseSeason()` /
    `GetCurrentSeasonID()` (traffic season = vehicle era per chapter).
  - Dbg: `DbgSetHumanSpawnRanges(4 nums)`, `DbgSetVehicleSpawnRanges(3 nums)`, `DbgRegulateTraffic(n)`.
  - Not bound in this build: `SwitchFarAmbient` (the node exists, the method does not).
- **Parked cars**: the `parked_cars` budget of `SetMaxVehicleElements`; there is no per-car parked-car API. Parked
  and moving traffic cars are normal `C_WrapperCar`s once found.
- **Mission cars**: SDS spawner objects (`cp_30_spawner_player_car`, `Spawn_Car_01..08`, `enemy_car`,
  `cp_70_spawner_sergio_car`) through the runtime spawner. Also
  `ObtainNewPlayerCar(model)` / `ObtainSpecificCar(model)` / `ObtainCarWithCrew(model, {profiles})` / `ObtainCar(?)`
  then `SpawnCar(id, pos, dir [, b])` or `SpawnCarArrive*`. Script-defined looks come from
  `data_traffic.obtained_cars_overrides` (`*tommys_taxi`: `shubert_six_taxi`, `color`, `plate TA-190`, `dirt`,
  `rust`, `tuning`, `wheelcolor`, `interior`, `painting`/`paintingNS`). `traffic.lua` applies them with
  `SetColor`, `SetInteriorColorsSet`, `SetWheel`, `SetSPZText`, `InstallTuningItems`, `SetWindowTint`,
  `SetPainting`/`SetPaintingNoStream`. Crews: `data_traffic.crew_presets` (`civilian_white_crew`, `police_lvl1`,
  `reinforcements_easy/medium/hard`, ...).
- **Reading a car**: `GetVehicleModelName()`, `GetColor(slot)` (one number per call; the fleet code saves
  `-paint1`, `-paint2`, `-metallic`, so slots are probably 1/2), `GetSPZText()` (plate), `GetDirty()`, `GetRust()`,
  `GetPainting()`, `GetWindowTint()`, `GetInteriorColorsSet()`, `GetCarTableId()`, `GetDamage()`,
  `GetMotorDamage()`, `IsEngineOn()`, `GetSpeed()`.
  - Writing: `SetColor(c1, c2 [, metallic])`, `SetSPZText(text, bool)`, `SetDirty(n)`, `SetRust(n)`,
    `SetWheelColor(a, b)`, `SetWindowTint(n)`, `SetInteriorColorsSet(n)`, `SetPainting(s)`,
    `InstallTuningPackage(s)`.
  - All of these exist on `C_WrapperCar` [confirmed bindings; arg meaning likely].
- **Police**:
  - `game.police:Enable()` / `Disable()`, `PreventPoliceSpawning(b)`, `DisableOffencesGlobal(b)` /
    `DisableOffencesTemp(b)` / `DisableOffencesGlobalAllowPolice(b)`, `ClampEscalationLevel(n)`,
    `ForceMinEscalationLevel(n)`.
  - Zones: `ZoneCreate(pos, type [, level [, b]])` (`enums.PoliceZoneType` SEARCH 0 / HUNT 1), `ZoneDelete(pos [,
    r])`, `ZoneDeleteAll()`, `ZoneDecay(...)`, `GetActiveZones()`, `GetZoneInfo(...)`, `ZoneQuery(...)`.
  - Events: `RegisterForPoliceEvent(...)` / `UnregisterForPoliceEvent(id)` with `enums.PoliceEventType` flags.
  - Persistent patrols: `SetPersistentPoliceGlobalDensityModifier(n)`, `PersistentPolice*SpawningFrom*`.
  - Roadblocks: `SpawnRoadBlockWithCrew(...)`, `DespawnRoadBlock(id, b)`.
  - HUD: `game.hud:PoliceWantedSet(n)`, `PoliceRelationSet(n, b)`; player: `p:IsSeenByPolice()`,
    `p:GetClosestPolicePos()`.

**Recommended start (matches PLAN B "start local")**:
- Keep ambient traffic local on every machine and only mirror cars a player interacts with (already done for
  shared cars).
- On guests, mission cars come from the guest's own mission run or from host copies (`ObtainSpecificCar` + looks
  from the getters above).
- Police: replaced (see "Police (implementation)" below): each player's wanted state stays his own game's (there
  is one zone per game, the player's: `ZoneCreate` cannot mirror another player's zone); near the host his puppet is
  a target of the host's police and his game holds its level while they chase him.

### Live tests

```lua
-- B4-1: traffic switches (look at the street before/after; restore at the end)
local t = game.traffic
local s = { "ambientOn=" .. tostring(t:GetIsAmbientTrafficOn()), "density=" .. tostring(t:GetGlobalCarDensityMultiplier()),
  "season=" .. tostring(t:GetCurrentSeasonID()) }
s[#s + 1] = "gen off " .. tostring(pcall(t.SwitchGenerators, t, false))
return table.concat(s, " | ") .. " -- later: t:SwitchGenerators(true); MafiaCoop.b4so = t:Populate()"
```

```lua
-- B4-2: budgets (0 pedestrians, 0 moving and parked cars), then back to the default profile values from data_traffic
local t = game.traffic
local a = pcall(t.SetMaxHumanElements, t, 0)
local b = pcall(t.SetMaxVehicleElements, t, 0, 0, 0, 0, 0)
local d = data_traffic and data_traffic.max_vehicle_tuning_profiles and data_traffic.max_vehicle_tuning_profiles.default
local dv = {}
if d then for k, v in pairs(d) do dv[#dv + 1] = k .. "=" .. tostring(v) end end
return "humans0 " .. tostring(a) .. " cars0 " .. tostring(b) .. " | default profile: " .. table.concat(dv, " ")
```

```lua
-- B4-3: read the look of the car you sit in (or the nearest traffic car)
local c = game.game:GetActivePlayer():GetOwner()
if not c then return "sit in a car" end
local function g(f, ...) local ok, v = pcall(c[f], c, ...); return f .. "=" .. tostring(ok and v or ("ERR " .. tostring(v))) end
return table.concat({ g("GetVehicleModelName"), g("GetColor", 0), g("GetColor", 1), g("GetColor", 2), g("GetSPZText"),
  g("GetDirty"), g("GetRust"), g("GetPainting"), g("GetWindowTint"), g("GetInteriorColorsSet"), g("GetCarTableId"),
  g("GetDamage") }, " ")
```

```lua
-- B4-4: apply a look to a copy (MafiaCoop.cars entry or any spawned car): colours, plate, dirt
local c = MafiaCoop.b4car or game.game:GetActivePlayer():GetOwner()
local r = {}
r[#r + 1] = "color " .. tostring(pcall(c.SetColor, c, 5, 7, false))
r[#r + 1] = "plate " .. tostring(pcall(c.SetSPZText, c, "COOP-01", true))
r[#r + 1] = "dirt " .. tostring(pcall(c.SetDirty, c, 0.8))
r[#r + 1] = "rust " .. tostring(pcall(c.SetRust, c, 0.5))
return table.concat(r, " ")
```

```lua
-- B4-5: police state (host) and zone mirroring (guest)
local pol = game.police
local ok, z = pcall(pol.GetActiveZones, pol)
local zs = { select(2, pcall(function() return pol:GetActiveZones() end)) }
return "zones: " .. tostring(ok) .. " " .. tostring(z) .. " (" .. #zs .. " values) offencesDisabled="
  .. tostring(select(2, pcall(pol.AreOffencesDisabled, pol)))
  .. " -- guest: pol:DisableOffencesGlobal(true); pol:ZoneCreate(pos, enums.PoliceZoneType.HUNT, 1)"
```

### Implementation (0.15.0; not yet run in game)

The "start local" recommendation above is replaced: **the host's traffic is authoritative near the host.**
`src/game/traffic_sync.lua` (game side), `src/game/traffic_sync.cpp` (session, Lua bridge) and
`src/game/traffic_model.*` (pure C++, unit-tested in `tests/traffic_test.cpp`); messages 0x0220-0x0223
(docs/protocol.md "Traffic"); trace records TRH / TRP / TRG / TRC / TRL (docs/sync-trace.md, tools/sync_compare.py
"Traffic").

- Host: moving traffic and police cars (generator drivers) within 380 m of a guest are reported 20 times a second;
  core's InterestManager (Kind::Vehicle) plus an admission cap give each guest the nearest 24 within 260 m, with
  spawn / despawn messages and changed-only packed states (22 bytes a car).
- Guest: within 250 m of the host (hysteresis to 400 m) his own ambient traffic and police spawning are switched off
  (`SetEnableAmbientTrafficSpawning`, `PreventPoliceSpawning`; not `SwitchGenerators`, risk 1) and his remaining
  moving traffic cars are deactivated once more than 70 m away; the nearest 20 host cars get crewed copies driven by
  core's VehicleController with the host car's controls, the way remote players' cars are. Far from the host he keeps
  his own traffic (the host's generators only fill the area around the host; no call was found to generate around
  another point).
- Parked cars stay local (risk: a parked car of one game where the other game's traffic drives). Mission cars and
  player cars are excluded on the host; a traffic car a player takes is handed over to player_sync on both games.
- Driver deaths and shots at drivers go through npc_sync (traffic drivers are NPCs, paired by position like mission
  NPCs). Damage to the cars themselves is not shared; a copy knocked off is put back and reported (TrafficBump).

Open (to check in game, README-dev / the coordinator's test plan): whether `SetEnableAmbientTrafficSpawning(false)`
removes cars already driving; whether `Deactivate` on crewed traffic cars is safe; the crew preset layout; whether
`LockControls` stops the copy's AI driver (else the copies get pinned: TRG pins); the siren heuristic; the look
getters' slots; Lua cost with 20 copies (each tick: SetPos/SetRot/SetSpeed and four control setters per near copy).

### Police (implementation; unreleased, not yet run in game)

`src/game/police_sync.lua` / `.cpp`, `src/game/police_model.*` (tests/police_test.cpp), messages 0x0260-0x0262
(docs/protocol.md "Police"), the API in docs/game-api.md 22 and docs/re-notes.md 10. `GetActiveZones` returns `0`
when the player has a zone and `GetZoneInfo(0)` gives centre, level and type (risk 3 below is answered statically).
Open: the level scale, what police do with an offence announced from a puppet, the callback argument of
`RegisterForPoliceEvent`, whether `ForceMinEscalationLevel(0)` is "none".

### Risks / open questions

1. `SwitchGenerators(false)` may also stop *mission* traffic generators (some chases rely on them). Test in a
   chase chapter. `SetEnableAmbientTrafficSpawning` is the narrower switch.
2. `GetColor` slot numbering and colour index meaning (palette index vs RGB) need B4-3.
3. `GetActiveZones` / `GetZoneInfo` return shapes: decoded statically (re-notes 10), not seen in game.
4. Mission-script traffic overrides (`SetMaxVehicleElements` per checkpoint) on the host would not reach guests
   unless mirrored. Mirror the 5 numbers when the host's node runs (wrap the Lua node) [guess].
5. Mission car swapped for a copy (0.9.0) and back (0.15, docs/game-api.md 15 "Mission car back"): the real car
   is hidden with `Deactivate()` and shown again with `Activate()` at the copy's place once the car is parked.
   Open: whether `Activate` brings a car back with its mission state as it was (the mission's scripts keep the
   entity, so they should), whether a deactivated car takes `SetPos`, `GetDamage()`'s scale (wreck detection),
   whether a checkpoint save made while a car is hidden stores it deactivated, and what a mission does with NPCs
   that should board the hidden car meanwhile [all unverified]. A cleaner alternative, if `Activate` misbehaves:
   keep the real car shown and driven (pinned like the 0.9 copies) with the remote driver's puppet beside it in
   the passenger seat, which keeps NPCs (passenger seats are fine, game-api 7).

---------------------------------------------------------------------------------------------------------------

## B5. Cutscenes

### Summary (confirms and extends research-characters-missions.md B5)

- Real-time cutscenes are scene objects `cine_<nnnn>_<name>_cs_object` in each chapter's primary SDS
  (`mm_<n>_<chapter>_p`; full list below) with a **`C_RuntimeCutsceneComponent`**. The cutscene's own data is
  `sds_retail\cutscenes\cine\cine_<...>\cine_<...>.sds` (+ `_audio.sds`), 125 folders, loaded by `LoadCutscene`.
  `_bik` entries are pre-rendered videos.
- **Reflected methods** (registration at 0xC5FE00..0xC60500) [confirmed]: GetCutsceneId, SetPlayOption,
  GetPlayOption, LoadCutscene, UnloadCutscene, IsCutsceneLoadStarted, IsCutsceneLoadFinished,
  GetVisualStartSyncObject, PlayCutscene, PlayCutscene2, StopCutscene, IsCutscenePlaybackInProgress,
  AllowPlayerCamera.
  - `PlayCutscene2` member 0xC5E890: `(this, outSyncObject, char* titleId, char* descId, char* texture)`. It hashes
    title and desc (FNV-1 64) for a title card and returns a sync object.
  - `PlayCutscene` member 0xC5E990: `(this, outSyncObject, bool)`.
- **The game's own play sequence** (`cutscene.lua` nodes `Play`, `Play2`..`Play5`, `Play_DLC1`) [confirmed from
  constants]:
  ```
  comp = csObj:GetComponent("C_RuntimeCutsceneComponent")
  comp:LoadCutscene(); wait until comp:IsCutsceneLoadFinished()
  so = comp:PlayCutscene2(titleId, descId, texture)            -- "" for none; older nodes: PlayCutscene(...)
  Wait(comp:GetVisualStartSyncObject())                        -- first frame visible
  DatastoreSet("cutscene_active", ..., "DefaultGroup")          -- only some node variants
  Wait(so)                                                      -- playback finished
  DatastoreDel("cutscene_active", "DefaultGroup"); comp:UnloadCutscene()  -- if inUnloadOnFinished
  ```
  - Stop/skip: `comp:StopCutscene()`.
  - Some nodes drop a carried body first (`getp():GetCarriedBody()` / `DropBody`) and show
    `game.hud:ShowTitleCard(text, desc)` from `title_cards`.
  - Audio: `audio:PlaySimpleEvent("Mix_Fade_Out_All")` when `mute_after`.
  - `C_CutsceneGameObjectComponent:SetCutsceneObject(gameEntity)` (node `SetGameObject`) binds a live game entity
    (e.g. the player's car) into a cutscene; `AllowPlayerCamera(b)` is node `EnablePlayerCamera`.
- **Detect start/end on the host**:
  - (a) Poll the current chapter's `cs_object`s every ~0.5 s: `IsCutscenePlaybackInProgress()` (and
    `IsCutsceneLoadFinished()` for "about to start"). Robust; the names are static per chapter (below).
  - (b) Wrap the Lua node functions `Cutscene.Play*.playCutscene*` / `Cutscene.Stop.stopCutscene` (nodeID, inEntity,
    ...): gives the `cs_object` and the title ids. Only works if the flows look the functions up by name (same
    caveat as B2-2).
  - (c) `cutscene_active` in the datastore: only set by some node variants, so not reliable alone.
- **Can a guest play the same cutscene locally?** [likely yes, if it runs the same chapter part]
  - The cs_object exists only while the chapter's `_p` SDS is loaded.
  - Its data loads on demand (`LoadCutscene`), so the guest calls the same sequence on its own copy of the object.
  - The cutscene uses its own actors, so the guest's puppets are not in it, and the guest's player is hidden or
    replaced like the host's. Remote puppets should be hidden during it (`ShowModel(false)`).
  - Bound game entities (`SetCutsceneObject`) would be the guest's own entities, not the host's: cars may differ.
  - A guest outside the chapter can only show a "cutscene playing" screen.
- **Skipping**:
  - Real cutscenes: the C++ skip input (`E_CTRL_SKIP_CUTSCENE`) on that machine; scripted skippable scenes use
    `game.skipscriptscene:BeginSkipableScene()` / `EndSkipableScene()`.
  - For co-op: host skip -> send "stop <cs_object>" -> guest `StopCutscene()`. The guest's own skip should be
    blocked or ignored (only `StopCutscene` locally, the host keeps playing).
- `C_WrapperCutscene` (entity type CUTSCENE 43: `Play()`, `Pause()`, `Stop()`) is a second, older cutscene entity
  kind. No game script uses it [likely unused]. `game.game:PlayCutscene` stays a stub (previous research).

### Cutscene objects per chapter (names without the `_cs_object` suffix; object name = `<name>_cs_object`)

- mm_10_chase_p: cine_0005_prologue_bik, cine_0100_chase_bik, cine_0110_chase_reward
- mm_20_taxi_p: cine_0200_taxi, cine_0270_taxi_salireris
- mm_30_molotov_p: cine_0300_molotov_intro, cine_0310_molotov_get_guns, cine_0320_molotov_get_car,
  cine_0330_molotov_outro
- mm_40_motel_p: cine_0400_motel_intro, cine_0420_motel_get_car, cine_0450_motel_arrive, cine_0460_motel_gorilla,
  cine_0490_intermission_one, cine_0495_intermission_one_morello_bik, cine_0497_intermission_one
- mm_50_race_p: cine_0500_race_intro, cine_0510_race_ralphy, cine_0540_race_tuning, cine_0560_race_driver,
  cine_0565_race_finish_bik, cine_0570_race_win, cine_0592_race_paulie
- mm_60_sarah_p: cine_0600_sarah_intro, cine_0610_sarah_punks, cine_0620_sarah_fight, cine_0625_sarah_fight_win,
  cine_0630_sarah_outro
- mm_70_hoodlums_p: cine_0700_hoodlums, cine_0705_hoodlums_get_car, cine_0730_hoodlums_backalley,
  cine_0750_hoodlums_chase, cine_0760_hoodlums_kill
- mm_80_brothel_p: cine_0800_brothel, cine_0830_brothel_michelle, cine_0840_brothel_escape, cine_0847_brothel_jump,
  cine_0850_brothel_funeral, cine_0860_brothel_priest, cine_0995_intermission_two,
  cine_0997_intermission_two_frank_bik
- mm_100_farm_p: cine_1000_farm, cine_1010_farm_get_car, cine_1020_farm_trucks, cine_1030_farm_ambush,
  cine_1040_farm_search, cine_1050_farm_sam, cine_1060_farm_company, cine_1062_farm_chase_split,
  cine_1063_farm_chase_dead1, cine_1064_farm_chase_stuck, cine_1065_farm_chase_dead2, cine_1066_farm_chase_free,
  cine_1067_farm_chase_turret_down, cine_1068_farm_chase_molotovs, cine_1069_farm_chase_victory,
  cine_1070_farm_outro, cine_1071_farm_chase_dead3, cine_1075_farm_warehouse, cine_1095_farm_sarah
- mm_110_omerta_p: cine_1100_omerta, cine_1130_omerta_cops, cine_1140_omerta_cops_arrive,
  cine_1160_omerta_confrontation, cine_1195_omerta_funeral_bik
- mm_120_mansion_p: cine_1200_mansion_bik, cine_1250_mansion_safe
- mm_130_parking_p: cine_1300_parking, cine_1320_parking_ambush, cine_1325_parking_leave, cine_1330_parking_outro,
  cine_1395_intermission_three
- mm_140_salieri_p: cine_1400_salieri, cine_1410_salieri_ambush1_bik, cine_1415_salieri_ambush1_bik,
  cine_1420_salieri_ambush2, cine_1430_salieri_carlos, cine_1440_salieri_outro, cine_1495_morello
- mm_150_boat_p: cine_1500_boat, cine_1530_boat_sailor_costume, cine_1550_boat_depart, cine_1575_boat_get_gun,
  cine_1580_boat_speech_bik, cine_1590_boat_outro_bik
- mm_160_harbor_p: cine_1600_harbor_bik, cine_1602_harbor_bomb, cine_1630_harbor_dame, cine_1640_harbor_restaurant,
  cine_1650_harbor_bike, cine_1670_harbor_port, cine_1680_harbor_kill_sergio
- mm_170_plane_p: cine_1700_plane_bik, cine_1710_plane_plane_late, cine_1750_plane_airport, cine_1760_plane_shoot,
  cine_1770_plane_crash, cine_1790_plane_end, cine_1795_intermission_four
- mm_180_sniper_p: cine_1800_sniper, cine_1810_sniper_get_gun, cine_1890_sniper_sarah
- mm_190_cigars_p: cine_1900_cigars, cine_1930_cigars_little_job_bik, cine_1950_cigars_hide,
  cine_1960_cigars_load_crates, cine_1990_cigars_outro, cine_1995_cigar_train_paulie_bik
- mm_200_bank_p: cine_2000_bank_intro, cine_2050_bank_robbery, cine_2060_bank_safe, cine_2070_bank_success
- mm_210_gallery_p: cine_2100_gallery_sarah, cine_2110_gallery_pauli_dead, cine_2120_gallery_round_one,
  cine_2130_gallery_round_two, cine_2150_gallery_outro, cine_2200_the_end, cine_2210_the_end_prison_bik

### Live tests

```lua
-- B5-1: poll every cutscene object of the loaded chapter (host; run repeatedly while a cutscene plays)
local list = MafiaCoop.b5list or { "cine_1100_omerta", "cine_1130_omerta_cops", "cine_1140_omerta_cops_arrive",
  "cine_1160_omerta_confrontation", "cine_1195_omerta_funeral_bik" }
local s = {}
for _, n in ipairs(list) do
  local o = game.entitywrapper:GetSceneObjectByName(n .. "_cs_object")
  local c = o and o:GetComponent("C_RuntimeCutsceneComponent")
  if c then
    local function q(f) local ok, v = pcall(c[f], c); return ok and tostring(v) or "err" end
    s[#s + 1] = n .. " id=" .. q("GetCutsceneId") .. " loaded=" .. q("IsCutsceneLoadFinished") .. " playing=" .. q("IsCutscenePlaybackInProgress")
  else s[#s + 1] = n .. " (absent)" end
end
return table.concat(s, " | ")
```

```lua
-- B5-2: play one locally (guest or host, in the chapter whose _p SDS is loaded). Step 1: load.
local o = game.entitywrapper:GetSceneObjectByName(MafiaCoop.b5name or "cine_1100_omerta_cs_object")
MafiaCoop.b5c = o and o:GetComponent("C_RuntimeCutsceneComponent")
if not MafiaCoop.b5c then return "no cutscene object" end
MafiaCoop.b5load = MafiaCoop.b5c:LoadCutscene()
return "load started: " .. tostring(MafiaCoop.b5load)
```
```lua
-- B5-2 step 2 (when IsCutsceneLoadFinished() is true): play without title card, keep the sync objects
local c = MafiaCoop.b5c
if not c:IsCutsceneLoadFinished() then return "still loading" end
local ok, so = pcall(c.PlayCutscene2, c, "", "", "")
if not ok then ok, so = pcall(c.PlayCutscene, c, false) end
MafiaCoop.b5play = so
MafiaCoop.b5vis = c:GetVisualStartSyncObject()
return "play " .. tostring(ok) .. " " .. tostring(so) .. " visual=" .. tostring(MafiaCoop.b5vis)
```
```lua
-- B5-2 step 3: progress / stop / unload
local c = MafiaCoop.b5c
local done = MafiaCoop.b5play and MafiaCoop.b5play.IsDone and MafiaCoop.b5play:IsDone()
if MafiaCoop.b5stop then c:StopCutscene() end
if done or MafiaCoop.b5stop then c:UnloadCutscene() end
return "playing=" .. tostring(c:IsCutscenePlaybackInProgress()) .. " done=" .. tostring(done)
```

```lua
-- B5-3: wrap the cutscene node functions (host) and log which ones the flows call
local hooked = {}
local C = Cutscene or (package.loaded.cutscene and package.loaded.cutscene.Cutscene)
if type(C) ~= "table" then return "Cutscene table not found" end
for k, node in pairs(C) do
  if type(node) == "table" then
    for fk, fv in pairs(node) do
      if type(fv) == "function" and (fk:find("play") or fk:find("stop") or fk:find("load")) and not MafiaCoop["b5h" .. k .. fk] then
        MafiaCoop["b5h" .. k .. fk] = true
        node[fk] = function(...)
          local a = { ... }
          local ent = a[2]
          mc_log("B5-3 " .. k .. "." .. fk .. " ent=" .. tostring(ent) .. " name=" .. tostring(ent and ent.GetName and ent:GetName()))
          return fv(...)
        end
        hooked[#hooked + 1] = k .. "." .. fk
      end
    end
  end
end
return "hooked " .. table.concat(hooked, ", ")
```

### Risks / open questions

1. `PlayCutscene2` with empty strings: the member skips the title-card path when the strings are empty (the code
   checks for non-empty title/desc and texture), but this must be confirmed in game.
2. Cutscenes that bind live entities (`SetCutsceneObject`, `cine_1140_car_police_substitute`) may look different on
   guests.
3. Mission flow side effects (teleports, outfit swaps, objective updates) run on the host after a cutscene. Guests
   need the host's resulting state (position sync already does most of it).
4. Starting a cutscene on a guest whose mission state differs may trigger its own flow callbacks if the guest runs
   the mission scripts. Safer: guests only *display* cutscenes and never run the chapter's flow for them.

---------------------------------------------------------------------------------------------------------------

## Dead ends and traps found statically

- `h:InventoryAddWeapon` on non-player humans is a silent no-op (type-9 check). Use
  `DbgTesting_HelperInventory_AddWeapon`.
- `h:DoDamage(n)` has one argument only; type SCRIPT, attacker = victim. No direction, zone or weapon.
- `game.traffic:Populate` has no arguments.
- Not bound in this build, though the shipped scripts mention them: `SetSpawnerObjectRaw`,
  `GetSpawnPointObjectArchetype`, `SwitchFarAmbient`. The scripts that call them are Mafia III-era leftovers.
- `game.entitywrapper:GetEntityByGUID` needs a `C_EntityGuid`, never a number.
- `enums.EventType` is the *entity message type* enum; the C++ `C_WrapE_EventType` is `enums.PoliceEventType`.
- No Lua getter for a human's spawn profile, current target, or "all humans near X".

## Method (to redo after a game update)

1. Enum names: find each `{vtable, "enums", "<LuaName>"}` registration (pattern
   `lea rcx,"enums"; mov [rax+8],0; mov [rax+0x10],rcx; lea rcx,NAME; mov [rax+0x18],rcx; lea rcx,VT`) and follow
   the vtable to the `C_WrapE_*` class string (`enumlua.py`).
   - Enum tables: `lea rdx,TABLE; lea rcx,"C_WrapE_X"; jmp 0x3812E50` (`enummap.py`). Entries are 32 bytes
     `{name, getter, name, setter}`; the getter stores the value as a float at `[rax+8]` (`enums_dump.py`).
2. Bound classes: every `lea r8,TABLE; lea rdx,PARENT; lea rcx,CLASS; call 0x3812B70` (`classes.py`). The methods
   are the luaL_Reg table.
3. Thunk signatures: `sig.py` walks each thunk, maps `mov edx,N` + `call` to the readers (0x785B30 number,
   0x1254DD0 string, 0x1AC6B10 bool, 0x3810FF0 vector, 0x1ABD980 / 0x1ABD570 wrapper, 0x3810D50 hash,
   0x170E430 C_EntityGuid, 0x23F2640 int) and follows argc dispatch targets one level.
   - `sigs.txt` holds all 1500. It misses readers it does not know (empty lines); disassemble those by hand.
4. Reflected component methods: raw strings just before the class name string, or the registration code storing
   `{vtable, owner, "Name", memberFn}` (e.g. 0xC5FE00 for the cutscene component, 0x2DA1F70 for the spawner).
5. Script usage: `c.py "<regex>" before after "<file glob>"` over the re2 constant dumps. The constants of each Lua
   function are in source order, so a node's calls read left to right.
