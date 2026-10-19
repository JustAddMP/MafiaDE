# Plan — next steps

Living plan for the native co-op rewrite. Update it as items land. Last updated 2026-10-19 (0.15.0 pre-release, first two-PC test; the Round 3 and Round 4 fixes merged, in-game check pending).
Read with `ARCHITECTURE.md` (game layer + clean-room rule), `protocol.md` (our app messages), `re-notes.md`,
`game-api.md`, `../README-dev.md`, and core's docs in `../external/core/docs/` (ARCHITECTURE.md, protocol.md,
interest.md).

## Where we are

| Phase | State |
|---|---|
| 1 Skeleton (launcher, injection, Present hook, crash dumps) | Done |
| 2 Connection (UDP + Steam lobby, handshake, overlay) | Done; Steam P2P between two real accounts works (2026-10-17 test); reconnect after a lost connection built in 0.15.0, needs the in-game test |
| 3 Second player (Lua bridge, puppets) | Done: on foot, weapons, shared cars, characters (picker in the CO-OP panel), damage (bullets, punches, rams) and respawns |
| 5 Native UI (CO-OP in main/pause menus, banners, lobby, settings, name tags, map markers) | Done: keyboard, gamepad (virtual pad) and mouse; Steam invite between two PCs works (2026-10-17); lobby: Free Ride, Story and chapter select |
| 4 Shared world | Weather/time, story (chapter, checkpoint, objectives), cutscenes and mission NPCs done (pairing, deaths and hits both ways, copies of host-only enemies; one chapter's fight checked); mission cars partly; 0.15.0 built, needs the in-game test: guest-owned NPCs, traffic and police, mission car back, failed missions in one load |
| 6 polish | Not started |

2026-10-19, first real two-PC test (0.15.0 release zip, core 0.11.0, over Steam; host the user, guest a friend; Free
Ride only, two sessions, no crash, no ERR line). Verified there, nothing ticked by it: Steam invite and join over
Steam P2P (twice), the lobby (characters, ready, FREE RIDE start on both), HOST LEFT on leave and hosting again;
movement and latency (loss about 0 %, rtt about 28 ms, playout delay about 40 ms, LAT p50 about 16 ms, the `held`
menu-wait split of the "Latency summary" check); car signals (horn only); ride along in seat 1 both ways; the host's
traffic on the guest's game (basic, with the problems fixed below); car damage sync (crashes, shot tyres and wheels,
reports from copies, the exploded flag); police (the host's cops chase a wanted guest's puppet, the host stays clean,
the guest's level held while chased but still decaying); guest-owned NPCs shown on the host (session 2); Free Ride
death and respawn on both games; PvP punches landing (through the swing fallback); traversal messages reaching the
puppet; the physical gamepad in the co-op panels and its prompts (host); a clean exit or leave from either side.
Seen again: the known issues fixed in the 2026-10-19 merges (melee visuals, minimap marker, car look, repair after
an explosion, guest police decay, garage.pf). Not tried: JOIN FRIEND, chapters, reconnect, doors, item takes,
grenades, seats after a reload, the pad guard, three or more players. Its new bugs (car copies at 0.28 of the speed,
two drivers in one car, bullets never hurting a player, motorbikes and water, the host's pause, driverless traffic
copies, character-change duplicates, guest cops without a profile) are fixed in code by the Round 4 merge (notes on
the items below), in-game check pending.

## Working rules

- Clean-room: no code from other multiplayer mods (see ARCHITECTURE.md).
- The game folder is read-only. Back up the save folder before in-world tests
  (`%TEMP%\claude\G--MafiaCoop\save-backup`), never start New Game, kill all game instances after a test,
  capture only game windows.
- Two instances on one PC: `out\MafiaCoop.exe --host --log host --debug-overlay` and
  `out\MafiaCoop.exe --join 127.0.0.1 --log guest --debug-overlay` (the status text is hidden without it). No `--name`:
  players are shown with their Steam name (both instances share one account, so both show the same name). Live Lua: F8 / `tools\devlua.ps1`.
- When a feature lands, also update the website (C:\AI\Claude\Projects\JS\justaddmp.com): tick the matching task
  in `app/lib/repos.ts` (Mafia entry, `progress.areas[].tasks`), add new tasks there when they're added here, refresh
  highlights / not-synced / known issues if they changed, run `npx tsc --noEmit` and `npm run build`, commit.
- Repo layout: this repo is the Mafia DE game layer (`src/game/`: descriptor `game_info.cpp`, `module.cpp`, Lua
  bridge, player sync, menus); everything game-agnostic is JustAddMP/core in the submodule `external/core`
  (`git submodule update --init` after cloning).
- Generic fixes go to core first: change and commit them in the core repo (its `build.cmd` + tests must pass, see
  its README.md), release it as a core version tag (core README "Versions"), then move this repo to that tag with
  `tools\update_core.cmd vX.Y.Z` (builds + tests) and commit `external/core`. This repo only ever pins core release
  tags (currently core v0.11.0 = 8577314, released with mod 0.15.0). Never edit core files only inside this repo's submodule checkout without committing them to core.
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
   Unreleased (Round 4 merge 2026-10-19, fixed in code, in-game check pending): gunfire between players (the
   2026-10-19 two-PC test: about 20 shots, no Hit, the friend brain made the game ignore our bullets on puppets):
   each shot is resolved from the game's DAMAGE, its ON_BULLET_HIT or a ray test of the aim against each puppet's
   body (walls by mc_raycast); a shielded puppet is resolved by the ray and ON_BULLET_HIT alone (pvp_hit.h, trace
   PV). Puppets in water and the puppet shield: a swimming or diving player is sent (pose bits 8 / 16) and his puppet
   shown at his position with swim strokes; puppets never die from local causes (drowning, falls, fire, explosion
   pictures, a sinking car: SetDemigod while at risk, puppet_guard.h), only a network death counts (water.h, trace
   PG / PD / PW / WT).
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
- [ ] **Animation sync** (0.13.0, built 2026-10-17 from static research only, needs the in-game check in README-dev.md
   "Test checklist"): puppets walk/run/sprint with the game's locomotion at the remote player's speed (gait + speed
   multiplier calibrated on the puppet, walk point; small nudges, snaps when far off) instead of being pulled every
   tick (feet slid); stand turns rate-limited; walks with him to the car door and gets in animated. Gate: no sliding
   feet, natural starts/stops/turns, error still ~0.5 m (sync trace, docs/sync-trace.md).
   First two-PC test of 0.13.0 (2026-10-17, Free Ride, ~5 min): walking stayed within 0.1-0.9 m, running and sprinting
   fell 1.3-3.2 m behind (the AI sprint is ~3.3 m/s) - 0.14.0 carries the shortfall (multiplier up to 1.8); animated
   get-in was cut after 1 s (ANIM_WAIT read before it was set) - fixed in 0.14.0; puppets are still thrown out of
   seats (logged in detail since 0.14.0).
   0.15.0 seats (static + the 0.13.0 logs, docs/game-api.md 18): the throw-outs come from the civilian brain (an AI
   civilian leaves cars). Every puppet now gets the friend brain on every game (where the game-sectors guard is in)
   before it is seated, its seat is checked every tick (`GetSeatOwner`) and it is put back at once; a ride along waits
   for the driver's puppet at the wheel (`SlideToSeat` if the game seats us there); `enemies_target_guests = 0` no
   longer sends hits. Built, needs the in-game test (README-dev.md "Test checklist: 0.15.0 features").
   Unreleased (merge 2026-10-19, fixed in code, in-game check pending): seats after a guest's reload (the 0.15.0
   test, Taxi cp_020: the guest hidden in the host's taxi for minutes): a remote car's copy waits 2 s, then stands
   beside his car with his puppet seated in it (seats.h `PlaceCopy` / `AsideSpot`) and moves onto his car once the
   cars have parted; a parked copy of his earlier car is dropped, car records are looked up again after a load
   (trace SCP / DRK).
   Unreleased (Round 4 merge 2026-10-19, fixed in code, in-game check pending): one wheel, one driver (the
   2026-10-19 two-PC test had both players drive one car for 100 s): a seat-0 reading while the driver's state still
   names the wheel is the game's slide, simultaneous claims go to the car's holder, else the lower id; the loser
   slides to a passenger seat or gets out, PlayerShouldRemainPassenger as a passenger, car damage follows the single
   driver; on a motorbike the loser counts as the rider behind (seats.h `LocalWheel`, trace WHL). Character changes:
   the old puppet is taken out of its seat, hidden and deactivated before its release, the new one takes the seat.
- [ ] **Car controls sync** (0.13.0, same): the driver's steering, throttle, brake and handbrake applied to the car
   copies (wheels steer, brake lights), copies carried by their own physics with a blended correction (pinned as
   before when the physics does not follow). Gate: wheels show the driver's steering, no wobble, error as before.
   First two-PC test of 0.13.0: every copy was pinned within ~4 s (0.2 m/s against 4.4) on both games; the steering
   values arrived. 0.14.0: engine started and kept on, the wanted speed given back after every correction (in case
   moving the car stops it; probed and logged), a pinned copy's physics retried after 5-40 s, readbacks of the copy's
   real controls, engine, gear and wheel angle in the sync trace.
   0.15.0: the puppet keeps its seat in the copy (seats, see the item above) and the copy shows the driver's lights
   and signals (item below). Built, needs the in-game test.
   Unreleased (Round 4 merge 2026-10-19, fixed in code, in-game check pending): the copies' speed. The 2026-10-19
   two-PC test measured copies at exactly 0.28 of their driver's speed, pinned 56-74 % of the time: SetSpeed takes
   km/h (read from the exe) and was given m/s. Every caller now gives km/h (player cars, traffic, negative when
   reversing) and GetSpeedFloat is read as km/h (car_units.h, mc_car_speed_arg / mc_car_speed_ms); the SetSpeed probe
   logs once. Motorbike copies are pinned to the rider's transform instead (no SetSpeed).
- [x] **Car signals** (0.15.0, built 2026-10-18 from static research, docs/game-api.md 7/16, re-notes 9): headlights,
   indicators, horn, siren and beacon read from the driver's car (vehicle part +0xD00 / +0xA88 / +0x119C,
   `mc_car_signals`), one byte at the end of the PlayerState vehicle block, set on the copy on change
   (`mc_car_set_signals`, SetHornOn / SetSirenOn / SetBeaconLightOn, SetLightState) and re-set when the copy's game
   changes them back; unit test car_signals. Gate: lights, indicators, horn and a police car's siren and beacon show
   on the copy within a moment, nothing flickers. Built, needs the in-game test.
   In-game 2026-10-18 (two instances, one PC, Free Ride): the game's own headlights in a tunnel reached the copy (`signals off -> park+head (copy has off, change #2)`, off again outside); indicators A/B, horn, siren and beacon set through the host car's own setters (the player has no key for lights or indicators) each showed on the copy (`indA`, `indB`, `park+head+horn`, `park+head+siren+beacon`, changes #4-#11, no re-sets, tail lights lit in the capture). A real police car was not driven.
- [ ] **Melee and other visible actions on puppets** (0.15.0, built 2026-10-18, static only, docs/game-api.md 17):
   punches, charged punches, counters, takedowns, throws and melee hit reactions sent as an action block (u8 seq,
   u8 kind) at the end of PlayerState and played with the game's own animation (`AnimPlay`) on the puppet, its walk
   control paused meanwhile; a puppet's swing cannot hurt anyone (the real Hit still applies); unit test
   player_actions_test. Gate: punches / counter / throw visible on the other game, no extra damage, no sliding or
   snap during a swing. Still missing: door kicks, grenade explosions, medkit, carrying bodies, lockpick, vault /
   ladder, jump, stalk, paired takedowns. Built, needs the in-game test.
   In-game 2026-10-18: BROKEN (visuals). Light / heavy punches go out and arrive (`our light goes out`, `puppet 0 plays a light punch: hero_nw_alert_melee_attack_light_1_v1`, trace AL/ACS/ACT/AP) and the real hit lands (36 damage), but no punch shows on the puppet (burst captures side-on); every AE ends `done=0 why=time_up`. From the console AnimPlay of a punch shows nothing within 5 s, while AnimPlay of `hero_nw_calm_idle_fidget_cracknuckles` plays, ~2 s after the call every time. The throw animation ends after 0.14 s the same way. Needs another way to play the melee set (not loaded outside a fight?) and the 2 s AnimPlay latency explained.
   Unreleased (merge 2026-10-19, fixed in code, in-game check pending): each action group tries the game's ways
   in turn (PlayAnimation direct, no-transition, DesirePlayAnimation, script control, script control + combat
   mode, AnimPlay), judged by the puppet's hand bones, the first that moves them kept (game/anim_methods.h,
   game-api.md 17, re-notes 12; log `ANIM: ... SHOW with ...`, console `MafiaCoop.TestAction`).
   Unreleased (Round 4 merge 2026-10-19, fixed in code, in-game check pending): punch lock-on (the 2026-10-19
   two-PC test: every swing had opponent 0, the friend brain makes puppets no melee target): at a swing the nearest
   puppet within 2.6 m and 70 degrees is locked on, we are turned to it (SetDir) and it is hit within 2.3 m at the
   impact (pvp_hit.h `PickMelee`, trace PVM).

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
  Unreleased (Round 4 merge 2026-10-19, fixed in code, in-game check pending): one PlayerModel per settled pick (the
  2026-10-19 two-PC test respawned the host's puppet five times in 2 s while he stepped through the list, one release
  seated in the guest's car): the pick goes out once it has stayed for a second or the panel closes (model_change.h);
  the old puppet is unseated, hidden, deactivated, then released, its id never taken for an NPC, and the police turn
  to the new one (trace CHM / PRT).
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
   (checked in Taxi). Fixed in 0.9.1 (re-notes 7): a failed mission loaded the player's own story save (the user's
   Omerta) before the takeover, and the host's game then sent that story's checkpoint to the guests; now that load
   gets a save of the session (the Free Ride start) and the takeover leaves it as soon as it is up. Checked: host
   failures 5x Taxi cp_010 (one with the real save already cached in the game, one with a guest connected) and 3x
   Omerta cp_020, guest failures 3x Taxi (CHAPTER) and 1x Omerta (STORY); a STORY host keeps the game's own fail
   screen.
   Not checked: failing by timer or a destroyed mission car. Also 0.9.1: the slots' profiles (`aslot.pf`, the story
   slot's script data) were rewritten by co-op chapters (unnoticed before: the checked chapter was the user's own
   Omerta); now redirected with the saves, all files byte-identical after a two-player Taxi game with failures. The guest is placed beside the host (1.5 m to his right) after loads and cutscenes; when a car
   stands on that spot he goes to the other side or behind the host (0.8.0; checked: a car spawned on the spot, the
   guest landed on the ground 3 m from it). The host of a lobby CHAPTER session is redirected too (0.8.0; before, the
   chapter overwrote his Free Ride slot): checked with CHAPTER 10: OMERTA through the intro to cp_020, the host's
   checkpoints still reached the guest from his scratch tree, every save and profile file byte-identical afterwards.
   A STORY host keeps saving his own story (his campaign). Fixed in 0.8.0: a guest who joined at start-up
   (`--join`, an invite) got the lobby over the title screen, and a START then loaded from behind the title and hung
   on a black screen; "at the main menu" now needs the first script tick (game-api.md 13). Checked: no lobby over the
   title or the loading screen, lobby STORY and CHAPTER starts with a `--join` guest loaded both games.
   0.10.0 (2026-10-16, re-notes 7): **a reloaded checkpoint starts where its save has the player**: a chapter-loader
   load always put him at the Free Ride start (Salieri's bar), whatever the save said; Omerta cp_020 does not move him
   itself, so its reload began at the bar and its leave-the-area check never armed. Now the save's player position is
   read from the save and he is put there before the quest starts. Checked: Omerta cp_020 failures host 5 (solo) + 3
   (guest connected), guest 2, each back at the park with the check active again (every later run failed again from
   the reloaded checkpoint); Taxi cp_010 host 2 + guest 2 at the depot. **One loading screen less after a failed
   mission**: the short Free Ride stop stays (a one-load variant, `fail_load = direct`, ran the checkpoint as a story
   load and its own fail screen read the real story save, so it is off), but its loading screen is confirmed
   automatically (Enter through DirectInput, 3 s after it is up; the game reads it only while its window has focus) and
   the screen stays black until the checkpoint loads; about 20 s from the failure to the checkpoint's loading screen.
   **Other failure kinds**: a destroyed mission car (Taxi, host and guest) goes the same way and came back to the
   checkpoint; with the player inside the car he dies instead (death reload). A timer failure exists in the Race chapter
   (`mm_50_race_go_time_out`) but was not reached. A guest whose own flow plays a cutscene of an earlier checkpoint no
   longer sits through it while the host is already past it (Bon Appetit: 3 minutes of ambush film): only the host's
   cutscene holds a reload. A crash in the game-sectors walk during a failed mission's load with a friend-brain puppet
   (Omerta, host) is guarded (re-notes 8).
   Unreleased (merge 2026-10-19, fixed in code, in-game check pending): `garage.pf` and the other progress
   profiles (my2k_game_stats.pf, profile_data_storage_persistantdata.pf, unknown *.pf) written in a co-op session
   (0.15.0 test: a co-op chapter changed the garage) go to the session's scratch copy with the slots and stay held
   until the game restarts; settings stay the player's own (save_paths.h, unit test save_paths_test).
- [x] **Failed missions in one load** (0.15.0, built 2026-10-18, static, docs/re-notes.md 7, game-api.md 14,
   protocol.md "Story"): the save system's FreeRoamRetry (exe+0x21E8B20, the Free Ride stop) is skipped in co-op
   chapters, so every fail kind takes one load: a chapter host goes straight to his newest checkpoint, a STORY host
   keeps the game's fail screen, a guest's failure is decided by the host (REJOIN / WAIT), a host's failure takes
   every guest with him (HOST_FAILED); the director's GameOver is hooked for the log; MissionFail 0x0205 /
   FailVerdict 0x0206, FailFlow (`src/game/fail_flow.h`, unit test fail_flow_test), trace FAIL; no fail role while
   a guest reconnects. Fallback to the game's way when still in the failed world after 20 s. Gate: Taxi cp_010, drive
   > 250 m away or `car:Explode()`: no `freeroam_before_quest_activated` load, one loading screen on each game,
   host and guest failures. Built, needs the in-game test. (Replaces the two-load path of 0.10.0 described above.)
   In-game 2026-10-18 (CHAPTER 2: TAXI, cp_020): host's mission taxi blown up -> `FreeRoamRetry ... as chapter host: skipped`, ONE load of checkpoint_mm_20_taxi_cp_020 on each game, 0 `freeroam_before_quest` loads, guest `verdict HOST_FAILED` -> waits at the menu -> loads when the host is back; guest's taxi blown up -> host `REJOIN (we play on, he loads our point)`, guest one load, host none. Trace FAIL freeroamretry skip=1 / verdict. (Driving the taxi 300 m away did not fail cp_020; no `ev=gameover` record was written; both failing at once not tried.)
- [x] **NPC mirroring** (research: docs/research-world-sync.md): host enumerates relevant humans (mission NPCs, enemies near players), streams
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
   0.9.0 (2026-10-16, game-api.md 15, re-notes 8): **enemies now react to guests**: the host gives each guest's
   puppet the FRIEND brain; the crash it caused before is understood (a freed friend-brain human stays in the
   game-sectors module's list, exe+0x243A915) and guarded in the dll (stale entries dropped before each walk: the
   repro that crashed every time now logs "dropped 1 stale human" and runs on; a 15-min churn of guest reloads,
   factory resets and a host death: no crash). Checked in Brothel cp_015 with enemy crews spawned by script on both
   games at the same spot (paired like mission NPCs): the host's enemies shot the guest's puppet, the hits reached the
   guest (100 -> 13, downed, respawned beside the host) and are skipped while his own copy of that enemy fights him;
   host NPC deaths repeated on the guest. **Still open:** a chapter's own scripted shootout (every chapter start
   probed begins with a drive or walk the tests cannot follow), a guest kill re-checked in a fight (checked in Taxi
   before), civilian copies that take no damage.
   0.10.0 (2026-10-16): **a chapter's own shootout reached through its mission flow**: Bon Appetit (lobby CHAPTER), the
   host drove Salieri's car to Pepe's (the car moved by script), the chapter's ambush cutscenes played on both games
   and both were in the restaurant fight at cp_040 (`B3-pepes-host.png`, `B3-pepes-guest.png`). Seen: 11 of the host's
   18 NPCs paired on the guest; the guest's own enemies shot him (downed, respawned beside the host, then killed and
   reloaded at the host's checkpoint); the host's enemies shot the guest's puppet and those hits reached the guest
   (skipped while his own copies fought him); host NPC deaths (4) repeated. **Not checked**: a guest kill counted on
   the host: the enemies of that phase took no damage on either game (the host's own `DoDamage` and bullets left them
   at 100), and after a guest reload in the moving fight his copies did not pair again. The guest's copies keep 5000
   health while the host's NPC lives, so a guest alone in a fight cannot win it. Not ticked. Civilian copies that take
   no damage: not investigated further.
   0.11.0 (2026-10-16, game-api.md 15, protocol.md "NPCs"): **guest copies are killable, the host's NPC dies with
   them**: no more 5000 health; a copy's health follows the host's NPC down (u16 in NpcStates), the guest's shots hurt
   it on his game and still go to the host (replayed from his puppet), and when they kill it the guest sends a lethal
   NpcHit: the host's NPC is killed 0.8 s later if the hit itself did not do it. **Re-pairing**: NPCs with a job that
   found no partner at the same spot within 2 s are paired with the nearest free local NPC of the same battle
   archetype (a tag in NpcStates) or AI type within 40 m (nearest first over all pairs), dead host NPCs too (their
   copy dies); a pairing is released after 5 s without states. **A host reload is followed**: a guest at the host's
   checkpoint loads it again when the host left his world (failed mission, death) and came back. **Passers-by that
   took no damage** were deactivated humans (`IsActive()` false on both games: not simulated, no damage from bullets,
   `DoDamage` or `health = 0`) that had been paired; they are skipped now. **Why the Bon Appetit enemies took no
   damage**: the mission keeps the four attackers in front of Pepe's `invulnerability = true` through cp_040 and
   cp_050 (on each game, for its own player; "Circle round and ambush attackers"); only the waves that come round the
   back are vulnerable. A guest's kill of such an enemy (his game let him) makes the host's NPC vulnerable for it;
   invulnerable allies and civilians are left alone. Checked in two runs of Bon Appetit (lobby CHAPTER, Salieri's car
   to Pepe's, ambush cutscenes, cp_040 and cp_050): 10 guest kills counted on the host (`guest N killed his copy` ->
   `host NPC died`, 5 by the replayed shots, 5 by the lethal hit), host kills repeated on the guest, a host
   `DoDamage(30)` took the guest's copy 100 -> 64 too, the host's enemies shot the guest's puppet and the hits reached
   him (downed, respawned), the guest alone cleared both cp_050 waves (4 + 4 enemies, the host did not shoot); the
   host walked from cp_040 into cp_050 and the guest followed his checkpoint; after the guest's reload mid-fight 7 of 7
   and 8 of 8 enemies paired again (`same kind, moved apart` at 5-8 m), after the host's death reload the guest
   reloaded on his own and 6 of 6 paired; no crash in either run. **Not done**: the cp_050 ambush itself (the four
   invulnerable attackers) was not completed: the tests teleport and cannot walk the route the mission waits for, so
   the chapter did not reach cp_060; no second chapter's fight. Not ticked.
   0.12.0 (2026-10-16, game-api.md 15, protocol.md "NPCs"): **host-only enemies get a copy on the guest's game**:
   each game spawns a checkpoint's waves for its own player, so an enemy only the host's game had brought in shot the
   guest unseen. Now a host NPC with a job that stays unpaired for 5 s (2 s loose pairing + 3 s), alive, on foot,
   within 90 m of the guest and with a known spawn profile is spawned on the guest's game from a second runtime
   spawner (like the player puppets): same profile, the host's weapon, driven from the host's states like a player
   puppet (position plus velocity, pulled along, heading), firing when the host's NPC fires (ON_SHOOT counted on the
   host, a u8 in NpcStates), health and death from the host, the guest's hits and kills going to the host as before.
   **The profile**: no getter on a human (the hash is not in its object; searched at runtime with `mc_memfind`); a
   mission spawner names its humans `behavior_object_prime-NN-<spawner>`, the spawners are type-65 entities whose
   runtime spawner component reports the profile (u64 in NpcStates). The copy is a civilian factory spawn but takes
   the enemy profile's brain (AI type 2): no brain switch, no melee orders; in a fight it took out its own pistol and
   some bullets hit the guest although it aims past him, so those are given back on the guest and the host's gun put
   back. No doubles: a free local NPC of the same kind at the same spot or within 40 m takes over from a copy (also
   seen when the guest's game spawned its own after a load); same-spot pairing now needs the same kind (the guest's
   kitchen rifleman had been paired with the host's kitchen machine gunner). Copies go 12 s after death, 5 s after the
   last state, in a car, beyond 110 m, during cutscenes, at session end and with the world (load); at most 10; none in
   the first 10 s after a load. Crash safety: copies never get a brain switch or a map marker, burning ones are hidden
   and released after the fire, the game-sectors guard dropped 4-5 stale humans at the failed-mission loads.
   **Checked in Bon Appetit** (lobby CHAPTER, three sessions of 22-24 min, run logs in the batch notes): host walked
   into the kitchen (real steps, `walk.lua`) -> host-only kitchen machine gunner, kitchen rifleman, yard triggermen,
   balcony gunner and alley machine gunner appeared as copies on the guest (15 copies, 2 forced by switching off the
   guest's own enemy), 0.0-0.3 m from the host's standing, ~1 s behind running, firing with it (`B3c-copy-guest.png`);
   guest killed 5 copies -> host's NPC died each time; host kills killed the copies; `4 spawned copies went with the
   old world` on the guest's follow load, none doubled after loads (9 of 9 paired); 3 failed-mission reloads, no
   crash. **The cp_050 ambush is solved**: walk kitchen -> back door -> alley (373.9 417.1 .. 395.1 414.4) -> behind
   the attackers (410 418 .. 410 398) with the alley wave dead, and the four attackers become vulnerable; the whole
   restaurant fight was cleared on both games (one attacker killed by the guest). **Not done**: right after the
   cleared fight the mission failed both times on the way to Salieri (`dummy_cp_050_obj_search_for_salieri`; probably
   the teleport steps), so the chapter did not reach cp_060; no second chapter's fight; enemies only the guest's game
   spawned are still his alone (built in 0.15.0, below; needs the in-game test). Ticked: a chapter's real fight works
   end to end, host-triggered waves included.
   0.15.0 (2026-10-18, static, protocol.md "Guest-owned NPCs", research-world-sync.md): **guest-owned NPCs**: an NPC
   only a guest's game has (he got somewhere first) is reported to the host (GuestNpcStates 0x0420, 10 Hz) and shown
   there as a driven copy (a mirror); hits and deaths go both ways (HostNpcHit 0x0422, GuestNpcEnd 0x0421), and when
   the host's own game spawns that NPC later the host claims it (NpcClaim 0x0423) and authority goes back to the
   host. Ownership rules in `src/game/npc_owner.h` (unit test npc_owner_test); traffic copy crews are never
   guest-owned (mission ObtainCarWithCrew crews stay ownable); trace NM / NO / NGO / NGE / NGC / NMC / NMX. Built,
   needs the in-game test (a fight where the guest goes first).
   Unreleased (Round 4 merge 2026-10-19, fixed in code, in-game check pending): guest-owned police and other NPCs
   without a spawn profile (the 2026-10-19 two-PC test: 12 guest cops, none shown on the host) get a police or
   civilian fallback profile by AI type; host cops chasing in cars (or faster than 11 m/s) are no longer paired with,
   and pulled to, cops on foot on the guest's game, a running host cop's partner is steered after it; none of this
   (and no pull-back) while the host's game is paused or just resumed (trace NP spd / car=2, NGS unpairs / steers /
   fallbacks).
- [ ] **Vehicles + traffic** (research: docs/research-world-sync.md): host's mission cars mirrored; ambient traffic either disabled on guests
   (`game.traffic` SwitchGenerators) and mirrored, or left local (cheap, accepted desync). Start local.
   Partly done 2026-10-15 (game-api.md 15): ambient traffic stays local on every game (accepted). A driven car whose
   model stands at the same spot on the other game is taken as that game's car (the mission's cars are not doubled:
   the copy spawned on top of the guest's mission taxi had made his mission fail), and no copy is spawned into another
   car. Checked in Taxi (host took the mission taxi, the guest's game used its own; both in their own taxis after the
   guest loaded cp_020, no copy until they parted). **Open:** the host's puppet is thrown out of a mission car on the
   guest's game (it does not keep an NPC at the wheel), only one chapter checked, police and mission traffic local.
   0.9.0 (2026-10-16): the guest's mission car is swapped for a copy (his own hidden meanwhile) when the host's puppet
   is thrown out of it twice; the puppet then stays at the wheel (Taxi cp_010 while the host drove, and the copy at
   cp_020). Copies wait for the loading screen to be gone (one spawned during the load flipped the guest's taxi).
   **Still open:** the mission car coming back when the host parks (Taxi does not let the player out), a second
   chapter (the chase and Taxi cp_020 put each player in his own mission car, so the host's puppet waits hidden).
   0.10.0 (2026-10-16): **second chapter**: Bon Appetit, the host drove Salieri's car (Salieri beside him); the guest's
   game took its own Salieri car as the host's ("our own lassiter_v16_roadster stands there: it is this car here") and
   the host's puppet sat at the wheel with Salieri beside him for the whole drive, no throw-outs
   (`B4-salieri-wheel-guest.png`). **Still open**: the mission car coming back when the host gets out: neither chapter
   lets the player out (Bon Appetit kept the doors locked even after `UnlockPlayerEntryPoints`). Ambient traffic and
   police stay each player's own (accepted). Also fixed: the entity scan (`mc_entity_ids`) missed most cars after
   several reloads in one game (new entities beyond its 8192-slot gap limit; now 65536), so mission cars were not
   found. Not ticked.
   0.15.0 (2026-10-18, static, game-api.md 15 "Mission car back" and 19, protocol.md "Shared cars" step 5 and
   "Traffic"): **mission car back**: a real car hidden behind a copy (the guest's mission car, and now also the
   host's own car, which stayed hidden for good) comes back at the copy's place once the last driver's CarParked
   arrived and the copy is empty (or it stood still, without CarParked), on a new world and at session end; the
   copy is locked for us meanwhile; a destroyed car comes back blown up (CarParked flags byte); machine in
   `src/game/car_swap.h` (unit test car_swap_test), trace SWP / SWR. **Traffic and police**: the host's moving
   traffic and police cars near a guest are sent to him (TrafficSpawn / Despawn / States / Bump, 0x0220-0x0223,
   `traffic_model.h` / `traffic_sync.*`, unit test traffic_test); within 250 m of the host the guest's own ambient
   traffic and police spawning go off, his moving cars out of the way are hidden, and the nearest 20 host cars get
   crewed copies driven with core's VehicleController (police with siren and beacon); trace TRH / TRP / TRG / TRC /
   TRL. Both built, need the in-game test: the "mission car coming back" and "traffic and police each player's own"
   points above are built, needs the in-game test. Pedestrians stay local.
   Unreleased (Round 4 merge 2026-10-19, fixed in code, in-game check pending), from the 2026-10-19 two-PC test:
   traffic copies kept losing their drivers (249 get-outs for about 430 copies): a host car only counts as
   driverless after 1 s without a driver read while slow (DriverWatch), a copy's crew only gets out when both cars
   stand and is seated again when found outside; crews use the civilian and police spawn profiles
   (traffic_base_ambient_combined, police_scotch) instead of Tommy (trace TRO, TRH nodrv / orphan). A paused game
   says so (PeerPaused 0x0103, peer_pause.h): the others hold his puppet, his car and (host) his traffic copies, and
   report no knocks until 1 s after the resume (trace PAU / TRZ). Motorbikes are vehicles (the test sent the host's
   bike ride as an on-foot sprint at up to 32 m/s and its mount as jumps): found through GetOwner /
   GetOwnerSceneObject and the motorcycle component, sent with the bike pose bit 32, a copy pinned to the rider's
   transform with the puppet on it or perched on its saddle; a player reported on foot above 12 m/s has his puppet
   hidden (T field 68; trace BK / BKS / FF / TS). A car copy under water leaves its engine to the game (CUW).
   In-game 2026-10-18 (0.15.0, one PC): traffic near the host shown on the guest (`the host's traffic shows here`, back to his own beyond 250 m, copies p95 1.1 m off the host's cars, bumps put back and reported). Police: a guest's wanted state reaches the host; the 0.15.0 offence announced from his puppet made the HOST wanted (fixed 968d300: no announce, cops attack a hunted guest's puppet; retested: `our cop ... set on guest 1's puppet: ok`, host stayed clean). Mission car back: the swap and the real car restored at the copy's place when the guest's world went (`our own car 3102474 is back at the copy's place`); the parked case not reached. A player's car copy is another variant/colour of the model (no colours/plate for player cars on the wire).
