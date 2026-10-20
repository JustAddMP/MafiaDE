# Host authority: the host's game owns the world

Status: plan (2026-10-20), decided by the user ("the host should own everything"). It replaces the region authority of
docs/sync-architecture.md (0.16.0) for traffic, pedestrians and NPCs. Nothing in this document is verified in game.
Numbers marked "measured" come from the 2026-10-20 two-PC traces (session A: host `-023938` / guest `-024103`; session
B: host `-024605` / guest `-024844`, `tools/sync_compare.py`), the rest are estimates.

**Defaults chosen (the user said "go"; each can be changed later, section 7):** leash 400 m with a warning from 300 m;
past 400 m a 10 s countdown, then the guest is put back beside the host (with his car when he drives). Guest-only
mission NPCs are held hidden until the host's game has them. Parked cars, trams and boats stay local in this round.
Pedestrian copies use a rotation of six civilian looks. The host's leash numbers win and go to the guests.

## 1. The model in one page

**Who owns what**

| Thing | Owner (simulates it) | Everyone else |
|---|---|---|
| A player's character | his own game | puppet (as today) |
| The car a player drives (bike included) | the driver's game | car copy (as today; holder / lower-id rule) |
| A player's shots, punches, explosions, run-overs | his own game *decides that he did it*; the **host resolves** what it does to the world (NpcHit, PedKill, CarDamageReport, ItemTake, DoorEvent: requests, as today) | |
| Traffic and police cars | **host** | copies with crews (TrafficSpawn/Despawn/States 0x0220-0x0223, as in 0.15) |
| Pedestrians | **host** | spawned copies only (PedStates 0x0280-0x0284) |
| Mission NPCs, police on foot | **host** | the guest's own paired NPC as a replica (AI off), else a spawned copy (NpcStates2 0x0212-0x0214) |
| World items, doors | host (as today) | |
| Explosions in the host's world, blown-up cars | host (as today) | picture |
| Time of day, weather | host (WorldEnv, as today) | |
| Wanted level | each player's own game (as today); only the **host's** police exist | his game spawns no police |

**What flows which way**

- Host -> each guest: PlayerState relays, WorldEnv, mission/cutscene/fail messages, traffic, pedestrians, NPCs, items,
  doors, police chase, car damage/look of host cars, ParkedCars, the leash settings (new LeashConfig 0x0208).
- Guest -> host: his PlayerState (60 Hz), requests (NpcHit, PedKill, CarDamageReport/Request, ItemTake, DoorEvent,
  TrafficBump, MissionFail, JoinReport, Hit), his car's damage and look, PoliceState, LeashEvent (new 0x0209).
- Guest -> guest: only player things through the host's relay (PlayerState, his car's damage/look, Hits). **No world
  state goes from one guest to another any more.**

**What the guest's game must switch off, and how** (new module `guest_world`, section 5 WP-A)

