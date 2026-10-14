# Plan — next steps

Living plan for the native co-op rewrite. Update it as items land. Last updated 2026-10-14.
Read with `ARCHITECTURE.md` (design + clean-room rule), `protocol.md`, `re-notes.md`, `game-api.md`, `../README-dev.md`.

## Where we are

| Phase | State |
|---|---|
| 1 Skeleton (launcher, injection, Present hook, crash dumps) | Done |
| 2 Connection (UDP + Steam lobby, handshake, overlay) | Done; Steam P2P between two real accounts untested |
| 3 Second player (Lua bridge, puppets) | On foot, weapons, shared cars (seated drivers, passengers, hand-off) done; gaps below |
| 5 Native UI (CO-OP in main/pause menus, banners) | Done for keyboard; gamepad/mouse/Steam invite untested |
| 4 Shared world, 6 polish | Not started |

## Working rules

- Clean-room: no code from other multiplayer mods (see ARCHITECTURE.md).
- The game folder is read-only. Back up the save folder before in-world tests
  (`%TEMP%\claude\G--MafiaCoop\save-backup`), never start New Game, kill all game instances after a test,
  capture only game windows.
- Two instances on one PC: `out\MafiaCoop.exe --host --name Host --log host --debug-overlay` and
  `out\MafiaCoop.exe --join 127.0.0.1 --name Guest --log guest --debug-overlay` (the status text is hidden without it). Live Lua: F8 / `tools\devlua.ps1`.
- When a feature lands, also update the website (C:\AI\Claude\Projects\JS\justaddmp.com): tick the matching task
  in `app/lib/repos.ts` (Mafia entry, `progress.areas[].tasks`), add new tasks there when they're added here, refresh
  highlights / not-synced / known issues if they changed, run `npx tsc --noEmit` and `npm run build`, commit.
- Build with `build.cmd` (warning-free /W4); `out\net_loopback.exe` must pass. Commit on main after each tested
  step; no Claude attribution lines; push when the user asks.

## Done

- [x] 1 Launcher finds the game via Steam, starts it, injects the dll (early APC), no writes to the game folder
- [x] 1 Present hook tick, logging, crash handler with minidump + module+offset stack
- [x] 2 Own UDP transport with reliability, fragmentation, loss simulation; `net_loopback` 16/16
- [x] 2 Steam lobby (friends-only), overlay invite dialog, own steam_api instance alongside the game's
- [x] 2 Host-authoritative session: handshake, version check, relay, ping, timeouts; two instances on one PC
- [x] 2 Status overlay with own D3D11 text renderer (F7)
- [x] 3 Lua bridge on the game's script thread + dev console (F8)
- [x] 3 Remote players as puppets: walk/run/sprint with animation, heading, pistol, 20 Hz interpolation
- [x] 3 Car copies follow the driver; cleanup on leave/timeout; game keeps running when unfocused
- [x] 5 CO-OP entry in the main menu (host / join friend / join by IP) and pause menu (players, invite, leave)
- [x] 5 Banners for join/leave/disconnect; in-game panels block game input
- [x] README with screenshots; pushed to GitHub

## Next steps, in order

Tick an item (`- [x]`) in the same commit that lands it.


### A. Finish phase 3 (second player)
- [x] **Driver/passenger visible in cars.** `GetOnVehicle` never seats our puppets. Investigate: other seat methods
   on C_WrapperHuman/C_WrapperCar (game-api.md appendix), AI "enter car" commands, spawning the puppet AS the
   car's crew (ObtainCarWithCrew already gives a driver), or attaching to the seat. Gate: remote driver visible.
   Done: the seat index of `GetOnVehicle` is 1-based; copies are no longer frozen (game-api.md 7).
- [x] **Passengers**: sync seat index; puppet seated in the driver's car copy. E / pad Y next to a car another
   player drives = ride along as passenger.
- [x] **Car ownership hand-off**: when the driver leaves, the car copy should stay where it was left (not vanish)
   and become drivable by others; decide owner = last driver. Flow in protocol.md "Shared cars, seats and
   ownership" (car keys, CarParked 0x0101).
- [x] **Crouch, aim direction, firing, reload** shown on puppets; health synced. Done: debug AI commands
   (game-api.md 10); aim input read from RMB/left trigger; panel health column not checked in a capture.
- [ ] (in progress, untested code on main) **Damage**: shooting/punching the other player hurts them on their own game (send hit events to the victim's
   owner), deaths/respawn; ramming.
- [ ] **Player models**: guests choose/see proper character models (Tommy for host, Paulie/Sam for guests);
   Thompson needs a model that accepts it.
- [x] **Faster first spawn**: puppet factory currently takes ~20 s if the other game is still loading; retry
   sooner / prepare the factory at world load. Done: factory at world load, model preloaded, puppet 0.0-0.1 s after
   the other player's first state.
- [x] Bug: at the main menu player-sync claims "in world" and queues puppets before a save is loaded.

