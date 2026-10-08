/**
 * coop-story - server entry point.
 *
 * Lobby + campaign flow on top of lib/runner.js:
 *   - players spawn at Salieri's, mark themselves ready (/ready or F5)
 *   - the host (first player to join) starts a chapter (/start [n] or F6)
 *   - the runner drives the mission; on completion the campaign advances
 *   - /missions, /skip, /restart, /abort, /next for control
 *   - /mark and /route for authoring new waypoints in-game
 */

console.log("[COOP] === coop-story loading ===");

const hud = require("./lib/hud.js");
const V = require("./lib/vec.js");
const { MissionRunner } = require("./lib/runner.js");
const { NpcManager } = require("./lib/npcs.js");
const { cmdMark, cmdRoute } = require("./lib/recorder.js");
const { CAMPAIGN } = require("./missions/index.js");
const { PLACES, WEATHER, VEHICLE_MODELS, UNVERIFIED } = require("./missions/places.js");

const RESOURCE = "coop-story";
// Story mode: what a guest gets on arrival (the host's weapons come from the game itself).
const STORY_LOADOUT = [[2, 120], [85, 200]];
const AUTO_START_WHEN_ALL_READY = true;
const MIN_PLAYERS = 1;

// "campaign": the scripted co-op chapters (lobby at Salieri's, /start).
// "story": the host plays the real single-player campaign in story-host mode and the server only mirrors
//          the host's world to guests. No lobby teleports, no scripted chapters.
// Default comes from mode.json next to package.json; /mode switches at runtime.
let mode = readMode();

function readMode() {
    if (globalThis.__COOP_TEST_MODE__) return globalThis.__COOP_TEST_MODE__; // offline simulator override
    try {
        const m = require("../mode.json");
        return m && m.mode === "story" ? "story" : "campaign";
    } catch (_) { return "campaign"; }
}

let runner = null;
let npcs = null;
let hostId = null;
let campaignIndex = 0;     // next chapter to play
let autoStartTimer = null;
const joinOrder = [];

function safe(what, fn) {
    try { return fn(); } catch (e) { console.warn(`[COOP] ${what} failed: ${e}\n${e && e.stack}`); return undefined; }
}

function panelData() {
    return {
        chapters: CAMPAIGN.map(m => ({ id: m.id, title: m.title, description: m.description || "" })),
        places: Object.keys(PLACES),
        unverified: UNVERIFIED,
        weather: Object.entries(WEATHER).map(([label, set]) => ({ label, set })),
        vehicles: VEHICLE_MODELS,
        npcs: npcs ? npcs.available : false,
    };
}

// ---------------------------------------------------------------- lifecycle

Events.on("resourceStart", (name) => {
    if (name !== RESOURCE) return;
    safe("resourceStart", () => {
        npcs = new NpcManager();
        console.log(`[COOP] resource started with ${CAMPAIGN.length} chapters, NPC API ${npcs.available ? "available" : "NOT available (enemies play as timers)"}`);

        runner = new MissionRunner(hud, {
            npcs,
            lobbySpawn: PLACES.lobby,
            extraState: () => {
                const host = hostId !== null ? runner.livePlayers().find(p => p.id === hostId) : null;
                return { host: host ? host.nickname : null, campaignIndex, chapters: CAMPAIGN.length, mode };
            },
            onMissionFinished: ({ index, result }) => {
                if (mode === "story") { resetReady(); return; }
                if (result === "complete") {
                    campaignIndex = Math.min(index + 1, CAMPAIGN.length);
                    if (campaignIndex >= CAMPAIGN.length) {
                        hud.say("Campaign complete. /start 1 to play it again, or /start <n> for any chapter.");
                        campaignIndex = 0;
                    } else {
                        hud.say(`Next: chapter ${campaignIndex + 1} - ${CAMPAIGN[campaignIndex].title}. /ready when you are.`);
                    }
                }
                resetReady();
                toLobby();
            },
        });

        if (globalThis.__COOP_TEST__) globalThis.__coopRunner = runner; // offline simulator hook (tools/sim)
        weaponSyncTimer = setInterval(() => safe("weaponSync", syncWeapons), 1000);
        console.log(`[COOP] mode: ${mode}`);
        if (mode === "campaign") toLobby();
        runner.livePlayers().forEach(p => onJoin(p));
    });
});

