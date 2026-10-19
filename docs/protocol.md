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
ends with the driver's controls (6 bytes). 0.15.0 (core 0.11.0, `kProtocolVersion` 9: the reconnect trailers and
PeerDropped (8), lanes and the session clock (9), core docs/protocol.md "Reconnect" / "Latency"): guest-owned NPCs, GuestNpcStates (0x0420), GuestNpcEnd (0x0421), HostNpcHit (0x0422), NpcClaim (0x0423) (see "Guest-owned NPCs"); traffic, TrafficSpawn (0x0220),
TrafficDespawn (0x0221), TrafficStates (0x0222), TrafficBump (0x0223) (see "Traffic"); failed missions, MissionFail
(0x0205), FailVerdict (0x0206); PlayerState still version 4 but with the car signals byte and the action block at
its end (both always sent); CarParked with a trailing flags byte (wrecked). World items (0.15.0): ItemStates (0x0270),
ItemTake (0x0271), ItemGone (0x0272), Collectible (0x0273), additive (see "World items"). Traversal (0.15.0): action
kinds 7..17 and the pose block (u8 [+ u32]) after the action block, both optional on the wire (see "PlayerState").
Doors (0.15.0): DoorEvent (0x0230), DoorApply (0x0231), DoorSnapshot (0x0232), DoorSnapRequest
(0x0233), additive (see "Doors"); action kinds 18 door kick, 19 lock pull, 20 door open, read as none by older builds.
Police (0.15.0): PoliceState (0x0260), PoliceEvent (0x0261), PoliceChase (0x0262), additive (see
"Police"); TrafficStates flag 2 (siren) of police cars now from their real siren / beacon when it can be read.
Explosions (0.15.0): Explosion (0x0240), CarExploded (0x0241), additive (see "Explosions").
Grenade arc (0.15.0): Throw (0x0242), ThrowFix (0x0243), additive; Explosion gets an optional
trailing u32 throwSeq (older senders end before it) (see "Explosions").
Car damage (0.15.0): CarDamage (0x0250), CarDamageReport (0x0251), CarDamageRequest (0x0252),
additive (see "Car damage").
Several guests (3-4 players; 0.15.0): HostNpcHit (0x0422) gets a trailing u8 attacker (read as the
host when missing), NpcStates carry the other guests' own NPCs under relay keys (see "Several guests").

Latency (0.15.0; core `kProtocolVersion` 9: lanes, session clock, app message send stamps, core
docs/protocol.md "Latency"): no message of ours changes; PlayerState's u32 time is now the moment the state was
sampled (was: sent), which only shifts the receiver's timeline. See "Latency budget" at the end.

Reconnect (core docs/protocol.md "Reconnect"): no app message of ours changes and none is added. A guest whose
connection was lost keeps playing while core rejoins the host; the host holds his slot. See "Reconnect" at the end.

Mission fail: MissionFail (0x0205) and FailVerdict (0x0206), additive (a peer without them ignores them).

Car damage: CarDamage (0x0250), CarDamageReport (0x0251), CarDamageRequest (0x0252); additive (a peer without them
ignores them and keeps its own car damage). See "Car damage". Since the car damage echo fix a copy never reports its
wreck, the exploded flag, the game's reaction to our own writes or the part change of a rebuild by its tuning, and
the holder drops such reports (rules only, the bodies are unchanged; an older peer's echoes are dropped by a newer
holder).

Car look (a copy shows its car's colours, plate, tint, interior, painting and body variant): CarLook (0x0253),
additive (a peer without it ignores it and keeps its copies as the game spawns them). See "Car look".

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
                          5 throw (grenade, molotov, knife), 6 melee hit reaction (an NPC punched him); traversal
                          (0.15.x): 7 jump, 8 vault, 9 climb up a ledge, 10 drop down, 11 hard landing, 12 ladder up,
                          13 ladder down, 14 slide into cover (7..14 are moves: the puppet changes place), 15 medkit,
                          16 body picked up, 17 body dropped; doors (unreleased): 18 door kick, 19 lock pull
                          (lockpick), 20 door opened by hand (the door itself is "Doors" below); other values
                          read as none
-- pose block (traversal; after the action block; optional, see below):
u8  pose                  what he keeps doing: bit0 sneaking (IsStealthMove), bit1 carrying a body (IsCarryingBody),
                          bit2 on a ladder (from USE_LADDER until he stops climbing); other bits are dropped
u32 body                  only when pose bit1: the carried body's key as the host numbers its NPCs (npc_sync: the
                          host's own entity id; a guest sends the host key his NPC is paired with), 0 = unknown
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

**Traversal in the action block, and the pose block** (src/game/player_actions.h, src/game/traversal.h; unit-tested
in tests/player_actions_test.cpp and tests/traversal_test.cpp; NOT checked in game). The kinds 7..17 ride in the
same counter: the sender's C++ side detects the moves (game/traversal.h `LocalDetector`, from the HumanMessages the
glue collected and the player's height and vertical speed) and `actions::Merger` adds them to the glue's own counter
(one step per detected move, every step of the glue's counter kept; a punch and a move in the same sample: the move
is the latest kind, the punch a missed step). A receiver without the traversal kinds reads 7..17 as unknown: its
counter moves on, it plays nothing (as any build does with a kind beyond its own). The pose block (1 byte, 5 when
carrying) follows the action block and is optional the same way: a state that ends after the action block reads as
"no pose", an older reader stops before it; a state whose pose says "carrying" but ends before the body is rejected.
`version` stays 4. A receiver drives the puppet from the pose (sneak walk, the body it carries, held on the ladder
path) and plays the moves: docs/game-api.md 17.

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
   With three or four players (0.15 multi-guest, `src/game/seats.h`, unit-tested by tests/seats_test.cpp): B takes a
   passenger seat that is free on his game **and** that no other player's state names (`mc_rideseat`): another
   passenger whose puppet was not seated on B's game yet (hidden, loading) made his seat look free, and both sat in
   it. If two players still got into the same passenger seat (both pressed E at the same moment), the lower player id
   keeps it: the other's game slides him to a seat that is free and unclaimed (`SlideToSeat`, checked twice a second,
   `M.SeatTick`, trace `SEA`), or he stays when there is none (his puppet is then hidden on the others' games, as for
   any seat taken).