| What | How (calls the code already uses or found) | When |
|---|---|---|
| Ambient traffic spawning | `game.traffic:SetEnableAmbientTrafficSpawning(false)` (traffic_sync.lua `T.LocalTraffic`, used since 0.15 near the host) | every world load, on the first tick in the world, until the session ends |
| Police spawning | `game.police:PreventPoliceSpawning(true)` (same place) | same |
| Pedestrian spawning | `game.traffic:SetMaxHumanElements(0)` (game-api 26: the global pedestrian budget; "not used" in 0.16 because it would empty the guest's world - exactly what we want now). Its old value is read first from `[traffic manager + 0x100]` (game-api 26, static; the manager is a global pointer, found 2026-10-20) so the session end can restore it; if that read fails, the default is taken from the first host game's reading (same build) or the game's own value comes back with the next world load. **Fallback** when the budget turns out to stop our copies or mission spawners as well: `BlockSpawnersInSphere(pos, 150, true)` every 20 m (ped_sync.lua today, one-shot per spawner) plus hiding leftovers | same |
| Cars and humans already there | hidden at once while the loading screen is still up, later only out of view: `Deactivate()` (+ `ShowModel(false)` for humans), the T.ClearLocal / ped hide code, now for every generator car and every ambient human (game-api 15/26 rules: `IsAmbientHuman`, generator driver `behavior_object_prime-NN-`), never our puppets, copies, crews, copy spawners or the puppet factory | from the first tick in the world, every 1 s |
| Trams, parked cars, boats | **left alone this round** (section 7 D4) | |
| Mission NPCs of the guest's own scripts | not switched off (scripts must keep running): paired with the host's NPC -> replica; not paired after 5 s -> **held** (hidden, AI interrupted, demigod) until the host's NPC appears | as npc_sync today |

**Stays from 0.15 / 0.16**: everything for players (puppets, car copies, seats, ParkedCars, PeerPaused, water, bikes,
melee, PvP, fail flow, saves, profile guard), the host halves of traffic (AuthorityTraffic, DriverWatch, CopySpeed),
pedestrians (PedAuthority, PedReceiver without adoption) and NPCs (Publisher, ReplicaSet, pairing, NpcHit replays,
spawned copies), core's replica/correction/interest layer, WorldEnv, JoinReport, doors, items, explosions, car damage
and look, police chase, latency lanes, the trace records and `sync_compare --check`.

**Removed / retired**

| Where | What goes |
|---|---|
| traffic_model.h/.cpp | `RegionConfig`, `RegionFollow`; the guest's own `AuthorityTraffic`; the host's `ReceiverTraffic` (the host shows nobody's traffic); the `0x0224-0x0227` codecs (kMsgSpawn2..kMsgBump2) |
| traffic_sync.cpp/.lua | the region update, "M 1/0", `T.LocalTraffic`, `T.ClearLocal` and `T.hidden` (moved to guest_world), the guest's own scan and TC events, the guest -> guest relay, TL lines, the region parts of the status line |
| ped_model.h/.cpp | `RegionTuning` / `region_` / `SetRegion`, the guest's `PedAuthority`, adoption (`adoptNear/adoptFar`, `MatchPair`), hiding surplus humans (`zoneRadius`, `showAfter`), the guest -> guest relay |
| ped_sync.cpp/.lua | the adopt / hide / show commands, `BlockSpawnersInSphere` (moved to guest_world as the fallback) |
| npc_owner.h/.cpp, tests/npc_owner_test.cpp | the whole module: `GuestOwner`, `HostMirrors`, claims, relay keys |
| npc_sync.cpp/.lua | the guest's own-NPC report, the host's mirrors, NpcClaim, the relay of guests' NPCs |
| Messages | retired (neither sent nor read; ids stay reserved, never reused): TrafficSpawn2/Despawn2/States2/Bump2 **0x0224-0x0227**, GuestNpcEnd/HostNpcHit/NpcClaim **0x0421-0x0423**, GuestNpcStates2/Info/Leave **0x0424-0x0426** (0x0420 already retired) |
| latency.cpp | lanes / priorities of the retired ids |
| core | nothing deleted. `RegionAuthority` and `OwnershipBook` stay in core (other games may want them); mafia-de no longer calls `RegionAuthority` |
| docs | sync-architecture.md principle 1 and the region parts are marked "superseded by host-authority.md" |

## 2. Tonight's branch fixes

| Fix (branch) | Under host authority |
|---|---|
| On-foot velocity matching, core `LocomotionController::Applied` (feat/sync-gaps aa3f8da, core feat/foot-track 3e892eb) | **carries over unchanged** (players own themselves) |
| Traffic: copies of standing cars no longer driven off them (`CopySpeed`) (af1bbbf) | **carries over** (host traffic on guests) |
| Traffic: driver LOD (DriverWatch `Far`, TC flag 64, 40 m) | **carries over** for the host's cars; the guest-authority half is moot |
| Traffic: crew churn held (no crew sent out for streamed-out drivers, put-back limits) | **carries over** |
| Fixed cars / ped spawner not shared as traffic (main 6a2dcb9) | host: still needed (his own puppet factory and copy spawners); guest: moot (he shares no traffic) |
| Host profile / save guard (feat/sync-profile 6a29d7b) | **carries over unchanged** |
| Pedestrians: riders faster than 8 m/s not shared (tram riders) (feat/sync-peds 652cbf9) | **carries over** (PedAuthority, host) |
| Pedestrians: lag snap / release of a lagging copy | **carries over** (every replica is a copy now) |
| Pedestrians: copy spawner health (FR after 2 failures, every copy gone), cap 24 / 4 loading | **carries over and matters more**; the cap rises to 40 copies (pool, WP-C) |
| Pedestrians: "spawns at once when there is nothing to adopt" | moot (nothing is ever adopted); its wait becomes the only wait |
| Pedestrians: copy spawner hidden by traffic_sync's ClearLocal (the 18 % coverage of session A) | the cause is gone with ClearLocal; guest_world's hide must skip every spawner of ours (test) |
| Bike GUIDs (main 6a2dcb9), puppet respawn after a loss, join timing (main f09322c) | **carry over unchanged** |
| sync_compare: seated filter, AI-walk report, foot.shown / respawns / dz | **carry over** |
| The region parts of 0.16 (traffic-regions, ped region, guest NPCs) | removed (section 1) |

## 3. The leash

Mafia simulates only around its own player. The host's world measured on 2026-10-20: his shared traffic cars were
median 56-58 m, p95 164-169 m, max 192-224 m from him; his game had a median 60 (max 96) ambient humans; the game
streams traffic drivers out beyond ~50 m. So the guest's streets thin out with his distance from the host and are
empty well before 400 m. The players were 14 m apart (median) in both sessions; in session B the guest drove to 648 m
from the host (from 205 m to 648 m in 25 s) - the case the leash is for.

**Distances (defaults, the host's ini wins and goes to guests in LeashConfig):** `leash_warn_m = 300`,
`leash_max_m = 400`, `leash_countdown_s = 10` (0 turns the pull off: warning only). Distance in the ground plane
between the guest's character (his car when he drives) and the host's puppet on the guest's game.

**What the guest sees**

| Distance | Banner (the existing centre banner, module.cpp `DrawNotice` style: black box, gold line, 82 % down) |
|---|---|
| < 300 m | nothing |
| 300-400 m | `FAR FROM THE HOST - THE STREETS THIN OUT (340 M)` |
| > 400 m | `TOO FAR FROM THE HOST - BACK IN 10 S` counting down; back under 380 m (hysteresis 20 m) stops it |
| 0 s | the screen fades (the game's `FadeOut` if it works, else a short black overlay), the guest is put beside the host (`M.NearHost`, as after dying); driving: his car is put on the road 15 m behind the host (`game.traffic:GetClosestPointOnRoad`, facing the host's heading), he stays in it, its copies jump (the teleport rule of every copy); then `PULLED BACK TO THE HOST` for 3 s |

The host sees `MILES IS FAR AWAY (420 M)` in his status panel (LeashEvent) - no banner over his game.

**Suspended (no countdown; the banner says why or nothing):** loading screens and 5 s after; cutscenes (the guest is
held by mission_sync anyway) and 5 s after; the host paused (PeerPaused) or not in the world; the host or the guest
dead / downed / respawning; 15 s after the host jumped more than 50 m in one state (a mission teleport, his own
respawn); the first 20 s after a join or a reconnect (JoinReport not complete); the guest in the host's car or in a car
the host is in; a fail-flow reload under way.

**Edge cases**

- *Guest loads first:* his world is empty (spawning off from the first tick); the leash waits for the host's states;
  when Free Ride starts, guests spawn beside the host (existing join placement).
- *Host pauses:* replicas hold (PeerPaused), the countdown freezes at its value, the banner says `THE HOST PAUSED`.
- *Host in a cutscene:* guests are held by mission_sync; leash suspended.
- *Guest dies far away:* the guest is downed and respawns beside the host already (`M.DownTick`); a real death loads
  his checkpoint and puts him beside the host; the leash does nothing extra.
- *Reconnect:* suspended until the JoinReport is complete + 20 s; then normal.
- *Passengers:* a guest who is a passenger is leashed by the car's driver: in the host's car never; in another guest's
  car the driver's leash pulls the car with everyone in it (only the driver's game moves it).
- *Missions:* the same numbers; a mission that needs the players apart (none known) would be a reason for D1.
- *Several guests:* each guest is leashed to the host, not to each other.

## 4. Interest and bandwidth

Within the leash everything the host's game simulates near a guest is relevant. Per guest (viewpoint = his puppet or
car on the host's game): traffic within 260 m (leave 320 m, 24 cars nearest first: unchanged), pedestrians within
**80 m** (leave 90 m; was 50/60: the guest has no others now), **48** at most, NPCs within 120/140 m (unchanged, 7 KB/s
budget).

Measured 0.16 host upload per guest (NetMonitor, session B, median / max KB/s): player 3.3 / 3.9, traffic 0.57 / 3.6,
items 1.9 / 1.9, NPCs 0.27 / 1.9, pedestrians 0.16 / 1.5, the rest < 0.2. Session A traffic: 4.6 cars admitted on
average (max 15), 29 % of states skipped as unchanged; NPCs in interest median 10, max 13 (1.25 KB/s median).

| Stream, per guest | Typical (estimate) | Worst (caps) |
|---|---|---|
| PlayerState (host + relays of other guests) | 3.5 (+3.5 per other guest) | 7.8 (+7.8) |
| Traffic (24 x 22 B x up to 20 Hz, 29 % skipped) | 2-4 | 10.5 |
| Pedestrians (48 x ~5 Hz x 16 B) | 2-4 | 6.5 (16 KB/s budget) |
| NPCs | 1.3 | 8 (budget) |
| Items, doors, world, police, car damage / look | 2.2 | 3 |
| **Total per guest** | **~11-15** | **~36** |
| Host upload, 1 guest / 3 guests | ~13 / ~50 | ~36 / ~130 |
| Guest upload | ~4 (his state + requests; was up to 7 with his traffic, peds and NPCs) | ~8 |

Under `bw.host_up` <= 160 KB/s. New check `bw.guest_up` <= 8 KB/s median.

## 5. Work packages

Each: worktree `G:\JustAddMP\wt\<name>`, branch `feat/<name>` from `feat/host-authority-plan` (= feat/sync-gaps +
this doc), tests are part of it, build.cmd + every `out\*_test.exe` green, sync_compare section for its metrics,
tools/onepc step for its in-game check. Shared files are touched only in the lines of its own ids/modules
(`module.cpp`, `latency.cpp`, `build.cmd` game_logic line, `docs/protocol.md`, `docs/sync-trace.md`,
`tools/sync_compare.py`, `tools/onepc/*`). No game launches.

| WP | Name / branch | Files owned | Wire | Tests (trace replays on tests/data) | `--check` targets | Effort |
|---|---|---|---|---|---|---|
| A | `guest-world`: the guest's own world off | new `src/game/guest_world.{h,cpp,lua}` (pure policy in .h/.cpp: when to switch, re-apply per world, restore at the end, what may be hidden), module.cpp wiring, `tools/onepc/suppress_probe.lua` (counts own cars / humans per distance band, checks the puppet factory, copy spawners and a mission spawner still spawn) and `world_extent.lua` (host: cars / humans per distance band) | none; trace `GW` 1 Hz: own cars, own humans, hidden, budget set/restored | `guest_world_test`: switch on the first world tick, again after a reload, never on the host, restore at the end; hide rules never touch our puppets/copies/crews/spawners (the 0.16 ClearLocal bug that hid the ped spawner); fallback path when the budget call fails | `world.guest_own_cars` = 0 and `world.guest_own_peds` = 0 (10 s after a world load) | 3-4 h |
| B | `host-traffic`: traffic from the host only | traffic_model.*, traffic_sync.*, tests/traffic_test.cpp | 0x0224-0x0227 retired | traffic_test: region tests removed; the 2026-10-20 session A replay (tests/data/traffic_replay.h) still <= 0.8 m true p95; a guest's game builds no authority and sends nothing; the host receives no traffic | `traffic.true_p95` <= 1.5, `traffic.shown` >= 95, `traffic.crew_exits_per_min` <= 1, `traffic.reseats_per_min` <= 3, `traffic.unshared_near` replaced by WP-A's metric | 3 h |
| C | `host-peds`: pedestrians from the host only, copy pool | ped_model.*, ped_sync.*, tests/ped_test.cpp, tests/data/ped_trace.h (from feat/sync-peds) | 0x0280-0x0284 unchanged (host -> guest only; PedKill guest -> host) | ped_test: replay of the real PDT of sessions A and B with a copies-only receiver and a spawner model (one load at a time, 0.8-2.5 s, failures as in A): coverage >= 80 %, p95 <= 1.5 m; pool reuse (a copy whose pedestrian left is hidden and given to the next); six-look rotation | `ped.coverage` >= 80, `ped.p95` <= 1.5, new `ped.copy_fail_per_min` <= 2 | 4-5 h |
| D | `host-npcs`: NPCs from the host only, guest-only NPCs held | npc_sync.*, npc_owner.* (deleted), npc_replica.* (hold rule), tests/npc_owner_test.cpp (deleted), new tests/npc_hold_test.cpp | 0x0421-0x0426 retired | npc_hold_test: a guest NPC unpaired for 5 s is held (hidden, AI off), paired and shown when the host's NPC appears, dies with it, never sent; replay of session A's guest NPC list (the 11 guest-owned NPCs of A) | `npc.p95` <= 1.0, `npc.max` <= 4, `npc.bw` <= 8, new `npc.guest_sent` = 0 | 3-4 h |
| E | `leash` | new `src/game/leash.{h,cpp}` (pure: distances, hysteresis, countdown, suspensions), player_sync.lua (pull: NearHost / car to road), module.cpp banner, `tools/onepc/leash_test.lua` | LeashConfig 0x0208 host -> guests R (u16 warnM, u16 maxM, u8 countdownS, u8 flags); LeashEvent 0x0209 guest -> host R (u8 kind: 1 warn, 2 countdown, 3 pulled, 4 back; u16 distM) | leash_test: replay of both players' positions of sessions A and B at 1 Hz (tests/data/leash_1020.h): A no warning (max 98 m); B warning at 300 m, countdown at 400 m, one pull; every suspension rule; config from the host | new `leash.pulls` (info), `leash.false_pulls` = 0 | 4-5 h |
| F | `host-mission-cars` (later, after the Free Ride two-PC test passes): cars driven by host NPCs in missions shared like traffic, the guest's own mission car hidden behind the copy | traffic_sync / npc_sync | none (traffic messages) | | | 6-8 h |

**Merge order** into `feat/host-authority` (from feat/host-authority-plan): A, B, C (with the feat/sync-peds parts
that still apply), D, E; then the integration commit: core branch pinned (feat/foot-track, no tag), protocol.md,
sync-architecture.md "superseded" notes, CHANGELOG Unreleased, README-dev checks, tools/onepc README rows, modVersion
stays 0.16.x until the release decision (a 0.17.0 bump makes 0.16 games incompatible, as they already are by the
retired ids).

## 6. Verification

**One PC (tools/onepc, two instances; when game runs are allowed again):** run_test.ps1 gets three steps:
1. `suppress` (guest_world): `suppress_probe.lua` on the guest after 10 s and 60 s in the world: own moving cars 0,
   own ambient humans 0, puppet present, ped copies > 0; `world_extent.lua` on the host (cars / humans per 50 m band:
   tells whether 300/400 m is right).
2. `peds` and `traffic`: a minute walking and a minute driving; compare.ps1 expects `ped.coverage` >= 80, the
   traffic rows of today.
3. `leash`: `leash_test.lua` moves the guest 450 m away (SetPos on foot, then in a car): banner lines in the log,
   `leash: pulled back beside the host` after ~10 s, `LSH` trace rows; then with the host paused: no pull.
Expected compare.ps1 additions: `world.guest_own_cars` 0, `world.guest_own_peds` 0, `npc.guest_sent` 0,
`leash.false_pulls` 0, `leash.pulls` 1-2, `bw.guest_up` <= 8.

**Two PCs (the user's script, ~45 min, both PCs send their `logs` folders afterwards):**

| # | Scenario | Min | Expected PASS lines (`sync_compare.py <host> <guest> --check`) |
|---|---|---|---|
| 1 | Host starts Free Ride, guest joins | 3 | `join.complete_s` <= 10; guest log `guest world: our own traffic, police and pedestrians are off` |
| 2 | Walk together downtown | 5 | `ped.coverage` >= 80, `ped.p95` <= 1.5, `world.guest_own_peds` 0, `foot.*` |
| 3 | Each drives his own car together, then ride along both ways | 8 | `traffic.true_p95` <= 1.5, `traffic.shown` >= 95, `world.guest_own_cars` 0, `car.*` |
| 4 | Guest drives away from the host until the countdown, lets it run out; then again and turns back at the warning | 5 | `leash.pulls` >= 1, `leash.false_pulls` 0; screenshots of both banners |
| 5 | Host pauses for a minute while the guest is 350+ m away | 2 | no pull while paused (`leash.false_pulls` 0) |
| 6 | Guest commits a crime near the host; host cops chase him | 4 | no own police on the guest (`world.guest_own_cars` 0), PoliceChase lines |
| 7 | A chapter (TAXI) to its first fight | 12 | `npc.p95` <= 1.0, `npc.max` <= 4, `npc.guest_sent` 0 |
| 8 | Guest dies far from the host; guest leaves and rejoins | 5 | respawn beside the host, `join.complete_s` <= 10 |
| | everything | | `bw.host_up` <= 160, `bw.guest_up` <= 8, `lat.player.p95` <= 40 |

## 7. Decisions (defaults chosen, changeable)

| # | Decision | Default chosen | Alternatives |
|---|---|---|---|
| D1 | Leash distance | warn 300 m, max 400 m (host ini) | 250/350 (busier streets), 500/600 (more freedom, emptier) - `world_extent.lua` will show where the host's world ends |
| D2 | Past the leash | 10 s countdown, then back beside the host (with his car) | warning only and empty streets; his own unshared local world beyond the leash (his traffic back on, nobody else sees it, hidden again when he returns); a hard wall (not doable reliably in this engine) |
| D3 | Guest runs ahead in a mission | his own mission NPCs held hidden until the host's game spawns them (the host leads) | keep the 0.15 guest-owned NPC mirrors in missions only |
| D4 | Parked cars, trams, boats | local on every game this round | the host's, through new messages (later) |
| D5 | Pedestrian copy looks | rotation of six civilian profiles from spawnprofiles.sds | one look (0.16) |
| D6 | Whose leash numbers | the host's (LeashConfig) | each game's own ini |
| D7 | Version | stays 0.16.x on the branch; 0.17.0 at release | |

## 8. Effort

A 3-4 h, B 3 h, C 4-5 h, D 3-4 h, E 4-5 h in parallel (five agents): ~5 h; merge, integration and docs ~2-3 h; the
one-PC run ~1 h and the two-PC test ~45 min when game runs are allowed again. F later (6-8 h).

## 9. Status (2026-10-20 evening): built, one-PC check done, two PCs not yet

Branch `feat/host-authority` (from feat/host-authority-plan): A guest-world 649c75c, B host-traffic 332b62a, C host-peds
03d6b54 (with feat/sync-peds 652cbf9), D host-npcs 4dcecac, E leash c97dfcc, merged in the order A, B, D, E, C (C came
last), then the integration fixes. core stays at feat/foot-track 3e892eb (no core change, no tag). build.cmd OK, all 33
`out\*_test.exe` pass. F (NPC-driven mission cars) is not started.

Trace replays on the real 2026-10-20 traces (unit tests):

| Package | Replay | Result |
|---|---|---|
| B traffic | session A host cars -> guest copies (tests/data/traffic_replay.h) | true p95 0.75 m (unchanged; the 0.16 copy speed: 1.52 sim / 2.17 game) |
| C pedestrians | real PDT of A and B through 80/90 m interest, copies-only pool, spawner model | coverage A 82.2 % (recorded 18.2 %), B 91.7 % (82.1 %); p95 0.40 / 0.37 m (3.2 m); copy fails 0.55 / 0 per min (86) |
| D NPCs | session A guest's NPCs (1 guest-owned cop, made his own 3 times by 0.16; 1350 states sent) | held at 120.4 s, never sent; 3 paired NPCs never held |
| E leash | both players at 1 Hz, A and B | A: no warning (max 98 m); B: warning at 300 m, countdown at 402 m, one pull 10 s later at 587 m, no false pulls |
| A guest world | guest traces A and B | his own ambient humans 31 / 24 -> 0 in the tick 0.16 switched spawning off (so one of the calls clears them) |

One PC, two instances, UDP loopback (tools/onepc, 2026-10-20 14:36 and 15:00; games muted and unmuted, saves backed up
and byte-identical afterwards, "profile and save files changed by the run: 0"):
- guest world: own moving cars 0 and own ambient humans 0 in every band at 10 s and after a minute, budget read as
  150 and set to 0, nothing of ours hidden; `world.guest_own_cars` / `world.guest_own_peds` PASS 0.
- host world extent (the leash question D1): the host's cars and humans were all within ~300 m (humans: 0-50 m 1-6,
  50-100 m 31-36, 100-150 m 6-7, then a few to 300 m; cars 0-5 per band to 200 m). 400 m is beyond everything the
  host's game had; 300 m is where it thins out. The defaults stand.
- pedestrians on the guest: coverage 96.4 / 97.5 %, copy failures 0, six looks loaded, pool reuse 166 / 207;
  `ped.p95` 2.05 / 1.72 m (FAIL, target 1.5; mostly reused copies, max 59.9 m right after a reuse).
- traffic on the guest: true p95 1.32 / 1.41 m, shown 99.4-99.9 %, crew exits 0; `traffic.reseats_per_min` 0 in the
  first run, 6.4 in the second (the second had the leash teleports).
- NPCs: p95 0.08-0.09 m, `npc.guest_sent` 0.
- players (first run, no extra teleports): every `foot.*` PASS but `foot.respawn_s` 5.03 s (the simulated loss);
  `car.p95` 0.02 / 1.58 m (the guest drove into a wall). The second run's foot FAILs (dz p95 1.52, snaps 0.91/min,
  shown 95.4 %, one respawn) come from the scripted teleports of the leash checks (a 340 m move 30 m up, a fall onto
  a roof).
- leash: the first run had no leash at all (this engine's Lua reports a Lua function as type 9: fixed f26144f); then
  the warning banner at 340 m, the countdown at 426 m and the pull beside the host (1.5 m) after 10 s, "THE HOST
  PAUSED" and no pull while the host was in his pause menu, the pull after he resumed; `leash.pulls` 2,
  `leash.false_pulls` 0.
- bandwidth: host upload median 7.3-9.0 KB/s with one guest.
- The one-PC check of feat/sync-gaps first (13:55; the "AI walk" question): every foot metric PASS (p95 0.05-0.06 m)
  with the velocity matching; the game's own walk under the puppets measured 2.94 m/s at RUN x1.8 (guest's puppet on
  the host, 34 % of the way nudged) and 0.49 m/s (the host's puppet on the guest after the simulated losses, 82 %
  nudged): the AI walk does not reach ~4 m/s at x1.8 even without a SetPos every frame, the velocity matching covers
  the rest (with some foot sliding).

One PC again with the 0.17.0 release build (2026-10-20 21:57-22:03; the one-PC fixes of feat/onepc-fixes and the session
diagnostics of feat/session-logs merged; games muted and unmuted, saves byte-identical afterwards), `sync_compare
--check` on the two session bundles: PASS `ped.p95` 0.61 m (coverage 96.7 %), `foot.respawn_s` 1.66 s, `car.p95`
0.03 / 0.58 m, `traffic.reseats_per_min` 0, `traffic.shown` 99.9 %, `npc.p95` 0.01, `leash.pulls` 1 /
`leash.false_pulls` 0, `join.complete_s` 4.6, `session.mismatch` 0, every other foot target; FAIL `traffic.true_p95`
1.97 m (moving cars across their way, 90+ m p95 2.36 m; 1.32 / 1.41 m in the 14:36 / 15:00 runs, the car driving code
unchanged since), `foot.dz_p95` 0.75 m for the host's puppet on the guest (it stood on the copy of the host's parked
motorbike 0.75 m up after he got off, until the loss step respawned it 0.3 m below his spot beside the bike; he stood
still the whole time), `world.guest_own_cars` 1 (one own moving car within 150 m for 10 s, 124 s into the world, then
hidden). Bundles written at each game exit in 0.1-0.2 s (2.2 / 3.8 MB), `-recovered` bundles for the killed 15:00 games
at the next start; Lua tracebacks on.

