# Mafia: Definitive Edition — Co-op

**Status: in development, playable but not optimal.** A from-scratch co-op mod: one small DLL, no separate
server, Steam invites. Players see each other walking, running, armed and driving. The host's NPCs, traffic,
weather and missions are not shared yet.

The previous MafiaMP-based version (host's NPCs and weather mirrored, but a 400 MB download) is still available
as release [v0.1.0](https://github.com/JustAddMP/MafiaDE/releases/tag/v0.1.0).

## What you need

- Windows 10/11 (64-bit) and Mafia: Definitive Edition on Steam, with Steam running

## Play

1. Run `MafiaCoop.exe`. It starts the game with co-op built in.
2. Host: choose **CO-OP → Host Co-op Game** in the main menu, then invite friends from the pause menu or the
   Steam overlay (Shift+Tab).
3. Join: accept the Steam invite, or choose **CO-OP → Join by IP**.

## For developers

Source, build steps and design notes are in `native/` (`native/README-dev.md`, `native/docs/`).