3. **Get out / park.** A's PlayerState stops carrying K (on foot). The others stop moving K: their entity stays where
   it was (a parked car with normal physics, unlocked) and A's puppet climbs out. When A's car has stopped (speed
   < 0.3 m/s, or 8 s after getting out) A's game sends **CarParked** (reliable) with K and the final transform; the
   others put their entity there (creating a copy if they never had one). Its flags say whether the car is
   destroyed (`car:GetDamage()` >= 100 on A's game: 0..100, 100 = exploded, docs/game-api.md 24). The car's damage
   itself (parts, tyres, fire, explosion) travels in CarDamage ("Car damage").
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
apply. On a third game (both are remote there) the one who had the wheel there first keeps it until his state leaves
the wheel (0.15 multi-guest; before, the claim flipped between them every tick, with a "drives it" line and a fresh
copy swap each time; trace `DRV`). Copies are capped at 12 per game (the oldest parked copy nobody sits in is despawned; never one with a real
car hidden behind it). When the session ends, copies nobody sits in are despawned (a hidden real car is shown at
the copy's place); the one the local player sits in stays as a normal car (a real car hidden behind it comes back
once he got out) until he gets out, then it is despawned when unseen (0.15 multi-guest, `M.LeftCopiesTick`; before,
it stayed with despawn immunity for the rest of the game). The same for a host traffic car's copy that a player took
(traffic_sync.lua hands its obtain id over with it, so player sync despawns it like its own copies: at the session
end, on a lost connection, when it is trimmed) and for an adopted ambient traffic car of this game (its despawn
immunity goes off again). A player who joins late
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
- PlayerModel is sent when a session starts, when his story look changes, whenever a new player appears in the
  session (the Hello model is fixed for the session, so a late joiner would otherwise see the old pick), and when the
  player picks something else in the panel: **not per step** but once the pick has stayed the same for 1 s, or at once
  when the panel with the CHARACTER rows closes, or at once outside a loaded world (the lobby)
  (src/game/model_change.h `Outbox`). Before (up to 0.15.0) every step went out, and the other games replaced the
  puppet for each one: five puppets in 2 s in the 0.15.0 two-PC test, one of them released while it sat in a car.
- Receivers make the puppet of a change that comes after a quiet time (no other change from him for 2 s) at once;
  changes inside a burst (an older sender, a pick and a look change together) are held until the newest one has
  stayed for 1 s, so a burst gives one more puppet, not one per step (`Gate`; a change back to what is shown cancels
  the wait). The lobby's list shows the newest pick at once.
- Receivers show a remote player's puppet with: `puppet_model` (this machine, tests) > his PlayerModel look > his
  PlayerModel / Hello model resolved locally. A profile that does not load within 12 s is marked bad on that machine
  and the default character for his id is used instead.
- The old puppet goes properly (player_sync.lua `M.Release`, docs/game-api.md 3 "Removing a puppet"): its map
  marker first, then out of its car seat, hidden, deactivated and released; the new one takes over its seat at
  once. No wire change.
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

- key = the NPC's entity id on the host's game (the number in its `C_EntityGuid`). The host sends at most 48 NPCs
  within 120 m of himself or any guest's puppet, 10 times a second (several messages when more than 16); a death also
  goes out reliably at once. With several guests in different places the 48 are taken in turns from each player's
  nearest (the host's, then every guest's; 0.15 multi-guest, `N.PickSend`), so a guest far from the others still gets
  the NPCs around him; before, the 48 nearest to anyone went out and a big fight around the host could take them all.
  With two or more guests the same message also carries the other guests' own NPCs under relay keys (below,
  "Guest-owned NPCs", "Several guests").
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
| 0x0422 | HostNpcHit | host -> that guest (`SendTo`) | R | u32 key (the guest's NPC), f32 amount, u8 damageType, u8 hitZone, u32 weaponId, u8 flags (1 = lethal: the host's copy died of his hits), u8 attacker (0.15 multi-guest, trailing: the player whose hit it was, 0 the host; another guest when he hit the NPC relayed to him; read as 0 when missing, older readers stop before it) |
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
  game it came from).
- **Other guests (several guests, 0.15 multi-guest; not yet checked in game):** the host relays its mirrors of guest
  A's own NPCs to every other guest in its NpcStates (SendTo per guest, 10 Hz, at most 48 per guest and round; never
  to A himself), keyed by a **relay key**: A's entity id for it with its type byte (8, a human: an id is `slot << 8 |
  type`, docs/game-api.md `mc_entity_ids`) replaced by `0xF0 + A` (`npcown::RelayKey`). No entity id has such a type
  byte, so it never collides with a host NPC, and it stays below 2^24, so the game's Lua (32-bit float numbers) holds
  it exactly; an id that does not fit (slot >= 65536) is not relayed. Guest B treats it like any host
  NPC: his own version of that NPC (his game spawned the same wave) pairs with it, so it never becomes B's own as well,
  or he shows a spawned copy of it. Live mirrors with a state of the last second and dead ones (so B's copy dies) are
  relayed; none with a claim under way. B's hits on it come to the host as NpcHit for the relay key; the host forwards
  them to A as HostNpcHit with B as the attacker, and A replays them from B's puppet (a lethal one kills A's NPC). When
  A and B both owned the same NPC before either saw the other's (both games spawned it before the host's did; the same
  spot, or the same tag within 40 m, one-to-one, nearest first), only the lower id's is relayed to the other: B pairs
  his with A's and hands his back (GuestNpcEnd handed back, hostKey = the relay key), and A never gets B's, so the
  two cannot hand each other's back in a loop. A guest who leaves or drops takes his relayed NPCs along.
- **Hits:** the host's hit on a mirror's copy goes to its guest as HostNpcHit; he replays a bullet as a real shot
  from the host's puppet on his game (`CreateShots`; else, or when it does not land within 0.35 s, `DoDamage`), so
  the NPC turns on the host's puppet. When the copy dies within 1.5 s of the host's hit, the hit is lethal: 0.8 s
  later the guest's NPC is killed if it still lives (the same rules as a guest's lethal NpcHit). The guest's own NPCs
  shoot at every puppet on his game (all puppets get the FRIEND brain where the game-sectors guard is in,
  player_sync.lua `M.Ally`); their hits go to that puppet's player as a Hit with source 1 (0.15 multi-guest: to any
  player; before, only hits on the host's puppet were sent and another guest's puppet drew the fire for nothing). The
  host takes them like any NPC's; another guest skips one when his own version of that NPC (paired with its relay
  key) is alive and hostile and fights him on his own game already (`N.FightsUs(key, from)`).
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
  (flag 2, police only: the car's own siren / beacon bits via `mc_car_signals`; when they cannot be read, on while the car goes faster than 14 m/s or is near a hunted player, and 8 s after; see "Police"). The
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
  through npc_sync (NpcHit) like any paired NPC. Damage to the cars themselves (dents, parts, tyres, fire, the
  explosion) is shared since the car damage sync: the host's game holds it, a guest's copy reports its own ("Car
  damage").

### Car damage (not yet checked in game)

Code: `src/game/car_damage.{h,cpp}` (wire format and rules, unit-tested in `tests/car_damage_test.cpp`),
`src/game/car_damage_sync.cpp` (messages, the native reads and writes), `src/game/car_damage.lua` (which cars, who holds
them). What is read and set on the game: docs/game-api.md 24, docs/re-notes.md 11. Additive: a build without these
messages ignores them (and keeps each game's own car damage, as before).

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0250 | CarDamage | the game that holds a car's damage -> everyone (`Broadcast`; the host's traffic: host -> guests); an answer to a request -> that player (`SendTo`) | R | see below |
| 0x0251 | CarDamageReport | a game with a copy -> the game that holds the car (`SendTo`; `SendToHost` for the host and his traffic) | R | the same body: what the copy has beyond the holder's state (parts, wheels, the fire / explosion bits) |
| 0x0252 | CarDamageRequest | a game with a new copy or entity for a car -> the game that holds it | R | car ref only: send me the whole state (answered with a full CarDamage) |
| 0x0253 | CarLook | see "Car look" | | |

Body (CarDamage and CarDamageReport; 18 bytes for a player car, 15 for traffic, + 1 per 4 wheels + 2 per listed part; at most ~520):

```
u8  refKind               0 player car, 1 the host's traffic car
    kind 0: u8 keyOwner, u32 vehId        the car key (Phase 3 "Shared cars")
    kind 1: u16 net                       its net id (TrafficSpawn; the host's own book keeps his entity id)
u16 epoch                 moves on with every repair of the car on the holder's game (a copy repairs itself when
                          it sees a newer one; a report of an older epoch is ignored)
