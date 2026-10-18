# Sync trace

Every co-op session writes a trace next to the log, for comparing the two games afterwards: what each game sent, what
the other received and when, what the motion controllers decided, and what the game really did (read back). The
main log stays readable; the trace is the high-rate record. Since 0.14.0 (core 0.10.0 `core/trace.h`). Always on, no
setting.

- **Where:** `logs\trace\<log name>-<date>-<time>.trace` in the mod's folder (next to `MafiaCoop.exe`), e.g.
  `logs\trace\mafiacoop-20261017-150912.trace`. One file per session (opened when the session becomes active, closed
  when it ends). The newest 20 are kept; one file stops at 1.5 GiB (about six hours).
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
| `TX` | both | each state sent (60 Hz) | `sT=<ms> p h v m=<0 idle 1 walk 2 run 3 sprint> f=<flags: 1 car, 2 crouch, 4 sprint, 8 aim, 16 reload, 32 in air, 64 cover> hp sh=<shots> w=<weapon>`; in a car also `car=<key> mdl seat cp q cv ctl=<valid,steer,power,brake,handbrake,gear>` |
| `RX` | both | each state received | `from sT off=<s> ...` the same fields as `TX` |
| `RXD` | both | dropped state | `from sT last` (older than or equal to the newest) |
| `CLK` | both | 1 Hz per remote | `id off rtt=<ms> hz=<received rate> snaps=<buffered> newest=<ms ahead of now> recv drop last=<s since the last state>` |
| `PUP` | both | 60 Hz per remote on foot (plus every snap) | `id rT=<the remote time the target stands for, ms> tgt tv have=<copy position known> age=<s> cp=<puppet> ch=<its heading> err g=<gait 0-3> mult want meas carry base=<walk,run,sprint calibrated> fix=<0 none 1 nudge 2 snap> nudge re=<walk re-aimed> mv=<walk point> turn sm=<his mode> sf=<his flags>` |
| `VEH` | both | 60 Hz per remote driver (plus pins, retries, snaps) | `id key rT have tgt tv cp cv err rot=<deg> fix=<0 none 1 blend 2 pin 3 snap> pin why=<0 - 1 too slow 2 ran off 3 stays off> jp=<pinned now> rt=<physics retried now> pins tsp csp vel=<wanted copy velocity> pw ctl st br hb gear` |
| `PAS` | both | 60 Hz per remote passenger | `id key rT seat cp` |
| `LR` | both | 20 Hz | our player read back: `p d spr hp`; in a car `car seat cp cv rb=<steer wish,power,brake,handbrake,steer linearity,front wheel angle (rad)> eng gear spd fixed rpm pwi` |
| `PR` | both | 20 Hz per puppet | `id st p d hid veh seat own=<car it sits in> seated ejects tries anim phys spr dead hp mv mult moves snaps sown=<who holds its seat: ours/free/other/?, - on foot> brain=<friend brain set> ai=<GetAIType> back=<put-backs into the seat> leaving=<out of its seat inside the car now> ejAll=<throw-outs this session>` (the last six since 0.15, docs/game-api.md 17) |
| `CR` | both | 20 Hz per driven car copy | `key drv adopted fix pin p v rb=<as LR> eng gear spd fixed rpm pwi cmd=<steer,power,brake,handbrake,gear given> steerIn=<SetSteer value> vel speedHow=<vector/number/none: how SetSpeed works> seat0=<puppet/none/other> locked` |
| `NHS` | host | 2 Hz | `sending hits kills` |
| `NP` | guest | 2 Hz per host NPC in range | `key loc=<our id> copy ai car host=<pos> our=<pos> d=<m> hp=<host>/<ours> dead=<host>/<ours> age` |
| `NGS` | guest | 2 Hz | `live matched copies avg max kills pulls` |
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
- **worst moments:** the largest errors at least a second apart, with the wall-clock time on both games.

`--csv DIR` writes the lined-up series (`foot-p<id>-on-<role>.csv`, `car-p<id>-on-<role>.csv`) for plotting.
