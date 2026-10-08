/**
 * Mission runner: a server-authoritative state machine that drives one
 * co-op mission at a time.
 *
 * A mission is plain data (see server/missions/*.js):
 *   {
 *     id, title, subtitle, description,
 *     environment: { weather, time },
 *     spawn: { pos, rot },                       // where the team starts
 *     vehicles: { tag: { model, pos, rot } },   // tagged mission vehicles
 *     weapons: [[weaponId, ammo], ...],
 *     steps: [ step, step, ... ],
 *   }
 *
 * Step types:
 *   board      { vehicle }                                   everyone sits in the tagged vehicle
 *   goto       { target, radius, who: "all"|"any", vehicle?, timeLimit? }
 *   wait       { seconds, countdown? }
 *   survive    { seconds, stayIn? }                          timer; leaving `stayIn` for >10 s fails
 *   together   { target, radius, maxSpread, graceSeconds, noVehicles? }
 *   convoy     { vehicles: [tag], target, radius }           each tagged car driven to the target
 *   race       { gates: [pos], laps, vehicles: [tag], radius }
 *   combat     { enemies: [npc], waves: [{ at, enemies }], kills? }   kill them all (or `kills`)
 *   defend     { seconds, area?, radius?, waves: [{ at, enemies }] }  hold out; enemies keep coming
 *   escort     { ally: npc, target, radius, enemies? }       the ally must reach the target alive
 *   stealth    { guards: [npc], target, radius, detectRadius, onAlarm: "combat"|"fail" }
 *   killTarget { target: npc, bodyguards: [npc], radius? }   kill the marked man
 *   chase      { runner: npc, path: [pos], catchRadius, escapeFails? }  catch the runner before the path ends
 *
 * Any step may carry `enemies`, `allies`, `guards` (arrays of NPC defs, or a
 * function(runner) returning one). They spawn when the step starts and are
 * removed when it ends, unless `keepNpcs: true`.
 *
 * Common optional fields: objective, hint, checkpoint, checkpointSpawn,
 * environment, teleport (team spawn), resetVehicles (back to their parking
 * spot), summonVehicles (next to the crew if far away), outroTitle, outro,
 * failText, timeLimit, onEnter(runner), onComplete(runner), fallbackSeconds
 * (combat without NPCs).
 */

const V = require("./vec.js");

const TICK_MS = 100;
const DOWN_SECONDS = 4;          // respawn delay after a death
const RESPAWN_GRACE_MS = 20000;  // no spread/vehicle checks against a player for this long after a respawn
const STAY_IN_GRACE_MS = 10000;
const SEAT_TRUST_MS = 6000;      // enter-event seat info is trusted this long without replication confirming it
const SEAT_TRUST_DIST = 7;       // ...or while the player is this close to the car
const NPC_STEP_MAX_SECONDS = 420; // combat/killTarget steps auto-complete after this (missing NPC model safety net)
const END_SCREEN_MS = 6000;
const COMPLETE_SCREEN_MS = 7000;
const PLATE_PREFIX = "CO-";

class MissionRunner {
    constructor(hud, opts = {}) {
        this.hud = hud;
        this.npcs = opts.npcs || null;
        this.now = opts.now || (() => Date.now());
        this.timers = opts.timers || {
            setTimeout: (f, ms) => setTimeout(f, ms), clearTimeout: (h) => clearTimeout(h),
            setInterval: (f, ms) => setInterval(f, ms), clearInterval: (h) => clearInterval(h),
        };
        this.log = opts.log || ((m) => console.log(`[COOP] ${m}`));
        this.warn = opts.warn || ((m) => console.warn(`[COOP] ${m}`));
        this.phase = "lobby";       // lobby | running | ended
        this.mission = null;
        this.missionIndex = -1;
        this.stepIndex = -1;
        this.step = null;
        this.stepStart = 0;
        this.scratch = {};
        this.checkpoint = null;     // { stepIndex, spawn }
        this.vehicles = new Map();  // tag -> { handle, def, tag }
        this.players = new Map();   // player.id -> record
        this.leaving = new Set();   // ids mid-disconnect (still listed by World.players)
        this.seats = new Map();     // player.id -> { vehicleId, at }
        this.endTimer = null;
        this.gen = 0;               // bumped on every mission transition; stale timers check it
        this.onMissionFinished = opts.onMissionFinished || (() => {});
        this.lobbySpawn = opts.lobbySpawn;
        this.extraState = opts.extraState || (() => ({}));
        this._tickHandle = this.timers.setInterval(() => this._safe(() => this.tick(), "tick"), TICK_MS);
        this._hudHandle = this.timers.setInterval(() => this._safe(() => this.pushHud(), "hud"), 1000);
        this.cleanupStrayVehicles();
    }

    dispose() {
        this.timers.clearInterval(this._tickHandle);
        this.timers.clearInterval(this._hudHandle);
        if (this.endTimer) this.timers.clearTimeout(this.endTimer);
        this.gen++;
        this.destroyVehicles();
        if (this.npcs) this.npcs.despawnAll();
    }

    _safe(fn, what) {
        try { fn(); } catch (e) { this.warn(`${what} error: ${e}\n${e && e.stack}`); }
    }

    /** A timer that is ignored if the mission moved on (start/abort/restart) before it fires. */
    later(fn, ms) {
        const gen = this.gen;
        return this.timers.setTimeout(() => { if (gen === this.gen) this._safe(fn, "timer"); }, ms);
    }

    // ---------------------------------------------------------------- players

    record(player) {
        let r = this.players.get(player.id);
        if (!r) {
            r = { id: player.id, name: safeName(player), ready: false, downUntil: 0, graceUntil: 0, deaths: 0, race: null, seatedByReplication: false };
            this.players.set(player.id, r);
        }
        return r;
    }

    forget(player) {
        this.players.delete(player.id);
        this.seats.delete(player.id);
    }

