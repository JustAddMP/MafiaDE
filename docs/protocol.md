# MafiaCoop app messages

The messages the Mafia DE game layer sends over a JustAddMP core session: player sync (phase 3) and world state
(phase 4). Sessions, the handshake, the password, relay rules (`SendToHost` / `Broadcast` / `SendTo`), the UDP and
Steam transports and lobbies are core's: `external/core/docs/protocol.md`. All integers are little-endian.
`varint` = unsigned LEB128, `str` = varint byte length + UTF-8 bytes.

Version rules: any incompatible change to these messages needs a bump of `modVersion` in `src/game/game_info.cpp`
(the session build number is derived from it, so mismatched builds are rejected in the handshake). 0.3.0: Hit (0x0400)
and the character per player. 0.4.0: PlayerModel (0x0102), character choice in the CO-OP panel, punch detection. 0.5.0:
core 0.5 (protocol 7: the pre-game lobby, core docs/protocol.md "Lobby"); lobby mode indexes: 0 FREE RIDE, 1 STORY (not
startable yet). 0.6.0: MissionState (0x0201), HostSave (0x0202), Cutscene (0x0203); lobby modes 0 FREE RIDE, 1 STORY,
2..21 CHAPTER 1..20 (`MissionSync::Chapters()`, the order of `mm_ids_replay`). 0.7.0: NpcStates (0x0210), NpcDeath
(0x0211), NpcHit (0x0410); shared cars take a standing car of the same model as the driver's car. 0.9.0: PlayerState
at up to 60 Hz, NpcStates at 10 Hz, Hit carries u8 source + u32 npc (an NPC of the sender's game hit the puppet). 0.12.0:
NpcStates carry each NPC's spawn profile (u64) and a shot counter (u8): guests spawn copies of host NPCs that have no
partner on their game. 0.13.0: PlayerState version 4: flags bit5 (in the air) and bit6 (in cover); the vehicle block
ends with the driver's controls (6 bytes). 0.15.0 (core 0.11.0, `kProtocolVersion` 8: the reconnect trailers and
PeerDropped, core docs/protocol.md "Reconnect"): guest-owned NPCs, GuestNpcStates (0x0420), GuestNpcEnd (0x0421), HostNpcHit (0x0422), NpcClaim (0x0423) (see "Guest-owned NPCs"); traffic, TrafficSpawn (0x0220),
TrafficDespawn (0x0221), TrafficStates (0x0222), TrafficBump (0x0223) (see "Traffic"); failed missions, MissionFail
(0x0205), FailVerdict (0x0206); PlayerState still version 4 but with the car signals byte and the action block at
its end (both always sent); CarParked with a trailing flags byte (wrecked).

Reconnect (core docs/protocol.md "Reconnect"): no app message of ours changes and none is added. A guest whose
connection was lost keeps playing while core rejoins the host; the host holds his slot. See "Reconnect" at the end.

Mission fail: MissionFail (0x0205) and FailVerdict (0x0206), additive (a peer without them ignores them).

App id allocation (core reserves 0x0000-0x00FF):

| Range | Owner |
|---|---|
| 0x0100-0x01FF | phase 3: player sync (ped transform, anim, weapon, vehicle seat) |
| 0x0200-0x02FF | phase 4: world (NPCs, cars, weather, time, mission state) |
| 0x0300-0x03FF | phase 5: UI / notifications |
| 0x0400-0x04FF | phase 6: damage, objectives |

## Phase 3: player sync (app ids 0x0100-0x01FF)

Code: `src/game/player_sync.{h,cpp}` (C++: wire format, buffering, interpolation) and `src/game/player_sync.lua`
(game side, embedded into the dll). Allocated ids:

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0100 | PlayerState | every peer -> everyone (`Broadcast`; the host relays guest states) | U, 60 Hz (0.9.0; was 20) | see below |
| 0x0101 | CarParked | last driver -> everyone (`Broadcast`) | R | u8 keyOwner, u32 vehId, str model, f32 x, y, z, f32 qx, qy, qz, qw, u8 flags (0.15: bit 0 the car is destroyed; read as 0 when missing, older builds ignore it) |
| 0x0102 | PlayerModel | every peer -> everyone (`Broadcast`) | R | str model, str look |
| 0x0103-0x01FF | reserved (phase 3 events) | | | |

PlayerState body (56 bytes on foot + weapon name, ~139 bytes in a car):

```
u8  version (4; 0.13.0, was 3)
u32 senderTimeMs          sender's monotonic clock (QueryPerformanceCounter), for interpolation only
f32 x, y, z               player position (world metres)
f32 hx, hy                heading, unit vector in the ground plane
f32 vx, vy, vz            velocity (m/s; on foot differentiated from positions, in a car the car's GetSpeed())
u8  mode                  0 idle, 1 walk, 2 run, 3 sprint (from the speed; sprint also when IsSprinting())
u8  flags                 bit0 in vehicle, bit1 crouch, bit2 sprint, bit3 aim (aim input held with a gun in hand),
                          bit4 reload (IsReloading(), or for 1 s after the clip was refilled),
                          bit5 in the air (vertical speed above 2 m/s: jumping, falling; 0.13.0),
                          bit6 in cover (IsInCover(); 0.13.0)
u8  health                p.health (0..100)
str weapon                weapon in hand (InventoryGetSelected), "" = none
u8  shots                 shots fired so far, mod 256 (from drops of the ammo in the clip); a change = new shots
f32 ax, ay, az            aim direction: the player camera's direction, unit vector
-- only when flags bit0:
str model                 vehicle model name (GetVehicleModelName, e.g. "smith_v12")
u8  seat                  0 = driver, 1..3 passengers, 9 = getting in/out (no seat yet: claims nothing)
u8  keyOwner              car key, part 1: id of the player who first drove this car
u32 vehId                 car key, part 2: the car's entity GUID on keyOwner's game
f32 cx, cy, cz            car position
f32 qx, qy, qz, qw        car rotation (quaternion, GetRot: v.x v.y v.z s)
f32 cvx, cvy, cvz         car velocity
u8  ctlValid              0.13.0: bit0 = the controls below were read (seat 0 only; docs/re-notes.md 9)
i8  steer                 steering wish -1..1 (x127), as the car stores it (after SetSteer's response curve)
u8  power                 throttle 0..1 (x255)
u8  brake                 brake 0..1 (x255)
u8  handbrake             handbrake 0..1 (x255)
i8  gear                  GetGear() (< 0 taken as reverse)
u8  signals               car signals (seat 0 only; src/game/car_signals.h; read from the car's vehicle part,
                          docs/re-notes.md 9, NOT checked in game): bit0 park/side lights, bit1 headlights,
                          bit2 indicator A, bit3 indicator B (both = hazard; which one is left is not known),
                          bit4 beacon, bit5 siren, bit6 horn, bit7 valid (the driver's game could read them; 0 =
                          unknown: the copy keeps its own). Last byte of the block, added without a version change
                          (with the action block below: a state without both reads as 0 / no action; the two are
                          only unambiguous together, so a build must send both or neither, and builds that differ
                          are kept apart by modVersion anyway)
-- action block (melee sync; after everything above, the vehicle block included; optional, see below):
u8  actSeq                visible one-shot actions of the player so far, mod 256; a change = new action(s)
u8  act                   the latest one: 0 none, 1 light punch, 2 heavy (charged) punch, 3 counter, 4 takedown,
                          5 throw (grenade, molotov, knife), 6 melee hit reaction (an NPC punched him); other values
                          read as none
```

