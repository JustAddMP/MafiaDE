# Sync trace

Every co-op session writes a trace next to the log, for comparing the two games afterwards: what each game sent, what
the other received and when, what the motion controllers decided, and what the game really did (read back). The
main log stays readable; the trace is the high-rate record. Since 0.14.0 (core 0.10.0 `core/trace.h`). Always on, no
setting.

- **Where:** `logs\trace\<log name>-<date>-<time>.trace` in the mod's folder (next to `MafiaCoop.exe`), e.g.
  `logs\trace\mafiacoop-20261017-150912.trace`. One file per session (opened when the session becomes active, closed
  when it ends; a guest's reconnect keeps it open, `RCN` lines mark the gap). The newest 20 are kept; one file stops at 1.5 GiB (about six hours).
- **Size:** about 60-80 KB/s with one other player (~250 MB an hour). It is written twice a second, so a crash loses
  at most the last half second.
- **Also kept now:** the last five main logs (`mafiacoop.log.1` newest .. `.5`), not just one.
- **Comparing:** `python tools/sync_compare.py <trace from one PC> <trace from the other> [--csv DIR] [--worst N]`
  (standard library only). See "What the report says" below.

To report a test: zip the whole `logs` folder of both PCs (the traces are in `logs\trace`).

## Line format

`<t> <TYPE> <key=value ...>`: `t` is this machine's QueryPerformanceCounter clock in seconds (4 decimals), the same
clock the game's network timestamps (`sT`) are taken from. Vectors are `x,y,z` (metres, z up); speeds m/s. Lines of
one file are in time order (per writing thread). Every line of the main log is copied in while the trace is open
(`<t> 2026-10-17 15:09:12.013 [92912] INF text`), so the file holds the whole session.

**Lining up two machines:** a state's `sT` (sender clock, ms) is in the sender's `TX` and in the receiver's `RX`:
that pairs them exactly. The receiver's `off` (its clock minus the sender's, smallest seen, so it includes the
shortest one-way trip) and the round trip (`CLK rtt`) give the clock difference: `receiver - sender = off - rtt/2`.
The tool uses that to compare where a puppet stood on one game with where the player really was at the same moment
on the other.

## Records

