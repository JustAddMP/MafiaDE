# One-PC in-game check (two instances, UDP loopback)

The scripted run for the 2026-10-20 fixes (docs/test-2026-10-20.md): both games on this PC, the host at the top of
the test area and the guest below it, a co-op Free Ride, then the checks the two-PC test failed. Windows PowerShell,
from the repo root, after `build.cmd`:

```
powershell -ExecutionPolicy Bypass -File tools\onepc\run_start.ps1   # saves backed up, keeper, both games, Free Ride
                                                                      # (-NetSim "latency=20,jitter=5" / "off": the lag)
powershell -ExecutionPolicy Bypass -File tools\onepc\run_test.ps1    # the checks, screenshots
powershell -ExecutionPolicy Bypass -File tools\onepc\run_stop.ps1    # keeper off, unmute, close, saves restored + checked
powershell -ExecutionPolicy Bypass -File tools\onepc\compare.ps1     # sync_compare --check + log counts
```

Settings (environment): `MC_GAME_DOCS` (the game's `My Games\Mafia Definitive Edition`, default under Documents; it is
OneDrive-redirected on the test PC), `MC_AREA_X` / `MC_AREA_R` (the screen strip the windows must stay in, default
2916 / 4233), `MC_ONEPC_WORK` (notes, screenshots, the save backup; default `%TEMP%\mafiacoop-onepc`).

Simulated lag (on by default): run_start.ps1 starts both games with `--net-sim latency=8,jitter=2,loss=0,dup=0`
(core `net/net_sim.h`, ini `net_sim = ...` in any section): every UDP datagram a game sends waits 8 ms +-2 ms, so
the round trip in game is 20-30 ms like a real link instead of ~7-10 ms (the games' own frames add ~8 ms: with
12 ms +-3 the 2026-10-21 01:45 run's SUM `rtt=` read 29-47 ms, median 34; core `tests/net_sim.cpp` measures 12 ms
+-3 on bare transports at 23 ms). The guest's `net: ... rtt` log lines and the SUM `rtt=` show it. It is a test tool, not a sync
setting: Steam P2P never gets it, the logs say `SIMULATED LAG ON` at the transport start and in the session start
block, and the SES `part=netsim` record puts it into sync_compare's session headers (`SIMULATED LAG ON ...`, and the
`session.mismatch` line names it). `-NetSim off` runs without it; any other value is passed on as it is.

Windows (the strip x `MC_AREA_X`..`MC_AREA_R`): the games are started with `--window-area <x>,<r>,<y>` (core
`dll/window_place.h`: every top-level window the game creates is made at x, y - its 480x481 start window used to come
up in the middle of the primary monitor, 2320,479, for about 3 s) and `Saves\videoconfig.cfg` says windowed (`x y
1280 720 0 ...`; the old `... 1 ...` made the game a fullscreen window over the whole primary monitor, 0,0, for
~0.3 s). The keeper starts before the games: a WinEvent hook moves any window of the game or MafiaCoop.exe that is
created, shown or moved outside the strip at once (before it is shown when it can), with a 50 ms poll as the
fallback; `keeper.log` has a line `OUTSIDE ... (VISIBLE)` for every window seen outside while shown (and the totals
when it stops: `windows seen outside the strip while shown: N`) - report every one. Left over: hidden system windows
of the game (SystemUserAdapterWindowClass, the D3D / DXGI helper windows) are made at 0,0 by Windows, never shown;
the keeper moves them anyway. Key presses go only to a game window that is in front;
screenshots copy only a game window's rectangle; run_stop.ps1 stops the keeper before unmuting (it would mute again)
and must end with `unmuted sessions: 0`, `profile and save files changed by the run: 0` and `saves: 0 file(s) differ`. Never pick New Game.

## What run_test.ps1 does and what to look for

