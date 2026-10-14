# Mafia: Definitive Edition — Co-op

**Status: in development, playable but not optimal.** A from-scratch co-op mod: one small DLL, no separate
server, Steam invites. Players see each other walking, running, armed and driving. The host's NPCs, traffic,
weather and missions are not shared yet.

## What you need

- Windows 10/11 (64-bit) and Mafia: Definitive Edition on Steam, with Steam running

## Play

1. Run `MafiaCoop.exe`. It starts the game with co-op built in.
2. Host: choose **CO-OP → Host Co-op Game** in the main menu, then invite friends from the pause menu or the
   Steam overlay (Shift+Tab).
3. Join: accept the Steam invite, or choose **CO-OP → Join by IP**.

## Start from Steam (optional)

To start the game from Steam as usual and still have co-op, run `MafiaCoop.exe --install` once (from a command
prompt in the MafiaCoop folder). It asks first, then puts two small files into the game folder: `version.dll` (loads
MafiaCoop when the game starts) and `mafiacoop_path.txt` (where your MafiaCoop folder is). Keep the MafiaCoop folder
where it is; settings come from `MafiaCoop.ini` next to `mafiacoop.dll`. If another mod already uses a
`version.dll`, MafiaCoop changes nothing and tells you. To remove it, run `MafiaCoop.exe --uninstall` (or delete
those two files); it only ever deletes its own files.

## For developers

Source, build steps and design notes: `README-dev.md` and `docs/`. The game-agnostic part (launcher, networking,
overlay, co-op UI) is [JustAddMP/core](https://github.com/JustAddMP/core), a git submodule in `external/core`
(clone with `git clone --recursive`, or run `git submodule update --init`).

## Screenshots

| | |
|---|---|
| ![CO-OP in the main menu](docs/screenshots/main-menu.png) | ![Co-op menu while hosting](docs/screenshots/coop-menu-hosting.png) |
| CO-OP entry in the game's own main menu | The co-op page while hosting |
| ![Co-op panel in the pause menu](docs/screenshots/pause-menu-coop.png) | ![Two players in the world](docs/screenshots/in-game-two-players.png) |
| Pause menu: players, ping, invite friends | Host's view: the guest standing next to them |
| ![Guest's view](docs/screenshots/in-game-guest-view.png) | |
| Guest's view after joining mid-game | |