**Action block** (src/game/player_actions.h, unit-tested in tests/player_actions_test.cpp). Punches and other
one-shot actions are not a state, so they ride on the state as a counter plus the latest kind: a lost or reordered
PlayerState does not lose a punch, because the next state carries the same counter. The receiver (one
`actions::Tracker` per remote player, fed with the interpolated state, whose counter only moves forward) plays an
action once per counter step: the first state it sees only sets the counter, a counter that went back (an older
state) is ignored, several steps at once play only the latest kind (the skipped ones are logged and traced as
`missed`). The block is **optional on the wire**: a state that ends before it (an older build) reads as no action,
and an older build reading a state with it stops before the two bytes (its reader checks for overruns, not for
trailing bytes); so `version` stays 4. A state that ends after one of the two bytes is rejected. Where the actions
come from and how the puppet plays them: docs/game-api.md 17. The punch's damage still comes only as a Hit (0x0400);
the puppet's swing is a picture.

Since 0.9.0 a game sends its state up to 60 times a second (`net_state_hz`, 10..120; at most once per frame, so a game
running in the background at 20 fps sends 20) and receivers render it **50 ms in the past** (`net_interp_ms`, 0..500;
before 0.9.0: 20 Hz and 100 ms): `renderTime = now - offset - 0.05`, where `offset = localReceiveTime - senderTime` is tracked as a running minimum
(slowly relaxed upward, 1 % per message, to follow drift). Between two snapshots everything is lerped (heading
renormalised, car quaternion nlerp with sign fix, discrete fields from the nearer one); past the newest snapshot the
position is extrapolated with the velocity for at most 250 ms, then held. Old/duplicate snapshots (sender time not
newer) are dropped, so loss and reordering need no reliability. The interpolated position is then moved forward
along its velocity by the same 50 ms (at most 1.5 m; player and car), so what is shown is where the player is now,
not where he was 50 ms ago.

**Measured (0.9.0, two games on one PC, with the position trace that docs/sync-trace.md replaced in 0.14.0):** the network
part (the target the puppet is steered to vs. where the player really is) was 0.11 m on average and 0.47 m at most
while running with 20 Hz / 100 ms, and 0.02-0.06 m on average (p95 0.10 m) with 60 Hz / 50 ms and the lead; a car at
up to 20.6 m/s: 0.12 m average, 0.34 m max (before: 0.29 m average and 1.04 m max at only up to 9.7 m/s). The
puppet itself was the real desync: the game's AI walk toward the target starts late, is slower than a running
player and sometimes does not move at all, so a host running around was shown 2.5 m behind on average and up to 9.3 m
off (stuck until the 2 s "blocked" snap). Since 0.9.0 the puppet is pulled toward the target every tick (30 x dt of
the gap above 0.3 m, put there at once above 1.5 m) and its walk is re-aimed at once when the player turns sharply:
0.3-0.5 m on average, p95 0.4-0.7 m, in Free Ride and in Taxi, right after a lobby CHAPTER start and after a
checkpoint reload. **0.13.0 replaces the pull** (it slid the puppet's feet: the body was moved while the walk
animation played at its own pace), see "Driving the puppet and the car copy" below. Traffic for two players in Taxi with NPC sync: host -> guest 15.4 KB/s (NpcStates 10.8, PlayerState
3.5), guest -> host 4.4 KB/s, no loss, no resends, no relay drops; the checkpoint save (HostSave, ~36 KB) goes out only
when the file changed.

### Driving the puppet and the car copy (0.13.0; not yet checked in game)

