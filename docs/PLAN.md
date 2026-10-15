# Plan — next steps

Living plan for the native co-op rewrite. Update it as items land. Last updated 2026-10-15 (batch 6).
Read with `ARCHITECTURE.md` (game layer + clean-room rule), `protocol.md` (our app messages), `re-notes.md`,
`game-api.md`, `../README-dev.md`, and core's docs in `../external/core/docs/` (ARCHITECTURE.md, protocol.md,
interest.md).

## Where we are

| Phase | State |
|---|---|
| 1 Skeleton (launcher, injection, Present hook, crash dumps) | Done |
| 2 Connection (UDP + Steam lobby, handshake, overlay) | Done; Steam P2P between two real accounts untested |
| 3 Second player (Lua bridge, puppets) | Done: on foot, weapons, shared cars, characters (picker in the CO-OP panel), damage (bullets, punches, rams) and respawns |
| 5 Native UI (CO-OP in main/pause menus, banners, lobby, settings, name tags, map markers) | Done for keyboard; gamepad/mouse/Steam invite untested; lobby: Free Ride, Story and chapter select |
| 4 Shared world | Weather/time, story (chapter, checkpoint, objectives) and cutscenes done; NPCs and mission cars partly (host-authoritative pairing, deaths, guest hits) |
| 6 polish | Not started |

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
- Repo layout: this repo is the Mafia DE game layer (`src/game/`: descriptor `game_info.cpp`, `module.cpp`, Lua
  bridge, player sync, menus); everything game-agnostic is JustAddMP/core in the submodule `external/core`
  (`git submodule update --init` after cloning).
- Generic fixes go to core first: change and commit them in the core repo (its `build.cmd` + tests must pass, see
  its README.md), release it as a core version tag (core README "Versions"), then move this repo to that tag with
  `tools\update_core.cmd vX.Y.Z` (builds + tests) and commit `external/core`. This repo only ever pins core release
  tags (currently v0.5.0). Never edit core files only inside this repo's submodule checkout without committing them to core.
- Build with `build.cmd` (warning-free /W4; it also builds core). Core's tests must pass
  (`external\core\build.cmd`, then `external\core\out\net_loopback.exe` and the others), and `out\proxy_test.exe`.
  Commit on main after each tested step; no Claude attribution lines; push when the user asks.

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
- [x] **Damage** (research: docs/research-world-sync.md): shooting/punching the other player hurts them on their own game (send hit events to the victim's
   owner), deaths/respawn; ramming. Checked in game 2026-10-15 (docs/protocol.md "Phase 6", game-api.md 11): bullets
   (pistol and real Tommy gun fire), rams, puppets never dying on the attacker's screen, guest downed -> respawn next to
   the host, guest death -> checkpoint loaded at once -> next to the host, host death -> YOU DIED flow, `friendly_fire = 0`.
   Punches done 2026-10-15 (game-api.md 11): melee only works outside safehouse zones (the Free Ride start at Salieri's
   bar locks it, which is why earlier tests saw nothing); puppets with a story profile take real punches (DAMAGE type 3,
   10 / 36 charged), puppets the game will not target (ally/Tommy profiles) get a Hit from our swing event; host->guest
   and guest->host landed, five punches in a row, charged punches downed the guest and he respawned next to the host.
   Re-verified: ram damage summed over 0.25 s (puppets no longer dodge cars), the >20 m snap (host car teleported 40 m
   through the guest: the copy jumped, no damage). The bullet replay (CreateShots from the attacker's puppet) only
   works in some game instances; the DoDamage fallback covers the rest.
- [x] **Player models**: guests choose/see proper character models (Tommy for host, Paulie/Sam for guests);
   Thompson needs a model that accepts it. Done 2026-10-15: default by player id (Tommy, Paulie, Sam, Salieri),
   `model =` / `--model` sent in Hello, the local player switched with the override slot (re-applied after loads,
   cleared at session end); Paulie and Sam seen on both screens; puppets armed through the debug inventory helper draw,
   aim and fire the Tommy gun. Sam's puppet variant wears a coat and hat his player model does not; Salieri (player 3)
   not checked.
- [x] **Faster first spawn**: puppet factory currently takes ~20 s if the other game is still loading; retry
   sooner / prepare the factory at world load. Done: factory at world load, model preloaded, puppet 0.0-0.1 s after
   the other player's first state.
- [x] Bug: at the main menu player-sync claims "in world" and queues puppets before a save is loaded.

### F. Customisable characters
Builds on A6 (player models). Each player picks how they look; everyone sees the same choice.
- [x] **Research** (static part done: docs/research-characters-missions.md; confirm in game with its test snippets): find the game's model/outfit APIs: spawn profiles for story characters and civilians, outfit/clothing
  variants, the ModelParts enum (hats, accessories), and whether the local player's model can be swapped at runtime.
  Record what works in game-api.md, including which models can hold every weapon, drive and animate correctly.
  Done 2026-10-15 (game-api.md 12): override switch confirmed, the player wrapper survives it, the override is saved
  into the save file but the checkpoint setup clears it on load; Paulie holds and fires every weapon; no per-part
  switch (ShowHat/SetDirtBlend show nothing on the player). Driving as Paulie/Sam not checked.
- [x] **Character list**: a curated list of playable characters (story cast, gangsters, civilians) with their outfit
  variants; only models that pass the weapon/vehicle/animation check above. Prefer profiles that look the same as the
  player and as a puppet (Sam does not); the host's "tommy" puppet in Free Ride falls back to the trench coat (no
  `mm_spawn_profiles` entry for `freeroam_test`).
  Done 2026-10-15 (game-api.md 12, src/game/characters.cpp): 14 characters, 26 outfits, each checked as the player and
  as a puppet; story outfits follow the loaded story point on both sides (that was the Sam mismatch); Tommy's outfits
  are his wardrobe profiles; the host as story Tommy sends the profile his game uses, so in Free Ride he is seen in his
  real outfit. Weapons: sailor Tommy fired the Tommy gun; driving: Sam drove.
