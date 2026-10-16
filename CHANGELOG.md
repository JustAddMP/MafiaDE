# Changelog

Mod versions of MafiaCoop (Mafia: Definitive Edition co-op). Everyone in a session needs the same version. Supported
game: the Steam build 16502642 (exe CRC32 `BCE2C8EF`).

## 0.12.0 (2026-10-16), pre-release

Fights:
- Enemies that only the host's game spawned now show on a friend's game. Each game spawns a fight's later waves when
  its own player gets there, so an enemy the host's game had brought in (the host walked into the kitchen, round the
  back) shot at a friend without being seen. Now the friend's game spawns a copy of it: the same character (the
  mission's own spawn profile) with the host's weapon, moving and firing as the host's enemy does. The friend can
  shoot and kill it, and the host's enemy dies with it; when the host kills it, the copy dies too. The copy runs no
  fight of its own: the host's enemy already shoots at the friend there, and those hits reach him as before (a
  copy's own bullet that still hits him is given back).
- When the friend's own game spawns that enemy after all (he walks there himself), his own takes over and the copy
  goes: no doubles. Copies go with a reload, during cutscenes, when the enemy gets into a car, is out of range (90 m)
  or has been dead for 12 s; at most 10 at a time.
- Characters standing at the same spot on both games are only matched when they are of the same kind (before, the
  host's kitchen machine gunner could be matched with the friend's kitchen rifleman standing there).
- Checked in Bon Appetit (lobby CHAPTER, two games on one PC, three sessions of 22-24 minutes): the kitchen machine
  gunner and rifleman, the yard and balcony gunners and the alley machine gunner that only the host's game had
  brought in appeared on the friend's game (15 copies, two of them forced in a test), stayed with the host's (0.0-0.3
  m apart standing, about a second behind when running) and fired when the host's fired; the friend killed five of
  them and the host's died with them, the host's kills removed them. The restaurant fight was cleared to its last
  enemy (the host circled round behind the four attackers, which then could be killed; one of them by the friend). No
  crash, with three failed-mission reloads and checkpoint follows.

Not finished: right after the cleared fight the chapter failed each time on the way to Salieri (the tests move by
small teleports, which the mission probably took as leaving the area), so Bon Appetit was not played past cp_050;
no second chapter's fight.

Everyone needs 0.12.0 (0.11.x cannot join it: the NPC messages changed).

## 0.11.0 (2026-10-16), pre-release

Fights:
- A friend can win a fight. His game's enemies were kept at 5000 health while the host's lived, so only the host
  could really kill them. Now their health follows the host's game, the friend's own bullets hurt and kill them on
  his game, and the host's enemy dies with them (his bullet is replayed on the host's game from the friend's
  character; when that misses, the kill is passed on anyway). Checked in Bon Appetit's restaurant fight with two
  games on one PC: 10 kills by the friend counted on the host, and with the host not shooting the friend cleared two
  attack waves on his own. The host's kills and the damage he deals show on the friend's game as before.
- Enemies match up again after a reload. After a friend reloaded a checkpoint mid-fight, his enemies were not
  matched with the host's any more (the fight had moved on, and only characters at the same spot were matched). Now
  an enemy that has moved is matched with the nearest one of the same kind within 40 m; enemies the host already
  killed die on the friend's game too. Checked: after a friend's reload in the fight every enemy (7 of 7, 8 of 8)
  matched again.
- When the host's mission fails or he dies and his checkpoint reloads, a friend at that checkpoint now reloads it
  too. Before, his game kept its own fight while the host's started over, and the host's enemies that came back shot
  at him unseen. Checked: the host died in the fight, the friend reloaded on his own and all 6 enemies matched.
- Passers-by that took no damage on a friend's game were characters the game keeps switched off; they are no longer
  matched with the host's.
- Why the restaurant enemies took no damage in the 0.10.0 test: the mission keeps the four attackers in front of the
  restaurant unhurtable until the player has circled round to them; the attack waves that come round the back can
  be killed. A friend whose game lets him kill one of those four also kills the host's.

Not finished: Bon Appetit's restaurant fight was not played to its end (the last part waits for a route the tests
cannot walk), and other chapters' fights were not tried. Each game spawns a fight's later waves when its own player
gets there, so stay together.

Everyone needs 0.11.0 (0.10.x cannot join it: the NPC messages changed).

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
