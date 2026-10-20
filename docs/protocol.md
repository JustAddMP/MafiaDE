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
One authority per NPC (0.16.0, `npc-authority`): NpcStates2 (0x0212), NpcInfo (0x0213), NpcLeave (0x0214),
GuestNpcStates2 (0x0424), GuestNpcInfo (0x0425), GuestNpcLeave (0x0426); NpcStates (0x0210) and GuestNpcStates
(0x0420) are retired, neither sent nor read: a peer of an older build sees none of the other game's NPCs, so this needs
a `modVersion` bump (see "NPCs").

0.16.0 (`modVersion` 0.16.0, so the session build is 1600 and a 0.15.0 game (build 1500) is rejected in the handshake
with "version mismatch: host has protocol 9 build 1600, you have protocol 9 build 1500. Both players need the same
MafiaCoop version."; JOIN FRIEND lists such a friend as "VERSION 0.15.0 - YOU HAVE 0.16.0"; core 0.12.0, its
`kProtocolVersion` stays 9): not playable with 0.15.0. New: PeerPaused (0x0103), ParkedCars (0x0104), JoinReport
(0x0207), NpcStates2 / NpcInfo / NpcLeave (0x0212-0x0214), TrafficSpawn2 / TrafficDespawn2 / TrafficStates2 /
TrafficBump2 (0x0224-0x0227; retired again by the host authority, Unreleased), GuestNpcStates2 / GuestNpcInfo / GuestNpcLeave (0x0424-0x0426), CarLook (0x0253),
PedStates / PedSpawn / PedGone / PedDeath / PedKill (0x0280-0x0284); retired: NpcStates (0x0210) and GuestNpcStates
(0x0420); WorldEnv gets an optional trailing u32 sampleMs; the PlayerState pose byte gets bits 3-5 (swimming, under
water, motorbike) without a format change.

