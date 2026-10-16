Mafia: Definitive Edition - Co-op (MafiaCoop) 0.8.0, pre-release
=================================================================

An unofficial, fan-made co-op mod: play Mafia: Definitive Edition together with friends. One small DLL, no separate
server, Steam invites. This is a PRE-RELEASE: playable, but parts of the world (enemies, traffic) are not shared yet.
Read "Known issues" below before you play.

Contents of this zip
  MafiaCoop.exe          the launcher: starts the game with co-op
  mafiacoop.dll          the mod itself (loaded into the game)
  mafiacoop_proxy.dll    only for the optional "Start from Steam" mode (see below)
  README.txt             this file
  licenses\              third-party licences (MinHook)


REQUIREMENTS
------------
- Windows 10 or 11, 64-bit.
- Mafia: Definitive Edition that you own on Steam, installed, with Steam running and signed in.
- The supported game version is the current Steam build 16502642. MafiaCoop.exe checks the game files before it
  starts the game; on another version it tells you and asks whether to start anyway (co-op may then not work).
- Everyone playing together must use the same version of the mod.
- Joining by IP needs UDP port 27960 open/forwarded to the host. Steam invites need nothing extra.


START
-----
1. Extract the whole MafiaCoop folder anywhere (not into the game folder) and run MafiaCoop.exe.
   It finds the game through Steam and starts it with co-op built in. Nothing in the game folder is changed.
2. A CO-OP entry is now in the game's main menu (after FREE RIDE) and in the pause menu.

Check without starting the game:  MafiaCoop.exe --check-only   (prints whether your game version is supported)


HOST A GAME
-----------
1. Main menu > CO-OP > HOST CO-OP GAME. The co-op lobby opens.
2. Invite friends: INVITE FRIENDS in the lobby (Steam's invite dialog), or the Steam overlay (Shift+Tab).
   Friends without Steam invites can join by your IP address (port 27960).
3. Wait until every friend shows READY, choose the MODE and press START. Everyone's game loads it (each player
   presses Enter on his own loading screen):
   - FREE RIDE: Lost Heaven together.
   - STORY: you continue your own story save; your friends join you at your checkpoint and follow you.
   - CHAPTER 1..20: everyone starts that chapter from its beginning.
   You can also just start a game yourself while hosting; friends then get JOIN GAME in their lobby.
4. In the pause menu, CO-OP shows the players and their ping; there you can invite, kick or kick and ban a player,
   set a password, or stop hosting.


JOIN A GAME
-----------
- Accept your friend's Steam invite (or right-click him in the Steam friends list > Join Game), or
- Main menu > CO-OP > JOIN BY IP and type the host's address (ip or ip:port), Enter.
You land in the lobby: pick your CHARACTER and OUTFIT and set READY. If the host is already playing, choose JOIN GAME.


SETTINGS
--------
CO-OP > SETTINGS (main menu, lobby or pause menu):
  NAME             your name for the others (empty = your Steam name; used from your next session)
  SHOW NAME TAGS   names over the other players
  MAP MARKERS      the other players on your minimap and map
  FRIENDLY FIRE    whether shots, punches and car hits between players hurt
  DEBUG OVERLAY    technical status text (also F7)
Your character: pause menu > CO-OP > CHARACTER / OUTFIT (Tommy, Paulie, Sam, Salieri, Frank and more).
Settings are kept in MafiaCoop.ini next to mafiacoop.dll.


YOUR SAVES
----------
- A player who joins someone else's game never has his own save slots written while he is in a co-op session.
- The host of a CHAPTER session does not have his slots written either (the chapter is not his story).
- The host of a STORY session plays and saves his own story as in single player.
- A Free Ride session saves the host's Free Ride as the game normally does.
Backing up "Documents\My Games\Mafia Definitive Edition" before you play a pre-release is still a good idea.


