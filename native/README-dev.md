# MafiaCoop native: developer notes

## Build

```
G:\MafiaCoop\native\build.cmd
```

- Works from any directory. Needs Visual Studio 2022 or newer with the x64 C++ toolset. The script finds
  `vcvars64.bat` through vswhere if `cl.exe` is not already on PATH. It does not use CMake or Ninja.
- Flags: C++20, `/MT` (static CRT), `/O2 /Zi` with full PDBs, `/W4 /permissive-`. MinHook is compiled as C at `/W3`.
- Shaders: every `srcender\*.hlsl` is compiled with `fxc.exe` (Windows SDK, on PATH after vcvars) into
  `out\obj\gen\<name>_vs.h` / `<name>_ps.h` (`vs_4_0`/`ps_4_0`, arrays `k<Name>VS` / `k<Name>PS`). Precompiling
  means the dll needs no `d3dcompiler_47.dll` at runtime and shader errors fail the build, not the game.
- Overlay font: `srcenderont8x16.h` is generated once and checked in (8x16 cells, 8-bit coverage, ASCII
  32..126 + a solid cell). To regenerate: build and run `toolsontgen.cpp` (command in its header comment).
- Output goes to `out\` (gitignored):
  - `MafiaCoop.exe` (+ `MafiaCoop-launcher.pdb`): the launcher, built from `src\launcher\*.cpp`
  - `mafiacoop.dll` (+ `mafiacoop.pdb`): every `.cpp` in `src\core`, `src\render`, `src\game`, `src\net` and
    `src\dll`, plus MinHook. New files are picked up automatically.
  - Lua that runs inside the game (`src\game\*.lua`) is embedded into the dll: `tools\embed_lua.ps1` turns
    `player_sync.lua` into `out\obj\gen\player_sync_lua.h` (`kPlayerSyncLua`) on every build.
  - `<name>.exe` for each `tests\<name>.cpp`, linked with `src\core` + `src\net` (+ MinHook). Skipped if there
    are none.
  - Object files go to `out\obj\<group>\`.
- Any compile or link error stops the build with exit code 1.

## Run

```
out\MafiaCoop.exe [--host] [--join <ip[:port]>] [--join-lobby <id>] [--name <name>] [--transport udp|steam]
                  [--port <n>] [--log <tag>] [--<key> [value]] [--game <path to exe>] [+connect_lobby <id>]