Host authority, NPCs (unreleased, docs/host-authority.md WP-D `host-npcs`; NOT checked in game): NPCs come from the
host only. Retired, neither sent nor read (ids reserved, never reused): GuestNpcEnd / HostNpcHit / NpcClaim
(0x0421-0x0423) and GuestNpcStates2 / GuestNpcInfo / GuestNpcLeave (0x0424-0x0426), as GuestNpcStates (0x0420) since
0.16.0. A guest's NPC that no host NPC is is held hidden on his game (see "Held NPCs"); a 0.16.0 guest's own NPCs are
not shown on such a host (the integration's `modVersion` bump keeps the builds apart).

Latency (0.15.0; core `kProtocolVersion` 9: lanes, session clock, app message send stamps, core
docs/protocol.md "Latency"): no message of ours changes; PlayerState's u32 time is now the moment the state was
sampled (was: sent), which only shifts the receiver's timeline. See "Latency budget" at the end.

Reconnect (core docs/protocol.md "Reconnect"): no app message of ours changes and none is added. A guest whose
connection was lost keeps playing while core rejoins the host; the host holds his slot. See "Reconnect" at the end.

Mission fail: MissionFail (0x0205) and FailVerdict (0x0206), additive (a peer without them ignores them).

Leash (host authority, docs/host-authority.md section 3; not yet checked in game): LeashConfig (0x0208) and LeashEvent
(0x0209), additive (a peer without them ignores them; a guest without the host's LeashConfig uses the defaults). See
"The leash" below.

Car damage: CarDamage (0x0250), CarDamageReport (0x0251), CarDamageRequest (0x0252); additive (a peer without them
ignores them and keeps its own car damage). See "Car damage". Since the car damage echo fix a copy never reports its
wreck, the exploded flag, the game's reaction to our own writes or the part change of a rebuild by its tuning, and
the holder drops such reports (rules only, the bodies are unchanged; an older peer's echoes are dropped by a newer
holder).

Car look (a copy shows its car's colours, plate, tint, interior, painting and body variant): CarLook (0x0253),
additive (a peer without it ignores it and keeps its copies as the game spawns them). See "Car look".

Pedestrians (0.16.0; host only since host authority): PedStates (0x0280), PedSpawn (0x0281), PedGone (0x0282),
PedDeath (0x0283) host -> guest, PedKill (0x0284) guest -> host. See "Pedestrians".

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
| 0x0103 | PeerPaused | every peer -> everyone (`Broadcast`) | R | u8 flags (bit0 paused), u8 reason (0 resumed, 1 no script tick, 2 the game's paused tick only), u32 ms paused so far (on the resume: how long it lasted). 0.16.0; see "A paused game" below |
| 0x0104 | ParkedCars | host -> a player who joined or came back (`SendTo`) | R | u8 version (1), u8 count (<= 24), count x { u8 keyOwner, u32 vehId, str model, f32 x, y, z, f32 qx, qy, qz, qw, u8 flags } (0.16.0; see "Parked cars for late joiners") |
| 0x0105-0x0107 | reserved (player sync events, sync architecture package `players`) | | | |
| 0x0108-0x01FF | reserved (phase 3 events) | | | |

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
                          (0.15.0): 7 jump, 8 vault, 9 climb up a ledge, 10 drop down, 11 hard landing, 12 ladder up,
                          13 ladder down, 14 slide into cover (7..14 are moves: the puppet changes place), 15 medkit,
                          16 body picked up, 17 body dropped; doors (0.15.0): 18 door kick, 19 lock pull
                          (lockpick), 20 door opened by hand (the door itself is "Doors" below); other values
                          read as none
-- pose block (traversal; after the action block; optional, see below):
u8  pose                  what he keeps doing: bit0 sneaking (IsStealthMove), bit1 carrying a body (IsCarryingBody),
                          bit2 on a ladder (from USE_LADDER until he stops climbing); 0.16.0, without a version
                          change: bit3 swimming, bit4 under water (game/water.h), bit5 on a motorbike
                          (the vehicle block above is a bike: its model, seat, key and transform); other bits are
                          dropped (a build without bits 3-5 drops them: its puppet walks the swimmer along the
                          bottom and takes the bike for a car model)
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

**Water and motorbikes in the pose block** (0.16.0; src/game/water.h, src/game/puppet_guard.h; unit-tested in
tests/water_test.cpp, tests/puppet_guard_test.cpp, tests/player_actions_test.cpp, tests/traversal_test.cpp; NOT
checked in game). Three more pose bits, added without a version change exactly like the pose block itself: an older
reader masks them away. Bit3 swimming / bit4 under water: the sender's water tracker (the IN_DEEP_WATER human message,
the height where he went in, a swim getter if the game has one) says so; meanwhile no jumps or drops are sent and
flags bit5 (in the air) is cleared. Bit5: the vehicle he is in is a motorbike (a scene object with a
`C_RuntimeMotorcycleComponent`, not a car); the vehicle block carries it like a car (model e.g. `motorcycle_1935`, seat
0 rider / 1 pillion, the key, the transform: the bike's lean and a wheelie are in its rotation, velocity from its
positions when the bike has no GetSpeed; controls and signals 0 / unknown). The receiver: a swimmer's puppet is put
at his position every tick with swim strokes, a bike's copy is pinned to the rider's transform with the puppet on it,
and a puppet whose player is sent as on foot faster than 12 m/s (a vehicle the sender could not read) is hidden at his
position until he is below 9 m/s (docs/game-api.md 25). Older senders (0.15.0) send a bike ride as an on-foot sprint:
a receiver with this change hides that puppet instead of letting it sprint and "jump".

**0.16.0 (sync architecture, package `players`): PlayerState unchanged on the wire.** The receiver now drives
the puppet's gait from `mode` (the sender's own gait, see "Driving the puppet and the car copy"), and interpolates
through core's shared replication layer (`net/replica.h`; `src/game/player_motion.h` `PlayerTrack`: one adaptive
playout clock per sender, a track for his body and one for the car he drives - cleared when he is in another car or
on foot, so two cars are never blended). Same curve and limits as below (Hermite through the states with their
velocities, 250 ms extrapolation, then held, discrete fields from the nearer state, controls blended); two changes:
the "now" lead is the playout delay **plus the one-way trip** (rtt / 2 of the session, at most 150 ms: the playout
clock's offset contains the smallest one-way delay, so without it the copies trailed by ~14 ms, 0.35 m at 25 m/s),
and it may move a position up to **4 m** (was 1.5 m: a car at 30 m/s is 1.5 m ahead after 50 ms).

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

### Driving the puppet and the car copy (0.16.0 rewrite; NOT checked in game)

The receiver turns his presented state (the interpolated state led to "now", above) into commands, one set per remote
player, in `player_sync.cpp` through `src/game/player_motion.h` (game-side policy over core's
`net/remote_motion.h` controllers and `net/correction.h` rules; unit-tested in tests/player_motion_test.cpp against
recorded 0.15.0 paths); the Lua glue carries them out and reports back where the puppet / car copy is after each tick
("PS" / "VS" lines).

- **On foot** (`FootDriver` over core's `LocomotionController`):
  - **Gait = his.** The gait is the `mode` his game reports (`gaitHint`: idle / walk / run / sprint), not one chosen
    from the wanted speed (0.15.0: gait matched 63-76 % of the time, "player run, puppet sprint" 8-15 %). Only the
    speed multiplier (`SetMovementSpeedMult`, 0.6-1.8, given right before each `MoveVec`) absorbs the error; when even
    1.8 is too slow (the game's AI sprint is ~3.3 m/s at 1, a player's 6.5-7.5) the shortfall is carried as a small
    nudge along his direction. The AI's speeds at multiplier 1 are first guessed as the 0.15.0 traces measured them
    (walk 1.3, run 2.2, sprint 3.3 m/s) and calibrated on the puppet itself at run time.
  - **Commands land in time.** The controller plans for the AI's command lag (0.15 s: the error and his velocity it
    will have then, his stop point when he brakes); a changed speed multiplier re-issues the walk at once (at most
    every 0.1 s) instead of at the next 0.35 s re-aim.
  - **Hidden corrections.** Errors beyond 0.3 m are slid away (2 / s) at most 35 % of the faster of him and the
    puppet (a puppet still running after he stopped is pulled back within its own motion); when he brakes or turns
    away and the puppet runs on level with him or ahead, its body is held back to his speed (at most 70 % of its own,
    its legs keep running: a moment of foot sliding instead of an overshoot it would walk back). Snap beyond 3.5 m
    (3 m height, 6 m in the air), when he stands and it is 0.5 m off for 2 s, or when blocked 1.5 s.
  - **Standing.** His game says idle: within 0.6 m the puppet stands at once (no walk for the last bit: a shuffle) -
    its stop is a walk to where it comes to rest by itself along its own line of motion, never back to a point it
    passed - and the rest blend slides it onto his spot (from 0.1 m off, 4 / s, at most 0.15 m/s, or 35 % of its own
    speed while it still brakes). Farther off it walks there. It turns to his heading at up to 400 degrees/s.
  - Simulated (tests/player_motion_test.cpp: 73 s of the host's real running, sprinting and stopping from the 0.15.0
    traces, sent at 60 Hz over 14 ms + jitter, against an AI walker 150-250 ms late, speeds off +-20 %, turning at
    most 360 degrees/s): p95 0.65-0.75 m, mean 0.25-0.31 m, standing p95 0.25-0.27 m, gait 99.8-99.9 %, no snaps.
    How closely the game's AI walk matches that walker is UNVERIFIED.
  - Unchanged: while he gets in or out of a car (seat 9) his puppet walks with him to the door; next to a standing car
    it gets in with the game's animation; actions, traversal moves, ladders, swimming and bikes pause the controller
    (busy flags) as before.
- **His car** (`CarDriver` over core's `VehicleController`): his controls are applied to the copy every tick
  (`SetSteer` with the response curve inverted, `SetPower`, `SetBrake`, `SetHandbrake`), so its own physics drives it.
  - **Position: the car preset.** Nothing is set while the copy is within 0.5 m and turned less than 3 degrees (most
    ticks: no `SetPos`, no `SetRot`); beyond, a blend of 5 / s of the excess at most 50 % of his speed (core's
    `ForCar` preset with a faster blend: a copy whose steering acts ~0.15 s late drifts wide in turns); while his car
    slides (its velocity more than 15 degrees off its heading above 2 m/s: a skid, a spin, a crash) at most his whole
    speed. Snap beyond 4 m, when stuck (he stands, 1 m off for 2 s) or on a teleport (> 20 m between snapshots).
  - **Speed.** After every move the copy gets his speed back (`SetSpeed`, km/h, Round 4) - his speed plus the error
    along the copy's own heading (1.5 / s, at most 4 m/s; a sideways error given as speed along the heading made a
    turning copy run wide). Not moved but its own speed is off his by more than 1 m/s or 12 %: given his speed alone
    (`T` field 69, at most every 0.25 s; UNVERIFIED that `SetSpeed` alone keeps the car's physics sane).
  - **Pins only as a last resort** (core 0.12 policy): pinned to his transform every tick when its physics does not
    keep up (slower than 35 % of him for 1 s above 3 m/s, more than 8 m/s faster, or more than 1.5 m off for 1.5 s),
    its physics retried after 1.5 s, doubling to 8 s, the doubling reset after 10 s kept up (was 5-40 s). VEH `pf` /
    `ri` say how long and until when.
  - Simulated (tests/player_motion_test.cpp: 30 s of the host's driving, 0-29 m/s, with a crash, against a copy whose
    physics reaches 85 % or 115 % of his speed 150-300 ms late): p95 0.88-0.98 m, pinned 0-0.1 %, copy / driver speed
    1.01-1.02. UNVERIFIED in game.
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

Conflicts (one wheel, one driver; 0.16.0, src/game/seats.h `LocalWheel`, unit-tested in tests/seats_test.cpp; the
2026-10-19 two-PC test had both players drive one car for 100 s, its damage bouncing between the games): when a game
reads its own player in seat 0 of K while another player's state names seat 0 of K too,
- and that player already drove K on this game, the reading is the game's **slide** (his puppet left the seat here:
  thrown out, down in the water, respawning, and the game moved our passenger to the wheel), not a takeover: we give
  way;
- otherwise (both took the wheel before seeing each other's state) the car's **holder** keeps it (the key's owner,
  the player who first drove K: `1:4746250` -> player 1), and when neither is the holder the lower player id. Both
  games apply the same rule to the same facts, so exactly one keeps it.
The player who gives way reports seat 9 (claims nothing) from that very tick, so the keeper's game never sees two
claims; his game then slides him to a free, unclaimed passenger seat (`SlideToSeat`, three tries 0.7 s apart) or, with
none, gets him out (`GetOffVehicle` with the controls locked, two tries); if neither works he stays, still claiming
nothing (logged; press E). The car's damage follows the single driver (car_damage.lua: the holder of a car's damage
is its driver). While a player sits in a passenger seat of a car another player drives, his game sets
`PlayerShouldRemainPassenger(true)` (UNVERIFIED that it stops the game's slide). Trace `WHL`. Before this, each game
kept its own player driving ("drives it too; we keep it") until one of them got out. On a third game (both are remote there) the one who had the wheel there first keeps it until his state leaves
the wheel (0.15 multi-guest; before, the claim flipped between them every tick, with a "drives it" line and a fresh
copy swap each time; trace `DRV`). Copies are capped at 12 per game (the oldest parked copy nobody sits in is despawned; never one with a real
car hidden behind it). When the session ends, copies nobody sits in are despawned (a hidden real car is shown at
the copy's place); the one the local player sits in stays as a normal car (a real car hidden behind it comes back
once he got out) until he gets out, then it is despawned when unseen (0.15 multi-guest, `M.LeftCopiesTick`; before,
it stayed with despawn immunity for the rest of the game). The same for a host traffic car's copy that a player took
(traffic_sync.lua hands its obtain id over with it, so player sync despawns it like its own copies: at the session
end, on a lost connection, when it is trimmed) and for an adopted ambient traffic car of this game (its despawn
immunity goes off again). A player who joins late, or comes back after a lost connection, gets the cars parked
before (ParkedCars, below; before this he did not see them until somebody drove them).

#### Parked cars for late joiners (ParkedCars 0x0104; 0.16.0, NOT checked in game)

`src/game/parked_cars.h`, unit-tested in tests/parked_cars_test.cpp. Every game keeps a book of the CarParked
messages it sent or received in this session (key, model, final transform, flags); a car that somebody drives again
(a PlayerState with seat 0 of that key) leaves it. The **host** sends a player the whole book as soon as he appears
in the session with a new `(id, joinSeq)` - a late joiner, or a guest back after a lost connection - in messages of
at most 24 cars, reliable, lane 0 (the player-car order), priority Normal. The receiver takes each car as if its
CarParked had just come (player_sync.lua `M.Parked`: a copy at that transform, or his own car of that key if it
stands there), except destroyed ones (flags bit 0): an intact copy of a wreck would stand there, so they are left
out (logged). Accepted before the receiver's sync is active (it can arrive in the poll that completes the join); the
cars are placed once he is in the world. Trace `PKB` (book changes), `PKS` (sent), `PKR` (received). Older builds
ignore the message (unknown app id).



Puppets that cannot be seated (seat taken, the game keeps throwing them out) are hidden at the car and retried every
4 s. The 0.15.0 two-PC test: the host's car `manta_prototype` threw puppets out 1-3 frames after every non-animated
`GetOnVehicle` on both games (seat 0 of a standing copy on the guest at 21:06:49, seat 1 of the host's moving car at
21:07:17-19), the puppet landing 0.8 m off on the car's roof with physics on; no `SetPos` / physics change of ours in
between (the in-car path of `M.Drive` only seats), the other cars of both sessions kept their puppets. Taken to be the
game's (that car model), UNVERIFIED; the thrown-out log line now names the model.

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

### A paused game (PeerPaused 0x0103; 0.16.0, NOT checked in game)

Code: `src/game/peer_pause.{h,cpp}` (pure: wire, detector, registry; tests/peer_pause_test.cpp and the host pause in
tests/traffic_test.cpp), sent and received in `src/game/player_sync.cpp`. While a game is paused (its pause menu, the
CO-OP panel opened from it, a loading screen) its script tick does not run, so it sends no PlayerState and, on the host,
no traffic or NPC states. In the 2026-10-19 two-PC test the guest's copies then coasted on their own, and when the host
resumed 6-10 "knocked ... put back, reported to the host" corrections fired within 0.5 s (and the host logged each
report back).

- **Sender:** the render thread watches the game's script tick (`lua::LastTickAt`, `LastNormalTickAt`: the hooked
  `C_ScriptMachine::Tick` / `TickPaused` of the game's machine). No tick for 0.3 s while the world is loaded (or only
  `TickPaused` ticks for 0.3 s) = paused: PeerPaused `paused=1` goes to everyone, again every 2 s while it lasts (a
  late joiner learns it), and `paused=0` with the duration on the first tick after it. Logged as `our game is paused
  (...)` / `our game runs again after N s`, trace `PAU`. Outside the world (menus, lobby) nothing is sent.
- **Receivers** (registry `peer_pause::For(id)`; Lua `mc_peer_quiet(id)` -> 0 none, 1 paused, 2 grace):
  - his puppet and the car copy he drives are held where they stand here (from their last reports): the puppet stands,
    the car is pinned to that pose every tick, braked, without speed; no pin is counted. Logged `player N's game is
    paused: his puppet and his car held ...` / `... runs again ... (held N s)`, trace `PAU ev=freeze/thaw`;
  - the host paused (the only traffic authority; 0.16.0: also any guest whose traffic we showed): every copy of his
    traffic is held the same way (`ReceiverTraffic::SetHostPaused`), none goes stale for the missing states and none is
    spawned; after his resume they are driven again and no knock (TrafficBump) is reported for 1 s (counted as `quiet`
    in `TRG`); logged `traffic: the host paused his game: N copies ... held`, trace `TRZ`;
  - the host's NPCs on a guest are not pulled back while he is paused or within 1 s of his resume (npc_sync.lua,
    `not put back: the host's game is paused or just resumed`);
  - the leash (see "The leash"): while the host is paused a guest's warning or countdown freezes at its value and the
    banner says `THE HOST PAUSED`; no pull until he runs again.
- **Our own pause** counts too: after a tick gap of more than 0.5 s (our game stood while the host's cars drove on)
  the guest reports no knocks for 1 s (`traffic: our own game stood for a while`, `TRZ ev=localgap`), and the resume
  of our own detector puts every copy of everybody's things in the 1 s grace.
- Older builds ignore 0x0103 (an unknown app id); the receiver tolerates trailing bytes and rejects a body shorter
  than 6 bytes.

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
| 0x0200 | WorldEnv | host -> guests (`Broadcast`; `SendTo` a newcomer) | R | f32 timeOfDay (hours 0..24), f32 timeFlowSpeed, str weatherSet, u32 sampleMs (0.16.0, optional) |
| 0x0207 | JoinReport | guest -> host (`SendToHost`) | R | u8 flags (1 reconnect, 2 complete, 4 late), u32 afterMs, u8 n, n x (u8 system, i32 ms of its first arrival, -1 none), str missing (at most 200 bytes) |
| 0x0208 | LeashConfig | host -> guests (`SendTo` each guest once per join / return, `Broadcast` on a change) | R | u16 warnM, u16 maxM, u8 countdownS (0 = warning only), u8 flags (bit0 1 = leash on); 6 bytes, trailing bytes ignored |
| 0x0209 | LeashEvent | guest -> host (`SendToHost`) | R | u8 kind (1 warn, 2 countdown, 3 pulled, 4 back), u16 distM (to the host's puppet on the guest's game); on each change and every 5 s while warn / countdown last |

WorldEnv (src/game/world_env.h, world_env.cpp, world_env.lua; since the world-join change no longer in PlayerSync) is
sent while the host is in the world: at once to every newcomer (also a guest back after a lost connection: newcomers
are keyed by `(id, joinSeq)`; one who arrives before the host has a reading gets it with the first one), at once when
the weather set or the time flow speed changes, and every 2 s (was 4 s). Priority Critical, lane 1.

- `sampleMs` (new, 0.16.0): the moment the host's game was read, on the session clock (ms). It is a trailing
  field: the reader before this change reads the three fields and never checks for the end of the body (verified in
  the 0.15 code: `OnWorldEnv` read f32, f32, str and `ByteReader` leaves the rest), so an older guest still works; a
  newer guest that gets no `sampleMs` from an older host takes the message's send stamp (core's, `MessageStampMs`)
  or, without one, its arrival. No protocol / version bump is needed for it.
- A guest predicts the host's time now: `timeOfDay + age x rate x timeFlowSpeed` (age = session clock now -
  `sampleMs`, 0..600 s; rate = game hours per real second per unit of flow speed, measured from the host's samples: the hours
  between two WorldEnv over the session time between them x his flow speed, two agreeing observations (~4 s) for the
  first value, one more than 2x off ignored (a jump of his clock); the first guess is 1/60 from docs/game-api.md 8,
  UNVERIFIED). The error is the host's predicted time
  minus ours, the short way round 24 h.
