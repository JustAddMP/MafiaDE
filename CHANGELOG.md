# Changelog

Mod versions of MafiaCoop (Mafia: Definitive Edition co-op). Everyone in a session needs the same version. Supported
game: the Steam build 16502642 (exe CRC32 `BCE2C8EF`).

## 0.10.0 (2026-10-16), pre-release

Story:
- A reloaded checkpoint starts where it should. The game's chapter loader always put the player at the Free Ride
  start (Salieri's bar), whatever the save said; most checkpoints move him themselves, but Omerta's second one does
  not, so after a failed mission the host stood at the bar and that checkpoint's leave-the-area check never armed.
  The mod now puts every player where his checkpoint save has him before the mission resumes. Checked: Omerta's
  second checkpoint failed and reloaded 8 times by the host (3 with a friend connected) and twice by a friend, Taxi
  twice each: always back at the checkpoint's spot, and Omerta's check failed the mission again from every reload.
- A failed mission needs one Enter less: the short Free Ride stop on the way back to the checkpoint is confirmed
  for you and stays black, so you only see the checkpoint's loading screen (about 20 s after the failure). The game
  reads that key only while its window has focus. (Going back in a single load was tried and is unsafe: the game
  then treated the checkpoint as your own story and its fail screen offered your real story save.)
- Losing the mission car fails the mission the same way and goes back to the checkpoint (checked in Taxi, host and
  friend). Failing by a mission timer was not reached in the tests.
- A friend whose own game plays a cutscene of an earlier checkpoint no longer has to watch it to the end while the
  host is already further on (seen in Bon Appetit: the 3-minute ambush film); he goes straight to the host's
  checkpoint.
- Fixed a crash of the host's game during the load after a failed mission (with a friend's character in the world;
  seen once in Omerta, guarded since; five more failures there ran without a crash).

Players:
- Name tags hide behind walls: the game's own ray cast checks the line from the camera to the other player's head.
- A killed or unplugged game is noticed after about 4.5 s (was 10 s): `net_timeout_s` (default 4) in MafiaCoop.ini,
  `net_steam_timeout_s` (6) over Steam. A game that is only busy is not dropped (core v0.8.0 keepalive thread).
- Fixed: a friend's name tag could stay hidden for the rest of the session after he was downed.
- Fixed: after several reloads in one game the mod found only a few of the world's cars (mission cars were not
  recognised).

Tested, not finished: a chapter's own shootout was reached in co-op (Bon Appetit's restaurant ambush): characters
matched up, enemies fought both players, the host's enemies hit the friend, deaths were shared; a friend's kill
counted on the host could not be checked there. In Bon Appetit the host sits at the wheel of the mission car with
Salieri beside him on a friend's screen (the second chapter after Taxi); the car being handed back when the host gets
out could not be checked (the mission keeps the doors locked).

## 0.9.1 (2026-10-16), pre-release

