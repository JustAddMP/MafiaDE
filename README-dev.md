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
one tag (currently core v0.7.0) and moves with `tools\update_core.cmd vX.Y.Z`, which rebuilds and runs the tests
(docs/PLAN.md).

## Build

```
build.cmd
tools\make_release.cmd 0.8.0      build, run every core test and proxy_test, write out\release\MafiaCoop-0.8.0.zip
                                 (player files, tools\release-readme.txt as README.txt, MinHook licence) and
                                 MafiaCoop-0.8.0-symbols.zip (PDBs); the version must equal modVersion
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
  - Our own unit tests of game logic that runs without the game (`tests\*.cpp`, each built into `out\<name>.exe`
    and run by `tools\make_release.cmd`; PASS/FAIL per check, exit code 0 when all pass), for example
    `car_swap_test.exe` (when a mission car hidden behind a copy comes back, `src\game\car_swap.*`) and
    `fail_flow_test.exe` (the co-op answer to a failed mission, `src\game\fail_flow.h`: one load on every game).
  - Object files go to `out\obj\game\`, core's to `out\obj\core\`.
- Core's own tests (net_loopback, net_password, net_kick, net_reconnect, net_interest, net_fuzz, proxy_test with a made-up game, ...):
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
  - **HOST CO-OP GAME** starts hosting (UDP 27960 + a friends-only Steam lobby) and opens the **LOBBY** over the menu
    (below). BACK there returns to the CO-OP page with hosting on, so the old way still works too: start the game
    yourself (CONTINUE, STORY > chapter, ...) and the others drop in.
  - **LOBBY (n PLAYERS)** (while hosting or connected) opens the lobby again; **SETTINGS** opens CO-OP SETTINGS.
  - The page updates by itself when the co-op state changes (a join completes, players come and go): the patched
    menu polls the dll every frame (docs/game-api.md 13).
  - **JOIN FRIEND** opens a list of your Steam friends who are playing Mafia DE (core `net/steam_friends.h`, core
    docs/protocol.md "JOIN FRIEND"): each with what they do, e.g. `HOSTING (1/4) - IN THE LOBBY`, `HOSTING (2/4) -
    PLAYING FREE RIDE`, `IN MILES'S CO-OP GAME (2/4)` (a guest: you join that host), `PLAYING, NOT HOSTING`,
    `VERSION 0.13.0 - YOU HAVE 0.14.0`, `FULL (4/4)`, `CHECKING THEIR GAME...`. Joinable friends come first; Enter /
    A / click joins (the same Steam lobby path as an invite), on the others a banner says why not and what to do.
    REFRESH asks Steam again; STEAM FRIENDS LIST opens the overlay (right-click a host, *Join Game*) as before; BACK
    returns to the CO-OP page. With no friends online / none in the game / no Steam the panel says so. The lobby the
    friend is in comes from Steam's "game played" info, else from their rich presence `connect` (set on hosts and
    guests); the version check uses the lobby data, so a mismatch is shown before anyone connects (and an invite
    from another version stops with the same message).
  - **JOIN BY IP** opens a text box (type `ip` or `ip:port`, Ctrl+V pastes; Enter connects, Esc cancels).
  - While hosting: **INVITE FRIENDS** (Steam invite dialog), **STOP HOSTING**; while joined: **LEAVE CO-OP**; **BACK**.
  - Idle or hosting: **SET PASSWORD** (a text box like JOIN BY IP, shown masked; **CHANGE PASSWORD** once set) and
    **CLEAR PASSWORD**. The password lives until the game exits (the ini/launcher one is the starting value). The
    pause panel shows a padlock next to its CO-OP title while a password is set. A join refused with "password
    required" / "wrong password" opens a PASSWORD box; Enter repeats the join with it.
- **Lobby** (core's panel over the main menu): the players with their character (CHARACTER column) and status
  (HOST / READY / NOT READY), your CHARACTER and OUTFIT rows, then for a guest **READY** (Left/Right/Enter toggle) and
  for the host **MODE** (FREE RIDE, STORY, CHAPTER 1..20) and **START** (waits until every guest is
  ready), INVITE FRIENDS, SETTINGS, LEAVE CO-OP / STOP HOSTING, BACK. A guest who joins (JOIN BY IP, JOIN FRIEND, a
  Steam invite, `--join`) gets the lobby by itself as soon as he is at the main menu (not over the title screen).
  START closes everybody's lobby and every game loads the mode (each player presses Enter on his loading screen):
  FREE RIDE the way the menu's own FREE RIDE does; STORY: the host CONTINUEs his save and every guest loads the host's
  newest checkpoint save, sent over the network; CHAPTER n: everyone loads the shipped start of that chapter. If the
  host is already playing, the lobby says "HOST IS PLAYING FREE RIDE / STORY" and **JOIN GAME** loads it (in the
  story: the host's current checkpoint). The host's host-menu flow and the lobby messages are core's (core
  docs/protocol.md "Lobby"); the story sync is ours (below, "Story").
- **CO-OP SETTINGS** (main menu CO-OP page, lobby, pause CO-OP panel): NAME (text box; empty = Steam name; used from
  the next session; the box shows the saved name, not a `--name` from the command line), DEBUG OVERLAY, SHOW NAME TAGS, MAP MARKERS, FRIENDLY FIRE; saved in MafiaCoop.ini.
- **In the world:** every other player's name floats over his character (fades with distance, hidden behind walls:
  a ray from the camera, docs/game-api.md 13) and he is an orange marker on the minimap and map.
