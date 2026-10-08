# Developer notes (moved to dev\; the user-facing README is G:\MafiaCoop\README.md)

# Mafia: Definitive Edition — co-op story mod (dev workspace)

Everything lives here, nothing in the game folder is modified.

```
G:\MafiaCoop\
  Framework\                     MafiaHub Framework (develop) — the multiplayer engine
    code\projects\MafiaMP\       MafiaMP (master) + local patches (NPC humans, heal fix, launcher, Lua escaping)
      resources\coop-story\      THE CO-OP STORY GAMEMODE (JS, hot-editable): 20 chapters
    builds\build-64\bin\         build output: MafiaMPLauncher.exe, MafiaMPClient.dll, MafiaMPServer.exe
  server\                        server run folder (server.json, logs, resources -> junctions into the repo)
  dist\MafiaMP-Coop-Client\      shareable client package (make_client_package.cmd)
  dev	ools\sim\                     offline campaign simulator: node play.js [--no-npc] [--chapter n] [--verbose]
  dev\build_all.cmd                  build server + client + launcher (Debug, x64), deploys the server exe
  dev\build_client.cmd               build client + launcher only
  dev\build_server.cmd               build the server and copy it into server\
  dev\run_server.cmd                 start the co-op server (UDP 27015); copies in the newest build first
  dev\run_client.cmd                 start the game through the MafiaMP launcher (free-ride co-op client)
  dev\run_story_host.cmd             start the game as STORY HOST (real campaign + world mirror, see below)
```

## Play

1. `run_server.cmd` — wait for `[COOP] resource started with 20 chapters, NPC API available`.
2. `run_client.cmd` on your PC; friends run `MafiaMPLauncher.exe` from a copy of `dist\MafiaMP-Coop-Client`
   (same Steam game version, Steam running, VC++ 2015-2022 x64 redistributable).
   In the MafiaMP menu connect to the host's IP, port 27015.
3. In game: `F4` panel, `F5` ready, `F6` start. See `Framework\code\projects\MafiaMP\resources\coop-story\README.md`.

Internet play: forward **UDP 27015** to the hosting PC. That is the only port; resources and voice travel over it.
TCP 27016 is the server's info page and is not needed by clients. Everyone's microphone is live on connect.

Edit mission files while the server runs, then type `ensure coop-story` in the
server console to reload them (clients get the new client script automatically).
The file watcher is off (`developmentMode = false`) so a stray save never restarts a running chapter.

## Story-host mode (the real campaign, friends injected) — EXPERIMENTAL

The gamemode has two modes. `campaign` is the scripted co-op recreation above. `story` is the
spike for what you actually asked for: the host plays the **genuine** single-player campaign (the
game's own mission scripts, cutscenes and NPCs alive inside the MafiaMP client) and the client
mirrors every human and car of the host's world to the server, so guests see them, drive with
you and can shoot the host's enemies. Guests run the normal client.

How to try it (host PC):

1. `run_server.cmd`. The gamemode starts in the mode set in
   `Framework\code\projects\MafiaMP\resources\coop-story\mode.json` (now `story`); `/mode campaign`
   switches back to the scripted chapters at runtime, `ensure coop-story` in the server console reloads.
2. `run_story_host.cmd` (sets `MAFIAMP_STORY_HOST=1`). The vanilla main menu appears instead of free ride.
3. Start New Game / pick a chapter in the vanilla menu. Once the chapter is loaded the client connects
   to `127.0.0.1 27015` by itself without taking your controls (`story_server.txt` next to the launcher
   overrides it: one line `<host> <port> [nickname]`; `F8` then `connect <host> <port>` is the fallback).
   Guests connect as usual and `/mirror` shows what is being streamed.
4. Chapter save paths look like `00_mission_replay/checkpoint_mm_100_farm_cp_010.sav` (chapter replay
   checkpoints; `mm_020_taxi` is chapter 1, `mm_210_gallery` chapter 20). Put one in `story_chapter.txt`
   next to `MafiaMPLauncher.exe` to skip the menu next time.

What to look for in `Framework\builds\build-64\bin\logs\MafiaMP.log`:
`[Story]` lines (mode on, `RunGame mission/part`, `OpenDebugLoadChapterString <path>`, stream-map
mission/part names) and `[Mirror]` lines (humans/cars adopted, caps, refused). Server log: `[Story]`
host election, `[Mirror]` entities created/destroyed.

Gates, in order. If one fails, stop and report the log:
1. The client stays stable with scripts alive through chapter 1's taxi ride (no crash, chapter
   objectives progress for the host).