    /** Live player handles: connected, not mid-disconnect. */
    livePlayers() {
        const out = [];
        try {
            World.players.forEach(p => {
                try { if (p && !this.leaving.has(p.id)) out.push(p); } catch (_) {}
            });
        } catch (_) {}
        return out;
    }

    isDown(player) {
        const r = this.players.get(player.id);
        return !!r && r.downUntil > this.now();
    }

    inGrace(player) {
        const r = this.players.get(player.id);
        return !!r && r.graceUntil > this.now();
    }

    activePlayers() {
        return this.livePlayers().filter(p => !this.isDown(p));
    }

    // --------------------------------------------------------------- vehicles

    plateFor(tag) {
        return plateFor(tag);
    }

    /** Vehicles left behind by a previous instance of the resource (hot reload). */
    cleanupStrayVehicles() {
        try {
            const stray = [];
            World.vehicles.forEach(v => { try { if (String(v.licensePlate || "").startsWith(PLATE_PREFIX)) stray.push(v); } catch (_) {} });
            let removed = 0;
            const seated = new Set();
            try { World.players.forEach(p => { try { const v = p.vehicle; if (v) seated.add(v.id); } catch (_) {} }); } catch (_) {}
            for (const v of stray) {
                if (seated.has(safeId(v))) continue;
                if (typeof v.destroy === "function") { try { v.destroy(); removed++; } catch (_) {} }
            }
            if (stray.length) this.log(`stray mission vehicles: ${stray.length}, removed ${removed}`);
        } catch (_) {}
    }

    ensureVehicle(tag, def) {
        let entry = this.vehicles.get(tag);
        if (entry) {
            let ok = false;
            try { ok = entry.handle.modelName === def.model; } catch (_) { ok = false; }
            if (ok) { entry.def = def; this.resetVehicle(tag); return entry; }
            this.destroyVehicle(tag);
        }
        let handle = null;
        try { handle = World.createVehicle(def.model); } catch (e) { this.warn(`createVehicle(${def.model}) threw: ${e}`); }
        if (!handle) { this.warn(`could not create vehicle ${def.model} (${tag})`); return null; }
        entry = { handle, def, tag };
        this.vehicles.set(tag, entry);
        this.resetVehicle(tag);
        return entry;
    }

    resetVehicle(tag, pos, rot) {
        const entry = this.vehicles.get(tag);
        if (!entry) return;
        try {
            entry.handle.position = pos || entry.def.pos;
            entry.handle.rotation = rot || entry.def.rot || V.yaw(0);
            entry.handle.engineOn = false;
            entry.handle.fuel = 100;
            entry.handle.lockState = 0;
            entry.handle.licensePlate = this.plateFor(tag);
        } catch (e) {
            this.warn(`resetVehicle(${tag}) failed: ${e}`);
        }
    }

    /** Brings a mission vehicle next to the crew (after a fight the car is "where you left it"), unless it already is. */
    summonVehicle(tag) {
        const entry = this.vehicles.get(tag);
        const anchor = this.activePlayers()[0] || this.livePlayers()[0];
        if (!entry || !anchor) return;
        try {
            const p = anchor.position;
            if (V.dist2(p, entry.handle.position) <= 40) return;
            entry.handle.position = V.v3(p.x + 6, p.y + 2, p.z);
            entry.handle.rotation = entry.def.rot || V.yaw(0);
            entry.handle.engineOn = false;
            entry.handle.fuel = 100;
            entry.handle.lockState = 0;
        } catch (e) {
            this.warn(`summonVehicle(${tag}) failed: ${e}`);
        }
    }

    /** True if any connected player sits in this mission vehicle (never destroy a car with people in it). */
    vehicleOccupied(entry) {
        const id = safeId(entry.handle);
        if (id === null) return false;
        return this.livePlayers().some(p => this.vehicleIdOf(p) === id);
    }

    destroyVehicle(tag) {
        const entry = this.vehicles.get(tag);
        if (!entry) return;
        if (this.vehicleOccupied(entry)) {
            // Keep it: the stray-vehicle sweep on the next resource start removes it once empty.
            this.resetVehicleState(entry);
            this.vehicles.delete(tag);
            return;
        }
        this.vehicles.delete(tag);
        try {
            if (typeof entry.handle.destroy === "function") entry.handle.destroy();
            else this.resetVehicle(tag);
        } catch (_) {}
    }

    resetVehicleState(entry) {
        try { entry.handle.engineOn = false; entry.handle.fuel = 100; entry.handle.lockState = 0; } catch (_) {}
    }

    destroyVehicles() {
        for (const tag of [...this.vehicles.keys()]) this.destroyVehicle(tag);
    }

    /** Network id of the vehicle a player sits in, or 0. Replication is the authority; the server's
     *  own enter event is only trusted briefly, since no leave event is sent for interrupted entries. */
    vehicleIdOf(player) {
        const r = this.record(player);
        let replicated = 0;
        try { const v = player.vehicle; replicated = v ? v.id : 0; } catch (_) {}
        if (replicated) { r.seatedByReplication = true; return replicated; }
        if (r.seatedByReplication) { r.seatedByReplication = false; this.seats.delete(player.id); return 0; }
        const seat = this.seats.get(player.id);
        if (!seat) return 0;
        if (this.now() - seat.at < SEAT_TRUST_MS) return seat.vehicleId;
        let near = false;
        try {
            const entry = [...this.vehicles.values()].find(e => safeId(e.handle) === seat.vehicleId);
            if (entry) near = V.dist3(player.position, entry.handle.position) < SEAT_TRUST_DIST;
        } catch (_) {}
        if (!near) { this.seats.delete(player.id); return 0; }
        return seat.vehicleId;
    }

    vehicleOf(player) {
        const id = this.vehicleIdOf(player);
        if (!id) return null;
        for (const entry of this.vehicles.values()) if (safeId(entry.handle) === id) return entry.handle;
        try { return player.vehicle || null; } catch (_) { return null; }
    }

