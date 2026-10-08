# Mafia: Definitive Edition — co-op

Play the real single-player campaign with friends riding along.

## Host (you)

1. Double-click **`1 - HOST - Play Campaign.cmd`**. It starts the co-op server if needed, then the game.
2. In the game's own menu start a New Game or pick a chapter. The game connects to the server by itself
   (top-right corner shows it connecting; you keep full control).
3. Play. Friends appear next to you and see what you see.

Forward **UDP 27015** on your router to this PC. That is the only port.

## Friends

Send them `dist\MafiaMP-Coop-Client.zip` (rebuild it with **`4 - Make Friend Package.cmd`** after any update,
it embeds your current public IP). They extract it anywhere and double-click `JOIN GAME.cmd`. They need the game
on Steam, Steam running, and the Microsoft Visual C++ 2015-2022 x64 redistributable.

## Testing alone

Start `1 - HOST - Play Campaign.cmd`, load a chapter, then `5 - TEST - Second Client.cmd`: a second game starts
from the friend package and joins your own server as "Tester" (enter `127.0.0.1` and `27015` in its menu if it
does not connect by itself). You should see the second Tommy next to you, and in the second window `/mirror`
should list your NPCs and cars. Play both windowed; it needs a lot of RAM.

## In game

| What | How |
| --- | --- |
| Teleport to the host | `/join` (friends) |
| What is being streamed | `/mirror` |
| Co-op panel / hide overlay / console / disconnect | `F4` / `F7` / `F8` / `F9` |
| Stuck, no controls | `Esc` closes the panel; `F1` bypasses the control lock |
| Scripted co-op chapters instead of the story | `/mode campaign` (host), `/mode story` to go back |

Everyone's microphone is live on connect (proximity voice).

## Folders

- `dev\` — build scripts, developer notes (`README-dev.md`), simulator, build logs. Only needed to change code.
- `server\` — the running server (logs in `server\logs`).
- `dist\` — the friend package.
- `Framework\` — source and build output (`Framework\builds\build-64\bin`, game logs in its `logs` folder).

## Known limits (experimental)

Mission logic only knows the host: friends can drive, shoot and follow, but objectives are yours. Mirrored
people all look like Tommy for now. No cutscene sync: friends wait while you watch one.

## Source layout (GitHub: JustAddMP/MafiaDE)

This repository holds the scripts, docs and the co-op gamemode, plus `overlay\`: every file we changed or added in
the two upstream projects, [MafiaHub/Framework](https://github.com/MafiaHub/Framework) and
[MafiaHub/MafiaMP](https://github.com/MafiaHub/MafiaMP), pinned to the commits in `overlay\VERSIONS.txt`.
To rebuild from scratch: clone Framework at that commit into `Framework\`, MafiaMP at its commit into
`Framework\code\projects\MafiaMP`, run `dev\apply_overlay.cmd`, then `dev\build_all.cmd`. After changing code,
run `python dev\tools\export_overlay.py` before committing.
