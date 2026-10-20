# Changelog

Mod versions of MafiaCoop (Mafia: Definitive Edition co-op). Everyone in a session needs the same version. Supported
game: the Steam build 16502642 (exe CRC32 `BCE2C8EF`).

## Unreleased (toward 0.17.0), not yet verified in game

- Host authority, the guest's own world off (docs/host-authority.md work package A; **NOT verified in game**, built
  and unit-tested only): on a guest's game (never the host's) his own ambient traffic and police spawning are off and
  his pedestrian budget is 0 for the whole session, switched on the first tick of every world (the loading screen) and
  again every 5 s; the cars and humans his game had already made are hidden at once behind the loading screen and in
  the first 3 s, later only while out of view; nothing of ours is ever hidden (puppets, the puppet factory, traffic
  copies and crews, ped and npc copies and their spawners, player cars - the 0.16 traffic hide took ped_sync's copy
  spawner along). Everything comes back when the session ends: spawning on, the budget to the value read before
  (from the traffic manager, found statically), the hidden cars and humans shown. Fallback when the budget call fails
  (or `guest_world_peds = block`): the spawners around the guest blocked every 20 m. Trace `GW`, sync_compare
  `world.guest_own_cars` / `world.guest_own_peds` (target 0), tools/onepc `suppress_probe.lua` / `world_extent.lua`.
- Update check (core v0.13.0): once per game start the mod asks https://justaddmp.com/api/version/mafia-de for the
  newest MafiaCoop release (the newest GitHub release of this repo, pre-releases included). When it is newer than
  this build, a banner at the main menu says so for 20 s ("UPDATE AVAILABLE: MafiaCoop v0.17.0 is out (you have
  v0.16.0). Get it at justaddmp.com") and CO-OP SETTINGS shows "UPDATE AVAILABLE: V0.17.0 AT JUSTADDMP.COM". The
  request runs on its own thread with 5 s timeouts; offline or any other failure only writes a log line
  (`update check: ...`). Checked against the live endpoint from a test harness (as 0.15.0: v0.16.0 found; as
  0.16.0: up to date); the banner in game is not yet seen.
- Core v0.13.0 (from v0.12.0): the update check; no other change, protocol stays 9.
- Fixes from the 2026-10-20 two-PC test of 0.16.0 (docs/test-2026-10-20.md: "the guest is invisible to the host"):
  - **Motorbikes stopped all car and bike sync** ("cars error" a hundred times a second, every car of that player
    unshared for the rest of the session, the rider shown as a man running 0.7 m under him and snapped up again and
    again): a bike has no GetGUID; it now gets a number of its own, and only counts while its seat names the rider.
    Seen working on one PC with the fix loaded into the running games (the other game showed the bike with the
    rider's puppet on it, off and on again); not yet on two PCs or with a garage bike.
  - **The other player vanished and never came back**: when the game removed a puppet (the Free Ride garage clears
    its door when a player takes a vehicle out), the guest's spawner hung and, after the fallback timed out too, the
    puppet was given up for good. Now it is spawned again at once, a spawner that hangs on a profile it loaded before
    is replaced after 4 s, and a puppet is never given up. Tried on one PC with a simulated loss; not yet on two PCs.
  - The pedestrian copy spawner (and any fixed car) is no longer shared as traffic (a crewed car 40 m under the
    other player). Not run in game.
  - A guest's "world complete" time starts when his world has loaded, not when he joined in the menu. Not run in
    game.
  - sync_compare: `foot.shown` (the other player's puppet or car copy there while his states arrive),
    `foot.respawns`, `foot.respawn_s`, `foot.dz_p95` (a puppet under the floor). On the 2026-10-20 traces the host was
    there on the guest's game 19.5-20.6 % of the time.
  - tools/onepc: a scripted one-PC run (two instances) with screenshots and the expected `--check` results; not run
    yet.

## 0.16.0 (2026-10-20), pre-release, not yet verified in game

The fixes for the problems found in the 2026-10-18 one-PC test and the 2026-10-19 two-PC test, and a rewrite of how
the games stay in step (docs/sync-architecture.md, core v0.12.0): other players, their cars, NPCs, pedestrians,
traffic and the time of day. Everything here was built from reading the game's code and the logs and traces of
those tests, with automated tests where the logic runs without the game. **None of what is new in 0.16.0 has been
played on two PCs yet.** The next test follows README-dev.md "Test checklist: 0.16.0" and "Sync architecture
checks" (`tools/sync_compare.py <host trace> <guest trace> --check`).

Verified in the 2026-10-19 two-PC test, with the 0.15.0 build (over Steam, Free Ride; 0.16.0 changes several of
these - movement, cars, traffic, NPCs - so they need checking again): Steam invite and join over Steam P2P, the
lobby, HOST LEFT and hosting again; movement at about 28 ms round trip; the horn on a car copy; riding along in a
friend's car both ways; the host's traffic on the guest's game (basic); car damage sync (crashes, shot tyres, the
exploded flag); the host's police chasing a wanted guest without making the host wanted; enemies only the guest's
game spawned shown on the host; Free Ride death and respawn; punches landing; a real gamepad in the co-op panels;
a clean leave from either side.

Written, not yet verified in game: everything below.

Fixes for the known issues of the 2026-10-18 test (round 3), all unverified in game:
- Co-op sessions no longer change the player's garage: garage.pf, the 2K play stats and the persistent script data are
  kept in the session's scratch copy like the save slots, and stay out of the real profile until the game restarts
  (the game keeps the garage in memory). Settings (controls, audio, video, language, map filter) are still the
  player's own.
- Police: a guest's own wanted level no longer runs out after ~20 s while the host's police chase him. His zone's
  decay is held while host cops are on him (a red zone follows him while one sees him or is close) and released when
  the chase ends, so he still has to lose them. A zone that decays or decayed during the chase is started again (not
  after a fine, an arrest or his death).
- Map markers: the host's marker was missing on a guest's minimap. The puppet's friend brain switch, about 2 s after
  it spawned, removed the icon on it. Now the icon waits at the player's position until the switch is done, and icons
  on puppets are checked once a second and made again if the game dropped them.
- Seats: after a reload, when both players sit in their own mission car at the same spot (Taxi cp_020), the other
  player's puppet is no longer hidden for minutes. His car's copy waits 2 s, then stands beside his car with him
  seated in it, and moves onto his car once the cars have parted. A parked copy of his car from before a reload or
  rejoin no longer blocks the new one, and car records are looked up again after every load.
- Remote players' punches, throws and other actions (if one of the ways works): the puppet's animation is now played
  through the game's direct animation call first (PlayAnimation), then the other ways the game offers (no-transition
  flag, DesirePlayAnimation, script control of the puppet's AI, with combat mode, plain AnimPlay). Each try is checked
  by watching the puppet's hands, and the first way that moves them is kept (log `ANIM: ... SHOW with ...`).
- A copy of another player's car now looks like his car: the same body variant (its tuning items), both colours, the
  plate, window tint, interior and painting (new CarLook message 0x0253; traffic copies too). Car damage can then sync
  the copy's parts as well.
- Car damage after an explosion is no longer undone: a copy never reports its wreck or the game's reaction to damage
  we set on it, and the holder never reads a wreck's change as a repair.
- A car look no longer rebuilds a copy someone sits in (unverified in game): installing its tuning rebuilds the car,
  so a remote player's puppet is seated in a new copy only once its look is on, and a tuning change that arrives
  while he sits in it waits until the copy is empty (colours, plate and the rest are set at once). After a rebuild
  car damage asks for the car's whole damage again and does not report the rebuild's part change.
- Script control on puppets (unverified in game): it is no longer asked before the friend brain switch is done,
  is given back before a car seat, a walk to a car door or a fall, is cleared when the puppet is re-created or
  respawns, and a late answer for an action that is already over is ignored.

Fixed from the first two-PC test (2026-10-19, round 4), all unverified in game:
- Car copies drive at their real speed: SetSpeed takes km/h (read from the exe); copies got m/s and ran at 0.28 of
  their driver's speed, pinned most of the time. Every caller is fixed (player cars, traffic), reversing gives a
  negative speed, and GetSpeedFloat is read as km/h too. The SetSpeed probe logs once instead of a traceback per copy.
- One wheel, one driver: when the game slides a passenger to the wheel because the driver's puppet left it, that is no
  longer a takeover; when both claim the wheel, the car's holder keeps it (else the lower id) and the other slides to
  a passenger seat or gets out. Car damage follows the single driver. On a motorbike the one who gives way counts as
  the rider behind.
- Traffic copies no longer lose their drivers while driving: a host car only counts as driverless after 1 s without a
  driver while standing, a copy's driver only gets out when both cars stand, and crews found outside their copy are
  seated again.
- Traffic crews use this game's civilian and police spawn profiles instead of Tommy (traffic_base_ambient_combined,
  police_scotch).