- The first time after a join, a reconnect or a new world of ours: `SetTime(t, 0)` once when 0.5 game minutes or
  more off. During play: below 0.5 game minutes nothing; up to 15 game minutes the guest's flow speed is the host's
  x (1 + share), share = error / (2 s x the host's game minutes per second), at most +-25 % in 1 % steps, until the
  error is under 0.1 game minutes, then exactly the host's flow speed again (no overshoot: the error closes at most at
  error / 2 s per second); beyond 15 game minutes, when the host's time stands still (flow 0, missions, the Free Ride
  of the 2026-10-19 test) or when a catch-up would take over 30 s: `SetTime` once (then 2 s for it to show). Until the
  rate is measured (~4 s), only errors beyond 15 game minutes are corrected. When the session ends during a catch-up,
  the host's flow speed is put back.
- The weather set is set at once when it differs (`SetWeatherSet(name, 0, 0)`; asked again every 2 s while the game
  reports another name).
- Guests' own weather/time scripts may change things in between; the next WorldEnv (or the guest's own reading of a
  different flow speed) puts them back.
- Trace: `WEH` (host), `WEG` (guest), docs/sync-trace.md; `tools/sync_compare.py` "World" and the
  `world.time_err` target. UNVERIFIED in game: how smoothly the game's clock (sun, shadows, street lights) follows a
  changed `SetTimeFlowSpeed`, and that `GetTimeFlowSpeed` reads back what was set.

### Join and reconnect: when is a guest's world complete (0.16.0, not checked in game)

On a guest, from the moment the session became active (a join, or back after a lost connection; src/game/join_sync.h)
each system's handler marks the first time it got its part of the world: the first PlayerState of every other player,
the host's MissionState, WorldEnv, the DoorSnapshot (asked 3 s after we are in the world), the first NpcStates and
TrafficSpawn (optional: the host may have none for us), the first PoliceState of every other player, and the first
CarDamage answer for every car copy that asked for one meanwhile. The world is complete when every required part is
here and we are in the world (log `join: world complete in X s`, trace `JN ev=complete`); when something required has
not come 15 s after we are in the world, `join: still missing ... after 15 s` (`JN ev=complete missing=...`), and a
late complete follows if it comes. The guest sends the result in JoinReport 0x0207 (once per verdict); the host logs
it (`join: player N's world is complete in X s (...)`) and traces it (`JN ev=report`). An older host ignores the
message. `tools/sync_compare.py` "Join and reconnect" and the `join.complete_s` target (<= 10 s). The leash waits for
this verdict (complete or the 15 s report, `JoinSync::Settled`) and 20 s more after every join and reconnect.

### The leash (host authority, not checked in game)