2. The `[Story]` log shows the chapter save paths and mission names.
3. A guest sees the host's NPCs and cars (`/mirror` counts > 0 on the guest; NPCs move).
4. Enemies react to the guest and the guest's shots hurt them.

Known risks: story scripts only know about Tommy, so guests are passengers to the mission logic;
cutscene input locks may fight the mod's control lock; the mod's aim/fire hooks apply to story NPCs
too; mirrored humans all use Tommy's model until the spawn-profile hashes are mapped (the log prints
each ped's profile hash for that).

## ScriptHook

The game folder may contain the NOMAD ScriptHook (`dinput8.dll`, `ScriptHook\`). The MafiaMP launcher now
resolves `dinput8.dll` to the Windows system copy (`code\launcher\src\main.cpp`), so the ScriptHook does not
load into MafiaMP and nothing needs renaming. `scripthook_off.cmd` / `scripthook_on.cmd` remain as a fallback.

## Game version

MafiaMP's launcher verifies the executable CRC32 and expects 3168979183.
The installed `mafiadefinitiveedition.exe` (Steam build 16502642) matches.

## Testing without the game

`cd dev	ools\sim && node play.js` plays all 20 chapters with two fake players through the real server scripts
(every step type, deaths, late joiners, disconnects, abort/restart, hot reload) and `node play.js --no-npc`
does the same for a server build without the NPC API. Both must print `ALL CHECKS PASSED`.

First in-game checks after a build: `/npc 2` next to you (enemies should appear, run at you and shoot; kill
them and the server log shows `[COOP:NPC] ... died`), `/npc 1 ally` (follows you), `/heal` after taking
damage, and chapter 1. If enemies never appear, `/npcmodels tommy`. Logs: `server\logs\mafiamp_server.log`
and `Framework\builds\build-64\bin\logs\MafiaMP.log` (grep `[COOP]` and `[NPC]`).

## Local patches on top of upstream

MafiaMP master (3 Sep 2026) predates the Framework's vcpkg migration
(21 Sep 2026) and a few API changes. Patched in `code\projects\MafiaMP`:

- include paths: `fu2/function2.hpp` -> `function2/function2.hpp`, `cxxopts/cxxopts.hpp` -> `cxxopts.hpp`, `imgui/imgui.h` -> `imgui.h`
- `GameInput`: `MapKey` no longer overrides, `IsAvailable()` added, `<cstdint>` include
- `Application::OnChatMessageReceived` takes `RPC::ChatMessage` now
- `DrawNameTag` takes an `ImDrawList*` first
- `game/helpers/human.cpp` needed `<algorithm>`; `SetHealthPercent` now really takes a percent
- client no longer links `lua54_static` (removed from the Framework; the client only calls the game's own Lua through hooks)
- server JS classes (`Human`, `Player`, `Vehicle`) are per-isolate, because every resource runs in its own Node isolate
- **NPC humans**: `HumanEntity.isNpc` + replicated orders, framework delegation (nearest client simulates),
  `game/ai/npc_brain.cpp`, `World.createHuman`/`World.humans`, `Human.goTo/attack/follow/flee/destroy`,
  `kHumanNpcDeath` RPC, `humanDied`/`humanDestroyed` events, `Vehicle.destroy()`
- `Human.health = x` from JS now reaches the owning client (forced health state), so respawn heals work
- `game/helpers/ui.cpp` escapes strings before building game Lua (no injection through chat text)
- launcher: `dinput8.dll` always resolved from System32 (ScriptHook coexistence)
- server `developmentMode = false`

Patched in `Framework`:

- `scripting/builtins/builtins.{h,cpp}`: `AddUnregisterHook()` so a project can drop its per-isolate class wrappers when an isolate is disposed.

Client and server share the entity wire format: after any C++ change rebuild both and re-run
`make_client_package.cmd` so friends get the matching client.