- [ ] **World sync added in 0.15.0, not yet verified in game** (built 2026-10-18 from static research, each with
   unit tests; README-dev.md "Test checklist: 0.15.0 features"):
   - Doors (protocol.md "Doors", game-api.md 21): doors shared on every game, the host decides story doors, door
     kick / lock pull / door open on the puppet. Not reached in the 2026-10-18 test (no doors near).
   - World items (protocol.md "World items", game-api.md 20): weapons on the ground and pickups shared, the host
     decides takes, collectibles per player. Not reachable in Free Ride (no item both games have).
   - Traversal on puppets (game-api.md 17): jumps, vaults, ledges, ladders, cover slides, sneaking, medkit, bodies.
     2026-10-18: teleport drops played as jumps / hard landings; the rest not tried.
   - Explosions and the grenade arc (game-api.md 23): explosions on every game as harmless pictures, a throw's
     flight drawn on the others. 2026-10-18: the messages and the flight numbers arrived (0.0 m off at the
     explosion), the pictures were not seen on screen.
     Unreleased (Round 4 merge 2026-10-19, fixed in code, in-game check pending): CAR_HAVE_PROBLEM is read from the
     car (the message has no getter: the 2026-10-19 test logged `Attempt to call a nil value` on both games), so car
     explosions are detected.
   - Car damage (protocol.md "Car damage", game-api.md 24): dents, parts, tyres, fire and explosions on every game.
     2026-10-18: crash damage and a blown-up car reached the copy; the copy's wreck damage came back to the host as
     a report and was read as a repair (to fix).
     Unreleased (merge 2026-10-19, fixed in code, in-game check pending): a copy never reports its wreck or the
     game's reaction to our writes, the holder never reads a wreck's change as a repair (CDX echo=). The other
     known issue, a player's car with another variant, colour and plate on the other screen: CarLook (0x0253)
     gives copies of player and traffic cars the car's tuning items (body variant), colours, plate, tint,
     interior and painting (game-api.md 24 "Car look", re-notes 13; unit test car_look_test).
     Unreleased (4500b98, fixed in code, in-game check pending): a copy's tuning (which rebuilds the car) waits
     while a player sits in it, a new copy holds the puppet back until its look is on, car damage asks for the
     whole state after a rebuild; puppet script control (melee) waits for the friend brain switch, is given back
     before a seat, door walk or ragdoll, is cleared on respawn, and late answers are ignored.
   - Police (protocol.md "Police"): wanted states shared, the host's police chase a wanted guest's puppet near the
     host. The offence fix (968d300) is checked in game; the guest's level still runs down during a chase.
     Unreleased (merge 2026-10-19, fixed in code, in-game check pending): the guest's zone decay is held
     (PreventZonesDecay) while the host's police are on him and released when the chase ends; a zone that decays
     during the chase is started again (trace POD / POZ).
   - Several guests (protocol.md "Several guests"): 3-4 players, other guests' NPCs relayed, seat rules, driver
     claims. Only unit tests (core four-player lobby and reconnect tests); no in-game test with three games.
   - Latency (protocol.md "Latency budget", core 0.11.0): lanes and priorities per system, adaptive playout delay,
     LAT / CLK trace. 2026-10-18 (one PC): playout delay 33-40 ms, jitter p95 9-15 ms; menu waits now listed as
     `LAT held=1` (48f827c, not yet checked in game).
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
- [x] Gamepad + mouse in our panels. Done 2026-10-16 (core v0.7.0 test input, README-dev.md "Testing the co-op
  panels"): with a virtual XInput pad (the same hook a real pad goes through) and mouse events, two instances in Free
  Ride: pause CO-OP panel d-pad up/down, right/left on CHARACTER, A, B, mouse hover/click/wheel; CO-OP SETTINGS; the
  lobby (host pad, guest mouse); real window mouse messages (PostMessage) selected and clicked a row too. The panels
  were opened through the test input, not the pause menu's CO-OP entry (that entry was checked with the keyboard in
  0.5.0). A physical gamepad needs a human (0.15.0 pad fixes: built, needs the in-game test, item below).