- [x] **Character picker**: a "Character" page in the CO-OP menu with the list, outfit and accessory choices and a live
  preview (the player's own character changes while browsing, or a preview character in front of the camera).
  Done 2026-10-15: CHARACTER and OUTFIT rows in the pause menu's CO-OP panel (core v0.4.0 PanelOptions), keyboard
  Left/Right/Enter checked in game; every step applies at once (the others see it while you browse; your own model
  switches when the pause menu closes, because the game is paused while it is open). Pad d-pad and mouse click on the
  rows not checked in game.
- [x] **Sync**: the choice (character, outfit, parts) is saved in MafiaCoop.ini, sent in Hello, and can be changed
  mid-session; everyone's puppet updates live. The local player sees themselves as the chosen character.
  Done 2026-10-15: `model =` saved by the panel, PlayerModel (0x0102) with the pick and its look, re-sent when a
  player joins; guest switched to Sam / suit mid-session and the host's puppet followed at once; a rejoining guest saw
  the host's sailor outfit straight away.
- [x] **Fallbacks**: an unknown or unsupported choice (e.g. from a newer version) falls back to a default model instead
  of failing; the host's story character can be kept as Tommy for cutscenes if swapping breaks them.
  Done 2026-10-15: unknown alias (`--model zorro`) -> the default for the player id on every game; a profile the game
  does not know -> after 12 s marked bad, default used (player and puppet, the factory is rebuilt). The host's default
  is story Tommy; a swapped character in a cutscene is not checked yet.

### B. Phase 4 — shared world (the campaign in co-op)
Goal: guests play the host's real campaign: same NPCs, traffic, weather, time, mission progress.
- [x] **Weather + time of day**: host sends `game.gfx` GetTime/weather set every few seconds; guests apply
   (SetTime/SetWeatherSet, re-notes §4). Done: WorldEnv 0x0200 (protocol.md), time flow speed included.
- [x] **Mission state** (static research in docs/research-characters-missions.md): find the mission/chapter API (E_MissionAction, mission/objective enums in the Lua
   tables). Guests load the same chapter as the host on join; follow host's checkpoint/chapter changes;
   objective text mirrored. Decide: guests run the mission scripts disabled and mirror the host's world.
   Done 2026-10-15 (src/game/mission_sync.*, game-api.md 14, re-notes 6, protocol.md "Story"). Decision: guests run
   the chapter's own mission scripts at the host's checkpoint (the world stays the guest's; NPC mirroring is B3).
   The host sends his chapter/checkpoint/objective (MissionState) and his newest checkpoint save (HostSave); a guest
   loads that save from memory through the menu's chapter loader (bytes swapped, nothing written), starts the quest at
   the host's checkpoint and is put next to him; objectives by id through the game's HUD helpers. Checked in game
   (two instances): **Omerta** (lobby STORY from the cp_020 save: guest at cp_020 with the quest, objective and
   markers; lobby CHAPTER 10: OMERTA: both at cp_010, both flows reached cp_020 together after the intro, the host's
   cp_020 save reached the guest; late join with JOIN GAME at cp_020) and **Taxi** (lobby CHAPTER 2: TAXI, both in the
   chapter with objective markers). A later checkpoint is only ever loaded from the host's save of it (from the chapter
   start it crashed). Other chapters and checkpoints than these were not played.
   Reload crash fixed 2026-10-15 (mod 0.7.0, re-notes 7): the guest's reload of the host's checkpoint from the world
   crashed (2 of 5) when the save had another id than the first one loaded in that game (the chapter loader's storage
   keeps one entry); now 9 of 9 forced reloads in a CHAPTER session and 11 of 11 in STORY sessions without a crash.
   A guest's saves in a co-op session go to a scratch folder (`%TEMP%\MafiaCoop\coop-saves\<pid>`), never to his save
   slots: after STORY sessions with reloads and a guest death every save and profile file was byte-identical (two
   instances share one save folder, so this was checked with the host in his story slot); in Free Ride the guest's
   autosaves were redirected too (0 real writes). Deaths
   and failed missions in a chapter loaded through the chapter loader (the game would load the free roam state from
   before the quest) reload the checkpoint instead: guest death -> the host's checkpoint, host death -> his own
   (checked in Taxi). The guest is placed beside the host (1.5 m to his right) after loads and cutscenes.
- [ ] **NPC mirroring** (research: docs/research-world-sync.md): host enumerates relevant humans (mission NPCs, enemies near players), streams
   spawn/state/despawn; guests spawn puppets driven like player puppets. Enemies must react to guests:
   either guests' shots are sent to the host which applies damage to the real NPC, or the host spawns an
   invisible proxy for each guest so its AI targets them. Prototype both, measure.
   Partly done 2026-10-15 (mod 0.7.0, src/game/npc_sync.*, game-api.md 15, protocol.md "NPCs"): every game runs the
   mission, so instead of spawning copies the guest pairs the host's NPCs with his own (same spots; found through the
   entity table, `mc_entity_ids`) and the host is authoritative for them (states 5 Hz, deaths, health). Checked in Taxi:
   pairing (0.0-1.3 m), host NPC deaths repeated on the guest, a guest's shots on his copy went to the host, were
   replayed there as shots from his puppet and killed the host's NPC, whose death came back. In Bon Appetit only the
   pairing was checked (26 of 37 near Salieri's). **Open:** the host's enemies do not target the guests' puppets (they
   are civilians to the AI; the only way found, `SwitchBrain`, crashes the game), not checked in a real shootout (the
   tests cannot drive to one; NPCs were made hostile by script), some civilian copies take no damage on the guest.