Events.on("resourceStop", (name) => {
    if (name !== RESOURCE) return;
    if (autoStartTimer) { clearTimeout(autoStartTimer); autoStartTimer = null; }
    if (weaponSyncTimer) { clearInterval(weaponSyncTimer); weaponSyncTimer = null; }
    if (runner) { safe("runner.dispose", () => runner.dispose()); runner = null; }
    if (npcs) { safe("npcs.dispose", () => npcs.dispose()); npcs = null; }
});

// ------------------------------------------------------------------ players

function onJoin(player) {
    runner.onPlayerConnect(player);
    if (!joinOrder.includes(player.id)) joinOrder.push(player.id);
    if (hostId === null || !runner.livePlayers().some(p => p.id === hostId)) {
        hostId = player.id;
        hud.tell(player, "You are the host. F6 or /start begins the chapter once the crew is ready.");
    }
    if (mode === "campaign" && runner.phase === "lobby") runner.placePlayer(player, PLACES.lobby, joinOrder.indexOf(player.id));
    if (mode === "story" && player.id !== hostId) { joinHost(player); giveStoryLoadout(player); }
    hud.help(player, mode === "story" ? [
        `[COOP] Welcome, ${player.nickname}. Story mode: the host plays the campaign, you ride along.`,
        "[COOP] /join puts you next to the host   /mirror shows what is streaming   /coophelp for commands",
    ] : [
        `[COOP] Welcome to the co-op story, ${player.nickname}.`,
        "[COOP] F4 panel   F5 /ready   F6 /start   /missions   /skip   /restart   /abort   /coophelp",
    ]);
    hud.say(`${player.nickname} joined the crew (${runner.livePlayers().length} online).`);
    sendPanelData(player);
}

function sendPanelData(player) {
    try { player.emit("coop:panelData", JSON.stringify(panelData())); } catch (e) { console.warn(`[COOP] panelData emit failed: ${e}`); }
}

Events.on("playerConnect", (player) => {
    if (!runner) return;
    safe("playerConnect", () => onJoin(player));
});

// The client script asks for its one-shot data after (re)starting, so a hot reload never leaves the panel empty.
Events.onClient("coop:hello", (basePlayer) => {
    if (!runner) return;
    safe("coop:hello", () => {
        const id = basePlayer && basePlayer.id;
        const p = runner.livePlayers().find(x => x.id === id);
        if (p) sendPanelData(p);
    });
});

Events.on("playerDisconnect", (player) => {
    if (!runner) return;
    safe("playerDisconnect", () => {
        weaponSeen.delete(player.id);
        hud.say(`${player.nickname} left the crew.`);
        const idx = joinOrder.indexOf(player.id);
        if (idx >= 0) joinOrder.splice(idx, 1);
        runner.onPlayerDisconnect(player);
        if (hostId === player.id) {
            const remaining = runner.livePlayers();
            hostId = joinOrder.find(id => remaining.some(p => p.id === id));
            if (hostId === undefined) hostId = null;
            const newHost = hostId !== null ? remaining.find(p => p.id === hostId) : null;
            if (newHost) hud.tell(newHost, "You are now the host.");
        }
        if (autoStartTimer && runner.livePlayers().length === 0) { clearTimeout(autoStartTimer); autoStartTimer = null; }
    });
});

Events.on("playerDied", (player) => {
    if (!runner) return;
    if (mode === "story") { hud.say(`${player.nickname} went down.`); return; } // the game handles death and checkpoints
    safe("playerDied", () => runner.onPlayerDied(player));
});

Events.on("humanDestroyed", (human) => { try { weaponSeen.delete(human.id); } catch (_) {} });

Events.on("humanDied", (human, killer) => {
    if (mode !== "story") return;
    try { console.log(`[COOP:MIRROR] human ${human.id} died${killer ? " to " + (killer.nickname || killer.id) : ""}`); } catch (_) {}
});

Events.on("vehiclePlayerEnter", (vehicle, player, seatIndex) => {
    if (!runner) return;
    safe("vehiclePlayerEnter", () => runner.onVehicleEnter(vehicle, player, seatIndex));
});

Events.on("vehiclePlayerLeave", (vehicle, player) => {
    if (!runner) return;
    safe("vehiclePlayerLeave", () => runner.onVehicleLeave(vehicle, player));
});

Events.on("chatMessage", (player, message) => {
    safe("chatMessage", () => Chat.sendToAll(`<${player.nickname}>: ${message}`));
});

// ----------------------------------------------------------------- campaign

function isHost(player) {
    return hostId === null || player.id === hostId;
}