- **Quitting:** when a game closes (window X, the game's quit) the others are told at once: guests see **HOST LEFT**,
  the host sees "<name> left the game". Killing the process from outside makes them wait for the timeout: `net_timeout_s` (default 4 s over UDP;
  `net_steam_timeout_s` 6 s over Steam P2P), measured 4.6 s (guest killed) and 4.7 s (host killed). A game that is only
  busy keeps its peers from the transport's keepalive thread (core v0.8.0). A timeout counts as a lost connection
  (see "Disconnects and reconnect" below): a killed guest's slot is held for 60 s before the others see that he did
  not come back; the guests of a killed host try to rejoin him for 60 s.
- **Pause menu:** GAME page (Esc opens the pause menu on MAP; `=` / `-` switch pages, gamepad LB/RB) has a fifth entry
  **CO-OP**. It opens a panel over the pause menu with the players and their ping and the same actions (host from
  the middle of a game, invite friends, join friend / by IP, leave). Esc / B closes the panel only (JOIN FRIEND's
  BACK returns to it).
  While hosting, Up from the first entry selects a guest in the player list; Enter offers **KICK** / **KICK AND BAN**
  with a confirmation ("Kick <name>?", A / Enter = yes, B / Esc = no). The guest sees "You were kicked by the host" and goes
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
| `--name <n>` | Player name for this run only (tests). Default: the NAME saved in CO-OP SETTINGS, else the Steam persona name, else the Windows user name. Never saved into MafiaCoop.ini (core 0.9). |
| `--password <p>` | Host: guests must answer with this password (docs/protocol.md "Session password"). Guest: the password to join with. Never logged (the launcher log shows `(hidden)`). |
| `--require-password-for-invites 1` | Host: Steam lobby members (invited friends) need the password too. Default: they skip it. |
| `--log <tag>` | Log to `logs\mafiacoop-<tag>.log` / `logs\launcher-<tag>.log`. |
| `--debug-overlay` | Show the debug status text (top-left) from the start; F7 toggles it. Pass it in test flows whose captures are read for the status lines. |
| `--window left\|right` | Keep the game window on the left/right half of its monitor's work area (full height, title bar kept), for watching two test instances side by side. Unset = the window is left alone. |
| `--model <alias>` | Your character (see "Characters" below), e.g. `--model sam_hat`; wins over the ini until you pick another in the CO-OP panel. |
| `--test_pad 1` | Tests: a virtual gamepad the game and our panels read like a real one, pressed with `mc_pad(...)` from the dev console. |
| `--update-as <version>` | Tests: the update check compares the newest release with this version instead of modVersion, e.g. `--update-as 0.15.0` (see "Update check"). |
| `--update-url <url>` | Tests: the update check asks this URL instead of https://justaddmp.com/api/version/mafia-de. |

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

**Disconnects and reconnect:** when a guest's connection is lost (timeout after `net_timeout_s` /
`net_steam_timeout_s`, a Steam P2P failure, the host's network blip) - not when the host left cleanly, kicked or banned
him, or he left himself - the guest's game keeps running and core rejoins the same host (same address or SteamID)
with backoff for 60 s; a line over the game says **CONNECTION LOST - RECONNECTING... 12 S** (and the CO-OP panels
"CONNECTION LOST - RECONNECTING TO <HOST>... 12 S"; LEAVE CO-OP stops it). The host keeps his slot (same player id)
for 60 s, removes his puppet meanwhile and shows **<NAME> LOST THE CONNECTION - WAITING FOR THE PLAYER TO
RECONNECT... 45 S** (the other guests too). Back in, the host sends him what a newcomer gets (his PlayerModel, story
state, checkpoint save, cutscene, NPC states) and the puppets come back; the guest is not reloaded unless the host
moved to another checkpoint meanwhile (then he follows as usual). Core docs/protocol.md "Reconnect", ours
docs/protocol.md "Reconnect". If the 60 s run out, the reason is logged (`coop: disconnected: ...`), shown in a
banner (and for 20 s in the debug status text), and the mod goes back to idle. Log lines: `session: ... reconnect
...`, `coop: reconnect: ...`, `reconnect: ...`; trace records `RCN`.

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
out\MafiaCoop.exe --host --log host --window left --debug-overlay
out\MafiaCoop.exe --join 127.0.0.1 --log guest --window right --debug-overlay      (after the first window is up)
```

`--window left|right` puts the two windows side by side (each on half of the monitor's work area; the dll moves the
window as soon as it exists and puts it back whenever the game resets its size, checked twice a second; the swap chain
follows through the ResizeBuffers hook). Both windows open; the games sit at the title screen. Check `out\logs\mafiacoop-host.log` for
`coop: player 1 '<your Steam name>' joined` and `out\logs\mafiacoop-guest.log` for `coop: handshake complete`, and the
`status:` lines for pings. Leave out `--name`: it replaces the Steam name for that run (both instances share one
Steam account, so both show the same name; that is expected). Before 0.13.0 these commands had `--name Host` /
`--name Guest`, which is why name tags said HOST / GUEST in tests. Both instances share one SteamID, so they can only connect over UDP; Steam P2P needs
two accounts on two PCs. Stop with `taskkill /IM mafiadefinitiveedition.exe /F`.

### Update check

Once per game start (core v0.13.0, `external/core/src/core/update_check.h`) the dll asks
`https://justaddmp.com/api/version/mafia-de` (`GameInfo::updateId` in `src/game/game_info.cpp`), which answers with
the newest GitHub release of this repo, pre-releases included, e.g.
`{"mod":"mafia-de","latest":"v0.16.0","prerelease":true,"url":"https://github.com/JustAddMP/MafiaDE/releases/tag/v0.16.0","download":".../MafiaCoop-0.16.0.zip",...}`
(the website caches it for 5 minutes; source: the justaddmp.com repo, `app/api/version/[mod]/route.ts`). The request
starts on the overlay's first frame, on its own thread, 5 s per step. If `latest` is newer than `modVersion`, a
banner "UPDATE AVAILABLE" shows for 20 s once the main menu is up, and CO-OP SETTINGS keeps a gold line
"UPDATE AVAILABLE: V... AT JUSTADDMP.COM". Nothing waits for it; offline, a timeout or an odd answer only log.

To see the banner without a newer release, pretend to be an older build:
`out\MafiaCoop.exe --update-as 0.15.0`. The log shows
`update check: checking as version 0.15.0`, `update check: asking https://justaddmp.com/api/version/mafia-de` and
`update check: v0.16.0 is available (this is v0.15.0)`; at the main menu the banner appears. Without it:
`update check: up to date (this is v0.16.0, newest release v0.16.0)`. Check the endpoint itself with
`curl https://justaddmp.com/api/version/mafia-de`. Core's `update_check` test covers parsing and comparing versions
and a whole check against a stand-in server; `make_release.cmd` runs it.

## Lua bridge and dev console

`src/game/lua.cpp` hooks `C_ScriptMachine::Tick`/`TickPaused` (pattern scan; docs/re-notes.md 1.5) and, after the
game's own scripts, runs queued jobs and the per-tick callbacks on **GameMainThread** against the game's main
`lua_State` (`luaL_loadbuffer` + `lua_pcall`, stack top restored after every job). Scripts tick from the main menu on
(not on the title screen, not while the game is paused). Missing patterns disable the bridge (logged), an access
violation inside a job disables it for the rest of the session instead of crashing.

Globals the dll adds: `mc_log(...)` (writes to our log), `mc_u64(str|num)` / `mc_u64str(v)` (HKS ui64 values),
`mc_pad(buttons, holdMs, lt, rt, lx, ly, rx, ry)` (tests: holds core's virtual gamepad, needs `--test_pad 1`; e.g.
`mc_pad(0, 1500, 0, 0, 0, 30000)` walks forward), `mc_player_profile()` ("default override" profile hashes of the
game's player model manager), and for story tests (main menu): `mc_menu_load("free_ride" | "continue_game")`,
`mc_load_replay("checkpoint_mm_110_omerta_cp_010.sav")` (a shipped 00_mission_replay save, loaded the way a guest
loads the host's save), `mc_load_file(path)` (any save file), `mc_follow_reset()` (the guest forgets which host point
he loaded, so a mismatch reloads it), `mc_follow_force()` (a following guest in the world goes to the main menu and
loads the host's point again: the reload path), `mc_start("story" | quest id | "quest@cp_040")` (at the main menu:
what the lobby's START of that mode does on this game; `quest@cp` = host only, the chapter start save with its quest
started at that checkpoint), `mc_story_reload()` (what a death in a co-op chapter calls), `mc_entity(id)` and
`mc_entity_ids(type)` (the entity table: a wrapper by entity id, every id of a type, 8 humans, 10 cars, 65 spawners),
`mc_fnv(text)` (the game's 64-bit name hash of a profile path, as a decimal string) and, for finding fields at
runtime, `mc_memfind(id, value [, span [, depth]])` (where a u64 or a text sits in an entity's object and the objects
it points to), `mc_memread(id, "+0x10>+0x38")` and `mc_memdump(id, path, bytes)` (reads go through
ReadProcessMemory, so a bad pointer only fails the read).

Dev console, to explore the API live:

- **F8** (game window focused) runs `out\dev.lua`; the chunk's first return value is logged and written to
  `out\dev\<tag>.out.txt`.
- **File drop:** write `out\dev\<tag>.in.lua` (`<tag>` = the `--log` tag, `default` without one); the dll picks it up
  within 0.5 s, deletes it, runs it and writes `out\dev\<tag>.out.txt` (`ok` or `error`, then the result).
  `tools\devlua.ps1 -Tag host -Code 'return game.game:GetActivePlayer():GetPos()'` does the round trip.
- What was found this way is in `docs/game-api.md`.

## Test checklist: 0.15.0 features

Everything in 0.15.0 was built without starting the game (static research + earlier logs). A first in-game test ran
on 2026-10-18 on one PC (two instances; Free Ride, CHAPTER 2: TAXI, CHAPTER 13: SALIERI). Verified there: car
signals, failed missions in one load (host's and guest's failure), the police offence fix (968d300). Everything else
still needs the checks below. Two instances side by side on one PC (host `--window left`, guest `--window right`,
`--debug-overlay`, see "Two instances on one PC"), unless a step says two PCs. After each run compare the traces
(`python tools/sync_compare.py <host trace> <guest trace>`: it lists actions, swapped cars, guest-owned NPCs,
traffic, map markers, latency and reconnects per game). Trace records: docs/sync-trace.md. Back up the save folder
first and compare it afterwards: in the 2026-10-18 test `profiles\temporaryprofile\garage.pf` was written by both
games during the co-op chapter.

Unreleased fixes (merged 2026-10-19, code only, none checked in game yet):
- [ ] Garage (save_paths.h): back up the save folder, play a CHAPTER session: `profiles\temporaryprofile\garage.pf`
  byte-identical afterwards; log `kept out of the player's profile` and `held until the game restarts`. (Single player
  garage changes after a co-op session in the same process are lost until the game restarts.)
- [ ] Host profile (save_paths.h `Guard`, after the 2026-10-20 test): back up the save folder, HOST a lobby FREE RIDE,
  take a car from the Free Ride garage (next to the start) and put it back, STOP HOSTING in the world, quit to the
  main menu: `garage.pf`, `aslot.pf` and `gamesaves\default\aslot\save1.sav` byte-identical afterwards (one PC:
  tools\onepc run_stop.ps1 `profile and save files changed by the run: 0`); host log `savesys: hosting: saves, slot
  profiles and progress profiles ... now go to`, `profile write #n kept out of the player's profile (progress: the
  cars in the player's garage)` at each garage visit, never `savesys: written: garage.pf`; `out of the co-op world
  (main menu)` only once the main menu is up. Then a STORY session as host: his story saves change as in single
  player (`written: save0.sav (... the STORY host's own story ...)`), `garage.pf` does not. Then single player
  without a session after a restart: `written: garage.pf` again.
- [ ] Police decay: the guest makes himself wanted (ZoneCreate HUNT) with the host's cops near his puppet:
  `POD hold=1`, his zone stays more than 60 s; drive away: `POD hold=0` within 4 s; the host's `POL w=0`
  throughout. Watch for a mission that holds the decay itself being released by ours.
- [ ] Map markers: the guest's log `waits at his position until ... brain switch` and `checked on his puppet: the game
  has it`; `MK 0:M/human/puppet/ok`; the marker on both minimaps on foot, in a car and after a reconnect. If
  GetIdByScriptWrapper gives 0 for a working icon it falls back to an icon at his position.
- [ ] Seats after a reload: Taxi cp_020, the guest rejoins: `SCP ev=place dec=wait` then `dec=aside`, the puppet on
  seat 0, not `veh-hidden`; the host drives off: `dec=release`; a guest reload: `SCP ev=world` / `ev=oldcopy`,
  `DRK ev=newkey`.
- [ ] Melee animations: the host taps Q about 15 times and throws (G); the guest's log `ANIM: melee actions SHOW with
  '<way>'`; the AE start= latency; from the console `MafiaCoop.TestAction(1, "direct")` and the other ways,
  `TestAction(15, ...)` checks the hand measurement. Regression: walking and seats after punches played through script
  control.
- [ ] Car look: the host's log `look: car 0:<id>: its look from our game`; the guest's `its look set on our copy` and
  `shows its look`; the same body, colours and plate on screen; no `parts differ ... layout`.
- [ ] Car damage echoes: blow up the host's car: the guest's log `an echo of our own writes, not reported`, trace
  `CDX echo=wreck`; the host's car stays a wreck.
- [ ] Occupied copies: the host gets into a car with another body variant than the guest's spawn and drives off at
  once: the guest's log `waits for car ...'s look before it is seated` then `look is on ... seated now` (trace
  `SCP ev=lookwait`), no `thrown out of car`. The host changes the car's tuning (a garage) while his puppet sits
  in the guest's copy: `its tuning (...) waits: someone sits in our copy`, trace `CLD ev=defer`; he gets out:
  `is empty: its tuning is set now`, `CLB` and `was rebuilt by its tuning`, no `CDX` report of the rebuild.
- [ ] Script control: punches right after the guest's puppet spawns (before `friend brain set`): `script control
  not asked ... friend brain switch is not over yet`; punch then get in a car / fall down: `script control given
  back (car seat|it goes down, ...)`; trace `SC late=1` only for answers after an action ended.

Unreleased fixes from the first two-PC test (merged 2026-10-19, code only, none checked in game yet; two PCs where
possible):
- [ ] Car copy speed: the log `SetSpeed(km/h) accepted` (once); `sync_compare.py` copy / driver speed ratio about 1.0
  (was 0.28) and a pinned share far below 56 %.
- [ ] One wheel: ride along, then the driver's puppet leaves the seat on the passenger's game: `sliding to seat 1 to
  give the wheel back` (trace `WHL ev=yield` / `ev=slide` / `ev=done`), never `drives it too; we keep it` on both
  games. Both on one motorbike: `bike ...: player N rides it too and keeps it` (`WHL ev=bike`).
- [ ] A paused game: the host pauses 10 s: the guest's log `player 0 paused his game`, trace `PAU ev=freeze` /
  `ev=thaw`, `TRZ ev=freeze` / `ev=thaw`; no burst of `knocked ... reported to the host` after the resume
  (`TRG quiet=` counts them instead), no `lost its driver` burst.
- [ ] Traffic crews: the guest drives 100-200 m through the host's traffic: few `ours gets out`, no empty copies driving
  on; crews are not Tommy (`crew profile ... works`); trace `TRO`, `TRH nodrv= orphan=`.
- [ ] NPC fallbacks: the guest gets wanted: the host's log `has no spawn profile (AI type 4): its copy gets
  characters/ai/police_scotch/...`; in a car chase the NPC distances (`sync_compare.py`) far below 60 m p95
  (`NGS unpairs= steers=`).
- [ ] CAR_HAVE_PROBLEM: a car explodes: `problem read from the car`, no `Attempt to call a nil value`.
- [ ] Character change: step CHARACTER 5 times fast: the host sends one `sent PlayerModel (... stable/panel-closed)`;
  the guest logs one `removed (his character changed)` and `old entity N released`, never `STILL there` (trace `CHM`,
  `PRT`). Again while seated in the guest's car seat 1: no second, frozen character.
- [ ] Shooting a player: the host shoots the guest's puppet about 10 times with a pistol at 5-15 m: `our shot ... at
  puppet 1: ray|bullet|game ... Hit of X sent`, the guest `hit by player 0`; a shot through a wall: `PV v=blocked`;
  shots at a swimming puppet still land (`PV demi=1 hit=1`). Friendly fire OFF: `v=off`. If the ray is not good
  enough: the next step is a native hook on the friend check (exe 0x25E0700).
- [ ] Punch lock-on: punch from 2-2.5 m at about 50 degrees off: `locked on to puppet 1 (SetDir ok)`, trace `PVM`.
- [ ] Motorbikes: the host rides a bike: `a motorbike (motorcycle_1935 ...)` and the probe line (`BK`); the guest
  `player 0 rides a motorbike`, `puppet 0 rides bike` or `perched`; no `we did a jump` on getting on or off (`TS`).
- [ ] Water: the host swims: the guest `its player swims`, `shielded, a demigod` (`PG`, `PW`), no `died here`; a car
  driven into the sea: `car K is under water here` (`CUW`).

Next in-game checks (fixes from the 2026-10-18 test, not yet checked):
- [ ] Pad guard: close the lobby with pad X (START), then press Enter on the loading screen / the next menu: the
  game takes it (core 0.11.0 `PadHold`; before, the keyboard stayed blocked until a panel was opened again).
- [ ] Latency summary: a guest that sits in the menus while the host is in the world, then loads in: the LAT summary
  lists the menu wait as `held` (count and max) and leaves it out of the per-system stats; `sync_compare.py` the same.
- [ ] Test input `open friends` opens JOIN FRIEND (README "Testing the co-op panels").

Melee and other actions (docs/game-api.md 17) - BROKEN in the 2026-10-18 test (no punch shows on the puppet):
- [ ] Free Ride, Omerta park (-583.5, 121.5; fists are off near Salieri's bar). Tap Q, hold Q (charged), Left Alt
  (counter), throw a grenade / molotov: the other window's puppet plays each. Host log `our light goes out`, the
  other's `puppet 0 plays a light punch`; trace AL / ACS / ACT / AP / AE (`done=1`, not `why=time_up`). No extra
  damage from the puppet's swing, no sliding or snap during it.
- [ ] Find a way to play the melee set on a puppet: `DesirePlayAnimation(name, 1)` from the console, the
  `MafiaCoop.actionAnims` table (F8), the puppet in combat mode first; and why `AnimPlay` starts ~2 s late.
- [ ] Traversal: vault a low wall, climb a ladder and a ledge, sneak, use a medkit, carry a body: the puppet plays
  each (trace TE / TL / TP / TVC).

Seats (docs/game-api.md 18):
- [ ] The host drives, the guest watches: the host's puppet stays at the wheel for several minutes of driving (no
  `thrown out of car` lines; `puppet N sits in car K seat S (... friend brain ...)`). If it starts to leave:
  `... is in car K but not in seat S ...: put back at once`. Trace PR `brain=1`, `back`, `ejAll`. (2026-10-18:
  thrown out 4 times in the first 0.6 s, then seated for 10 min.)
- [ ] Ride along (E next to the other's car): the driver's puppet is put at the wheel first
  (`ride along: put player N's puppet at the wheel ... first`), the rider ends up in the passenger seat.
- [ ] After the guest's game reloads (following the host to a new checkpoint while both sit in a car): the host
  shows the guest's puppet seated, not `alive/veh-hidden` for minutes (seen 2026-10-18 in Taxi).

Car signals (docs/game-api.md 7 / 16) - verified 2026-10-18:
- [ ] Drive a real police car: siren and beacon from the player's own key reach the copy.

Car look:
- [ ] A player's car copy has the driver's variant, colours and plate (2026-10-18: a black closed smith_v12 showed
  as a light-blue open one; player cars carry no colours / plate yet - needs a wire field first).

Mission car back (docs/game-api.md 15 "Mission car back"):
- [ ] Taxi (lobby CHAPTER 2): the host drives the mission taxi so the guest's is swapped for a copy, then parks and
  gets out where the mission allows it (or a Free Ride car swapped the same way: the host drives the guest's car).
  On the guest the real car comes back at the copy's place (`car K: our own car id is back at the copy's place ...
  parked`), the copy is locked meanwhile; trace SWP / SWR. A wrecked car comes back blown up. The host's own car
  is shown again on the host after a guest drove it. (2026-10-18: only the restore at a world change was seen.)

Car damage and explosions (docs/game-api.md 23 / 24):
- [ ] Crash a car, burst a tyre, set one on fire, blow one up: each shows on the copy (trace CDS / CDT / CDR / CDA /
  CDX). After a blown-up car the host does not log `repaired on our game` from the copy's report (seen 2026-10-18).
- [ ] A grenade, a molotov, an exploding barrel and a blown-up car on one game: the other game shows the picture
  (fire, smoke) and nobody is hurt by it (`boom: ... shown`); check on screen, not only in the log.
- [ ] Grenade arc: a thrown grenade's flight shows on the other game and ends where it exploded (`... 0.0 m from
  where our simulation had it`: seen 2026-10-18; the drawn arc on screen still to confirm with a real throw).

Guest-owned NPCs (docs/protocol.md "Guest-owned NPCs"):
- [ ] A fight where the guest gets there first (Bon Appetit: the guest walks into the kitchen, the host stays
  outside): the guest's enemies appear on the host (`guest N's own NPC ... : mirror M, AI type ..., profile ...`),
  move and shoot; the host's hits reach the guest's NPC, the host's kill kills it; their hits on the host's puppet
  reach the host (`our own NPC ... hit the host's puppet ...: sent to him`). When the host's game spawns that enemy
  itself it is claimed (`guest N handed his NPC ... back`), no doubles. Trace NM / NO / NGO / NGE / NGC / NMC / NMX.

Doors and world items (docs/protocol.md "Doors" / "World items"):
- [ ] A story chapter with doors (Bon Appetit's restaurant, a chapter with a locked door): open, close, kick and
  lockpick a door on one game: the other game's door does the same and the puppet plays the kick / lock pull / door
  open (trace DR / DL / DA). Story doors follow the host.
- [ ] A story fight's dropped enemy weapons and a medkit pickup: both games show the same items (`items here N
  (shared N)` on both), a take by one removes it on the other, the host decides when both reach for it (trace ITH /
  ITG / ITE / ITL). A collectible counts for the player who took it (COL).

Traffic and police (docs/game-api.md 19, protocol.md "Traffic" / "Police"):
- [ ] Free Ride, both on a busy street within 250 m: the guest's log `the host's traffic shows here (...)`, his own
  traffic goes, the host's cars drive past on both screens with the same model, colours and plate (colours and
  plates not compared on 2026-10-18). Walk the guest > 250 m away: `our own traffic is back (...)`.
- [ ] Police: a guest commits a real crime near the host's police: the host stays clean (no `we are wanted` on the
  host), the host's cops chase the guest's puppet (`our police chase guest N`), the guest's own level is held while
  they do (2026-10-18: it ran down) and drops once they lose him. Bump a police copy: put back, TrafficBump logged.

Failed missions in one load (docs/re-notes.md 7) - verified 2026-10-18 (host and guest failure, Taxi cp_020):
- [ ] Both fail at once (a timer, or both drive away): one load each. A mission timer, an ally dead, a target
  escaped, arrested. Check for the trace FAIL `ev=gameover` record (none was written on 2026-10-18).

Players on the map (docs/game-api.md 13):
- [ ] The other player is on the minimap and the full map with his name: on foot, while he drives, during and
  after a cutscene, after a respawn, after a checkpoint reload. Never two icons for one player. Trace MK / MKE.
  (2026-10-18: the logs were right, but no marker was found on the minimap 12 / 40 m away: look again, zoom the
  full map in, check `SetIconTextByNavId` and the icon type.)

Reconnect (core docs/protocol.md "Reconnect", protocol.md "Reconnect"):
- [ ] Over Steam P2P (two PCs): pull the guest's network cable / turn off his Wi-Fi for 10-30 s: guest banner
  `CONNECTION LOST - RECONNECTING... N S`, host banner `<NAME> LOST THE CONNECTION - WAITING ...`; back within 60 s:
  same player id, no reload, puppets, NPC copies and traffic copies return; trace RCN. (One PC over UDP, a
  suspended guest: worked on 2026-10-18, 15 s back in the same slot, 66 s slot freed.)
- [ ] LEAVE CO-OP during the retries ends them; the guest's own banner is shown while it retries.

Several guests (docs/protocol.md "Several guests"):
- [ ] Three games (three PCs, or three instances): every game shows two puppets, NPC hits on any puppet, three in
  one car (seat rules), two players going for one car's wheel (one wins, `driver claim` lines), each guest's own
  NPCs on the other guest (trace NRL / NRH / SEA / DRV).

Latency (docs/protocol.md "Latency budget"):
- [ ] Two PCs: `CLK` playout delay and jitter, LAT p50 / p95 per system in `sync_compare.py` (one PC 2026-10-18:
  playout 33-40 ms, jitter p95 9-15 ms, rtt ~6 ms); a big checkpoint save does not hold back player states.

JOIN FRIEND and the physical gamepad (README-dev.md "Player flow", "Testing the co-op panels"):
- [ ] Two PCs, two Steam accounts that are friends: one hosts; the other's main menu CO-OP > JOIN FRIEND lists him
  as `HOSTING (1/4) - IN THE LOBBY`, then `... PLAYING` after START; pick him: joins through the lobby. A friend
  with another mod version shows OTHER VERSION and is not joined. (The panel, REFRESH and BACK worked on one PC.)
- [ ] A real controller (also one plugged in while the game runs, and one unplugged): d-pad and both sticks move
  one row per push, left / right change values, A / B / START / BACK work, a button held while a panel opens does
  nothing; after unplugging, the game still takes keyboard input. Log lines for connect / disconnect.

Button prompts (docs/game-api.md 9 "Button prompts"):
- [ ] Text boxes (X delete / Y paste), the pause panel's Y INVITE and the main menu's CO-OP page footer (the
  game's own). The lobby footers and the pad / keyboard switch worked on 2026-10-18.

Unexplained (2026-10-18):
- [ ] CHAPTER 13: SALIERI: the guest's scripts stopped about 10 s after the opening cutscene (no Lua lines, status
  `no local player`, no error). Play the chapter start again and watch the guest's log.

## Test checklist: 0.16.0

0.16.0 (pre-release 2026-10-20) is not yet tested in game. The next two-PC test (about 25 minutes) plays the
scenarios of "Sync architecture checks" below and runs `python tools/sync_compare.py <host trace> <guest trace>
--check` on each session's pair (every line PASS), then goes through the open round 3 / round 4 items of "Test
checklist: 0.15.0 features" ("Unreleased fixes", "Unreleased fixes from the first two-PC test"; both are in 0.16.0).
Also: a 0.15.0 game joining a 0.16.0 host is turned away with `version mismatch: host has protocol 9 build 1600, you
have protocol 9 build 1500` (JOIN FRIEND: `VERSION 0.15.0 - YOU HAVE 0.16.0`).

## Sync architecture checks (next two-PC test)

The targets of docs/sync-architecture.md section 4. After the test, zip both `logs` folders and run
`python tools/sync_compare.py <host trace> <guest trace> --check` on each session's pair (the newest trace of each PC
in `logs\trace`); every line must say PASS. "no data" means the scenario below was not played long enough.

Play in this order, both players together unless a step says otherwise (about 25 minutes):
1. **On foot, 3 min:** run, sprint, stop sharply, zig-zag, stand still; both players take turns leading.
   `foot.p95` <= 0.8 m, `foot.mean` <= 0.35 m, `foot.stand.p95` <= 0.3 m, `foot.gait` >= 90 %,
   `foot.snaps_per_min` <= 0.5, both directions. Watch: no foot sliding beyond a short stop slide, no walking back.
2. **Driving, 4 min:** each player drives his own car through town, then crashes and drifts.
   `car.p95` <= 1.0 m, `car.pinned` <= 10 %, `car.speed_ratio` 0.9-1.1. VEH `gs=1` rare, `pf` short.
3. **Traffic, 4 min:** together near the host, then the guest drives more than 400 m away and back.
   `traffic.true_p95` <= 1.5 m, `traffic.shown` >= 95 %, `traffic.unshared_near` 0 (both directions). No car pops in
   view when the guest comes back (`TRR` changes, `TRL near=0` 10 s later). Stop next to a red light with cars
   waiting (the host 50+ m away): `traffic.crew_exits_per_min` <= 1, `traffic.reseats_per_min` <= 3, the
   report's `copies of a standing car driven off it` near 0 %; the waiting copies keep their drivers and stand still
   (2026-10-20: drivers got out and back in, copies crept away from the stop line at 4 m/s).
4. **Pedestrians, 3 min:** walk a busy street together, look around; shoot one pedestrian each.
   `ped.coverage` >= 80 %, `ped.p95` <= 1.5 m; the shot pedestrians die on both games (`PDE ev=killreq` / `death`).
5. **NPCs, 5 min:** get a wanted level and let the host's cops chase the guest on foot; then a mission fight if one is
   at hand (Bon Appetit cp_040/050). `npc.p95` <= 1.0 m, `npc.max` <= 4 m, `npc.bw` <= 8 KB/s. Guest log:
   `freeze ... ok`; any `does not follow ... hidden, a copy shows it` means the AI freeze did not hold (note which).
   No double damage on the guest (only Hits with source 1).
6. **World and join, 3 min:** the host sets the time flow fast (`game.gfx:SetTimeFlowSpeed(20)`) for a minute, then
   back; the guest leaves the session and joins again. `world.time_err` <= 0.5, `join.complete_s` <= 10;
   guest log `join: world complete in X s`; the cars parked before the rejoin are there (`PKR`).
7. **Always:** `lat.player.p95` <= 40 ms, `bw.host_up` <= 160 KB/s (with 3-4 players if available).
8. **Motorbike and garage (2026-10-20 findings, docs/test-2026-10-20.md), 3 min:** each player takes a vehicle out of
   the Free Ride garage (a motorbike once, a car once) while the other stands next to the garage door, then rides
   past him. `foot.shown` >= 98 %, `foot.respawns` 0, `foot.respawn_s` <= 5 s, `foot.dz_p95` <= 0.5 m (both
   directions). Logs: no `cars error`; the rider's game `car ...: a motorbike (...)`, the other's `bike copy ready` and
   `puppet N rides bike`; if the garage removes a puppet, `puppet N is gone (removed by the game ...)` and `puppet N
   spawned` within 5 s, never `did not load in 12 s` for a profile that loaded before.

Before the two-PC test, the scripted one-PC run (tools/onepc/README.md) checks the same on one PC.

## Test checklist: animation and car controls (0.13.0; 0.14.0 fixes not yet tested in game)

First two-PC test of 0.13.0 (2026-10-17): see docs/PLAN.md A/B. After each test, compare the two sync traces
(`python tools/sync_compare.py <trace A> <trace B>`, "Network statistics and measuring desync" below).

Built from reading the exe only (docs/game-api.md 16, docs/re-notes.md 9). Two instances side by side
(`--debug-overlay`, see "Two instances on one PC"), Free Ride or a story save, outside the Free Ride start zone if you
want to sprint (Salieri's bar locks sprint). Watch the other window while you play one; F7 shows the status lines.

On foot (watch the puppet of the player you move):
- [ ] **Feet**: walking, running and sprinting straight: the feet stay planted, no ice-skating; the puppet keeps up
  (the status line's error stays below ~0.5 m; `on foot: run x..` shows the multiplier; after a few seconds of each
  gait the measured walk/run/sprint speeds settle).
- [ ] **Start / stop**: from standing it starts with the player, stops where he stops (a step back at most), no
  sliding into place.
- [ ] **Turns**: a 90 and a 180 degree turn while running follow without a wide arc; turning on the spot turns the
  puppet smoothly (not in one snap).
- [ ] **Sprint**: holding sprint shows the sprint animation (not a fast run); releasing it goes back to a run.
- [ ] **Crouch**: crouch-walk is shown crouched and at crouch speed.
- [ ] **Aim**: aiming (right mouse button with a gun) while walking: the puppet aims in the camera direction and
  still walks; firing shows shots.
- [ ] **Jump / fall**: jumping a fence or dropping off a wall: the puppet ends up at the right spot (snap allowed),
  no stuck puppet.
- [ ] **Blocked**: walk the player where the puppet cannot follow (through a narrow gap / over a car): it is put at
  his spot within ~2 s.
- [ ] In the log: `lua: sync: puppet N: walk speed x...` (the multiplier was taken) and no
  `SetMovementSpeedMult failed`.

Cars (watch the copy of the car the other player drives):
- [ ] **Steering**: the copy's front wheels turn left/right with the driver's, also standing still.
- [ ] **Brake lights / reverse**: braking lights the brake lights; reversing moves the copy backwards smoothly.
- [ ] **Handbrake**: a handbrake turn shows the rear wheels locking / the slide.
- [ ] **Smooth**: no wobble, shaking or jumping of the copy at speed and in corners; it stays on the driver's
  position (`car ...: own physics, X m off` below ~1 m). If the log says `pinned to his transform every tick`, its
  physics did not follow: it then moves exactly as before 0.13.0 (wheels still steer) - please note when it happened.
- [ ] **Getting in / out**: the other player walks to a standing car: his puppet walks with him and gets in with the
  door animation (log `gets into car ... (animated)`); getting out plays the exit and the car stays parked there.
- [ ] **Passenger**: ride along (E next to his car) still works.

Name tags:
- [ ] Without `--name`, tags, the lobby, the CO-OP panel and banners show the Steam name (the same on both instances
  of one account). A name typed in CO-OP SETTINGS > NAME shows after the next session start; with `--name X` the log
  says `name 'X' (from the command line, this run only ...)`.

Measuring: every session writes a sync trace; compare the two with tools/sync_compare.py (below).

## Network statistics and measuring desync

- Every 5 s (`net_log_interval`, 0 = off) each game logs `net:` lines: per peer rtt, loss, KB/s and packets each way,
  resends, the reliable backlog, then KB/s and messages/s per message (PlayerState, NpcStates, HostSave, ...), and any
  drops (core's NetMonitor, external/core/docs/protocol.md "Traffic statistics"). F7 shows the last second.
- Settings (ini or `--key value`): `net_budget_kbps` (per-peer limit, KiB/s, default 1024), `net_state_hz` (default
  60), `net_interp_ms` (default 50). Host: `enemies_target_guests` (default 1: the host's enemies target the guests' puppets,
  docs/game-api.md 15).
- **Sync trace (0.14.0, always on):** each session writes `out\logs\trace\mafiacoop[-<tag>]-<date>-<time>.trace`:
  every state sent and received, clocks, every controller decision per puppet / car copy, readbacks of what the game
  really did (car controls, engine, gear, wheel angle; puppet seat, physics, sprint), NPC distances (guest) and a copy
  of the log. Format: docs/sync-trace.md.
- **Comparing two games:** `python tools/sync_compare.py <trace of one game> <trace of the other> [--csv DIR]
  [--worst N]` (the same session; from two PCs or one). Per direction: loss, latency and jitter, the clock offset,
  the distance between the puppet / car copy and where the player really was at the same moment (per gait), gait
  matches, snaps / nudges / catching up, pins and retries, copy speed, steering wish and wheel angle against the
  driver's, seat ejections, NPC distances, the worst moments with wall-clock times; `--csv` for plotting. Numbers from
  0.9.0 are in docs/protocol.md "PlayerState". The test pad (`mc_pad`) did not move the player in some sessions;
  keyboard keys sent to the focused window (W/A/S/D) always did.

## Testing the co-op panels (gamepad and mouse)

Scripts in the game stop while the pause menu is open, so tests drive the panels from the dll instead (core 0.7,
external/core/src/dll/test_input.h): start the games with `--test_input 1` and write commands into
`out\dev\<log tag>.input.txt`, e.g.

```
open pause            # the pause CO-OP panel (also: lobby, settings, joinip)
pad DOWN              # virtual XInput pad (the same hook a real pad goes through); A, B, LEFT, RIGHT, A+B, 0x1000 ...
pad A
move 600 360          # mouse in backbuffer pixels, handed to the panel's input queue
click 634 465
wheel 1
wait 300
dump                  # the open panel, its rows with hit boxes and the selected row, into the log
```

Checked 2026-10-16 (two instances, Free Ride): pause CO-OP panel d-pad up/down, right/left on CHARACTER, A on
SETTINGS, B back, mouse hover and click (into SETTINGS), wheel; CO-OP SETTINGS d-pad; the lobby (host: d-pad to
MODE; guest: mouse hover and click on SETTINGS). Real window mouse messages (`PostMessage` WM_MOUSEMOVE /
WM_LBUTTONDOWN to the game window) selected a row and clicked BACK too. Not checked: a physical gamepad.

`open friends` opens JOIN FRIEND the same way (its rows are the friends, then REFRESH, STEAM FRIENDS LIST, BACK).

How a physical gamepad reaches the panels (core `dll/ui_input.cpp`, `dll/pad_nav.h`; automated part: core
`tests/pad_nav.cpp`):

- Every XInput user 0-3 counts (a pad that Windows put on slot 2, or two pads: buttons combined, the stick pushed
  furthest wins). Empty slots are probed again once a second, not every frame. `ui_input: gamepad N connected` /
  `disconnected` in the log.
- Navigation: d-pad or left stick (stick: dominant axis only, pressed beyond 16000, released below 11000, so a stick
  resting near the edge does not chatter), first repeat after 0.4 s then every 0.15 s, a long frame gives one step;
  Left/Right change `< VALUE >` rows; A selects (in a text box: submits); B, START or BACK go back; X / Y do what
  the footer shows (lobby: X START / READY / JOIN GAME, Y INVITE; JOIN FRIEND: Y REFRESH; text boxes: X delete,
  Y paste; pause CO-OP while hosting: Y INVITE), nothing on panels whose footer has no X / Y.
- Footer prompts: pad glyphs after pad input, key names after a key / click / mouse move, like the game's own footer
  (docs/game-api.md 9 "Button prompts"; core `dll/ui_prompts.h`, automated part: core `tests/ui_prompts.cpp`).
  Each switch is logged (`ui_input: prompts now for gamepad ...`); `dump` prints the footer.
- Only presses act: the A that picked the menu entry, or a stick still pushed when a panel opens, does nothing until
  released (the old code ignored a held A but not a held stick, and took a stick held through the open as a step).
- The game sees an empty pad on every user while a panel is open and until A/B/START/BACK are released on every pad
  (+250 ms); a pad unplugged while a button is held no longer keeps the game blocked (its held buttons are
  cleared when the read fails; before, an unplugged pad with B down blocked the game's input for good). The XInput
  packet number changes when blocking starts and ends, so a game that skips "unchanged" packets cannot keep the
  last real stick position.
- DirectInput joysticks (if the game reads one): blocked with buttons released, POVs centred and the axes frozen at
  their last value (zeroing them would read as a stick pushed to its end); only button releases of their buffered
  data pass. Mafia DE imports XInput 9.1.0 for the pad, so this is a safety net. UNVERIFIED with real hardware: Steam
  Input (a PlayStation / Switch pad mapped to XInput by Steam) going through the same `XInputGetState` hook.

## Player sync (phase 3)

Each game samples its player on GameMainThread, sends `PlayerState` (app id 0x0100, docs/protocol.md) at 60 Hz and
shows every other player as a **puppet**: an AI human in that player's character (host = Tommy as the story dresses
him, guest 1 = Paulie, guest 2 = Sam, guest 3 = Salieri, or the `model` he set) that walks/runs/sprints with the game's own locomotion (`MoveVec`) toward the interpolated position (50 ms
buffer, moved forward by the velocity). Since 0.13.0 its gait and walk speed follow the player's (a speed multiplier
calibrated on the puppet, so the animation matches the ground speed and the feet do not slide); its position is only
nudged when it is more than 0.9 m off and snapped beyond 3.5 m (docs/protocol.md "Driving the puppet and the car
copy"). Standing, it turns to the reported heading; it holds the reported weapon, walks with its player to a car door
and gets in with the game's animation when the car stands.
Puppets only appear once the local player is in a loaded world (not in the main menu).

Cars: when a player drives, the others see the same car (their own copy of it) following the driver, with the
driver's puppet in the driver seat. Since 0.13.0 the copy gets the driver's steering, throttle, brake and handbrake
(its front wheels steer, brake lights), and its own physics carries it with a small correction; when its physics does
not keep up it is moved to the driver's transform every tick as before (logged "pinned"). **E** (pad **Y**) next to a car another player drives puts you in its first free
passenger seat ("ride along"); the others see your puppet sitting there. When the driver gets out, the car stays
where it was left on every game, and anyone can get in and drive it (owner = last driver). Details and message flow:
docs/protocol.md "Shared cars, seats and ownership". Puppets are removed when their player leaves and all at once when
the session ends.

On foot, puppets also crouch, aim (right mouse button / pad left trigger with a gun in hand, in the camera's
direction) and fire a real shot (flash and sound) for every shot the player fires, and play a reload.

**Characters:** pause menu > CO-OP has **CHARACTER** and **OUTFIT** rows (Left/Right or Enter step through them; pad
d-pad left/right): DEFAULT (by player id: host Tommy as his story dresses him, then Paulie, Sam, Salieri) or Tommy,
Paulie, Sam, Salieri, Frank, Vincenzo, Luigi, Ralph, Sarah, Morello, Sergio, Norman, Bertone, Billy, each with its
outfits (STORY = as the loaded story point dresses him; Tommy: casual, cab driver, suit and hat, trench coat, racing,
sailor; Paulie and Sam: suit, suit and hat, trench coat; Frank: suit, coat). A pick applies at once: you change in the
world (when the pause menu closes; the game does not run while paused) and the others' puppet of you is replaced live
(PlayerModel, docs/protocol.md). It is saved as `model = <alias>` in MafiaCoop.ini (`--model` and the ini line work
too; a spawn profile path or decimal hash also works). An unknown alias, or a profile that does not load within 12 s,
falls back to the default for your player id. The list and its aliases: `src/game/characters.cpp`. Two test
instances on one PC share `out\MafiaCoop.ini`: give the guest `--model`, and delete the file after a test that used
the panel. Puppets get any weapon (Tommy gun included) through the debug inventory helper.

**Damage between players** (docs/protocol.md "Phase 6"): shooting, punching (key Q: tap = punch, hold = charged
punch) or running over another player's puppet hurts that player on his own game: bullets are replayed as a real shot
from your puppet there (else `DoDamage`), punches and rams are `DoDamage` (a flinch). The game itself turns melee off
in safehouse zones (around Salieri's bar, where Free Ride starts) and at some story checkpoints: Q does nothing there.
Puppets do not dodge cars, so a ram lands like it would on the player. Puppets never die on your screen; they die when their player
dies or is downed. A guest at 12 health or less is downed for 5 s and comes back next to the host; a guest who dies
anyway (NPCs, falls) has his checkpoint loaded at once and is put next to the host; the host dies like in single
player (YOU DIED, load the checkpoint). `friendly_fire = 0` / `--friendly_fire 0` turns it off for that machine.
Health is synced: shown on the debug status line (verified) and in the pause menu's CO-OP panel (`HEALTH n` next to each
player; not checked in a capture yet).
The puppet factory is prepared as soon as the local player is in the world with a session running, and the model of
a player who is still loading is loaded ahead, so his puppet appears with his first state.

**Weather and time of day:** the host's game is the clock. The host sends its time of day, time flow speed and weather
set (WorldEnv, docs/protocol.md) every 4 s and at once on a change; guests apply them.

The debug status text (`--debug-overlay` or F7) shows `Sync: in world, puppet factory ready, 1 shared car(s)` and one line per remote player
(`AEastwood (PAULIE): puppet alive:0.2, 60 Hz, last 0.0 s ago, health 100, crouch, aim` = state, metres off the
target, receive rate, age of the newest snapshot, his health and stance; `alive/seat0` = sitting in seat 0 of a car,
`alive/veh-hidden` = in a car but not seated), and below it either `on foot: run x1.04 (walk 1.4, run 3.9, sprint
6.3 m/s), 0.21 m off` (the puppet's gait and speed multiplier, the walk/run/sprint speeds measured on it, its error) or
`car 0:24577: own physics, 0.12 m off, steer +0.35, power 0.80, brake 0.00` (`pinned` when its physics did not
follow; `(no controls from his game)` when the driver's game could not read them). The log has `sync:` lines (C++) and `lua: sync:` lines (game side).

**background_run** (default on): the game pauses its simulation when its window loses focus (`WM_ACTIVATEAPP`). The
dll swallows that message so an unfocused game keeps running and sending (needed for co-op and for two instances on
one PC). Set `background_run = 0` to get the original pause-on-alt-tab.

### Story (mission sync)

The host is authoritative; guests play his chapter and checkpoint (docs/game-api.md 14, docs/protocol.md "Story").
- The host reports his chapter, checkpoint, objective and the cutscene that plays, and sends his newest story
  checkpoint save (from `Documents\My Games\Mafia Definitive Edition\Data`) whenever the game writes one.
- A following guest (lobby STORY / CHAPTER, JOIN GAME) loads that save from memory: the main menu's FREE RIDE load
  with the file's bytes swapped for the host's save (nothing is written to disk), then the chapter's quest is started
  at the host's checkpoint and he is put next to the host. If the host reaches a checkpoint the guest's own mission
  flow does not reach within 8 s, the guest leaves for the main menu and loads the host's save of it (a crash on this
  reload is fixed: docs/re-notes.md 7). The guest sees the host's objective; cutscenes play on both games (the host skipping
  ends the guest's; a guest who skips waits for the host, "THE HOST IS WATCHING A CUTSCENE"); puppets are hidden
  while a cutscene plays.
- A guest's game writes no saves into his own slots while he is in a co-op session: every save file goes to
  `%TEMP%\MafiaCoop\coop-saves\<pid>` (read back from there in the session, folders of ended games removed at the next
  start; docs/re-notes.md 7). The log says `savesys: save write #n kept out of the real slot`. The slots' profiles
  (`profiles\...\aslot.pf`, `bslot.pf`, `cslot.pf`: the slot's script data) are redirected the same way (0.9.1),
  and so are the progress profiles `garage.pf`, `my2k_game_stats.pf`, `profile_data_storage_persistantdata.pf`
  (after 0.15.0: the 2026-10-18 test found garage.pf rewritten by a co-op chapter); once the session wrote one, it
  stays held (`coop-saves\<pid>-held`) until the game restarts, as the game may keep the session's garage in memory.
  The settings (`gamesettings.pf`, key bindings, language, audio, video, map filter, `Saves\videoconfig.cfg`) are
  the player's own and left alone. Table: src/game/save_paths.h; log `savesys: profile write #n kept out of the
  player's profile (progress: the cars in the player's garage)`.
  The host of a lobby CHAPTER session is redirected the same way from START on (the game would otherwise save that chapter into his Free
  Ride slot); his checkpoint scan reads the scratch tree too, so his new checkpoints still reach the guests. Since the
  2026-10-20 test (the host's `garage.pf` written for real at each garage visit of a co-op Free Ride, his
  `aslot\save1.sav` and `aslot.pf` with it) every other host is covered too: in a FREE RIDE (or any world not loaded
  from his story slot, also his own Free Ride he started hosting from) saves, slot profiles and progress profiles go
  to the scratch tree like a guest's; the host of a STORY session (his world loaded from `aslot`: CONTINUE, his
  checkpoints) continues his own story and keeps saving it (his campaign, as in single player), only his progress
  profiles (garage, stats, persistent data) are kept out. Log: `savesys: hosting: ... now go to <scratch> (the host,
  in a world that is not his own story)` / `(the host of his own story: his story saves stay his own)`. All of it
  ends only once the main menu is up after the session (`savesys: out of the co-op world (main menu)`), so the quit
  save of the co-op world is kept out too; a single-player game after that writes the real files again (held
  progress files excepted). Decision table: src/game/save_paths.h (`Guard`), replayed from that test's logs by
  save_paths_test.
- Dying in a co-op chapter (or failing its mission) reloads the checkpoint: a guest the host's, the host his own. The
  game itself answers a failed mission in these loads with a load of `freeroam_before_quest_activated`, a save of the
  player's own story; save_guard keeps that load inside the session (the slot lookup gets the chapter loader's entry,
  the save file read is served from `coop-saves\<pid>\session-checkpoint.dat`; docs/re-notes.md 7), and once that
  world is up mission_sync leaves it for the main menu and loads the checkpoint (log: `our mission failed`). A STORY
  host keeps the game's own fail screen (his campaign). After a load or cutscene the guest stands 1.5 m to the host's
  right.
- NPCs (docs/protocol.md "NPCs", docs/game-api.md 15): the host sends the NPCs near the players; each guest pairs them
  with the NPCs of his own mission run (same spots) and keeps them in line: dead when the host's die, health kept, put
  back when they wander off. A guest's hits on them go to the host (replayed as shots from his puppet), so the host's
  NPC dies and the guest's copy with it. The debug status line shows `NPCs: 31 of the host's 66 paired` (guest) or
  `NPCs: sending 48, guest hits 2` (host). Not yet: the host's enemies ignore the guests' puppets; ambient traffic and
  pedestrians are local. A car another player drives is the car of that model standing at the same spot here, if
  there is one (mission cars are not doubled).

### In-world test with two instances on one PC

1. Back up the save folder first (`%USERPROFILE%\Documents\My Games\Mafia Definitive Edition`, OneDrive-redirected
   on some PCs) and never pick "New Game".
2. `out\MafiaCoop.exe --host --log host --window left --debug-overlay`; at the title press Enter, then Enter on **Continue**, then Enter
   when the loading screen says "Continue".
3. `out\MafiaCoop.exe --join 127.0.0.1 --log guest --window right --debug-overlay`, same three Enters.
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
-- give the player a pistol (the puppet draws one too); "smg_trench_a_v1" = Tommy gun
local p = game.game:GetActivePlayer(); p:InventoryAddWeapon("p_mast_a_v1", 50); p:InventorySelect("p_mast_a_v1", true)
-- shoot the puppet of player 1 (real bullets from the local player; his game gets a Hit)
local p, n = game.game:GetActivePlayer(), MafiaCoop.pup["1"].so; local a, b = p:GetBonePos("Head"), n:GetPos()
game.shotmgr:CreateShots(p, Math:newVector(a.x, a.y, a.z), Math:newVector(b.x, b.y, b.z + 1.2), "p_mast_a_v1", 0, 1, true, true)
-- hurt yourself the way a punch from another player would (flinch; a guest is downed at <= 12 health)
local p = game.game:GetActivePlayer(); p:DoDamage(10)
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
model = paulie             ; your character (alias from src/game/characters.cpp, spawn profile or decimal hash); set by
                           ; the CO-OP panel's CHARACTER/OUTFIT rows; empty = default by player id (host tommy)
puppet_model = visuals/hero/game_main_tommy_casual   ; spawn profile for every puppet on this machine (tests)
friendly_fire = 1          ; 0 = shooting/punching/ramming other players does not hurt them (and they cannot hurt you)
name_tags = 1              ; 0 = no names over the other players (CO-OP SETTINGS > SHOW NAME TAGS)
map_markers = 1            ; 0 = the other players are not on the minimap/map (CO-OP SETTINGS > MAP MARKERS)
debug_overlay = 0          ; 1 = debug status text at start (CO-OP SETTINGS > DEBUG OVERLAY, F7)
test_pad = 0               ; 1 = a virtual gamepad for automated tests (mc_pad from the dev console)
follow_grace = 8           ; guest: seconds the host may be at another checkpoint before we reload his (story)
story_load = replay        ; replay | story | free: flags of our chapter loads (tests; docs/re-notes.md 6)
fail_load = freeride       ; freeride | direct: a failed mission's load (direct = one load, unsafe, tests only; re-notes 7)
net_timeout_s = 4          ; seconds without a packet before a player is dropped (UDP; core v0.8.0; 2..60)
net_steam_timeout_s = 6    ; the same over Steam P2P
window = left              ; left|right: keep the game window on that half of its monitor (test instances side by side)
```

## Logs and crashes

- `out\logs\mafiacoop.log` (or `mafiacoop-<tag>.log` with `--log <tag>`; a second concurrent instance without
  `--log` writes `mafiacoop-<pid>.log`): one line per event (timestamp, thread id, level), written to the file straight away.
  The previous five runs are kept as `mafiacoop.log.1` (newest) .. `.5`. A heartbeat with FPS is logged every 10 s
  from the Present hook.
- `out\logs\trace\*.trace`: the sync trace of each co-op session (docs/sync-trace.md), the newest 20 kept.
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