    playerInTagged(player, tag) {
        const entry = this.vehicles.get(tag);
        if (!entry) return false;
        const entryId = safeId(entry.handle);
        return entryId !== null && this.vehicleIdOf(player) === entryId;
    }

    tagOfVehicle(veh) {
        const id = safeId(veh);
        if (id === null) return null;
        for (const [tag, entry] of this.vehicles) if (safeId(entry.handle) === id) return tag;
        return null;
    }

    // ---------------------------------------------------------------- control

    start(mission, index) {
        if (this.endTimer) { this.timers.clearTimeout(this.endTimer); this.endTimer = null; }
        this.gen++;
        this.mission = mission;
        this.missionIndex = index;
        this.phase = "running";
        this.checkpoint = null;
        this.scratch = {};
        for (const r of this.players.values()) { r.downUntil = 0; r.graceUntil = 0; r.race = null; }

        this.log(`starting mission ${mission.id}: ${mission.title}`);
        this.applyEnvironment(mission.environment);
        this.destroyVehicles();
        this.cleanupStrayVehicles();
        for (const [tag, def] of Object.entries(mission.vehicles || {})) this.ensureVehicle(tag, def);
        this.placeTeam(mission.spawn);
        this.giveWeapons();

        this.hud.fade("in", 1.0);
        this.hud.title(mission.title, mission.subtitle || "");
        this.hud.say(`Chapter ${index + 1}: ${mission.title}`);
        if (mission.description) this.hud.say(mission.description);

        this.enterStep(0);
    }

    abort(reason) {
        if (this.phase === "lobby") return;
        if (this.endTimer) { this.timers.clearTimeout(this.endTimer); this.endTimer = null; }
        this.gen++;
        const finished = { mission: this.mission, index: this.missionIndex, result: "aborted" };
        this.leaveStep();
        this.phase = "lobby";
        this.step = null;
        this.hud.hideTitle();
        this.hud.say(`Mission aborted${reason ? ": " + reason : ""}.`);
        this.destroyVehicles();
        this.cleanupStrayVehicles();
        this.placeTeam(this.lobbySpawn);
        this.onMissionFinished(finished);
    }

    skipStep() {
        if (this.phase !== "running") return false;
        this.hud.say(`Skipping objective ${this.stepIndex + 1}.`);
        this.completeStep();
        return true;
    }

    /** Host-requested restart from the checkpoint (no "failed" screen). */
    restart() {
        if (this.phase !== "running") return false;
        this.phase = "ended";
        this.gen++;
        this.leaveStep();
        this.step = null;
        this.hud.missionEnd("RESTART", "Back to the last checkpoint", 3);
        this.endTimer = this.later(() => this.restartFromCheckpoint(), 3500);
        return true;
    }

    applyEnvironment(env) {
        if (!env) return;
        try {
            if (env.weather) World.setWeatherSet(env.weather);
            if (typeof env.time === "number") World.setDayTimeHours(env.time);
        } catch (e) {
            this.warn(`environment failed: ${e}`);
        }
    }

    placeTeam(spawn, spread = 2.0) {
        if (!spawn) return;
        this.livePlayers().forEach((p, i) => this.placePlayer(p, spawn, i, spread));
    }

    placePlayer(player, spawn, slot = 0, spread = 2.0) {
        try {
            const offset = slot === 0 ? { x: 0, y: 0 } : { x: ((slot % 2) ? spread : -spread) * Math.ceil(slot / 2), y: (slot > 2 ? spread : 0) };
            player.position = V.v3(spawn.pos.x + offset.x, spawn.pos.y + offset.y, spawn.pos.z);
            if (spawn.rot) player.rotation = spawn.rot;
            player.health = 100.0;
        } catch (e) {
            this.warn(`placePlayer failed: ${e}`);
        }
    }

    /** Where a player who comes back mid-mission should appear: next to the crew, or the current car. */
    rejoinSpawn(exclude) {
        const s = this.step;
        if (s) {
            const tag = s.vehicle || s.stayIn || (s.type === "board" ? s.vehicle : null);
            const entry = tag ? this.vehicles.get(tag) : null;
            if (entry) { try { return { pos: V.add(entry.handle.position, 2.5, 2.5) }; } catch (_) {} }
        }
        const mates = this.activePlayers().filter(p => !exclude || p.id !== exclude.id);
        if (mates.length) {
            try { return { pos: V.add(mates[0].position, 1.5, 1.5) }; } catch (_) {}
        }
        return (this.checkpoint && this.checkpoint.spawn) || (this.mission && this.mission.spawn) || this.lobbySpawn;
    }

    giveWeapons(player) {
        const list = (this.mission && this.mission.weapons) || [];
        const targets = player ? [player] : this.livePlayers();
        for (const p of targets) {
            for (const [id, ammo] of list) { try { p.addWeapon(id, ammo); } catch (_) {} }
        }
    }

    // ------------------------------------------------------------------ steps

    stepGroup(i) { return `step:${i}`; }