function giveStoryLoadout(player) {
    for (const [id, ammo] of STORY_LOADOUT) { try { player.addWeapon(id, ammo); } catch (_) {} }
}

// Story mode weapon sync. Weapon *selection* replicates by itself, but a ped can only show a weapon its
// own inventory holds on every client, and the game hands weapons to the host and to its NPCs locally.
// Whenever a player's or NPC's current weapon changes, hand that weapon out through the server (ammo 0:
// the owner already has the real ammo; observers only need the item to render and replay shots).
const weaponSeen = new Map(); // entity id -> Set of weapon ids already broadcast
let weaponSyncTimer = null;
function syncWeapons() {
    if (mode !== "story") return;
    const visit = (h) => {
        let id, weapon;
        try { id = h.id; weapon = h.weaponId; } catch (_) { return; }
        if (!weapon) return;
        let seen = weaponSeen.get(id);
        if (!seen) { seen = new Set(); weaponSeen.set(id, seen); }
        if (seen.has(weapon)) return;
        seen.add(weapon);
        try { h.addWeapon(weapon, 0); } catch (e) { console.warn(`[COOP] weapon sync ${id}/${weapon}: ${e}`); }
    };
    try { World.players.forEach(visit); } catch (_) {}
    try { if (World.humans) World.humans.forEach(visit); } catch (_) {}
}

/** Story mode: put a player next to the host, where the mirrored world is streamed. */
function joinHost(player) {
    const host = hostId !== null ? runner.livePlayers().find(p => p.id === hostId) : null;
    if (!host || host.id === player.id) return false;
    try {
        const p = host.position;
        const slot = Math.max(1, joinOrder.indexOf(player.id));
        runner.placePlayer(player, { pos: p }, slot, 2.5);
        return true;
    } catch (e) {
        console.warn(`[COOP] joinHost failed: ${e}`);
        return false;
    }
}

function resetReady() {
    for (const r of runner.players.values()) r.ready = false;
}

function toLobby() {
    runner.applyEnvironment({ weather: WEATHER.day, time: 17.5 });
    runner.placeTeam(PLACES.lobby);
}

function startChapter(index, byPlayer) {
    if (!runner) return;
    if (mode === "story") {
        if (byPlayer) hud.tell(byPlayer, "Story mode: the host plays the real campaign. /mode campaign for the scripted chapters.");
        return;
    }
    if (runner.phase !== "lobby") {
        if (byPlayer) hud.tell(byPlayer, "A mission is already running. /abort first.");
        return;
    }
    if (index < 0 || index >= CAMPAIGN.length) {
        if (byPlayer) hud.tell(byPlayer, `Chapters are 1-${CAMPAIGN.length}.`);
        return;
    }
    if (runner.livePlayers().length < MIN_PLAYERS) {
        if (byPlayer) hud.tell(byPlayer, `Need at least ${MIN_PLAYERS} player(s).`);
        return;
    }
    campaignIndex = index;
    hud.fade("out", 0.8);
    setTimeout(() => safe("startChapter", () => {
        if (!runner || runner.phase !== "lobby" || runner.livePlayers().length === 0) return;
        runner.start(CAMPAIGN[index], index);
    }), 900);
}

function maybeAutoStart() {
    if (!AUTO_START_WHEN_ALL_READY || runner.phase !== "lobby") return;
    const players = runner.livePlayers();
    if (players.length < MIN_PLAYERS) return;
    const allReady = players.every(p => runner.record(p).ready);
    if (!allReady) return;
    if (autoStartTimer) clearTimeout(autoStartTimer);
    hud.say(`Everyone is ready. Chapter ${campaignIndex + 1} starts in 5 seconds.`);
    hud.countdown(5);
    autoStartTimer = setTimeout(() => safe("autoStart", () => {
        autoStartTimer = null;
        if (!runner) return;
        const live = runner.livePlayers();
        if (live.length && live.every(p => runner.record(p).ready)) startChapter(campaignIndex, null);
    }), 5000);
}

// ----------------------------------------------------------------- commands

const COMMANDS = {};
function command(name, fn, help) { COMMANDS[name] = { fn, help }; }