- [x] A real Steam invite with two accounts on two PCs. Done 2026-10-17 (the user and a friend, 0.13.0, logs): the
  friend joined the host's Steam lobby from Steam ("lobby join requested ... via friend"), connected over Steam P2P in
  5 s (rtt ~30 ms, 0 % loss), went through the lobby (ready, FREE RIDE start) and played about five minutes; leaving
  showed HOST LEFT at once.
- [ ] JOIN FRIEND (the CO-OP menu entry) between two PCs: not tried yet (the invite above came through Steam).
  0.15.0 (core 0.11.0, `net/steam_friends`, README-dev.md "Player flow"): JOIN FRIEND opens core's friends panel
  (Steam friends playing Mafia DE: hosting with players / max and lobby or playing, in a friend's game, not hosting,
  other version, full; REFRESH, STEAM FRIENDS LIST), joins through the friend's Steam lobby, the version is checked
  before connecting (invites too), Steam join errors in words; unit test steam_friends (fake Steam). Built, needs
  the in-game test (two accounts, two PCs).
  In-game 2026-10-18 (one PC): the panel opens from the pause CO-OP panel when idle, `NO FRIEND IS PLAYING MAFIA: DEFINITIVE EDITION (9 ONLINE)` with the hint line, pad Y refreshes (`coop: friends list refresh`), B goes back. Listing and joining a friend still needs two PCs.