- Guest-owned police and other NPCs without a spawn profile are shown on the host with a police or civilian fallback
  profile.
- Host cops chasing in cars are no longer paired with, and pulled to, cops on foot on the guest's game.
- CAR_HAVE_PROBLEM is read from the car (the message has no getter): no more error line, and car explosions are
  detected.
- Changing character no longer replaces your character on the other games for every step through the list: your pick
  goes out once it has stayed for a second or when you close the panel. The old character goes properly (out of its
  car seat, hidden, deactivated, removed) and the new one takes its seat at once; a left-over old character is never
  taken for an NPC, and the police turn to the new one at once.
- Players can shoot each other: the friend brain made the game ignore our bullets on puppets. Each shot is now checked
  against the game's own damage, its bullet-hit message, and a ray test of the aim against each puppet (walls block);
  a shielded puppet (swimming, falling, burning) is hit by the ray and bullet-hit checks alone. Punches lock on to a
  puppet in front of you (you turn to it; reach 2.3 m). Friendly fire OFF turns both off.
- Motorbikes are vehicles: the rider is seen on his bike (a pinned copy, the puppet on it or on its saddle); no more
  sprinting puppet or getting on and off as jumps. A player moving faster than 12 m/s while reported on foot has his
  puppet hidden.
- Water: swimming and diving are sent (pose bits 8 / 16) and shown (the puppet at his position with swim strokes).
  Puppets never die from local causes (drowning, falls, fire, explosion pictures, a sinking or exploding car); only a
  network death counts. A sunk car copy's engine is left to the game.

Added (unverified in game):
- A paused game says so (new message PeerPaused 0x0103): the others hold that player's puppet, his car and (host) his
  traffic copies, and report no knocks, pull back no NPCs and start no driverless-car or NPC-pairing decisions until
  1 s after the resume.

