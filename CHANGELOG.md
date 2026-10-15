# Changelog

Mod versions of MafiaCoop (Mafia: Definitive Edition co-op). Everyone in a session needs the same version. Supported
game: the Steam build 16502642 (exe CRC32 `BCE2C8EF`).

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
