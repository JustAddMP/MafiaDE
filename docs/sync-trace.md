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
- **Comparing:** `python tools/sync_compare.py <trace from one PC> <trace from the other> [--csv DIR] [--worst N]
  [--check]` (standard library only). See "What the report says" below. `--check` ends the report with every sync
  target of docs/sync-architecture.md section 4 (PASS / FAIL / no data per direction; exit code 1 when one fails).

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
| `CLK` | both | 1 Hz per remote | `id off rtt=<ms> hz=<received rate> snaps=<buffered> newest=<ms ahead of now> recv drop last=<s since the last state> pd=<playout delay now, ms> pdt=<its target> jit=<lateness p95, ms> late=<states that came after the render time passed them> sclk=<our session clock offset to the host, ms; 0 on the host> ow=<the one-way trip his body and car are led forward by on top of pd, ms: rtt / 2>` (net/playout.h, core "Session clock"; `ow` unreleased) |
| `LAT` | both | each message applied (streams 1 in 4) | `sys=<player world npc npc-event traffic traffic-event door explosion car-damage police item item-event hit> from=<sender id> age=<ms from the sender's send to our handler (core's send stamp on the session clock; both hops when relayed)> hand=<ms from the handler to the system's script tick taking it> apply=<age + hand> held=<1: the system's tick had not run for over 1 s (or not yet) when it took the message - a guest in the menus / loading, e.g. WorldEnv received in the menu; that wait is left out of the stats and counted apart>` (game/latency.h); tools/sync_compare.py "Latency" |
| `PUP` | both | 60 Hz per remote on foot (plus every snap) | `id rT=<the remote time the target stands for, ms> tgt tv have=<copy position known> age=<s> cp=<puppet> ch=<its heading> err g=<gait 0-3> mult want meas carry base=<walk,run,sprint calibrated> fix=<0 none 1 nudge 2 snap> nudge re=<walk re-aimed> mv=<walk point> turn sm=<his mode> sf=<his flags> busy=<puppet report: 0 free 1 action on the spot 2 traversal move / ladder> nz=<height part of the nudge> tm=<followed along his path now> rec=<within the catch-up time after a move> pose=<his pose bits> hint=<the gait his game reported, which the puppet walks with: 0-3> rest=<1: the rest blend slid the standing puppet onto his spot> brk=<1: the puppet ran on after his stop / turn and its body was held back to his speed> stp=<1: he brakes to a stop, the walk point is his stop point> cspd=<the puppet's own walking speed, m/s> lead=<ms the target is led forward: playout delay + one-way trip> nsum=<m our nudges have moved the puppet so far (ground plane; since the velocity matching, unreleased): its increase against the puppet's way is the foot slide, sync_compare "the AI walk">` (hint .. lead unreleased; since the 2026-10-20 velocity matching `brk` = held back by more than half its own walking speed, `carry` is the core controller's own value, no longer applied: game/player_motion.h FootDriver, docs/protocol.md "Driving the puppet and the car copy") |
| `VEH` | both | 60 Hz per remote driver (plus pins, retries, snaps) | `id key rT have tgt tv cp cv err rot=<deg> fix=<0 none 1 blend 2 pin 3 snap> pin why=<0 - 1 too slow 2 ran off 3 stays off> jp=<pinned now> rt=<physics retried now> pins tsp csp vel=<wanted copy velocity> pw ctl st br hb gear sig=<his car signals> pf=<s pinned so far, 0 not pinned> ri=<s until a pinned copy gets its physics back> gs=<1: not moved, given his speed alone (SetSpeed)> skid=<1: his car slides, blended at his whole speed> cz=<the car preset's verdict: 0 nothing 1 blend 2 snap> lead=<ms led forward>` (pf .. lead unreleased: game/player_motion.h CarDriver) |
| `PAS` | both | 60 Hz per remote passenger | `id key rT seat cp` |
| `PKB` | both | per change | the book of parked shared cars (src/game/parked_cars.h; unreleased): `ev=parked key by=<the player whose CarParked it was, us included> wrecked n=<cars in the book now>`, `ev=driven key by n` (somebody drives it again: out of the book) |
| `PKS` | host | per player who joined / came back | `to n=<parked cars sent> msgs=<ParkedCars messages>` |
| `PKR` | both | per ParkedCars received | `from n=<cars in the message> applied=<placed here> wrecked=<destroyed ones left out>` |
| `AL` | both | each action of ours | Lua: our player punched / countered / threw ... (docs/game-api.md 17): `kind=<1 light 2 heavy 3 counter 4 takedown 5 throw 6 hitreact 18 doorkick 19 lockpick 20 dooropen> seq=<counter now> opp=<GUID of the human the game aimed it at, - none>` |
| `ACS` | both | each action of ours sent | `sT=<ms of the state that first carries it> kind=<name> seq` |
| `ACT` | both | each action of a remote player | his counter moved on the state his puppet is driven from: `id kind=<name> seq missed=<steps skipped>`; the puppet plays it this tick |
| `AP` | both | each action played | Lua, on the puppet: `id kind anim=<animation name, - none accepted> ok=<1 the call accepted it> dur=<s the puppet is left alone at most (2.5 while the way is not confirmed)> method=<the way it was played: direct direct_notrans desire script script_combat animplay (game/anim_methods.h)> group=<melee throw move other> round=<passes through all ways so far> conf=<1 this way moved the hands before> bones=<1 the hands could be read>` |
| `AE` | both | each action ended | Lua: `id kind anim took=<s> done=<1 the animation reported done> why=<finished, time_up, next_action, he_is_in_a_car> method hand=<m the farther hand got from its rest, in the puppet's frame> root=<m the puppet moved> start=<s after the call a hand first moved 0.12 m, -1 never> bones verdict=<moved still unknown> ev=<none works switched wrapped lost> next=<the way the next action of the group is played>`; the puppet walks again (its `PUP` lines show `have=0` while it played). `verdict=moved` is the animation seen to play; the `start` of those is its start latency |
| `SC` | both | script control asked / answered on a puppet | Lua: `id on=<1 asked 0 given back> ok=<1 the call worked>`, and for the game's answer `id msg=<SCRIPT_CONTROL AI_CONTROL> state=<GetState()> after=<s since we asked>`; `id msg late=1 for=<action serial it was asked for, 0 none> running=<the action playing now, 0 none>` (an answer after its action ended or after the control was given back: ignored); `id skip=1 pending=<1 the friend brain switch is not over> state=<foot seat door ragdoll>` (not asked: the action plays without it); `id release=<why> state` (given back before a car seat, a door walk or a ragdoll); `id forget=1 on act` (the puppet entity went: re-queued or respawned, its state cleared) |
| `TE` | both | each traversal message of ours | Lua: `msg=<USE_FENCE USE_LADDER PICKUP_BODY DROP_BODY PLAYER_USED_MEDKIT>` (collected for the next sample) |
| `TL` | both | each traversal move / body / medkit action of ours | C++ (game/traversal.h LocalDetector): `kind=<name> seq=<action counter now> z vz sp=<ground speed> ev=<message bits of that sample: 1 fence 2 ladder 4 pickup 8 drop 16 medkit 32 deep water>` |
| `TS` | both | each vertical snap of ours not taken for a move | C++ (game/traversal.h): `z vz n=<snaps so far>`: a vertical speed no jump (above 8 m/s up) or fall (below -12 m/s at its start) begins with, e.g. getting on or off a motorbike (docs/game-api.md 25) |
| `WT` | both | our water state changed | C++ (game/water.h): `st=<dry/swim/under> was why=<IN_DEEP_WATER / dived / surfaced / climbed out / messages stopped / in a vehicle / teleport / getter> z surf=<the height taken for the surface> get=<swim getter answer, -1 none>` |
| `TP` | both | our pose changed | `bits=<1 sneaking 2 carrying 4 ladder 8 swimming 16 under water 32 bike> body=<carried body key>` |
| `TVC` | both | a remote puppet's move started / ended, a snap softened | `id ev=<start end soft> err=<m> dz=<target - puppet height> nudge=<x,y,z> snap=<the controller had asked for a snap>` (game/traversal.h Catchup) |
| `CB` | both | a puppet's carried body | Lua: `id key real=<1 TakeBody took>` after the check, `id key end=<why>` when it is put down |
| `PV` | both | each of our shots near a puppet resolved (unreleased) | Lua (M.ShotTick, game/pvp_hit.h): `id=<puppet, - none> v=<game: the game's own DAMAGE (Hit already sent), bullet: ON_BULLET_HIT named it, ray: our aim ray hit it, ray_unconfirmed: the ray hit but the game reports our bullets on friends and did not this time, blocked: a wall in between, not_hittable: in a car, dead or hidden, miss, off: friendly fire off, bullet_alone: an ON_BULLET_HIT no counted shot took> hit=<1 a Hit was sent> w=<weapon> wid=<weapon id> dmg=<the Hit's amount> dist=<m along the aim> zone=<enums.Hitzone of the ray hit> miss=<m between the aim ray and the puppet's axis> blk=<1 wall> ok=<1 hittable> ge=<1 the game's DAMAGE matched> be=<1 ON_BULLET_HIT matched> brain=<1 the puppet has the friend brain> rep=<- , game or bullet: the game reported our bullets on a friend-brain puppet this session> demi=<1 the puppet was shielded (a demigod: no DAMAGE from the game, so rep is not used for this shot)>` |
| `PVM` | both | a locked-on punch at its impact (unreleased) | Lua (M.MeleeTick): `id heavy=<1 charged> off0=<degrees between our facing and the puppet at the swing> off=<the same at the impact: small = the SetDir turn took> dist=<m> hit=<1 it was reached: Hit sent>` |
| `LR` | both | 20 Hz | our player read back: `p d spr hp`; in a car `car seat cp cv rb=<steer wish,power,brake,handbrake,steer linearity,front wheel angle (rad)> eng gear spd fixed rpm pwi sig=<our car's signals as read back, -1 = not readable>` |
| `PR` | both | 20 Hz per puppet | `id st p d hid veh seat own=<car it sits in> seated ejects tries anim phys spr dead hp mv mult moves snaps sown=<who holds its seat: ours/free/other/?, - on foot> brain=<friend brain set> ai=<GetAIType> back=<put-backs into the seat> leaving=<out of its seat inside the car now> ejAll=<throw-outs this session> demi=<shielded: a demigod now> fast=<hidden: too fast on foot> wet=<water mode: 1 swims, 2 under water> perch=<perched on a bike's saddle> bike=<its vehicle is a motorbike>` (sown .. ejAll since 0.15, docs/game-api.md 18; demi .. bike: docs/game-api.md 25) |
| `FF` | both | a remote player's "too fast on foot" changes | C++ (game/puppet_guard.h FastFoot): `id hide=<1 his puppet hidden> v=<his ground speed> peak veh=<in a vehicle> wet=<in the water>` |
| `PH` | both | a puppet hidden at its player's position / shown again | Lua: `id why=<fast: too fast on foot; deaths: it died here again and again; none: shown again> was` |
| `PG` | both | a puppet's shield changes | Lua (M.ApplyShield): `id on=<1 demigod> why=<hazards: seat water in_water fall fire blast hidden down, joined by +; "none" while it holds on after them> ok=<SetDemigod accepted>` |
| `PD` | both | a puppet died here although its player lives | Lua (M.PuppetLife): `id hp=<its player's health> cause=<death type / last damage / none seen> demi=<shield was on> wet=<in the water> veh=<car key or -> n=<local deaths in the last 20 s>` |
| `PW` | both | a puppet's water mode | Lua (M.WaterTick, M.OnTravEvent): `id ev=<in / out / under / surface / deepwater (IN_DEEP_WATER on the puppet)> p / z / dur / wet` |
| `BK` | both | once per motorbike | Lua (M.BikeProbe): `id how=<owner / owner_scene_object / our_copy> type=<GetType> tb=<GUID type byte> ent=<car methods its entity has> mc=<those its motorcycle component has>` |
| `BKS` | both | a puppet on a bike | Lua (M.SeatBike): `key id seat ev=<on: sits on it / perch: shown on its saddle instead> tries anim` |
| `CUW` | both | a shared car went under water here | Lua (M.OnCarUnderwater, CAR_IS_UNDERWATER): `key z drv=<who drives it: local, a player id, ->` |
| `CR` | both | 20 Hz per driven car copy | `key drv adopted fix pin p v rb=<as LR> eng gear spd fixed rpm pwi cmd=<steer,power,brake,handbrake,gear given> steerIn=<SetSteer value> vel speedHow=<kmh/none: SetSpeed took km/h / failed on this copy; vector/number in traces before the km/h fix> seat0=<puppet/none/other> locked sigw=<signals wanted (his)> sigch=<changes applied> sigre=<times set again after the copy's game changed them back>`; `sig` = the copy's signals read back |
| `SWP` | both | on change | swapped-car machine (src/game/car_swap.h): `key phase=<driven/parking/done> act=<none/restore/restore_here/restore_wrecked/forget> lock=<copy closed to us> bits=<car_swap.h Bit flags it was given> spd=<copy m/s> parked=<s since the CarParked, -1 none> why` |
| `SWR` | both | on event | a real car hidden behind a copy: `key ev=swap orig at why`, `key ev=restore how=<copy/here> from p active copy wrecked why`, `key ev=forget orig world` |
| `MK` | both | 1 Hz | Lua, the other players' map markers (game-api.md 13): `players puppet=<icons on a puppet> pos=<icons at a position> none=<players without an icon> ids=<player>:<nav id>/<human or pos icon>/<puppet or pos>/<check>[/brainwait],...` (`-` no icon; check, for an icon on a puppet: `ok` the game has it there, `unchecked` not yet (1 s after it was put there), `unknown` the check failed, else `-`; `/brainwait`: his puppet's friend brain switch is still to come, so his icon waits at his position) |
| `MKE` | both | each marker change | Lua: `id nav ev=<create/to_puppet/to_pos/detach/remove/lost/give_up> icon=<human/pos> to=<puppet/pos> p=<position it was put at>` (moves at a position are not traced, see `MK`) |
| `NHS` | host | 2 Hz | `sending hits kills mirrors=<guests' own NPCs with a state> shown=<with a copy> lateKills=<our NPCs killed as dead mirrors>` |
| `NP` | guest | 2 Hz per host NPC in range | `key loc=<our id; hidden:<id> for a paired NPC that did not follow> copy ai car=<0 on foot, 1 its car flag, 2 faster than 11 m/s: taken as in a vehicle> spd=<its speed, m/s> host=<pos> our=<pos> d=<m> hp=<host>/<ours> dead=<host>/<ours> age` (`spd`, `car=2` unreleased; unreleased: `host` is the replica target, the host's NPC interpolated and led to now, `spd` its velocity: not lined up with the host in time, `NPR` / `NPT` are) |
| `NGS` | guest | 2 Hz | `live matched copies avg max kills snaps=<replica snaps> own=<our own NPCs> claims=<host claims carried out> unpairs=<pairs released: the host's NPC in a vehicle, ours on foot over 40 m off> refused=<paired NPCs that did not follow: hidden, a copy shown> fallbacks=<copies spawned with a fallback profile> frozen=<paired NPCs whose AI was interrupted> drives=<drive commands applied>` (unreleased: `snaps refused frozen drives` replace `pulls steers`) |
| `NPT` | the authority (host; a guest for his own NPCs) | per NPC state sent (15 Hz near a receiver to 5 Hz at 120 m) | one authority per NPC, the truth: `key=<its entity id here> sT=<sample time, ms, this game's steady clock as TX's> p=<pos> v=<vx,vy m/s> g=<gait 0-3 from its speed>` (C++, unreleased) |
| `NPR` | the receiver (guest; the host for guests' own NPCs) | 10 Hz per driven replica | `key=<the authority's key> from=<the authority's player id> kind=<1 a paired NPC, 2 a spawned copy> rT=<the authority time its target stood for, ms (render time + lead)> cp=<the replica where our game shows it> tp=<its target> err=<cp vs tp, m> fix=<0 none 1 nudge 2 snap> why=<-, far, height, stuck, teleport, blocked> g=<commanded gait> sg=<the authority's gait>` (C++, unreleased) |
| `NPS` | the receiver | per replica snap | `key from kind err=<m> why=<far, height, stuck, teleport, blocked>` (C++, unreleased) |
| `NBW` | host | every 10 s per guest | `to=<guest> kbs=<NPC states + infos + leaves sent to him, KB/s> near=<NPCs in his interest>` (C++, unreleased) |
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
| `SCP` | both | per change | where a remote player's car copy stands (Lua, seats.h `PlaceCopy`): `key ev=place dec=<wait at_car aside hold release> blk=<m to the nearest other car, -1 none> for=<s his spot has been blocked> spot=<candidate:x,y,z beside his car, - none>`; `key ev=oldcopy drop=<key of the parked copy of his earlier car dropped from his spot> last=<its last driver>`; `ev=world dropped=<records looked up again in a new world>`; `key ev=reseat id seat aside=<1 copy beside his car>` (the puppet is seated anew: the record's car is another entity now); `key ev=lookwait id seat` (the puppet waits for our new copy's look before it is seated, car_look.h holds) |
| `DRV` | both | once per conflict | two other players claim the wheel of one car: `key keeps=<the driver here> claims=<the other one>` (Lua) |
| `WHL` | both | per event | one wheel, one driver (Lua, src/game/seats.h `LocalWheel`; unreleased): `key ev=yield to=<the keeper> why=<slide: he drove it here, the game slid us over / keeper: the holder or the lower id> holder` (we give way, claim seat 9 from now); `key ev=keep other holder drv` (we keep it, he gives way on his game); `key ev=slide seat ok try` / `key ev=out ok try` (M.WheelTick moving us); `key ev=done to seat=<where we sit now, nil out> took=<s>`; `key ev=stuck to` (slides and get-outs did not move us: we stay, claiming nothing); `key ev=hold on=<1/0> ok` (PlayerShouldRemainPassenger set as a passenger of another player's car); `key ev=bike to holder` (a motorbike whose saddle another player's state claims too and he keeps: we count as the rider behind him, seat 1, nothing moved) |
| `PAU` | both | per change | a paused game (src/game/peer_pause.h; unreleased): ours `id=<us> ev=<local-paused local-repeat local-resumed> reason=<1 no script tick 2 the game's paused tick> ms=<paused so far / in all>`; another's PeerPaused `id ev=<paused resumed> reason ms`; his copies here `id ev=freeze car=<key of the car copy held, - on foot> pup=<1 from its fresh report>` and `id ev=thaw held=<s>` |
| `DRK` | both | his car key changes | a remote driver's car key changed (a reload, a rejoin, another car): `key ev=newkey id was=<his key before> st=<our record's state: new obtain spawn ready failed>` (Lua) |
| `TRH` | every traffic authority (0.15: host) | 2 Hz | our own traffic's totals: `cars=<our traffic cars near other players> spawnMsgs stateMsgs despawnMsgs bytes=<all traffic bytes sent> bumps=<TrafficBump / TrafficBump2 received> nodrv=<cars without a driver read now> orphan=<of those, confirmed driverless (1 s, slow)> orphaned=<confirmed so far> driven=<found driven again so far>` (the last four unreleased; unreleased: on a guest too, once it has cars or sent any) |
| `TRS` | every traffic authority | per car, when its state went to someone, at most 10 Hz (unreleased) | the truth of our traffic car: `net sT=<our state clock, ms: the stamp of the TrafficStates / TrafficStates2 it went in> p=<x,y,z> v=<vx,vy,vz>` (full precision); tools/sync_compare.py lines a receiver's `TRC` up with it, at the same moment (the clock relation) and at `rT` (traffic.true_p95) |
| `TRO` | both | each event (unreleased) | traffic drivers and crews. Authority (any game, our own cars): `key ev=miss why=<seat_0_empty, seat_0_holds_NAME_no_generator_human, its_driver_is_dead, seat_read_failed (spaces as _ , other characters dropped)> drv=<active inactive -> the human in seat 0> spd=<m/s> dist=<m from our player>`; `key ev=back after=<s without a driver read> how=<scan round>`. Receiver (our copies): `a=<authority> net ev=out` (the copy's driver got out: its car driverless and both stand), `a net ev=in` (driven again: back in), `a net ev=reseat seat try ok=<GetOnVehicle ran>` (a crew member put back in his seat) (`a=` unreleased) |
| `TRP` | every traffic authority (0.15: host) | 2 Hz per other player | `id relevant=<our cars in his range> admitted=<cars he has copies of> spawned despawned states skipped=<unchanged, not sent> bytes deferred=<due but over the budget, this tick>` |
| `TRG` | every receiver (0.15: guest) | 2 Hz | `mode=<1 another player's region here: our spawning off, 0 our own> auth=<whose region (our id: ours)> known live copies avg max=<copy vs target, m> pins snaps bumps states spawns despawns old=<states dropped as older> frozen=<1 an authority with cars here is paused: its copies held> quiet=<knocks not reported: an authority paused or just resumed, or our own pause> due=<the other games' cars within 150 m of us, at most 20> shown=<of those, the ones with a copy that drove this tick> blends fsnaps=<corrector blends / snaps so far>` (`frozen` on: unreleased; `auth`, `due`, `shown`, `blends`, `fsnaps` unreleased) |
| `TRA` | every receiver | 2 Hz per authority (unreleased) | `a=<authority id> d=<m from us to his player, -1 unknown> live copies due shown paused pd=<his playout delay, ms>` |
| `TRR` | every game | per change (unreleased) | whose region we are in (src/game/traffic_model.h RegionFollow): `auth=<the region's owner; our id: ours> from=<the one before, 255 none> own=<1 our spawning runs> d=<m to the owner's player, -1 us>` |
| `TRZ` | every receiver | per change | an authority's pause and ours for its traffic copies (unreleased): `ev=<freeze thaw> a=<authority> copies quiet=<knocks not reported so far>`; `ev=localgap copies quiet` |
| `TRC` | every receiver (0.15: guest) | 2 Hz per traffic copy, at its first report after the mark | `a=<authority> net rT=<the authority's clock, ms, that the target stands for: render time + lead> tgt tv cp=<the copy after this tick's commands> cv err pin pins f=<state flags: 1 handbrake 2 siren 4 no driver (unreleased: confirmed, 1 s without a driver read while slow) 8 controls 16 reversing>` (`err` 1e9 before the first report; `a`, `rT` unreleased: a 0.15 TRC has neither and is the host's) |
| `TRL` | every game (0.15: guest) | 2 Hz | `own=<our ambient spawning on> auth=<whose region> hidden=<our traffic cars hidden> near=<our cars within 70 m of us that a player within 240 m of them has not had for 2 s (0.15: ours still driving near us while the host's showed)> mine=<our traffic cars within 70 m of us> left=<ours within 70 m while our spawning is off> since=<s since the region changed>` (all but `own`, `hidden`, `near` unreleased) |
| `PDT` | the pedestrians' authority | 1 in 2 states sent per pedestrian (5 Hz near) | pedestrian truth (unreleased): `net=<its net id> id=<entity id here> sT=<our clock at the sample, ms: the authMs of its PedStates> p=<where it really was then>` |
| `PDR` | receiver | 10 Hz per replica | `from=<authority> net rT=<the authority's clock time the target stands for, ms: render time + lead> cp=<our replica> tp=<its target then> err=<cp vs tp, ground> how=<adopted / spawned> g=<gait sent>`; sync_compare lines `rT` up with the authority's `PDT sT` for the true error |
| `PDC` | receiver | 1 Hz | `near=<the authorities' living pedestrians within 40 m of us> rep=<of them with a replica here> hidden=<our own hidden> adopted spawned loading known=<authority pedestrians we have> snaps fails` (`ped.coverage` = rep / near) |
| `PDS` | both | 1 Hz | our pedestrians as an authority: `peds=<our ambient humans near players> owned=<of them ours to send> bytes states spawns gones deaths killreq` (totals) |
| `PDP` | both | 1 Hz per other player | `id relevant admitted states skipped bytes deferred` (our pedestrians for him) |
| `PDE` | both | per event | `ev=adopt / spawn / release / hide / show / kill / diedhere / death / killreq key=<authority << 16 + net> local=<entity id here> how p from net` |
| `PDL` | both | 1 Hz | ped_sync.lua: `amb=<ambient humans here> near=<reported near players> adopted spawned queued hidden fac=<copy spawner state> blocks=<spots our spawners were blocked around>` |
| `POL` | both | on change, 1 Hz while wanted | Lua, our police state: `f=<PoliceState flags> w hunt lvl seen decay fine arrest off zone=<centre> p=<our position> held=<our zone's decay held (guest)> follow=<our red zone follows us> forced=<level held>` |
| `POE` | both | per police event | `from=me ev=<name> arg` (ours, as sent) or `from=<id> ev arg` (another player's, received) |
| `POR` | both | per changed state received | `from seq f lvl zone` (another player's PoliceState) |
| `POT` | host | 1 Hz per targeted guest | Lua: `id lvl hunt cops=<our police within 90 m of his puppet> seers=<of them, saw it in 5 s> near=<m> set=<set on it this scan> attacks=<Attack calls so far>` |
| `POC` | both | per PoliceChase | host `dir=tx id active hunt lvl cops seers near`; guest `dir=rx active hunt lvl cops seers near forced=<level we hold> held=<our zone's decay held>` |
| `POD` | guest | per change | our zone's decay hold (PreventZonesDecay) while the host's police are on us: `hold=<1 held / 0 released> follow=<1: our red zone follows us>` |
| `POZ` | guest | per zone we start | `why=<seen: they see us, no zone / refresh: our zone decays while they are on us / revive: it decayed while they are on us> lvl hunt` |
| `CDS` | both | 1 Hz per car | car damage (docs/game-api.md 24): `ref=<o:g player car, h<id> our traffic car (host), t<net> a host traffic car's copy (guest)> ent role=<A we hold its damage, C a copy> auth=<the player whose game holds it> quiet=<1 moved by SetPos lately> read=<1 its damage could be read> parts dmg=<damaged parts> gone worst=<0-254> tyres=<burst or off> flags=<1 exploded 2 burning> burn=<s left> gd md=<GetDamage, GetMotorDamage> known=<a full state of the car is known> ep=<epoch> adm atyres aflags=<the holder's state as known here> layout` |
| `CDT` | both | per CarDamage sent | `ref kind=<first, diff, full, keyframe, repair, answer> ep n=<parts listed> flags parts=<index:damage,...> to=<answer: the player>`; also `ref kind=drop-after-write ep` (nothing sent: the holder's car read less damage within 1 s of our own writes on it, taken as the game's reaction, no repair) |
| `CDR` | both | per car damage message received | `from ref kind=<damage, report, request> ep n flags full repaired` |
| `CDA` | both | per change set on a car | `ref ent why=<sync: a copy raised to the holder's; report: another game's copy's damage on our car; rebind: a new entity for a car; rebuild: our own entity rebuilt by its tuning; repair> parts=<index:damage,...> wheels=<count> burn explode reason=<repair: epoch, stale, quiet>` |
| `CDX` | both | per CarDamageReport sent | `ref to ep parts wheels=<1 a tyre / wheel> flags` (never the exploded flag); `ref to ep echo=<wreck: the copy or the holder's car is exploded; write: within 1.5 s of our cascading writes on the copy; rebuild: within 3 s of a rebuild by its tuning> parts` when the copy's new damage was taken as an echo of our own writes and NOT sent (docs/game-api.md 24 "Echoes") |
| `CDQ` | both | per CarDamageRequest sent | `ref to why=<rebuilt: at once after car look rebuilt our copy; none: no state yet, every 3 s>` (`why` only for a rebuild) |
| `CDK` | both | another game's explosion put on a car we hold (C++) | `ref ent d=<m from the blast> fire=<1 a molotov's fire only> seq from parts=<quanta added> tyres burn explode writes=<writes made>` (docs/game-api.md 24) |
| `CLS` | both | per look of ours | car look (docs/game-api.md 24 "Car look"): `ref ent seq look=<model\|c1\|c2\|plate\|tint\|interior\|dirt\|rust\|painting\|tuning: the line form with \| for spaces>` when our car's look is new or changed; `ref ent to seq answer=1` per answer to a request |
| `CLR` | both | per CarLook received | `from ref op=<look, request> seq look=<as CLS, - for a request>` |
| `CLQ` | both | per request sent | `ref ent to=<the player whose game has the car>` (our copy has no look yet; every 3 s) |
| `CLA` | both | per look set on a copy | `ref ent try=<1..3> what=<colours, plate, window tint, interior, painting, tuning N item(s) ids; spaces as \|>` |
| `CLW` | both | per look set on a copy (Lua) | `ent what=<color,plate,tint,interior,painting,tuning> ids=<tuning items installed, - none> res=<each setter: name, or name FAILED error>` |
| `CLM` | both | once per copy entity | `ref ent tries` the copy's read back matches the car's look |
| `CLF` | both | once per copy entity | `ref ent what` the copy still differs after 3 tries: given up (a new copy entity is tried again) |
| `CLD` | both | per wait of a copy's tuning | `ref ent ev=<defer: someone sits in the copy, the tuning waits (the rest is set); resume: empty again, the tuning is set now> items=<tuning items>` |
| `CLB` | both | per tuning install on a copy (C++) | `ref ent damage=<1 car damage was told: whole state asked, rebuild echo window>` (InstallTuningItems rebuilt the car) |
| `CDB` | both | each ask of the one-explosion book (C++) | `ent first=<1 it blows up now, 0 refused: blown up already>` (docs/game-api.md 24 "One explosion") |
| `CDE` | both | each car blown up by the one explode routine (Lua D.Explode) | `ent ok=<SetExploded worked> picture=<1 a picture shown> claimed=<1 our own car, asked already> from=<the player whose game said so, - car damage>` |
| `WEH` | host | 1 Hz while in the world | time of day truth (src/game/world_env.h): `t=<game hours 0..24> flow=<time flow speed> weather=<weather set> sT=<session clock, ms, of this reading>` |
| `WEG` | guest | 1 Hz while in the world, and on every SetTime / change of what it does | our clock against the host's: `t=<ours, hours> flow=<ours now (a catch-up's while one runs)> weather=<ours> host=<the host's time now as predicted from his newest WorldEnv> err=<host - ours, game minutes> act=<none: in step or within 0.5 min / flow: a catch-up runs (flow speed changed up to 25 %) / set: SetTime this tick> sT=<session clock, ms> hflow=<the host's flow speed> rate=<game hours per second per flow unit used> measured=<1 the rate is measured from the host's samples, 0 still the guess>` |
| `JN` | guest (host: `report`) | per event | join / reconnect completeness (src/game/join_sync.h): `ev=begin reconnect=<1 back after a lost connection> others=<other players then>`; per first arrival `ev=<player mission worldenv doors npcs traffic police cardamage> key=<player id or car, - one per system> after=<s since begin>`; `ev=complete after=<s: the last required part> missing=<- or the required parts not here 15 s after we were in the world, e.g. doors,player:2> none=<optional systems that sent nothing> late=<1 complete only after the missing report> reconnect`; host: `ev=report from complete after reconnect late missing times=<system:s,...>` (the guest's JoinReport) |
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
| `CHM` | both | each character pick / PlayerModel | C++ (game/model_change.h): `id ev=pick model` (our panel step), `id ev=send model steps=<panel steps folded in> why=<stable panel-closed no-world with-join join>` (ours sent), `id ev=recv model wait=<1 held: inside a burst>` and `id ev=apply model held=<s it waited> skipped=<picks never shown>` (a remote player's; his puppet is made of it from now) |
| `PRT` | both | each puppet entity retired | Lua (player_sync.lua M.Release): `id ent=<entity id> ev=retire why car seat burns`, `ev=release how=<on_foot ...> ok=<1 the spawner released it> seat=<what its old seat reads now: free ours other ->`, `ev=check gone=<0 still there 1 s after the release, 1 gone>`, `ev=gone` (went with the factory or the world first); `id ev=replace from to seated car seat` (a new character takes the old puppet's place); `id ev=lost n=<times>` (the game removed the puppet, e.g. the Free Ride garage clearing its door: spawned again at once; unreleased) |
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
- **NPCs** (guest): host NPCs in range and paired, the distance of ours from its target, the NPCs most often more
  than 3 m off; drive commands, snaps, frozen and refused paired NPCs (`NGS`).
- **NPC replicas** (unreleased; both directions: the host's NPCs on the guest, the guest's own NPCs on the host): each
  replica (`NPR`) against the authority's NPC at the same moment (`NPT`, Hermite through its states with their
  velocities, at the receiver's time mapped to the authority's clock through the player states' clock relation, as
  "on foot"), overall, paired NPCs and spawned copies apart; the controller error; how far the target's time was behind
  the true moment; fixes and snaps by reason; the NPCs most often more than 3 m off. `npc.p95` / `npc.max` for
  `--check`.
- **NPC bandwidth** (unreleased): NPC bytes the host sent to each guest per `NBW` (median / max KB/s) and the NPCs in
  his interest; `npc.bw` for `--check` (<= 8 KB/s).
- **guest-owned NPCs:** how many NPCs became the guest's own (only his game had them), how they ended (died,
  handed back to the host, gone, claim refused), the host's claims, how often the host showed them with a copy and
  how far that copy was from the guest's NPC.
- **traffic:** per direction (each game an authority for its own cars): its cars near the other player, bytes per
  second, unchanged states skipped, budget deferrals; the copy-vs-target error and the copies most often off; the
  true error (`TRC cp` against the authority's `TRS` at the same moment, the clocks lined up by the RX offsets;
  `traffic.true_p95`) and the error at the target's time `rT`; per receiver what was shown (`TRG due / shown`,
  `traffic.shown`), its region share and changes (`TRR`), each authority (`TRA`), and its own cars not shared with a
  player near them 10 s after a region change (`TRL near`, `traffic.unshared_near`). A 0.15 trace is read as before.
- **doors:** per game, the door events by kind and verdict, how changes were attributed, the door messages, the
  applies with a failed call, how many of the changes it sent arrived on the other game, and the first 30 sends /
  receipts / snapshots.
- **car damage:** per game what it sent (by kind), received, set on cars and reported; each player car's last
  state on both games side by side (marked when it differs), cars whose damage could not be read, and the
  traffic cars each game had (damaged ones named).
- **World** (`WEH` on the host, `WEG` on the guest): the guest's time of day against the host's own reading at the
  same session time (both stamped `sT`; the host's two readings around it interpolated, the short way round 24 h;
  without a host trace the guest's own `err`): how many readings, what the guest did (none / flow / set) and every
  SetTime, `|host - guest|` after the first 10 s of each stretch (a gap of over 5 s between guest readings starts a new
  stretch: a load or a rejoin; its first sync is left out) as mean / median / p95 / max and the worst moment, the
  guest's own estimate against the truth, how often the weather was the same, the game's rate the guest measured.
  Target `world.time_err` = the max of those (<= 0.5 game minutes).
- **Join and reconnect** (`JN`): each join / rejoin of the guest, the first arrival per system, complete after how
  many seconds or what was still missing, and the host's copy of the report. Target `join.complete_s` (<= 10 s) for
  every complete that came in time (a "missing" verdict records its time, over 15 s: FAIL).
- **reconnects:** per game, the `RCN` events (dropped, lost, back with the time without the host, sync paused /
  resumed).
- **explosions:** none of the X records are read by the tool yet; grep them (`XC` on the thrower, `XR`/`XV`/`XD` on
  the others: a sent explosion's `seq` pairs them). Throws (G records, not read by the tool either): `GT`/`GX out`/`GE out`
  on the thrower and `GR`/`GX in`/`GV`/`GE in` on the others pair by `seq`; the explosion that ends a throw names it
  (`XO`/`XR throw=`).
- **worst moments:** the largest errors at least a second apart, with the wall-clock time on both games.

`--csv DIR` writes the lined-up series (`foot-p<id>-on-<role>.csv`, `car-p<id>-on-<role>.csv`) for plotting.