    enterStep(i) {
        const steps = this.mission.steps;
        if (i >= steps.length) return this.complete();
        this.stepIndex = i;
        this.step = steps[i];
        this.stepStart = this.now();
        this.scratch = { warned: false, leftAt: new Map(), arrivedVehicles: new Set(), firstArrival: 0, wavesSpawned: 0, kills: 0, alarm: false, chasePoint: 0 };

        const s = this.step;
        if (s.checkpoint) {
            this.checkpoint = { stepIndex: i, spawn: s.checkpointSpawn || (s.target ? { pos: s.target } : this.mission.spawn) };
            this.hud.banner("CHECKPOINT", "Progress saved");
        }
        if (s.environment) this.applyEnvironment(s.environment);
        if (s.teleport) this.placeTeam(s.teleport);
        if (s.resetVehicles) for (const tag of s.resetVehicles) this.resetVehicle(tag);
        if (s.summonVehicles) for (const tag of s.summonVehicles) this.summonVehicle(tag);
        if (s.type === "race") for (const r of this.players.values()) r.race = { gate: 0, lap: 0, finished: false, finishTime: 0 };
        if (s.type === "wait" && s.countdown) this.hud.countdown(Math.round(s.seconds));
        if (s.type === "survive" || s.type === "defend") this.hud.countdown(Math.round(s.seconds));

        this.spawnStepNpcs(s, i);
        if (s.onEnter) { try { s.onEnter(this); } catch (e) { this.warn(`onEnter threw: ${e}`); } }

        this.hud.objective(s.objective || `Objective ${i + 1}`, s.hint || "");
        this.log(`step ${i + 1}/${steps.length}: ${s.type} - ${s.objective || ""}`);
    }

    spawnStepNpcs(s, i) {
        if (!this.npcs) return;
        const group = this.stepGroup(i);
        const defs = (list, role) => (typeof list === "function" ? list(this) : list || []).map(d => ({ role, ...d }));
        let list = [...defs(s.enemies, "enemy"), ...defs(s.allies, "ally"), ...defs(s.guards, "guard")];
        switch (s.type) {
            case "combat": list = list.concat(defs(s.enemies === undefined ? [] : [], "enemy")); break;
            case "stealth": list = list.concat(defs(s.guards === undefined ? [] : [], "guard")); break;
            case "escort": if (s.ally) list.push({ role: "ally", tag: "escort", ...s.ally }); break;
            case "killTarget":
                if (s.target) list.push({ role: "target", tag: "mark", ...s.target });
                list = list.concat(defs(s.bodyguards, "enemy"));
                break;
            case "chase": if (s.runner) list.push({ role: "target", tag: "runner", fleeRadius: 0, ...s.runner }); break;
            default: break;
        }
        if (!this.npcs.available) {
            if (list.length) this.log(`NPC API unavailable: ${list.length} NPC(s) of step ${i + 1} skipped (fallback rules apply)`);
            return;
        }
        this.npcs.spawnMany(list, group);
        this.npcs.onDeath = (rec, killer) => this.onNpcDeath(rec, killer);
        if (s.type === "chase") this.scratch.chasePoint = 0;
    }

    leaveStep() {
        if (!this.npcs || !this.step) return;
        if (!this.step.keepNpcs) this.npcs.despawnGroup(this.stepGroup(this.stepIndex));
        this.npcs.onDeath = null;
    }

    completeStep() {
        const s = this.step;
        if (!s) return;
        if (s.onComplete) { try { s.onComplete(this); } catch (e) { this.warn(`onComplete threw: ${e}`); } }
        if (s.outro) this.hud.banner(s.outroTitle || "OBJECTIVE COMPLETE", s.outro);
        this.leaveStep();
        this.enterStep(this.stepIndex + 1);
    }

    complete() {
        this.phase = "ended";
        this.gen++;
        this.step = null;
        if (this.npcs) this.npcs.despawnAll();
        this.hud.missionEnd("MISSION COMPLETE", this.mission.title, 6);
        this.hud.say(`Mission complete: ${this.mission.title}`);
        const finished = { mission: this.mission, index: this.missionIndex, result: "complete" };
        this.endTimer = this.later(() => {
            this.endTimer = null;
            this.phase = "lobby";
            this.destroyVehicles();
            this.cleanupStrayVehicles();
            this.hud.fade("in", 1.0);
            this.onMissionFinished(finished);
        }, COMPLETE_SCREEN_MS);
    }

    fail(reason) {
        if (this.phase !== "running") return;
        this.phase = "ended";
        this.gen++;
        this.leaveStep();
        this.step = null;
        this.hud.missionEnd("MISSION FAILED", reason || "", 5);
        this.hud.say(`Mission failed: ${reason || "unknown"}`);
        this.log(`mission failed at step ${this.stepIndex + 1}: ${reason}`);
        this.endTimer = this.later(() => { this.endTimer = null; this.restartFromCheckpoint(); }, END_SCREEN_MS);
    }

    restartFromCheckpoint() {
        if (this.livePlayers().length === 0) {
            this.log("nobody left to restart for; back to lobby");
            this.phase = "running"; // abort() expects a non-lobby phase
            return this.abort("everyone left");
        }
        const cp = this.checkpoint;
        this.phase = "running";
        this.gen++;
        for (const r of this.players.values()) { r.downUntil = 0; r.graceUntil = 0; }
        const spawn = cp ? cp.spawn : this.mission.spawn;
        this.hud.fade("in", 1.0);
        this.placeTeam(spawn);
        this.giveWeapons();
        for (const tag of Object.keys(this.mission.vehicles || {})) {
            if (!cp || (cp.resetVehicles !== false && !this.vehicles.has(tag))) this.ensureVehicle(tag, this.mission.vehicles[tag]);
        }
        if (!cp) for (const tag of this.vehicles.keys()) this.resetVehicle(tag);
        this.hud.say(cp ? `Restarting from checkpoint (objective ${cp.stepIndex + 1}).` : "Restarting mission.");
        this.enterStep(cp ? cp.stepIndex : 0);
    }

    // ----------------------------------------------------------------- events

    onPlayerConnect(player) {
        const r = this.record(player);
        r.name = safeName(player);
        this.leaving.delete(player.id);
        if (this.phase === "running" || this.phase === "ended") {
            const spawn = this.rejoinSpawn(player);
            this.placePlayer(player, spawn, 1);
            this.giveWeapons(player);
            r.graceUntil = this.now() + RESPAWN_GRACE_MS;
            this.hud.tell(player, `Joined mid-mission: ${this.mission.title}`);
            if (this.step && this.step.type === "race") r.race = { gate: 0, lap: 0, finished: false, finishTime: 0 };
        }
    }

