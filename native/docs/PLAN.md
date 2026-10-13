# Plan — next steps

Living plan for the native co-op rewrite. Update it as items land. Last updated 2026-10-13.
Read with `ARCHITECTURE.md` (design + clean-room rule), `protocol.md`, `re-notes.md`, `game-api.md`, `../README-dev.md`.

## Where we are

| Phase | State |
|---|---|
| 1 Skeleton (launcher, injection, Present hook, crash dumps) | Done |
| 2 Connection (UDP + Steam lobby, handshake, overlay) | Done; Steam P2P between two real accounts untested |
| 3 Second player (Lua bridge, puppets) | On foot + weapons + car following done; gaps below |
| 5 Native UI (CO-OP in main/pause menus, banners) | Done for keyboard; gamepad/mouse/Steam invite untested |
| 4 Shared world, 6 polish | Not started |

## Working rules

- Clean-room: never use MafiaHub/MafiaMP code (deleted from the repo; it's only in old git history — don't look).
- The game folder is read-only. Back up the save folder before in-world tests
  (`%TEMP%\claude\G--MafiaCoop\save-backup`), never start New Game, kill all game instances after a test,
  capture only game windows.
- Two instances on one PC: `out\MafiaCoop.exe --host --name Host --log host` and
  `out\MafiaCoop.exe --join 127.0.0.1 --name Guest --log guest`. Live Lua: F8 / `tools\devlua.ps1`.
- Build with `build.cmd` (warning-free /W4); `out\net_loopback.exe` must pass. Commit on main after each tested
  step; no Claude attribution lines; push when the user asks.

## Next steps, in order

### A. Finish phase 3 (second player)
1. **Driver/passenger visible in cars.** `GetOnVehicle` never seats our puppets. Investigate: other seat methods
   on C_WrapperHuman/C_WrapperCar (game-api.md appendix), AI "enter car" commands, spawning the puppet AS the
   car's crew (ObtainCarWithCrew already gives a driver), or attaching to the seat. Gate: remote driver visible.
2. **Passengers**: sync seat index; puppet seated in the driver's car copy.
3. **Car ownership hand-off**: when the driver leaves, the car copy should stay where it was left (not vanish)
   and become drivable by others; decide owner = last driver.
4. **Crouch, aim direction, firing, reload** shown on puppets; health synced.
5. **Damage**: shooting/punching the other player hurts them on their own game (send hit events to the victim's
   owner), deaths/respawn; ramming. Melee (punches) was the big gap in the old version.
6. **Player models**: guests choose/see proper character models (Tommy for host, Paulie/Sam for guests);
   Thompson needs a model that accepts it.
7. **Faster first spawn**: puppet factory currently takes ~20 s if the other game is still loading; retry
   sooner / prepare the factory at world load.
8. Bug: at the main menu player-sync claims "in world" and queues puppets before a save is loaded.

### B. Phase 4 — shared world (the campaign in co-op)
Goal: guests play the host's real campaign: same NPCs, traffic, weather, time, mission progress.
1. **Weather + time of day**: host sends `game.gfx` GetTime/weather set every few seconds; guests apply
   (SetTime/SetWeatherSet, re-notes §4). Smallest, do first.
2. **Mission state**: find the mission/chapter API (E_MissionAction, mission/objective enums in the Lua
   tables). Guests load the same chapter as the host on join; follow host's checkpoint/chapter changes;
   objective text mirrored. Decide: guests run the mission scripts disabled and mirror the host's world.
3. **NPC mirroring**: host enumerates relevant humans (mission NPCs, enemies near players), streams
   spawn/state/despawn; guests spawn puppets driven like player puppets. Enemies must react to guests:
   either guests' shots are sent to the host which applies damage to the real NPC, or the host spawns an
   invisible proxy for each guest so its AI targets them. Prototype both, measure.
4. **Vehicles + traffic**: host's mission cars mirrored; ambient traffic either disabled on guests
   (`game.traffic` SwitchGenerators) and mirrored, or left local (cheap, accepted desync). Start local.
5. **Cutscenes**: guests see host cutscenes (trigger the same cutscene id) or a "cutscene playing" screen.
6. Interest management: only stream within ~300 m of any player; bandwidth budget.

### C. Phase 5 polish (native UI)
1. Hide the debug status text by default (F7 shows it); keep banners.
2. Gamepad + mouse in our panels; test JOIN FRIEND and a real Steam invite with two accounts/PCs.
3. CO-OP page refreshes when a background join completes.
4. Player name tags above puppets (projected 2D text via our overlay) and minimap blips if feasible.
5. Settings page (name, show tags, debug overlay).

### D. Release / distribution
1. Stable protocol build number (currently changes every rebuild → only identical builds connect): derive from
   a version constant, not the build hash.
2. Package: `MafiaCoop.exe` + `mafiacoop.dll` (+ README) in a zip, a few MB; `tools\make_release.cmd`.
3. Optional proxy-DLL install mode (drop-in so the game can be started from Steam) — must be opt-in because it
   writes into the game folder.
4. Detect unsupported game builds (exe CRC) and show a clear message.
5. GitHub release v0.2.0 with the new zip when the user asks.

### E. Robustness (ongoing)
- Every pattern/Lua call fails soft (log + disable feature); keep it that way.
- Crash triage: `out\logs\crash-*.dmp` + log stack (module+offset) → symbolize with the PDBs in out\.
- Long-session test (1 h, two instances, driving + shooting) before each release.