command("coophelp", (player) => {
    hud.help(player, [
        "[COOP] /ready - toggle ready (F5)   /start [n] - start chapter n (F6, host)",
        "[COOP] /missions - list chapters   /next - start the next chapter",
        "[COOP] /skip - skip objective (host)   /restart - restart from checkpoint (host)   /abort - back to lobby (host)",
        "[COOP] /wai - your position   /mark <name> - print a marker   /route start|add|undo|dump|stop",
        "[COOP] /veh <model> - spawn a car   /heal   /tp <place>   /time <h>   /weather <set>",
        "[COOP] /npc <n> [ally] - test NPCs   /npcclear   /npcmodels pool|tommy   /places",
        "[COOP] /mode story|campaign (host)   /mirror - what the story host is streaming   /join   /guns   /unstick",
    ]);
}, "show help");

command("join", (player) => {
    if (mode !== "story") return hud.tell(player, "/join is for story mode (the host's campaign).");
    if (isHost(player)) return hud.tell(player, "You are the host; the others join you.");
    hud.tell(player, joinHost(player) ? "Teleported to the host." : "The host is not in game yet.");
}, "teleport to the story host");

command("guns", (player) => {
    if (mode !== "story") return hud.tell(player, "Weapons come with the chapter in campaign mode.");
    giveStoryLoadout(player);
    hud.tell(player, "Pistol and Thompson handed out.");
}, "story mode: get the guest loadout again");

command("unstick", (player) => {
    // Client-side: release every lock/focus the co-op script could hold and log the input state.
    try { player.emit("coop:unstick", JSON.stringify({})); } catch (_) {}
    hud.tell(player, "Releasing panel, overlay focus and control lock on your client.");
}, "release stuck controls");

command("ready", (player) => {
    if (mode === "story") return hud.tell(player, "Story mode: nothing to ready up for. /join puts you next to the host.");
    const r = runner.record(player);
    r.ready = !r.ready;
    hud.say(`${player.nickname} is ${r.ready ? "ready" : "not ready"}.`);
    maybeAutoStart();
}, "toggle ready");

command("start", (player, args) => {
    if (!isHost(player)) return hud.tell(player, "Only the host can start. Ask them, or /ready.");
    const n = args[0] ? parseInt(args[0], 10) - 1 : campaignIndex;
    if (Number.isNaN(n)) return hud.tell(player, "Usage: /start [chapter number]");
    startChapter(n, player);
}, "start a chapter");

command("next", (player) => {
    if (!isHost(player)) return hud.tell(player, "Only the host can do that.");
    startChapter(campaignIndex, player);
}, "start the next chapter");

command("missions", (player) => {
    CAMPAIGN.forEach((m, i) => {
        const marker = i === campaignIndex ? ">" : " ";
        hud.tell(player, `${marker} ${i + 1}. ${m.title} - ${m.description || ""}`);
    });
}, "list chapters");

command("skip", (player) => {
    if (!isHost(player)) return hud.tell(player, "Only the host can skip.");
    if (!runner.skipStep()) hud.tell(player, "No mission running.");
}, "skip the current objective");

command("restart", (player) => {
    if (!isHost(player)) return hud.tell(player, "Only the host can restart.");
    if (!runner.restart()) hud.tell(player, "No mission running.");
}, "restart from checkpoint");

command("abort", (player) => {
    if (!isHost(player)) return hud.tell(player, "Only the host can abort.");
    if (runner.phase === "lobby") return hud.tell(player, "No mission running.");
    runner.abort(`by ${player.nickname}`);
    resetReady();
}, "abort the mission");

command("wai", (player) => {
    const pos = player.position, rot = player.rotation;
    hud.tell(player, `pos ${V.fmt(pos)}  rot ${rot.w.toFixed(3)}, ${rot.x.toFixed(3)}, ${rot.y.toFixed(3)}, ${rot.z.toFixed(3)}`);
    console.log(`[COOP] ${player.nickname} pos ${V.fmt(pos)} rot ${rot.w}, ${rot.x}, ${rot.y}, ${rot.z}`);
}, "where am I");

command("mark", (player, args) => cmdMark(player, args), "record a marker");
command("route", (player, args) => cmdRoute(player, args), "record a route");

command("heal", (player) => { player.health = 100.0; hud.tell(player, "Healed."); }, "heal");

command("veh", (player, args) => {
    const model = args[0] || "bolt_model_b";
    if (!VEHICLE_MODELS.includes(model)) return hud.tell(player, `Unknown vehicle model ${model}. Known: ${VEHICLE_MODELS.slice(0, 12).join(", ")}...`);
    const veh = World.createVehicle(model);
    if (!veh) return hud.tell(player, `Could not spawn ${model}.`);
    const p = player.position;
    // Well clear of the player (buses and trucks are long), on the ground plane.
    veh.position = V.v3(p.x + 8, p.y + 3, p.z + 0.3);
    veh.rotation = player.rotation;
    veh.licensePlate = "CO-veh";
    hud.tell(player, `Spawned ${model} a few metres away.`);
}, "spawn a vehicle");