| Step | Screenshots (`shots\`) | Must see |
|---|---|---|
| spawn | 01-spawn-host/guest | each window shows the other player standing near |
| suppress (10 s, and after the minute) | (run.log lines `guest suppress` / `host world extent`, tools/onepc/suppress_probe.lua and world_extent.lua) | guest: `own moving cars: 0-50 m 0, 50-100 m 0, 100-150 m 0, 150- m 0; own ambient humans: ... 0 ...` (every band 0), `switches off`, `budget 0` (or `nil`: not readable, then the log says so), `puppets 1 of 1`, `ped copies` > 0 once the host has pedestrians near, npc copies when the host has NPCs near; guest log `guest world: our own traffic, police and pedestrians are off (the first world of the session): ambient traffic ok, police ok, pedestrian budget 0 (it was N)`; host: `cars/humans per band (m): 0-50: ..., 400+: ...` (where the bands run empty is where a guest's streets end: the leash numbers, host-authority D1) and `pedestrian budget N`, host log `guest world: we host: our own world stays on; the pedestrian budget here is N` |
| on foot | 02-guest-walked, 03-host-walked | the walker's puppet followed (face.lua line: dist a few m, st=alive, hidden=false) |
| fall | 04-host-fell, 05-guest-fell | puppet on the ground next to its player, one fall; no "put at its player's spot" loop |
| motorbike | 06-host-on-bike, 07-host-off-bike | guest: a motorcycle with the host's puppet on it (log `puppet 0 rides bike`), then the puppet on foot; host log: `a motorbike (...)` with a key `0:16000001`-like, no `cars error` |
| garage | - | host: `garage_take.lua` answers with what it called (`CheatAddVehicle=...`; `no game.garage` = the garage cannot be scripted: take a car from the Free Ride garage by hand, it is next to the start); run.log `host garage.pf: kept out >= 1, written for real 0`; host log `savesys: profile write #n kept out of the player's profile (progress: the cars in the player's garage)` |
| car | 08-guest-in-car, 09-guest-out-of-car | host: a car copy with the guest's puppet at the wheel (`puppet 1 sits in car`), then on foot |
| character | 10-guest-new-character | host: `puppet 1 removed (his character changed)` then `spawned` with the new profile within ~1 s |
| loss | 11-after-loss | guest log: `puppet 0 is gone (removed by the game, #1)`, `... (loaded before) not ready in 1.5 s: the spawner is stuck`, `factory reset: the spawner is stuck`, `factory: obtaining crewed car` in the same tick, `puppet 0 spawned` within ~2 s (was 5.0-5.1 s); never `did not load in 12 s` for a known profile |
| a minute | 12-a-minute-later | traffic and pedestrians around both; both puppets still there |
| leash (by hand, after the steps above: `Lua 1 tools\onepc\leash_test.lua`, then again in a car, then once with the host in his pause menu) | a guest screenshot at the countdown and after the pull | run.log: `moved to ... (450 m from the host, ...)`; guest: banner `TOO FAR FROM THE HOST - BACK IN 10 S` counting down, log `leash: ... -> countdown`, after ~10 s the screen fades and `leash: pulled back beside the host (foot)` (in the car: `(road)` or `(beside)`, still in the car), `PULLED BACK TO THE HOST` for 3 s, never falling through the street after it (if the ground there was not loaded yet: `leash: ... m under the spot we were pulled to ...: put back on it`, trace `LSH ev=hold`, and no `we are down`); host status panel `<NAME> IS FAR AWAY (450 M)`; host paused: banner `THE HOST PAUSED`, no pull until he resumes, then the rest of the countdown |
| traffic | (run.log lines `host traffic:` / `guest traffic:`, tools/onepc/traffic_probe.lua) | host: `ours: 0-40 m N (no driver 0), 40-60 m ..., 60- m N (no driver K)`: the cars without a driver are the far ones (the game streams drivers out beyond ~50 m; one within 40 m is a real get-out); guest: `copies N: driver seated M (within 40 m: K of K), crew out 0` (unless a host car's driver really got out next to the host; beyond 40 m his game streams the copies' crews out, they are not put back: M may be less than N) |
| peds | (guest log / trace; walk a busy street together for a minute, look around) | guest log: `peds: copy spawner ready`, `copy profile <name> loaded` for several of the six looks, `spawned a copy for the host's pedestrian N`, later `pool copy N is the copy of the host's pedestrian M now`; never `copy profile ... not used again` for all six; no `ev=adopt` / `ev=show` in the guest's PDE (0.16 rules gone); host log: no pedestrians received (a guest sends none) |

## Expected `compare.ps1` (sync_compare --check)

| metric | expected |
|---|---|
| world.guest_own_cars / world.guest_own_peds | 0 (the guest's own moving cars within 150 m / ambient humans within 80 m still shown, max after 10 s in a world: `GW` records) |
| foot.shown | >= 98 % both ways (was 19.5-40 % in the 2026-10-20 two-PC test) |
| foot.respawns | 0 both ways |
| foot.respawn_s | <= 5 s (the simulated loss on the guest; ~1.6 s since onepc-fixes, was 5.03 / 5.10) |
| foot.dz_p95 | <= 0.5 m both ways (was 0.76-0.79: a bike rider sent as on foot) |
| foot.p95 / mean / gait | PASS (one PC: ~0.1-0.5 m) |
| car.* | PASS for the guest's car on the host |
| traffic.*, npc.*, ped.*, world.time_err, lat.player.p95, bw.host_up | PASS |
| npc.guest_sent | 0 (no state or message of the guest's own NPCs; his NPCs without a host NPC are held: `NHD` in his trace) |
| ped.coverage | >= 80 % on the guest (the host's pedestrians within 40 m with a copy; the replays in tests/ped_test.cpp: 82 % for session A with its hidden spawner, 92 % for B; recorded on 2026-10-20: 18.2 / 82.1 %) |
| ped.p95 | <= 1.5 m (replays: 0.40 / 0.37 m; recorded 3.2 m; the one-PC runs of 2026-10-20: 2.05 / 1.72 m with the NPC blend, the copies now have the puppets' FootDriver) |
| ped.copy_fail_per_min | <= 2 (replays: 0.55 / 0; recorded in session A: 86 a minute, the hidden spawner) |
| traffic.true_p95 | <= 1.5 m (was 2.14 m in session A: copies of standing cars driven off them; the replay of A in tests/traffic_test.cpp: 0.75 m) |
| traffic.crew_exits_per_min | <= 1 per minute of copies (was 26.5 on the guest in session A, 5.2 in B: drivers the host's game streamed out 45-76 m from him counted as gone) |
| traffic.reseats_per_min | <= 3 per minute of copies (was 71.6 on the guest in session A: crews leaving copies that were pushed off their standing car, and every get-out put back) |
| traffic "driven off" line | `copies of a standing car driven off it` near 0 % (was 13 % of the standing rows in session A) |
| leash.false_pulls / leash.pulls | 0 / 1-3 (one per leash_test.lua move; none while the host was paused) |
| join.complete_s | <= 10 s (now timed from the world's load, not from joining in the menu) |
| log counts, files | `savesys: written: (garage\|my2k\|profile_data\|.slot)` 0 and `savesys: written: save` 0 on both (only settings are written for real in co-op, the host's too: he hosts a FREE RIDE, not his story); `save write #n kept out` >= 1 and `kept out of the player's profile` >= 1 on the host |
| run_stop.ps1 | `profile and save files changed by the run: 0` (before anything is put back; was garage.pf and aslot\save1.sav for the host in the 2026-10-20 test) and `saves: 0 file(s) differ`; only settings (gamesettings.pf, videoconfig.cfg, ...) may be listed as `restored` |
| log counts | `cars error` 0 on both; `did not load in` 0; `rides bike` >= 1 (guest); `sits in car` >= 1 (host); `has no driver (for 1 s` and `is driven again: our crew gets back in` 0 or a handful (guest), `keeps leaving seat` 0; `the game streams drivers out` (host) whenever a host car farther than 40 m from him lost its driver |

One PC cannot show: Steam P2P, real latency / loss, two different machines' timing, the real Free Ride garage
removing a puppet (simulated by losstest.lua), a bike from the garage (spawned by bike_spawn.lua instead), a car
taken out of the real garage menu (garage_take.lua changes the garage by script; that the script API is
`game.garage` is UNVERIFIED), a STORY host (his story saves stay real, only the profile is kept out: a STORY run
would change aslot\save*.sav on purpose).
