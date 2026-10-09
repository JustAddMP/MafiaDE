# Mafia: Definitive Edition — Co-op

**Status: playable, but not optimal.** The host plays the real campaign and friends ride along.
Expect rough edges: NPCs all look like Tommy on the guests' screens, punches don't show for the other
player, and mission objectives only track the host.

## What you need

- Windows 10/11 (64-bit)
- Mafia: Definitive Edition on Steam, up to date, with Steam running
- Microsoft Visual C++ 2015-2022 x64 redistributable (most PCs already have it)
- The latest `MafiaDE-Coop.zip` from the [Releases page](https://github.com/JustAddMP/MafiaDE/releases), extracted anywhere

## Host

1. Forward UDP port 27015 on your router to your PC.
2. Double-click `1 - HOST - Play Campaign.cmd`.
3. In the game's menu start a New Game or pick a chapter. The game connects by itself.
4. Give your friends your public IP.

## Join

1. Double-click `3 - JOIN - Join a Game.cmd`.
2. Type the host's IP and your name. The game starts and connects by itself.
3. You appear next to the host. Type `/join` in chat at any time to get back to them.

## In game

- `F7` hide the overlay, `F8` console, `F9` disconnect
- `/join` teleport to the host, `/guns` get a pistol and a Thompson again
- If you can't move: press `Esc`, then `F1`, or type `/unstick`

## For developers

The full source is in this repository (`Framework\` is the MafiaHub engine, `Framework\code\projects\MafiaMP\`
the mod with our changes, the gamemode in its `resources\coop-story\`). Build with `dev\build_all.cmd`,
package with `4 - Make Package.cmd`, commit with `python dev\tools\git_stage.py`. Notes in `dev\README-dev.md`.