```

1. The launcher finds the game in this order: `--game`, then the Steam registry (`HKCU\Software\Valve\Steam\SteamPath`,
   HKLM `InstallPath`), then every library in `steamapps\libraryfolders.vdf` that holds `appmanifest_1030840.acf`
   (using its `installdir`). Last fallback: a `game_path = <folder or exe>` line in `out\MafiaCoop.ini`.
2. A game that is already running does not stop it: a second instance is allowed (two games on one PC for
   testing). The game is started with `/multiinst` on its command line, which skips its leftover Mafia II
   launcher mutex (the exe has no working single-instance check, see `docs/re-notes.md` 3.2). `+word` arguments
   (e.g. `+connect_lobby <id>`) are appended to the game's command line.
3. It starts `mafiadefinitiveedition.exe` with `CREATE_SUSPENDED`. The working directory is the game folder, and
   `SteamAppId=1030840` and `SteamGameId=1030840` are added to the environment. With these set, steam_api does
   not restart the game through Steam (tested: the same PID keeps running). Steam must be running. Nothing is
   written to the game folder.
4. It injects `out\mafiacoop.dll`: `QueueUserAPC(LoadLibraryW)` on the suspended main thread, then resumes. The APC
   runs when the loader finishes process initialization, before the game's entry point. If the dll has not
   signalled `Local\MafiaCoopInjected-<pid>` within 15 s, the launcher falls back to `CreateRemoteThread(LoadLibraryW)`.
   Set `inject_method = thread` in the ini to force the fallback.
5. It watches the game for 20 s and then exits. Failures show a message box and are written to
   `out\logs\launcher.log`.

**Passing arguments to the dll:** each `--key value` becomes the environment variable `MAFIACOOP_KEY=value` in the
game process. A bare flag such as `--host` becomes `MAFIACOOP_HOST=1`. `mc::core::Config` checks `MAFIACOOP_<KEY>`
before `MafiaCoop.ini`, so `Config::Get().GetString("join")` sees `--join 1.2.3.4`.

## Co-op: host and join

All connection logic runs in the dll (`src/dll/coop.cpp`), driven by `Mod::Tick` once per frame.

| Launcher arg | Meaning |
|---|---|
| `--host` | Host. UDP on `--port` (default 27960) **and**, when Steam is available, a friends-only Steam lobby (P2P). |
| `--join <ip[:port]>` | Join a host over UDP. |
| `--join-lobby <id>` | Join a Steam lobby (its owner is the host) over Steam P2P. `+connect_lobby <id>` on the game's command line does the same (Steam's "Join Game"). |
| `--transport udp\|steam` | Host only on that transport. With `steam`, `--join <id>` is read as a lobby id. |
| `--name <n>` | Player name. Default: Steam persona name, else the Windows user name. |
| `--log <tag>` | Log to `logs\mafiacoop-<tag>.log` / `logs\launcher-<tag>.log`. |

ini equivalents: `host = 1`, `join = ...`, `join_lobby = ...`, `transport = ...`, `name = ...`, `port = ...`,
`steam = 0` (no Steam at all), `overlay = 0` (start with the overlay hidden).

**Invites:** while hosting, open the Steam overlay (Shift+Tab) and invite a friend to the lobby. A friend who
accepts while their game (started through MafiaCoop.exe) is running gets `GameLobbyJoinRequested`; the dll leaves
whatever it was doing, joins the lobby and connects to the lobby's host over Steam P2P. The host's Steam transport
only accepts P2P sessions from members of its lobby.

**Steam inside the game:** the game does not use the on-disk `steam_api64.dll`; it memory-loads an embedded 2015
steam_api (`docs/re-notes.md` 3.1), so there is no flat API in the process. Three seconds after the first frame
the dll `LoadLibrary`s the game folder's `steam_api64.dll` (read-only) itself, calls its `SteamAPI_Init` (a second
pipe to the same Steam client, the game's own Steam keeps working) and calls its `SteamAPI_RunCallbacks` every
frame. If that fails, Steam features are off and UDP still works.

**Disconnects:** no automatic reconnect. The reason is logged (`coop: disconnected: ...`), shown on the overlay
for 20 s, and the mod goes back to idle.

**Status overlay:** top-left of the game window, drawn by the dll's own D3D11 text renderer
(`src/render/overlay.cpp`): `Co-op: hosting (2 players)` or `Co-op: connected to Host - 8 ms`, then one line per player with their ping
to the host. **F7** toggles it (only when a game window has focus). The same lines are written to the log with every
10 s heartbeat (`status: ...`).

### Two instances on one PC (gate test)

```
out\MafiaCoop.exe --host --name Host --log host
out\MafiaCoop.exe --join 127.0.0.1 --name Guest --log guest      (after the first window is up)
```

Both windows open; the games sit at the title screen. Check `out\logs\mafiacoop-host.log` for
`coop: player 1 'Guest' joined` and `out\logs\mafiacoop-guest.log` for `coop: handshake complete`, and the
`status:` lines for pings. Both instances share one SteamID, so they can only connect over UDP; Steam P2P needs
two accounts on two PCs. Stop with `taskkill /IM mafiadefinitiveedition.exe /F`.

## Lua bridge and dev console

`src/game/lua.cpp` hooks `C_ScriptMachine::Tick`/`TickPaused` (pattern scan; docs/re-notes.md 1.5) and, after the
game's own scripts, runs queued jobs and the per-tick callbacks on **GameMainThread** against the game's main
`lua_State` (`luaL_loadbuffer` + `lua_pcall`, stack top restored after every job). Scripts tick from the main menu on
(not on the title screen, not while the game is paused). Missing patterns disable the bridge (logged), an access
violation inside a job disables it for the rest of the session instead of crashing.

Globals the dll adds: `mc_log(...)` (writes to our log), `mc_u64(str|num)` / `mc_u64str(v)` (HKS ui64 values).

Dev console, to explore the API live:

- **F8** (game window focused) runs `out\dev.lua`; the chunk's first return value is logged and written to
  `out\dev\<tag>.out.txt`.
- **File drop:** write `out\dev\<tag>.in.lua` (`<tag>` = the `--log` tag, `default` without one); the dll picks it up
  within 0.5 s, deletes it, runs it and writes `out\dev\<tag>.out.txt` (`ok` or `error`, then the result).
  `tools\devlua.ps1 -Tag host -Code 'return game.game:GetActivePlayer():GetPos()'` does the round trip.
- What was found this way is in `docs/game-api.md`.

## Player sync (phase 3)

Each game samples its player on GameMainThread, sends `PlayerState` (app id 0x0100, docs/protocol.md) at 20 Hz and
shows every other player as a **puppet**: an AI human with its own outfit (host = trench coat, guest 1 = suit and
hat, ...) that walks/runs/sprints with the game's own locomotion (`MoveVec`) toward the interpolated position (100 ms
buffer), snaps when more than 10 m off, turns to the reported heading when standing and holds the reported weapon.
When a player drives, the others see a copy of the car following the driver's transform (the puppet is hidden: the
game does not seat our NPCs). Puppets are removed when their player leaves and all at once when the session ends.

The overlay shows `Sync: in world, puppet factory ready` and one line per remote player
(`Guest: puppet alive:0.2, 20 Hz, last 0.0 s ago` = state, metres off the target, receive rate, age of the newest
snapshot). The log has `sync:` lines (C++) and `lua: sync:` lines (game side).

**background_run** (default on): the game pauses its simulation when its window loses focus (`WM_ACTIVATEAPP`). The
dll swallows that message so an unfocused game keeps running and sending (needed for co-op and for two instances on
one PC). Set `background_run = 0` to get the original pause-on-alt-tab.

### In-world test with two instances on one PC

1. Back up the save folder first (`%USERPROFILE%\Documents\My Games\Mafia Definitive Edition`, OneDrive-redirected
   on some PCs) and never pick "New Game".
2. `out\MafiaCoop.exe --host --name Host --log host`; at the title press Enter, then Enter on **Continue**, then Enter
   when the loading screen says "Continue".
3. `out\MafiaCoop.exe --join 127.0.0.1 --name Guest --log guest`, same three Enters.
4. Both stand at the save's spawn point; each sees the other's puppet next to them. Move one player and watch the
   other window (`tools\devlua.ps1` can teleport/step the player: see the test snippets below).
5. `taskkill /IM mafiadefinitiveedition.exe /F`.

Test snippets (run with `tools\devlua.ps1 -Tag host -Code '...'`):

```lua
-- walk the local player 12 m along +x in 4 s (the remote puppet runs after it)
local p = game.game:GetActivePlayer(); local p0 = p:GetPos(); local t0 = MafiaCoop.now
StartThread(function() while true do Sleep(0); local t = math.min(MafiaCoop.now - t0, 4)
  p:SetPos(Math:newVector(p0.x + 3 * t, p0.y, p0.z)); p:SetDir(Math:newVector(1, 0, 0)); if t >= 4 then break end end end)
