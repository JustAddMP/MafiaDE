# Sync architecture: how close to "100 % synced" we can get, and how

**Superseded in part (2026-10-20): the host owns the world, docs/host-authority.md.** The region authority below
(principle 1, traffic-regions, the region parts of ambient-peds, guest-owned NPCs 0x0424-0x0426) is retired; the rest
(time base, correction, NetRef, measuring, targets) stands.

Status: plan (2026-10-19), built 2026-10-20 (section 8 "Status"), written from the first real two-PC test (0.15.0, two sessions, 906 s and 345 s, Steam
P2P, rtt ~28 ms, loss ~0 %) and the code on main after the Round 4 merge. **Nothing in this document is verified in
game yet**; the numbers under "now" are measured (`python tools/sync_compare.py <host trace> <guest trace> --check` on
the 0.15.0 traces: session 1 = `-204809` / `-204821`, session 2 = `-210320` / `-210331`), the targets are what the next
two-PC test must show (README-dev "Sync architecture checks", `--check`). The Round 4 fixes (car speed in km/h, one
driver per wheel, PeerPaused, bikes, water, crews) were merged after that test and are not in these numbers.

## 1. What "100 % synced" can and cannot mean here

Mafia DE is a single-player engine. Every game runs its own physics, its own AI, its own animation system and streams
the world around its own player. We cannot step the games in lockstep (no control over the simulation step, no
determinism across PCs: float physics, AI decisions on timers and perception, streaming order), and the game has no
replication of its own. So exact equality of two worlds is impossible. What is possible, and what this plan builds:

- **One authority per thing.** Each player, car, NPC, traffic car, pedestrian, door, item has exactly one game that
  simulates it. Every other game shows a **replica** that is driven toward the authority's state. Never two AIs or two
  physics bodies deciding the same thing for long (the "dual simulation" that makes host cops drift 60 m today).
- **Replicas within a measured error budget**, corrected so smoothly that the correction is hidden inside the motion
  (no stutter), with a snap only when the replica is clearly wrong.
- **Events exactly once, in order** (hits, deaths, explosions, doors, takes): reliable, on the same lane as the states
  they refer to.
- **Everything measured**: every replicated kind leaves a truth record on its authority and a replica record on the
  receiver, keyed so tools/sync_compare.py can compute the real error at the same moment. "Synced" means "the
  numbers pass", not "looks fine".

What stays different between games, by design (section 9): ragdolls and debris, particles, ambient sounds, the exact
animation frame, crowd members beyond the replication radius, and anything the game simulates only around its own
player (a guest far from everyone else simulates his own surroundings).

## 2. Principles

1. **Authority.** A player's character and the car he drives: his game. Mission NPCs: the host (the story's owner);
   NPCs only a guest's game spawned: that guest until the host's game has them (claim, existing). **The ambient world
   (traffic, pedestrians): the region authority** = the host within 250 m of the host; elsewhere the nearest player's
   game; hysteresis (an owner keeps a region until he is 400 m away and another player is 50 m nearer). Today traffic
   follows the host only and pedestrians are not shared at all.
2. **Stable identity.** Every replicated thing has a `NetRef {home player, kind, id on the home game}` (core
   `net/authority.h`). Existing wire keys map onto it without a wire change (table in section 6); new messages use it.
3. **Ownership changes are explicit.** A claim carries an epoch; the higher epoch wins, an equal one keeps the current
   owner, simultaneous claims go to the lower player id (core `OwnershipBook`). Cars keep their tested rules (the
   holder / the lower id keeps the wheel, Round 4) which are the same rule.
4. **States vs events.** Continuous things are unreliable snapshots stamped with the sender's **sample** time; a lost
   one is replaced by the next. One-shot things are reliable events (hits, deaths, explosions, door changes, takes,
   claims) or counters riding the state (actions, shots). Spawn/despawn of replicas are reliable.
5. **One time base.** Each sender's stream goes through one `PlayoutClock` (adaptive playout delay: jitter p95 +
   interval, 25-60 ms on the measured link) and is interpolated with Hermite curves through the snapshots' velocities
   (core `net/replica.h SnapshotTrack / Stream`), extrapolated at most 250 ms, then held. Today players and traffic
   each have a copy of this and NPCs have none (the AI walks to the newest state).
