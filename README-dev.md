# MafiaCoop: developer notes

## Repo layout

This repo is the Mafia: Definitive Edition game layer. The game-agnostic part (launcher, proxy, hooks, networking,
overlay, co-op UI, SWF patcher) is [JustAddMP/core](https://github.com/JustAddMP/core), a git submodule at
`external\core` (its `docs\ARCHITECTURE.md` describes it and the game-module interface). After cloning:
`git submodule update --init` (or clone with `--recursive`).

- `src\game\game_info.cpp`: the descriptor core reads (names, Steam app id 1030840, exe name, supported builds,
  default port, proxy names); linked into all three binaries.
- `src\game\module.cpp`: the game module core calls (`Init`, render tick, session start/end, player left, menu
  state/commands).
- `src\game\lua.*`, `player_sync.*` (+ `player_sync.lua`), `menu.*`: the Lua bridge, player sync and the CO-OP
  menu entries.

Generic fixes go to core first (commit there, its tests pass) and are released as a core version tag; this repo pins
one tag (currently core v0.3.0) and moves with `tools\update_core.cmd vX.Y.Z`, which rebuilds and runs the tests
(docs/PLAN.md).

## Build

```
build.cmd
```

- Works from any directory. Needs Visual Studio 2022 or newer with the x64 C++ toolset (found through vswhere by
  `external\core\tools\msvc.cmd` if `cl.exe` is not already on PATH). No CMake or Ninja. Fails with "external\core
  is empty. Run: git submodule update --init" if the submodule is missing.
- Flags (from core's `tools\msvc.cmd`): C++20, `/MT` (static CRT), `/O2 /Zi` with full PDBs, `/W4 /permissive-`.
- It first runs `external\core\build.cmd out\obj\core` (core's libraries, shaders, MinHook), then compiles
  `src\game\*.cpp` and links. It runs nothing.
- Output goes to `out\` (gitignored):
  - `MafiaCoop.exe` (+ `MafiaCoop-launcher.pdb`): core's launcher objects + `game_info.obj`
  - `mafiacoop.dll` (+ `mafiacoop.pdb`): core's `dllmain.obj` + `mc_core.lib` + every `.cpp` in `src\game`.
    New files are picked up automatically.
  - Lua that runs inside the game (`src\game\*.lua`) is embedded into the dll: core's `tools\embed_lua.ps1` turns
    `player_sync.lua` into `out\obj\gen\player_sync_lua.h` (`kPlayerSyncLua`) on every build.
  - `mafiacoop_proxy.dll` (+ `mafiacoop_proxy.pdb`): core's version.dll proxy + `game_info.obj`; see "Start from
    Steam" below.
  - `proxy_test.exe`: core's `tests\proxy_test.cpp` built against our descriptor (tests our launcher and proxy
    without the game; see below).
  - Object files go to `out\obj\game\`, core's to `out\obj\core\`.
- Core's own tests (net_loopback, net_password, net_kick, net_interest, net_fuzz, proxy_test with a made-up game, ...):
  `external\core\build.cmd`, then the exes in `external\core\out\` (see core's README.md).
- Any compile or link error stops the build with exit code 1.

## Run

```
out\MafiaCoop.exe [--host] [--join <ip[:port]>] [--join-lobby <id>] [--name <name>] [--transport udp|steam]
                  [--port <n>] [--log <tag>] [--<key> [value]] [--game <path to exe>] [+connect_lobby <id>]
                  [--skip-version-check] [--check-only]
out\MafiaCoop.exe --install | --uninstall [--game <exe or folder>] [--yes]
```

1. The launcher finds the game in this order: `--game`, then the Steam registry (`HKCU\Software\Valve\Steam\SteamPath`,
   HKLM `InstallPath`), then every library in `steamapps\libraryfolders.vdf` that holds `appmanifest_1030840.acf`
   (using its `installdir`). Last fallback: a `game_path = <folder or exe>` line in `out\MafiaCoop.ini`.
2. A game that is already running does not stop it: a second instance is allowed (two games on one PC for
   testing). The game is started with `/multiinst` on its command line, which skips its leftover Mafia II
   launcher mutex (the exe has no working single-instance check, see `docs/re-notes.md` 3.2). `+word` arguments
   (e.g. `+connect_lobby <id>`) are appended to the game's command line.
3. **Game version check:** it computes the CRC32 of the exe (slicing-by-8, ~55 ms for the 111 MB exe when it is in
   the OS file cache) and compares it with `kBuilds` in `src\game\game_info.cpp` (today only Steam build
   16502642, CRC `BCE2C8EF` = 3168979183). The result is cached in `out\version-cache.txt` keyed by exe path, size
   and last-write time, so later launches take under 1 ms. Unknown build: a message box names the supported builds
   and the CRC found; **Yes** starts anyway (the dll then gets `MAFIACOOP_UNSUPPORTED_BUILD=<crc>` and shows an
   "Unsupported game version" banner), **No** quits with exit code 2. `--skip-version-check` or
   `skip_version_check = 1` in the ini skips the check. `--check-only` only does the check (never starts the game,
   no message boxes): it prints the result to the console / redirected stdout, logs to `logs\launcher-check.log` and
   exits with 0 = supported, 2 = unsupported, 1 = error. Example:
   `out\MafiaCoop.exe --check-only` or `out\MafiaCoop.exe --check-only --game D:\other\mafiadefinitiveedition.exe`.
   In the dll, a feature whose patterns are missing reports to core's `core/compat.h`; if the Lua bridge is missing, co-op
   is disabled (sessions refuse to start) and a banner says "Co-op disabled: unsupported game version".
4. It starts `mafiadefinitiveedition.exe` with `CREATE_SUSPENDED`. The working directory is the game folder, and
   `SteamAppId=1030840` and `SteamGameId=1030840` are added to the environment. With these set, steam_api does
   not restart the game through Steam (tested: the same PID keeps running). Steam must be running. Nothing is
   written to the game folder.
5. It injects `out\mafiacoop.dll`: `QueueUserAPC(LoadLibraryW)` on the suspended main thread, then resumes. The APC
   runs when the loader finishes process initialization, before the game's entry point. If the dll has not
   signalled `Local\MafiaCoopInjected-<pid>` within 15 s, the launcher falls back to `CreateRemoteThread(LoadLibraryW)`.
   Set `inject_method = thread` in the ini to force the fallback.
6. It watches the game for 20 s and then exits. Failures show a message box and are written to
   `out\logs\launcher.log`.

**Passing arguments to the dll:** each `--key value` becomes the environment variable `MAFIACOOP_KEY=value` in the
game process. A bare flag such as `--host` becomes `MAFIACOOP_HOST=1`. `mc::core::Config` checks `MAFIACOOP_<KEY>`
before `MafiaCoop.ini`, so `Config::Get().GetString("join")` sees `--join 1.2.3.4`. The launcher also sets
`MAFIACOOP_LAUNCHER=1` (tells an installed proxy, below, not to load a second copy).

## Start from Steam (optional proxy dll)

Opt-in, because it is the only feature that writes into the game folder. `MafiaCoop.exe --install` copies
`mafiacoop_proxy.dll` into the game folder as `version.dll` and writes `mafiacoop_path.txt` next to it (a `# MafiaCoop`
header line, then the full path of the `mafiacoop.dll` next to `MafiaCoop.exe`). After that the game started from
Steam (or its exe directly) loads co-op; `MafiaCoop.exe` keeps working as before.

- **Why version.dll:** the exe imports `VERSION.dll` statically (6 functions; checked with pefile: no delay-load,
  load-config `DependentLoadFlags` = 0) and version.dll is not in `KnownDLLs`, so the loader takes the copy in the
  exe's folder first. It has only 17 exports. The game folder has no version.dll of its own; ScriptHook uses the
  `dinput8.dll` slot, so the two do not collide.
- **Forwarding:** the proxy loads `<GetSystemDirectory>\version.dll` by full path in `DllMain` and fills a table of
  17 pointers; each export (core's `src\proxy\version.def`, same names and ordinals as Windows' file) is a one-instruction
  `jmp [g_real+i*8]` stub (core's `version_stubs.asm`), so every signature, including the undocumented
  `GetFileVersionInfoByHandle`, passes through untouched. A missing export fails with `ERROR_PROC_NOT_FOUND`.
- **Loading the mod:** only when the process is `mafiadefinitiveedition.exe` and `MAFIACOOP_LAUNCHER` is not set, the
  proxy queues an APC on the loading (main) thread. It runs when the loader finishes process initialization, before
  the game's entry point: the same moment as the launcher's APC injection. It loads the dll named in
  `mafiacoop_path.txt` (else `mafiacoop.dll` next to the proxy), skips it if a `mafiacoop.dll` is already loaded, and
  sets `MAFIACOOP_PROXY=<proxy path>`; the dll logs `loaded by the version.dll proxy ...` (or `loaded by
  MafiaCoop.exe`). Problems go to `OutputDebugString` (DebugView); the game then runs without co-op.
- **Settings:** without the launcher there are no `MAFIACOOP_*` variables, so everything comes from `MafiaCoop.ini` next
  to `mafiacoop.dll` (`core::Config` reads it in either mode; `name`, `password`, `window`, ...). Steam sets
  `SteamAppId` itself, and Steam's "Join Game" passes `+connect_lobby <id>` straight to the game. Not available in
  this mode: the launcher's exe CRC check (the dll's compat banners still report missing patterns) and the
  `--log <tag>` file name (always `logs\mafiacoop.log`).
- **`--install`:** finds the game like a normal launch (`--game <exe or folder>` overrides), then refuses (exit 3,
  nothing changed) if the folder has a `version.dll` without our marker string (`GameInfo::proxyMarker` in `src\game\game_info.cpp`, e.g.
  another mod's) or a `mafiacoop_path.txt` without our header, or if a game started from that folder is running.
  Otherwise it shows a Yes/No dialog naming both files, what they do and how to remove them, then writes them (our
  own older proxy is overwritten = update). **`--uninstall`** deletes exactly those two files, each only if it is
  ours, after a Yes/No dialog; anything else is reported as "left alone". `--yes` skips the dialogs and prints the
  result instead (scripts/tests). Exit codes: 0 done or nothing to do, 1 error, 3 refused or cancelled. Log:
  `logs\launcher-install.log`.
- **Test without the game:** `out\proxy_test.exe [work folder]` (default `%TEMP%\mafiacoop-proxy-test`): forwarding
  (17 exports, by name and ordinal, results equal to System32's) in a non-game process that must not load the mod; a
  copy of itself named `mafiadefinitiveedition.exe` next to the proxy, whose static version.dll import resolves to the
  proxy like the game's (mod loaded before `main` from the pointer path; not loaded with `MAFIACOOP_LAUNCHER=1`;
  same-folder fallback; nothing without either); `MafiaCoop.exe --install/--uninstall --yes` against a fake game
  folder (install, update, uninstall twice, foreign `version.dll` / pointer refused and untouched, other files
  untouched, folder without the exe). A copy of `msimg32.dll` stands in for `mafiacoop.dll`. Never point it at the
  real game folder.

## Co-op: host and join

All connection logic runs in the dll (`external/core/src/dll/coop.cpp`), driven by `Mod::Tick` once per frame. Players start
`MafiaCoop.exe` without arguments and use the game's own menus; the launcher arguments below are for testing and
scripts and still work.

### Player flow (the game's menus)

- **Main menu:** a **CO-OP** entry after FREE RIDE (it reads `CO-OP (HOSTING)` / `CO-OP (CONNECTED)` while a
  session runs). It opens a CO-OP page in the same style, with mouse, keyboard and gamepad like every other entry:
  - **HOST CO-OP GAME** starts hosting (UDP 27960 + a friends-only Steam lobby) and goes back to HOME, where the player
    starts the game the usual way (CONTINUE, STORY > chapter, ...) with hosting on.
  - **JOIN FRIEND** opens the Steam overlay's friends list: right-click a friend who hosts and pick *Join Game*, or
    accept their invite.
  - **JOIN BY IP** opens a text box (type `ip` or `ip:port`, Ctrl+V pastes; Enter connects, Esc cancels).
  - While hosting: **INVITE FRIENDS** (Steam invite dialog), **STOP HOSTING**; while joined: **LEAVE CO-OP**; **BACK**.
  - Idle or hosting: **SET PASSWORD** (a text box like JOIN BY IP, shown masked; **CHANGE PASSWORD** once set) and
    **CLEAR PASSWORD**. The password lives until the game exits (the ini/launcher one is the starting value). The
    pause panel shows a padlock next to its CO-OP title while a password is set. A join refused with "password
    required" / "wrong password" opens a PASSWORD box; Enter repeats the join with it.
- **Pause menu:** GAME page (Esc opens the pause menu on MAP; `=` / `-` switch pages, gamepad LB/RB) has a fifth entry
  **CO-OP**. It opens a panel over the pause menu with the players and their ping and the same actions (host from
  the middle of a game, invite friends, join friend / by IP, leave). Esc / B closes the panel only.
  While hosting, Up from the first entry selects a guest in the player list; Enter offers **KICK** / **KICK AND BAN**
  with a confirmation ("Kick <name>? Enter = yes, Esc = no"). The guest sees "You were kicked by the host" and goes
  back to idle; a banned guest's rejoin (same IP, or same SteamID over Steam) is refused with "You are banned from
  this session" until the host stops hosting (core Session::Kick / KickAndBan).
- **Steam invites:** accepting an invite (or *Join Game* in the friends list) at the main menu or in the world leaves
  the current session and joins the host; a banner says "Joining your friend's game..." and then
  "Connected to <host>'s game".
- **Banners** at the top of the screen tell about hosting, players joining/leaving, connects, disconnects and errors.

How the entries are added to the game's menus: docs/game-api.md 9 and docs/re-notes.md 5. If the game's menu files
ever change so that the patch does not apply, the menus load unchanged (logged as `menu: could not patch ...`) and
the launcher arguments still work.

### Launcher arguments

| Launcher arg | Meaning |
|---|---|
| `--host` | Host. UDP on `--port` (default 27960) **and**, when Steam is available, a friends-only Steam lobby (P2P). |
| `--join <ip[:port]>` | Join a host over UDP. |
| `--join-lobby <id>` | Join a Steam lobby (its owner is the host) over Steam P2P. `+connect_lobby <id>` on the game's command line does the same (Steam's "Join Game"). |
| `--transport udp\|steam` | Host only on that transport. With `steam`, `--join <id>` is read as a lobby id. |
| `--name <n>` | Player name. Default: Steam persona name, else the Windows user name. |
| `--password <p>` | Host: guests must answer with this password (docs/protocol.md "Session password"). Guest: the password to join with. Never logged (the launcher log shows `(hidden)`). |
| `--require-password-for-invites 1` | Host: Steam lobby members (invited friends) need the password too. Default: they skip it. |
| `--log <tag>` | Log to `logs\mafiacoop-<tag>.log` / `logs\launcher-<tag>.log`. |
| `--debug-overlay` | Show the debug status text (top-left) from the start; F7 toggles it. Pass it in test flows whose captures are read for the status lines. |
| `--window left\|right` | Keep the game window on the left/right half of its monitor's work area (full height, title bar kept), for watching two test instances side by side. Unset = the window is left alone. |

ini equivalents: `host = 1`, `join = ...`, `join_lobby = ...`, `transport = ...`, `name = ...`, `port = ...`,
`password = ...`, `require_password_for_invites = 0|1`,
`window = left|right`, `steam = 0` (no Steam at all), `debug_overlay = 1` (show the debug status text from the start; hidden by default), `native_menu = 0` (no CO-OP
entries in the game's menus), `menu_dump = 1` (write the original and patched menu movies to `out\dev\gfx\`),
`menu_debug = 1` (log every request from the patched menus).

**Invites:** while hosting, open the Steam overlay (Shift+Tab) and invite a friend to the lobby. A friend who
accepts while their game (started through MafiaCoop.exe) is running gets `GameLobbyJoinRequested`; the dll leaves
whatever it was doing, joins the lobby and connects to the lobby's host over Steam P2P. The host's Steam transport
only accepts P2P sessions from members of its lobby.

**Steam inside the game:** the game does not use the on-disk `steam_api64.dll`; it memory-loads an embedded 2015
steam_api (`docs/re-notes.md` 3.1), so there is no flat API in the process. Three seconds after the first frame
the dll `LoadLibrary`s the game folder's `steam_api64.dll` (read-only) itself, calls its `SteamAPI_Init` (a second
pipe to the same Steam client, the game's own Steam keeps working) and calls its `SteamAPI_RunCallbacks` every
frame. If that fails, Steam features are off and UDP still works.

**Disconnects:** no automatic reconnect. The reason is logged (`coop: disconnected: ...`), shown in a banner (and
for 20 s in the debug status text), and the mod goes back to idle.

**Debug status text:** hidden by default for players; **F7** shows/hides it, `--debug-overlay` / `debug_overlay = 1`
shows it from the start. Banners and the co-op panels are drawn either way. Top-left of the game window, drawn by the dll's own D3D11 renderer (`external/core/src/render/overlay.cpp`,
glyphs rasterised at startup from the Windows font Bahnschrift, `external/core/src/render/ui_font.cpp`; the same renderer draws
the co-op panels and banners): `Co-op: hosting (2 players)` or `Co-op: connected to Host - 8 ms`, then one line per player with their ping
to the host. F7 works only when a game window has focus. The same lines are written to the log with every
10 s heartbeat (`status: ...`).

### Two instances on one PC through the menus

```
out\MafiaCoop.exe --log host --debug-overlay
out\MafiaCoop.exe --log guest --debug-overlay      (after the first window is up)
```

Host window: Enter at the title, CO-OP > HOST CO-OP GAME. Guest window: Enter at the title, CO-OP > JOIN BY IP,
type `127.0.0.1`, Enter. The guest's HOME then shows `CO-OP (CONNECTED)`; both pick CONTINUE (and Enter on the
loading screen). In the world, Esc, `=` three times (GAME page), CO-OP shows the players with ping; STOP HOSTING /
LEAVE CO-OP ends it (the other side gets a "Disconnected" banner).

### Two instances on one PC (gate test, launcher arguments)

```
out\MafiaCoop.exe --host --name Host --log host --window left --debug-overlay
out\MafiaCoop.exe --join 127.0.0.1 --name Guest --log guest --window right --debug-overlay      (after the first window is up)
```

`--window left|right` puts the two windows side by side (each on half of the monitor's work area; the dll moves the
window as soon as it exists and puts it back whenever the game resets its size, checked twice a second; the swap chain
follows through the ResizeBuffers hook). Both windows open; the games sit at the title screen. Check `out\logs\mafiacoop-host.log` for
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
Puppets only appear once the local player is in a loaded world (not in the main menu).

Cars: when a player drives, the others see the same car (their own copy of it) following the driver, with the
driver's puppet in the driver seat. **E** (pad **Y**) next to a car another player drives puts you in its first free
passenger seat ("ride along"); the others see your puppet sitting there. When the driver gets out, the car stays
where it was left on every game, and anyone can get in and drive it (owner = last driver). Details and message flow:
docs/protocol.md "Shared cars, seats and ownership". Puppets are removed when their player leaves and all at once when
the session ends.

On foot, puppets also crouch, aim (right mouse button / pad left trigger with a gun in hand, in the camera's
direction) and fire a real shot (flash and sound, no damage yet) for every shot the player fires, and play a reload.
Health is synced: shown on the debug status line (verified) and in the pause menu's CO-OP panel (`HEALTH n` next to each
player; not checked in a capture yet).
The puppet factory is prepared as soon as the local player is in the world with a session running, and the model of
a player who is still loading is loaded ahead, so his puppet appears with his first state.