u8  flags                 bit0 exploded, bit1 burning (the countdown to the explosion runs), bit4 full (every
                          damaged part is listed: the rest are intact), bit5 repaired (the epoch moved on with this)
u16 burnLeft              seconds to the explosion while burning (1/100 s)
u32 layout                hash of the car's part types (FNV-1a of the count and each type): part indexes are only
                          exchanged between cars of the same layout
u8  partCount             parts the car has (at most 250)
u8  wheelCount            at most 16
    ceil(wheelCount / 4) bytes: 2 bits per wheel, low bits first: 0 ok, 1 tyre burst, 2 wheel off
u8  n                     parts listed
    n x (u8 index, u8 damage)   0 intact .. 254 no energy left, 255 gone (broken off / destroyed)
```

- **Who holds a car:** a player car's driver (the game whose player sits at its wheel), while parked its last driver
  (each game remembers who that was from the states; the key's first driver if unknown); the host for his traffic.
  Two drivers at once (Phase 3 "Conflicts"): each takes the other's CarDamage as raise-only damage of its own.
- **Sending:** the holder reads the car 4 times a second and sends a CarDamage with the parts that changed (2 quanta
  or more, or reaching / leaving intact, empty or gone) whenever something changed; the whole state (full) the first
  time it is damaged, every 15 s while damaged, after a repair (repaired, new epoch) and to whoever asks.
- **Receiving:** the state of the car merges raise-only (a newer epoch replaces it); a copy sets what it lacks (more
  than 3 quanta below) with the game's own functions, never lowers anything except by a repair (newer epoch), and
  asks (CarDamageRequest, every 3 s) until it has a full state. A copy whose own damage stands above the holder's for
  0.4 s reports it (CarDamageReport, only what it did not report yet); not while it was moved by SetPos in the last
  second (pinned or snapped by the car controller), except tyres and the fire / explosion bits; such bumps are
  undone by a repair once they add up (see docs/game-api.md 24). **Echoes are never reported** (0.15.0 test: a
  copy's wreck came back as a report and the holder read the drop that followed as a repair): nothing while the copy
  or the holder's car is exploded, never the exploded flag, and nothing new for 1.5 s after this game wrote cascading
  damage on the copy (an explosion, a fire, a wheel, a part broken off: the game's reaction to our writes is taken as
  reported). The holder drops reports of a car that is exploded on its game and the exploded flag in any report; a
  car that stays exploded is never a repair, nor is a drop within 1 s of the holder's own writes on its car (a
  report, a blast, a new entity). A copy never blows up by itself: a burning copy
  burns with its countdown held, and the holder's exploded flag (or the explosions sync's CarExploded, whichever comes
  first) sets it exploded once, with a visual-only picture ("Explosions"; docs/game-api.md 24 "One explosion").
- **Blasts on other games:** an Explosion from another game (a picture here) is put on the cars this game holds
  within 10 m by this game itself (explode within 4 m, parts / tyres / fire by distance; docs/game-api.md 24), and
  CarDamage carries it on; the holder drops copies' CarDamageReports of that car for 1.5 s after, so the blast counts
  once.
- **The host's traffic:** CarDamage goes out by net id to all guests (a guest without that copy ignores it); a
  traffic car nobody has a copy of is not sent. Reports and requests for it go to the host, who maps the net id back
  to his car.
- A message that does not parse is dropped (counted, `bad msgs` in the overlay). Car refs of player cars that this
  game does not know are kept for 30 s (the copy may come a moment later).

### Car look (a copy shows its car's look; not yet checked in game)

Code: `src/game/car_look.{h,cpp}` (wire format, line form, the Book; unit-tested in `tests/car_look_test.cpp`),
`src/game/car_look_sync.cpp` (messages, the native tuning read), `src/game/car_look.lua` (which cars, the reads and
setters). What is read and set on the game: docs/game-api.md 24 "Car look", docs/re-notes.md 13. Additive: a build
without it ignores CarLook and keeps its copies as the game spawns them (another variant, colour and plate).

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0253 | CarLook | a player car's look: its key owner's game -> everyone (`Broadcast`, on change) or -> one player (`SendTo`, an answer); a request: a game with a copy -> the game that has the car (`SendTo`; `SendToHost` for the host and his traffic); a traffic car's look: host -> the guest who asked (`SendTo`) | R | see below |

Body:

```
u8  op                    0 look, 1 request (the car ref follows; nothing more)
u8  refKind               0 player car: u8 keyOwner, u32 vehId (the car key, Phase 3 "Shared cars");
                          1 the host's traffic car: u16 net (TrafficSpawn's net id)
op 0 only:
u16 seq                   the sender's look number for this car (moves on with every change)
str model                 GetVehicleModelName (at most 64)
i16 color1, color2        GetColor(1), GetColor(2) (-1 unknown)
str plate                 GetSPZText ("" unknown; at most 16, may hold spaces)
i16 tint                  GetWindowTint (-1 unknown)
i16 interior              GetInteriorColorsSet (-1 unknown)
u8  dirt, rust            0..250 (x 250 of a 0..1 scale), 255 unknown: always 255 for now (scale UNVERIFIED)
str painting              GetPainting ("" none / unknown; at most 64)
u8  flags                 bit0: the tuning was read (else the list is empty and means "unknown")
u8  n                     tuning items (at most 32)
    n x u16               the installed tuning item ids, slot order (mc_car_tuning)