| Type | Who | Rate | Fields |
|---|---|---|---|
| `H` | both | once | `trace=1 wall=<local date-time> qpc=<t> pid game mod core proto state role=host/guest local=<player id> hz=<state rate> interp=<ms> built` |
| `PL` | both | per player | `id model name=<rest of the line>` (also our own) |
| `PLX` | both | on leave | `id` |
| `TX` | both | each state sent (60 Hz) | `sT=<ms> p h v m=<0 idle 1 walk 2 run 3 sprint> f=<flags: 1 car, 2 crouch, 4 sprint, 8 aim, 16 reload, 32 in air, 64 cover> hp sh=<shots> w=<weapon> act=<action counter>:<latest kind: none light heavy counter takedown throw hitreact>`; in a car also `car=<key> mdl seat cp q cv ctl=<valid,steer,power,brake,handbrake,gear> sig=<car signals byte: 1 park, 2 head, 4 indicator A, 8 indicator B, 16 beacon, 32 siren, 64 horn, 128 valid; 0 unknown>` |
| `RX` | both | each state received | `from sT off=<s> ...` the same fields as `TX` |
| `RXD` | both | dropped state | `from sT last` (older than or equal to the newest) |
| `CLK` | both | 1 Hz per remote | `id off rtt=<ms> hz=<received rate> snaps=<buffered> newest=<ms ahead of now> recv drop last=<s since the last state>` |
| `PUP` | both | 60 Hz per remote on foot (plus every snap) | `id rT=<the remote time the target stands for, ms> tgt tv have=<copy position known> age=<s> cp=<puppet> ch=<its heading> err g=<gait 0-3> mult want meas carry base=<walk,run,sprint calibrated> fix=<0 none 1 nudge 2 snap> nudge re=<walk re-aimed> mv=<walk point> turn sm=<his mode> sf=<his flags>` |
| `VEH` | both | 60 Hz per remote driver (plus pins, retries, snaps) | `id key rT have tgt tv cp cv err rot=<deg> fix=<0 none 1 blend 2 pin 3 snap> pin why=<0 - 1 too slow 2 ran off 3 stays off> jp=<pinned now> rt=<physics retried now> pins tsp csp vel=<wanted copy velocity> pw ctl st br hb gear sig=<his car signals>` |
| `PAS` | both | 60 Hz per remote passenger | `id key rT seat cp` |
| `AL` | both | each action of ours | Lua: our player punched / countered / threw ... (docs/game-api.md 17): `kind=<1 light 2 heavy 3 counter 4 takedown 5 throw 6 hitreact> seq=<counter now> opp=<GUID of the human the game aimed it at, - none>` |
| `ACS` | both | each action of ours sent | `sT=<ms of the state that first carries it> kind=<name> seq` |
| `ACT` | both | each action of a remote player | his counter moved on the state his puppet is driven from: `id kind=<name> seq missed=<steps skipped>`; the puppet plays it this tick |
| `AP` | both | each action played | Lua, on the puppet: `id kind anim=<animation name, - none accepted> ok=<1 AnimPlay accepted> dur=<s the puppet is left alone at most>` |
| `AE` | both | each action ended | Lua: `id kind anim took=<s> done=<1 the animation reported done> why=<finished, time_up, next_action, he_is_in_a_car>`; the puppet walks again (its `PUP` lines show `have=0` while it played) |
| `LR` | both | 20 Hz | our player read back: `p d spr hp`; in a car `car seat cp cv rb=<steer wish,power,brake,handbrake,steer linearity,front wheel angle (rad)> eng gear spd fixed rpm pwi sig=<our car's signals as read back, -1 = not readable>` |
| `PR` | both | 20 Hz per puppet | `id st p d hid veh seat own=<car it sits in> seated ejects tries anim phys spr dead hp mv mult moves snaps sown=<who holds its seat: ours/free/other/?, - on foot> brain=<friend brain set> ai=<GetAIType> back=<put-backs into the seat> leaving=<out of its seat inside the car now> ejAll=<throw-outs this session>` (the last six since 0.15, docs/game-api.md 18) |
| `CR` | both | 20 Hz per driven car copy | `key drv adopted fix pin p v rb=<as LR> eng gear spd fixed rpm pwi cmd=<steer,power,brake,handbrake,gear given> steerIn=<SetSteer value> vel speedHow=<vector/number/none: how SetSpeed works> seat0=<puppet/none/other> locked sigw=<signals wanted (his)> sigch=<changes applied> sigre=<times set again after the copy's game changed them back>`; `sig` = the copy's signals read back |
| `SWP` | both | on change | swapped-car machine (src/game/car_swap.h): `key phase=<driven/parking/done> act=<none/restore/restore_here/restore_wrecked/forget> lock=<copy closed to us> bits=<car_swap.h Bit flags it was given> spd=<copy m/s> parked=<s since the CarParked, -1 none> why` |
| `SWR` | both | on event | a real car hidden behind a copy: `key ev=swap orig at why`, `key ev=restore how=<copy/here> from p active copy wrecked why`, `key ev=forget orig world` |
| `MK` | both | 1 Hz | Lua, the other players' map markers (game-api.md 13): `players puppet=<icons on a puppet> pos=<icons at a position> none=<players without an icon> ids=<player>:<nav id>/<human or pos icon>/<puppet or pos>,...` (`-` no icon) |
| `MKE` | both | each marker change | Lua: `id nav ev=<create/to_puppet/to_pos/detach/remove> icon=<human/pos> to=<puppet/pos> p=<position it was put at>` (moves at a position are not traced, see `MK`) |
| `NHS` | host | 2 Hz | `sending hits kills mirrors=<guests' own NPCs with a state> shown=<with a copy> lateKills=<our NPCs killed as dead mirrors>` |
| `NP` | guest | 2 Hz per host NPC in range | `key loc=<our id> copy ai car host=<pos> our=<pos> d=<m> hp=<host>/<ours> dead=<host>/<ours> age` |
| `NGS` | guest | 2 Hz | `live matched copies avg max kills pulls own=<our own NPCs> claims=<host claims carried out>` |
| `NM` | host | 2 Hz per mirror (a guest's own NPC) | `mid from key=<its id on the guest's game> loc=<our copy> st=<copy state> ai guest=<pos there> our=<copy pos> d=<m> hp=<guest>/<copy> dead=<guest>/<copy> age` |
| `NO` | guest | 2 Hz per own NPC | `key p hp dead ai` |
| `NGO` | guest | on change | `key owned=<1 ours now, 0 not any more>` (C++) |
| `NGE` | both | per GuestNpcEnd | `[from] key why=<0 died 1 handed back 2 gone 3 refused> host=<host NPC> hp byHost` (C++) |
| `NGC` | guest | per NpcClaim | `key host dead ours` (C++) |
| `NMC` | host | per claim sent | `mid to key host dead` (C++) |
| `NMX` | host | mirror end | `mid why host hp` (C++) |
| `TRH` | host | 2 Hz | traffic totals: `cars=<host traffic cars near guests> spawnMsgs stateMsgs despawnMsgs bytes=<all traffic bytes sent> bumps=<TrafficBump received>` |
| `TRP` | host | 2 Hz per guest | `id relevant=<host cars in his range> admitted=<cars he has copies of> spawned despawned states skipped=<unchanged, not sent> bytes deferred=<due but over the budget, this tick>` |
| `TRG` | guest | 2 Hz | `mode=<1 host traffic here, 0 our own> known live copies avg max=<copy vs target, m> pins snaps bumps states spawns despawns old=<states dropped as older>` |
| `TRC` | guest | 2 Hz per traffic copy | `net tgt tv cp cv err pin pins f=<state flags: 1 handbrake 2 siren 4 no driver 8 controls 16 reversing>` (`err` 1e9 before the first report) |
| `TRL` | guest | 2 Hz | `own=<our ambient spawning on> hidden=<our traffic cars hidden> near=<ours still driving near us>` |
| `RCN` | both | on reconnect events (docs/protocol.md "Reconnect") | `ev=dropped id` (a player lost his connection, slot held), `ev=lost why` (guest: we lost the host), `ev=sync-paused`, `ev=sync-resumed local`, `ev=back same=<1 same slot> local attempts` |
| `FAIL` | both | on a failed mission | `ev=freeroamretry reason role skip fresh` (the game's answer to a failure: skipped or not), `ev=gameover reason role fresh`, `ev=report from seq reason point verdict` (host: a guest's MissionFail and the answer), `ev=verdict kind seq reason` (guest), `ev=action kind seq reason` (what FailFlow decided: 0 SendHostFailed, 1 SelfReload, 2 SendReport, 3 Quit, 4 LoadHostPoint, 5 ReloadInWorld, 6 GameFallback), `ev=log <text>` (its state changes) |
| `XT` | both | each explosive throw of ours | `weapon=<the selected explosive, - unknown> p=<where we stood> throw=<its throw number, GT>` (docs/game-api.md 20) |
| `XG` | both | on event | `ev=player_grenade_explosion`: PLAYER_GRENADE_EXPLOSION came to our player |
| `XB` | both | on event | Lua: our grenade bounced (EXPLOSIVE_BOUNCE sound owned by us): `p` (points of its flight, not shown elsewhere) |
| `XC` | both | each explosion seen here | the detector's decision (src/game/explosion_model.h): `seq=<0 not sent> kind=<grenade/molotov/dynamite/object/car/unknown> flags=<1 fire, 2 an NPC's> p weapon owner=<us/npc/other/unknown: the sound's owner> npc obj=<crash object> reports=<HEAR_SOUND reports merged> send=<1 sent> why=<ours: the reason, or why not> throw=<the throw it was matched with, 0 none> since=<s from that throw: the fuse / flight, -1 none>` |
| `XO` | both | each Explosion sent | `seq kind flags p weapon npc obj throw` |
| `XR` | both | each Explosion received | `from seq kind flags p weapon npc obj throw` (duplicates and malformed ones are not traced) |
| `XV` | both | each received explosion handled | Lua: `from seq kind act=<shown/far/npc_here/failed> how=<script, or the error> d=<m from us> p fire` |
| `XD` | both | each guard verdict other than keep | `victim=<local/puppet/npc/copy> enemy=<us/npc/puppet/copy/none/other> dtype=<1 explosion, 8 fire> amount verdict=<undo/ours> zone=<own:seq / remote:from:seq> p` |
| `XU` | both | each NPC undo | Lua: `id act=<health: from to / died: could not be undone>` |
| `XCS` | both | each CarExploded sent | `what=<shared/traffic> key p` |
| `XCR` | both | each CarExploded received | `from what key p` |
| `XCV` | both | each CarExploded handled | Lua: `from what key act=<set_exploded/already/no_car/failed:...>` |
| `GA` | both | while our player lines up a throw, 5 Hz | `ev=<start/aim> weapon how=<what said we aim: hand (explosive in hand), input (aim input), msg (PLAYER_AIM_ENTER), joined by +> p=<standing> head hand bone=<1 a hand bone, 0 head + offset> cam dir yaw pitch land=<the aim model's landing, none:...> flight=<s to it>`; `ev=end lasted samples` when it stops (C++, throw_sync.cpp) |
| `GT` | both | each throw of ours | `ev=release seq weapon kind p aimed=<s aimed before, -1 none>` (WEAPON_THROWN); `ev=sent seq kind flags=<1 fire 2 measured on the projectile> o v yaw pitch g delay fuse weapon bytes` (the one Throw 0x0242) |
| `GS` | both | each throw of ours | Lua: the projectile search (0.3 s at most): `seq found=1 id type d=<m from the hand> after=<s after the release> scans p` or `found=0 scans seen=<projectile entities seen at most>` |
| `GX` | both | each ThrowFix | `dir=out seq t=<ms since the origin> flags=<1 bounce 2 settled 4 stop 8 turn 16 position only> p v rel bytes` (sent), `dir=bounce seq p entity` (our bounce sound while following), `dir=in from seq t flags p v` (received), `dir=applied from seq t flags ok=<0 late / unknown / exploded> jump=<m the marker slides>` |
| `GR` | both | each Throw received | `from seq kind flags o v speed yaw pitch g delay fuse weapon added` (its simulation starts) |
| `GV` | both | 2 Hz per flight shown | Lua: `from seq p src=<sim (flying) / roll / settled / boom (at the explosion's true position) / arc (no ray cast)> held age d=<m from the camera> screen=<1 on screen>` |
| `GE` | both | each throw over | `dir=out seq weapon why=<measured/model/no_projectile_no_model> sent entity samples after=<s release -> sent> speed pitch model_speed model_pitch g=<gravity measured, 0 not> o` (ours), `dir=boom seq fuse=<s release -> explosion> p measuring` (our explosion for it decided), `dir=in from seq why=explosion age bounces casts settled p=<simulated> boom=<true> off=<m between> named=<1 by seq>` / `why=<fuse/timeout/left> age bounces casts settled p` / `why=explosion_no_flight` (another game's) |
| `END` | both | at the end | `why` |

`rb` values come from the car's vehicle part (docs/re-notes.md 9); `eng gear spd fixed rpm pwi` from the script
getters (IsEngineOn, GetGear, GetSpeedFloat, IsCarPosFixed, GetEngineRotations, GetPowerInternal). A `-` means the
call failed on that car.

## What the report says

Per player as seen on the other game:

- **network:** states sent and received in that time, loss, states dropped as old, the clock relation, one-way
  latency (mean, median, p95, max) and jitter.
- **on foot:** the distance between the puppet and the player at the same moment (the player's own `TX` position at
  the receiver's time mapped to his clock), overall and per gait the player was in; the controller's own error
  (puppet vs network target); how often the puppet's gait matched his and the most common mismatches; snaps, nudges
  and how often it was catching up (`carry`).
- **cars:** the distance between the copy and the driver's car at the same moment; how often it was pinned, why and
  how often its physics was retried; the copy's speed against the driver's; the steering wish on the copy against
  the driver's (did SetSteer stick) and the front wheel angle against the driver's car; engine states; how SetSpeed
  worked.
- **seats:** "thrown out" lines of that player's puppet (with the car's speed, fix, physics state, brain, who holds
  the seat); since 0.15 also its brain lines, put-backs inside the car, seats taken by others, and the receiver's own
  ride-along lines.
- **car notes:** the one-time probe (`probe: speed .. after SetPos .. after SetRot .. after SetSpeed`), SetSpeed and
  engine lines.
- **NPCs** (guest): host NPCs in range and paired, the distance of ours from the host's, the NPCs most often more
  than 3 m off.
- **guest-owned NPCs:** how many NPCs became the guest's own (only his game had them), how they ended (died,
  handed back to the host, gone, claim refused), the host's claims, how often the host showed them with a copy and
  how far that copy was from the guest's NPC.
- **traffic:** host cars near the guest, bytes per second, unchanged states skipped, budget deferrals; on the guest
  how often the host's traffic showed, copies, the copy-vs-target error and the copies most often off, pins, bumps,
  and how many of the guest's own traffic cars were hidden or still drove near him.
- **reconnects:** per game, the `RCN` events (dropped, lost, back with the time without the host, sync paused /
  resumed).
- **explosions:** none of the X records are read by the tool yet; grep them (`XC` on the thrower, `XR`/`XV`/`XD` on
  the others: a sent explosion's `seq` pairs them). Throws (G records, not read by the tool either): `GT`/`GX out`/`GE out`
  on the thrower and `GR`/`GX in`/`GV`/`GE in` on the others pair by `seq`; the explosion that ends a throw names it
  (`XO`/`XR throw=`).
- **worst moments:** the largest errors at least a second apart, with the wall-clock time on both games.

`--csv DIR` writes the lined-up series (`foot-p<id>-on-<role>.csv`, `car-p<id>-on-<role>.csv`) for plotting.
