# MafiaCoop app messages

The messages the Mafia DE game layer sends over a JustAddMP core session: player sync (phase 3) and world state
(phase 4). Sessions, the handshake, the password, relay rules (`SendToHost` / `Broadcast` / `SendTo`), the UDP and
Steam transports and lobbies are core's: `external/core/docs/protocol.md`. All integers are little-endian.
`varint` = unsigned LEB128, `str` = varint byte length + UTF-8 bytes.

Version rules: any incompatible change to these messages needs a bump of `modVersion` in `src/game/game_info.cpp`
(the session build number is derived from it, so mismatched builds are rejected in the handshake).

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
| 0x0100 | PlayerState | every peer -> everyone (`Broadcast`; the host relays guest states) | U, 20 Hz | see below |
| 0x0101 | CarParked | last driver -> everyone (`Broadcast`) | R | u8 keyOwner, u32 vehId, str model, f32 x, y, z, f32 qx, qy, qz, qw |
| 0x0102-0x01FF | reserved (phase 3 events) | | | |

PlayerState body (54 bytes on foot + weapon name, ~130 bytes in a car):

```
u8  version (3)
u32 senderTimeMs          sender's monotonic clock (QueryPerformanceCounter), for interpolation only
f32 x, y, z               player position (world metres)
f32 hx, hy                heading, unit vector in the ground plane
f32 vx, vy, vz            velocity (m/s; on foot differentiated from positions, in a car the car's GetSpeed())
u8  mode                  0 idle, 1 walk, 2 run, 3 sprint (from the speed; sprint also when IsSprinting())
u8  flags                 bit0 in vehicle, bit1 crouch, bit2 sprint, bit3 aim (aim input held with a gun in hand),
                          bit4 reload (IsReloading(), or for 1 s after the clip was refilled)
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
```

Receivers keep up to 64 snapshots per player and render them **100 ms in the past** on the sender's clock:
`renderTime = now - offset - 0.1`, where `offset = localReceiveTime - senderTime` is tracked as a running minimum
(slowly relaxed upward, 1 % per message, to follow drift). Between two snapshots everything is lerped (heading
renormalised, car quaternion nlerp with sign fix, discrete fields from the nearer one); past the newest snapshot the
position is extrapolated with the velocity for at most 250 ms, then held. Old/duplicate snapshots (sender time not
newer) are dropped, so loss and reordering need no reliability.

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
3. **Get out / park.** A's PlayerState stops carrying K (on foot). The others stop moving K: their entity stays where
   it was (a parked car with normal physics, unlocked) and A's puppet climbs out. When A's car has stopped (speed
   < 0.3 m/s, or 8 s after getting out) A's game sends **CarParked** (reliable) with K and the final transform; the
   others put their entity there (creating a copy if they never had one).
4. **Take over.** Anybody gets into the parked car K's driver seat the normal way (E). That game now simulates K and
   sends `seat 0` with K: step 1 with the new owner. On a game whose player drove K before (e.g. A's own real car),
   the entity is swapped for a fresh copy first, because a car the local player has driven throws NPC drivers out
   (docs/game-api.md 7): a copy we spawned is despawned, a real car is deactivated (hidden).

Conflicts: if two players get into the driver seat of the same parked car at the same moment, each game keeps its
own player driving (the other's `seat 0` claim for K is ignored and logged) until one of them gets out; then step 3/4
apply. Copies are capped at 12 per game (the oldest parked copy nobody sits in is despawned). When the session ends,
copies nobody sits in are despawned; the one the local player sits in stays as a normal car. A player who joins late
does not see cars parked before he joined until somebody drives them.

Puppets that cannot be seated (seat taken, the game keeps throwing them out) are hidden at the car and retried every
4 s.

Puppet lifetime: created when the first PlayerState of a player arrives, removed on `PeerLeft` / transport timeout /
session end (the whole set, including the hidden "factory" car, is removed when the session ends). Model per player:
`visuals/hero/game_main_tommy_{gangster_trenchcoat, gangster_suit_2_hat, cab_driver_flatcap, racing_outfit}` by
player id (0..3), overridable per machine with `puppet_model = <spawn profile>`.

Fire, aim, crouch, reload (no damage yet): a receiver turns `aim` (or a new shot, kept for 1 s as hip fire) into
`DbgTesting_AimAtDir(ax, ay, az)` on the puppet, fires one real shot per new `shots` count (trigger press/release, at
most a few per tick; a far backlog is skipped), `crouch` into `SetStalkMove`, and a rising `reload` into a reload
press. Shots are not separate messages: they ride on the 20 Hz state, so a lost packet only delays them by 50 ms.

## Phase 4: world (app ids 0x0200-0x02FF)

| App id | Name | Dir | Channel | Body |
|---|---|---|---|---|
| 0x0200 | WorldEnv | host -> guests (`Broadcast`) | R | f32 timeOfDay (hours 0..24), f32 timeFlowSpeed, str weatherSet |

WorldEnv is sent while the host is in the world: every 4 s and at once when the weather set or the time flow speed
changes. A guest in the world sets the weather set when it differs (`SetWeatherSet(name, 0, 0)`), the flow speed when it
differs, and its clock when it is more than 0.05 h (3 game minutes) off (`SetTime(t, 0)`). Guests' own weather/time
scripts may change things in between; the next WorldEnv puts them back.