src/game/leash.h (the logic, tests/leash_test.cpp), src/game/leash_sync.h (the session part), player_sync.lua
`M.Leash` / `M.LeashPull` (the reading and the pull); the spec is docs/host-authority.md section 3. The host reads
`leash_warn_m` (300), `leash_max_m` (400) and `leash_countdown_s` (10) from MafiaCoop.ini every 2 s and sends them in
LeashConfig to every guest when he joins or comes back (newcomers keyed by `(id, joinSeq)`) and to everyone when they
change; `leash_warn_m = 0` turns the leash off, `leash_countdown_s = 0` is warning only. On a guest every script tick:
the ground-plane distance from us (our car while we sit in one) to the host's puppet's newest target; 300 m and over
the banner `FAR FROM THE HOST - THE STREETS THIN OUT (340 M)` (in 10 m steps), over 400 m `TOO FAR FROM THE HOST -
BACK IN 10 S` counting down (under 380 m stops it, under 280 m ends the warning: hysteresis 20 m), at 0 s the pull
while we are still over 400 m: the screen fades out (`HUD_FaderFadeOut(0.25)`, as the failed-mission flow uses it),
on foot we are put beside the host (`M.NearHost`, as after being downed), at the wheel our car goes on the road 15 m
behind the host (`game.traffic:GetClosestPointOnRoad`, its second argument UNVERIFIED; else 4 m to his right),
facing his heading and stopped, and we stay in it (its copies on the other games jump with it); the screen fades in
and `PULLED BACK TO THE HOST` shows for 3 s. A pull that does not work is tried again 2 s later. The banner is the
centre notice (module.cpp `DrawNotice`) after the reconnect and the story notices. Suspended (no banner, no
countdown): our world not loaded and 5 s after; a fail-flow reload; the join / reconnect not settled (JoinReport
complete or its 15 s verdict, `JoinSync::Settled`) and 20 s after; no state of the host's here; the host paused
(PeerPaused: a running countdown or warning freezes, banner `THE HOST PAUSED`); a cutscene and 5 s after; we or the
host downed / dead; 15 s after the host's puppet jumped more than 50 m in one state; we sit in the car the host is in;
we are a passenger (the driver's leash moves the car). Each warn / countdown / pulled / back goes to the host in
LeashEvent; his status panel shows `<NAME> IS FAR AWAY (420 M)` (12 s after the last one) or `<NAME> WAS PULLED BACK
TO US` (5 s), no banner over his game. Trace `LSH` (docs/sync-trace.md); `tools/sync_compare.py` "Leash" with
`leash.pulls` (info) and `leash.false_pulls` (0).

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

### NPCs (0.7.0; 0.11.0 changed NpcStates and NpcHit; 0.12.0 NpcStates; since 0.16.0: one authority per NPC)

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0210 | NpcStates | - | - | retired (0.16.0): neither sent nor read. Was: u8 n, then n x the full record (u32 key, f32 x y z, i8 hx hy, u16 health, u8 flags, u8 aiType, u32 tag, str weapon, u64 profile, u8 shots), host -> guests, 10-15 Hz, no time stamp |
| 0x0211 | NpcDeath | host -> guests (`Broadcast`) | R | u32 key |
| 0x0212 | NpcStates2 | host -> each guest (`SendTo`) | U, per NPC 15 Hz near to 5 Hz at 120 m | u32 sampleMs (the host's steady clock when its Lua round read the NPCs: the clock of PlayerState's time), f32 origin x y z, u8 n (at most 48: 929 bytes), then n x 19 bytes: u16 net, i16 x y z (cm from the origin; NPCs more than 327 m from it go into another message), i8 hx hy (heading x127), i16 vx vy (ground velocity, cm/s), u16 health, u8 flags (1 dead, 2 in a car), u8 shots (fired, mod 256), u8 gait (0 stand, 1 walk, 2 run, 3 sprint: from its speed on the host) |
| 0x0213 | NpcInfo | host -> each guest (`SendTo`) | R | u8 n, then n x: u16 net, u32 key, u8 aiType, u32 tag (battle archetype, hashed; 0 none), str weapon ("-" none), u64 profile (spawn profile hash, 0 unknown), u8 nameClass (0 unknown, 1 a mission spawner's human, 2 a generator's); at most 1000 bytes per message |
| 0x0214 | NpcLeave | host -> each guest (`SendTo`) | R | u8 n, then n x u16 net: no longer sent to him (out of his interest, gone) |
| 0x0410 | NpcHit | guest -> host (`SendToHost`) | R | u32 key, f32 amount, u8 damageType, u8 hitZone, u32 weaponId, u8 flags (0.11.0: 1 = lethal, the guest's replica died of his hits) |

**One authority per NPC** (0.16.0; `src/game/npc_replica.h`, `tests/npc_replica_test.cpp`; NOT checked in game).
Every NPC is simulated by exactly one game: the host (mission NPCs, police, anything with a job, mission civilians;
since host authority, docs/host-authority.md, the host only: an NPC only a guest's game has is held there, "Held NPCs"
below; 0.16.0 made the guest the authority for those, "Guest-owned NPCs (retired)"). Every other game shows a **replica**
(its own NPC paired with it, or a spawned copy) that is driven from the authority's states every tick and never runs
its own AI for it. Before, the guest's paired NPC ran its own combat and movement AI and was only pulled back after
6 m (12 m in a fight) for 1.5 s: host cops chasing the guest were p95 60 m, max 252 m off in the first two-PC test.

- key = the NPC's entity id on the host's game (the number in its `C_EntityGuid`), as in NpcDeath, NpcHit and Hit's
  npc. **net** = a u16 the host gives an NPC while it publishes it (one number space for all
  guests; freed when the NPC is gone, reused only after the 65535 others), named by NpcInfo. Ordering: a guest gets
  NpcLeave, then NpcInfo, then NpcStates2 of a round (the two reliable ones on lane 0 in order); states of a net id
  whose info has not arrived yet are kept and shown once it has; an NpcInfo naming a known net id with another key
  replaces it (the old NPC's replica target goes).
- **What the host sends (interest, per guest):** every round (15 Hz) the host's Lua glue lists the NPCs within 140 m of
  any guest's puppet (128 at most, nearest first) that are not **ambient humans** (not a player / puppet / copy, not in
  a vehicle, AI type 0 and not named after a mission spawner: `npcrep::IsAmbientHuman`, the contract with the
  `ambient-peds` package, whose humans in `MafiaCoop.PedOwned` are skipped as well) and not deactivated. Core's `InterestManager` (Kind::Npc) decides per guest: viewpoint = his puppet; enter within 120 m,
  leave beyond 140 m; one state every 1/15 s up to 30 m, linearly down to one every 0.2 s at 120 m (5 Hz; the plan's
  2 Hz left the far target 0.6-0.7 m off at p95 in the unit test), an NPC slower than 0.35 m/s half as often (at most
  1 s apart), a dead one at most once a second; a byte budget of 7000 B/s of records per guest (~8 KB/s with the
  message headers; deferred NPCs go next round, oldest first). NpcStates2 per guest: 48 NPCs in range measured
  6.3 KB/s in the unit test (NpcStates was ~21 KB/s at 48 NPCs). The velocity and the gait are the host's (from its
  last two rounds); the slow fields go once per guest in NpcInfo and again when they change.
- **The replica (guest):** one `replica::Stream` (core net/replica.h) for the host's NPCs: its playout clock from the
  messages' sample times, a track per net id; each NPC is shown further in the past by its own sample interval (a
  5 Hz far NPC is interpolated like a 15 Hz near one, changing at most 0.3 s per s) and led to "now" along its velocity
  and acceleration (bounded: 6 m/s^2, 3 m). Per replica a `LocomotionController` (core net/remote_motion.h; the gait it
  chooses itself from the wanted speed and the target's acceleration: the host's gait byte, derived from speed, lagged
  every start and stop) plans the walk and a `Corrector` (preset ForNpc with the dead zone 0.3 m and at most 50 % of
  the speed: snap beyond 5 m, 3 m high, stuck 0.8 m for 3 s, a teleport of the target; the controller's own snap when
  the walk gets nowhere for 1.5 s) fixes the position. Every tick the C++ side sends the Lua glue one command per
  replica (walk to a point with a gait and a speed multiplier, a nudge, a snap, a turn) and gets where it is after it
  (`npc_sync.lua` `N.DriveAll`). Simulated (unit test, the session-1 cops' recorded paths through a lagged AI walker,
  latency 25-40 ms, 0-2 % loss): replica vs the host's NPC p95 0.97 m, max 2.15 m, no snaps (UNVERIFIED in game).
- **Pairing (guest):** a guest pairs each host NPC with the nearest human of his own world within 1.5 m of it (the same
  mission scripts spawn the same NPCs at the same spots on every game, docs/game-api.md 15) and keeps the pairing.
  0.11.0: a host NPC with a job (AI type not 0) that found none there within 2 s is paired with the nearest free local
  human of the same tag (or, when one side has no tag, the same AI type) within 40 m (since 0.16.0: and put at the host
  NPC's spot when more than 5 m off); all candidate pairs are taken nearest first (same spot before moved apart, the
  same tag counts at half its distance); dead host NPCs are paired too (their replica is killed). Deactivated humans
  (`IsActive()` false), ambient humans and `MafiaCoop.PedOwned` humans are never paired. A pairing is released after
  5 s without a state (out of the guest's interest, gone); a host NPC in a vehicle (its car flag, or faster than
  11 m/s, held 2 s) is never loose-paired with a local NPC on foot (nor the other way round), and a pairing whose host
  NPC drives while ours walks more than 40 m away is released after 2 s.
- **The paired NPC's own AI is stopped** while it is a replica (0.16.0; UNVERIFIED which call holds, docs/game-api.md
  15 "Replicas"): `InterruptBattle`, `InterruptCivilWander`, `InterruptPatrol`, `InterruptAvoidingCar`,
  `SetReactionOnSound(false)` on pairing, `InterruptBattle` again every 2 s while it is driven; `ResumeBattle` and
  `SetReactionOnSound(true)` when the pairing ends. A paired NPC that does not follow (three snaps within 20 s, not
  counting the host NPC's own teleports) is given up: hidden where the host's NPC is (no model, physics state 2,
  demigod), kept paired so it is not paired again and dies with the host's NPC, and a spawned copy shows the host's
  NPC instead. Mission scripts keep running on the guest: a mission NPC is never despawned.
- **Health and death:** the replica's health follows the host's NPC down (never up); it dies when the host's dies
  (killed; one that still does not die after 3 tries is hidden).
- **Shots and damage (one authority):** a replica's shots are pictures (a copy's and, since 0.16.0, an armed paired NPC's:
  the host's shot counter fires its gun along its heading, past the guest when it faces him). A replica's bullet that
  hits the guest here is given back (player_sync.lua `M.OnLocalDamage`, `N.IsPicture`). The guest's damage from host
  NPCs always comes from the host: their hits on his puppet there go to him as a Hit with source 1 and the NPC's key,
  and are always applied (since 0.16.0: before, a hostile paired NPC that "fought him on his own game" skipped them; now
  only an NPC his own game is the authority for skips it, `npcrep::SkipRelayedHit`: none since host authority).
- **The guest's hits:** a hit on a replica hurts it on his game and goes to the host as NpcHit; the host replays a
  bullet as a real shot from the guest's puppet (`CreateShots`, so the NPC's own hit reaction and damage apply), else
  `DoDamage(amount)`; a replay that does not land within 0.35 s is `DoDamage`. When the guest's replica dies within
  1.5 s of his hit, he sends a lethal NpcHit (no damage of its own): 0.8 s later the host kills his NPC if it still
  lives (an enemy that his mission keeps invulnerable is made vulnerable for it; an invulnerable ally or civilian is
  left alone).
- Enemies and guests (0.9.0): on the host each guest's puppet gets the FRIEND brain 2 s after it spawned
  (`SwitchBrain(1)`, docs/game-api.md 15; ini `enemies_target_guests = 0` turns it off), so the host's enemies shoot at
  it; an NPC that a guest attacked fights back at his puppet (hostile NPCs only).
- Spawned copies (0.12.0): a host NPC with a job (AI type not 0), on foot, alive, with a known profile, within 90 m of
  the guest, that found no partner for 5 s (2 s for the loose pairing, then 3 s more; none in the first 10 s after
  the guest's world loaded; at once for a paired NPC that did not follow) gets a copy on the guest's game: a human
  from a runtime spawner (like the player puppets) with that spawn profile, its health, and the NPC's weapon. The
  host knows the profile from the NPC's name: a human a mission spawner made is called
  `behavior_object_prime-NN-<spawner name>`, and the spawner (entity type 65, by that name, the nearest when several
  share it) tells its profile (docs/game-api.md 15); 0 for NPCs from traffic or police generators: an NPC with
  profile 0 gets a fallback by its AI type (4: the police profile `characters/ai/police_scotch/ai_police_triggerman`,
  else the civilian `characters/traffic/traffic_base_ambient_combined`, as their FNV-1 64 hashes, `mc_fnv`). The copy
  is a replica like a paired NPC (driven the same way, 0.16.0; before: walk/run toward the newest state plus its
  velocity, pulled along), released 12 s after its death, 5 s after the last state, when the NPC gets into a car,
  when the guest is more than 110 m away, during a cutscene, when the session ends and with the world. When the
  guest's own game spawns that NPC after all, his own takes over and the copy is released: no doubles. At most 10
  copies at a time, one spawn profile loading at a time.
- Measuring (docs/sync-trace.md): the host writes `NPT` for every NPC state it sends (its sample time and position),
  the guest `NPR` per replica at 10 Hz; `tools/sync_compare.py` lines them up (`npc.p95`, `npc.max`) and reads the
  NPC bandwidth per guest from `NBW` (`npc.bw`).
- Ambient pedestrians are not npc_sync's: they are replicated by ped_sync (below, "Pedestrians"); npc_sync never
  sends, pairs or drives one. Ambient traffic and police cars are the host's (Unreleased host authority; 0.16.0: by region; 0.15.0: the host's near him; below, "Traffic"). Cars a player drives are shared as before (Phase 3); a
  remote driver's car is first looked for here: a car of the same model standing where his car stands (within 3 m)
  is taken as his car (mission cars are not doubled), and no copy is spawned into another car standing there.
  Such a mission car does not keep an NPC at its wheel: when the remote driver's puppet is thrown out of it twice
  (0.9.0), it is swapped for a fresh copy (the mission car hidden with `Deactivate`, the copy spawned in its place) and
  the puppet drives the copy; when the remote driver parks it, the mission car comes back at the copy's place. No
  copy is looked for or spawned during a loading screen and for 4 s after it (a copy spawned before the mission had
  placed its car landed on it).