- [ ] **Physical gamepad in the co-op panels** (0.15.0, core 0.11.0 `pad_nav`, README-dev.md "Testing the co-op
  panels"): every XInput slot, hotplug (empty slots probed once a second, logged), stick hysteresis with the
  dominant axis (left / right too), repeat, a button held at open ignored, B / START / BACK = back, an unplugged pad
  no longer blocks the game; unit test pad_nav. Built, needs the in-game test with a real pad.
- [ ] **Gamepad button prompts on the co-op panels** (0.15.0, core 0.11.0 `ui_prompts`, docs/game-api.md 9 "Button
  prompts"): the footers follow the input device like the game's Infobar (pad glyphs after pad input, key names
  after keys / mouse, each switch logged); X / Y / LB / RB act through the prompts (lobby X START / READY / JOIN
  GAME, Y INVITE; CO-OP panel Y INVITE; JOIN FRIEND Y REFRESH; text boxes X delete, Y paste); the main menu's CO-OP
  page keeps the game's own footer; unit test ui_prompts. Built, needs the in-game test.
  In-game 2026-10-18 (test pad): lobby footers switch to pad glyphs after a pad press (`prompts now for gamepad`, host `[A] Select [X] Start [Y] Invite [B] Back`, guest `[X] Ready`) and back to key names on a mouse move; pad X started the lobby, X readied, JOIN FRIEND `[Y] Refresh` worked. Not checked: text boxes (X delete / Y paste), pause panel Y INVITE, the main menu CO-OP page footer. Found: closing a panel with a pad button left the game's keyboard blocked on the loading screen (core ui_input: the release guard reads a pad state that is only refreshed while the game polls XInput or a panel is open).
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
  so a tag also showed through walls. Occlusion done in 0.10.0 (game-api.md 13): the ray cast behind
  `GetCollisionPointBelow` works with any from/to (`mc_raycast`); a wall between the camera and the head hides the tag
  (checked in Free Ride: shown 14 m away in the open, hidden behind a building, both ways twice). Also fixed: a puppet
  told to die that did not die kept its tag hidden after its player came back Minimap: the game's own marker on every puppet
  (`navigation:RegisterHumanEntityDirect(puppet, 1, 1)`, an orange marker on the minimap and map), MAP MARKERS in
  settings; checked on both screens in Free Ride (the marker followed the other player into a building). Markers off
  was not checked in a capture.
- [ ] **Every player on the map at all times** (0.15.0, built 2026-10-18, static, docs/game-api.md 13): one
  navigation icon per other player while we are in a world: on his puppet on foot, at his position (his car, no
  puppet yet, hidden, respawning, far away) otherwise, moved between the two (`ChangeIconTargetToPos` /
  `ChangeIconTargetToEntityDirect`), never made twice for one puppet and moved off a puppet before it is released;
  names on the icons (`SetIconTextByNavId`); decision in `src/game/map_markers.h` (unit test map_markers_test), trace
  MK / MKE. Gate: the other player stays on the minimap and map while he drives, after a respawn and a cutscene,
  with his name, no crash on reloads. Built, needs the in-game test.
  In-game 2026-10-18: the logs show one icon per player through spawn, car, reconnect (`made at his position (no puppet yet)` -> `made on his puppet` -> `at his position (in a car)` -> back on his puppet), never two; but no orange marker for the host could be found on the guest's minimap with him 12 / 40 m away. Not confirmed on screen.
  Unreleased (merge 2026-10-19, fixed in code, in-game check pending): the puppet's friend brain switch (~2 s
  after its spawn) removed the icon on it; the icon now waits at his position until the switch is done, icons on
  puppets are checked once a second (`GetIdByScriptWrapper`) and made again when the game dropped them
  (map_markers.h brainPending / probe, game-api.md 13).
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
- [x] Package: `MafiaCoop.exe` + `mafiacoop.dll` (+ README) in a zip, a few MB; `tools\make_release.cmd`.
   Done 2026-10-15 (0.8.0): `tools\make_release.cmd <version>` builds, runs every core test and proxy_test, and writes
   `MafiaCoop-<v>.zip` (exe, dll, proxy dll, player README with known issues, MinHook licence; 844 KB) and
   `MafiaCoop-<v>-symbols.zip` (PDBs). Checked: unzipped into an empty folder, `MafiaCoop.exe --check-only` found the
   game through Steam and reported the supported build.