START FROM STEAM (OPTIONAL)
---------------------------
If you prefer to start the game from Steam as usual, open a command prompt in the MafiaCoop folder and run:
    MafiaCoop.exe --install
It asks first, then puts two small files into the game folder: version.dll (loads MafiaCoop when the game starts)
and mafiacoop_path.txt (where your MafiaCoop folder is). Keep the MafiaCoop folder where it is afterwards.
If another mod already uses a version.dll, nothing is changed and you are told so.
To remove it:
    MafiaCoop.exe --uninstall
It deletes exactly those two files (only if they are MafiaCoop's) and leaves everything else alone.
MafiaCoop.exe keeps working either way. In this mode the launcher's game version check does not run.


KNOWN ISSUES
------------
- Enemies and NPCs are partly shared. Mission characters are matched up between the games: deaths are shared
  both ways (since 0.11.0 a friend's kill also kills the host's enemy, so a friend can clear a fight on his own),
  health follows the host's game, and the host's enemies also aim at the other players. Each game spawns a fight's
  later waves when its own player gets there, so stay together: an enemy that only the host's game spawned shoots
  at a friend without being seen on his screen. Tested in one chapter's shootout (Bon Appetit's restaurant), not
  played through to its end; other chapters' fights were not tried. enemies_target_guests = 0 in MafiaCoop.ini
  turns the enemies-aim-at-friends part off if it causes trouble.
- Mission cars are partly shared: a mission car is not doubled, and the host sits at the wheel of it on a friend's
  screen (checked in Taxi and Bon Appetit). Where each player starts in his own mission car, the host's character
  waits hidden until the cars have parted. The car being handed back when the host gets out was not checked. Only a
  few chapters were checked.
- Ambient traffic, pedestrians and police are each player's own (not shared).
- Story co-op was tested in a few chapters (Omerta, Taxi, Bon Appetit); other chapters and checkpoints may behave
  differently. A friend who falls behind the host's checkpoint is reloaded at the host's checkpoint.
- A failed mission in a co-op CHAPTER (and a friend's failed mission in any co-op game) goes back to the last co-op
  checkpoint in two loads: a short stop in a Free Ride world (its loading screen is confirmed for you and the screen
  stays black, as long as the game window has focus), then the checkpoint. Your own story saves are never loaded or
  changed. Failing by leaving the mission area and by losing the mission car were tested; running out of time was
  not.
- If a player's game is killed (Task Manager, crash) instead of closed, the others wait about 4.5 seconds before he
  is shown as gone (net_timeout_s in MafiaCoop.ini). Closing the game normally tells everyone at once. There is no
  automatic reconnect.
- Punches do not work near Salieri's bar (where Free Ride starts) or at some story checkpoints: the game itself
  turns fists off there.
- Your own new character shows once you close the pause menu (the others see it at once).
- The co-op panels work with keyboard, gamepad (d-pad, A, B) and mouse; they were tested with a virtual gamepad,
  not a physical one.
- Steam invites between two different PCs and accounts have not been fully tested; joining by IP is the tested
  route.
- Only the Steam version of the game (build 16502642) is supported.


UNINSTALL
---------
1. If you used "Start from Steam": run  MafiaCoop.exe --uninstall  first (or delete version.dll and
   mafiacoop_path.txt from the game folder).
2. Delete the MafiaCoop folder. That is all: the mod writes nothing else, except temporary co-op saves under
   %TEMP%\MafiaCoop (removed automatically; you may delete that folder too).


PROBLEMS
--------
Logs and crash reports are written to the "logs" folder next to mafiacoop.dll. Report problems (with the log) at
https://github.com/JustAddMP/MafiaDE/issues

Unofficial fan-made mod, not affiliated with or endorsed by 2K, Hangar 13 or Valve. You need your own copy of the
game; the mod contains no game files. MinHook (c) Tsuda Kageyu, BSD 2-clause licence: licenses\MinHook-LICENSE.txt.