Fixes:
- A failed mission in a co-op CHAPTER (leaving the mission area, abandoning the objective) no longer loads the
  player's own story save. In these chapter loads the game answers a failed mission by loading "the free roam state
  from before the quest", which is a save of the player's own campaign (the reported case: the host's real Omerta
  story). The mod now keeps that load inside the session: the game gets a save of the co-op session instead (in
  practice the Free Ride start, never a save of the player's), and as soon as that world is up the host goes back to
  his last co-op checkpoint, a friend to the host's. Before, the host's takeover only started once the wrong world
  was up, and in that world his game even sent the story save on to the friends. Checked: 8 failed missions of the
  host (Taxi cp_010 five times, one of them with the story save already read earlier in that game and one with a
  friend connected; Omerta cp_020 three times) and 4 of a friend (Taxi three times, Omerta in a STORY game once):
  each time back at the checkpoint, no crash.
- A co-op CHAPTER (and a friend's co-op game) no longer rewrites the save slots' profile files (`aslot.pf`, the
  story slot's script data: its current chapter and weapons; seen: the story slot's Omerta became Taxi). They go to
  the co-op scratch folder like the saves. Checked: a two-player CHAPTER game with a failed mission on each side,
  every save and profile file byte-identical afterwards (only the window size file changed).
- No crash when FREE RIDE is started from the main menu after a co-op chapter in the same game (the chapter
  loader's save storage only knew the first slot it was used with).
- The host of a STORY game keeps the game's own "You abandoned the objective" screen (Load Last Checkpoint): that is
  his own campaign.

## 0.9.0 (2026-10-16), pre-release

Players stay in sync:
- The other player is shown where he really is. Measured on one PC: on foot his character was 2.5 m behind on average
  and up to 9 m off (the game's own walking for him starts late and is slower than a running player); now 0.3-0.5 m on
  average (95 % within 0.7 m), also right after a lobby start and a checkpoint reload. A car at 70 km/h: 0.1 m. The
  game sends its player 60 times a second (was 20) and shows the others 50 ms behind (was 100 ms), moved forward along
  their speed.
- Traffic statistics in the log every 5 s and on F7 (per player and per message); the network limits are much higher
  (`net_budget_kbps`, default 1 MB/s per player), though two players use only about 15-20 KB/s. Core v0.6.0.

Story and NPCs:
- The host's enemies now shoot at the other players too, and those hits hurt them on their own game (not twice when
  their own copy of that enemy already fights them). The game crash this caused before (in the game's own list of
  humans, exe+0x243A915) is guarded by the mod. `enemies_target_guests = 0` in MafiaCoop.ini turns it off.
- In a mission car the host now sits at the wheel on a friend's screen (Taxi), instead of standing next to it.
- No car copy is spawned while a loading screen is up (one spawned then had landed on a friend's own mission car).

Menus:
- Gamepad (d-pad, A, B, left/right on settings) and mouse (hover, click, wheel) checked in the CO-OP panel, the lobby
  and CO-OP SETTINGS, with a virtual pad and real window mouse messages; a physical gamepad was not tried.
- Tests can drive the panels from a file (core v0.7.0 `test_input`).

## 0.8.0 (2026-10-15), first public pre-release

Fixes:
- The host of a lobby CHAPTER session no longer has the chapter saved into his own save slots (it overwrote his Free
  Ride slot): his saves go to the co-op scratch folder like a guest's, and his new checkpoints still reach the
  guests. The host of a STORY session keeps saving his own story, as in single player.
- A player who joined at start-up (Steam invite, `--join`) no longer gets the lobby over the title screen. Before, a
  START from the host then loaded the game from behind the title and hung on a black screen. The lobby now opens
  once the main menu is really shown, and never over a loading screen.
- A friend put next to the host after a load or cutscene no longer lands on a car standing there: another spot
  beside or behind the host is used.

Release:
- Player zip (`MafiaCoop.exe`, `mafiacoop.dll`, `mafiacoop_proxy.dll`, README, licences) built by
  `tools\make_release.cmd`, which also runs every automated test; PDBs in a separate symbols zip.
- Optional "Start from Steam" mode (`MafiaCoop.exe --install` / `--uninstall`) tested with the real game started
  from Steam.

## 0.7.0 (2026-10-15)

- Fixed a crash when a following guest reloaded the host's checkpoint from the world.
- A guest's saves are kept out of his own save slots for the whole co-op session (story and Free Ride).
- Mission NPCs: the host is authoritative (states, deaths, health); guests pair the host's NPCs with their own and
  their hits on them are replayed on the host. Mission cars are not doubled.
- Deaths and failed missions in co-op chapters reload the checkpoint; guests are put next to the host.

## 0.6.0 (2026-10-15)

- Story in co-op: guests follow the host's chapter, checkpoint, objectives and cutscenes; lobby STORY and CHAPTER
  modes.

## 0.5.0 (2026-10-15)

- Pre-game lobby (characters, ready, mode, START for everyone), CO-OP SETTINGS, name tags over players, minimap
  markers, the CO-OP page refreshes itself, players are told at once when someone quits. Core v0.5.0.

## 0.4.0 (2026-10-15)

- Characters and outfits picked in the CO-OP panel and seen by everyone; punches between players; damage from shots
  and cars.

## Earlier (2026-10-13 to 2026-10-15)

- From-scratch rewrite: launcher, injection, own UDP transport and Steam lobbies, host/join from the game's own menus,
  players seen walking, running, armed and driving, shared cars and passengers, weather and time from the host,
  kick / ban / session password, game version check, optional version.dll proxy. The old v0.1.0 build (2026-10-08)
  was a different code base.