```

About 30-40 bytes for a player car with a few tuning items; a request is 4 (traffic) or 7 bytes (player car).

- **Who sends:** the game that has the car itself, never a copy: a player car's key owner (its first driver: his
  real car, also while it is hidden behind a copy because another player drives it), once when it is first seen and
  whenever the look changes (at most once a second), plus an answer to whoever asks; the host for his traffic, only
  to a guest who asks (by net id on the wire, his entity id inside his book, like car damage). A message that does
  not parse is dropped (`bad msgs` in the overlay); a traffic look from anyone but the host, or a traffic request to a
  guest, is ignored.
- **Who takes it:** only copies this game spawned (player_sync's copies of player cars, traffic_sync's copies of the
  host's traffic). A real car (ours, an adopted mission or traffic car) is never changed, and the car's own game
  never takes a look, so a look cannot echo. The newest `seq` of the sender counts; a look from another sender
  replaces it (the car changed hands).
- **A copy:** reads its own look every round (0.5 s; a traffic copy older than 10 s every 3 s) and sets what differs
  (colours, plate, tint, interior, painting, the tuning items it lacks: `InstallTuningItems(ids...)`), up to 3 tries 2
  s apart; then it is given up (logged with what still differs) until a new look arrives or the copy is a new entity.
  Without a look it asks the car's game every 3 s.
- **Occupied copies:** `InstallTuningItems` rebuilds the car (docs/re-notes.md 13), which would put whoever sits in
  it out. So the tuning waits while a player (our own or another player's puppet) sits in a copy, and is set once it
  is empty; colours, plate, tint, interior and painting do not rebuild the car and are set at once. A new copy of a
  player car is held back from the puppet (player_sync seats it only after the look, with its tuning, is on) for 4 s
  at most. Traffic copies with their own crew take the tuning as before. UNVERIFIED in game: that the rebuild puts a
  seated human out, and that the other setters do not.
- **Car damage:** after a rebuild by the tuning, car damage asks the holder for the whole state at once (a
  CarDamageRequest), puts what it knows back on the new parts and does not report what the parts show new for 3 s
  (the holder, when it is our own entity, reads no repair from the drop for as long).
- **Car damage:** a copy of another body variant has another deform part list (CarDamage's layout hash differs, only
  tyres and the explosion are synced); once the tuning items match, the layout should too (UNVERIFIED).

### World items (not yet checked in game)

Things lying in the world: weapons on the ground (entity type 63 `C_ItemWeapon`, what a dead enemy drops) and the
ground pickups of the item manager (type 66 `C_ItemInventory`, e.g. medkits; docs/game-api.md 20). Logic in
src/game/world_items.h (unit-tested, tests/world_items_test.cpp), glue in src/game/item_sync.cpp / item_sync.lua.
Additive: a peer without these messages ignores them (each game then keeps its own items, as before).

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0270 | ItemStates | host -> guests (`Broadcast`) | U, 2 Hz | u8 n (at most 16), then n x: u32 key, u8 type (63 weapon, 66 inventory item), f32 x y z, str name (the item's scene object name, at most 40 bytes of `[A-Za-z0-9_.-]`, "-" unknown) |
| 0x0271 | ItemTake | guest -> host (`SendToHost`) | R | u32 key (the host's item), u8 type, f32 x y z (where the guest's copy lay) |
| 0x0272 | ItemGone | host -> guests (`Broadcast`; `SendTo` one guest for a refused take) | R | u32 key, u8 by (who has it: player id, 255 nobody), u8 reason (0 taken, 1 forget) |
| 0x0273 | Collectible | any -> everyone (`Broadcast`, relayed by the host) | R | str name (the game's `lh_pickup_...` name) |

- key = the item's entity id on the host's game. The host sends the 96 items nearest to its player that lie within
  100 m of it or of a guest's puppet, twice a second (several messages when more than 16).
- **Pairing (guest):** each host item is paired with an item of the guest's world of the same type: the nearest within
  1.5 m, or within 4 m when both have the same name (the guest's copy of an enemy died a few metres from the host's);
  nearest pairs first, then kept. A paired item more than 0.3 m from the host's is put there (`SetPos`), again when
  the host's item moves. A pairing is released after 5 s without a state (the guest's item stays, his own).
- **The guest's own drops:** a weapon that appears on the guest's game more than 5 s after his world loaded (a new drop),
  more than 2.5 m from his player (not his own weapon swap) and within 60 m of him, and that finds no host partner in
  3 s while host states arrive, is hidden: the host's world has no such drop (his copy of an enemy dropped one that
  the host's enemy did not, or the host's lies elsewhere). Items his world had from the start and inventory items are
  never hidden this way. A host item without a partner on the guest's game is not shown there (no spawn; counted as
  "not here" in the status and the trace).
- **Taking:** an item that vanishes from a game (the entity gone or switched off) within 2.5 m of that game's player
  was taken by him. On the host that is a take by the host at once; on a guest whose item was paired, an ItemTake to
  the host. The host decides: the first take it learns of wins (its own, or the first ItemTake). The winner is named
  in an ItemGone broadcast; every other game hides its copy (`Deactivate`; a weapon still lying there after that:
  `ClearWeaponsInRadius(pos, 0.3)`). A later take of the same item (two players grabbed it at the same time) gets an
  ItemGone sent to that guest only, naming the winner: his game takes away the weapons he gained in the 2 s before
  (`InventoryRemoveWeapon`). A take of an item the host does not know (forgotten, or never in its world) is granted.
  The host remembers taken items for 30 s (later takes refused, late states ignored on guests).
- **Despawns:** an item that vanishes away from every player was removed by the game (dropped weapons despawn at a
  distance, `ItemWeaponDespawnDistance`): no take. The host forgets it (ItemGone reason 1): the guests unpair theirs
  and keep it. A guest's paired item that vanishes away from him is just unpaired.
- **Ammo and health pickups are shared like weapons** (a medkit taken by one player is gone for all). The game's
  wall first-aid boxes are mission-script interactions on each game, no item entities: they stay per player. Whether
  the game respawns ground pickups is not known (UNVERIFIED); a respawned item is a new entity and simply a new item.
- **Collectibles are per player:** each player collects his own copy for his own save; nothing is hidden on the other
  games and no other game's save or collection is written. Collectible only tells the others (log, status line).
  A guest's saves are redirected away from his real save folder during co-op, so what he collects is not kept in his
  real save (docs/game-api.md 20).
- Money: the game has no money items (the money and cash-loot bindings are unused in Mafia DE, UNVERIFIED); nothing
  is sent. Physics props (crates, barrels) stay local.
- A new world on either game (checkpoint, reload, following the host) starts the books over (ids are per world).

### Doors (0.15.0; not yet checked in game)

Doors are the same on every game: a door a player opens, closes, kicks open, unlocks or lockpicks (the lock pull)
changes on the other games too, and his puppet plays the kick / lock pull / door opening (the action block above).
The host's game decides the story doors. Code: `src/game/door_model.h` (the rules, unit-tested in
`tests/door_test.cpp`), `src/game/door_sync.{h,cpp,lua}`; the game side is docs/game-api.md 21.

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0230 | DoorEvent | guest -> host (`SendToHost`) | R | u16 seq, u8 by (ignored: the host puts the sender), u8 act, u8 state, u32 name, f32 x y z, f32 fx fy fz (33 bytes) |
| 0x0231 | DoorApply | host -> guests (`Broadcast`; `SendTo` one guest for a correction) | R | the DoorEvent body; `by` = the player who did it, 255 = the host's world (his mission scripts, his NPCs, a door closing by itself) |
| 0x0232 | DoorSnapshot | host -> one guest (`SendTo`) | R | u8 n (at most 96), then n x: u32 name, f32 x y z, u8 state (17 bytes) |
| 0x0233 | DoorSnapRequest | guest -> host (`SendToHost`) | R | f32 x y z (the guest's player), u8 reason (1 join, 2 reconnect, 3 a new world) |

- **A door on every game:** doors are entities of type 21; their entity ids differ between games, so a door is
  identified by `name` = FNV-1a 32 of its lower-case scene name (`GetName()`; 0 when it has none) and its position
  (`GetPos()` when first seen): the same name within 1.0 m is the same door (nameless doors: 0.3 m), the nearest one
  when several match (double doors).
- **state:** bit0 open (`not IsClosed()`), bit1 locked (`IsLocked()`); other bits are refused. **act** says how the
  door got there and how the receiver gets it there: 1 open (by hand), 2 close, 3 kick (forced open: unlocks), 4
  unlock, 5 lockpick (the lock pull), 6 lock, 7 set (several bits, corrections, snapshots); the receiver brings its
  door to `state` (Unlock, then Kick(from) for a kick or Open(from), Close, Lock) and `from` (where the one who did it
  stood) gives the direction the door swings. Other act values and non-finite numbers drop the message.
- **Who sends what:** every game polls the doors within 35 m of its own player (5 times a second, plus any door that
  just got a door message or that it changed for somebody else) and works out who changed one: its own player (the
  door message's sender is him; or within 2 s of his door kick / lock pull / door open action and within 3.5 m; or
  he stood within 1.8 m on foot), else its own world. A **guest** sends his player's changes to the host
  (DoorEvent); the changes of his own world (NPCs, his own copies of the mission scripts) stay on his game. The
  **host** sends his player's changes and his world's (`by` 255: the story's locks and unlocks, NPCs) to every guest
  as DoorApply.
- **The host decides:** a guest's DoorEvent that opens by hand a door that is locked on the host's game (a state
  read in the last 3 s: near the host) is refused: the host answers that guest alone with a DoorApply `set` to the
  host's state (his door closes and locks again). Kicks, lock pulls, unlocks and doors the host has not loaded
  (or not polled lately) are accepted: the host applies it to his door and sends it as DoorApply to every guest (the
  sender skips his own). On a guest, a lock change by his own world of a door the host told him about (DoorApply,
  snapshot) is put back to the host's lock, at most 3 times a minute per door; his world opening or closing such a
  door is left alone (NPCs walk through doors on every game).
- **No echoes:** a game that changes a door for somebody else expects that state for 3 s; the poll seeing it (or a
  step toward it: a kick unlocks, then opens) is not reported again. One that did not get there in time is logged
  (`unmet`).
- **Doors that are not loaded:** every game remembers the changes of this session (the host all of his, a guest
  what the host told him and his own; at most 256, the oldest go first); a door that comes into the entity table
  later (streamed in, or loaded again) is set to its remembered state. A new world of a game (a load, a checkpoint,
  the main menu) forgets its doors and its memory.
- **Snapshot:** a guest asks for one 3 s after the session became active (join; reconnect) and 5 s after each new
  world of his, once he is in it: the host answers with the doors within 40 m of the guest's position whose state
  his game read in the last 3 s (doors near the host too), nearest first, then the remembered changes nearest first
  (no door twice), at most 96 (~1.6 KB). The guest remembers every entry and sets his loaded doors that differ.
- Bandwidth: one 33-byte reliable message per door change (a few a minute), the snapshot per join / world.
- Logs: `doors: ...` (main log), trace records `DR` (C++: local changes and their verdict, tx, rx, applies,
  corrections, snapshots, unmet), `DL` (Lua: door messages and detected changes with the attribution) and `DA`
  (Lua: what was called on a door), docs/sync-trace.md; `tools/sync_compare.py` has a doors section per game.
- UNVERIFIED in game: all of it (docs/game-api.md 21): that `mc_entity_ids(21)` lists the doors of houses, shops,
  garages and gates, that names and positions match across games, the door calls, the door messages.

### Police (0.15.0; the offence fix checked in game, the rest not yet)

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0260 | PoliceState | every player -> everyone (`Broadcast`) | R on change (at most 5/s), U heartbeat (1 s wanted, 3 s clean) | u8 version (1), u16 seq, u8 flags (1 wanted, 2 hunt, 4 seen by police, 8 zone decaying, 16 fine going on, 32 arrest going on, 64 offences disabled, 128 in a world), u8 level (0-16), f32 zone centre x y z. Readers ignore bytes after these |
| 0x0261 | PoliceEvent | every player -> everyone (`Broadcast`) | R | u8 kind (police_model.h `Event`: the game's police events PLAYER_ENTER_BLUEZONE 1 .. POLICE_BLUEZONE_STARTED 18, the human messages OFFENDER 19 .. ARRESTED 23, LOST 24, WANTED 25), u32 arg (ARRESTED: GetArrestFinished) |
| 0x0262 | PoliceChase | host -> one guest (`SendTo`) | R when it starts / ends, U every 0.5 s while it goes on | u8 flags (1 active, 2 hunt), u8 level, u8 cops (host police within 90 m of his puppet), u8 seers (of them, those that saw it in the last 5 s), f32 nearest (m) |
| 0x0263 | - | - | - | reserved for police (not sent) |

- **Each player's wanted state is his own game's** (docs/game-api.md 22): his game's police zone (search / hunt, its
  level and centre), seen by police, fines, arrests. It is read 5 times a second and broadcast; every game logs the
  others' changes (`police: player 1 is wanted hunt level 2 ...`) and shows them in the overlay. Nothing of it is
  copied into another game's police: arrests, fines and losing the police stay per player.
- **Host:** a guest is a target of the host's police while his state is fresh (6 s), says wanted and in a world, and
  his puppet is within 250 m of the host (kept to 400 m; released 3 s after he is clean again, at once when far,
  stale or gone). For a target the Lua glue announces an offence from his puppet (when he becomes a target, when his
  level or zone type goes up, at most every 5 s for a new offence event of his: OFFENDER, a zone entered / started,
  a witness), sets the nearest police humans on his puppet when he is hunted at level 3 or more (`Attack`), counts
  the police near it and those that saw it (SEE_ENEMY) once a second, and the dll sends him PoliceChase while police
  are near it (none for 3 s: an inactive PoliceChase, reliable). When he is no target any more the cops set on him
  `Forget` (not while the host himself is wanted).
- **Guest:** while a PoliceChase says active (and for 4 s after it ended or went silent for 3 s), his game holds his
  wanted level at least at the chase's level (`ForceMinEscalationLevel`; 0 releases it, also when the session ends)
  and holds his zone's decay (`PreventZonesDecay(true, follow)`, follow: his red zone follows him while a host cop
  saw his puppet or is within 40 m; released with `(false, false)`): his wanted state follows the host's chase and
  no longer runs out after ~20 s while the host's cops are on him (0.15 test). When the host's police see him and his
  game has no zone, it starts one at his position; when his zone decays or decayed away while host cops are near his
  puppet, it starts a fresh one (not after a fine, an arrest or his death, not with offences off) (`ZoneCreate`, at
  most every 5 s). No wire change: all of this follows the existing PoliceChase (host-authoritative). The host's own
  zone is never touched. Far from the host no chase comes: his own police work as always (traffic sync turns his police spawning off
  only near the host).
- **Police copies** (traffic): the host reads the real siren / beacon of its police cars (`mc_car_signals`) for the
  copies' flag 2 (TrafficStates); the speed guess is only the fallback, now also "near a hunted player".
- Additive: a peer without these messages ignores them. Logs `police: ...`; trace records POL / POE / POR / POT /
  POC (docs/sync-trace.md).

### Explosions (0.15.0; messages seen in game, the pictures not yet)

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0240 | Explosion | the game where it happened -> everyone (`Broadcast`; a guest's is relayed by the host) | R | u32 seq (per sender; a new session starts elsewhere: `(GetTickCount() & 0xFFFF) << 16`), u8 kind (0 unknown, 1 grenade, 2 molotov, 3 dynamite `exp_satchel`, 4 crash object, 5 car blast), u8 flags (1 fire: the picture burns too; 2 one of the sender's NPCs threw it), f32 x y z, str weapon (`exp_grenade`, ...; "" unknown; at most 32 of `[A-Za-z0-9_-.:/]`), u32 npc (flag 2: the NPC's key on the sender's game: on the host its entity id, as in NpcStates; on a guest his own NPC's key, as in GuestNpcStates), str object (kind 4: the crash object's scene name, at most 64; "" none), **[u32 throwSeq]** (grenade arc: the Throw 0x0242 whose explosion this is, 0 none; older senders end before it, older readers ignore it; 1-3 trailing bytes = malformed) |
| 0x0241 | CarExploded | the car's authoritative game -> everyone (`Broadcast`) | R | u8 what (0 a shared player car; 1 a host traffic car, only from the host), str key (what 0: the shared car key `o:g`, Phase 3; what 1: its traffic net id, Traffic above), f32 x y z |
| 0x0242 | Throw | the thrower's game -> everyone (`Broadcast`; relayed by the host) | R | u32 seq (per sender, below 2^24; a new session starts elsewhere: `(GetTickCount() & 0x3FF) << 12`), u8 kind (as Explosion: 0 other/unknown, 1 grenade, 2 molotov, 3 dynamite), u8 flags (1 fire: a molotov; 2 measured on the real projectile, else the aim model's guess), f32 origin x y z, f32 velocity x y z (m/s, at the origin; at most 80), f32 gravity (m/s^2, 0.5..50; sent 9.81), u16 delayMs (how old the origin was when sent), u16 fuseMs (origin to explosion, 0 unknown: sent 0 for now), str weapon (at most 32). About 40-50 bytes, **one per throw** |
| 0x0243 | ThrowFix | the thrower's game -> everyone (`Broadcast`) | U (settled / gone: R) | u32 seq (the Throw's), u16 tMs (since the Throw's origin), u8 flags (1 bounce: our EXPLOSIVE_BOUNCE sound; 2 settled: it lies still here; 4 stop: it is gone, its last position; 8 turn: the readings left the predicted path; 16 position only: no projectile to read, keep your own velocity), f32 x y z, i16 vx vy vz (cm/s, clamped to +-327 m/s). **25 bytes**, at most 8 per throw (the 8th only for settled / gone), each sent the moment it is made |

Damage is decided by **one** game; the picture is shown on all (src/game/explosion_model.h, unit-tested by
tests/explosion_test.cpp; the game side in explosion_sync.lua; docs/game-api.md 23).

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
  0, 0, 1.5, fire)`: no owner, damage 0, no stun, both damage ranges 0, fire for a molotov (docs/game-api.md 23).
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
  **One explosion per car on every game:** only the game that holds the car's damage ("Car damage") has a real
  explosion of it; everywhere else it never blows up by itself (explosion off, a burning copy's countdown held) and
  CarExploded, car damage's exploded flag and a wrecked car coming back all go through one routine that runs once per
  car entity, whichever comes first (docs/game-api.md 24).
- **The flight (grenade arc; Throw; src/game/throw_model.h, tests/throw_arc_test.cpp; the game side in
  throw_arc.lua; docs/game-api.md 23 "The flight").** While the player lines up a throw his game only logs the aim
  (GA trace records). On the release (WEAPON_THROWN) the throw gets its number at once; the explosion detector keeps
  it with the throw, so the Explosion decided for it later carries it (`throwSeq`). The thrower's game looks for the
  real projectile entity (types 25 grenade, 26 molotov, 88, 90; new or moved within 8 m of the hand) for at most
  0.3 s and reads up to three positions: the first is the origin, the first and the last give the launch velocity
  (the three also the gravity, logged). Then **one Throw** goes out (reliable). After it the projectile is followed
  cheaply and corrected by **events, not a stream**: its position is read every 0.2 s (moved less than 5 cm twice in
  a row = settled; more than 1 m off the path the last reading predicts = a sharp turn) and at each of our
  EXPLOSIVE_BOUNCE sounds; at a bounce or a turn the next tick's reading gives the velocity out of it and one
  **ThrowFix** goes out at once (unreliable), when it has settled or is gone one more (reliable), at most 8 per throw.
  A typical grenade: Throw ~50 bytes + 2-3 bounce fixes + the settled fix (25 bytes each) = ~125-150 bytes, 4-5
  messages per receiver. If fewer than two positions were read in 0.3 s, the aim model's launch is sent
  instead (flag 2 clear: the camera direction turned up 15 degrees at 16 m/s from the hand; UNVERIFIED constants,
  the GE record compares them with the measured launch). Receivers (`Book`, `Sim`) **simulate** the throw in their
  own world from the origin's time (receive time - delayMs): ballistic steps with the sent gravity, one step per
  script tick per flight (at most 0.1 s; a long tick catches up over the next ones), each step one ray cast along
  its segment; on a hit one short probe down in front of it tells floor from wall; the bounce keeps 35 % of the
  speed into the surface and 75 % of the speed along it; slower than 1.5 m/s into the floor it rolls (4 m/s^2), below
  0.2 m/s it has settled and casts nothing more; a molotov stops at its first hit; hits next to the origin in the
  first 0.15 s are the thrower's own body. A ThrowFix re-anchors the simulation: position and velocity at the fix's
  time (a settled / gone fix: stopped there; position only: its own velocity, bounced), advanced to now by the
  simulation; the marker slides there over 0.1 s instead of jumping; a fix older than the last one applied (late,
  unreliable) is ignored, a lost one costs nothing (the next or the settled one corrects). So a receiver whose world
  differs (a car parked there, a door closed) drifts for one bounce at most. It is shown as a marker with a fading trail drawn by our overlay (nothing
  is spawned in the world: it cannot hurt anyone) until the Explosion naming its seq arrives: the marker then jumps
  to the explosion's true position for 0.25 s (the simulated-vs-true offset is logged, GE); an Explosion without a
  seq ends the sender's nearest flight within 30 m (never an NPC's explosion); without an explosion it goes after
  its fuse + 1 s (if one was sent) or 15 s. Throws are reliable and numbered (one relayed twice is shown once).
  NPCs' throws are not carried (only their explosions, above). Rolling does not fall off ledges (it settles on the
  edge's height; the settled fix puts it where it really lies).