    /** Called while the player is still listed by World.players. */
    onPlayerDisconnect(player) {
        this.leaving.add(player.id);
        this.forget(player);
        const id = player.id;
        this.timers.setTimeout(() => this.leaving.delete(id), 10000);
        const remaining = this.livePlayers();
        if (this.phase !== "lobby" && remaining.length === 0) this.abort("everyone left");
    }

    onPlayerDied(player) {
        const r = this.record(player);
        r.deaths++;
        r.downUntil = this.now() + DOWN_SECONDS * 1000;
        this.seats.delete(player.id);
        r.seatedByReplication = false;

        this.hud.say(`${safeName(player)} went down.`);
        const gen = this.gen;
        this.timers.setTimeout(() => this._safe(() => {
            if (!this.livePlayers().some(p => p.id === player.id)) return;
            const spawn = (this.phase === "running" && gen === this.gen) ? this.rejoinSpawn(player) : (this.phase === "lobby" ? this.lobbySpawn : this.rejoinSpawn(player));
            this.placePlayer(player, spawn, 1);
            r.graceUntil = this.now() + RESPAWN_GRACE_MS;
            if (this.phase === "running") this.giveWeapons(player);
        }, "respawn"), DOWN_SECONDS * 1000);

        if (this.phase === "running") {
            const alive = this.livePlayers().filter(p => !this.isDown(p) && p.id !== player.id);
            if (alive.length === 0) this.fail("The whole crew went down.");
        }
    }

    onVehicleEnter(vehicle, player, seat) {
        const id = safeId(vehicle);
        if (id !== null) this.seats.set(player.id, { vehicleId: id, at: this.now() });
        try { vehicle.engineOn = true; } catch (_) {}
        this.log(`${safeName(player)} entered vehicle ${id} seat ${seat} (${this.tagOfVehicle(vehicle) || "untracked"})`);
    }

    onVehicleLeave(vehicle, player) {
        this.seats.delete(player.id);
        this.log(`${safeName(player)} left vehicle ${safeId(vehicle)}`);
    }

    onNpcDeath(rec, killer) {
        this.scratch.kills = (this.scratch.kills || 0) + 1;
        const s = this.step;
        if (!s) return;
        if (killer) this.hud.tell(killer, rec.role === "ally" ? `${rec.name || "Your man"} is down!` : `Got one.`);
        if (s.type === "escort" && rec.tag === "escort") return this.fail(s.failText || `${rec.name || "The man you were escorting"} was killed.`);
        if (s.type === "killTarget" && rec.tag === "mark") this.scratch.markDead = true;
        if (s.type === "chase" && rec.tag === "runner") this.scratch.runnerDead = true;
        if (rec.role === "enemy" || rec.role === "guard") this.hud.banner("", `${this.npcs.alive(this.stepGroup(this.stepIndex)).filter(r => r.role !== "ally").length} left`);
    }

    // ------------------------------------------------------------------- tick

    tick() {
        if (this.phase !== "running" || !this.step) return;
        const s = this.step;
        const live = this.livePlayers();
        const active = live.filter(p => !this.isDown(p));
        const elapsed = (this.now() - this.stepStart) / 1000;
        if (this.npcs) this.npcs.tick(active);
        if (live.length === 0) return;

        if (s.timeLimit && elapsed > s.timeLimit) return this.fail(s.failText || "Out of time.");
        if (s.waves) this.spawnWaves(s, elapsed, this.stepGroup(this.stepIndex));

        switch (s.type) {
            case "board": return this.tickBoard(s, live, active);
            case "goto": return this.tickGoto(s, live, active, elapsed);
            case "wait": return elapsed >= s.seconds ? this.completeStep() : undefined;
            case "survive": return this.tickSurvive(s, live, active, elapsed);
            case "together": return this.tickTogether(s, live, active, elapsed);
            case "convoy": return this.tickConvoy(s, live, active);
            case "race": return this.tickRace(s, live, active);
            case "combat": return this.tickCombat(s, live, active, elapsed);
            case "defend": return this.tickDefend(s, live, active, elapsed);
            case "escort": return this.tickEscort(s, live, active, elapsed);
            case "stealth": return this.tickStealth(s, live, active, elapsed);
            case "killTarget": return this.tickKillTarget(s, live, active, elapsed);
            case "chase": return this.tickChase(s, live, active, elapsed);
            default:
                this.warn(`unknown step type ${s.type}; skipping`);
                return this.completeStep();
        }
    }

    arrived(players, target, radius, zTol = 8) {
        return players.filter(p => { try { return V.within(p.position, target, radius, zTol); } catch (_) { return false; } });
    }

    tickBoard(s, live, active) {
        const aboard = active.filter(p => this.playerInTagged(p, s.vehicle));
        if (aboard.length === live.length) return this.completeStep();
        this.scratch.progress = `${aboard.length}/${live.length} aboard`;
    }

    tickGoto(s, live, active, elapsed) {
        const radius = s.radius || 15;
        const who = s.who || "all";
        const candidates = s.vehicle ? active.filter(p => this.playerInTagged(p, s.vehicle)) : active;
        const arrived = this.arrived(candidates, s.target, radius, s.zTol || 8);
        if (s.vehicle) this.scratch.progress = `${candidates.length}/${live.length} in vehicle`;
        if (who === "any" && arrived.length > 0) return this.completeStep();
        if (who === "all" && arrived.length === live.length) return this.completeStep();
        if (arrived.length > 0 && !this.scratch.firstArrival) {
            this.scratch.firstArrival = this.now();
            if (who === "all" && live.length > 1) this.hud.banner("ALMOST", `${safeName(arrived[0])} is there. Wait for the crew.`);
        }
    }