- [x] Optional proxy-DLL install mode (built and tested on a fake game folder in afa56cd: `MafiaCoop.exe --install`; tick after a real start from Steam) (drop-in so the game can be started from Steam) — must be opt-in because it
   writes into the game folder. Real start from Steam checked 2026-10-15 with the unzipped 0.8.0 release: `--install`
   wrote `version.dll` (= `mafiacoop_proxy.dll`, same SHA-256) and `mafiacoop_path.txt`; `steam://rungameid/1030840`
   started the game, the log said `loaded by the version.dll proxy ... (game started without MafiaCoop.exe)`, the main
   menu showed CO-OP; after quitting, `--uninstall` removed both files. Game folder before/after (5215 entries: names,
   sizes, times; SHA-256 of every top-level file): identical except `BootCount` and one `cache\...\*.md`, which the
   game itself rewrites on every start.
- [x] Detect unsupported game builds (exe CRC) and show a clear message. Done: launcher CRC32 check against
   `core/version.h` (cached, `--check-only`, `--skip-version-check`); dll banner when patterns are missing.
- [x] GitHub pre-release v0.8.0 with the new zip (and a symbols zip). Done 2026-10-15:
   https://github.com/JustAddMP/MafiaDE/releases/tag/v0.8.0 (pre-release; release notes list features and known issues).