Sync architecture (2026-10-20; docs/sync-architecture.md; core v0.12.0's replication layer). Built from the
2026-10-19 two-PC traces and unit tests with simulated copies (the game's AI walk, car physics and civilians are
modelled, not run). **None of it is verified in game**; the next two-PC test checks it with
`tools/sync_compare.py <host trace> <guest trace> --check` (README-dev.md "Sync architecture checks"):
- Puppets walk with the gait their player's game reports (the gait matched only 63-76 % of the time), speed changes
  reach them at once, small errors are corrected inside the motion (from 0.3 m, was 0.9 m), a standing puppet is slid
  to its player's spot slowly instead of shuffling there, and one that runs on after its player stopped is held back
  instead of walking back. Remote players and their cars are shown where they are now, including the network trip.
  Simulated: p95 0.65-0.75 m (measured in the test: 2.0-2.3 m), standing 0.25-0.27 m, gait 99.8 %.
- Car copies drive on their own physics most of the time: nothing is set within 0.5 m, the speed is given along the
  copy's heading, a copy can get the driver's speed without being moved, slides are followed, and a pinned copy gets
  its physics back after 1.5 s (was 5-40 s). Simulated: p95 0.88-0.98 m, pinned 0-0.1 % (measured: pinned 56-74 %).
- A player who joins late or reconnects sees the cars parked before he came (new message ParkedCars 0x0104).
- NPCs: one authority per NPC. The host's NPCs are no longer run by a second AI on the guest's game (the host's cops
  chasing the guest were 60 m apart at p95): the guest's own NPC has its AI stopped and is driven from the host's
  states (interpolated, the puppets' walk controller and correction); one that does not follow is hidden and a copy
  shows it. Its shots there are pictures, the host's NPC's real hits come as Hits. NPC states are compact,
  time-stamped and sent per guest by distance (NpcStates2 / NpcInfo / NpcLeave 0x0212-0x0214, a guest's own NPCs
  0x0424-0x0426; NpcStates 0x0210 and GuestNpcStates 0x0420 are retired): about 6 KB/s per guest at 48 NPCs (was
  ~21). Simulated on two of the test's cops: p95 0.97 m.
- Pedestrians are shared (only ~2 % of the street was the same): the game that simulates the ambient world in a place
  (the host within 250 m of him, elsewhere the nearest player) sends its pedestrians near each other player
  (0x0280-0x0284, ~5.5 KB/s for 32). The others turn their own pedestrians there into replicas of them (one-to-one,
  nearest first), spawn copies for missing ones (up to 10, a generic civilian look) and hide their extra ones while
  nobody looks. Deaths follow the authority; a pedestrian you kill dies on every game.
- Traffic is shared by region: every player's game shares the traffic it spawned with every player near it and shows
  everyone else's (a guest far from the host, and two guests together, now see each other's cars; 0x0224-0x0227
  through the host's relay). Near the host a guest's own spawning goes off; his cars already there keep driving and
  are shared until out of everyone's sight, so nothing pops. Far copies are no longer shown metres behind their cars.
  Simulated: true error p95 0.9-1.3 m.
- Time of day follows the host without visible jumps (it was corrected only beyond 3 game minutes): WorldEnv carries
  the host's sample time and goes to a joining or reconnecting player at once and every 2 s; small drifts are closed
  by running the clock up to 25 % faster or slower for a few seconds.
- A guest's log (and the host's) says when his world is complete after a join or a reconnect (JoinReport 0x0207).
- Measuring: truth records on the game that owns a thing and replica records where it is shown (NPT / NPR, TRS / TRC,
  PDT / PDR, WEH / WEG, JN and more, docs/sync-trace.md), and `sync_compare.py --check` prints every target of
  docs/sync-architecture.md section 4 as PASS / FAIL.

Known issues:
- Not yet tested in game on two PCs: all of 0.16.0 (the round 3 and round 4 fixes above and the whole sync
  rewrite). Simulated numbers are from unit tests that model the game's AI walk, car physics and civilians; the game
  may behave differently. Please send both PCs' `logs` folders after playing.
- A player sitting in a car cannot be shot by the other players (shots on a seated puppet are not counted, also
  through an open window).
- After a player gets off a motorbike, the bike's copy may stay on the other screens for up to a second.
- Car damage and car look are not shared for a guest's own traffic (the host's traffic and players' cars have them).
- Pedestrian copies spawned for missing ones use a generic civilian look.
- Not reached in any test yet: JOIN FRIEND between two PCs, chapters on two PCs, reconnect over Steam, doors, item
  takes, grenades, seats after a reload, three or four players.

Protocol: `modVersion` 0.16.0 (session build 1600; core v0.12.0, its `kProtocolVersion` stays 9). New messages:
PeerPaused 0x0103, ParkedCars 0x0104, JoinReport 0x0207, NpcStates2 / NpcInfo / NpcLeave 0x0212-0x0214,
TrafficSpawn2 / Despawn2 / States2 / Bump2 0x0224-0x0227, GuestNpcStates2 / Info / Leave 0x0424-0x0426, CarLook
0x0253 and the pedestrian messages 0x0280-0x0284; NpcStates 0x0210 and GuestNpcStates 0x0420 are retired; WorldEnv
gets an optional trailing sample time; the pose bits 8 / 16 / 32 use the existing PlayerState pose byte.

Everyone needs 0.16.0: a 0.15.0 game cannot join a 0.16.0 host (or the other way round); it is turned away with
"version mismatch: ... Both players need the same MafiaCoop version." (docs/protocol.md).

## 0.15.0 (2026-10-18), pre-release, mostly not yet verified in game

A big step for the shared world: fights, doors, items, explosions and car details you can see on the other screen,
the host's traffic and police on a friend's game, enemies only a friend's game has shown to the host, failed
missions in one load, every player on the map, reconnecting after a lost connection, three or four players, lower
latency, a JOIN FRIEND list and real gamepad support in the co-op panels. Everything here was built from reading the
game's code and the logs of earlier tests, with automated tests where the logic can run without the game.

A first in-game test ran on 2026-10-18 on one PC (two instances, Free Ride, CHAPTER 2: TAXI and CHAPTER 13:
SALIERI). **Verified in game:** car signals, failed missions in one load (the host's and the guest's failure), and
the police fix below. **Everything else is written but not yet verified in game**, and some of it showed problems
(see Known issues). The next checks are in README-dev.md "Test checklist: 0.15.0 features".

Verified in game:
- Car signals: headlights, indicators, horn, siren and police beacon of the car another player drives show on your
  screen (tunnel headlights on and off, indicators, horn, siren and beacon, each once, nothing flickered). A real
  police car was not driven.
- A failed mission takes one loading screen instead of two: no more short stop in a Free Ride world. When the host
  fails, everyone goes back to his checkpoint with him; when a friend fails, the host decides whether he rejoins at
  the host's checkpoint at once or waits for the host. Checked with a blown-up mission taxi in Taxi, by the host and
  by the guest. Both failing at once and the other fail kinds (timer, ally dead, target escaped, arrested) were not
  tried.
- Police: a wanted friend's character no longer makes the host wanted. In the test the offence announced from the
  friend's character made the host's own game create a police zone around it; now no offence is announced, the
  host's police simply go after a hunted friend's character (retested: the host stayed clean).

Written, not yet verified in game:

Fights and moving around:
- Punches, charged punches, counters, takedowns, throws (grenades, molotovs, knives) and being hit in a fist fight
  are sent to the other players' characters with the game's own animations; the character stops walking while it
  plays and its swing cannot hurt anyone on your screen (the real hit still counts on the victim's game). In the
  test the punches arrived and the real hit landed, but the punch itself did not show (Known issues).
- Jumps, vaults, ledge climbs, drops, ladders, cover slides, sneaking, the medkit and carrying bodies are shown on
  the other players' characters.
- Enemies that only a friend's game spawned (he got somewhere first) show on the host's game: they move, fight and
  can be hurt and killed there, their hits on the host count, and when the host's own game spawns the same enemy
  later the two are matched up instead of doubled.

Cars:
- The other player's character stays in its car seat: it gets the game's "friend" behaviour on every game (the
  civilian one made it climb out, the cause of the throw-outs in the 0.13.0 test), its seat is checked every moment
  and it is put straight back if it starts to leave. Riding along with a friend waits until his character sits at
  the wheel, so you are not put in the driver's seat of his car.
- A mission car (or the host's own car) that was replaced by a copy while another player drove it comes back when
  the driver parks and gets out, where he left it (and wrecked if he wrecked it). The copy is locked meanwhile.
- Traffic and police: near the host (250 m), a friend's game shows the host's traffic and police cars (the nearest
  20, driven like the host's, police with siren and beacon) instead of its own; away from the host each game keeps
  its own traffic.
- Car damage: dents, broken parts, tyres, fire and explosions of cars show on every game; the game that holds a car
  decides its damage.
- Explosions: grenades, molotovs, dynamite, exploding barrels and blown-up cars are sent to every game and shown
  there as a picture that hurts no one; the real damage stays on the game where it happened. A grenade's or
  molotov's flight is drawn on the other games (one Throw message plus a few corrections at bounces).

The world:
- Doors are the same on every game: a door a player opens, closes, kicks open, unlocks or lockpicks changes on the
  other games too, and his character plays the kick, lock pull or door opening. The host's game decides the story
  doors.
- Weapons lying on the ground and ground pickups (medkits) are shared; the host decides who takes them.
  Collectibles are counted per player.
- Police: each player's wanted level is shared; the host's police go after a wanted friend's character near the
  host, and the friend's game holds his level while they chase him. Police copies' sirens come from the host's cars.

Map:
- Every other player is on your minimap and map all the time, with his name: on his character on foot, at his car
  while he drives, and at his position while his character is hidden, respawning or not spawned yet.

Connection and players:
- Reconnect: when a player's connection drops he is not thrown out. His game keeps running and rejoins the host on
  its own for up to a minute ("CONNECTION LOST - RECONNECTING... 12 S"), the others see "<NAME> LOST THE CONNECTION -
  WAITING FOR THE PLAYER TO RECONNECT" and he gets his place back (same player, same character, the host's world
  re-sent). LEAVE CO-OP stops the retries.
- Three or four players: other guests' enemies are relayed, enemies can hit any player's character, seat rules for
  three or four in one car, and conflicts when two players claim the same car's wheel are settled.
- Lower latency (core 0.11.0): messages leave at the end of the frame they were made in, lost ones are resent after
  about one round trip, the systems that do not depend on each other (world / mission, doors, items, police) have
  their own lanes so a big message no longer holds the others back, and bulk data gives way when the link is full.
  Other players' characters and traffic copies are shown with an adaptive delay instead of a fixed one.
- Latency stats: the time a guest waits in the menus or on a loading screen (a world message received in the menu
  and applied once the world loaded) is no longer counted as latency; it is listed apart (`LAT held=1`).
- JOIN FRIEND (main menu CO-OP) opens a list of your Steam friends who play Mafia DE and what they are doing:
  hosting (with players / maximum and whether they are in the lobby or playing), in a friend's game, not hosting,
  another mod version, full. Pick one to join. The version is checked before connecting (Steam invites too), and
  Steam's join errors are said in words. (The panel opened and refreshed in the test; listing and joining a friend
  needs two PCs.)

Co-op panels and the gamepad:
- A physical gamepad works in the co-op panels: any controller slot, plugged in or out while playing, sticks that
  do not drift or jump two rows, left / right on the sticks, a button held while a panel opens is ignored, B / START
  / BACK go back. An unplugged pad no longer blocks the game's input.
- The panels' footers show the right buttons for what you use, like the game's own menus: gamepad buttons after
  you use the pad, key names after keys or the mouse. The pad's X / Y / LB / RB do what the footer says (lobby: X
  START / READY / JOIN GAME, Y INVITE; JOIN FRIEND: Y REFRESH; text boxes: X delete, Y paste).
- Fix from the test: after a pad button closed a co-op panel, the keyboard no longer stays blocked while the game is
  not reading the pad (the loading screen after the lobby ignored Enter for minutes).

Known issues (from the 2026-10-18 test):
- Melee: the punch, charged punch and throw animations do not show on the other player's character (it keeps
  walking or standing; the real hit still lands). The game's melee animations seem not to play on an AI character
  outside a fight, and any animation played this way starts about 2 s late. Needs another way to play them.