command("tp", (player, args) => {
    const name = args[0];
    const place = name && PLACES[name];
    if (!place) return hud.tell(player, "Places: " + Object.keys(PLACES).join(", "));
    runner.placePlayer(player, place);
    hud.tell(player, `Teleported to ${name}${UNVERIFIED.includes(name) ? " (stand-in location, see /places)" : ""}.`);
}, "teleport");

command("places", (player) => {
    hud.tell(player, `Verified: ${Object.keys(PLACES).filter(k => !UNVERIFIED.includes(k)).join(", ")}`);
    hud.tell(player, `Stand-ins (record the real spot with /mark <name>): ${UNVERIFIED.join(", ")}`);
}, "list places");

command("time", (player, args) => {
    const t = parseFloat(args[0]);
    if (Number.isNaN(t) || t < 0 || t > 24) return hud.tell(player, "Usage: /time <0-24>");
    World.setDayTimeHours(t);
}, "set time");

command("weather", (player, args) => {
    const set = args[0];
    if (!set) return hud.tell(player, "Sets: " + Object.values(WEATHER).join(", "));
    World.setWeatherSet(set);
}, "set weather");

command("npc", (player, args) => {
    if (!npcs || !npcs.available) return hud.tell(player, "This server build has no NPC API.");
    const n = Math.min(8, Math.max(1, parseInt(args[0] || "2", 10) || 2));
    const { ring } = require("./lib/npcs.js");
    const defs = ring(player.position, n, 8, { role: args[1] === "ally" ? "ally" : "enemy", weapon: [2, 60] });
    const made = npcs.spawnMany(defs, "test");
    hud.tell(player, `Spawned ${made.length} test ${args[1] === "ally" ? "allies" : "enemies"}. /npcclear removes them.`);
}, "spawn test NPCs");

command("npcmodels", (player, args) => {
    if (!npcs) return;
    const mode = (args[0] || "").toLowerCase();
    if (mode !== "pool" && mode !== "tommy") return hud.tell(player, `NPC models: ${npcs.modelMode}. Usage: /npcmodels pool|tommy (tommy = the one verified model, use it if enemies are invisible)`);
    npcs.modelMode = mode;
    hud.say(`NPC models set to ${mode} by ${player.nickname}.`);
}, "choose NPC models");

command("mode", (player, args) => {
    if (!isHost(player)) return hud.tell(player, "Only the host can switch modes.");
    const m = (args[0] || "").toLowerCase();
    if (m !== "story" && m !== "campaign") return hud.tell(player, `Mode: ${mode}. Usage: /mode story|campaign`);
    if (m === mode) return hud.tell(player, `Already in ${mode} mode.`);
    if (runner.phase !== "lobby") runner.abort("mode change");
    mode = m;
    hud.say(`Mode switched to ${mode} by ${player.nickname}.`);
    if (mode === "campaign") toLobby();
}, "switch story/campaign mode");

command("mirror", (player) => {
    let humans = 0, vehicles = 0, dead = 0;
    try { World.humans.forEach(h => { humans++; try { if (h.dead) dead++; } catch (_) {} }); } catch (_) {}
    try { vehicles = World.vehicles.length; } catch (_) {}
    hud.tell(player, `Mirrored: ${humans} humans (${dead} dead), ${vehicles} vehicles, ${runner.livePlayers().length} players. Mode ${mode}.`);
}, "mirror status");

command("npcclear", (player) => {
    if (!npcs) return;
    npcs.despawnGroup("test");
    hud.tell(player, "Test NPCs removed.");
}, "remove test NPCs");

Events.on("chatCommand", (player, message, cmd, args) => {
    if (!runner) return;
    const entry = COMMANDS[cmd];
    if (!entry) return safe("unknown-cmd", () => hud.tell(player, `Unknown command /${cmd}. Try /coophelp.`));
    try {
        entry.fn(player, args || []);
    } catch (e) {
        console.warn(`[COOP] /${cmd} failed: ${e}\n${e && e.stack}`);
        safe("cmd-error-reply", () => hud.tell(player, `/${cmd} failed: ${e}`));
    }
});

console.log("[COOP] === coop-story loaded ===");