Still to confirm on two PCs (section 6), plus: a reload / chapter with held NPCs (D), the car pull (E), whether
`SetMaxHumanElements(0)` also starves mission spawners in a chapter (A: the probe saw npc copies 0 in Free Ride, no
mission there), the ped.p95 of reused copies, the traffic put-backs after a teleport.

**The failed one-PC checks, looked into (branch `feat/onepc-fixes`, built and unit-tested, not run in game):**
- `ped.p95` 2.05 / 1.72: not the reuse. The copies (reused or spawned alike) were off their own target: run / sprint
  copies went 2.84 m/s against their pedestrians' 4.72 (medians; 37-43 % of those samples over 1.5 m), walking ones
  sometimes kept 1.5-1.9 m behind, standing ones walked off 2-4 m (the stop was a walk to the pedestrian's spot) or
  fell off a raised spot; the 59.9 m (and run 2's 123.6 m) were the host's game moving a pedestrian 65 m, the truth
  interpolated across it. Now the copies have the puppets' FootDriver and the stop walk of the puppets;
  sync_compare no longer interpolates a pedestrian's truth across a jump (2.05 -> 2.04, 1.72 -> 1.71: the rest needs a
  game run). Replay of every one-PC pedestrian (tests/ped_test.cpp, copies whose AI run is too slow): p95 0.11 / 0.13
  m, max 0.79 / 0.53 (the old driver in the same model 0.44 / 0.47, max 3.22 / 1.39 - the model does not reproduce
  the in-game 2.05: watch it).
- `foot.respawn_s` 5.03 / 5.10: 4.0 s waiting for a known profile on the stuck spawner, 1.0 s before the fresh one
  was obtained, 0.1 s to make it. Now 1.5 s (a working spawner had it in 0.00-0.1 s) and at once: ~1.6 s
  (tests/respawn_test.cpp).
- `traffic.reseats_per_min` 6.4: all 33 put-backs were of copies 81-272 m from the guest (31 of one standing copy,
  every second for 34 s): his game streams the copies' crews out beyond ~50 m as it does its own traffic's drivers.
  Beyond 40 m no put-back (kDriverFar in the Drive command; tests/traffic_test.cpp TestFarCrew), and a member who is
  out again right after each put-back is no longer counted from 0.
- `car.p95` 1.58: a real lag, not a spike. After the crash his car slid along the wall and stopped; the copy stood
  against the same wall beside it, was given 0.7-0.9 m/s into it and slid away, 1.0-1.8 m off for 2.4 s until the
  lag pin; those 3.5 s were 17 % of the 20 s he drove. CarDriver puts a copy that does not close in on a car slower
  than 1.5 m/s at it after 0.5 s; the replay of the crash (tests/player_motion_test.cpp): p95 1.61 -> 0.73 m, max
  1.73 -> 0.93.
- Foot after the scripted teleports: sync_compare leaves out 3 s after a player's own teleport (a jump of over 50 m,
  a leash pull: run 2 5 teleports, 9.6 s on foot) and the time he was over 300 m from the receiving player (no world
  there: run 1 136 s - the leash did not work in it - run 2 38 s); `foot.shown` 95.4 % was the guest's loading screen
  (10 s of the host's states before his world loaded), not a teleport. The puppet took a teleport with one snap (a
  few more in the next 0.1 s while the player himself slid 4 m after being put down) and no respawn; the one
  respawn came from the second pull: the guest was put down where his game had no ground yet, fell through the
  street and was downed (his puppet died with him and came back 10 s later, a logged reason now). Fixed: for 3 s
  after a pull he is put back on the spot when he sinks 1.5 m under it (leash.h Settle, tests/leash_test.cpp). With
  these rules run 2's dz p95 1.52 -> 0.01, snaps 0.91 -> 0/min, shown 95.4 -> 98.2 %, respawns 1 -> 0.
