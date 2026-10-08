# coop-story — the Mafia: Definitive Edition campaign as a co-op game (MafiaMP)

A server-authoritative mission system that runs on the MafiaMP multiplayer mod.
Players join one server, ready up in front of Salieri's Bar, and play the 20
chapters of the campaign together: shared vehicles, shared objectives, enemies
spawned by the server, checkpoints, time limits, team wipes, a race, and an
in-game HUD overlay.

The game's own mission scripts, cutscenes and story NPCs cannot run under
MafiaMP (the mod boots the free-ride save and disables the game's Lua), so
every chapter is rebuilt from the gameplay beats of the original on the
free-ride map.

## Layout

```
coop-story/
  package.json              resource manifest (server + client entry points)
  server/
    main.js                 lobby, host, chat commands, campaign flow
    lib/runner.js           mission state machine (step types below)
    lib/npcs.js             NPC manager: enemies, guards, allies, targets (World.createHuman)
    lib/hud.js              server -> client presentation events
    lib/vec.js              vector helpers (Z-up world)
    lib/recorder.js         /mark and /route authoring commands
    missions/places.js      verified Lost Heaven coordinates + stand-ins, parking spots, weather sets, vehicle models
    missions/models.js      NPC spawn-profile hash pool
    missions/helpers.js     enemy placement helpers (foes, guards, ally, mark, waves, car)
    missions/index.js       campaign order
    missions/NN_*.js        one chapter per file (20)
  client/
    main.js                 turns events into game HUD calls + overlay updates, key binds
    hud.html                CEF overlay (objective, timer, waypoint distance, crew list)
    panel.html              F4 control panel (every command as a button)
```

`G:\MafiaCoop\tools\sim\play.js` plays the whole campaign through these
scripts with fake players (no game needed): `node play.js`, `node play.js
--no-npc`, `node play.js --chapter 7 --verbose`.

## In-game

| Key / command | What it does |
| --- | --- |
| `F4` | open the **control panel**: buttons for every command below, crew list, chapter list, time/weather/teleport, authoring tools. Esc closes it. |
| `F5` or `/ready` | toggle ready; when everyone is ready the chapter starts in 5 s |
| `F6` or `/start [n]` | host starts the next (or chapter `n`) |
| `F7` | hide/show the overlay |
| `F10` or `/wai` | print your position and rotation (`F8` is the MafiaMP console, `F9` disconnects, `F1` unlocks controls) |
| `/missions` | list chapters, `>` marks the next one |
| `/next` | start the next chapter |
| `/skip` | host skips the current objective |
| `/restart` | host restarts from the last checkpoint |
| `/abort` | host returns everyone to the lobby |
| `/places` | verified places vs stand-ins that still need `/mark` |
| `/mark <name>` | print a marker line to the server console (paste into `places.js`) |
| `/route start|add|undo|dump|stop` | record a list of waypoints, printed as JS |
| `/npc <n> [ally]`, `/npcclear` | spawn/remove test NPCs around you |
| `/npcmodels pool|tommy` | varied NPC looks, or the one verified model (use if enemies are invisible) |
| `/veh <model>`, `/tp <place>`, `/time <h>`, `/weather <set>`, `/heal` | utilities |

The host is the first player who joined; it passes on when they leave.
Downed players respawn after 4 s next to the crew (or at the mission car) with
20 s of grace before spread/vehicle rules apply. If everyone is down at once
the mission fails and restarts from the last checkpoint.

## Chapters

1. An Offer You Can't Refuse — taxi fares to the station and the airport, Morello's men at the airport, escape to the bar.
2. The Running Man — station to Salieri's on foot, together, thugs behind you.
3. Molotov Party — sneak into Morello's lot, two cars to the chop shop, fight, back to the bar.
4. Ordinary Routine — protection round, Clark's Motel: rescue Sam, hold the motel (3 waves), drive him home.
5. Fair Play — deliver the race car at night, then the Grand Prix: 2 laps grid → autodrome → grid.
6. Sarah — escort Sarah home, alley brawl (fists), back.
7. Better Get Used To It — hoodlum hangout shootout, chase the leader on foot.
8. The Saint and the Sinner — sneak into the brothel, the informant, church shootout, escape.
9. A Trip to the Country — truck to the farm, 3 waves, load the whiskey, truck home under fire.
10. Omertà — the bank (stealth), the station (fight), the airport (Frank).
11. Visiting Rich People — mansion stealth, steal the papers, escape.
12. Great Deal — parking garage, three floors of Thompsons, escape.
13. Bon Appetit — protect Don Salieri at Pepe's, escort him to the car, chase the shooters.
14. Happy Birthday — board the steamboat unseen, the councilman, fight off the boat.
15. You Lucky Bastard — the bomb, the hotel, the harbor warehouse, Sergio.
16. Crème de la Crème — Morello's hotel, chase to the airport, Morello on the tarmac.
17. Election Campaign — the old prison, the perch, the councilman, escape.
18. Just for Relaxation — harbor stealth, two cigar trucks, delivery, pursuers.
19. Moonlighting — bank lobby, hold 75 s while Paulie opens the vault, escape.
20. The Death of Art — the gallery, three floors, Sam, the last drive to the airport.

Story locations whose coordinates are not recorded yet use a verified stand-in
nearby (see `UNVERIFIED` in `places.js` and `/places`). To move a chapter to
its real location: stand there, `/mark <placeName>`, paste the printed line
over the stand-in in `places.js`, remove the name from `UNVERIFIED`, and
`ensure coop-story` in the server console.

## Mission schema

```js
{
  id, title, subtitle, description,
  environment: { weather, time },      // weather set name + hour 0-24
  spawn: { pos, rot },                 // team start
  vehicles: { tag: { model, pos, rot, label? } },
  weapons: [[weaponId, ammo]],
  steps: [ ... ]
}
```

Step types handled by `lib/runner.js`:

| type | fields | completes when |
| --- | --- | --- |
| `board` | `vehicle` | every player sits in that tagged vehicle |
| `goto` | `target`, `radius`, `who: "all"\|"any"`, `vehicle?`, `timeLimit?` | the required players (in the vehicle, if given) are inside the radius |
| `wait` | `seconds`, `countdown?` | timer |
| `survive` | `seconds`, `stayIn?` | timer; leaving `stayIn` for 10 s fails |
| `together` | `target`, `radius`, `maxSpread`, `graceSeconds`, `noVehicles?` | all arrive; splitting up beyond `maxSpread` for `graceSeconds` fails |
| `convoy` | `vehicles: [tags]`, `target`, `radius` | each tagged vehicle, with someone inside, reaches the target |
| `race` | `gates: [pos]`, `laps`, `vehicles?`, `radius` | all players pass every gate `laps` times |
| `combat` | `enemies`, `waves: [{at, enemies}]`, `kills?` | every enemy is dead (or `kills` reached) |
| `defend` | `seconds`, `area?`, `radius?`, `waves` | timer; everyone outside `area` for `graceSeconds` fails |
| `escort` | `ally`, `target`, `radius` | the ally (follows the nearest player) reaches the target alive |
| `stealth` | `guards`, `target`, `radius`, `detectRadius`, `onAlarm: "combat"\|"fail"` | players reach the target; a player inside `detectRadius` of a guard raises the alarm |
| `killTarget` | `target`, `bodyguards` | the marked man is dead |
| `chase` | `runner`, `path: [pos]`, `catchRadius` | a player gets within `catchRadius` of the runner before he finishes the path |

Any step may also carry `enemies`, `allies`, `guards` (NPC defs, spawned at step start, removed at step end), and the
optional fields `objective`, `hint`, `checkpoint`, `checkpointSpawn`, `environment`, `teleport`, `resetVehicles`,
`summonVehicles` (bring the car to the crew), `outroTitle`, `outro`, `failText`, `timeLimit`, `fallbackSeconds`
(combat without the NPC API), `maxSeconds`, `onEnter(runner)`, `onComplete(runner)`.

NPC def: `{ pos, rot?, model?, weapon?: [id, ammo], role?, name?, health? }` with roles `enemy`, `guard`, `ally`,
`target`, `civilian`. `missions/helpers.js` builds them: `foes(place, n, radius)`, `guards(...)`, `thugs(...)`,
`ally(name, place)`, `mark(name, place)`, `waves([[at, place, n], ...])`.

## NPC API (server build with `World.createHuman`)

`World.createHuman(modelHash: string, pos, rot)`, `World.humans`, `human.goTo(pos, run)`, `attack(target)`,
`follow(target)`, `flee(from)`, `clearOrders()`, `destroy()`, `dead`, `health`, `addWeapon(id, ammo)`; events
`humanDied(human, killer)` and `humanDestroyed(human)`. Without the API the manager reports `available = false`
and combat steps become timed "hold out" objectives (`fallbackSeconds`).

NPCs are simulated by the nearest client (framework delegation) and move in straight lines: no pathfinding, so
they can walk through walls. Combat and killTarget steps auto-complete after `maxSeconds` (default 420 s) so a
model that never streams in cannot soft-lock a chapter.

## Known limits (MafiaMP today)

- Original cutscenes, dialogue and the game's mission scripts are not used; the game always runs the free-ride map.
- NPC AI is mod-side (straight-line movement, aim and burst fire); no cover, no navmesh.
- Only three weapon ids are verified: pistol 2, gold pistol 3, Thompson 85.
- Boats are not spawnable; the steamboat chapter plays on the river bank.
