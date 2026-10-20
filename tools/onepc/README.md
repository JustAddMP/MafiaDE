# One-PC in-game check (two instances, UDP loopback)

The scripted run for the 2026-10-20 fixes (docs/test-2026-10-20.md): both games on this PC, the host at the top of
the test area and the guest below it, a co-op Free Ride, then the checks the two-PC test failed. Windows PowerShell,
from the repo root, after `build.cmd`:

```
powershell -ExecutionPolicy Bypass -File tools\onepc\run_start.ps1   # saves backed up, keeper, both games, Free Ride
powershell -ExecutionPolicy Bypass -File tools\onepc\run_test.ps1    # the checks, screenshots
powershell -ExecutionPolicy Bypass -File tools\onepc\run_stop.ps1    # keeper off, unmute, close, saves restored + checked
powershell -ExecutionPolicy Bypass -File tools\onepc\compare.ps1     # sync_compare --check + log counts
```

Settings (environment): `MC_GAME_DOCS` (the game's `My Games\Mafia Definitive Edition`, default under Documents; it is
OneDrive-redirected on the test PC), `MC_AREA_X` / `MC_AREA_R` (the screen strip the windows must stay in, default
2916 / 4233), `MC_ONEPC_WORK` (notes, screenshots, the save backup; default `%TEMP%\mafiacoop-onepc`).

Rules the scripts follow: the keeper starts before the games and moves / shrinks a window into the area within a
quarter second (the game opens a 480x481 start window elsewhere for a moment and can grow its window to the profile's
resolution: both are logged in `run.log` as "first seen at"); key presses go only to a game window that is in front;
screenshots copy only a game window's rectangle; run_stop.ps1 stops the keeper before unmuting (it would mute again)
and must end with `unmuted sessions: 0`, `profile and save files changed by the run: 0` and `saves: 0 file(s) differ`. Never pick New Game.

## What run_test.ps1 does and what to look for

| Step | Screenshots (`shots\`) | Must see |
|---|---|---|
| spawn | 01-spawn-host/guest | each window shows the other player standing near |
| on foot | 02-guest-walked, 03-host-walked | the walker's puppet followed (face.lua line: dist a few m, st=alive, hidden=false) |
| fall | 04-host-fell, 05-guest-fell | puppet on the ground next to its player, one fall; no "put at its player's spot" loop |
| motorbike | 06-host-on-bike, 07-host-off-bike | guest: a motorcycle with the host's puppet on it (log `puppet 0 rides bike`), then the puppet on foot; host log: `a motorbike (...)` with a key `0:16000001`-like, no `cars error` |
| garage | - | host: `garage_take.lua` answers with what it called (`CheatAddVehicle=...`; `no game.garage` = the garage cannot be scripted: take a car from the Free Ride garage by hand, it is next to the start); run.log `host garage.pf: kept out >= 1, written for real 0`; host log `savesys: profile write #n kept out of the player's profile (progress: the cars in the player's garage)` |
| car | 08-guest-in-car, 09-guest-out-of-car | host: a car copy with the guest's puppet at the wheel (`puppet 1 sits in car`), then on foot |
| character | 10-guest-new-character | host: `puppet 1 removed (his character changed)` then `spawned` with the new profile within ~1 s |
| loss | 11-after-loss | guest log: `puppet 0 is gone (removed by the game, #1)`, `... (loaded before) not ready in 4 s: the spawner is stuck`, `factory reset: the spawner is stuck`, `puppet 0 spawned` within ~5 s; never `did not load in 12 s` for a known profile |
| a minute | 12-a-minute-later | traffic and pedestrians around both; both puppets still there |
| traffic | (run.log lines `host traffic:` / `guest traffic:`, tools/onepc/traffic_probe.lua) | host: `ours: 0-40 m N (no driver 0), 40-60 m ..., 60- m N (no driver K)`: the cars without a driver are the far ones (the game streams drivers out beyond ~50 m; one within 40 m is a real get-out); guest: `copies N: driver seated N, crew out 0` (unless a host car's driver really got out next to the host) |

## Expected `compare.ps1` (sync_compare --check)

| metric | expected |
|---|---|
| foot.shown | >= 98 % both ways (was 19.5-40 % in the 2026-10-20 two-PC test) |
| foot.respawns | 0 both ways |
| foot.respawn_s | <= 5 s (the simulated loss on the guest) |
| foot.dz_p95 | <= 0.5 m both ways (was 0.76-0.79: a bike rider sent as on foot) |
| foot.p95 / mean / gait | PASS (one PC: ~0.1-0.5 m) |
| car.* | PASS for the guest's car on the host |
| traffic.*, npc.*, ped.*, world.time_err, lat.player.p95, bw.host_up | PASS |
| traffic.true_p95 | <= 1.5 m (was 2.14 m in session A: copies of standing cars driven off them; the replay of A in tests/traffic_test.cpp: 0.75 m) |
| traffic.crew_exits_per_min | <= 1 per minute of copies (was 26.5 on the guest in session A, 5.2 in B: drivers the host's game streamed out 45-76 m from him counted as gone) |
| traffic.reseats_per_min | <= 3 per minute of copies (was 71.6 on the guest in session A: crews leaving copies that were pushed off their standing car, and every get-out put back) |
| traffic "driven off" line | `copies of a standing car driven off it` near 0 % (was 13 % of the standing rows in session A) |
| join.complete_s | <= 10 s (now timed from the world's load, not from joining in the menu) |
| log counts, files | `savesys: written: (garage\|my2k\|profile_data\|.slot)` 0 and `savesys: written: save` 0 on both (only settings are written for real in co-op, the host's too: he hosts a FREE RIDE, not his story); `save write #n kept out` >= 1 and `kept out of the player's profile` >= 1 on the host |
| run_stop.ps1 | `profile and save files changed by the run: 0` (before anything is put back; was garage.pf and aslot\save1.sav for the host in the 2026-10-20 test) and `saves: 0 file(s) differ`; only settings (gamesettings.pf, videoconfig.cfg, ...) may be listed as `restored` |
| log counts | `cars error` 0 on both; `did not load in` 0; `rides bike` >= 1 (guest); `sits in car` >= 1 (host); `has no driver (for 1 s` and `is driven again: our crew gets back in` 0 or a handful (guest), `keeps leaving seat` 0; `the game streams drivers out` (host) whenever a host car farther than 40 m from him lost its driver |

One PC cannot show: Steam P2P, real latency / loss, two different machines' timing, the real Free Ride garage
removing a puppet (simulated by losstest.lua), a bike from the garage (spawned by bike_spawn.lua instead), a car
taken out of the real garage menu (garage_take.lua changes the garage by script; that the script API is
`game.garage` is UNVERIFIED), a STORY host (his story saves stay real, only the profile is kept out: a STORY run
would change aslot\save*.sav on purpose).