- Map markers: the logs show one marker per player at all times, but no marker for the other player could be found
  on the minimap.
- A player's car looks different on the other screen: same model, but another variant and colour (and plate);
  player cars carry no colours or plate yet (traffic cars do).
- Car damage after an explosion: the wreck damage of the copy came back to the host's game as a report, and the
  host's game took the lower numbers as a repair.
- Seats after a guest's reload: after the guest's game reloaded (following the host to a new checkpoint), the host
  saw the guest's character hidden in his taxi, not seated, for minutes.
- Police: a wanted guest's level runs down on his own game even while the host's police are after him (holding
  his level during the chase does not work yet).
- `garage.pf` (the player's garage, a profile file) is written by the host's and the guest's game during a co-op
  chapter, so a co-op chapter can change your garage. Your story saves stay untouched.
- Not reached in the test: doors, world items (Free Ride has no item both games share), guest-owned enemies, the
  mission car coming back after parking, three or four players, two PCs, a real gamepad, JOIN FRIEND with a friend.
- Once in CHAPTER 13 the guest's scripts stopped about 10 s after the opening cutscene (no error in the log; not
  seen again).

Technical notes:
- Melee: PlayerState action block (u8 actSeq, u8 act) at the end, after the vehicle block's signals byte; the puppet
  plays `AnimPlay` with the controller paused, a swing shield keeps its swing harmless (docs/game-api.md 17).
  Unit test player_actions_test. Still missing: grenade explosions on the puppet, lockpick, stalk, paired takedowns.