- **Not carried:** how a fire spreads (each picture burns on its own game). Cars are not left out: the holder of
  a car puts another game's blast on it itself ("Car damage", blasts on other games), so a remote player's grenade
  does wreck your car, on every game, once; a copy never blows up by itself.

## Reconnect (no messages of its own)

Core does the transport and session part (external/core/docs/protocol.md "Reconnect": slot held 60 s on the host,
the guest rejoins with backoff for 60 s and his resume token, PeerDropped to the others, `PlayerInfo::joinSeq`). What
the game layer does with it:

| Who | When | What |
|---|---|---|
| host, other guests | a player is dropped (`GameModule::OnPlayerDropped`) | his puppet is removed (`PlayerSync::OnPlayerLeft`, as when he leaves); banner "<NAME> LOST THE CONNECTION - WAITING FOR THE PLAYER TO RECONNECT... 45 S" (`Session::DroppedPlayers`; several at once: "SAM AND PAULIE LOST THE CONNECTION - WAITING FOR THEM ..." with the longest wait) |
| lost guest | `OnConnectionLost` | the game keeps running (no menu, no load). The session is inactive: PlayerSync removes the puppets and stops sending (the trace stays open: `RCN ev=sync-paused`), MissionSync keeps the host's point and our `_followPoint` (nothing reloads by itself) and forgets the host's cutscene, NpcSync's Lua drops the copies it spawned (it does that whenever the session is inactive); banner "CONNECTION LOST - RECONNECTING... 12 S" |
| host | the player is back in `Players()` with a new `joinSeq` | MissionSync sends him MissionState, his HostSave (the newest checkpoint save) and the Cutscene, as to a newcomer; PlayerSync sends our PlayerModel; NpcStates reach him with the next 10 Hz send |
| lost guest | `OnReconnected(sameSlot)` | PlayerSync is active again: every player is new (`_seenPlayers` cleared), so we send our PlayerModel and the puppets are spawned from the next states. MissionSync: the host's MissionState and HostSave arrive; at the same checkpoint nothing happens (the save is the same `#seq` or our point equals his), at another one the usual follow (quit to the menu and load his point after `follow_grace`). |