    checkStayIn(s, active) {
        const now = this.now();
        for (const p of active) {
            if (this.inGrace(p)) continue;
            const inside = this.playerInTagged(p, s.stayIn);
            const left = this.scratch.leftAt;
            if (inside) { left.delete(p.id); continue; }
            if (!left.has(p.id)) { left.set(p.id, now); this.hud.tell(p, "Get back in the car!"); }
            else if (now - left.get(p.id) > STAY_IN_GRACE_MS) return `${safeName(p)} abandoned the vehicle.`;
        }
        return null;
    }

    tickSurvive(s, live, active, elapsed) {
        if (s.stayIn) { const why = this.checkStayIn(s, active); if (why) return this.fail(why); }
        if (elapsed >= s.seconds) return this.completeStep();
    }

    checkSpread(s, active) {
        const now = this.now();
        const checked = active.filter(p => !this.inGrace(p));
        if (checked.length > 1 && s.maxSpread) {
            let spread = 0;
            for (let i = 0; i < checked.length; i++) for (let j = i + 1; j < checked.length; j++) {
                try { spread = Math.max(spread, V.dist2(checked[i].position, checked[j].position)); } catch (_) {}
            }
            this.scratch.progress = `spread ${spread.toFixed(0)} m / ${s.maxSpread} m`;
            if (spread > s.maxSpread) {
                if (!this.scratch.spreadAt) { this.scratch.spreadAt = now; this.hud.objective(s.objective, `Stay together! (max ${s.maxSpread} m apart)`); }
                else if (now - this.scratch.spreadAt > (s.graceSeconds || 10) * 1000) return "You split up.";
            } else if (this.scratch.spreadAt) {
                this.scratch.spreadAt = 0;
                this.hud.objective(s.objective, s.hint || "");
            }
        } else {
            this.scratch.spreadAt = 0;
        }
        return null;
    }

    tickTogether(s, live, active, elapsed) {
        const now = this.now();
        if (s.noVehicles) {
            const riding = active.find(p => !this.inGrace(p) && this.vehicleOf(p));
            if (riding) {
                if (!this.scratch.rideWarnAt) { this.scratch.rideWarnAt = now; this.hud.tell(riding, "On foot only. Get out of the vehicle."); }
                else if (now - this.scratch.rideWarnAt > STAY_IN_GRACE_MS) return this.fail(`${safeName(riding)} took a car. This one is on foot.`);
            } else {
                this.scratch.rideWarnAt = 0;
            }
        }
        const why = this.checkSpread(s, active);
        if (why) return this.fail(why);
        const arrived = this.arrived(active, s.target, s.radius || 12);
        if (arrived.length === live.length) return this.completeStep();
    }

    tickConvoy(s, live, active) {
        const radius = s.radius || 20;
        for (const tag of s.vehicles) {
            if (this.scratch.arrivedVehicles.has(tag)) continue;
            const entry = this.vehicles.get(tag);
            if (!entry) continue;
            let pos;
            try { pos = entry.handle.position; } catch (_) { continue; }
            const occupied = active.some(p => this.playerInTagged(p, tag));
            if (occupied && V.within(pos, s.target, radius, 10)) {
                this.scratch.arrivedVehicles.add(tag);
                this.hud.banner("DELIVERED", `${entry.def.label || entry.def.model} is in. ${this.scratch.arrivedVehicles.size}/${s.vehicles.length}`);
            }
        }
        this.scratch.progress = `${this.scratch.arrivedVehicles.size}/${s.vehicles.length} delivered`;
        if (this.scratch.arrivedVehicles.size === s.vehicles.length) return this.completeStep();
    }

    tickRace(s, live, active) {
        const laps = s.laps || 1;
        const gateRadius = s.radius || 18;
        let finished = 0;
        for (const p of live) {
            const r = this.record(p);
            if (!r.race) r.race = { gate: 0, lap: 0, finished: false, finishTime: 0 };
            if (r.race.finished) { finished++; continue; }
            if (this.isDown(p)) continue;
            if (s.vehicles && !s.vehicles.some(tag => this.playerInTagged(p, tag))) continue;
            const gate = s.gates[r.race.gate];
            let pos;
            try { pos = p.position; } catch (_) { continue; }
            if (V.within(pos, gate, gateRadius, 10)) {
                r.race.gate++;
                if (r.race.gate >= s.gates.length) {
                    r.race.gate = 0;
                    r.race.lap++;
                    if (r.race.lap >= laps) {
                        r.race.finished = true;
                        r.race.finishTime = this.now() - this.stepStart;
                        finished++;
                        const place = [...this.players.values()].filter(x => x.race && x.race.finished).length;
                        this.hud.banner(`${ordinal(place)} PLACE`, `${safeName(p)} finished in ${(r.race.finishTime / 1000).toFixed(1)}s`);
                        continue;
                    }
                    this.hud.tell(p, `Lap ${r.race.lap}/${laps}`);
                }
                this.hud.waypoint(p, { label: `Gate ${r.race.gate + 1}/${s.gates.length}` });
            }
        }
        this.scratch.progress = `${finished}/${live.length} finished`;
        if (finished === live.length) {
            const order = [...this.players.values()].filter(x => x.race && x.race.finished).sort((a, b) => a.race.finishTime - b.race.finishTime);
            if (order.length) this.hud.say(`Winner: ${order[0].name} (${(order[0].race.finishTime / 1000).toFixed(1)}s)`);
            return this.completeStep();
        }
    }

    /** Spawns the next wave of `waves` whose `at` (seconds) has passed. Returns true when all are out. */
    spawnWaves(s, elapsed, group) {
        const waves = s.waves || [];
        while (this.scratch.wavesSpawned < waves.length && elapsed >= (waves[this.scratch.wavesSpawned].at || 0)) {
            const w = waves[this.scratch.wavesSpawned++];
            if (this.npcs && this.npcs.available) {
                const defs = (typeof w.enemies === "function" ? w.enemies(this) : w.enemies || []).map(d => ({ role: "enemy", ...d }));
                this.npcs.spawnMany(defs, group);
                this.hud.banner(w.title || "MORE OF THEM", w.text || `Wave ${this.scratch.wavesSpawned}/${waves.length}`);
            }
        }
        return this.scratch.wavesSpawned >= waves.length;
    }