- Traversal: action kinds 7..17 and an optional pose block (u8 [+ u32]) after the action block (docs/game-api.md 17;
  trace TE / TL / TP / TVC / CB).
- Car signals: read from the vehicle part (+0xD00 / +0xA88 / +0x119C; `mc_car_signals` / `mc_car_set_signals`),
  u8 signals at the end of the PlayerState vehicle block, re-set on the copy when its game changes them back; trace
  field `sig` (T field 64). Unit test car_signals.
- Seats: friend brain on every puppet where the game-sectors guard is in, seat checked every tick (`GetSeatOwner`)
  and put back, ride along puts the driver's puppet at the wheel first and tries `SlideToSeat` (docs/game-api.md
  18). `enemies_target_guests = 0` no longer sends the host's enemies' hits.
- Mission car back: the swapped-car machine `src/game/car_swap.h` (unit test car_swap_test); CarParked carries a
  flags byte (bit 0 destroyed); trace records SWP / SWR.
- Guest-owned NPCs: messages 0x0420-0x0423 (GuestNpcStates, GuestNpcEnd, HostNpcHit, NpcClaim), ownership rules in
  `src/game/npc_owner.h` (unit test npc_owner_test); traffic copy crews are never guest-owned; trace records NM / NO /
  NGO / NGE / NGC / NMC / NMX.
- Traffic: messages 0x0220-0x0223 (TrafficSpawn, TrafficDespawn, TrafficStates, TrafficBump), `traffic_model.h` /
  `traffic_sync.*` (unit test traffic_test); the guest's own ambient traffic and police spawning go off within
  250 m of the host; trace records TRH / TRP / TRG / TRC / TRL; hidden traffic cars are never adopted as a player's
  car.
- Doors: DoorEvent / DoorApply / DoorSnapshot / DoorSnapRequest (0x0230-0x0233), `door_model.h` (unit test
  door_test); action kinds 18 door kick, 19 lock pull, 20 door open (docs/game-api.md 21; trace DR / DL / DA).
- World items: ItemStates / ItemTake / ItemGone / Collectible (0x0270-0x0273), `world_items.h` (unit test
  world_items_test; trace ITH / ITG / ITE / ITL / COL).
- Police: PoliceState / PoliceEvent / PoliceChase (0x0260-0x0262), `police_model.h` (unit test police_test); no
  offence is announced from a guest's puppet (it made the host wanted); cops are set on a hunted guest's puppet at
  any level (trace POL / POE / POR / POT / POC).
- Explosions and the grenade arc: Explosion / CarExploded (0x0240-0x0241), Throw / ThrowFix (0x0242-0x0243;
  receivers simulate the flight with `lua::RayCast`), a damage guard for the pictures; a car blows up only on the
  game that holds its damage, another game's blast is applied there (docs/game-api.md 23).
- Car damage: CarDamage / CarDamageReport / CarDamageRequest (0x0250-0x0252), native part / tyre / fire / explosion
  reads and writes, raise-only merge with repair detection (unit test car_damage_test; docs/game-api.md 24; trace
  CDS / CDT / CDR / CDA / CDX / CDQ).
- Failed missions: the save system's FreeRoamRetry (exe+0x21E8B20) is skipped in co-op chapters, FailFlow
  (`src/game/fail_flow.h`, unit test fail_flow_test), the director's GameOver hooked (log), MissionFail 0x0205 /
  FailVerdict 0x0206; 75 patterns; trace record FAIL. No fail role while a guest is reconnecting.
- Map markers: one navigation icon per player, moved between puppet and position (`ChangeIconTargetToPos` /
  `ChangeIconTargetToEntityDirect`), names with `SetIconTextByNavId`; decision in `src/game/map_markers.h` (unit
  test map_markers_test); trace records MK / MKE.
- Reconnect: core's slot hold, resume token and PeerDropped; newcomer re-send keyed by (id, joinSeq) in PlayerSync
  and MissionSync; the trace stays open (RCN records, listed by tools/sync_compare.py). No app message changed.
- Several guests: HostNpcHit gets a trailing u8 attacker; NpcStates carry the other guests' own NPCs under relay
  keys (the owner's human id with its type byte replaced by 0xF0 + owner, exact in the game's float Lua); NpcStates
  taken in turns from each player's nearest (docs/protocol.md "Several guests"; trace NRL / NRH / SEA / DRV).