| lost guest | the session is inactive / back | DoorSync: nothing is sent or applied meanwhile; when the session is active again the glue reads its doors again and asks the host for a DoorSnapshot (reason reconnect) |
| everyone | a player is dropped | CarDamageSync forgets him as the holder of cars and as someone who asked for a state: his cars' copies ask whoever drives them next; when he is back his copies ask for every car's state again |
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
- Several guests: each held slot is independent (core tests/net_reconnect.cpp section 10: two of three guests lost at
  once, both back into their own slots, no newcomer meanwhile). A guest who drops takes his own NPCs' relay to the
  others along; when he is back, his GuestOwner sends them again and the lower-id rule (above) settles any NPC that
  another guest owned meanwhile.

## Several guests (3-4 players; 0.15 multi-guest, not yet checked in game)

Sessions take up to 4 players (host + 3 guests, core `kMaxPlayers`). What changes with more than one guest, and the
2-player assumptions that were fixed (each in its section above):

| Area | With several guests |
|---|---|
| PlayerState, PlayerModel, CarParked | every peer `Broadcast`s, the host relays to the other guests: each game drives one puppet per other player (`Remote` per id) |
| Hit (player -> player) | `SendTo(victim)`, guest -> guest through the host relay; the victim replays from the attacker's puppet; friendly fire as before |
| NPC hits on puppets | host NPCs -> any guest's puppet (as before); a guest's own NPC -> any puppet on his game, sent to that puppet's player (was: the host's only) |
| NpcStates | at most 48, in turns from each player's nearest (was: the 48 nearest to anyone) |
| Guest-owned NPCs | relayed by the host to the other guests (relay keys); hits forwarded to the owner with the attacker; one owner per NPC (lower id) |
| Seats | ride along skips seats the other players' states claim; the same passenger seat twice: the lower id keeps it, the other slides; the wheel of one car claimed by two other players: the current one keeps it |
| Traffic | per guest (HostTraffic: own interest set, 24 cars, 40 KB/s budget); a car near two guests has one net id; a guest who drops is a new peer when back |
| Failed missions | a guest's failure is his own (MissionFail -> FailVerdict to him only, the others play on); the host's failure is everyone's (HostFailed broadcast); tests/fail_flow_test.cpp "3 guests" |
| Story, cutscenes, saves | per guest by `(id, joinSeq)`: a newcomer or a reconnected guest gets state, save and cutscene; the others nothing extra |
| Lobby | ready per guest, START needs all of them; a fifth player is refused (core tests/net_lobby.cpp "4 players") |
| Name tags, map markers | one per other player (tests/map_markers_test.cpp "3 others"); two players in one car have their markers at the car |
| Reconnect banner | every dropped player's name, the longest wait |