### F. Customisable characters
Builds on A6 (player models). Each player picks how they look; everyone sees the same choice.
- [ ] **Research** (static part done: docs/research-characters-missions.md; confirm in game with its test snippets): find the game's model/outfit APIs: spawn profiles for story characters and civilians, outfit/clothing
  variants, the ModelParts enum (hats, accessories), and whether the local player's model can be swapped at runtime.
  Record what works in game-api.md, including which models can hold every weapon, drive and animate correctly.
- [ ] **Character list**: a curated list of playable characters (story cast, gangsters, civilians) with their outfit
  variants; only models that pass the weapon/vehicle/animation check above.
- [ ] **Character picker**: a "Character" page in the CO-OP menu with the list, outfit and accessory choices and a live
  preview (the player's own character changes while browsing, or a preview character in front of the camera).
- [ ] **Sync**: the choice (character, outfit, parts) is saved in MafiaCoop.ini, sent in Hello, and can be changed
  mid-session; everyone's puppet updates live. The local player sees themselves as the chosen character.
- [ ] **Fallbacks**: an unknown or unsupported choice (e.g. from a newer version) falls back to a default model instead
  of failing; the host's story character can be kept as Tommy for cutscenes if swapping breaks them.

### B. Phase 4 — shared world (the campaign in co-op)
Goal: guests play the host's real campaign: same NPCs, traffic, weather, time, mission progress.
- [x] **Weather + time of day**: host sends `game.gfx` GetTime/weather set every few seconds; guests apply
   (SetTime/SetWeatherSet, re-notes §4). Done: WorldEnv 0x0200 (protocol.md), time flow speed included.
- [ ] **Mission state** (static research in docs/research-characters-missions.md): find the mission/chapter API (E_MissionAction, mission/objective enums in the Lua
   tables). Guests load the same chapter as the host on join; follow host's checkpoint/chapter changes;
   objective text mirrored. Decide: guests run the mission scripts disabled and mirror the host's world.
- [ ] **NPC mirroring**: host enumerates relevant humans (mission NPCs, enemies near players), streams
   spawn/state/despawn; guests spawn puppets driven like player puppets. Enemies must react to guests:
   either guests' shots are sent to the host which applies damage to the real NPC, or the host spawns an
   invisible proxy for each guest so its AI targets them. Prototype both, measure.
- [ ] **Vehicles + traffic**: host's mission cars mirrored; ambient traffic either disabled on guests
   (`game.traffic` SwitchGenerators) and mirrored, or left local (cheap, accepted desync). Start local.
- [ ] **Cutscenes**: guests see host cutscenes (trigger the same cutscene id) or a "cutscene playing" screen.
- [x] Interest management: only stream within ~300 m of any player; bandwidth budget. (Module + tests in
  src/net/interest.*, docs/interest.md; NPC/vehicle mirroring (B3/B4) plugs into it.)

### C. Phase 5 polish (native UI)
- [x] Hide the debug status text by default (F7 shows it); keep banners. `debug_overlay = 1` / `--debug-overlay` shows it at start.
- [ ] Gamepad + mouse in our panels; test JOIN FRIEND and a real Steam invite with two accounts/PCs.
- [ ] CO-OP page refreshes when a background join completes.
- [ ] Player name tags above puppets (projected 2D text via our overlay) and minimap blips if feasible.
- [ ] Settings page (name, show tags, debug overlay).

- [ ] **Session password** (network side + tests done in 65c562f; tick after the menus are checked in game): the host can set an optional password (CO-OP page: "Set password", reusing the text box
  from Join by IP; ini `password=`). Joining a passworded game prompts for it. Never sent in plain text: the host
  sends a random challenge after Hello and the guest answers with HMAC-SHA256(password, challenge) via Windows
  BCrypt (no extra libraries); wrong answers get Reject "wrong password", with a short delay and a per-address attempt
  limit. Steam invites from the host skip the password by default (option to require it anyway). The pause-menu
  panel shows a lock icon when a password is set. Protocol version bump; loopback tests for right/wrong/missing
  password and the attempt limit.

### D. Release / distribution
- [x] Stable protocol build number (currently changes every rebuild → only identical builds connect): derive from
   a version constant, not the build hash.
- [ ] Package: `MafiaCoop.exe` + `mafiacoop.dll` (+ README) in a zip, a few MB; `tools\make_release.cmd`.
- [ ] Optional proxy-DLL install mode (built and tested on a fake game folder in afa56cd: `MafiaCoop.exe --install`; tick after a real start from Steam) (drop-in so the game can be started from Steam) — must be opt-in because it
   writes into the game folder.
- [x] Detect unsupported game builds (exe CRC) and show a clear message. Done: launcher CRC32 check against
   `core/version.h` (cached, `--check-only`, `--skip-version-check`); dll banner when patterns are missing.
- [ ] GitHub release v0.2.0 with the new zip when the user asks.

### E. Robustness (ongoing)
- Every pattern/Lua call fails soft (log + disable feature); keep it that way.
- Crash triage: `out\logs\crash-*.dmp` + log stack (module+offset) → symbolize with the PDBs in out\.
- Long-session test (1 h, two instances, driving + shooting) before each release.