### Held NPCs (host authority; unreleased, NOT checked in game)

NPCs come from the host only (docs/host-authority.md). Every game runs the chapter's mission scripts, so a guest who
got somewhere first has NPCs the host's game has not spawned (yet). Such an NPC is never sent anywhere and does not
fight on the guest's game; the rule is C++ (`src/game/npc_hold.h`, unit-tested by `tests/npc_hold_test.cpp`), the
game side npc_sync.lua (`N.HoldRound`, `N.Hold`, `N.Unhold`):

- **Which (guest):** every round (15 Hz) his Lua glue lists his NPCs with a job (AI type not 0; ambient humans are
  ped_sync's) on foot within 100 m (|dz| < 30 m; the 96 nearest), no puppet, spawned copy, traffic copy crew or
  pedestrian replica, with the host NPC each is paired with (npc_sync's usual pairing above); and every held one,
  wherever it is. None in the first 10 s after his world loaded or during a cutscene (a gap of more than 1 s in the
  rounds restarts the wait). NPCs in a vehicle are not held (the drivers of his own traffic are guest_world's; mission
  cars driven by NPCs are docs/host-authority.md WP-F).
- **Held:** one that stays unpaired (alive) for **5 s** is held: hidden where it is (`ShowModel(false)`, physics state
  2, demigod), its AI interrupted as a paired replica's (`InterruptBattle`, `InterruptCivilWander`, `InterruptPatrol`,
  `InterruptAvoidingCar`, `SetReactionOnSound(false)`; again every 2 s, and the model hidden again). Its shots on the
  guest's player are given back like a replica's (`N.IsPicture`). Its mission scripts keep running: it is never
  despawned.
- **Paired later:** when a host NPC pairs with it (the host's game spawned it: the same spot within 1.5 m, or the same
  tag / AI type within 40 m after 2 s, as any pairing), it is shown at once (model, physics state 0, demigod off) and
  is that NPC's replica, driven like any paired NPC; paired with a dead host NPC it is shown and dies with it. A
  pairing that is released again (5 s without host states) starts its 5 s again.
- **Dead:** an NPC that died before it was held is never held; a held one that dies (its own scripts) stays hidden.
- **Released:** when the session ends (or the game becomes the host), every held NPC is shown with its own AI again
  (`ResumeBattle`, `SetReactionOnSound(true)`); with a new world nothing is remembered.
- Log: `npc: our NPC <id> has no host NPC (unpaired for 5 s): held hidden ...`, `our held NPC <id> is host NPC <key>
  now: shown, its replica`, `... released (session_end)`; trace `NHD` (each decision), `NHL` (2 Hz per held NPC),
  `NGS held holds unholds`; `tools/sync_compare.py` "NPCs only <guest>'s game had" and `npc.guest_sent` (target 0).
- Replay of session A of the 2026-10-20 two-PC test (tests/data/npc_guest_1020.h, `tools/extract_npc_guest.py`): the
  0.16.0 rule made one guest cop (AI type 4) his own three times and sent 1350 states of it (GuestNpcStates2 in 27 of
  53 NetMonitor reports, ~1340 messages, ~60 KB); his three other NPCs with a job were paired at the same spot. Under
  this rule the cop is held at the same moment (120.4 s), stays held for 148 s (no host NPC ever paired with it) and
  is shown at the session's end; the three paired ones are never held; nothing is sent.

### Guest-owned NPCs (0.15.0-0.16.0; retired by host authority)

0.15.0 / 0.16.0 made a guest the authority for NPCs only his game had: sent to the host, shown there as mirrors
(spawned copies), relayed to the other guests under relay keys, claimed back by the host when its game spawned them
(`npc_owner.h`, deleted). Retired with host authority (the "Held NPCs" above instead); the ids stay reserved, never
reused, and are neither sent nor read:

| App id | Name | Was |
|---|---|---|
| 0x0420 | GuestNpcStates | (retired in 0.16.0) the full NpcStates body, guest -> host |
| 0x0421 | GuestNpcEnd | guest -> host: one of his own NPCs died / was handed back / went / a claim refused |
| 0x0422 | HostNpcHit | host -> that guest: the host (or another guest, relayed) hit his own NPC |
| 0x0423 | NpcClaim | host -> that guest: the host's game has that NPC now |
| 0x0424 | GuestNpcStates2 | guest -> host: his own NPCs as NpcStates2 |
| 0x0425 | GuestNpcInfo | guest -> host: as NpcInfo |
| 0x0426 | GuestNpcLeave | guest -> host: as NpcLeave |

### Traffic (0.15.0; by region in 0.16.0; the host's only again (host authority, Unreleased); not yet checked in game)

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0220 | TrafficSpawn | host -> one guest (`SendTo`) | R | u8 n (at most 16), then n x: u16 net, u8 kind (0 traffic, 1 police), str model, i16 color1 color2 (`GetColor(1)` / `GetColor(2)`, -1 unknown), str plate ("" unknown), f32 x y z, u32 quat (smallest three), i16 vx vy vz (1/100 m/s), u8 flags |
| 0x0221 | TrafficDespawn | host -> one guest (`SendTo`) | R | u8 n, then n x u16 net |
| 0x0222 | TrafficStates | host -> one guest (`SendTo`) | U, up to 20 Hz per car | u32 ms (the host's clock), f32 origin x y z, u8 n (at most 40: 897 bytes), then n x 22 bytes: u16 net, i16 x y z (1/32 m from the origin, +-1023 m), i16 vx vy vz (1/100 m/s), u32 quat, i8 steer (x127), u8 power, u8 brake (x255), u8 flags |
| 0x0223 | TrafficBump | a guest -> host (`SendToHost`) | R | u16 net, f32 error (m), f32 copySpeed, f32 hostSpeed (his copy of a host car was knocked off) |
| 0x0224-0x0227 | TrafficSpawn2 / TrafficDespawn2 / TrafficStates2 / TrafficBump2 | **retired** (Unreleased) | - | 0.16.0: a guest's own traffic (u8 authority, then the 0x0220-0x0223 body), guest -> guest through core's relay. Neither sent nor read any more (no handler, no name, no lane or priority); the ids stay reserved and are never reused |

**The host's game owns all ambient traffic and police cars** (docs/host-authority.md; Unreleased). A guest's game reads
0x0220-0x0222 only from the host, the host reads 0x0223 only from a guest; everything else (a guest's TrafficSpawn /
States to the host, another guest's cars or bumps, the host's own messages back) is ignored and counted
(`game/traffic_model.h` `Route()`, logged `traffic: message 0x022x from player N ignored`, `TRH ignored`). Only the
host's game runs an authority (`RunsAuthority`: `AuthorityTraffic` exists on the host only); a guest's game only shows
copies (`ShowsCopies`) and sends nothing but TrafficBump. The latency trace counts 0x0222 as "traffic", 0x0220 /
0x0221 / 0x0223 as "traffic-event"; 0x0222 is sent with the Bulk priority. The version stays 0.16.x on the branch
(docs/host-authority.md D7): a 0.16.0 guest still connects, his own cars (0x0224-0x0227) go unread and he still shows
the host's; a 0.16.0 host gets no traffic from a new guest (none is sent). Mixed versions are not supported.

- **The host is the authority for all traffic.** His Lua glue (`traffic_sync.lua`) scans the car table once a second
  and reports, 20 times a second while a guest is in the world, his traffic cars within 380 m of any guest (his
  puppet, or his car): transform, velocity, the driver's controls (`mc_car_controls`, flag 8), reversing (velocity
  against the car's heading, flag 16), handbrake (flag 1), siren (flag 2, police only: the car's own siren / beacon
  bits via `mc_car_signals`; when they cannot be read, on while the car goes faster than 14 m/s or is near a hunted
  player, and 8 s after; see "Police"). The dll (`game/traffic_model.h` AuthorityTraffic) gives each car a net id (u16,
  kept while the car lives and never reused while a guest has it) and keeps core's InterestManager with **every guest**
  as a peer (Kind::Vehicle: enter 260 m / leave 320 m from him, every tick up to 60 m, every 0.4 s at 260 m, x4 for
  cars below 0.3 m/s, at most every 2 s; 40 KB/s per guest). Each guest gets the **nearest 24** relevant cars (a car
  beyond 150 m gives way to one within 90 m): a spawn when one is admitted, a despawn when it leaves his range, is
  pushed out or is no longer traffic, and its states when the interest manager says it is due and its quantised state
  changed (an unchanged one is sent again after 1.5 s). About 22 bytes x 20 Hz per car near a guest: ~10-15 KB/s for a
  busy street.
- **A guest's game has no traffic of its own to share.** It does not scan or report its cars (no TC lines, no
  AuthorityTraffic), shows no other guest's cars (there are none) and keeps no region: its own ambient traffic and
  police spawning are off for the whole session and its leftover cars hidden by `game/guest_world` (docs/host-authority.md
  WP-A; traffic_sync's 0.16 `T.LocalTraffic` / `T.ClearLocal` / `T.hidden` are gone). The host's game shows no copies.
- **What is traffic (on the host):** a car whose driver the traffic or police generators made (its name is
  `behavior_object_prime-NN-` with an empty suffix, docs/game-api.md 15), not a player car (player_sync's, Phase 3),
  not a puppet factory's or a copy spawner's (ped_sync's / npc_sync's: every car fixed 40 m under its player,
  `IsCarPosFixed`), not the one he sits in; police when the driver's AI type is 4 or the model name says so. Parked cars
  (no driver) and mission cars (drivers named after a mission spawner) stay local to each game as before. A traffic car
  whose driver died or got out stays shared; once it has stood (under 2 m/s, within 40 m of the host) for 20 s without
  a driver it counts as parked.
- **Driverless (flag 4, 0.16.0):** the host's Lua reads seat 0 of each car every second (and every round while it
  read no driver): flag 4 in its TC line is "no living generator driver read now". That read flickers (0.15.0 two-PC
  test: 179 cars flagged, 118 of them still moving, 84 still at speed 4 s later, mostly 50-100 m from the host's
  player). The dll (`game/traffic_model.h` DriverWatch) sends flag 4 only when no driver was read for **1 s while the
  car was slower than 2 m/s** all that time; it clears it when a driver is read again or the car speeds up past
  4 m/s (someone drives it). Why the read fails on moving cars (2026-10-20 two-PC test): 61 of 62 misses were
  `seat 0 empty` 45-169 m from the authority's player and the driver came back within 44-51 m - the game streams
  traffic drivers out beyond ~50 m and drives their cars on. A car farther than **40 m** from the host's player without
  a driver read is a driven car (TC flag 64, never on the wire; DriverWatch `Far`), it is not sent as driverless and,
  standing, is no parked car either; a car declared driverless near him stays so when he goes away.
- **A guest's game shows the host's cars near him.** His `game/traffic_model.h` ReceiverTraffic keeps the host's cars
  by their net id (the key everywhere: the Lua's copies, car damage, car look, explosions; 0.16's `authority << 16 |
  net` is gone) and gives the nearest **20** within 240 m a copy (dropped beyond 290 m; a far copy gives way to a car
  at less than half its distance): `ObtainCarWithCrew(model, {profile})` (a civilian or police spawn profile,
  `characters/traffic/traffic_base_ambient_combined` / `characters/ai/police_scotch/ai_police_triggerman` and
  fallbacks, docs/game-api.md 19; one that fails twice before it ever worked is dropped; the puppet factory's Tommy
  last; `ObtainSpecificCar` when that fails), `SetDespawnImmunity`, `DisableExplosion`, the crew locked
  (`LockControls(true)`, no reaction to sounds), the host's colours and plate. Each copy is driven like a remote
  player's car (Phase 3 "Driving the puppet and the car copy"): the host's states go through one core
  `replica::Stream` (its playout clock: the adaptive delay over the host's jitter and state interval, 120 ms until it
  has enough states; Hermite between the states, extrapolated at most 0.5 s), moved forward by velocity x the playout
  delay (at most 9 m), then core's VehicleController (the driver's controls, the wanted velocity, pinned when its
  physics does not follow) and core's Corrector with the traffic preset as tuned on a recorded car path
  (`TrafficCorrection()`: nothing within 0.5 m, beyond it 8 /s of the excess but at most 70 % of the car's speed, a
  snap beyond 8 m, after 3 s stuck 2 m off or when the target jumps 20 m). Copies farther than 90 m are driven every
  third tick. A copy goes when its car is despawned (1 s later, so player sync can take it first), after 5 s without a
  state, when we drive off, when the session ends, and with the world; near ones (80 m) with `DespawnEntity` (when
  unseen), others at once. Siren and beacons follow flag 2. The copy's driver gets out only when the car is
  driverless (flag 4, confirmed as above) **and** both the host's car and the copy stand (under 2 m/s; the dll adds
  flag 32 to the copy's drive command, `CrewGetsOut`); a moving copy never loses its crew, and once out it stays out
  until flag 4 goes again, whatever the copy's speed does (2026-10-20: 19 of 40 get-outs were back in within 2 s
  before this hold). The speed a copy is given (`SetSpeed`, one number along its own heading) is the host car's speed
  plus the error along the copy's heading x 1.5 (that part at most 4 m/s), never below 0 (`CopySpeed`; flag 16 in the
  drive command: backwards, when the car reverses): in 0.16.0 it was the length of the controller's wanted velocity
  vector, so a copy that had rolled past a car stopping at a light was driven on, away from it, at up to 4 m/s
  (2026-10-20 session A: true error p95 2.14 m in game, 0.75 m in the replay of the same session with CopySpeed,
  tests/traffic_test.cpp). Twice a second the crew is checked: a member out of his seat (alive, the seat free, our
  player not within 8 m) is put back with a non-animated `GetOnVehicle` (at most once a second, 8 times in a row).
  npc_sync kills the copy's driver when the host's driver died: traffic drivers are NPCs too and pair by position.