**Weather and time of day:** the host's game is the clock. The host sends its time of day, time flow speed and weather
set (WorldEnv, docs/protocol.md) every 4 s and at once on a change; guests apply them.

The debug status text (`--debug-overlay` or F7) shows `Sync: in world, puppet factory ready, 1 shared car(s)` and one line per remote player
(`Guest: puppet alive:0.2, 20 Hz, last 0.0 s ago, health 100, crouch, aim` = state, metres off the target, receive
rate, age of the newest snapshot, his health and stance; `alive/seat0` = sitting in seat 0 of a car,
`alive/veh-hidden` = in a car but not seated). The log has `sync:` lines (C++) and `lua: sync:` lines (game side).

**background_run** (default on): the game pauses its simulation when its window loses focus (`WM_ACTIVATEAPP`). The
dll swallows that message so an unfocused game keeps running and sending (needed for co-op and for two instances on
one PC). Set `background_run = 0` to get the original pause-on-alt-tab.

### In-world test with two instances on one PC

1. Back up the save folder first (`%USERPROFILE%\Documents\My Games\Mafia Definitive Edition`, OneDrive-redirected
   on some PCs) and never pick "New Game".
2. `out\MafiaCoop.exe --host --name Host --log host --window left --debug-overlay`; at the title press Enter, then Enter on **Continue**, then Enter
   when the loading screen says "Continue".