- [ ] **Vehicles + traffic** (research: docs/research-world-sync.md): host's mission cars mirrored; ambient traffic either disabled on guests
   (`game.traffic` SwitchGenerators) and mirrored, or left local (cheap, accepted desync). Start local.
   Partly done 2026-10-15 (game-api.md 15): ambient traffic stays local on every game (accepted). A driven car whose
   model stands at the same spot on the other game is taken as that game's car (the mission's cars are not doubled:
   the copy spawned on top of the guest's mission taxi had made his mission fail), and no copy is spawned into another
   car. Checked in Taxi (host took the mission taxi, the guest's game used its own; both in their own taxis after the
   guest loaded cp_020, no copy until they parted). **Open:** the host's puppet is thrown out of a mission car on the
   guest's game (it does not keep an NPC at the wheel), only one chapter checked, police and mission traffic local.
- [x] **Cutscenes** (research: docs/research-world-sync.md): guests see host cutscenes (trigger the same cutscene id) or a "cutscene playing" screen.
  Done 2026-10-15 (game-api.md 14): the host polls his chapter's cutscene objects and sends the one that plays; the
  guest plays his own copy (`PlayCutscene2` with four empty strings) or lets his own flow play it, stops it when the
  host's ends or the host skips (then fades in and is put next to the host); a guest who skipped, or cannot play it,
  is held (controls locked, "THE HOST IS WATCHING A CUTSCENE") until the host's ends; puppets are hidden while a
  cutscene plays. Checked in Omerta with cine_1100_omerta (played from the console on the host, and the chapter's
  real intro): played on both, host skip stopped the guest's, guest skip held him and released him at the host's
  end. A cutscene that binds live entities (cars) was not checked.