- **The host pauses:** every copy is held where it stands (see "A paused game"), knocks are not reported for 1 s
  after his resume.
- **Taking a traffic car:** when a guest takes a copy of a host car, his game makes it his car (player sync), the
  host's game adopts its own traffic car there as the guest's car and its traffic driver gets out; when the host takes
  one of his traffic cars, the guest's player sync finds the copy where the car is (same model, within 3 m) and adopts
  it; the traffic glue then hands it over (its crew gets out, `TH`) and no longer drives or removes it.
- **Hits and rams:** a copy is a real car on the guest's game: bullets and cars hit it there. The host's car is not
  moved by a guest's game; the guest's own car is a copy on the host's game (Phase 3) and hits the car there by itself.
  A copy knocked more than 3 m off its target (that was on it a moment before) is blended back (snapped beyond 8 m) and
  reported to the host (TrafficBump; at most every 2 s per car), who logs it. Shots at a copy's driver go through
  npc_sync (NpcHit) like any paired NPC. Damage to the cars themselves (dents, parts, tyres, fire, the explosion) is
  shared since the car damage sync (the host's game holds it, a copy reports its own, "Car damage").
- **Traces** (docs/sync-trace.md): the host writes TRS (truth), TRH, TRP and TRO (driver reads); a guest TRG, TRC,
  TRZ and TRO (crews). 0.16's TRA / TRL / TRR and the `mode` / `auth` / `a` fields are gone.

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
  only in another player's region: near the host, or near a lower-id guest far from the host).
- **Police copies** (traffic): the host reads the real siren / beacon of its police cars (`mc_car_signals`) for the
  copies' flag 2 (TrafficStates); the speed guess is only the fallback, now also "near a hunted player".
- Additive: a peer without these messages ignores them. Logs `police: ...`; trace records POL / POE / POR / POT /
  POC (docs/sync-trace.md).

### Explosions (0.15.0; messages seen in game, the pictures not yet)

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0240 | Explosion | the game where it happened -> everyone (`Broadcast`; a guest's is relayed by the host) | R | u32 seq (per sender; a new session starts elsewhere: `(GetTickCount() & 0xFFFF) << 16`), u8 kind (0 unknown, 1 grenade, 2 molotov, 3 dynamite `exp_satchel`, 4 crash object, 5 car blast), u8 flags (1 fire: the picture burns too; 2 one of the sender's NPCs threw it), f32 x y z, str weapon (`exp_grenade`, ...; "" unknown; at most 32 of `[A-Za-z0-9_-.:/]`), u32 npc (flag 2: the NPC's key on the sender's game: on the host its entity id, as in NpcInfo; on a guest his own NPC's key, as in GuestNpcInfo), str object (kind 4: the crash object's scene name, at most 64; "" none), **[u32 throwSeq]** (grenade arc: the Throw 0x0242 whose explosion this is, 0 none; older senders end before it, older readers ignore it; 1-3 trailing bytes = malformed) |
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
  - on the host one of its NPCs threw it (0.16.0 also: on a guest one of his own NPCs; retired with the guest-owned
    NPCs, a held NPC throws nothing): flag 2 with its key. Receivers show those only where that NPC is shown by a spawned copy (which runs no fight of its own); a game
    that runs its own copy of the NPC (a paired mission NPC) has that NPC's own grenades already;
  - a car: the game whose player drives it (or, parked, whose own explosion blew it up; on the host any shared
    traffic car), seen as CAR_HAVE_PROBLEM with `GetCarProblem() == EXPLODE`.
  Nothing else is sent: explosions of a mission script (dynamite in a mission, scripted car bombs) happen on every
  game by its own mission; an explosion of someone else's (an enemy of a guest's own fight) is that game's.
- **Who is hurt, once.** The sender's own DAMAGE events decide, through the usual paths: a puppet in the blast or the
  fire sends **Hit** (Phase 6; fire summed for 0.5 s into one Hit, like a ram) to its player; on a guest a copy of a
  host NPC sends **NpcHit** (fire summed per NPC for 0.5 s; a copy that died of it makes the hit lethal); the host's
  own NPCs simply take it (0.16.0: a guest's own NPC's copy on the host sent HostNpcHit, retired; a held NPC is a
  demigod). An NPC's explosion on the host hurting a guest's puppet goes to him like any NPC hit (Hit, source 1).
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

