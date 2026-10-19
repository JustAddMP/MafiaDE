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
| `TX` | both | each state sent (60 Hz) | `sT=<ms> p h v m=<0 idle 1 walk 2 run 3 sprint> f=<flags: 1 car, 2 crouch, 4 sprint, 8 aim, 16 reload, 32 in air, 64 cover> hp sh=<shots> w=<weapon> act=<action counter>:<latest kind: none light heavy counter takedown throw hitreact jump vault climbup dropdown land ladderup ladderdown coverslide medkit pickupbody dropbody doorkick lockpick dooropen> pose=<bits: 1 sneaking 2 carrying 4 ladder>:<carried body key>`; in a car also `car=<key> mdl seat cp q cv ctl=<valid,steer,power,brake,handbrake,gear> sig=<car signals byte: 1 park, 2 head, 4 indicator A, 8 indicator B, 16 beacon, 32 siren, 64 horn, 128 valid; 0 unknown>` |
| `RX` | both | each state received | `from sT off=<s> ...` the same fields as `TX` |
| `RXD` | both | dropped state | `from sT last` (older than or equal to the newest) |
| `CLK` | both | 1 Hz per remote | `id off rtt=<ms> hz=<received rate> snaps=<buffered> newest=<ms ahead of now> recv drop last=<s since the last state> pd=<playout delay now, ms> pdt=<its target> jit=<lateness p95, ms> late=<states that came after the render time passed them> sclk=<our session clock offset to the host, ms; 0 on the host>` (net/playout.h, core "Session clock") |
| `LAT` | both | each message applied (streams 1 in 4) | `sys=<player world npc npc-event traffic traffic-event door explosion car-damage police item item-event hit> from=<sender id> age=<ms from the sender's send to our handler (core's send stamp on the session clock; both hops when relayed)> hand=<ms from the handler to the system's script tick taking it> apply=<age + hand> held=<1: the system's tick had not run for over 1 s (or not yet) when it took the message - a guest in the menus / loading, e.g. WorldEnv received in the menu; that wait is left out of the stats and counted apart>` (game/latency.h); tools/sync_compare.py "Latency" |
| `PUP` | both | 60 Hz per remote on foot (plus every snap) | `id rT=<the remote time the target stands for, ms> tgt tv have=<copy position known> age=<s> cp=<puppet> ch=<its heading> err g=<gait 0-3> mult want meas carry base=<walk,run,sprint calibrated> fix=<0 none 1 nudge 2 snap> nudge re=<walk re-aimed> mv=<walk point> turn sm=<his mode> sf=<his flags> busy=<puppet report: 0 free 1 action on the spot 2 traversal move / ladder> nz=<height part of the nudge> tm=<followed along his path now> rec=<within the catch-up time after a move> pose=<his pose bits>` |
| `VEH` | both | 60 Hz per remote driver (plus pins, retries, snaps) | `id key rT have tgt tv cp cv err rot=<deg> fix=<0 none 1 blend 2 pin 3 snap> pin why=<0 - 1 too slow 2 ran off 3 stays off> jp=<pinned now> rt=<physics retried now> pins tsp csp vel=<wanted copy velocity> pw ctl st br hb gear sig=<his car signals>` |
| `PAS` | both | 60 Hz per remote passenger | `id key rT seat cp` |
| `AL` | both | each action of ours | Lua: our player punched / countered / threw ... (docs/game-api.md 17): `kind=<1 light 2 heavy 3 counter 4 takedown 5 throw 6 hitreact 18 doorkick 19 lockpick 20 dooropen> seq=<counter now> opp=<GUID of the human the game aimed it at, - none>` |
| `ACS` | both | each action of ours sent | `sT=<ms of the state that first carries it> kind=<name> seq` |
| `ACT` | both | each action of a remote player | his counter moved on the state his puppet is driven from: `id kind=<name> seq missed=<steps skipped>`; the puppet plays it this tick |
| `AP` | both | each action played | Lua, on the puppet: `id kind anim=<animation name, - none accepted> ok=<1 AnimPlay accepted> dur=<s the puppet is left alone at most>` |
| `AE` | both | each action ended | Lua: `id kind anim took=<s> done=<1 the animation reported done> why=<finished, time_up, next_action, he_is_in_a_car>`; the puppet walks again (its `PUP` lines show `have=0` while it played) |
| `TE` | both | each traversal message of ours | Lua: `msg=<USE_FENCE USE_LADDER PICKUP_BODY DROP_BODY PLAYER_USED_MEDKIT>` (collected for the next sample) |
| `TL` | both | each traversal move / body / medkit action of ours | C++ (game/traversal.h LocalDetector): `kind=<name> seq=<action counter now> z vz sp=<ground speed> ev=<message bits of that sample: 1 fence 2 ladder 4 pickup 8 drop 16 medkit>` |
| `TP` | both | our pose changed | `bits=<1 sneaking 2 carrying 4 ladder> body=<carried body key>` |
| `TVC` | both | a remote puppet's move started / ended, a snap softened | `id ev=<start end soft> err=<m> dz=<target - puppet height> nudge=<x,y,z> snap=<the controller had asked for a snap>` (game/traversal.h Catchup) |
| `CB` | both | a puppet's carried body | Lua: `id key real=<1 TakeBody took>` after the check, `id key end=<why>` when it is put down |
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
| `NRL` | host | when the count changes | several guests: `to n=<other guests' own NPCs relayed to him this round> was` (C++; npc_owner.h "relay") |
| `NRH` | host | per forwarded hit | several guests: a guest hit another guest's own NPC (its relay key): `from owner key=<the owner's id of it> amount type lethal` (C++; sent to the owner as HostNpcHit with `from` as the attacker) |
| `SEA` | both | per seat yield | several players in one car: `key from=<our seat> to=<seat we slide to> claims=<id:seat of the others' states> ok` (Lua, src/game/seats.h) |
| `SEA` | both | per change | where a remote player's car copy stands (Lua, seats.h `PlaceCopy`): `key ev=place dec=<wait at_car aside hold release> blk=<m to the nearest other car, -1 none> for=<s his spot has been blocked> spot=<candidate:x,y,z beside his car, - none>`; `key ev=oldcopy drop=<key of the parked copy of his earlier car dropped from his spot> last=<its last driver>`; `ev=world dropped=<records looked up again in a new world>`; `key ev=reseat id seat aside=<1 copy beside his car>` (the puppet is seated anew: the record's car is another entity now) |
| `DRV` | both | once per conflict | two other players claim the wheel of one car: `key keeps=<the driver here> claims=<the other one>` (Lua) |
| `DRV` | both | his car key changes | a remote driver's car key changed (a reload, a rejoin, another car): `key ev=newkey id was=<his key before> st=<our record's state: new obtain spawn ready failed>` (Lua) |
| `TRH` | host | 2 Hz | traffic totals: `cars=<host traffic cars near guests> spawnMsgs stateMsgs despawnMsgs bytes=<all traffic bytes sent> bumps=<TrafficBump received>` |
| `TRP` | host | 2 Hz per guest | `id relevant=<host cars in his range> admitted=<cars he has copies of> spawned despawned states skipped=<unchanged, not sent> bytes deferred=<due but over the budget, this tick>` |
| `TRG` | guest | 2 Hz | `mode=<1 host traffic here, 0 our own> known live copies avg max=<copy vs target, m> pins snaps bumps states spawns despawns old=<states dropped as older>` |
| `TRC` | guest | 2 Hz per traffic copy | `net tgt tv cp cv err pin pins f=<state flags: 1 handbrake 2 siren 4 no driver 8 controls 16 reversing>` (`err` 1e9 before the first report) |
| `TRL` | guest | 2 Hz | `own=<our ambient spawning on> hidden=<our traffic cars hidden> near=<ours still driving near us>` |
| `POL` | both | on change, 1 Hz while wanted | Lua, our police state: `f=<PoliceState flags> w hunt lvl seen decay fine arrest off zone=<centre> p=<our position>` |
| `POE` | both | per police event | `from=me ev=<name> arg` (ours, as sent) or `from=<id> ev arg` (another player's, received) |
| `POR` | both | per changed state received | `from seq f lvl zone` (another player's PoliceState) |
| `POT` | host | 1 Hz per targeted guest | Lua: `id lvl hunt cops=<our police within 90 m of his puppet> seers=<of them, saw it in 5 s> near=<m> set=<set on it this scan> attacks=<Attack calls so far>` |
| `POC` | both | per PoliceChase | host `dir=tx id active hunt lvl cops seers near`; guest `dir=rx active hunt lvl cops seers near forced=<level we hold>` |
| `CDS` | both | 1 Hz per car | car damage (docs/game-api.md 24): `ref=<o:g player car, h<id> our traffic car (host), t<net> a host traffic car's copy (guest)> ent role=<A we hold its damage, C a copy> auth=<the player whose game holds it> quiet=<1 moved by SetPos lately> read=<1 its damage could be read> parts dmg=<damaged parts> gone worst=<0-254> tyres=<burst or off> flags=<1 exploded 2 burning> burn=<s left> gd md=<GetDamage, GetMotorDamage> known=<a full state of the car is known> ep=<epoch> adm atyres aflags=<the holder's state as known here> layout` |
| `CDT` | both | per CarDamage sent | `ref kind=<first, diff, full, keyframe, repair, answer> ep n=<parts listed> flags parts=<index:damage,...> to=<answer: the player>` |
| `CDR` | both | per car damage message received | `from ref kind=<damage, report, request> ep n flags full repaired` |
| `CDA` | both | per change set on a car | `ref ent why=<sync: a copy raised to the holder's; report: another game's copy's damage on our car; rebind: a new entity for a car; repair> parts=<index:damage,...> wheels=<count> burn explode reason=<repair: epoch, stale, quiet>` |
| `CDX` | both | per CarDamageReport sent | `ref to ep parts wheels=<1 a tyre / wheel> flags` |
| `CDQ` | both | per CarDamageRequest sent | `ref to` |
| `CDK` | both | another game's explosion put on a car we hold (C++) | `ref ent d=<m from the blast> fire=<1 a molotov's fire only> seq from parts=<quanta added> tyres burn explode writes=<writes made>` (docs/game-api.md 24) |
| `CDB` | both | each ask of the one-explosion book (C++) | `ent first=<1 it blows up now, 0 refused: blown up already>` (docs/game-api.md 24 "One explosion") |
| `CDE` | both | each car blown up by the one explode routine (Lua D.Explode) | `ent ok=<SetExploded worked> picture=<1 a picture shown> claimed=<1 our own car, asked already> from=<the player whose game said so, - car damage>` |
| `RCN` | both | on reconnect events (docs/protocol.md "Reconnect") | `ev=dropped id` (a player lost his connection, slot held), `ev=lost why` (guest: we lost the host), `ev=sync-paused`, `ev=sync-resumed local`, `ev=back same=<1 same slot> local attempts` |
| `FAIL` | both | on a failed mission | `ev=freeroamretry reason role skip fresh` (the game's answer to a failure: skipped or not), `ev=gameover reason role fresh`, `ev=report from seq reason point verdict` (host: a guest's MissionFail and the answer), `ev=verdict kind seq reason` (guest), `ev=action kind seq reason` (what FailFlow decided: 0 SendHostFailed, 1 SelfReload, 2 SendReport, 3 Quit, 4 LoadHostPoint, 5 ReloadInWorld, 6 GameFallback), `ev=log <text>` (its state changes) |
| `ITH` | host | 2 Hz | world items (docs/game-api.md 20): `items=<shared items known> round=<in this round> sent=<in ItemStates> ours=<taken by the host> guests=<taken by guests> refused=<late takes refused> forgot=<despawned, no take> unknown=<takes of items we did not know>` (C++) |
| `ITG` | guest | 2 Hz | `host=<host items known> paired unshown=<host items without ours> round=<our items in range> pairs moves hidden=<our new drops hidden> hiddenForHost=<ours hidden: taken on another game> takes confirmed lost` (C++) |
| `ITE` | both | on event | C++: `ev=take from key type p granted by` (host: a guest's ItemTake), `ev=gonesent key by why to=<all/player>` (host), `ev=gone key by why ours` (guest: an ItemGone), `ev=localgone id host how gained` (guest: our item vanished), `ev=takesent key type p` (guest) |
| `ITL` | both | on event | Lua: `ev=gone id type p d=<from us> name gained`, `ev=hide id type p ok active why`, `ev=clear id ok try` (ClearWeaponsInRadius after a Deactivate that did not take), `ev=move id from to ok off=<read back, m>`, `ev=undo weapon had ok still`, `ev=collect name d` |
| `COL` | both | per collectible | `from name` (ours sent, or another player's) |
| `DR` | both | each door event (C++, docs/protocol.md "Doors") | `ev=local id door=<name hash> p old state ours act how verdict=<send/echo/kept/revert/same/unknown>` (our poll saw a door change), `ev=tx seq by=<p<id> or world> act state door p from sent` (we sent it: guest DoorEvent, host DoorApply), `ev=rx ... verdict=<accept/reject (host), applied/kept/own (guest)>`, `ev=fix ...` (host: the correction a refused guest gets), `ev=apply id act state by why=<remote/memory/snapshot/revert>` (we change our door), `ev=unmet id` (it did not get there in 3 s), `ev=snapreq reason p`, `ev=snaptx to reason p n open unlocked mem`, `ev=snaprx n applied`, `ev=world key doors mem` (a new world: table and memory forgotten) |
| `DL` | both | each door message / door change (Lua) | `ev=msg id kind=<OPEN CLOSE KICK LOCKPICK TOGGLELOCK TOGGLEOPEN> sender=<GUID> ours known`; `ev=change id name p old state ours act how=<msg: its door message came from our player, act: our player's door action, near: he stood at it, -: our world> d=<m from our player> msg=<latest message kind@age>` |
| `DA` | both | each change carried out on a door (Lua) | `id name act want cur by why calls=<calls made, ! = failed> ok` |
| `XT` | both | each explosive throw of ours | `weapon=<the selected explosive, - unknown> p=<where we stood> throw=<its throw number, GT>` (docs/game-api.md 23) |
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
- **doors:** per game, the door events by kind and verdict, how changes were attributed, the door messages, the
  applies with a failed call, how many of the changes it sent arrived on the other game, and the first 30 sends /
  receipts / snapshots.
- **car damage:** per game what it sent (by kind), received, set on cars and reported; each player car's last
  state on both games side by side (marked when it differs), cars whose damage could not be read, and the
  traffic cars each game had (damaged ones named).
- **reconnects:** per game, the `RCN` events (dropped, lost, back with the time without the host, sync paused /
  resumed).
- **explosions:** none of the X records are read by the tool yet; grep them (`XC` on the thrower, `XR`/`XV`/`XD` on
  the others: a sent explosion's `seq` pairs them). Throws (G records, not read by the tool either): `GT`/`GX out`/`GE out`
  on the thrower and `GR`/`GX in`/`GV`/`GE in` on the others pair by `seq`; the explosion that ends a throw names it
  (`XO`/`XR throw=`).
- **worst moments:** the largest errors at least a second apart, with the wall-clock time on both games.

`--csv DIR` writes the lined-up series (`foot-p<id>-on-<role>.csv`, `car-p<id>-on-<role>.csv`) for plotting.