-- get into a car (the remote side spawns a copy that follows it)
carpls("smith_v12")
-- give the player a pistol (the puppet draws one too)
local p = game.game:GetActivePlayer(); p:InventoryAddWeapon("p_mast_a_v1", 50); p:InventorySelect("p_mast_a_v1", true)
-- puppet state on this machine
local r = {} for id, pp in pairs(MafiaCoop.pup) do r[#r+1] = id .. " " .. pp.st .. " " .. tostring(pp.dist) end return table.concat(r, "\n")
```

`out\player_sync.lua` (a copy of `src\game\player_sync.lua`), if present, is loaded instead of the embedded glue: edit
it, restart the game, no rebuild needed.

## MafiaCoop.ini (optional, next to the exe/dll)

```
game_path = G:\SteamLibrary\steamapps\common\Mafia Definitive Edition
inject_method = apc        ; or thread
window_timeout_s = 300     ; how long the dll waits for the game window
crash_test = 0             ; 1 = raise an access violation in Tick after crash_test_delay seconds (debug only)
crash_test_delay = 20
background_run = 1         ; 0 = the game pauses when its window loses focus (original behaviour)
puppet_model = visuals/hero/game_main_tommy_casual   ; spawn profile for every puppet on this machine (default: by player id)
```

## Logs and crashes

- `out\logs\mafiacoop.log` (or `mafiacoop-<tag>.log` with `--log <tag>`; a second concurrent instance without
  `--log` writes `mafiacoop-<pid>.log`): one line per event (timestamp, thread id, level), written to the file straight away.
  The previous run is kept as `mafiacoop.log.1`. A heartbeat with FPS is logged every 10 s from the Present hook.
- `out\logs\launcher.log` (`launcher-<tag>.log`, or `launcher-<pid>.log` when a game was already running): what
  the launcher found and did.
- `out\logs\crash-<date>-<time>.dmp`: a minidump written on a fatal exception. The log gets the exception code,
  the faulting module+offset, the registers, and an unwound stack as module+offset. Open the dump in Visual Studio
  or WinDbg with `out\*.pdb` on the symbol path.
- The game installs its own unhandled-exception filter after we do. The heartbeat puts ours back and chains to
  the game's.

## Stop the game

```
taskkill /IM mafiadefinitiveedition.exe /F
```