- [x] Interest management: only stream within ~300 m of any player; bandwidth budget. (Module + tests in
  core: external/core/src/net/interest.*, external/core/docs/interest.md; NPC/vehicle mirroring (B3/B4) plugs into it.)

### C. Phase 5 polish (native UI)
- [x] Hide the debug status text by default (F7 shows it); keep banners. `debug_overlay = 1` / `--debug-overlay` shows it at start.
- [ ] Gamepad + mouse in our panels; test JOIN FRIEND and a real Steam invite with two accounts/PCs.
- [x] CO-OP page refreshes when a background join completes. Done 2026-10-15 (game-api.md 13 "Main menu live
  refresh"): the patched main menu polls us every frame (an enterFrame listener) and rebuilds the page when what it
  shows changed; checked in game: the host's page went to LOBBY (1 PLAYER) when hosting started and to LOBBY (2
  PLAYERS) the moment the guest joined, the guest's page to its connected entries, no key pressed.
- [x] Player name tags above puppets (projected 2D text via our overlay) and minimap blips if feasible. Done
  2026-10-15 (game-api.md 13): each player's name over his puppet (the head projected with
  `game.game:ProjectPointToScreen`, drawn by our overlay through core's `GameModule::DrawHud`), size and fade by
  distance (full to 35 m, gone at 90 m), hidden off screen, while the puppet is hidden or dead, and in menus/loading;
  SHOW NAME TAGS in CO-OP SETTINGS turns them off (checked both ways in Free Ride). **No occlusion**: no working ray
  cast was found from script (`StartCollisionQuery` never hits) and `IsVisibleFromCamera` is not a visibility test,
  so a tag also shows through walls. Minimap: the game's own marker on every puppet
  (`navigation:RegisterHumanEntityDirect(puppet, 1, 1)`, an orange marker on the minimap and map), MAP MARKERS in
  settings; checked on both screens in Free Ride (the marker followed the other player into a building). Markers off
  was not checked in a capture.
- [x] Settings page (name, show tags, debug overlay). Done 2026-10-15: CO-OP SETTINGS (core v0.5.0 panel; main menu
  CO-OP > SETTINGS, the lobby, pause menu CO-OP > SETTINGS): NAME (text box; used from the next session), DEBUG
  OVERLAY, SHOW NAME TAGS, MAP MARKERS, FRIENDLY FIRE, all saved in MafiaCoop.ini with `Config::Set`. Checked in game:
  debug overlay off/on, name saved, name tags off in the world (pause panel > SETTINGS).