- Latency: `src/game/latency.cpp` sets core's lanes and send priorities per system; player states are sent in the
  frame the sample is first seen, stamped with the sample time and shown with core's adaptive playout delay on a
  Hermite curve; traffic copies use an adaptive delay instead of a fixed 120 ms; doors / items / police / car damage
  / NPC states polled at higher rates. Trace LAT (send -> applied per system, `held=1` for a message that waited for
  the game's tick) and CLK; `tools/sync_compare.py` Bandwidth and Latency sections (unit test latency_test).
- Core 0.11.0 (`external/core` v0.11.0): JOIN FRIEND (`net/steam_friends`), pad navigation (`pad_nav`, `PadHold`),
  button prompts (`ui_prompts`), reconnect, lanes, send priorities, session clock, playout; protocol version 9.
  The main menu's CO-OP page keeps the game's own footer.

Everyone needs 0.15.0: the PlayerState tail, the new messages and the protocol version (9) changed (0.14.0 cannot
join it).

## 0.14.0 (2026-10-17), pre-release, fixes not yet tested in game

Fixes from the first test of 0.13.0 on two PCs (Steam invite, Free Ride, about five minutes), and a sync trace on
every session so the next tests can be measured instead of guessed. The fixes are built from those logs; not yet
tried in game.

Cars:
- Car copies stood still under their own physics (0.2 m/s while the driver did 4.4 m/s) and fell back to being
  moved every tick ("pinned") within seconds, on both games. Likely causes, all handled now: the copy's engine was
  off (a car spawned from traffic stands with it off; it is started and kept running), and moving the copy may stop
  its body (after every correction the copy is given the speed it should have). The log probes this three times per
  car ("probe: speed .. after SetPos .. after SetSpeed").
- A pinned copy gets its own physics back after 5 s (then 10, 20, 40 s after each new pin), so one bad moment no
  longer pins it for the rest of the drive. The log says why it was pinned (too slow / ran off / stays off).

On foot:
- Running and sprinting players were up to 3 m ahead of their character on the other screen: the game's AI sprint is
  only ~3.3 m/s. The character now runs up to 1.8x its normal speed (was 1.5x), learns a slow gait faster, and when
  even that is too slow the missing speed is made up by small steps along the way, so it keeps up instead of being
  put back every few seconds.

Getting into cars:
- The other player's character got into a car with the door animation but was put straight into the seat after
  1 s, cutting it short (a script setting meant to give it 4 s was read before it was set). It now has the 4 s.
- Every time a character is thrown out of a seat the log says how (car speed, how the car is being moved, physics
  state, who holds the seat) - it still happens, the cause is not known yet.

Logs:
- **Sync trace:** every session writes `logs\trace\mafiacoop-<date>-<time>.trace` (core 0.10.0): every state sent
  and received (60 a second), the clock and link to each player, what the motion controllers saw and decided for
  every puppet and car copy each tick, readbacks of what the game really did (20 a second: the controls, engine,
  gear and front wheel angle of each car copy and of our own car, each puppet's seat, physics and sprint state), the
  NPCs' distances on the guest, and a copy of the main log. About 250 MB an hour; the newest 20 are kept.
  docs/sync-trace.md.
- `python tools/sync_compare.py <trace from one PC> <trace from the other>` lines the two games up (clock offset from
  the states' timestamps and the round trip) and reports loss, latency, how far each character and car really was
  from its player at the same moment (per gait), gait matches, pins, steering and wheel angles, seats, NPC distances
  and the worst moments with wall-clock times. Replaces `trace_pos` and tools/pos_error.py.
- The last five logs are kept (`mafiacoop.log.1` .. `.5`), not just one.
- The F7 status shows a car copy's pins and speeds, and the speed a character wants and whether it is catching up.

Compatible with 0.13.0 (same messages); both players should still use 0.14.0 for the fixes and the traces.

## 0.13.0 (2026-10-17), pre-release, not yet tested in game

Other players look like players. Built from reading the game's code while the game could not be started: nothing
here has been tried in game yet (README-dev.md "Test checklist: animation and car controls").

On foot:
- The other players' characters no longer slide across the ground. Before, they were pulled toward the player's
  position every frame while their walk animation played at its own pace. Now they walk, run and sprint with the
  game's own animation at the player's speed: the speed of each gait is measured on the character and adjusted to
  match, the walk is re-aimed when he turns, and his position is only corrected when he is clearly off (gently,
  beyond 0.9 m) or put in place when far off or stuck.
- Starting, stopping and turning on the spot follow the player smoothly; sprinting shows the sprint (his sprint
  key), jumps and falls no longer count as being stuck.
- Getting into a car: the character walks with the player to the door and gets in with the game's animation (when
  the car stands), instead of vanishing and appearing in the seat.

Cars:
- A car another player drives shows his steering: its front wheels turn with his. His throttle, brake and handbrake
  are applied too (brake lights, engine), and the car is moved by its own physics with a small correction, so its
  wheels roll. If a car's physics does not keep up on your game it is moved exactly as before (logged).
- Not synced: lights, horn, siren and indicators (the game has no way to read them that we found).

Names:
- Name tags, the lobby and the panels show the Steam name. They said HOST / GUEST in tests because the test commands
  started the games with `--name Host` / `--name Guest`; those are gone from the docs. A `--name` from the command
  line is now for that run only: CO-OP SETTINGS > NAME shows (and saves) your own saved name, never the test name
  (core 0.9.0).

Under the hood: PlayerState version 4 (in the air / in cover flags, the driver's controls), core 0.9.0 (remote
motion controllers, unit-tested). Everyone needs 0.13.0 (0.12.x cannot join it).

## 0.12.0 (2026-10-16), pre-release

Fights:
- Enemies that only the host's game spawned now show on a friend's game. Each game spawns a fight's later waves when
  its own player gets there, so an enemy the host's game had brought in (the host walked into the kitchen, round the
  back) shot at a friend without being seen. Now the friend's game spawns a copy of it: the same character (the
  mission's own spawn profile) with the host's weapon, moving and firing as the host's enemy does. The friend can
  shoot and kill it, and the host's enemy dies with it; when the host kills it, the copy dies too. The copy runs no
  fight of its own: the host's enemy already shoots at the friend there, and those hits reach him as before (a
  copy's own bullet that still hits him is given back).
- When the friend's own game spawns that enemy after all (he walks there himself), his own takes over and the copy
  goes: no doubles. Copies go with a reload, during cutscenes, when the enemy gets into a car, is out of range (90 m)
  or has been dead for 12 s; at most 10 at a time.
- Characters standing at the same spot on both games are only matched when they are of the same kind (before, the
  host's kitchen machine gunner could be matched with the friend's kitchen rifleman standing there).
- Checked in Bon Appetit (lobby CHAPTER, two games on one PC, three sessions of 22-24 minutes): the kitchen machine
  gunner and rifleman, the yard and balcony gunners and the alley machine gunner that only the host's game had
  brought in appeared on the friend's game (15 copies, two of them forced in a test), stayed with the host's (0.0-0.3
  m apart standing, about a second behind when running) and fired when the host's fired; the friend killed five of
  them and the host's died with them, the host's kills removed them. The restaurant fight was cleared to its last
  enemy (the host circled round behind the four attackers, which then could be killed; one of them by the friend). No
  crash, with three failed-mission reloads and checkpoint follows.

Not finished: right after the cleared fight the chapter failed each time on the way to Salieri (the tests move by
small teleports, which the mission probably took as leaving the area), so Bon Appetit was not played past cp_050;
no second chapter's fight.

Everyone needs 0.12.0 (0.11.x cannot join it: the NPC messages changed).

## 0.11.0 (2026-10-16), pre-release

Fights:
- A friend can win a fight. His game's enemies were kept at 5000 health while the host's lived, so only the host
  could really kill them. Now their health follows the host's game, the friend's own bullets hurt and kill them on
  his game, and the host's enemy dies with them (his bullet is replayed on the host's game from the friend's
  character; when that misses, the kill is passed on anyway). Checked in Bon Appetit's restaurant fight with two
  games on one PC: 10 kills by the friend counted on the host, and with the host not shooting the friend cleared two
  attack waves on his own. The host's kills and the damage he deals show on the friend's game as before.