Bandwidth with 4 players (estimates from the per-message sizes above, host upstream): PlayerState ~60-140 B x 60 Hz
per player, the host sends his own and relays two others' to each guest: ~3 x 3 x 6 KB/s = ~55 KB/s in cars; NpcStates
~45 B x 48 x 10 Hz = ~22 KB/s per guest (broadcast); relayed guest NPCs up to 48 x 45 B x 10 Hz = ~22 KB/s per guest
(only with guests' own NPCs); traffic up to 40 KB/s per guest (its budget). Worst case ~3 x (18 + 22 + 22 + 40) = ~300
KB/s up from the host, ~100 KB/s down to each guest; the relay limit per guest (`net_budget_kbps`, 1024 KB/s) is far
above what one guest sends. Per-guest totals: core's traffic log (`net: <name> (<id>): ... out .. KB/s, in ..`
every `net_log_interval` s, per peer), the overlay's traffic line on the host ("guest N: cars of relevant (KB)"),
trace `TRP` per guest.

Known limits (documented, not fixed):
- Traffic is only the host's: two guests together far from the host (beyond 250 m) each run their own ambient
  traffic and do not see each other's (a guest's traffic is never shared). The host-traffic switch uses the distance
  to the host only.
- A guest's own NPCs reach the other guests only through the host (one more hop: ~1 RTT later than the host's own).
- Puppets are spawned one at a time (one runtime spawner): with three others the last one may appear a few seconds
  after the first.