6. **One correction layer.** `core net/correction.h Corrector` with presets per kind: a dead zone, then a blend of
   the excess capped at a share of the replica's own speed (a moving replica slides at most ~35 % of its speed, a
   standing one at most 0.15 m/s), a snap only past a distance / height, on a teleport, or when stuck. A pinned car
   copy (SetPos every tick) is a last resort, retried within 1.5-8 s (was 5-40 s).
7. **Interest by distance to every player.** Core `InterestManager` (per receiving peer, viewpoints = that player and
   his car) chooses what each peer gets and how often; nothing is chosen by distance to the host alone.
8. **An absent authority is announced** (PeerPaused 0x0103, Round 4): replicas freeze instead of extrapolating and
   "knocking".
9. **No switches.** New behaviour replaces old; nothing can be turned off.
10. **Measured.** Truth + replica trace records per kind, `sync_compare.py --check` with the targets of section 4.

## 3. Audit per system (now)

Numbers: S1 / S2 = the two sessions of the 2026-10-19 test; "host->guest" = the host as seen on the guest's game.

| System | Authority now | State / events now | Rate, size | Correction now | Join / reconnect | Measured now | Main gaps |
|---|---|---|---|---|---|---|---|
| Players on foot | his own game | PlayerState 0x0100 U, sample-stamped; actions as counters | 60 Hz, ~60 B (~4-7 KB/s per player) | LocomotionController: gait from the wanted speed, nudge only beyond 0.9 m, snap 3.5 m | puppet from the first state | puppet vs player mean 0.71-1.04 m, **p95 1.97-2.34 m**, max 7.5 m; controller error p95 1.87-2.18 m (the network target is ~0.1 m); **gait match 63-76 %**; snaps 64 / 7 (S1) | the AI walk, not the network, is the error; the gait is not the one he sends; 0.3-0.9 m errors are never corrected |
| Player cars (and bikes, Round 4) | the driver's game | in PlayerState (transform, velocity, controls, signals) | 60 Hz, ~140 B | VehicleController blend 8/s, pinned when slow | copies of cars parked before a join are not sent | copy vs car mean 0.39-0.73 m, **p95 0.80-1.93 m**, **pinned 56-74 %**, speed ratio 0.28 (km/h bug, fixed in Round 4, unverified); wheel angle p95 40-51 deg | pins are sticky (5-40 s retry); no parked-car snapshot on join |
| Seats, ride-along | driver's game; seat claims in PlayerState | | | put back / slide (seats.h) | | 2 dual-driver episodes (fixed Round 4), puppet thrown out x4 then "retrying slower" | |
| Traffic | **the host only** (guest within 250 m of the host) | TrafficSpawn/Despawn R, TrafficStates U (host clock) | up to 20 Hz, 22 B per car; 1.0-1.6 KB/s median, max 4.7 | VehicleController + its own interpolation copy, pins | spawns on interest enter | shown 86-90 % of the time, copy vs **target** p95 0.65-0.71 m (the true error vs the host's car is not measurable: no host truth record); pins 519 / 651; 65 / 70 knocks (host pauses); **11 of the guest's own cars still drove near him unshared** (S1); a guest far from the host and two guests together: never shared | host-only authority; no truth record; pop on hand-over (own cars hidden, host's appear) |
| Mission NPCs | host; the guest's own game runs a **second AI** for the paired NPC | NpcStates 0x0210 U, **no timestamp**; deaths R | 10 Hz, ~45 B x 48 = **~21 KB/s per guest** (largest stream) | paired: pulled back only after 6 m (12 m in a fight) for 1.5 s; spawned copies: ad-hoc MoveVec in Lua, no interpolation | states every 0.1 s | S1: distance mean 9.4 m, **p95 60 m, max 252 m** (host cops chasing the guest: two AIs); S2 spawned copies 0.00-0.06 m | dual AI; no time base; host-centric pick; fat records |
| Ambient pedestrians | **each game its own** (not shared) | sent by the host in NpcStates but never shown | | | | **47-48 host NPCs in range, 0.6-1.1 paired** (~2 % of the street is the same) | not replicated at all |
| Guest-owned NPCs | that guest, claimed back by the host | GuestNpcStates 0x0420 | 10 Hz | as NPC copies | | S1: 12 owned, 0 of 179 shown (cops without profile, fixed Round 4); S2 0.03 m | |
| Police | each player's own wanted state; host cops chase guests' puppets | PoliceState/Event/Chase | <0.3 KB/s | | heartbeat | decay held (Round 3, unverified) | cops on the guest's game = NPC dual AI (above) |
| World (time, weather) | host | WorldEnv R every 4 s; SetTime when > 3 game-minutes off | | snap | next WorldEnv (up to 4 s) | fine | 3 min jumps; not stamped |
| Missions, cutscenes, fail flow | host | MissionState / HostSave / Cutscene / MissionFail / FailVerdict R | | follow + reload | sent to newcomers | death/fail in Free Ride works | not tried in chapters on two PCs |
| Doors | host for story doors | DoorEvent / Apply R, snapshots | | | snapshot on join | 0 doors near in the test | |
| Items | host | ItemStates U 2-4 Hz, take/gone R | | | periodic | 34 despawns, no take tried | |
| Explosions, throws | the game where it happened | Explosion / Throw / ThrowFix R | events | marker sim | | not tried | |
| Car damage, look | the car's holder | CarDamage / Report / Request / CarLook R | changes | | request per new copy | works; echo fixes Round 3 | |
| Transport | core 0.11 | lanes, priorities, session clock, adaptive playout | | | reconnect 60 s | loss 0-0.01 %, rtt 25-27 ms, LAT p50 16-18 ms / p95 26-34 ms, pd ~40 ms | fine; NPC/traffic bypass the shared playout layer |

## 4. Targets (what the next two-PC test must show)

| Kind | Metric (sync_compare --check name) | Now | Target |
|---|---|---|---|
| Puppet on foot | `foot.p95` puppet vs player at the same moment | 1.97-2.34 m | **<= 0.8 m** |
| | `foot.stand.p95` (player standing) | 1.25-1.53 m | <= 0.3 m |
| | `foot.mean` | 0.71-1.04 m | <= 0.35 m |
| | `foot.gait` gait match | 63-76 % | >= 90 % |
| | `foot.snaps_per_min` | 8.2-17.3 (S1), 0.3-6.9 (S2) | <= 0.5 |
| | `foot.shown` his puppet (or his car copy) there while his states arrive (added after the 2026-10-20 test) | 19.5-20.6 % (host on the guest, 0.16.0) | >= 98 % |
| | `foot.respawns` / `foot.respawn_s` puppet spawned again with no reason / back after the game removed it | 1 / never (0.16.0) | 0 / <= 5 s |
| | `foot.dz_p95` height puppet vs player on foot | 0.76-0.79 m (0.16.0, bike riders) | <= 0.5 m |
| Puppet in a seat | `seat.flips_per_min` off its seat and put back while its player sits on (PR `ejects`; the first / last 2 s of a stay left out) | 490 a minute (0.17.0 one-PC run 02:19: on and off a motorbike ten times a second) | 0 |
| Player car copy | `car.p95` copy vs car | 0.80-1.93 m | **<= 1.0 m** |
| | `car.pinned` share of samples | 56-74 % | <= 10 % |
| | `car.speed_ratio` median | 0.28 | 0.9-1.1 |
| Traffic copy | `traffic.true_p95` copy vs the host's car at the same moment (new truth record) | n/a | **<= 1.5 m** |
| | `traffic.shown` share of the time near the authority | 86-90 % | >= 95 % |
| | `traffic.unshared_near` the receiver's own cars driving within 70 m | max 11 | 0 after 10 s |
| Mission NPCs | `npc.p95` replica vs authority NPC at the same moment | 55 m (S1), 0.06 m (S2, spawned copies only) | **<= 1.0 m** (on foot) |
| | `npc.max` | 252 m (S1), 0.06 m (S2) | <= 4 m (snap) |
| Pedestrians | `ped.coverage` authority pedestrians within 40 m of a receiver that have a replica | ~2 % | >= 80 % |
| | `ped.p95` | n/a | <= 1.5 m |
| World | `world.time_err` game-minutes | up to 3 | <= 0.5 |
| Bandwidth | `bw.host_up` host upload, median | 25-29 KB/s measured with 2 players; ~130-160 estimated with 4 | <= 160 KB/s with 4 players |
| Latency | `lat.player.p95` send -> applied | 26-34 ms | <= 40 ms (unchanged) |

The on-foot target is the hard one: the puppet is an AI human walked by the game's own locomotion (that is what
gives it real feet and animation). 0.8 m p95 is ~110 ms of a sprint. If the next test shows the AI walk cannot get
there with the gait from the sender and continuous hidden corrections, the fallback is a stronger share (up to 50 %
of the speed) at the cost of some foot sliding; we do not go to SetPos every frame (that is the stutter / skating the
0.13.0 controller removed).

## 5. Gaps ranked by how much they hurt play

1. **Mission NPCs run two AIs** (host cops chasing the guest: p95 60 m, max 252 m; a guest's enemies are in other
   places than the host's). Every fight looks different on each game. -> package `npc-authority`.
2. **Puppets on foot 2-2.3 m off at p95, wrong gait a third of the time.** You aim at a friend who is not there; he
   stops and slides. -> `players`.
3. **The street is not the same**: ~2 % of the pedestrians and, away from the host, none of the traffic is shared;
   near the host the guest still has up to 11 of his own cars driving. -> `ambient-peds`, `traffic-regions`.
4. **Car copies pinned most of the time** (stutter, no suspension / wheels, they cannot be hit properly). Partly the
   km/h bug (Round 4); the sticky pin retry stays. -> `sync-core` (pin policy), `players`.
5. **NPC stream is the fattest** (21 KB/s per guest, no timestamp, host-centric choice) -> with 4 players the host
   uploads ~60 KB/s of NPC states alone. -> `npc-authority`.
6. **Late joiners miss parked shared cars**; time of day can be 3 game-minutes off; nothing tells the guest when his
   world is complete. -> `players` (parked cars), `world-join`.
7. Interpolation / correction implemented three different ways (players, traffic, none for NPCs). -> `sync-core`.

## 6. Identity map (NetRef, no wire change for existing messages)

| Thing | Existing wire key | NetRef |
|---|---|---|
| Player car | `(keyOwner u8, vehId u32)` | `{keyOwner, PlayerCar, vehId}` |
| Host NPC | `u32` host entity id | `{0, Npc, id}` |
| Guest-owned NPC | `u32` guest entity id; relay key `0xF0+A` type byte | `{A, Npc, id}` |
| Traffic car | `u16 net` (per host) | `{authority, Traffic, net}` (new: any authority) |
| Pedestrian | new | `{authority, Pedestrian, net}` |
| Door | `u32` name hash + position | `{0, Door, hash}` |
| Item | `u32` host key | `{0, Item, key}` |

## 7. Bandwidth with 4 players (host upload, KB/s)

| Stream | Now (2 players measured; 4 estimated) | After |
|---|---|---|
| PlayerState own + relays (3 x 3 streams x ~6.5) | ~58 | ~58 (unchanged; relays could be thinned by distance later) |
| NpcStates (48 x 10 Hz x 45 B per guest) | 21 measured per guest -> ~63 | compact timestamped records (~20 B), rate by distance (15 Hz near, 2 Hz far): ~8 per guest -> ~24 |
| Traffic | 1-1.6 measured per guest (cap 40) -> ~5-15 | similar, plus relayed guest-owned traffic |
| Pedestrians | 0 | ~6 per guest (32 x 10 Hz x ~18 B) -> ~18 |
| Everything else | < 3 | < 3 |
| **Total** | **~130-160 (est.), worst case ~300** | **~110-120, worst case ~200** |

## 8. Work packages

Each package: its own worktree `G:\JustAddMP\wt\<pkg>`, branch `feat/<pkg>`, tests are part of it, rules in
agent-rules.md. Message ids are fixed here so nothing clashes. Only `players` changes PlayerState.

| Package | Repo / files owned | Message ids | Builds on | Acceptance (unit) | Acceptance (two-PC, --check) |
|---|---|---|---|---|---|
| `sync-core` | core: `net/replica.*`, `net/correction.*`, `net/authority.*`, `remote_motion.*` (gait hint, hidden nudges, pin retry), tests `net_replica`, `net_motion`; docs/replication.md | none (NetRef 6-byte form) | - | gait match >= 95 % and p95 <= 0.6 m on a simulated laggy AI walker; pin released within 2 s of recovery; ownership / region rules | - |
| `players` | mafia-de `player_sync.*` (C++ + Lua), `seats.*`, `car_swap.*`, PlayerState, CarParked | 0x0104-0x0107 | sync-core | the player stream through `replica::Stream`; gait hint; parked-car snapshot codec | `foot.*`, `car.*` |
| `npc-authority` | `npc_sync.*`, `npc_owner.*`, tests | 0x0212-0x0217, 0x0424-0x0427 | sync-core | one authority per NPC; timestamped compact NpcStates2; interest per guest; replica driving in C++ | `npc.*`, NPC bandwidth |
| `traffic-regions` | `traffic_sync.*`, `traffic_model.*`, tests | 0x0224-0x022B | sync-core | region authority (any player's game owns traffic around him), hand-over without pops, host truth record | `traffic.*` |
| `ambient-peds` | new `ped_sync.*`, `ped_model.*`, `tests/ped_test.cpp` | 0x0280-0x0287 | sync-core | pedestrian replication by region authority, local pedestrians adopted / hidden | `ped.*` |
| `world-join` | `mission_sync.*` (WorldEnv part), new `join_sync.*` | 0x0207-0x020B | - | stamped WorldEnv, smooth time convergence, a join / reconnect "world complete" report | `world.*`, join timing |

Shared files every package may touch only in the lines for its own ids: `src/game/latency.cpp` (lanes, priorities,
system names), `src/game/module.cpp` (wiring), `build.cmd`, `docs/protocol.md` / `docs/sync-trace.md` (own sections),
`tools/sync_compare.py` (own section + its metrics through `record()`).

### Status (2026-10-20): built, not verified in game

All six packages are merged on main (core v0.12.0). What each delivered and its simulated numbers (unit tests with
modelled copies; the game's real AI walk, car physics and civilians may differ):

| Package | Branch | Wire | Simulated |
|---|---|---|---|
| sync-core | core `feat/sync-core` (v0.12.0) | NetRef form only | walker: gait 95-98 %, p95 0.34-0.41 m moving, 0.07-0.09 m standing |
| players | `feat/players` | ParkedCars 0x0104 (PlayerState unchanged) | foot p95 0.65-0.75 m, standing 0.25-0.27 m, gait 99.8 %; car p95 0.88-0.98 m, pinned 0-0.1 % |
| npc-authority | `feat/npc-authority` | 0x0212-0x0214, 0x0424-0x0426; 0x0210 / 0x0420 retired | two session-1 cops: p95 0.97 m, max 2.15 m; 6.3 KB/s at 48 NPCs |
| traffic-regions | `feat/traffic-regions` | 0x0224-0x0227 | true p95 0.91 m at 30 m, 1.27 m at 150 m; unshared 0; shown 99.7 % |
| ambient-peds | `feat/ambient-peds` | 0x0280-0x0284 (lane 5) | p95 0.78-0.86 m; coverage 100 % |
| world-join | `feat/world-join` | JoinReport 0x0207; WorldEnv + u32 sample time | convergence without overshoot |

Two more `--check` metrics came with the packages: `npc.bw` (<= 8 KB/s per guest) and `join.complete_s` (<= 10 s).
The biggest open questions for the game: whether a mission NPC's and a civilian's AI really stops while it is a
replica, how close the game's AI walk is to the simulated walker, and the semantics of BlockSpawnersInSphere.

## 9. What stays out of reach (said plainly)

- Exact animation frames, ragdoll poses, debris, glass, particles, sounds: local pictures on every game.
- Physics contacts between a local car and a replica car are resolved by each game separately; the authority's
  result wins after the correction (a crash can look slightly different on each screen).
- A guest far from everyone simulates his own surroundings (region authority); when another player arrives, cars and
  pedestrians already there keep their authority until they leave (no hand-over of a running car's physics).
- The NPC's real AI exists only on the authority; a replica's shots are pictures, its damage comes from the authority
  (Hit with source 1). Cover and aim therefore depend on how well the authority's puppet of you matches you
  (target <= 0.8 m p95). No lag compensation beyond that (the shooter's game decides, "favour the shooter").
- Mission scripts still run on every game; only their visible results (objectives, cutscenes, NPC deaths) are
  mirrored. A script that reacts to a purely local event can still diverge (follow/reload catches it).

## 10. Measuring

Every package adds a truth record on the authority and a replica record on the receiver keyed by (NetRef, sender
sample time), and its section in `tools/sync_compare.py` reports the true error and calls `record(metric, value)`.
`python tools/sync_compare.py <trace A> <trace B> --check` prints every metric of section 4 with PASS / FAIL / no data.