- Enemies match up again after a reload. After a friend reloaded a checkpoint mid-fight, his enemies were not
  matched with the host's any more (the fight had moved on, and only characters at the same spot were matched). Now
  an enemy that has moved is matched with the nearest one of the same kind within 40 m; enemies the host already
  killed die on the friend's game too. Checked: after a friend's reload in the fight every enemy (7 of 7, 8 of 8)
  matched again.
- When the host's mission fails or he dies and his checkpoint reloads, a friend at that checkpoint now reloads it
  too. Before, his game kept its own fight while the host's started over, and the host's enemies that came back shot
  at him unseen. Checked: the host died in the fight, the friend reloaded on his own and all 6 enemies matched.
- Passers-by that took no damage on a friend's game were characters the game keeps switched off; they are no longer
  matched with the host's.
- Why the restaurant enemies took no damage in the 0.10.0 test: the mission keeps the four attackers in front of the
  restaurant unhurtable until the player has circled round to them; the attack waves that come round the back can
  be killed. A friend whose game lets him kill one of those four also kills the host's.

Not finished: Bon Appetit's restaurant fight was not played to its end (the last part waits for a route the tests
cannot walk), and other chapters' fights were not tried. Each game spawns a fight's later waves when its own player
gets there, so stay together.

Everyone needs 0.11.0 (0.10.x cannot join it: the NPC messages changed).

## 0.10.0 (2026-10-16), pre-release

Story:
- A reloaded checkpoint starts where it should. The game's chapter loader always put the player at the Free Ride
  start (Salieri's bar), whatever the save said; most checkpoints move him themselves, but Omerta's second one does
  not, so after a failed mission the host stood at the bar and that checkpoint's leave-the-area check never armed.
  The mod now puts every player where his checkpoint save has him before the mission resumes. Checked: Omerta's
  second checkpoint failed and reloaded 8 times by the host (3 with a friend connected) and twice by a friend, Taxi
  twice each: always back at the checkpoint's spot, and Omerta's check failed the mission again from every reload.
- A failed mission needs one Enter less: the short Free Ride stop on the way back to the checkpoint is confirmed
  for you and stays black, so you only see the checkpoint's loading screen (about 20 s after the failure). The game
  reads that key only while its window has focus. (Going back in a single load was tried and is unsafe: the game
  then treated the checkpoint as your own story and its fail screen offered your real story save.)
- Losing the mission car fails the mission the same way and goes back to the checkpoint (checked in Taxi, host and
  friend). Failing by a mission timer was not reached in the tests.
- A friend whose own game plays a cutscene of an earlier checkpoint no longer has to watch it to the end while the
  host is already further on (seen in Bon Appetit: the 3-minute ambush film); he goes straight to the host's
  checkpoint.
- Fixed a crash of the host's game during the load after a failed mission (with a friend's character in the world;
  seen once in Omerta, guarded since; five more failures there ran without a crash).

Players:
- Name tags hide behind walls: the game's own ray cast checks the line from the camera to the other player's head.
- A killed or unplugged game is noticed after about 4.5 s (was 10 s): `net_timeout_s` (default 4) in MafiaCoop.ini,
  `net_steam_timeout_s` (6) over Steam. A game that is only busy is not dropped (core v0.8.0 keepalive thread).
- Fixed: a friend's name tag could stay hidden for the rest of the session after he was downed.
- Fixed: after several reloads in one game the mod found only a few of the world's cars (mission cars were not
  recognised).

Tested, not finished: a chapter's own shootout was reached in co-op (Bon Appetit's restaurant ambush): characters
matched up, enemies fought both players, the host's enemies hit the friend, deaths were shared; a friend's kill
counted on the host could not be checked there. In Bon Appetit the host sits at the wheel of the mission car with
Salieri beside him on a friend's screen (the second chapter after Taxi); the car being handed back when the host gets
out could not be checked (the mission keeps the doors locked).

