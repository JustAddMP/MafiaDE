# MafiaCoop — architecture (Mafia DE game layer)

A from-scratch, self-contained co-op mod for Mafia: Definitive Edition (Steam build, exe CRC32 3168979183).
All code is our own. Goal: co-op that feels like a built-in game feature.

The game-agnostic part — launcher, version.dll proxy, crash handler, hooks, Present hook and overlay, UDP/Steam
networking and sessions, the co-op panels and banners, the SWF/AVM2 patcher — is JustAddMP core, a git submodule at
`external/core`. Its `docs/ARCHITECTURE.md` describes it, its runtime model and the game-module interface this repo
implements; its `docs/protocol.md` the session and transports. This file covers what is specific to Mafia DE.

## Clean-room rule (binding)

Do NOT copy or adapt code from other multiplayer mods. Allowed sources: the game's own binaries and data, the ScriptHook (MPlus) Lua scripts in the
game folder as API reference, public documentation (Microsoft, Steamworks, Lua 5.x manual), and code we write here.
Every game address/struct offset is derived by us from `mafiadefinitiveedition.exe` and recorded in `docs/re-notes.md`
with how it was found.

## Layout

```
build.cmd                builds core (external\core\build.cmd out\obj\core) + src\game, links into out\
external/core/           JustAddMP core (submodule)
src/game/
  game_info.cpp          GetGameInfo(): MafiaCoop names, Steam app 1030840, mafiadefinitiveedition.exe, /multiinst,
                         supported builds (CRC32), default port 27960, proxy names/marker
  module.cpp             GetGameModule(): wires the pieces below to core's calls (Init, render tick, session
                         start/end/tick, player left, health/status, menu state/commands, lobby modes and start,
                         CO-OP SETTINGS rows, name tags via DrawHud); reads the enter-vehicle and aim controls
  lua.{h,cpp}            bridge into the game's Havok Script VM (C_ScriptMachine::Tick hook), dev console (F8)
  characters.{h,cpp}     the character list (aliases -> spawn profiles), defaults by player id, fallbacks
  player_sync.{h,cpp}    phase 3: local player sampling, puppets, copy cars, weather/time (C++ side; wire format in
  player_sync.lua        docs/protocol.md); the .lua is embedded at build time. Also the CHARACTER/OUTFIT
                         rows of the CO-OP panel (module.cpp, GameModule::PanelOptions) and PlayerModel
  mission_sync.{h,cpp}   phase 4 story: the host's chapter/checkpoint/objective/cutscene and checkpoint save to the
  mission_sync.lua       guests, guests load and follow it (lobby STORY / CHAPTER); the chapter loader hooks;
                         deaths / failed missions in chapter loads reload the checkpoint
  npc_sync.{h,cpp,lua}   phase 4 NPCs: host-authoritative mission NPCs (states, deaths), guests pair them with
                         their own copies, guests' hits replayed on the host from their puppets
  save_guard.{h,cpp}     save system: logs loads, the chapter loader's one-entry storage fix (reload crash), a
                         following guest's (and a CHAPTER host's) save files redirected to %TEMP% (their slots
                         stay untouched)
  menu.{h,cpp}           phase 5: CO-OP entries in the game's Scaleform main/pause menus, using core's swf_patch;
                         the main menu's per-frame poll (live refresh, the lobby's Free Ride start)
docs/                    ARCHITECTURE.md (this), PLAN.md, protocol.md (our app messages), re-notes.md, game-api.md,
                         research-*.md, re-tools/, screenshots/
tools/                   devlua.ps1 (run Lua in a running game), make_release.cmd + release-readme.txt
```

Game code lives in `mc::game`.

## Runtime model (game side)

- Core's init thread calls `GameModule::Init`: the Lua bridge hooks the script tick (and player sync registers its
  main-thread tick on it), the menu patcher hooks the movie loader. Missing patterns report to core's compat module:
  the Lua bridge is required (co-op disabled without it), the menu entries are optional.
- Two threads (confirmed at runtime, re-notes 1.5):
  - **Render thread:** core's Present hook calls `Mod::Tick(dt)`: `RenderTick` (Lua results, dev console), the
    session, and `SessionTick` (sending the 60 Hz player state, receiving snapshots).
  - **GameMainThread:** the `C_ScriptMachine::Tick` hook drains the Lua bridge's job queue and runs the per-tick
    callbacks (player sync: sample the local player, drive puppets and copy cars). **Every game/Lua call happens
    here.**
  - Hand-over: one mutex per subsystem (bridge job/result queues; player sync snapshots + local sample; menu state
    and commands), held only for copies.
- The game pauses its simulation on `WM_ACTIVATEAPP(FALSE)` (window procedure exe+0x5DF0D0); core's background_run
  swallows it so an unfocused game keeps running.
- Logs: `<dll dir>\logs\mafiacoop.log`; crash dumps `<dll dir>\logs\crash-<time>.dmp`.

## Phases and gates

1. Skeleton — injected dll logs, Present hook ticks, crash handler writes a dump. Gate: game runs normally for 10 min.
2. Connection — Steam lobby + invite + direct-IP fallback, handshake, ping display. Gate: two instances/PCs connect.
3. Second player — remote player ped spawned and synced (position, heading, anim state, weapon, vehicle seat).
4. Shared world — host's NPCs/cars/weather/time/mission state mirrored to guests.
5. Native UI — co-op entries in the game's menus (main menu CO-OP page, pause menu CO-OP), invites, notifications
   (done: entries patched into the game's own AS3 menus; the pause panel, IP box and banners are core's overlay in the
   menu style because the game's HUD banners show nothing from script, docs/game-api.md 9).
6. Melee/damage sync, guest-side objectives, NPC appearance.