    enemiesAlive() {
        if (!this.npcs) return 0;
        return this.npcs.alive(this.stepGroup(this.stepIndex)).filter(r => r.role === "enemy" || r.role === "guard").length;
    }

    tickCombat(s, live, active, elapsed) {
        if (!this.npcs || !this.npcs.available) {
            const secs = s.fallbackSeconds || 40;
            this.scratch.progress = `hold out ${Math.max(0, Math.ceil(secs - elapsed))}s`;
            if (elapsed >= secs) return this.completeStep();
            return;
        }
        const allOut = this.scratch.wavesSpawned >= (s.waves || []).length;
        const alive = this.enemiesAlive();
        if (s.kills) {
            this.scratch.progress = `${this.scratch.kills}/${s.kills} down`;
            if (this.scratch.kills >= s.kills) return this.completeStep();
            return;
        }
        this.scratch.progress = `${alive} left`;
        if (allOut && alive === 0 && elapsed > 2) return this.completeStep();
        if (allOut && elapsed > (s.maxSeconds || NPC_STEP_MAX_SECONDS)) {
            // Safety net: an enemy whose model never streamed in cannot be killed. Do not soft-lock the chapter.
            this.warn(`combat step ${this.stepIndex + 1} timed out with ${alive} enemies left; completing`);
            this.hud.banner("THEY PULLED BACK", "The rest of them ran.");
            return this.completeStep();
        }
    }

    tickDefend(s, live, active, elapsed) {
        if (s.area) {
            const now = this.now();
            const inside = this.arrived(active.filter(p => !this.inGrace(p)), s.area, s.radius || 25, 10);
            if (inside.length === 0 && active.some(p => !this.inGrace(p))) {
                if (!this.scratch.leftAreaAt) { this.scratch.leftAreaAt = now; this.hud.objective(s.objective, "Hold the position! Get back inside."); }
                else if (now - this.scratch.leftAreaAt > (s.graceSeconds || 15) * 1000) return this.fail(s.failText || "You gave up the position.");
            } else if (this.scratch.leftAreaAt) {
                this.scratch.leftAreaAt = 0;
                this.hud.objective(s.objective, s.hint || "");
            }
        }
        this.scratch.progress = `${Math.max(0, Math.ceil(s.seconds - elapsed))}s, ${this.enemiesAlive()} hostile`;
        if (elapsed >= s.seconds) return this.completeStep();
    }

    tickEscort(s, live, active, elapsed) {
        const ally = this.npcs && this.npcs.available ? this.npcs.byTag("escort") : null;
        if (!ally) {
            // No NPC: play it as a together/goto to the target.
            const arrived = this.arrived(active, s.target, s.radius || 12);
            if (arrived.length === live.length) return this.completeStep();
            return;
        }
        if (!ally.alive) return this.fail(s.failText || `${ally.name || "Your man"} was killed.`);
        let apos;
        try { apos = this.npcs.posOf(ally); } catch (_) { return; }
        const allyThere = V.within(apos, s.target, s.radius || 12, 8);
        const crewThere = this.arrived(active, s.target, (s.radius || 12) + 6).length > 0;
        this.scratch.progress = `${ally.name || "escort"} ${Math.round(V.dist2(apos, s.target))} m out`;
        if (allyThere && crewThere) return this.completeStep();
    }

    tickStealth(s, live, active, elapsed) {
        const group = this.stepGroup(this.stepIndex);
        const guards = this.npcs && this.npcs.available ? this.npcs.alive(group).filter(r => r.role === "guard") : [];
        if (!this.scratch.alarm && guards.length) {
            const detect = s.detectRadius || 10;
            for (const g of guards) {
                const gpos = this.npcs.posOf(g);
                const seen = active.find(p => { try { return V.within(p.position, gpos, detect, 4); } catch (_) { return false; } });
                if (seen) {
                    this.scratch.alarm = true;
                    if ((s.onAlarm || "combat") === "fail") return this.fail(s.failText || `${safeName(seen)} was spotted.`);
                    this.npcs.alert(group);
                    this.hud.banner("SPOTTED", "They know you are here. Fight your way through.");
                    this.hud.objective(s.objective, "Cover blown: deal with them or get to the objective.");
                    break;
                }
            }
        }
        this.scratch.progress = this.scratch.alarm ? `alarm, ${this.enemiesAlive()} hostile` : `${guards.length} guards, unseen`;
        const who = s.who || "all";
        const arrived = this.arrived(active, s.target, s.radius || 10);
        if (who === "any" ? arrived.length > 0 : arrived.length === live.length) return this.completeStep();
    }

    tickKillTarget(s, live, active, elapsed) {
        if (!this.npcs || !this.npcs.available) {
            const arrived = this.arrived(active, s.target.pos, s.radius || 10);
            if (arrived.length > 0) {
                if (!this.scratch.fallbackAt) { this.scratch.fallbackAt = this.now(); this.hud.banner("TARGET", "He is here. Hold the spot."); }
                else if (this.now() - this.scratch.fallbackAt > (s.fallbackSeconds || 15) * 1000) return this.completeStep();
            }
            return;
        }
        const mark = this.npcs.byTag("mark");
        if (this.scratch.markDead || (mark && !mark.alive) || !mark) return this.completeStep();
        this.scratch.progress = `${this.enemiesAlive()} bodyguards`;
        if (elapsed > (s.maxSeconds || NPC_STEP_MAX_SECONDS)) {
            this.warn(`killTarget step ${this.stepIndex + 1} timed out; completing`);
            this.hud.banner("HE'S DOWN", "Someone else got to him first.");
            return this.completeStep();
        }
    }

