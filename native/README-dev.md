# MafiaCoop native: developer notes

## Build

```
G:\MafiaCoop\native\build.cmd
```

- Works from any directory. Needs Visual Studio 2022 or newer with the x64 C++ toolset. The script finds
  `vcvars64.bat` through vswhere if `cl.exe` is not already on PATH. It does not use CMake or Ninja.
- Flags: C++20, `/MT` (static CRT), `/O2 /Zi` with full PDBs, `/W4 /permissive-`. MinHook is compiled as C at `/W3`.
- Output goes to `out\` (gitignored):
  - `MafiaCoop.exe` (+ `MafiaCoop-launcher.pdb`): the launcher, built from `src\launcher\*.cpp`
  - `mafiacoop.dll` (+ `mafiacoop.pdb`): every `.cpp` in `src\core`, `src\render`, `src\game`, `src\net` and
    `src\dll`, plus MinHook. New files are picked up automatically.
  - `<name>.exe` for each `tests\<name>.cpp`, linked with `src\core` + `src\net` (+ MinHook). Skipped if there
    are none.
  - Object files go to `out\obj\<group>\`.
- Any compile or link error stops the build with exit code 1.

## Run

```
out\MafiaCoop.exe [--host] [--join <ip[:port]>] [--name <name>] [--<key> [value]] [--game <path to exe>]
```

1. The launcher finds the game in this order: `--game`, then the Steam registry (`HKCU\Software\Valve\Steam\SteamPath`,
   HKLM `InstallPath`), then every library in `steamapps\libraryfolders.vdf` that holds `appmanifest_1030840.acf`
   (using its `installdir`). Last fallback: a `game_path = <folder or exe>` line in `out\MafiaCoop.ini`.
2. It refuses to start if the game is already running.
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

## MafiaCoop.ini (optional, next to the exe/dll)

```
game_path = G:\SteamLibrary\steamapps\common\Mafia Definitive Edition
inject_method = apc        ; or thread
window_timeout_s = 300     ; how long the dll waits for the game window
crash_test = 0             ; 1 = raise an access violation in Tick after crash_test_delay seconds (debug only)
crash_test_delay = 20
```

## Logs and crashes

- `out\logs\mafiacoop.log`: one line per event (timestamp, thread id, level), written to the file straight away.
  The previous run is kept as `mafiacoop.log.1`. A heartbeat with FPS is logged every 10 s from the Present hook.
- `out\logs\launcher.log`: what the launcher found and did.
- `out\logs\crash-<date>-<time>.dmp`: a minidump written on a fatal exception. The log gets the exception code,
  the faulting module+offset, the registers, and an unwound stack as module+offset. Open the dump in Visual Studio
  or WinDbg with `out\*.pdb` on the symbol path.
- The game installs its own unhandled-exception filter after we do. The heartbeat puts ours back and chains to
  the game's.

## Stop the game

```
taskkill /IM mafiadefinitiveedition.exe /F
```