3. `out\MafiaCoop.exe --join 127.0.0.1 --name Guest --log guest --window right --debug-overlay`, same three Enters.
4. Both stand at the save's spawn point; each sees the other's puppet next to them. Move one player and watch the
   other window (`tools\devlua.ps1` can teleport/step the player: see the test snippets below).
5. `taskkill /IM mafiadefinitiveedition.exe /F`.

Test snippets (run with `tools\devlua.ps1 -Tag host -Code '...'`):

```lua
-- walk the local player 12 m along +x in 4 s (the remote puppet runs after it)
local p = game.game:GetActivePlayer(); local p0 = p:GetPos(); local t0 = MafiaCoop.now
StartThread(function() while true do Sleep(0); local t = math.min(MafiaCoop.now - t0, 4)
  p:SetPos(Math:newVector(p0.x + 3 * t, p0.y, p0.z)); p:SetDir(Math:newVector(1, 0, 0)); if t >= 4 then break end end end)
-- spawn a car next to the player and get in as driver (the others see a copy with your puppet at the wheel)
local p = game.game:GetActivePlayer(); local pos, d = p:GetPos(), p:GetDir(); local so, id = game.traffic:ObtainSpecificCar("smith_v12")
StartThread(function() Wait(so); local s = game.traffic:SpawnCar(id, Math:newVector(pos.x + d.y * 3, pos.y - d.x * 3, pos.z + 0.3), d)
  Wait(s); local car = game.traffic:GetSpawnedEntity(id); Sleep(0.5)
  p:LockControls(true); local e = p:GetOnVehicle(car, 1, false, "WALK"); Wait(e); p:LockControls(false) end)
-- shared cars on this machine
local r = {} for k, c in pairs(MafiaCoop.cars) do r[#r+1] = k .. " " .. c.st .. " driver=" .. tostring(c.driver) end return table.concat(r, "
")
-- weather/time on the host (guests follow within 4 s)
game.gfx:SetWeatherSet("_default_game_rainy", 0, 0); game.gfx:SetTime(21, 0)
-- aim (instead of holding the right mouse button), fire once (about 1 s later), crouch
MafiaCoop.testAim = true   -- or {x, y, z}: a fixed aim direction; nil = off
local p = game.game:GetActivePlayer(); p:ShootWeapon(true)
local p = game.game:GetActivePlayer(); p:SetStalkMove(true)
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
skip_version_check = 0     ; 1 = start even if the game exe's CRC is not a supported build (launcher)
window_timeout_s = 300     ; how long the dll waits for the game window
crash_test = 0             ; 1 = raise an access violation in Tick after crash_test_delay seconds (debug only)
crash_test_delay = 20
background_run = 1         ; 0 = the game pauses when its window loses focus (original behaviour)
puppet_model = visuals/hero/game_main_tommy_casual   ; spawn profile for every puppet on this machine (default: by player id)
window = left              ; left|right: keep the game window on that half of its monitor (test instances side by side)
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
- **Crash triage:** `python external\core\tools\crashinfo.py` (from the repo root) (Python 3.11, standard library + ctypes, dbghelp.dll from Windows)
  reads the newest `out\logs\crash-*.dmp` (or the dump given as argument) and prints the exception (code, address,
  read/write/execute address), the faulting module+offset, the registers, the faulting thread's stack and the crash
  block of the log that mentions the dump (`--log <file>` to pick one). The stack is unwound with the x64 unwind data
  (`.pdata`) of the module files on disk (the game exe from the game folder, Windows DLLs, our dll), so game frames
  come out as `mafiadefinitiveedition.exe+0x...`; where the unwind stops (no matching image) the rest of the stack is
  scanned for return addresses that follow a call instruction, marked `[scanned]`. A call through a null/garbage
  pointer is unwound from `[rsp]`. Frames in our modules get function, inlined functions and `file:line` from the
  PDBs in `out\` (dbghelp `SymLoadModuleEx`/`SymFromAddr`/`SymGetLineFromAddr64`).
  ```
  python external\core\tools\crashinfo.py                                   newest dump
  python external\core\tools\crashinfo.py out\logs\crash-20261013-171825.dmp --log out\logs\mafiacoop.log.1
  python external\core\tools\crashinfo.py <dump> --symbols D:\old-build\out   PDBs of the build that crashed
  python external\core\tools\crashinfo.py <dump> --all-threads --modules      every thread's stack, the module list
  ```
  A PDB is used only when the dump's module has the same CodeView GUID and age as the dll next to it; every rebuild
  changes the age, so **keep the `out\` folder (dll + pdb) of a build whose dumps you may need to read**, or rebuild the
  same commit and pass `--force-symbols` (names are then marked "unverified"; correct only if the code layout is the
  same). Example: rebuilding commit 93134d5 into a temp folder and running
  `crashinfo.py out\logs\crash-20261013-171825.dmp --symbols <that out> --force-symbols` names the crash_test fault
  `mc::Mod::Tick+0x167 src\dll\mod.cpp:65` with `OnFrame` / `PresentDetour` above it, matching the log's stack.
- The game installs its own unhandled-exception filter after we do (a second or two after start). Ours is put back
  every frame for the first minute and then with every heartbeat, chaining to the game's.

## Stop the game

```
taskkill /IM mafiadefinitiveedition.exe /F
```