## 0.9.1 (2026-10-16), pre-release

Fixes:
- A failed mission in a co-op CHAPTER (leaving the mission area, abandoning the objective) no longer loads the
  player's own story save. In these chapter loads the game answers a failed mission by loading "the free roam state
  from before the quest", which is a save of the player's own campaign (the reported case: the host's real Omerta
  story). The mod now keeps that load inside the session: the game gets a save of the co-op session instead (in
  practice the Free Ride start, never a save of the player's), and as soon as that world is up the host goes back to
  his last co-op checkpoint, a friend to the host's. Before, the host's takeover only started once the wrong world
  was up, and in that world his game even sent the story save on to the friends. Checked: 8 failed missions of the
  host (Taxi cp_010 five times, one of them with the story save already read earlier in that game and one with a
  friend connected; Omerta cp_020 three times) and 4 of a friend (Taxi three times, Omerta in a STORY game once):
  each time back at the checkpoint, no crash.
- A co-op CHAPTER (and a friend's co-op game) no longer rewrites the save slots' profile files (`aslot.pf`, the
  story slot's script data: its current chapter and weapons; seen: the story slot's Omerta became Taxi). They go to
  the co-op scratch folder like the saves. Checked: a two-player CHAPTER game with a failed mission on each side,
  every save and profile file byte-identical afterwards (only the window size file changed).
- No crash when FREE RIDE is started from the main menu after a co-op chapter in the same game (the chapter
  loader's save storage only knew the first slot it was used with).
- The host of a STORY game keeps the game's own "You abandoned the objective" screen (Load Last Checkpoint): that is
  his own campaign.

## 0.9.0 (2026-10-16), pre-release

Players stay in sync:
- The other player is shown where he really is. Measured on one PC: on foot his character was 2.5 m behind on average
  and up to 9 m off (the game's own walking for him starts late and is slower than a running player); now 0.3-0.5 m on
  average (95 % within 0.7 m), also right after a lobby start and a checkpoint reload. A car at 70 km/h: 0.1 m. The
  game sends its player 60 times a second (was 20) and shows the others 50 ms behind (was 100 ms), moved forward along
  their speed.
- Traffic statistics in the log every 5 s and on F7 (per player and per message); the network limits are much higher
  (`net_budget_kbps`, default 1 MB/s per player), though two players use only about 15-20 KB/s. Core v0.6.0.

Story and NPCs:
- The host's enemies now shoot at the other players too, and those hits hurt them on their own game (not twice when
  their own copy of that enemy already fights them). The game crash this caused before (in the game's own list of
  humans, exe+0x243A915) is guarded by the mod. `enemies_target_guests = 0` in MafiaCoop.ini turns it off.
- In a mission car the host now sits at the wheel on a friend's screen (Taxi), instead of standing next to it.
- No car copy is spawned while a loading screen is up (one spawned then had landed on a friend's own mission car).

Menus:
- Gamepad (d-pad, A, B, left/right on settings) and mouse (hover, click, wheel) checked in the CO-OP panel, the lobby
  and CO-OP SETTINGS, with a virtual pad and real window mouse messages; a physical gamepad was not tried.
- Tests can drive the panels from a file (core v0.7.0 `test_input`).

## 0.8.0 (2026-10-15), first public pre-release

Fixes:
- The host of a lobby CHAPTER session no longer has the chapter saved into his own save slots (it overwrote his Free
  Ride slot): his saves go to the co-op scratch folder like a guest's, and his new checkpoints still reach the
  guests. The host of a STORY session keeps saving his own story, as in single player.
- A player who joined at start-up (Steam invite, `--join`) no longer gets the lobby over the title screen. Before, a
  START from the host then loaded the game from behind the title and hung on a black screen. The lobby now opens
  once the main menu is really shown, and never over a loading screen.
- A friend put next to the host after a load or cutscene no longer lands on a car standing there: another spot
  beside or behind the host is used.

Release:
- Player zip (`MafiaCoop.exe`, `mafiacoop.dll`, `mafiacoop_proxy.dll`, README, licences) built by
  `tools\make_release.cmd`, which also runs every automated test; PDBs in a separate symbols zip.
- Optional "Start from Steam" mode (`MafiaCoop.exe --install` / `--uninstall`) tested with the real game started
  from Steam.

## 0.7.0 (2026-10-15)

- Fixed a crash when a following guest reloaded the host's checkpoint from the world.
- A guest's saves are kept out of his own save slots for the whole co-op session (story and Free Ride).
- Mission NPCs: the host is authoritative (states, deaths, health); guests pair the host's NPCs with their own and
  their hits on them are replayed on the host. Mission cars are not doubled.
- Deaths and failed missions in co-op chapters reload the checkpoint; guests are put next to the host.

## 0.6.0 (2026-10-15)

- Story in co-op: guests follow the host's chapter, checkpoint, objectives and cutscenes; lobby STORY and CHAPTER
  modes.

## 0.5.0 (2026-10-15)

- Pre-game lobby (characters, ready, mode, START for everyone), CO-OP SETTINGS, name tags over players, minimap
  markers, the CO-OP page refreshes itself, players are told at once when someone quits. Core v0.5.0.

## 0.4.0 (2026-10-15)

- Characters and outfits picked in the CO-OP panel and seen by everyone; punches between players; damage from shots
  and cars.

## Earlier (2026-10-13 to 2026-10-15)

- From-scratch rewrite: launcher, injection, own UDP transport and Steam lobbies, host/join from the game's own menus,
  players seen walking, running, armed and driving, shared cars and passengers, weather and time from the host,
  kick / ban / session password, game version check, optional version.dll proxy. The old v0.1.0 build (2026-10-08)
  was a different code base.
