Mafia: Definitive Edition - Co-op (MafiaCoop) 0.17.0, pre-release
==================================================================

An unofficial, fan-made co-op mod: play Mafia: Definitive Edition together with friends. One small DLL, no separate
server, Steam invites. This is a PRE-RELEASE: 0.17.0 was checked on one PC with two copies of the game, but it
has NOT been tested on two PCs yet. Read "Known issues" below before you play.

EVERYONE NEEDS 0.17.0: it cannot play with 0.16.0 or older. A player with another version is turned away
with "version mismatch ... Both players need the same MafiaCoop version."

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
- Main menu > CO-OP > JOIN FRIEND: your Steam friends playing Mafia DE and whether they host; pick one, or
- Main menu > CO-OP > JOIN BY IP and type the host's address (ip or ip:port), Enter.
You land in the lobby: pick your CHARACTER and OUTFIT and set READY. If the host is already playing, choose JOIN GAME.


THE HOST'S WORLD AND THE LEASH
------------------------------
Since 0.17.0 the host's game runs the whole world: traffic, police, pedestrians and enemies. Your friends' games show
copies of them and make none of their own. Mafia only brings the streets to life around its own player, so far from
the host a friend's streets would be empty. That is why there is a leash:
- from 300 m away from the host a friend sees FAR FROM THE HOST - THE STREETS THIN OUT;
- beyond 400 m TOO FAR FROM THE HOST - BACK IN 10 S counts down (coming back under 380 m stops it);
- at 0 the screen fades and he is put back beside the host (in his car, on the road behind the host, if he drives).
Nothing is counted during loading screens and cutscenes, while the host has paused, while someone is down, or in the
first seconds after joining. The host can change the numbers in
his MafiaCoop.ini (they count for everyone):  leash_warn_m = 300   leash_max_m = 400   leash_countdown_s = 10
(leash_countdown_s = 0: only the warning, never a pull).


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
- 0.17.0 is NOT yet tested on two PCs. It was checked on one PC with two copies of the game: the host's traffic,
  pedestrians and enemies on the friend's game, the friend's own traffic and pedestrians switched off, the leash
  countdown and pull, a player removed by the game coming back in under 2 seconds, and the new session logs. A few
  numbers were still off in that check (traffic copies up to 2 m from the host's cars at the 95 % mark, a player's
  copy standing on a parked motorbike) - please send your logs (see PROBLEMS at the end).
- A player sitting in a car cannot be shot by the other players, not even through an open window.
- After a player gets off a motorbike, its copy may stay on the other screens for up to a second.
- Enemies and NPCs are partly shared. Mission characters are matched up between the games: deaths are shared
  both ways (a friend's kill also kills the host's enemy, so a friend can clear a fight on his own), health follows
  the host's game, and the host's enemies also aim at the other players. An enemy that only the host's game spawned
  shows on a friend's game as a copy that moves and fires with it and can be killed. Since 0.17.0 an enemy that only
  a friend's game spawned (he got somewhere first) is kept hidden on his game until the host's game has it. Tested in one chapter's shootout (Bon
  Appetit's restaurant, cleared to the last enemy, the chapter not played past it); other chapters' fights were not
  tried. enemies_target_guests = 0 in MafiaCoop.ini turns the enemies-aim-at-friends part off if it causes trouble.
- Mission cars are partly shared: a mission car is not doubled, and the host sits at the wheel of it on a friend's
  screen (checked in Taxi and Bon Appetit). Since 0.15.0 the mission car comes back when the driver parks and gets
  out (not yet tested). Only a few chapters were checked.
- Traffic, police and pedestrians come from the host's game only (0.17.0): far from the host the streets thin out,
  hence the leash above. Parked cars, trams and boats are still each game's own.
- Story co-op was tested in a few chapters (Omerta, Taxi, Bon Appetit, the start of Salieri); other chapters and
  checkpoints may behave differently. A friend who falls behind the host's checkpoint is reloaded at the host's
  checkpoint.
- A failed mission in a co-op CHAPTER (and a friend's failed mission in any co-op game) goes back to the last co-op
  checkpoint in one loading screen (checked in Taxi; other kinds of failure not yet).
- If a player's connection drops, the others see that after about 4.5 seconds (net_timeout_s in MafiaCoop.ini).
  Since 0.15.0 his game then reconnects by itself for up to a minute and he gets his place back (seen once on
  one PC, not yet confirmed, nor over Steam between two PCs). Closing the game normally tells everyone at once.
- Punches do not work near Salieri's bar (where Free Ride starts) or at some story checkpoints: the game itself
  turns fists off there.
- Your own new character shows once you close the pause menu (the others see it at once).
- The co-op panels work with keyboard, gamepad and mouse. They were tested with a virtual gamepad; 0.15.0 fixes
  real controllers (any slot, plugging in and out, sticks) and shows gamepad buttons in the footers (the footers
  were seen switching in the test), not yet tested with a real controller.
- A Steam invite between two PCs worked in the 0.13.0 test; JOIN FRIEND between two PCs has not been tested yet.
- Only the Steam version of the game (build 16502642) is supported.


UNINSTALL
---------
1. If you used "Start from Steam": run  MafiaCoop.exe --uninstall  first (or delete version.dll and
   mafiacoop_path.txt from the game folder).
2. Delete the MafiaCoop folder. That is all: the mod writes nothing else, except temporary co-op saves under
   %TEMP%\MafiaCoop (removed automatically; you may delete that folder too).


PROBLEMS
--------
Logs and crash reports are written to the "logs" folder next to mafiacoop.dll. Each co-op session also writes a
detailed sync record to logs\trace (about 250 MB an hour; only the newest 20 are kept), and when the session ends
(or you quit the game) one zip with everything about it to logs\bundles (MafiaCoop-<date>-<time>-host.zip or
-guest.zip; your password is not in it). After playing, send the newest zip from the logs\bundles folder - both
players. If the game crashed, a "...-recovered.zip" is made the next time you start it: send that one. Report
problems at https://github.com/JustAddMP/MafiaDE/issues

Unofficial fan-made mod, not affiliated with or endorsed by 2K, Hangar 13 or Valve. You need your own copy of the
game; the mod contains no game files. MinHook (c) Tsuda Kageyu, BSD 2-clause licence: licenses\MinHook-LICENSE.txt.