- [x] GitHub pre-release v0.9.0 (player sync, enemies target guests, mission car driver, gamepad/mouse in the panels;
   core v0.7.0). Done 2026-10-16: https://github.com/JustAddMP/MafiaDE/releases/tag/v0.9.0 (zip + symbols zip).
- [x] GitHub pre-release v0.9.1 (a failed mission never loads a save outside the co-op session; FREE RIDE after a
   chapter no longer crashes). Done 2026-10-16: https://github.com/JustAddMP/MafiaDE/releases/tag/v0.9.1.
- [x] GitHub pre-release v0.10.0 (checkpoint reloads at the save's spot, failed-mission stop confirmed for the player,
   name tags behind walls, 4 s disconnect detection; core v0.8.0). Done 2026-10-16:
   https://github.com/JustAddMP/MafiaDE/releases/tag/v0.10.0.
- [x] GitHub pre-release v0.11.0 (fights: guest kills count on the host, health follows the host, re-pairing after
   reloads, guests follow a host's checkpoint reload; no core change). Done 2026-10-16:
   https://github.com/JustAddMP/MafiaDE/releases/tag/v0.11.0.
- [x] GitHub pre-release v0.12.0 (copies of enemies only the host's game spawned; NPC messages changed). Done
  2026-10-16: https://github.com/JustAddMP/MafiaDE/releases/tag/v0.12.0.
- [x] GitHub pre-release v0.13.0 (animation and car-control sync, Steam names; PlayerState v4; core v0.9.0), released
  untested in game at the user's request. First tested 2026-10-17 by the user and a friend on two PCs: Steam invite
  between two accounts worked, Steam names shown, no crash; cars pinned, sprint lag, seat ejections (see A and B). Done 2026-10-17: https://github.com/JustAddMP/MafiaDE/releases/tag/v0.13.0.
- [x] GitHub pre-release v0.14.0 (fixes from that test: car engine, speed after corrections, pin retries, sprint
  catch-up, animated get-in; sync trace + tools/sync_compare.py; core v0.10.0), fixes not yet tested in game. Done
  2026-10-17: https://github.com/JustAddMP/MafiaDE/releases/tag/v0.14.0.
- [ ] GitHub pre-release v0.15.0 (melee actions, car signals, seat keeping, mission car back, guest-owned NPCs,
  traffic and police, doors, world items, traversal, explosions and the grenade arc, car damage, several guests,
  latency, failed missions in one load, players on the map, reconnect, JOIN FRIEND panel, physical pad and button
  prompts; core v0.11.0, protocol 9). Published 2026-10-18 as a pre-release after the first in-game test on one PC
  (verified: car signals, failed missions in one load, the police fix; the rest not yet verified):
  https://github.com/JustAddMP/MafiaDE/releases/tag/v0.15.0. Not ticked until the remaining checks of README-dev.md
  "Test checklist: 0.15.0 features" are done.

### E. Robustness (ongoing)
- [x] Clean shutdown: call Coop::Shutdown when the game window closes so guests see "host left" at once, not a 10 s timeout.
  Done 2026-10-15 (core v0.5.0, its docs/protocol.md "Leaving cleanly"): `ExitProcess` / `TerminateProcess(self)`
  hooks leave the session before the process ends. Checked in game: closing the host's window -> the guest's banner
  "HOST LEFT: Host left the game" 4 ms later; closing the guest's window -> "Guest left the game" on the host 3 ms
  later; STOP HOSTING -> HOST LEFT at once. A kill from outside (taskkill /F) times out after `net_timeout_s` (core v0.8.0: 4 s, was 10 s; a
  keepalive thread keeps a busy game's peers): guest killed -> host noticed after 4.6 s, host killed -> guest after
  4.7 s; no false disconnect in this session's loads, failures and reloads (the frame never stalled more than 0.2 s).
- [x] Overlay: release the backbuffer reference after each draw; log Present/ResizeBuffers thread ids in game to confirm assumptions.
  Done 2026-10-15 (core v0.5.0): a backbuffer view per frame, released after drawing; everything released on device
  removed/reset and at exit. In game: ResizeBuffers ran on the Present thread (logged) and succeeded right after the
  overlay had drawn; the exit release logged. Device loss itself was not provoked.
- [x] Steam join request: only record the lobby id in the callback and act after polling (no session reset mid-use).
- [x] Crash handler: don't let a first-chance stack overflow use up the one-shot dump flag.
- [x] Host relay rate limits for guest chat/app messages; evict oldest un-handshaken UDP connections when slots are full.
- [x] Launcher: require a value for keys like --password (reject "--password --x").
- [ ] **Reconnect after a lost connection** (0.15.0, core 0.11.0, core docs/protocol.md "Reconnect", protocol.md
  "Reconnect"): the host holds a lost guest's slot for 60 s (PeerDropped 0x0D to the others), the guest's game
  keeps running and rejoins the same host with backoff for 60 s and gets his slot back with the resume token from
  the Welcome; banners on both sides, the dropped player's puppet removed, the world re-sent keyed by (id,
  joinSeq), the guest keeps following the host's point (no reload unless the host moved), trace RCN; kProtocolVersion
  8 (9 since core 0.11.0's lanes and session clock); unit test net_reconnect. Gate: a guest whose network is cut for 10-30 s comes back as the same player without
  a reload, NPC copies and traffic return; also over Steam P2P. Built, needs the in-game test.
  In-game 2026-10-18 (one PC, UDP, guest process suspended): 15 s -> host `lost the connection (timed out); slot held 60 s`, banner `... WAITING FOR THE PLAYER TO RECONNECT... 55 S`, puppet removed; on resume `reconnected after 0.5 s: same slot, still player 1`, no reload, puppets back, trace RCN. 66 s -> host `did not reconnect within 60 s; slot freed`. Not yet: Steam P2P, LEAVE CO-OP during the retries, the guest-side banner (a suspended game draws nothing).
- [x] **Bandwidth and player desync** (user, 2026-10-16: "there was some desync with the players loaded"). Done
  2026-10-16 (core v0.6.0, docs/protocol.md "PlayerState", README-dev.md "Network statistics"): traffic statistics per
  peer and per message in the log every 5 s and on F7; limits raised (relay 1 MB/s per guest, interest 512 KB/s,
  `net_budget_kbps`), though a two-player session uses ~15 KB/s and nothing was ever dropped: the network was not the
  cause. Measured with `trace_pos` + tools/pos_error.py: the network target was within 0.1-0.5 m, but the puppet's AI
  walk trailed a running player by 2.5 m on average and up to 9.3 m. Now 60 Hz states, 50 ms interpolation plus a
  velocity lead, the puppet pulled toward the target each tick and re-aimed on sharp turns, NpcStates 10 Hz and size
  capped: on foot 0.3-0.5 m average (p95 <= 0.7 m) in Free Ride and Taxi, after a CHAPTER start and a checkpoint reload;
  a car copy at up to 20.6 m/s 0.12 m average. Over Steam P2P between two PCs not measured.
- Every pattern/Lua call fails soft (log + disable feature); keep it that way.
- Crash triage: `out\logs\crash-*.dmp` + log stack (module+offset) → symbolize with the PDBs in out\.
- Long-session test (1 h, two instances, driving + shooting) before each release.
