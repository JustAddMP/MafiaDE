# Sync trace

Every co-op session writes a trace next to the log, for comparing the two games afterwards: what each game sent, what
the other received and when, what the motion controllers decided, and what the game really did (read back). The
main log stays readable; the trace is the high-rate record. Since 0.14.0 (core 0.10.0 `core/trace.h`). Always on, no
setting.

- **Where:** `logs\trace\<log name>-<date>-<time>.trace` in the mod's folder (next to `MafiaCoop.exe`), e.g.
  `logs\trace\mafiacoop-20261017-150912.trace`. One file per session (opened when the session becomes active, closed
  when it ends; a guest's reconnect keeps it open, `RCN` lines mark the gap). The newest 20 are kept and the folder
  holds at most 4 GiB (before a new trace opens the oldest go until the rest take 3 GiB); one file stops at 1 GiB
  (about four hours; a last `END` line says so) (core `core/trace.h` kKeepFiles, kMaxFolderBytes, kMaxBytes).
- **Size:** about 60-80 KB/s with one other player (~250 MB an hour). It is written twice a second, so a crash loses
  at most the last half second.
- **Also kept now:** the last ten main logs per log name (`mafiacoop-host.log.1` newest .. `.10`; core
  `core/log.h` kKeepRuns).
- **Bundles:** at the end of every session a zip of everything about it in `logs\bundles\` (see "Bundles" below).
- **Comparing:** `python tools/sync_compare.py <trace from one PC> <trace from the other> [--csv DIR] [--worst N]
  [--check]` (standard library only). See "What the report says" below. `--check` ends the report with every sync
  target of docs/sync-architecture.md section 4 (PASS / FAIL / no data per direction; exit code 1 when one fails).

To report a test: send the newest zip from `logs\bundles` of both PCs (one bundle per session and role, e.g.
`MafiaCoop-20261020-143610-host.zip` and `MafiaCoop-20261020-143622-guest.zip`). `sync_compare.py` reads a bundle as
it is. The whole `logs` folder is still the complete picture (older sessions, other runs).

## Line format

`<t> <TYPE> <key=value ...>`: `t` is this machine's QueryPerformanceCounter clock in seconds (4 decimals), the same
clock the game's network timestamps (`sT`) are taken from. Vectors are `x,y,z` (metres, z up); speeds m/s. Lines of
one file are in time order (per writing thread). Every line of the main log is copied in while the trace is open
(`<t> 2026-10-17 15:09:12.013 [92912] INF text`), so the file holds the whole session.

**Lining up two machines:** a state's `sT` (sender clock, ms) is in the sender's `TX` and in the receiver's `RX`:
that pairs them exactly. The receiver's `off` (its clock minus the sender's, smallest seen, so it includes the
shortest one-way trip) and the round trip (`CLK rtt`) give the clock difference: `receiver - sender = off - rtt/2`.
The tool uses that to compare where a puppet stood on one game with where the player really was at the same moment
on the other. Since session-logs both games also write `SCK` every 5 s (their session clock offset to the host, the
NTP-like clock of core's session pings); the tool lines the traces up by those when both have them (the `CLK sclk`
field in older traces; the RX method otherwise) and prints both methods side by side ("Clocks"). On the one-PC runs of
2026-10-20 (both games on one clock: the truth is 0) the session clocks were 0.3 / 0.8 ms off, the RX method 1.8 -
2.2 ms. The senders' `sT` stamps are 32-bit milliseconds: on a PC up for more than 49.7 days they wrap, which the tool
takes out (the "stamps' 32-bit wrap" in its Clocks section).

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
| `NHS` | host | 2 Hz | `sending hits kills` (0.16: also `mirrors shown lateKills` of the retired guest-owned NPCs) |
| `NP` | guest | 2 Hz per host NPC in range | `key loc=<our id; hidden:<id> for a paired NPC that did not follow> copy ai car=<0 on foot, 1 its car flag, 2 faster than 11 m/s: taken as in a vehicle> spd=<its speed, m/s> host=<pos> our=<pos> d=<m> hp=<host>/<ours> dead=<host>/<ours> age` (`spd`, `car=2` unreleased; unreleased: `host` is the replica target, the host's NPC interpolated and led to now, `spd` its velocity: not lined up with the host in time, `NPR` / `NPT` are) |
| `NGS` | guest | 2 Hz | `live matched copies avg max kills snaps=<replica snaps> held=<our NPCs held now> holds=<held so far> unholds=<shown again so far> unpairs=<pairs released: the host's NPC in a vehicle, ours on foot over 40 m off> refused=<paired NPCs that did not follow: hidden, a copy shown> fallbacks=<copies spawned with a fallback profile> frozen=<paired NPCs whose AI was interrupted> drives=<drive commands applied>` (unreleased: `snaps refused frozen drives` replace `pulls steers`; host authority: `held holds unholds` replace `own claims`) |
| `NPT` | the authority (the host; 0.16: a guest for his own NPCs, retired) | per NPC state sent (15 Hz near a receiver to 5 Hz at 120 m) | one authority per NPC, the truth: `key=<its entity id here> sT=<sample time, ms, this game's steady clock as TX's> p=<pos> v=<vx,vy m/s> g=<gait 0-3 from its speed>` (C++, unreleased) |
| `NPR` | the receiver (guest; 0.16: the host for guests' own NPCs, retired) | 10 Hz per driven replica | `key=<the authority's key> from=<the authority's player id> kind=<1 a paired NPC, 2 a spawned copy> rT=<the authority time its target stood for, ms (render time + lead)> cp=<the replica where our game shows it> tp=<its target> err=<cp vs tp, m> fix=<0 none 1 nudge 2 snap> why=<-, far, height, stuck, teleport, blocked> g=<commanded gait> sg=<the authority's gait>` (C++, unreleased) |
| `NPS` | the receiver | per replica snap | `key from kind err=<m> why=<far, height, stuck, teleport, blocked>` (C++, unreleased) |
| `NBW` | host | every 10 s per guest | `to=<guest> kbs=<NPC states + infos + leaves sent to him, KB/s> near=<NPCs in his interest>` (C++, unreleased) |
| `NM` | host (0.16, retired) | 2 Hz per mirror (a guest's own NPC) | `mid from key=<its id on the guest's game> loc=<our copy> st=<copy state> ai guest=<pos there> our=<copy pos> d=<m> hp=<guest>/<copy> dead=<guest>/<copy> age` |
| `NO` | guest (0.16, retired) | 2 Hz per own NPC | `key p hp dead ai` |
| `NHD` | guest | per hold decision | held NPCs (src/game/npc_hold.h, C++): `key ev=<hold/show/kill> host=<the host NPC that paired with it, 0 none> why=<unpaired: no host NPC for 5 s / paired: a host NPC is it, its replica now / host_dead: that host NPC is dead, ours dies with it / session_end: shown with its own AI>` |
| `NHL` | guest | 2 Hz per held NPC | Lua: `key p hp dead ai for=<s held>` |
| `NGO` | guest (0.16, retired) | on change | `key owned=<1 ours now, 0 not any more>` (C++) |
| `NGE` | both (0.16, retired) | per GuestNpcEnd | `[from] key why=<0 died 1 handed back 2 gone 3 refused> host=<host NPC> hp byHost` (C++) |
| `NGC` | guest (0.16, retired) | per NpcClaim | `key host dead ours` (C++) |
| `NMC` | host (0.16, retired) | per claim sent | `mid to key host dead` (C++) |
| `NMX` | host (0.16, retired) | mirror end | `mid why host hp` (C++) |
| `NRL` | host (0.16, retired) | when the count changes | several guests: `to n=<other guests' own NPCs relayed to him this round> was` (C++; npc_owner.h "relay", deleted) |
| `NRH` | host (0.16, retired) | per forwarded hit | several guests: a guest hit another guest's own NPC (its relay key): `from owner key=<the owner's id of it> amount type lethal` (C++; sent to the owner as HostNpcHit with `from` as the attacker) |
| `SEA` | both | per seat yield | several players in one car: `key from=<our seat> to=<seat we slide to> claims=<id:seat of the others' states> ok` (Lua, src/game/seats.h) |
| `SCP` | both | per change | where a remote player's car copy stands (Lua, seats.h `PlaceCopy`): `key ev=place dec=<wait at_car aside hold release> blk=<m to the nearest other car, -1 none> for=<s his spot has been blocked> spot=<candidate:x,y,z beside his car, - none>`; `key ev=oldcopy drop=<key of the parked copy of his earlier car dropped from his spot> last=<its last driver>`; `ev=world dropped=<records looked up again in a new world>`; `key ev=reseat id seat aside=<1 copy beside his car>` (the puppet is seated anew: the record's car is another entity now); `key ev=lookwait id seat` (the puppet waits for our new copy's look before it is seated, car_look.h holds) |
| `DRV` | both | once per conflict | two other players claim the wheel of one car: `key keeps=<the driver here> claims=<the other one>` (Lua) |
| `WHL` | both | per event | one wheel, one driver (Lua, src/game/seats.h `LocalWheel`; unreleased): `key ev=yield to=<the keeper> why=<slide: he drove it here, the game slid us over / keeper: the holder or the lower id> holder` (we give way, claim seat 9 from now); `key ev=keep other holder drv` (we keep it, he gives way on his game); `key ev=slide seat ok try` / `key ev=out ok try` (M.WheelTick moving us); `key ev=done to seat=<where we sit now, nil out> took=<s>`; `key ev=stuck to` (slides and get-outs did not move us: we stay, claiming nothing); `key ev=hold on=<1/0> ok` (PlayerShouldRemainPassenger set as a passenger of another player's car); `key ev=bike to holder` (a motorbike whose saddle another player's state claims too and he keeps: we count as the rider behind him, seat 1, nothing moved) |
| `PAU` | both | per change | a paused game (src/game/peer_pause.h; unreleased): ours `id=<us> ev=<local-paused local-repeat local-resumed> reason=<1 no script tick 2 the game's paused tick> ms=<paused so far / in all>`; another's PeerPaused `id ev=<paused resumed> reason ms`; his copies here `id ev=freeze car=<key of the car copy held, - on foot> pup=<1 from its fresh report>` and `id ev=thaw held=<s>` |
| `DRK` | both | his car key changes | a remote driver's car key changed (a reload, a rejoin, another car): `key ev=newkey id was=<his key before> st=<our record's state: new obtain spawn ready failed>` (Lua) |
| `TRH` | host (the only traffic authority; 0.16: every game with traffic) | 2 Hz | the host's traffic totals: `cars=<his traffic cars near guests> spawnMsgs stateMsgs despawnMsgs bytes=<all traffic bytes sent> bumps=<TrafficBump received> nodrv=<cars without a driver read now> orphan=<of those, confirmed driverless (1 s, slow)> orphaned=<confirmed so far> driven=<found driven again so far> far=<cars without a driver read farther than 40 m from his player: the game streamed the driver out, counted as driven (kDriverLod; not in nodrv)> ignored=<traffic messages a game ignored: a guest's traffic, another guest's, the retired 0x0224-0x0227 (Route)>` (`far` and `ignored` unreleased) |
| `TRS` | host | per car, when its state went to a guest, at most 10 Hz | the truth of a host traffic car: `net sT=<his state clock, ms: the stamp of the TrafficStates it went in> p=<x,y,z> v=<vx,vy,vz>` (full precision); tools/sync_compare.py lines a guest's `TRC` up with it, at the same moment (the clock relation) and at `rT` (traffic.true_p95) |
| `TRO` | both | each event | traffic drivers and crews. Host (his cars): `key ev=miss why=<seat_0_empty, seat_0_holds_NAME_no_generator_human, its_driver_is_dead, seat_read_failed (spaces as _ , other characters dropped)> drv=<active inactive -> the human in seat 0> spd=<m/s> dist=<m from his player> far=<1: beyond 40 m, where the game streams traffic drivers out: the car counts as driven>`; `key ev=back after=<s without a driver read> how=<scan round>`. Guest (his copies of the host's cars): `net ev=out` (the copy's driver got out: its car driverless and both stand), `net ev=in` (the host's car is driven again: back in), `net ev=reseat seat try ok=<GetOnVehicle ran>` (a crew member put back in his seat) (0.16 had `a=<authority>` in front; unreleased: gone) |
| `TRP` | host | 2 Hz per guest | `id relevant=<his cars in the guest's range> admitted=<cars the guest has copies of> spawned despawned states skipped=<unchanged, not sent> bytes deferred=<due but over the budget, this tick>` |
| `TRG` | guest | 2 Hz | `known live copies avg max=<copy vs target, m> pins snaps bumps states spawns despawns old=<states dropped as older> frozen=<1 the host is paused: his cars' copies held> quiet=<knocks not reported: the host paused or just resumed, or our own pause> due=<the host's cars within 150 m of us, at most 20> shown=<of those, the ones with a copy that drove this tick> blends fsnaps=<corrector blends / snaps so far> pd=<the host's playout delay, ms>` (unreleased: 0.16's `mode` / `auth` gone, `pd` new; a 0.15 TRG has `mode=1` while the host's traffic showed) |
| `TRA`, `TRR`, `TRL` | - | retired (unreleased) | 0.16's per-authority receiver totals, region changes and a game's own leftover traffic (the region authority, src/game/traffic_model.h RegionFollow): gone with it; tools/sync_compare.py only counts them in a 0.16 trace. A guest's own cars are guest_world's now (`GW`, docs/host-authority.md WP-A) |
| `TRZ` | guest | per change | the host's pause and ours for the traffic copies: `ev=<freeze thaw> copies quiet=<knocks not reported so far>`; `ev=localgap copies quiet` (0.16 had `a=<authority>`) |
| `TRC` | guest | 2 Hz per traffic copy, at its first report after the mark | `net rT=<the host's clock, ms, that the target stands for: render time + lead> tgt tv cp=<the copy after this tick's commands> cv err pin pins f=<state flags: 1 handbrake 2 siren 4 no driver (confirmed, 1 s without a driver read while slow) 8 controls 16 reversing> pp=<x,y,z: the copy before this tick's commands, where the game had it (cp is after our SetPos / nudge); - unknown>` (`err` 1e9 before the first report; a 0.15 TRC has no `rT`; a 0.16 TRC has `a=<authority>` first: tools/sync_compare.py takes `a=0` (the host's) and counts the others) |
| `PDT` | host (the pedestrians' authority; 0.16: every region authority) | 1 in 2 states sent per pedestrian (5 Hz near) | pedestrian truth (unreleased): `net=<its net id> id=<entity id here> sT=<our clock at the sample, ms: the authMs of its PedStates> p=<where it really was then>` |
| `PDR` | guest | 10 Hz per copy | `from=<the host> net rT=<the host's clock time the target stands for, ms: render time + lead> cp=<our copy> tp=<its target then> err=<cp vs tp, ground> how=<spawned (made for it) / reused (a pool copy put there); 0.16: adopted> g=<gait sent>`; sync_compare lines `rT` up with the host's `PDT sT` for the true error (not across a jump of the truth faster than 12 m/s: the host's game moved the pedestrian; unreleased) |
| `PDC` | guest | 1 Hz | `near=<the host's living pedestrians within 40 m of us> rep=<of them with a copy here> spawned=<copies standing for a pedestrian> pool=<free copies (hidden or about to be)> loading known=<the host's pedestrians we have> snaps fails=<spawn failures, total> asked reuses handovers hides removes fcreate fprofile freset ftimeout lost resets lagsnaps unresp profiles=<looks still in the rotation>` (`ped.coverage` = rep / near; `ped.copy_fail_per_min` = the rise of fails per minute of records with known or spawned > 0); 0.16: `hidden adopted` instead of `pool` and the counters after `asked` |
| `PDS` | host (0.16: both) | 1 Hz | our pedestrians: `peds=<our ambient humans near players> owned=<of them shared> bytes states spawns gones deaths killreq riding rides` (totals) |
| `PDP` | host (0.16: both) | 1 Hz per guest | `id relevant admitted states skipped bytes deferred` (our pedestrians for him) |
| `PDE` | both | per event | guest: `ev=spawn (prof) / reuse (why: handover) / hide / remove (why: body, surplus, unresponsive, cancelled) / kill / diedhere / death / lagsnap / spawnfail (why) / spawnerreset (why) / spawnerwasreset / profiledrop (prof) / gone`; host: `ev=killreq`; `key=<host << 16 + net> local=<entity id here> p from net` (0.16: `adopt / release / show` too) |
| `PDL` | both | 1 Hz | ped_sync.lua: `amb=<ambient humans here (host)> near=<reported near players (host)> copies=<bound copies> pool=<free ones> hidden=<copies hidden> queued fac=<copy spawner state> prof=<the look loaded now>` (0.16: `adopted spawned queued hidden fac blocks`) |
| `GW` | guest | 1 Hz (and once when everything is given back) | guest_world (src/game/guest_world.h, docs/host-authority.md WP-A; unreleased): `ph=<0 no world 1 loading screen 2 in the world> w=<worlds seen (loads)> inw=<s in this world, -1 not in one> on=<1 our switches stand: ambient traffic and police spawning off, pedestrians off> mode=<budget block off: how our pedestrians are stopped (block: the fallback after a failed SetMaxHumanElements, or the ini)> hide=<0 nothing 1 hidden at once (loading screen, the first 3 s) 2 only out of view> cars=<our own moving traffic cars (a generator driver at the wheel) within 150 m still shown after the last scan> peds=<our own ambient humans within 80 m still shown> hidC hidP=<cars / humans hidden now> hid=<hidden so far this session> cand=<our own ambient cars and humans seen in the last scan> bud=<the pedestrian budget read before we set it, -1 unknown> bs=<budget: 0 untouched 1 set to 0 2 put back 3 a call failed 4 the old value unknown (left to the next world load)> blocks=<spots BlockSpawnersInSphere'd (fallback)> ra=<re-applies (every 5 s, or after other glue switched traffic on)> rs=<restores>` |
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
| `LSH` | guest (host: `send`, `rx`) | per event | the leash (src/game/leash.h, docs/host-authority.md 3): `ev=config warn max cd on from=<host: his LeashConfig / default>`; `ev=state st=<near warn countdown paused pulling pulled suspended> why=<- or the suspension: off loading afterload failreload joining nohost hostpaused cutscene selfdown hostdown hostteleport hostcar passenger> dist=<m to the host's puppet, -1 none> left=<countdown s> warn max cd` on every change of state or suspension; `ev=pull` (the same fields) when the countdown ran out and the pull is carried out; `ev=pulled ok=<1 we were moved> how=<foot road beside, or why not: nohost nearhost passenger notinworld error>`; `ev=hold n=<holds after this pull> z=<our height> spot=<the pull's>` (unreleased: within 3 s of a pull we were more than 1.5 m under the spot it put us on - the ground not loaded yet - and are put back on it, game/leash.h Settle); host: `ev=send warn max cd on to=<new / all>` (LeashConfig sent), `ev=rx from kind=<warn countdown pulled back> dist repeat=<1 the 5 s repeat of a warn / countdown>` |
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
| `END` | both | at the end | `why=<session-detached session-inactive process-exit>` (`via=<ExitProcess TerminateProcess>` at a game exit with the session open: core writes it, then the summary and the bundle) |
| `SES` | both | at the start of the session (core: dll/session_log.h), per player, again when the leash in force changes | the session header, one record per part; the last field (`name=`, `value=`, `path=`, `target=`) runs to the end of the line. `part=build mod ver core build=<handshake build number> proto core_built=<dll build date> exe_crc=<CRC32 of the game exe, hex; pending while it is still being read> exe_ts=<its PE timestamp> exe_label=<the GameInfo build it matches, unknown_build>`; `part=me role id steam=<SteamID64, 0 none> steam_ready name=<our player name>`; `part=persona name=<Steam persona>`; `part=net mode=<host, join_(udp), join_(steam_lobby)> transport=<udp+steam, udp, steam> port=<UDP port hosting, 0> lan=<this PC's LAN IPv4> lobby=<Steam lobby id> relay_kbs=<per-peer budget> password=<1 set; never its value> target=<what a guest connects to>`; `part=window x y w h client=<WxH> bb=<backbuffer WxH> fmt=<DXGI format> windowed fullscreen refresh style maximized minimized monitor=<\\.\DISPLAYn> mon=<x,y,WxH> primary dpi`; `part=sys win=<Windows major.minor.build> cpus ram_mb ram_free_mb fps fps_min=<last 10 s>`; `part=ini key src=<file env set> value=<the setting in force; password-like keys (password, passwd, pwd, secret, token) <redacted>>` per setting; `part=path what=<log trace bundle ini exe dumps> path=<...>`; `part=peer id seq=<joinSeq> me host steam via=<udp steam -> addr=<UDP: the peer's IPv4> model later=<1 joined after the start> name=<...>` (every player, also each later join and rejoin); Mafia DE: `part=leash role=<host guest> warn max cd on pull from=<settings: the host's own; host: a guest got the host's LeashConfig; default: not yet>` |
| `SCK` | both | every 5 s (core) | session clock sample: `sclk=<the session clock now, ms> off=<host - local, ms; 0 on the host> err=<its uncertainty, ms> rtt=<to the host, ms; -1 on the host> synced=<1 the guest's clock has a sample>`; line time t = this machine's trace clock at that moment: host trace time = guest t + off |
| `SUM` | both | every 10 s (core) | `part=peer id rtt=<ms> loss=<%> kbo kbi=<KB/s out / in to that peer> pko pki=<packets/s> rs=<resends in the 10 s> q=<reliable units queued>` per other player; `part=net dt kbo kbi lanes=<lane:out/in KB/s,...> fps fps_min=<lowest frame of the 10 s as fps> warn err supp=<log warnings / errors / rate-limited lines: in the 10 s / in the session> off off_err=<session clock offset, ms>`; `part=game <name=value ...>`: the game's numbers (core/diag.h; levels as their value, counts as `<added in the 10 s>/<session total>`; several lines when long), Mafia DE: `traffic.*` (guest: known copies due shown err_avg err_max pins snaps fix_snaps blends bumps spawns despawns states_old; host: cars spawn_msgs despawn_msgs bumps_rx orphaned p<id>.admitted p<id>.relevant), `ped.*` (guest: near near_with_copy copies pool_free known snaps spawn_fails spawns_asked reuses hides removes lost lag_snaps spawner_resets; host: peds shared spawns_sent gones_sent deaths_sent), `npc.*` (guest: known tracks driven snaps refusals dropped_states held held_total held_paired_later; host: published states_sent infos_sent leaves_sent hits_rx), `player.*` (remotes p<id>.hz p<id>.dropped foot_snaps car_pins car_retries car_snaps), `leash.*` (dist state state.<name> ev.pull), `world.*` (suppressed own_cars_shown own_peds_shown hidden reapplies), `vis.<kind>.<ev>` (every VIS event), `lua.err.<module>` (Lua errors per module) |
| `VIS` | both | per event (game/vis.h; at most 60 a second per kind, then one `ev=dropped n=<count>`) | every authority / visibility event with a key that is the same on both PCs: `kind=<player traffic ped npc held> key=<player: player id; traffic: the host car's net id; ped: host << 16 + net id; npc: the host's entity id; held: the guest's own entity id> ev dir=<tx: the host decided it for a guest, rx: from the host / on our screen, -: local> peer=<the other player, -> [extra]`. Host: traffic / ped / npc `ev=admit` (with `p=` or `net= ai=`) and `ev=leave` per guest (`peer`). Guest: the same from the host (`dir=rx`); traffic `show` (`p=`), `hide` (`grace=`), `lost` (the game removed the copy), `taken` (a player took it); ped `spawn` (`prof=`), `show` (the copy is in the world: `local= prof=`), `reuse` (`local= why=`), `remove` (`why=`), `kill`, `lagsnap` (`err=`), `spawnfail` (`why=`), `death`, `diedhere`; npc `show` (driven: `rk=<1 a paired NPC 2 a spawned copy>`), `switch` (`rk` changed), `hide` (not driven any more), `forget` (no states for 3 s); held `hold` / `show` / `kill` (`host= why=`); player (his puppet here): `spawn` (`after= world= lost=`), `lost` (`n=`), `hide` (`why=`), `show` (`was=`), `respawn` (`why=<back died_here>`), `replace` (`to=`), `despawn` (`why=`), `remove` (`why=<left session_end>`) |
| `FIN` | both | at the end of the session (core) | `part=session dur=<s> role local peers=<ids> start end=<wall clock>`; `part=peer id secs out_mb in_mb kbo kbi pko pki rtt_avg rtt_max loss_avg loss_max resends name=<...>`; `part=net msgs_out msgs_in out_mb in_mb lanes=<lane:MB out/in>`; `part=msgs top=<name:sent/received count:MB out/MB in,...>` (12 busiest messages); `part=game <the SUM game numbers as session totals>`; `part=log warn err supp dropped_names`; `part=repeat n level fmt=<format string>` (the 5 most frequent warning / error call sites of the session); `part=bundle path=<the session bundle being written>` |

`rb` values come from the car's vehicle part (docs/re-notes.md 9); `eng gear spd fixed rpm pwi` from the script
getters (IsEngineOn, GetGear, GetSpeedFloat, IsCarPosFixed, GetEngineRotations, GetPowerInternal). A `-` means the
call failed on that car.

## Session header, summaries and bundles

Core writes these for every session (dll/session_log.h), the game layer adds `SES part=leash` and the `VIS` records:

- **Main log:** a block `session: ---- session start: <role>, player <id> '<name>' ----` with the versions, Steam
  account, transport and addresses, window and monitor, OS and fps, every setting (password-like ones `<redacted>`),
  the file paths and each peer (`session: peer 1 'Miles': over Steam ...`); every 60 s `session: summary (60 s): ...`
  (latency, loss, fps, warnings and errors added, the game's numbers with what they added); at the end
  `session: ---- session end: ...` with the totals, the worst latency, the game's counts, warnings / errors and the
  five most repeated ones, and the bundle (`session:   bundle: <path> (being written)`, then
  `bundle: wrote <path> (N files, X MB -> Y MB, T s)`).
- **Repeated warnings and errors** (core/log.h): each call site writes at most 10 lines a minute; then
  `repeated N more times in 60 s (rate limited; last: <the last one>)`. The counts are in `SUM` / `FIN`.
- **Lua errors:** every module's "Update error #n" goes through one helper (game/lua.h ReportError: the first 20 per
  module, then every 200th, counted as `lua.err.<module>`); with the Lua stack when the game's Lua has
  `debug.traceback` and its pcall runs a message handler (checked once at the first tick: `lua: error tracebacks on
  (...)` or `lua: error tracebacks off: ...`); the stack's lines are joined with ` | `.
- **Bundles** (core/bundle.h): `logs\bundles\MafiaCoop-<yyyymmdd>-<hhmmss>-<role>.zip` (the session's start = the
  trace's name; role host / guest) written at the end of each session, and at a game exit with a session open (the
  exit waits for it, at most 20 s). In it: `trace/<the session's trace>`, `logs/<the main log>`, `MafiaCoop.ini`
  (password-like values `<redacted>`), `logs/launcher*.log` of the last 24 h, `dumps/crash-*.dmp` written since the
  session started, `manifest.txt` (versions, exe CRC, role, the files and what was cut or left out). Written to
  `<name>.tmp` on a worker thread and renamed when complete; the newest 30 bundles are kept. At game start, the
  newest trace of this log name without a bundle (the game crashed or was killed) gets
  `MafiaCoop-<its start>-<role>-recovered.zip` with the previous run's log (`logs/<name>.log.1`). Zip without zlib:
  core/zip_writer.h (deflate: LZ77 with hash chains and dynamic Huffman blocks, CRC-32, no zip64: an entry stops at
  2 GiB, the archive takes no new entries past 3.8 GB). Measured on the 2026-10-20 one-PC traces: 28.9 MB in 0.16 -
  0.18 s (165 - 180 MB/s), 7.3x smaller; 21.6 MB -> 2.3 MB (9.5x); 20.0 MB -> 2.2 MB (8.9x).

## What the report says

Per player as seen on the other game:

- **network:** states sent and received in that time, loss, states dropped as old, the clock relation, one-way
  latency (mean, median, p95, max) and jitter.
- **on foot:** the distance between the puppet and the player at the same moment (the player's own `TX` position at
  the receiver's time mapped to his clock), overall and per gait the player was in; the controller's own error
  (puppet vs network target); how often the puppet's gait matched his and the most common mismatches; snaps, nudges
  and how often it was catching up (`carry`). Left out of every foot number and counted apart (unreleased,
  onepc-fixes): 3 s after each of his own teleports (a jump of more than 50 m at more than 70 m/s between two of his
  `TX` states, or his `LSH ev=pulled ok=1`: his puppet is put there and may fall with him), and the time he was more
  than 300 m from the receiving player (no world there on that game: the leash's warning distance). `foot.shown`
  leaves out the receiver's loading screen before his first world (`local player in world`), and a respawn after
  `puppet N: its player is back, respawning it` (he was downed) has a reason.
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
- **NPC replicas** (unreleased; the host's NPCs on the guest; a 0.16 trace's guest-owned NPCs on the host are no
  longer listed): each
  replica (`NPR`) against the authority's NPC at the same moment (`NPT`, Hermite through its states with their
  velocities, at the receiver's time mapped to the authority's clock through the player states' clock relation, as
  "on foot"), overall, paired NPCs and spawned copies apart; the controller error; how far the target's time was behind
  the true moment; fixes and snaps by reason; the NPCs most often more than 3 m off. `npc.p95` / `npc.max` for
  `--check`.
- **NPC bandwidth** (unreleased): NPC bytes the host sent to each guest per `NBW` (median / max KB/s) and the NPCs in
  his interest; `npc.bw` for `--check` (<= 8 KB/s).
- **held NPCs** (host authority): the guest's NPCs with a job that no host NPC paired with for 5 s, held hidden
  (`NHD`, `NHL`): how many, how many a host NPC paired with later (shown) or killed, released at the session's end, the
  longest held; and what the guest sent of his own NPCs: his `NPT` states and the guest-owned NPC messages
  (0x0421-0x0426, by name or id) going out in his NetMonitor reports, `npc.guest_sent` for `--check` (target 0; a
  0.16 trace also shows what its guest-owned rule made his own: 1350 states in session A of 2026-10-20).
- **traffic** (host authority: the host's cars on the guest's game only): the host's cars near the guest, bytes per
  second, unchanged states skipped, budget deferrals, the guests' traffic messages he ignored (`TRH ignored`); the
  copy-vs-target error and the copies most often off; the true error (`TRC cp` against the host's `TRS` at the same
  moment, the clocks lined up by the RX offsets; `traffic.true_p95`) and the error at the target's time `rT`; what the
  guest showed (`TRG due / shown`, `traffic.shown`) and the playout delay. A 0.15 trace is read as before; in a 0.16
  trace only the host's cars on the guest are compared (the region authority's other copies, `TRA` / `TRL` / `TRR`, are
  counted in one line; `traffic.unshared_near` is gone: a guest's own cars are guest_world's `world.guest_own_cars`).
  Where the true error comes from: standing / moving cars, along / across the way the car drives, by distance from
  the guest's player, the copies of a standing car driven off it (moving >= 2 m/s away from their target, the 0.16.0
  copy speed), the copy before our commands (`TRC pp`). Drivers and crews (`TRO`): the host's misses and how many were
  beyond 40 m (the game's level of detail), the guest's crews out / back in (within 2 s: a flap) / put back per minute
  with copies (`traffic.crew_exits_per_min`, `traffic.reseats_per_min`).
- **guest world** (unreleased, `GW`): per world the guest loaded, his own moving cars and ambient humans still shown
  near him from 10 s after the load (`world.guest_own_cars`, `world.guest_own_peds`: both must be 0), what was hidden,
  how his pedestrians were stopped, the budget read / set / put back, blocked spots and re-applies; and whether
  everything was given back at the end.
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
- **Leash** (`LSH`): per game the config, the state changes (the first 60), every pull with its distance and the
  answer (`pulled ok how`), the host's received events. Metrics `leash.pulls` (info, >= 0) and `leash.false_pulls`
  (a pull while a suspension held or under the max distance; target 0).
- **reconnects:** per game, the `RCN` events (dropped, lost, back with the time without the host, sync paused /
  resumed).
- **explosions:** none of the X records are read by the tool yet; grep them (`XC` on the thrower, `XR`/`XV`/`XD` on
  the others: a sent explosion's `seq` pairs them). Throws (G records, not read by the tool either): `GT`/`GX out`/`GE out`
  on the thrower and `GR`/`GX in`/`GV`/`GE in` on the others pair by `seq`; the explosion that ends a throw names it
  (`XO`/`XR throw=`).
- **worst moments:** the largest errors at least a second apart, with the wall-clock time on both games.
- **session headers** (`H`, `SES`): both games side by side; the fields that must be the same (mod, core, protocol,
  state version, state rate, session build, exe CRC, the leash in force) are flagged `<-- DIFFERENT`
  (`session.mismatch` for `--check`, target 0; only when both traces have SES); the settings, paths and peers.
- **clocks** (`SCK`, `CLK sclk`, `RX`): each game's session clock offset, the relation by the session clocks and by the
  RX offsets and their difference; when both traces come from one PC (the same QueryPerformanceCounter zero) the
  true relation is 0, so the values are the methods' errors.
- **periodic summaries** (`SUM`): per game and peer rtt / loss / KB/s (median, max), fps, per lane, the game's levels
  (median / max) and counts (session totals).
- **visibility per entity** (`VIS`; a trace without VIS: the pedestrians' `PDE`): per kind the host's admits / leaves
  and the guest's events, the time from the host's admit to the guest showing it (p50 / p95 / max, the slowest
  entities), admitted entities never shown, how long shown (and flaps under 2 s), failed / lost copies and pool reuses,
  the worst entities by copy error (`TRC` / `PDR` / `NPR`); per puppet its events with the wall-clock time; held NPCs.
- **end of session** (`FIN`): per game the totals; "no FIN" means the game did not end the session (crash or kill: its
  `-recovered` bundle has the trace).

`sync_compare.py` takes a bundle zip wherever it takes a trace (it reads the biggest `.trace` in it).

`--csv DIR` writes the lined-up series (`foot-p<id>-on-<role>.csv`, `car-p<id>-on-<role>.csv`) for plotting.