    tickChase(s, live, active, elapsed) {
        const path = s.path || [];
        if (!this.npcs || !this.npcs.available) {
            const end = path[path.length - 1];
            if (end && this.arrived(active, end, s.catchRadius || 8).length > 0) return this.completeStep();
            return;
        }
        const runner = this.npcs.byTag("runner");
        if (!runner) return this.completeStep();
        if (this.scratch.runnerDead || !runner.alive) {
            if (s.mustCatchAlive) return this.fail(s.failText || "You killed him. He had the answers.");
            return this.completeStep();
        }
        const rpos = this.npcs.posOf(runner);
        const caught = active.find(p => { try { return V.within(p.position, rpos, s.catchRadius || 4, 3); } catch (_) { return false; } });
        if (caught) { this.hud.banner("CAUGHT", `${safeName(caught)} grabbed him.`); return this.completeStep(); }
        const idx = this.scratch.chasePoint;
        if (idx < path.length) {
            if (!this.scratch.chaseOrdered || this.now() - this.scratch.chaseOrdered > 2000) {
                try { runner.handle.goTo(path[idx], true); } catch (_) {}
                this.scratch.chaseOrdered = this.now();
            }
            if (V.within(rpos, path[idx], 3, 3)) { this.scratch.chasePoint++; this.scratch.chaseOrdered = 0; }
        } else if (s.escapeFails !== false) {
            return this.fail(s.failText || "He got away.");
        } else {
            return this.completeStep();
        }
        this.scratch.progress = `${Math.round(V.dist2(rpos, active[0] ? active[0].position : rpos))} m behind`;
    }

    // -------------------------------------------------------------------- hud

    currentTarget(player) {
        const s = this.step;
        if (!s) return null;
        switch (s.type) {
            case "race": { const r = this.players.get(player.id); return r && r.race ? s.gates[r.race.gate] : s.gates[0]; }
            case "board": { const entry = this.vehicles.get(s.vehicle); try { return entry ? entry.handle.position : null; } catch (_) { return null; } }
            case "escort": return s.target;
            case "killTarget": { const m = this.npcs && this.npcs.byTag("mark"); if (m) return this.npcs.posOf(m); return s.target ? s.target.pos : null; }
            case "chase": { const r = this.npcs && this.npcs.byTag("runner"); if (r) return this.npcs.posOf(r); return s.path ? s.path[s.path.length - 1] : null; }
            case "combat": {
                if (!this.npcs) return null;
                const foes = this.npcs.alive(this.stepGroup(this.stepIndex)).filter(x => x.role !== "ally");
                if (!foes.length) return null;
                let pos; try { pos = player.position; } catch (_) { return this.npcs.posOf(foes[0]); }
                let best = foes[0], bd = Infinity;
                for (const f of foes) { const d = V.dist2(this.npcs.posOf(f), pos); if (d < bd) { bd = d; best = f; } }
                return this.npcs.posOf(best);
            }
            case "defend": return s.area || null;
            default: return s.target || null;
        }
    }

    pushHud() {
        const players = this.livePlayers();
        const s = this.step;
        const elapsed = s ? (this.now() - this.stepStart) / 1000 : 0;
        let timer = null;
        if (s) {
            if (s.timeLimit) timer = Math.max(0, Math.ceil(s.timeLimit - elapsed));
            else if (s.seconds) timer = Math.max(0, Math.ceil(s.seconds - elapsed));
        }
        const team = players.map(p => {
            const r = this.record(p);
            let veh = null;
            try { const v = this.vehicleOf(p); veh = v ? (this.tagOfVehicle(v) || v.modelName) : null; } catch (_) {}
            return { name: safeName(p), ready: r.ready, down: this.isDown(p), vehicle: veh, health: Math.round(safeHealth(p)) };
        });
        let extra = {};
        try { extra = this.extraState() || {}; } catch (_) {}
        this.hud.state({
            ...extra,
            phase: this.phase,
            npcs: this.npcs ? this.npcs.available : false,
            mission: this.mission ? { title: this.mission.title, index: this.missionIndex + 1 } : null,
            step: s ? { index: this.stepIndex + 1, total: this.mission.steps.length, type: s.type, objective: s.objective || "", hint: s.hint || "", progress: this.scratch.progress || "" } : null,
            timer,
            team,
        });

        if (this.phase === "running" && s) {
            for (const p of players) {
                const target = this.currentTarget(p);
                if (!target) continue;
                let d = null;
                try { d = V.dist2(p.position, target); } catch (_) {}
                if (d !== null) this.hud.waypoint(p, { distance: Math.round(d), x: target.x, y: target.y, z: target.z });
            }
        }
    }
}

/** Mission vehicles carry a plate that identifies their tag, so a hot reload can find and remove strays.
 *  Plates are short (8 chars); long tags get a 2-char hash so "truckA"/"truckB" stay distinct. */
function plateFor(tag) {
    const t = String(tag);
    if (t.length <= 5) return PLATE_PREFIX + t;
    let h = 0;
    for (let i = 0; i < t.length; i++) h = (h * 31 + t.charCodeAt(i)) >>> 0;
    const code = h.toString(36).slice(-2).padStart(2, "0");
    return PLATE_PREFIX + t.slice(0, 3) + code;
}

function safeName(p) {
    try { return p.nickname || `#${p.id}`; } catch (_) { return "?"; }
}

function safeId(e) {
    try { const id = e.id; return (id === undefined || id === null) ? null : id; } catch (_) { return null; }
}

function safeHealth(p) {
    try { return p.health; } catch (_) { return 0; }
}

function ordinal(n) {
    const s = ["th", "st", "nd", "rd"], v = n % 100;
    return n + (s[(v - 20) % 10] || s[v] || s[0]);
}

module.exports = { MissionRunner, plateFor };