- Two players in one car: their name tags and map markers overlap.

## Latency budget (lanes, priorities, rates; not yet checked in game)

Every sync should reach the other games as fast as it can without jittery motion or a fat link. Core does the
transport part (core docs/protocol.md "Latency", "Lanes", "Send priorities", "Session clock", "Playout delay"):
what a game sends in a frame goes out at the end of that frame (`Session::Flush` after `SessionTick`), a lost
reliable unit is resent after about one round trip (fast retransmit), every app message carries the sender's
session-clock stamp. Ours (`src/game/latency.cpp`, `Configure`):

| Lane | Messages | Why apart |
|---|---|---|
| 0 | players, NPCs, traffic cars, car damage, explosions (incl. throws), hits | they refer to each other's cars and NPCs: one order |
| 1 | world / mission 0x0200-0x020F | the host's checkpoint save (up to 256 KB) holds back nothing else |
| 2 | doors 0x0230-0x023F | |
| 3 | police 0x0260-0x026F | |
| 4 | items 0x0270-0x027F | |

A lane message still comes after the lane-0 messages sent before it; lane 0 does not wait for the lanes, so e.g. an
NPC death sent after a mission change can arrive before it (UNVERIFIED that no handler minds; none checks for it).

Priorities against the per-peer send budget (`net_budget_kbps`, lowered automatically on a lossy / backed-up link):
Critical (never thinned or deferred): PlayerState, CarParked, PlayerModel, Hit, NpcHit, HostNpcHit, NpcClaim,
NpcDeath, explosions and throws (0x0240-0x024F), DoorEvent / DoorApply, ItemTake / ItemGone / Collectible, police
(0x0260-0x0262), MissionFail / FailVerdict. Bulk (first to go): TrafficStates (thinned), ItemStates (thinned),
DoorSnapshot and HostSave (deferred, in order). Normal: NpcStates, GuestNpcStates, car damage, the rest.

Per system: the delay from the sender's game to the receiver applying it (wire + both games' frames), measured
parts from core's `net_latency` (loopback) and our tests, the rest estimated (E) for 60 fps games and a one-way
network delay N. "Before" is 0.15 main (core 0.11).

| System | Rate / trigger | Before | Now | Bandwidth (per receiving peer) |
|---|---|---|---|---|
| PlayerState (on foot, car) | 60 Hz | sample -> send 0..1 frame (E) + send queued until the next Poll 16.7 ms (measured) + N + 0..1 frame + fixed 50 ms playout (led forward by 50 ms) | sent in the frame it is first seen, flushed at once (0.06 ms measured) + N + 0..1 frame + adaptive playout 24.5 ms at a quiet 60 Hz (measured, net_latency; grows with jitter), stamped with the sample time | ~4 KB/s per player on foot (~60 B x 60), ~6 KB/s in a car (E; +4 B stamp per state = +0.24 KB/s) |
| Actions (punch, throw, jump ...) | ride on PlayerState | as PlayerState | as PlayerState | none extra |
| Traffic cars | 20 Hz host rounds | 16.7 ms queue + N + fixed 120 ms playout | flushed + N + adaptive ~57 ms (traffic_test, quiet link; 140 ms under 80 ms jitter) | ~25 B per car per state: 20 cars x 20 Hz = ~10 KB/s typical, ~20 KB/s worst (cap 20 copies, E); Bulk: thinned first |
| NPC states | 10 Hz -> 15 Hz | 0..100 ms round + 16.7 + N | 0..66 ms + N (no playout delay: the AI walks to the newest state) | ~15 KB/s for 48 NPCs (was ~10; E) |
| NPC deaths / hits / claims | event | 16.7 + N (+ RTO 50 ms+ on a loss) | N (+ ~1 RTT on a loss) | < 0.5 KB/s (E) |
| Doors | door message -> next tick; poll 5 Hz -> 10 Hz | 0..200 ms poll + 16.7 + N | next tick on a door message, else 0..100 ms + N | < 0.2 KB/s events; snapshots on request ~2-6 KB each (E), Bulk |
| Items | scan 2 Hz -> 4 Hz | 0..500 ms + 16.7 + N | 0..250 ms + N | ItemStates ~1-3 KB/s near items (E), Bulk |
| Police | read 5 Hz -> 10 Hz | 0..200 ms + 16.7 + N | 0..100 ms + N | < 0.3 KB/s (E) |
| Car damage | rounds 4 Hz -> ~6.7 Hz | 0..250 ms + 16.7 + N | 0..150 ms + N | only changes: < 1 KB/s typical, a few KB in a crash (E) |
| Explosions, throws | event (detector) | 16.7 + N | N | < 0.2 KB/s (E) |
| Mission state / fail | on change | 16.7 + N (HostSave held back everything after it) | N; HostSave on its own lane | HostSave up to 256 KB once per checkpoint, Bulk |

Everything is also behind one hop more for guest -> guest traffic (via the host's relay, which forwards in the Poll
that received it). Lua <-> C++: every system's tick hands what arrived to its Lua glue in the same script tick
(`latency::Applied` measures that hand-off, `LAT hand`) and the glue applies it in that call; what the glue reports
goes out in the next render-thread `SessionTick` (0..1 frame, E: the script tick and Present are on different
threads).

Measuring: the sync trace has `LAT` records per applied message (send -> applied, session clock) and `CLK pd/jit`
per remote player; `tools/sync_compare.py` prints "Latency" (p50 / p95 / max per system and receiving game) and
"Bandwidth" (median / max KB/s per system from core's NetMonitor lines, thinned / deferred counts). The log has a
`latency (send -> applied, p50/p95/max): ...` line every 10 s. Messages a system took after its tick had stopped
for over a second (a guest in the menus or on a loading screen: WorldEnv received in the menu, applied once the
world is loaded) are `LAT held=1`, left out of those stats and listed apart ("waited for the game's tick").