- [x] **Pre-game lobby** (user agreed 2026-10-15): main menu CO-OP > HOST CO-OP GAME opens a lobby screen listing the
  joined players with their character and ready state; guests reach it via JOIN BY IP / JOIN FRIEND / a Steam invite
  (it opens by itself once they are at the main menu) and pick their character (the batch-3 rows) and toggle READY;
  the host picks the MODE and presses START; the host's game loads that mode, every guest's loads the same, and
  everyone spawns together. Generic parts in core v0.5.0 (`net/lobby.h`: ready, mode, start broadcast, in-game state
  for late joiners, Steam lobby data `mc_state`/`mc_mode`; the LOBBY panel); UDP and Steam joins use the same
  messages. Done includes tests: core `net_lobby` (ready, start waits for everyone, reaches every guest once, guests
  cannot start, late joiner). **Free Ride works; Story next.** Checked in game 2026-10-15 (two instances, UDP): lobby
  over the main menu, the guest's character change (Sam -> Salieri) seen on the host's lobby, READY, MODE (STORY shows
  "NEXT VERSION" and cannot start), START: both games loaded Free Ride at once and both players stood at the Free Ride
  start seeing each other (Enter on each loading screen, as in single player); a guest joining while the host already
  plays sees "HOST IS PLAYING FREE RIDE" and JOIN GAME loads it. Story mode (continue the host's save / pick a chapter)
  comes with mission/chapter sync (B2). A real Steam invite into the lobby between two accounts is not checked.
  Story done 2026-10-15 (mod 0.6.0): MODE STORY (host CONTINUE, guests load the host's checkpoint save) and CHAPTER
  1..20 (everyone loads that chapter's shipped start save; the host's own story slot is not touched). Checked in game:
  STORY into Omerta cp_020, CHAPTER 10: OMERTA and CHAPTER 2: TAXI, JOIN GAME into the host's running story.

- [x] **Session password** (network side + tests done in 65c562f; menus checked in game 2026-10-15: SET PASSWORD on
  the host, guest gets the PASSWORD box on JOIN BY IP, joins with it): the host can set an optional password (CO-OP page: "Set password", reusing the text box
  from Join by IP; ini `password=`). Joining a passworded game prompts for it. Never sent in plain text: the host
  sends a random challenge after Hello and the guest answers with HMAC-SHA256(password, challenge) via Windows
  BCrypt (no extra libraries); wrong answers get Reject "wrong password", with a short delay and a per-address attempt
  limit. Steam invites from the host skip the password by default (option to require it anyway). The pause-menu
  panel shows a lock icon when a password is set. Protocol version bump; loopback tests for right/wrong/missing
  password and the attempt limit.

- [x] **Kick**: the host picks a player in the pause-menu CO-OP panel and chooses Kick; the player is disconnected
  with the banner "You were kicked by the host". Only the host can kick (core Session::Kick). Done includes loopback
  tests: the kicked player gets the reason, guests can't kick.
- [x] **Kick and ban**: Kick plus a ban for the rest of the session (SteamID and IP address); a banned player's
  rejoin is refused with a clear reason. The ban list clears when the host stops hosting. Done includes tests: refused
  on rejoin by SteamID and by address, list cleared in a new session. Done in core v0.3.0 (tests: net_kick); checked
  in game 2026-10-15 (kick, rejoin, kick and ban, banned rejoin refused; by address only, two instances on one PC).

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
- [x] Clean shutdown: call Coop::Shutdown when the game window closes so guests see "host left" at once, not a 10 s timeout.
  Done 2026-10-15 (core v0.5.0, its docs/protocol.md "Leaving cleanly"): `ExitProcess` / `TerminateProcess(self)`
  hooks leave the session before the process ends. Checked in game: closing the host's window -> the guest's banner
  "HOST LEFT: Host left the game" 4 ms later; closing the guest's window -> "Guest left the game" on the host 3 ms
  later; STOP HOSTING -> HOST LEFT at once. A kill from outside (taskkill /F) still times out after 10 s.
- [x] Overlay: release the backbuffer reference after each draw; log Present/ResizeBuffers thread ids in game to confirm assumptions.
  Done 2026-10-15 (core v0.5.0): a backbuffer view per frame, released after drawing; everything released on device
  removed/reset and at exit. In game: ResizeBuffers ran on the Present thread (logged) and succeeded right after the
  overlay had drawn; the exit release logged. Device loss itself was not provoked.
- [x] Steam join request: only record the lobby id in the callback and act after polling (no session reset mid-use).
- [x] Crash handler: don't let a first-chance stack overflow use up the one-shot dump flag.
- [x] Host relay rate limits for guest chat/app messages; evict oldest un-handshaken UDP connections when slots are full.
- [x] Launcher: require a value for keys like --password (reject "--password --x").
- Every pattern/Lua call fails soft (log + disable feature); keep it that way.
- Crash triage: `out\logs\crash-*.dmp` + log stack (module+offset) → symbolize with the PDBs in out\.
- Long-session test (1 h, two instances, driving + shooting) before each release.