### Pedestrians (0.16.0; host only since host authority, docs/host-authority.md WP-C; not yet checked in game)

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0280 | PedStates | host -> one guest (`SendTo`) | U, 10 Hz near, 3 Hz at 80 m | u32 authMs (the host's clock at the sample), f32 origin x y z, u8 n (at most 50: 817 bytes), then n x 16 bytes: u16 net, i16 x y z (1/32 m from the origin, +-1023 m), i8 hx hy (heading x127), i16 vx vy (1/100 m/s), u8 flags (1 dead, 2 panic, 4 crouch, 8 running), u8 gait (0 idle, 1 walk, 2 run, 3 sprint) |
| 0x0281 | PedSpawn | host -> one guest | R | u8 n (at most 24), then n x 22 bytes: u16 net, u8 class (0: unknown, nothing readable yet), u32 look (the host's `GetCivilArchetype` hashed to 32 bits, 0 unknown; logged, not applied), f32 x y z, i8 hx hy, u8 flags |
| 0x0282 | PedGone | host -> one guest | R | u8 n, then n x u16 net |
| 0x0283 | PedDeath | host -> one guest | R | u16 net, f32 x y z (where it died) |
| 0x0284 | PedKill | guest -> the host | R | u16 net: the guest's copy of it died by his hand |

0x0285-0x0287 are reserved. The bodies are 0.16.0's; what changed is who sends: **the host's game owns every ambient
pedestrian** and is the only sender. A guest never sends PedStates / PedSpawn / PedGone / PedDeath (his own
pedestrian spawning is switched off by guest_world, WP-A) and takes them from the host only (anything from another
player is ignored, so a 0.16 guest's region pedestrians do not show); only the host takes PedKill. There is no
region authority, no guest -> guest relay, no adoption of a guest's own humans and no hiding of them by ped_sync any
more. A receiver still keys a pedestrian by (sender, net) (`NetRef {authority, Pedestrian, net}`).

- **What is an ambient pedestrian** (the contract with npc_sync and guest_world): a human of this game that is not a
  player, puppet or any copy (MafiaCoop's own), not in a vehicle, `GetAIType()` 0, and not named after a mission
  spawner (`behavior_object_prime-NN-<suffix>` with a non-empty suffix, docs/game-api.md 15).
  `MafiaCoop.IsAmbientHuman(h)` says so; `MafiaCoop.PedOwned` (entity id -> true) lists every local human ped_sync
  controls - every pedestrian copy of the pool, shown or hidden, and the copy spawner's driver - kept up to date at
  every change. npc_sync never pairs, sends or steers one; guest_world never hides one (and leaves
  `MafiaCoop.Peds.fac.car`, the copy spawner's car, alone).
- **The host** (`game/ped_model.h` PedAuthority): ped_sync.lua scans the human table every 0.25 s and reports the
  ambient humans within 100 m of any player 20 times a second (position, heading, dead / panic / crouch read; the
  velocity and the gait come from the measured positions, smoothed, gait with hysteresis 0.5 / 2.4 / 4.8 m/s up,
  0.3 / 2.0 / 4.3 down). The dll gives each pedestrian it shares a net id (u16, not reused while a guest has it) and
  keeps core's InterestManager per guest (Kind::Npc: enter **80 m** / leave **90 m** from him or his car; every
  0.1 s up to 15 m, every 0.33 s at 80 m; 16 KB/s budget; the nearest **48** at most): PedSpawn when one comes into
  his range, PedGone when it leaves (or is gone), PedDeath once when it dies, its state when due and changed (an
  unchanged one again after 1 s). States carry the sample time on the host's own clock. A human faster than 8 m/s
  for two rounds rides something (a tram's passengers): not shared while it does (PedGone), shared again with a new
  net id after 3 s slower. A guest's game runs no sender at all.
- **A guest** (PedReceiver): one `replica::Stream` for the host (its playout clock, Hermite interpolation, the "now"
  lead). **Every** living pedestrian of the host (all of them are within 90 m of him) gets a copy from a pool of at
  most **40**, nearest first, 0.2 s after it came: a **free pool copy** if there is one (put at its spot with
  `SetPos` / `SetDir`, `Activate`, `ShowModel(true)`; no profile load, no CreateObject), else a **new copy** from our
  own spawner (the puppet factory recipe: a crewed car fixed 40 m under us, a `C_RuntimeSpawnerComponent` on its
  driver; up to 40 requests outstanding). When every copy is in use, a pedestrian without one takes the copy of the
  farthest one if that is beyond 40 m, more than 15 m farther and out of view (a handover). A copy whose pedestrian
  left our interest or went is **hidden and kept** (`ShowModel(false)`, `Deactivate`; once nobody looks, at the
  latest after 3 s); a dead one (a body) is removed (`ReleaseObjectRaw`), and so is one that lags again right after a
  lag snap. A copy that is more than 1 m off at its first report after it came is put at its target at once.
  UNVERIFIED in game that a copy hidden like that comes back with `Activate` + `ShowModel(true)`.
- **Looks** (docs/host-authority.md D5): a new copy gets one of six civilian spawn profiles (all in
  spawnprofiles.sds, docs/game-api.md 19: `characters/traffic/traffic_base_ambient_combined`,
  `characters/game/game_attractivemen`, `characters/game/game_attractivegirls`, `characters/game/game_blackmen`,
  `characters/game/game_blackwomen`, `characters/traffic/traffic_workers_male`; `SetSpawnProfile` gets the decimal
  `mc_fnv` hash) picked by its net id (the live looks, `net % n`). The spawner loads **one profile at a time** and
  makes every queued request of the loaded look before it loads another (then the look most requests wait for); a
  request that waited 2 s is made with the look loaded now (a crowd that comes at once is not kept waiting for six
  loads). A look that does not load in 8 s fails its requests (`no_profile`); it rests for 60 s, and failing again
  before it ever made a copy it leaves the rotation (`peds: copy profile ... not used again`; the last look always
  stays). A pool copy keeps the look it was made with. UNVERIFIED in game that the six load through a runtime
  spawner and look like passers-by; the host's pedestrian's own look is not readable.
- **The spawner's health** (feat/sync-peds): CreateObject failing twice in a row, or 3+ copies gone within 0.5 s, with
  no copy of it left (free ones included: its despawn would take them along), asks for a new spawner (`FR`, at most
  every 5 s); the Lua's own resets (spawner lost, deactivated, idle 30 s without any copy, an error) tell the dll
  (`PFR`) so it does not ask again on top. The copies are walked like puppets: core's LocomotionController with the
  host's gait as the hint and the correction rules of `Corrector::ForNpc` (0.5 m dead zone, a nudge capped at 35 % of
  its speed or 0.15 m/s standing, a snap beyond 5 m or 3 m up, stuck 0.8 m for 3 s), applied with `MoveVec`,
  `SetMovementSpeedMult`, `SetPos` / `SetDir`; a copy more than 1.5 m behind a moving target for 2 s is snapped, and
  removed (another one comes) when it lags like that again within 6 s.
- **Deaths and panic** follow the host: a PedDeath (or the dead flag) kills the copy (`DoDamage`); the crouch flag
  makes a standing copy `PanicCrouch` (UNVERIFIED read and picture); running is in the gait. A copy that dies on a
  guest by his hand sends PedKill to the host, whose pedestrian then dies (and that death reaches every guest).
- **Gone / stale:** a pedestrian's copy goes to the pool on PedGone or after 3 s without a state. The session's end
  or a new world: every copy goes.
- **Bandwidth** (per guest): 16 bytes per pedestrian per state + 17 bytes per packet. 48 pedestrians walking within
  80 m: ~4-6 KB/s (tests/ped_test.cpp: 5.6 KB/s for 48 walkers spread over 3-77 m), standing ones once a second;
  the replays of the 2026-10-20 sessions: 0.32 / 0.48 KB/s (A / B; 0.28 / 0.41 with 0.16's 50 m), plus PedSpawn 22
  bytes per pedestrian when it comes into range. Lane 5, PedStates Bulk (thinned first), PedDeath / PedKill
  Critical. A guest uploads only PedKill.
- **Measured:** trace `PDT` (the host's truth), `PDR` (each copy at 10 Hz, keyed by the host's sample time), `PDC`
  (coverage and the pool, 1 Hz) and `PDS` / `PDP` / `PDE` / `PDL` (docs/sync-trace.md); `tools/sync_compare.py`
  "Pedestrians" computes `ped.p95`, `ped.coverage` and `ped.copy_fail_per_min`.

## Reconnect (no messages of its own)

Core does the transport and session part (external/core/docs/protocol.md "Reconnect": slot held 60 s on the host,
the guest rejoins with backoff for 60 s and his resume token, PeerDropped to the others, `PlayerInfo::joinSeq`). What
the game layer does with it:

| Who | When | What |
|---|---|---|
| host, other guests | a player is dropped (`GameModule::OnPlayerDropped`) | his puppet is removed (`PlayerSync::OnPlayerLeft`, as when he leaves); banner "<NAME> LOST THE CONNECTION - WAITING FOR THE PLAYER TO RECONNECT... 45 S" (`Session::DroppedPlayers`; several at once: "SAM AND PAULIE LOST THE CONNECTION - WAITING FOR THEM ..." with the longest wait) |
| lost guest | `OnConnectionLost` | the game keeps running (no menu, no load). The session is inactive: PlayerSync removes the puppets and stops sending (the trace stays open: `RCN ev=sync-paused`), MissionSync keeps the host's point and our `_followPoint` (nothing reloads by itself) and forgets the host's cutscene, NpcSync's Lua drops the copies it spawned (it does that whenever the session is inactive); banner "CONNECTION LOST - RECONNECTING... 12 S" |
| host | the player is back in `Players()` with a new `joinSeq` | MissionSync sends him MissionState, his HostSave (the newest checkpoint save) and the Cutscene, as to a newcomer; WorldEnvSync sends him WorldEnv at once; PlayerSync sends our PlayerModel; NpcInfo / NpcStates2 reach him in the next round (he is a new peer of the NPC publisher) |
| lost guest | `OnReconnected(sameSlot)` | PlayerSync is active again: every player is new (`_seenPlayers` cleared), so we send our PlayerModel and the puppets are spawned from the next states. MissionSync: the host's MissionState and HostSave arrive; at the same checkpoint nothing happens (the save is the same `#seq` or our point equals his), at another one the usual follow (quit to the menu and load his point after `follow_grace`). |

| lost guest | the session is inactive / back | DoorSync: nothing is sent or applied meanwhile; when the session is active again the glue reads its doors again and asks the host for a DoorSnapshot (reason reconnect) |
| everyone | a player is dropped | CarDamageSync forgets him as the holder of cars and as someone who asked for a state: his cars' copies ask whoever drives them next; when he is back his copies ask for every car's state again |
| host | a player is dropped | NpcSync forgets his interest counters (no mirrors since host authority), TrafficSync forgets him as a peer (`HostTraffic::RemovePeer`: nothing is sent to him; when back he is a new peer and gets every near car spawned again) |
| lost guest | the session is inactive | TrafficSync's Lua drops the host-traffic copies; the C++ side forgets the host's cars (bringing his own traffic back is guest_world's, docs/host-authority.md). Our held NPCs are shown with their own AI again (the session is inactive) and held again 5 s after the rejoin when still unpaired. MissionSync gives up its fail role meanwhile (`FailRole::None`): a mission failing in the gap takes the game's own way (FreeRoamRetry, then the usual follow after the rejoin) instead of a report nobody would send |

- A lost guest that is back: his WorldEnv follower starts with a first sync again (one SetTime when off), a catch-up
  that ran when the connection was lost has had the host's flow speed put back; JoinSync times his world anew
  ("Join and reconnect" above), and his leash is suspended until that is settled and 20 s more; the host sends him
  LeashConfig again (a newcomer by `(id, joinSeq)`; "The leash" above).
- Newcomer detection keys by `(id, joinSeq)` (PlayerSync `_seenPlayers`, MissionSync `_seen`, WorldEnvSync
  `HostSender`), so a drop and a resume
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
  once, both back into their own slots, no newcomer meanwhile). (No guest's NPCs go to the others since host
  authority: nothing of his to take along.)

## Several guests (3-4 players; 0.15 multi-guest, not yet checked in game)

Sessions take up to 4 players (host + 3 guests, core `kMaxPlayers`). What changes with more than one guest, and the
2-player assumptions that were fixed (each in its section above):

| Area | With several guests |
|---|---|
| PlayerState, PlayerModel, CarParked | every peer `Broadcast`s, the host relays to the other guests: each game drives one puppet per other player (`Remote` per id) |
| Hit (player -> player) | `SendTo(victim)`, guest -> guest through the host relay; the victim replays from the attacker's puppet; friendly fire as before |
| NPC hits on puppets | host NPCs -> any guest's puppet (as before); 0.15-0.16: a guest's own NPC -> any puppet on his game (retired: his NPCs without a host NPC are held) |
| NpcStates2 | per guest by interest around his puppet (0.16.0; before: NpcStates broadcast, at most 48 in turns from each player's nearest) |
| Guest-owned NPCs | retired (host authority): each guest holds his NPCs that no host NPC is; nothing is relayed (0.15-0.16: relay keys, hits forwarded to the owner, one owner per NPC) |
| Seats | ride along skips seats the other players' states claim; the same passenger seat twice: the lower id keeps it, the other slides; the wheel of one car claimed by two other players: the current one keeps it |
| Traffic | the host's game is the only traffic authority, with an interest set per guest (24 cars, 40 KB/s budget); a car near two guests has one net id; no guest's traffic, no guest -> guest traffic (0x0224-0x0227 retired, Unreleased; 0.16.0 had a region per guest); a guest who drops is a new peer when back and gets every near car spawned again |
| Failed missions | a guest's failure is his own (MissionFail -> FailVerdict to him only, the others play on); the host's failure is everyone's (HostFailed broadcast); tests/fail_flow_test.cpp "3 guests" |
| Story, cutscenes, saves | per guest by `(id, joinSeq)`: a newcomer or a reconnected guest gets state, save and cutscene; the others nothing extra |
| Lobby | ready per guest, START needs all of them; a fifth player is refused (core tests/net_lobby.cpp "4 players") |
| Name tags, map markers | one per other player (tests/map_markers_test.cpp "3 others"); two players in one car have their markers at the car |
| Reconnect banner | every dropped player's name, the longest wait |

Bandwidth with 4 players (estimates from the per-message sizes above, host upstream): PlayerState ~60-140 B x 60 Hz
per player, the host sends his own and relays two others' to each guest: ~3 x 3 x 6 KB/s = ~55 KB/s in cars; NpcStates
~45 B x 48 x 10 Hz = ~22 KB/s per guest (broadcast; since 0.16.0: NpcStates2 by interest, <= 8 KB/s per guest); traffic up to 40 KB/s per guest (its budget). Worst case ~3 x (18 + 22 + 22 + 40) = ~300
KB/s up from the host, ~100 KB/s down to each guest; the relay limit per guest (`net_budget_kbps`, 1024 KB/s) is far
above what one guest sends. Per-guest totals: core's traffic log (`net: <name> (<id>): ... out .. KB/s, in ..`
every `net_log_interval` s, per peer), the overlay's traffic line on the host ("guest N: cars of relevant (KB)"),
trace `TRP` per guest.

Known limits (documented, not fixed):
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
| 5 | pedestrians 0x0280-0x0287 | many small states; their spawn / gone / death need no order with the rest |

A lane message still comes after the lane-0 messages sent before it; lane 0 does not wait for the lanes, so e.g. an
NPC death sent after a mission change can arrive before it (UNVERIFIED that no handler minds; none checks for it).

Priorities against the per-peer send budget (`net_budget_kbps`, lowered automatically on a lossy / backed-up link):
Critical (never thinned or deferred): PlayerState, CarParked, PlayerModel, Hit, NpcHit, NpcDeath, explosions and throws (0x0240-0x024F), DoorEvent / DoorApply, ItemTake / ItemGone / Collectible, police
(0x0260-0x0262), MissionFail / FailVerdict, WorldEnv. Bulk (first to go): TrafficStates (thinned), ItemStates (thinned),
DoorSnapshot and HostSave (deferred, in order). Normal: NpcStates2, NpcInfo, NpcLeave (0.16.0; their rates and the
per-guest budget are the NPC publisher's), LeashConfig / LeashEvent (lane 1, a few bytes now and then), car damage,
the rest. (The retired 0x0420-0x0426 have no priority any more.)

Per system: the delay from the sender's game to the receiver applying it (wire + both games' frames), measured
parts from core's `net_latency` (loopback) and our tests, the rest estimated (E) for 60 fps games and a one-way
network delay N. "Before" is 0.15 main (core 0.11).

| System | Rate / trigger | Before | Now | Bandwidth (per receiving peer) |
|---|---|---|---|---|
| PlayerState (on foot, car) | 60 Hz | sample -> send 0..1 frame (E) + send queued until the next Poll 16.7 ms (measured) + N + 0..1 frame + fixed 50 ms playout (led forward by 50 ms) | sent in the frame it is first seen, flushed at once (0.06 ms measured) + N + 0..1 frame + adaptive playout 24.5 ms at a quiet 60 Hz (measured, net_latency; grows with jitter), stamped with the sample time | ~4 KB/s per player on foot (~60 B x 60), ~6 KB/s in a car (E; +4 B stamp per state = +0.24 KB/s) |
| Actions (punch, throw, jump ...) | ride on PlayerState | as PlayerState | as PlayerState | none extra |
| Traffic cars | 20 Hz host rounds | 16.7 ms queue + N + fixed 120 ms playout | flushed + N + adaptive ~57 ms (traffic_test, quiet link; 140 ms under 80 ms jitter) | ~25 B per car per state: 20 cars x 20 Hz = ~10 KB/s typical, ~20 KB/s worst (cap 20 copies, E); Bulk: thinned first |
| NPC states | 10 Hz -> 15 Hz; since 0.16.0: per NPC 15 Hz near to 5 Hz at 120 m (NpcStates2) | 0..100 ms round + 16.7 + N | 0..66 ms + N + adaptive playout (one replica::Stream per sender; a slow NPC further back by its interval), led to now along its velocity and acceleration | ~6 KB/s per guest for 48 NPCs in range (unit test; was ~21 measured) |
| NPC deaths / hits / claims | event | 16.7 + N (+ RTO 50 ms+ on a loss) | N (+ ~1 RTT on a loss) | < 0.5 KB/s (E) |
| Doors | door message -> next tick; poll 5 Hz -> 10 Hz | 0..200 ms poll + 16.7 + N | next tick on a door message, else 0..100 ms + N | < 0.2 KB/s events; snapshots on request ~2-6 KB each (E), Bulk |
| Items | scan 2 Hz -> 4 Hz | 0..500 ms + 16.7 + N | 0..250 ms + N | ItemStates ~1-3 KB/s near items (E), Bulk |
| Police | read 5 Hz -> 10 Hz | 0..200 ms + 16.7 + N | 0..100 ms + N | < 0.3 KB/s (E) |
| Car damage | rounds 4 Hz -> ~6.7 Hz | 0..250 ms + 16.7 + N | 0..150 ms + N | only changes: < 1 KB/s typical, a few KB in a crash (E) |
| Pedestrian states (0.16.0; host only) | 20 Hz host rounds, 10 Hz near / 3 Hz at 80 m per pedestrian | - | flushed + N + adaptive playout (ped_test: ~125 ms quiet, ~160 ms under 60 ms jitter at 10 Hz) | 16 B per pedestrian per state: ~4-6 KB/s for 48 walking near one guest (ped_test 5.6), 16 KB/s budget; Bulk |
| Explosions, throws | event (detector) | 16.7 + N | N | < 0.2 KB/s (E) |
| Mission state / fail | on change | 16.7 + N (HostSave held back everything after it) | N; HostSave on its own lane | HostSave up to 256 KB once per checkpoint, Bulk |
| WorldEnv (time, weather) | every 4 s, on change | a newcomer waited up to 4 s; corrected only beyond 3 game minutes, by a jump | at once to a newcomer, on a change, every 2 s; the sample's age compensated, small errors closed by the flow speed | ~40 B every 2 s (< 0.03 KB/s) |

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