The receiver turns the interpolated state into commands with core's controllers (`external/core/src/net/
remote_motion.h`, unit-tested in core's `net_motion`), one pair per remote player, in `player_sync.cpp`; the Lua glue
carries them out and reports back where the puppet / car copy is after each tick ("PS" / "VS" lines).

- **On foot** (`LocomotionController`): a gait from his speed (stand / walk / run / sprint, with hysteresis; his sprint
  flag allows sprint from 3.5 m/s), a speed multiplier for that gait (`SetMovementSpeedMult`, given right before each
  `MoveVec` so a walk is never interrupted by it), and a walk point ahead along the wanted velocity (his velocity plus
  1.2/s of the error, at most 2.5 m/s of correction), re-aimed every 0.35 s, on a gait change or a turn of more than
  12 degrees. The walk/run/sprint speed at multiplier 1 is measured on the puppet itself (0.25 s windows while gait
  and multiplier are steady, nudges not counted), so the multiplier makes the game's own animation run at his speed.
  Position fixes stay small: only the error beyond 0.9 m is nudged (2/s); a snap beyond 3.5 m (or 3 m height, 6 m while
  he is in the air), when he stands and the puppet is more than 0.5 m off for 2 s, or when it walks but does not get
  anywhere for 1.5 s while more than 0.9 m off. Standing, it turns to his heading at up to 400 degrees/s; when he
  stops it walks the last bit to his spot. While he gets in or out of a car (seat 9) his puppet stays on foot and
  walks with him to the door; next to a standing car it gets in with the game's animation (`GetOnVehicle(..., true)`,
  4 s, then seated at once as before).
- **His car** (`VehicleController`): his controls are applied to the copy every tick (`SetSteer` with the response
  curve inverted, `SetPower`, `SetBrake`, `SetHandbrake`), so its wheels steer, brake lights and engine react and its
  own physics drives it; the transform is corrected toward his (8/s for position and rotation) only as far as needed,
  and nothing is set while it is on target. If its physics does not keep up (the copy slower than 35 % of his speed for
  1 s above 3 m/s, more than 8 m/s faster, or more than 1.5 m off for 1.5 s) the copy is pinned to his transform every
  tick from then on (the pre-0.13.0 way, logged "pinned to his transform every tick"). Snap on a teleport (> 20 m
  between snapshots) or > 4 m off. No throttle while the copy is faster than him (+1.5 m/s) or he reverses. When he
  gets out the controls are released (power, brake, handbrake, steering 0).
- **His car's signals** (`signals` byte; `RV.Signals` in player_sync.lua, NOT checked in game): applied to the copy
  when they change, each change logged on both sides ("player N's car signals: ..." from the C++ side when it arrives,
  "remote car ...: signals A -> B" when it is applied, "our car's signals: ..." on the driver's game). Lights and
  indicators are set with the game's own bit setters (`mc_car_set_signals`), horn, siren and beacon with `SetHornOn`
  / `SetSirenOn` / `SetBeaconLightOn`; an on/off change of the lights also sets the script light override
  (`SetLightState(true, on)`) so the game's automatic lights keep them. Every tick the copy is read back: lights and
  indicators its own game changed back are set again at once, horn / siren / beacon at most once a second (counted,
  CR `sigre`). Brake and reverse lights are not sent: the copy makes them from the brake and gear it gets. When he gets
  out the copy's horn is switched off and its lights are given back to the game (`SetLightState(false, false)`).

### Shared cars, seats and ownership

Every car a player drives gets a network **car key** `(keyOwner, vehId)`: the id of the first player who drove it and
the car's entity GUID on that player's game. The key never changes, whoever drives the car later; every game maps
keys to its own entity (`MafiaCoop.cars` in the Lua glue): the real car on the game where it was first driven, a copy
(`ObtainSpecificCar` + `SpawnCar`, same model) everywhere else. A copy is created when a key is first seen (from a
driver's PlayerState or a CarParked) and keeps that key, so a passenger reports the key of the car he sits in on his
own game, which is the same car on every other game.

**Owner = last driver.** The game whose player sits in the driver seat simulates the car; everyone else moves their
entity for that key to the driver's interpolated transform every tick (`SetPos`/`SetRot` on a normal, unfrozen car;
`FixCarPos` is not used, docs/game-api.md 7). Puppets sit in the cars like real occupants (`GetOnVehicle`, seat
1-based, no animation): the driver's puppet in the driver seat, passengers' puppets in their seats. While a remote
player drives a car, player entry to it is locked on the other games (`LockPlayerEntryPoints`), so E there does not
carjack the remote driver's puppet.

Message flow:

1. **Drive.** A sends PlayerState with `seat 0` and key K. Every other game spawns/uses its entity for K, seats A's
   puppet as driver and follows A's transform.
2. **Ride along.** B presses E (pad Y) within 5 m of a car K driven by someone else: B's game seats B in the first free
   passenger seat (scripted: `LockControls(true)` + `GetOnVehicle`). B's PlayerState then carries K and his seat;
   the others seat B's puppet there (on A's game in A's real car). E in the car gets B out the normal way.
   Since 0.15 A's puppet must sit at the wheel of B's entity first (B's game puts it there, else no ride): with the
   driver seat empty the game slid B over to the wheel and both games had a driver for K. While B gets in, B reports
   seat 9 (docs/game-api.md 18).
3. **Get out / park.** A's PlayerState stops carrying K (on foot). The others stop moving K: their entity stays where
   it was (a parked car with normal physics, unlocked) and A's puppet climbs out. When A's car has stopped (speed
   < 0.3 m/s, or 8 s after getting out) A's game sends **CarParked** (reliable) with K and the final transform; the
   others put their entity there (creating a copy if they never had one). Its flags say whether the car is
   destroyed (`car:GetDamage()` >= 100 on A's game; the scale is unverified).
4. **Take over.** Anybody gets into the parked car K's driver seat the normal way (E). That game now simulates K and
   sends `seat 0` with K: step 1 with the new owner. On a game whose player drove K before (e.g. A's own real car),
   or whose own mission car K is (adopted, its NPC driver thrown out twice), the entity is swapped for a fresh copy
   first, because such a car throws NPC drivers out (docs/game-api.md 7): a copy we spawned is despawned, a real car
   is deactivated (hidden) and kept.
5. **The real car comes back** (0.15). A real car hidden behind a copy comes back at the copy's place when K is
   parked again: the copy is empty (the driver's puppet climbed out, nobody else in it) and A's CarParked arrived
   (or, without one, the copy stood still for 1 s 10 s after A got out). Meanwhile the copy is closed to the local
   player. It also comes back on a new world (where it is, if it survived the load), when the session ends, and
   when the copy is lost; destroyed on A's game, it is blown up on ours. The rules: docs/game-api.md 15 "Mission
   car back", src/game/car_swap.h.

Conflicts: if two players get into the driver seat of the same parked car at the same moment, each game keeps its
own player driving (the other's `seat 0` claim for K is ignored and logged) until one of them gets out; then step 3/4
apply. Copies are capped at 12 per game (the oldest parked copy nobody sits in is despawned; never one with a real
car hidden behind it). When the session ends, copies nobody sits in are despawned (a hidden real car is shown at
the copy's place); the one the local player sits in stays as a normal car (a real car hidden behind it comes back
once he got out). A player who joins late
does not see cars parked before he joined until somebody drives them.

Puppets that cannot be seated (seat taken, the game keeps throwing them out) are hidden at the car and retried every
4 s.

Puppet lifetime: created when the first PlayerState of a player arrives, removed on `PeerLeft` / transport timeout /
session end (the whole set, including the hidden "factory" car, is removed when the session ends).

Character per player (code: `src/game/characters.cpp`, docs/game-api.md 12): every player picks a `model`: an alias
from the character list (`paulie`, `paulie_suit`, `sam_hat`, `tommy_sailor`, ...; the CO-OP panel's CHARACTER and
OUTFIT rows, `model =` in MafiaCoop.ini or `--model`), a spawn profile path or a decimal profile hash. Empty = the
default for his player id: 0 `tommy` (the host, as his story dresses him), 1 `paulie`, 2 `sam`, 3 `salieri`. Unknown
aliases (a newer version's, a typo) are shown as that default.

- The pick at session start travels in core's Hello (`model`). **PlayerModel** (0x0102, reliable broadcast) carries
  the current pick and its **look**: the spawn profile his own game uses for it. The look lets a build whose list
  lacks that alias show the character anyway, and for story Tommy it is the profile his game's model manager holds
  (a decimal hash, e.g. the Free Ride outfit; reported by the Lua glue as `ML <hash>`), so everyone sees his real
  clothes. Both strings are `[A-Za-z0-9_/.-]`, at most 96 bytes, else ignored.
- PlayerModel is sent when a session starts, when the player picks something else in the panel (at once: the others'
  puppet of him is replaced live), when his story look changes, and whenever a new player appears in the session
  (the Hello model is fixed for the session, so a late joiner would otherwise see the old pick).
- Receivers show a remote player's puppet with: `puppet_model` (this machine, tests) > his PlayerModel look > his
  PlayerModel / Hello model resolved locally. A profile that does not load within 12 s is marked bad on that machine
  and the default character for his id is used instead.
- His own game switches the local player with the model manager's override slot (`SwitchOverridePlayerSpawnProfile`,
  docs/game-api.md 12; re-applied every 3 s and after every load, cleared when the session ends; `tommy` = no
  override). A switch that has not finished after 12 s falls back to the default for his id the same way.

Fire, aim, crouch, reload: a receiver turns `aim` (or a new shot, kept for 1 s as hip fire) into
`DbgTesting_AimAtDir(ax, ay, az)` on the puppet, fires one real shot per new `shots` count (trigger press/release, at
most a few per tick; a far backlog is skipped), `crouch` into `SetStalkMove`, and a rising `reload` into a reload
press. Shots are not separate messages: they ride on the player state (60 Hz), so a lost packet only delays them by a frame or two.

## Phase 6: damage between players (app ids 0x0400-0x04FF)

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0400 | Hit | attacker -> victim (`SendTo`, relayed by the host) | R | u8 victim, f32 amount, u8 damageType, u8 hitZone, u32 weaponId, f32 dx, dy, dz, u8 source (0.9.0: 0 the sender's player, 1 an NPC of the sender's game), u32 npc (its entity id there) |

The attacker's game decides what hit. Puppets are AI humans that take damage like any NPC (their own hit reaction plays
on the attacker's screen) and never die from it there (health kept at 5000). When the local player's bullet, punch or
car hurts the puppet of player V, the attacker's game sends **Hit** to V:

- bullets and anything else with a DAMAGE event: amount, `enums.DamageType` (0 shot, 3 melee, 10/11 car), `enums.Hitzone`
  and the weapon id from the puppet's DAMAGE message (one blanket DAMAGE subscription per game, the receiver GUID says
  which puppet and the enemy GUID must be the local player or the car he drives);
- car contact sends one DAMAGE per frame: these are summed for 0.25 s into one Hit;
- punches (key Q, only outside safehouse zones, docs/game-api.md 11): a puppet with a story profile (AI type 0) is a
  melee target like any NPC, so its DAMAGE event (type 3, 10 for a light and 36 for a charged punch) sends the Hit.
  Puppets with an ally or Tommy profile are never targeted by the game's melee; for those, our swing
  (`MELEE_LIGHT_ATTACK` / `MELEE_HEAVY_ATTACK` with no opponent) is a Hit of 10 / 36 when a puppet stands within 1.9 m
  in front of us 0.25 s after it (the puppet gets a small `DoDamage` for its flinch);
- `dx dy dz`: unit vector from the attacker to the puppet (unused by receivers so far).

The attacker's swing itself is shown on the victim's game by the action block of his PlayerState (above): his puppet
plays a punch animation there. That swing is only a picture: while a puppet within 3 m swings, the local player is a
demigod for the swing (plus 0.3 s), a Hit arriving meanwhile still applies (`DoDamage` with the demigod switched off
for the call), and should the puppet's swing land anyway (melee DAMAGE with that puppet as enemy) the health is given
back (docs/game-api.md 17).

The victim's game applies it to its real player:
- a bullet (type 0) is replayed as one real shot from the attacker's puppet at the hit body part
  (`game.shotmgr:CreateShots`), so the game's own bullet reaction and damage happen; the victim's "picture shot" shield
  (the demigod moment after a puppet fires near him) is opened for it. If it has not landed after 0.3 s (seen in the
  victim's own DAMAGE event, enemy = that puppet) the hit is applied with `DoDamage(amount)` instead;
- everything else: `DoDamage(amount)` (the game's flinch and damage vignette).

Deaths: a player's health in PlayerState is 0 while he is dead or downed; every puppet of his then dies (a big
`DoDamage` on it) and is spawned again when his health comes back.
- Guests are never killed by a Hit: hits are capped, at 12 health or less the guest is **downed** (demigod, controls
  locked, reported as health 0) and after 5 s put behind the host's puppet with full health. A replayed bullet is only
  used while the guest would stay above that line.
- A guest who dies anyway (an NPC, a fall, a car) has his last checkpoint loaded during the death camera
  (`game.savesystem:Retry()`, so the YOU DIED menu, where scripts do not run, is skipped); 3 s after he is back in the
  world he is put next to the host.
- The host dies like in single player: YOU DIED, his choice in the menu, his checkpoint loads; guests see his puppet
  die and come back.

`friendly_fire = 0` (MafiaCoop.ini or `--friendly_fire 0`) on a machine: its player sends no Hits and ignores the ones
he gets.

## Phase 4: world (app ids 0x0200-0x02FF)

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0200 | WorldEnv | host -> guests (`Broadcast`) | R | f32 timeOfDay (hours 0..24), f32 timeFlowSpeed, str weatherSet |

WorldEnv is sent while the host is in the world: every 4 s and at once when the weather set or the time flow speed
changes. A guest in the world sets the weather set when it differs (`SetWeatherSet(name, 0, 0)`), the flow speed when it
differs, and its clock when it is more than 0.05 h (3 game minutes) off (`SetTime(t, 0)`). Guests' own weather/time
scripts may change things in between; the next WorldEnv puts them back.

### Story: chapter, checkpoint, objectives, cutscenes (0.6.0)

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0201 | MissionState | host -> guests (`Broadcast`; `SendTo` a new player) | R | str quest, str checkpoint, str objective, str overrideObjective, str saveId, u8 inWorld |
| 0x0202 | HostSave | host -> guests (`Broadcast`; `SendTo` a new player) | R | u32 seq, str saveId, u32 size, size bytes (the save file, at most 200 KB) |
| 0x0203 | Cutscene | host -> guests (`Broadcast`; `SendTo` a new player) | R | str name (`cine_..._cs_object` without the suffix; "" = none) |
| 0x0205 | MissionFail | guest -> host (`SendToHost`) | R | u32 seq (counts the guest's failures), u32 reason (the last GameOver id, 0 = unknown), str quest, str checkpoint (the guest's point) |
| 0x0206 | FailVerdict | host -> guests (`Broadcast` for HOST_FAILED; `SendTo` the reporting guest otherwise) | R | u8 kind (1 HOST_FAILED, 2 REJOIN, 3 WAIT), u32 seq (the MissionFail it answers; 0 for HOST_FAILED), u32 reason |

- MissionState: `quest` = `GetActiveQuest()` (`mm_110_omerta`), `checkpoint` = the quest's custom state (`cp_020`),
  the objective ids (`active_objective`, `override_objective`), `saveId` = `GetLoadedSaveId()`, `inWorld`. Sent at
  once on a change and every 5 s, also from the main menu (then all empty but the save id, inWorld 0). Strings are
  checked on receipt (`[A-Za-z0-9_.!]`, at most 96 bytes); anything else drops the message.
- HostSave: the newest `checkpoint_*` save file under the host's `Documents\My Games\Mafia Definitive Edition\Data`
  (`*.sav`, the save id read from the file) of the point he is at; at the main menu the newest one (what CONTINUE
  loads). The host looks every 2 s and sends a file when its bytes changed; `seq` counts them. Guests keep only the
  newest in memory; nothing is written to their disk.
- Cutscene: the host polls the cutscene objects of his chapter (`IsCutscenePlaybackInProgress`, every 0.25 s) and sends
  the one that plays, and "" when it ended or was skipped.
- Guests that follow the host (lobby STORY or CHAPTER, JOIN GAME): load the host's point (docs/game-api.md 14) and,
  in the world, mirror his objective ids and play/stop his cutscenes. A guest is "at" a point when his own
  `GetActiveQuest()` / checkpoint equal it; when the host's point differs for `follow_grace` seconds (default 8) and
  no cutscene plays, the guest quits to the main menu and loads the host's save of that point (one attempt per host
  point; without a save of it he waits for the next HostSave). 0.11.0: when the host leaves his world (inWorld 0 or
  no quest: a failed mission, a death reload) while the guest is in his, and comes back at the point the guest is at,
  the guest loads that point again too (the host's world restarted the checkpoint; its NPCs live again).
- Deaths and failed missions in a chapter loaded through the chapter loader (replay flags: lobby CHAPTER, every guest
  load): a following guest who dies reloads the host's point the same way (no Retry: in a replay-style load the game
  answers a death or a failed mission with the free roam state from before the quest, another world). The host who
  dies there, or whose mission fails, goes back to the main menu and loads his own newest save of the point he was at
  (`mc_story_reload`, `save_guard::MissionAborts`); guests wait meanwhile. A host in a normal story load (lobby STORY,
  CONTINUE) keeps the game's own YOU DIED / checkpoint flow and its "You abandoned the objective" screen.
  Since 0.9.1 the free roam load the game starts on a failed mission never reaches the player's own saves: in a
  co-op chapter (save slots blocked) it gets the chapter loader's entry (in practice the Free Ride start) and the
  takeover leaves that world as soon as it is up (docs/re-notes.md 7). While the host is in it his MissionState
  carries that save id (`freeroam_...`), so guests do not follow him there.
- **Failed missions (0.15; MissionFail 0x0205, FailVerdict 0x0206; src/game/fail_flow.h, re-notes 7).** Before, a
  failure in a co-op chapter took two loads: the game's own answer (the free roam state before the quest, the "Free
  Ride stop") and then our reload of the checkpoint from the main menu. Every failure kind (objective abandoned, the
  mission car lost, a timer run out, an ally dead, a target escaped, arrested) and a death the game handles itself
  end in the save system's FreeRoamRetry; it is skipped now and the player goes straight to the checkpoint:
  - chapter host (lobby CHAPTER): screen faded out, at once to the main menu, his newest checkpoint of the session
    loads (one load). He broadcasts FailVerdict HOST_FAILED;
  - STORY host: his game shows its fail screen as in single player (Load Last Checkpoint = one load); when the
    director's GameOver fires he broadcasts HOST_FAILED too;
  - guest: skips it, leaves for the main menu at once and sends MissionFail. The host answers REJOIN when he is in
    his world at a chapter point and not failing himself (the guest loads the host's point now; the host plays on),
    else WAIT (the guest loads the host's point once the host has left his world and is back at a chapter point).
    No answer within 6 s counts as REJOIN;
  - a guest that gets HOST_FAILED leaves his world at once (also when his own failure is under way) and loads the
    host's point once the host is back: one load on every game, also when the mission fails on both at the same
    moment (a timer). If the host never leaves his world, the guest loads his point after 90 s;
  - while a failure is handled the follow logic does not load or reload on its own, and a death adds no reload; the
    game's second request of the same failure is skipped too. If the player is still in the failed world 20 s after
    skipping (the quit did not happen), or the game loads the free roam state anyway, the 0.9.1 way runs (Free Ride
    stop, then the reload). A peer without these messages ignores them; a guest whose host does not answer goes back
    after 6 s; a host without them does not send HOST_FAILED (the guests then follow his reload as in 0.11.0).
- A guest's game in a co-op session writes its saves (checkpoints, quest state, quit, Free Ride autosaves) to `%TEMP%\MafiaCoop\coop-saves\<pid>`
  instead of his save slots (docs/re-notes.md 7); his real slots are never opened for writing during the session.

### NPCs (0.7.0; 0.11.0 changed NpcStates and NpcHit; 0.12.0 NpcStates)

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0210 | NpcStates | host -> guests (`Broadcast`) | U, 10 Hz (0.9.0; was 5) | u8 n (at most 16, fewer so the payload stays under 1000 bytes), then n x: u32 key, f32 x y z, i8 hx hy (heading x127), u16 health (0.11.0; was u8), u8 flags (1 dead, 2 in a car), u8 aiType, u32 tag (0.11.0: the battle archetype, hashed; 0 = none), str weapon ("-" = none), u64 profile (0.12.0: its spawn profile hash, 0 = unknown), u8 shots (0.12.0: shots it fired, mod 256) |
| 0x0211 | NpcDeath | host -> guests (`Broadcast`) | R | u32 key |
| 0x0410 | NpcHit | guest -> host (`SendToHost`) | R | u32 key, f32 amount, u8 damageType, u8 hitZone, u32 weaponId, u8 flags (0.11.0: 1 = lethal, the guest's copy died of his hits) |

- key = the NPC's entity id on the host's game (the number in its `C_EntityGuid`). The host sends the 48 nearest NPCs
  within 120 m of himself or any guest's puppet, 5 times a second (several messages when more than 16); a death also
  goes out reliably at once.
- A guest pairs each host NPC with the nearest human of his own world within 1.5 m of it (the same mission scripts
  spawn the same NPCs at the same spots on every game, docs/game-api.md 15) and keeps the pairing. 0.11.0: a host NPC
  with a job (AI type not 0) that found none there within 2 s is paired with the nearest free local human of the same
  tag (or, when one side has no tag, the same AI type) within 40 m: after a guest's reload, a late join or NPCs that
  streamed in late the fight has moved on. All candidate pairs are taken nearest first (same spot before moved apart,
  the same tag counts at half its distance); dead host NPCs are paired too (their copy is killed). Deactivated humans
  (`IsActive()` false) are neither sent nor paired. A pairing is released after 5 s without a state. A paired copy:
  dies when the host's dies (killed; one that still does not die after 3 tries is hidden); its health follows the host's
  NPC down (never up); is put back to the host's position when it is more than 6 m off for 1.5 s (NPCs with a job
  only: AI type not 0, not in a car). A hit by the guest on a copy hurts it on his game and goes to the host as NpcHit;
  the host replays a bullet as a real shot from the guest's puppet (`CreateShots`, so the NPC's own hit reaction and
  damage apply), else `DoDamage(amount)`; a replay that does not land within 0.35 s is `DoDamage`. When the guest's
  copy dies within 1.5 s of his hit, he sends a lethal NpcHit (no damage of its own): 0.8 s later the host kills his
  NPC if it still lives (an enemy that his mission keeps invulnerable is made vulnerable for it; an invulnerable
  ally or civilian is left alone).
- Enemies and guests (0.9.0): on the host each guest's puppet gets the FRIEND brain 2 s after it spawned
  (`SwitchBrain(1)`, docs/game-api.md 15; ini `enemies_target_guests = 0` turns it off), so the host's enemies shoot at
  it. Their hits on a puppet go to that guest as a Hit with source 1 and the NPC's key; the guest takes it (an NPC
  bullet as ~2.6x its DAMAGE amount, the downed rules apply, friendly fire does not matter) unless his own copy of
  that NPC is alive and hostile: then it fights him on his own game already and the relayed hit is skipped, so he is
  not hurt twice. Hostile copies are put back only when more than 12 m off (6 m for other NPCs with a job).
- Spawned copies (0.12.0): a host NPC with a job (AI type not 0), on foot, alive, with a known profile, within 90 m of
  the guest, that found no partner for 5 s (2 s for the loose pairing, then 3 s more; none in the first 10 s after
  the guest's world loaded) gets a copy on the guest's game: a human from a runtime spawner (like the player
  puppets) with that spawn profile, its health, and the NPC's weapon. The host knows the profile from the NPC's name:
  a human a mission spawner made is called `behavior_object_prime-NN-<spawner name>`, and the spawner (entity type
  65, by that name, the nearest when several share it) tells its profile (docs/game-api.md 15); 0 for NPCs from
  traffic or police generators, which then get no copy. The copy is driven from the states like a player puppet
  (walk/run toward the position plus the velocity of the last two states, pulled along, heading when standing), aims
  and fires when the shot counter goes up (along its heading; past the guest, 1.6 m beside and 2 m above him, when it
  faces him: its bullets are only a picture, the host's NPC already shot at the guest's puppet and that hit comes as
  a Hit). It is paired like any copy: the guest's hits go to the host as NpcHit, a lethal one kills the host's NPC,
  the host's death kills it; relayed hits of that NPC are always applied (it never fights the guest itself). It is
  released 12 s after its death, 5 s after the last state (out of range, gone), when the NPC gets into a car, when
  the guest is more than 110 m away, during a cutscene, when the session ends and with the world (loads). When the
  guest's own game spawns that NPC after all (a free local NPC at the same spot, or of the same tag within 40 m), his
  own takes over and the copy is released: no doubles. At most 10 copies at a time, one spawn profile loading at a
  time. Unpaired NPCs without a job (ambient pedestrians that differ per game) still get no copy.
- Ambient pedestrians stay local to each game; ambient traffic and police cars are the host's near him (below,
  "Traffic"). Cars a player drives are shared as before (Phase 3); a
  remote driver's car is first looked for here: a car of the same model standing where his car stands (within 3 m)
  is taken as his car (mission cars are not doubled), and no copy is spawned into another car standing there.
  Such a mission car does not keep an NPC at its wheel: when the remote driver's puppet is thrown out of it twice
  (0.9.0), it is swapped for a fresh copy (the mission car hidden with `Deactivate`, the copy spawned in its place) and
  the puppet drives the copy; when the remote driver parks it, the mission car comes back at the copy's place. No
  copy is looked for or spawned during a loading screen and for 4 s after it (a copy spawned before the mission had
  placed its car landed on it).

### Guest-owned NPCs (0.15.0; not yet checked in game)

The reverse of the copies above: NPCs that only a guest's game has. Every game spawns a checkpoint's waves for its
own player, so a guest who got somewhere first has enemies the host's game has not spawned (yet); the host saw
nothing of them before.

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0420 | GuestNpcStates | guest -> host (`SendToHost`) | U, 10 Hz | the NpcStates body (u8 n at most 16, then n x the same NPC record); key = the NPC's entity id on the guest's game |
| 0x0421 | GuestNpcEnd | guest -> host (`SendToHost`) | R | u32 key, u8 reason (0 died, 1 handed back, 2 gone, 3 claim refused), u32 hostKey (1 and 3: the host NPC), u16 health (its last), u8 flags (1: it died of the host's hit) |
| 0x0422 | HostNpcHit | host -> that guest (`SendTo`) | R | u32 key (the guest's NPC), f32 amount, u8 damageType, u8 hitZone, u32 weaponId, u8 flags (1 = lethal: the host's copy died of his hits) |
| 0x0423 | NpcClaim | host -> that guest (`SendTo`) | R | u32 guestKey, u32 hostKey, u8 flags (1: the guest's NPC is dead) |

The ownership rules are C++ (`src/game/npc_owner.h`, unit-tested by `tests/npc_owner_test.cpp`); the game side is
npc_sync.lua.

- **Which NPCs (guest):** 10 times a second the guest reports his NPCs with a job (AI type not 0) within 100 m (the
  64 nearest; none in the first 10 s after his world loaded or during a cutscene) with the host NPC each is paired
  with. One that stays unpaired for 5 s (alive) becomes **his own**: the 16 nearest at most, their states go to the
  host as GuestNpcStates (at most 16 a round, one message). Spawned copies (his copies of host NPCs) are never
  reported, and the C++ side drops any id in the copy list again (no feedback loop). A paired moment restarts the 5 s;
  an NPC that died before it was his own is never sent.
- **Mirrors (host):** every guest NPC gets a mirror id (unique on the host; at most 24 per guest). It is shown like a
  guest's copy of a host NPC (above), the other way round: a human from the copy spawner with the NPC's spawn profile
  (the guest reads it from the NPC's spawner name like the host does), its weapon and health, driven from the
  guest's states (walk/run, heading, its shots as pictures), a civilian on the host's game (no fight of its own), at
  most 10 at a time, within 90 m of the host, released 12 s after its death, 5 s after the last state, in a car, in
  a cutscene, with the session and the world. Mirror copies are never in the host's NpcStates (no NPC goes back to the
  game it came from). Other guests do not see a guest's own NPCs.
- **Hits:** the host's hit on a mirror's copy goes to its guest as HostNpcHit; he replays a bullet as a real shot
  from the host's puppet on his game (`CreateShots`; else, or when it does not land within 0.35 s, `DoDamage`), so
  the NPC turns on the host's puppet. When the copy dies within 1.5 s of the host's hit, the hit is lethal: 0.8 s
  later the guest's NPC is killed if it still lives (the same rules as a guest's lethal NpcHit). The guest's own NPCs
  shoot at the host's puppet on his game (it gets the FRIEND brain there while the guest has NPCs of his own,
  player_sync.lua `M.Ally`); those hits go to the host as a Hit with source 1, which he takes like any NPC's.
- **Deaths:** the guest's NPC dies -> GuestNpcEnd died -> the mirror's copy dies on the host (health also follows the
  guest's down, never up). The host kills the copy -> lethal HostNpcHit -> the guest's NPC dies.
- **Back to the host (no doubles):** when the host's own game spawns that NPC (the host arrives), it is paired again
  and the host is authoritative for it. Either the guest's usual pairing finds the host NPC (same spot, or same kind
  within 40 m after 2 s) or the host **claims** it: a host NPC (alive, with a job, sent for 2.5 s, no copy) at the
  same spot as a mirror (1.5 m, same tag or none) or of the same tag within 40 m (8 m height) is sent to that guest
  as NpcClaim (pairs taken nearest first, same spot first; one claim per mirror and per host NPC at a time). The
  guest pairs them (a spawned copy of that host NPC goes), or answers GuestNpcEnd claim refused (his NPC is paired
  with another host NPC, that host NPC with another of his, or no state of it yet); no answer in 3 s counts as
  refused, and a refused pair is not tried again for 15 s. On the pairing the guest sends GuestNpcEnd handed back
  with its health: the host releases the copy and lowers his own NPC's health to it. A late GuestNpcStates of an
  ended NPC (unreliable after reliable) makes no new mirror for 3 s.
- **Dead mirrors:** a mirror that died is remembered for 60 s: a host NPC that spawns at its place (same rules)
  in that time is claimed dead: the host kills his NPC (the guest already killed it) and the guest pairs it with his
  dead one, so it gets no copy there.
- **Gone:** an NPC of his own out of the guest's rounds for 2 s (out of 100 m, despawned) ends (GuestNpcEnd gone); a
  mirror without a state for 5 s ends on the host; a guest who leaves takes his mirrors along.
- Bandwidth: at most 16 NPCs x 10 Hz x ~45 bytes = ~7 KB/s per guest (like the host's 48 x 10 Hz).

### Traffic (0.15.0; not yet checked in game)

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0220 | TrafficSpawn | host -> one guest (`SendTo`) | R | u8 n (at most 16), then n x: u16 net, u8 kind (0 traffic, 1 police), str model, i16 color1 color2 (`GetColor(1)` / `GetColor(2)`, -1 unknown), str plate ("" unknown), f32 x y z, u32 quat (smallest three), i16 vx vy vz (1/100 m/s), u8 flags |
| 0x0221 | TrafficDespawn | host -> one guest (`SendTo`) | R | u8 n, then n x u16 net |
| 0x0222 | TrafficStates | host -> one guest (`SendTo`) | U, up to 20 Hz per car | u32 hostMs (the host's clock), f32 origin x y z, u8 n (at most 40: 897 bytes), then n x 22 bytes: u16 net, i16 x y z (1/32 m from the origin, +-1023 m), i16 vx vy vz (1/100 m/s), u32 quat, i8 steer (x127), u8 power, u8 brake (x255), u8 flags |
| 0x0223 | TrafficBump | guest -> host (`SendToHost`) | R | u16 net, f32 error (m), f32 copySpeed, f32 hostSpeed |

- **What is traffic:** a car of the host's game whose driver the traffic or police generators made (its name is
  `behavior_object_prime-NN-` with an empty suffix, docs/game-api.md 15), not a player car (player_sync's, Phase 3),
  not a puppet factory's, not the one the host sits in; police when the driver's AI type is 4 or the model name says
  so. Parked cars (no driver) and mission cars (drivers named after a mission spawner) stay local to each game as
  before. A traffic car whose driver died or got out is shared for 20 s more (flag 4), then it counts as parked.
- **Host:** the Lua glue (`traffic_sync.lua`) scans the car table once a second and reports the traffic cars within
  380 m of any guest (his puppet, or his car) 20 times a second: transform, velocity, the driver's controls
  (`mc_car_controls`, flag 8), reversing (velocity against the car's heading, flag 16), handbrake (flag 1), siren
  (flag 2, police only: no siren getter exists, so it is on while the car goes faster than 14 m/s and 8 s after). The
  dll (`game/traffic_model.h` HostTraffic) gives each car a net id (u16, never reused while a guest has it) and keeps
  core's InterestManager (Kind::Vehicle: enter 260 m / leave 320 m from the guest, every tick up to 60 m, every 0.4 s
  at 260 m, x4 for cars below 0.3 m/s, at most every 2 s; 40 KB/s per guest). Each guest gets the **nearest 24**
  relevant cars (a car beyond 150 m gives way to one within 90 m): a TrafficSpawn when one is admitted, a
  TrafficDespawn when it leaves his range, is pushed out or is no longer traffic on the host's game, and its states in
  TrafficStates when the interest manager says it is due and its quantised state changed (an unchanged one is sent
  again after 1.5 s). About 22 bytes x 20 Hz per car near him: ~10-15 KB/s for a busy street.
- **Guest:** within 250 m of the host (back to his own beyond 400 m; the host's puppet or car gives his position) the
  guest shows the host's traffic: his ambient traffic spawning (`SetEnableAmbientTrafficSpawning(false)`) and police
  spawning (`PreventPoliceSpawning(true)`) go off, and 6 s later his own moving traffic cars farther than 70 m are
  hidden (`Deactivate`, the car and its crew; shown again when he goes back to his own traffic). The nearest **20**
  host cars within 240 m (dropped beyond 290 m; a far copy gives way to a car at less than half its distance) get a
  copy: `ObtainCarWithCrew(model, {profile})` (a profile from `data_traffic.crew_presets`, else the puppet factory's;
  `ObtainSpecificCar` when that fails), `SetDespawnImmunity`, `DisableExplosion`, the crew locked
  (`LockControls(true)`, no reaction to sounds), the host's colours and plate. Each copy is driven like a remote
  player's car (Phase 3 "Driving the puppet and the car copy"): the states are interpolated 120 ms behind the newest
  (host clock: the smallest local-minus-host offset seen), moved forward by velocity x 120 ms (at most 2 m), then
  core's VehicleController: the host car's controls, a blended correction, pinned when its physics does not follow,
  snapped when more than 4 m off. Copies farther than 90 m are driven every third tick. A copy goes when its car is
  despawned (1 s later, so player sync can take it first), after 5 s without a state, when the guest goes back to
  his own traffic, and with the world; near ones (80 m) with `DespawnEntity` (when unseen), others at once. Siren
  and beacons follow flag 2; a host car without driver (flag 4) makes the copy's driver get out (npc_sync kills him
  when the host's driver died: traffic drivers are NPCs too and pair by position).
- **Taking a traffic car:** when the host takes a car that is a traffic copy on a guest's game, the guest's player
  sync finds that copy where the host's car is (same model, within 3 m) and adopts it; the traffic glue then hands
  it over (its crew gets out, `TH`) and no longer drives or removes it. When a guest takes a copy, his game makes it
  his car (player sync), the host's game adopts its own traffic car there as the guest's car, and its traffic
  driver gets out.
- **Hits and rams:** a guest's copy is a real car on his game: his bullets and his car hit it there. The host's car
  is not moved by a guest's game; the guest's own car is a copy on the host's game (Phase 3) and hits the host's car
  there by itself. A copy knocked more than 3 m off its target (that was on it a moment before) is put back by the
  controller and reported (TrafficBump, at most every 2 s per car), which the host logs. Shots at a copy's driver go
  through npc_sync (NpcHit) like any paired NPC. Damage to the cars themselves is not shared.

### Explosions (0.16; not yet checked in game)

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0240 | Explosion | the game where it happened -> everyone (`Broadcast`; a guest's is relayed by the host) | R | u32 seq (per sender; a new session starts elsewhere: `(GetTickCount() & 0xFFFF) << 16`), u8 kind (0 unknown, 1 grenade, 2 molotov, 3 dynamite `exp_satchel`, 4 crash object, 5 car blast), u8 flags (1 fire: the picture burns too; 2 one of the sender's NPCs threw it), f32 x y z, str weapon (`exp_grenade`, ...; "" unknown; at most 32 of `[A-Za-z0-9_-.:/]`), u32 npc (flag 2: the NPC's key on the sender's game: on the host its entity id, as in NpcStates; on a guest his own NPC's key, as in GuestNpcStates), str object (kind 4: the crash object's scene name, at most 64; "" none) |
| 0x0241 | CarExploded | the car's authoritative game -> everyone (`Broadcast`) | R | u8 what (0 a shared player car; 1 a host traffic car, only from the host), str key (what 0: the shared car key `o:g`, Phase 3; what 1: its traffic net id, Traffic above), f32 x y z |

Damage is decided by **one** game; the picture is shown on all (src/game/explosion_model.h, unit-tested by
tests/explosion_test.cpp; the game side in explosion_sync.lua; docs/game-api.md 20).

- **Who sends.** The game where the explosion really happens and that is authoritative for its cause:
  - our player threw it (grenade, molotov, dynamite) or set it off (a barrel he shot within 1.5 s; an explosion
    inside his own blast within 2.5 s: a chain). The detector merges the HEAR_SOUND reports of one explosion (every
    human that hears it gets one: same spot within 4 m and 0.6 s) and decides 0.2 s after the first: ours when the
    sound's owner is our player, PLAYER_GRENADE_EXPLOSION came to our player within 0.6 s, or a throw of ours
    (WEAPON_THROWN, the selected explosive) up to 12 s before within 70 m (one throw, one explosion; the oldest
    pending throw first). Kind and weapon from the throw; a molotov also when a BURNING_FIRE sound is within 9 m and
    2 s (flag 1);
  - on the host one of its NPCs threw it, on a guest one of his own NPCs (guest-owned NPCs above): flag 2 with its
    key. Receivers show those only where that NPC is shown by a spawned copy (which runs no fight of its own); a game
    that runs its own copy of the NPC (a paired mission NPC) has that NPC's own grenades already;
  - a car: the game whose player drives it (or, parked, whose own explosion blew it up; on the host any shared
    traffic car), seen as CAR_HAVE_PROBLEM with `GetCarProblem() == EXPLODE`.
  Nothing else is sent: explosions of a mission script (dynamite in a mission, scripted car bombs) happen on every
  game by its own mission; an explosion of someone else's (an enemy of a guest's own fight) is that game's.
- **Who is hurt, once.** The sender's own DAMAGE events decide, through the usual paths: a puppet in the blast or the
  fire sends **Hit** (Phase 6; fire summed for 0.5 s into one Hit, like a ram) to its player; on a guest a copy of a
  host NPC sends **NpcHit** (fire summed per NPC for 0.5 s; a copy that died of it makes the hit lethal); on the host
  a guest's own NPC's copy sends **HostNpcHit**; the host's own NPCs and a guest's own NPCs on his own game simply
  take it. An NPC's explosion on the host hurting a guest's puppet goes to him like any NPC hit (Hit, source 1).
  The sender keeps an **own zone** where its explosion went off (blast 14 m / 2.5 s; molotov fire 9 m / 35 s;
  UNVERIFIED reach and duration): explosion or fire damage there whose message names no attacker (or a non-human
  one) counts as its player's, so it is sent on too.
- **The picture.** Each receiver within 400 m shows `game.shotmgr:CreateScriptExplosion(nil, pos, weapon, 0, false,
  0, 0, 1.5, fire)`: no owner, damage 0, no stun, both damage ranges 0, fire for a molotov (docs/game-api.md 20).
  Before it goes off a **remote zone** is marked there (same sizes): explosion / fire damage inside it whose attacker
  is nobody, a puppet, a spawned NPC copy or anything that is not one of this game's own fighters is **undone** (the
  local player gets his health back on the next tick, minus what real Hits took meanwhile; an NPC its health from
  just before the picture; puppets and copies need nothing). An NPC the picture killed cannot be brought back
  (logged). A crash object the sender named is hidden (its own blast here would hurt people a second time).
  Explosions are reliable and numbered: one relayed twice is shown once.
- **Cars.** CarExploded: the receiver's entity for that car (the copy of a remote driver's car, his adopted mission
  car, or the copy of a host traffic car) gets `DisableExplosion(false)` (copies have it disabled) and `SetExploded()`
  (the wreck without a blast; UNVERIFIED), and a picture of the blast with a remote zone. A swapped car whose real car
  is hidden behind the copy: when the real car comes back wrecked (CarParked, Phase 3) it is set exploded too, no
  second blast. The driver's game takes a car that blew up as wrecked for CarParked whatever `GetDamage` says.
- **Not carried:** the projectile's flight (no harmless projectile exists: `ThrowProjectileWeapon` throws a real
  one; the throw animation shows on the thrower's puppet, the explosion appears where and when it happened); damage
  to cars from another game's explosion (a copy cannot blow up: a remote player's grenade does not wreck your car);
  how a fire spreads (each picture burns on its own game).

## Reconnect (no messages of its own)

Core does the transport and session part (external/core/docs/protocol.md "Reconnect": slot held 60 s on the host,
the guest rejoins with backoff for 60 s and his resume token, PeerDropped to the others, `PlayerInfo::joinSeq`). What
the game layer does with it:

| Who | When | What |
|---|---|---|
| host, other guests | a player is dropped (`GameModule::OnPlayerDropped`) | his puppet is removed (`PlayerSync::OnPlayerLeft`, as when he leaves); banner "<NAME> LOST THE CONNECTION - WAITING FOR THE PLAYER TO RECONNECT... 45 S" (`Session::DroppedPlayers`) |
| lost guest | `OnConnectionLost` | the game keeps running (no menu, no load). The session is inactive: PlayerSync removes the puppets and stops sending (the trace stays open: `RCN ev=sync-paused`), MissionSync keeps the host's point and our `_followPoint` (nothing reloads by itself) and forgets the host's cutscene, NpcSync's Lua drops the copies it spawned (it does that whenever the session is inactive); banner "CONNECTION LOST - RECONNECTING... 12 S" |
| host | the player is back in `Players()` with a new `joinSeq` | MissionSync sends him MissionState, his HostSave (the newest checkpoint save) and the Cutscene, as to a newcomer; PlayerSync sends our PlayerModel; NpcStates reach him with the next 10 Hz send |
| lost guest | `OnReconnected(sameSlot)` | PlayerSync is active again: every player is new (`_seenPlayers` cleared), so we send our PlayerModel and the puppets are spawned from the next states. MissionSync: the host's MissionState and HostSave arrive; at the same checkpoint nothing happens (the save is the same `#seq` or our point equals his), at another one the usual follow (quit to the menu and load his point after `follow_grace`). |

| host | a player is dropped | NpcSync drops his mirrors (his own NPCs; `HostMirrors::OnPlayerLeft`, the Lua side drops their copies), TrafficSync forgets him as a peer (`HostTraffic::RemovePeer`: nothing is sent to him; when back he is a new peer and gets every near car spawned again) |
| lost guest | the session is inactive | TrafficSync's Lua drops the host-traffic copies and brings our own traffic back (spawning on, the cars it hid shown again); the C++ side forgets the host's cars. Our own NPCs (GuestOwner) stay ours and are sent again after the rejoin (the host makes new mirrors). MissionSync gives up its fail role meanwhile (`FailRole::None`): a mission failing in the gap takes the game's own way (FreeRoamRetry, then the usual follow after the rejoin) instead of a report nobody would send |

- Newcomer detection keys by `(id, joinSeq)` (PlayerSync `_seenPlayers`, MissionSync `_seen`), so a drop and a resume
  between two ticks (a guest's new Hello on the live Steam connection) still re-sends everything.
- A guest that comes back after the 60 s hold is a new player for the host (maybe another id): the same re-send
  happens; his characters default by the new id unless he picked one (`model =`, re-sent as PlayerModel).
- Logs: `reconnect: ...` (module.cpp), `mission: connection to the host lost ...` / `back in the host's session ...`
  / `player N is new (joined or back after a lost connection)`, `sync: connection to the host lost ...`. Trace
  records `RCN` (docs/sync-trace.md).
- UNVERIFIED in game: that the guest's world keeps running cleanly while inactive (puppets removed, NPC copies
  dropped, the story Lua with `active = 0`), that his NPC copies re-pair after the rejoin, and Steam P2P reconnects
  (lobby membership after a drop).
