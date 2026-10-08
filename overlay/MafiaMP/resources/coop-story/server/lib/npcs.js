/**
 * NPC manager: server-spawned humans used as enemies, guards, allies and
 * targets. Built on the MafiaMP NPC API:
 *
 *   World.createHuman(modelHash: string, pos: Vector3, rot) -> Human
 *   human.goTo(pos, run) / attack(target) / follow(target) / flee(pos) / clearOrders()
 *   human.destroy(), human.dead, human.health, human.isNpc, human.addWeapon(id, ammo)
 *   Events "humanDied" (human, killerPlayer|undefined), "humanDestroyed" (human)
 *
 * When the server binary has no NPC API (`World.createHuman` missing) every
 * call degrades to a no-op and `available` is false, so missions fall back to
 * timers/pressure instead of combat. Nothing here throws into the runner.
 *
 * NPC def: { pos, rot?, model?, weapon?: [id, ammo], role?, tag?, name? }
 *   role: "enemy" (attacks nearest player)      "guard" (idle until alerted, then enemy)
 *         "ally" (follows nearest player, shoots enemies)   "target" (flees from players)
 *         "civilian" (stands still)
 */

const V = require("./vec.js");
const { pickProfile, TOMMY } = require("../missions/models.js");

const TAG = "[COOP:NPC]";
const RETARGET_MS = 1500;
const CORPSE_MS = 15000;

class NpcManager {
    constructor(opts = {}) {
        this.now = opts.now || (() => Date.now());
        this.log = opts.log || ((m) => console.log(`${TAG} ${m}`));
        this.records = new Map();   // human id -> record
        this.onDeath = null;        // (record, killerPlayer) => void, set by the runner per step
        this.nextOrderAt = 0;
        this.modelMode = opts.modelMode || "pool"; // "pool": varied spawn profiles; "tommy": the one verified profile
        this._unsubs = [];
        this._unsubs.push(safeOn("humanDied", (human, killer) => this._onDied(human, killer)));
        this._unsubs.push(safeOn("humanDestroyed", (human) => this._onDestroyed(human)));
    }

    dispose() {
        for (const u of this._unsubs) { try { u && u(); } catch (_) {} }
        this.despawnAll();
    }

    get available() {
        try { return typeof World.createHuman === "function"; } catch (_) { return false; }
    }

    // ----------------------------------------------------------------- spawn

    spawn(def, group) {
        if (!this.available) return null;
        let handle = null;
        try {
            const model = def.model || (this.modelMode === "tommy" ? TOMMY : pickProfile());
            handle = World.createHuman(String(model), def.pos, def.rot || V.yaw(0));
        } catch (e) {
            this.log(`createHuman failed: ${e}`);
            return null;
        }
        if (!handle) return null;
        const rec = {
            handle, def, group: group || def.group || "default", role: def.role || "enemy", tag: def.tag || null,
            name: def.name || null, alive: true, target: null, alerted: def.role !== "guard" && def.role !== "civilian",
            spawnedAt: this.now(), diedAt: 0, id: safeId(handle),
        };
        this.records.set(rec.id, rec);
        if (def.weapon) {
            try { handle.addWeapon(def.weapon[0], def.weapon[1] || 100); } catch (e) { this.log(`addWeapon failed: ${e}`); }
        }
        if (typeof def.health === "number") { try { handle.health = def.health; } catch (_) {} }
        this.log(`spawned ${rec.role} #${rec.id} (${rec.group}) at ${V.fmt(def.pos)}`);
        return rec;
    }

    spawnMany(defs, group) {
        const out = [];
        for (const d of defs || []) { const r = this.spawn(d, group); if (r) out.push(r); }
        return out;
    }

    despawn(rec) {
        if (!rec) return;
        this.records.delete(rec.id);
        try { rec.handle.destroy(); } catch (e) { this.log(`destroy #${rec.id} failed: ${e}`); }
    }

    despawnGroup(group) {
        for (const rec of [...this.records.values()]) if (rec.group === group) this.despawn(rec);
    }

    despawnAll() {
        for (const rec of [...this.records.values()]) this.despawn(rec);
    }

    // ----------------------------------------------------------------- query

    all(group) {
        return [...this.records.values()].filter(r => !group || r.group === group);
    }

    alive(group) {
        return this.all(group).filter(r => r.alive && !isDead(r.handle));
    }

    byTag(tag) {
        return [...this.records.values()].find(r => r.tag === tag) || null;
    }

    byRole(role, group) {
        return this.all(group).filter(r => r.role === role);
    }

    posOf(rec) {
        try { return rec.handle.position; } catch (_) { return rec.def.pos; }
    }

    // ----------------------------------------------------------------- orders

    alert(group) {
        for (const rec of this.alive(group)) if (rec.role === "guard") { rec.role = "enemy"; rec.alerted = true; rec.target = null; }
        this.nextOrderAt = 0; // react on the next tick, not after the retarget interval
    }

    /** Nearest non-down player to a point. */
    nearestPlayer(pos, players) {
        let best = null, bestD = Infinity;
        for (const p of players) {
            let d;
            try { d = V.dist2(p.position, pos); } catch (_) { continue; }
            if (d < bestD) { bestD = d; best = p; }
        }
        return { player: best, distance: bestD };
    }

    /** Re-issue orders every RETARGET_MS; also reaps corpses. */
    tick(players) {
        if (!this.available || this.records.size === 0) return;
        const now = this.now();
        for (const rec of [...this.records.values()]) {
            if (!rec.alive || isDead(rec.handle)) {
                if (rec.alive) this._markDead(rec, undefined);
                if (rec.diedAt && now - rec.diedAt > CORPSE_MS) this.despawn(rec);
            }
        }
        if (now < this.nextOrderAt) return;
        this.nextOrderAt = now + RETARGET_MS;
        const enemies = this.alive().filter(r => r.role === "enemy");
        for (const rec of this.alive()) {
            const pos = this.posOf(rec);
            try {
                switch (rec.role) {
                    case "enemy": {
                        const { player } = this.nearestPlayer(pos, players);
                        if (player && rec.target !== player.id) { rec.handle.attack(player); rec.target = player.id; }
                        else if (!player) { rec.handle.clearOrders(); rec.target = null; }
                        break;
                    }
                    case "ally": {
                        const foe = nearestRecord(pos, enemies);
                        if (foe && foe.distance < 40) {
                            if (rec.target !== foe.rec.id) { rec.handle.attack(foe.rec.handle); rec.target = foe.rec.id; }
                        } else {
                            const { player } = this.nearestPlayer(pos, players);
                            if (player && rec.target !== `p${player.id}`) { rec.handle.follow(player); rec.target = `p${player.id}`; }
                        }
                        break;
                    }
                    case "target": {
                        const { player, distance } = this.nearestPlayer(pos, players);
                        if (player && distance < (rec.def.fleeRadius || 35)) {
                            rec.handle.flee(player.position); rec.target = "flee";
                        } else if (rec.target === "flee") { rec.handle.clearOrders(); rec.target = null; }
                        break;
                    }
                    case "guard":
                    case "civilian":
                    default:
                        break;
                }
            } catch (e) {
                this.log(`order for #${rec.id} failed: ${e}`);
            }
        }
    }

    // ----------------------------------------------------------------- events

    _find(human) {
        const id = safeId(human);
        return id !== null ? this.records.get(id) || null : null;
    }

    _markDead(rec, killer) {
        if (!rec.alive) return;
        rec.alive = false;
        rec.diedAt = this.now();
        this.log(`#${rec.id} (${rec.role}, ${rec.group}) died${killer ? " to " + safeName(killer) : ""}`);
        if (this.onDeath) { try { this.onDeath(rec, killer); } catch (e) { this.log(`onDeath threw: ${e}`); } }
    }

    _onDied(human, killer) {
        const rec = this._find(human);
        if (rec) this._markDead(rec, killer);
    }

    _onDestroyed(human) {
        const rec = this._find(human);
        if (rec) this.records.delete(rec.id);
    }
}

function nearestRecord(pos, recs) {
    let best = null, bestD = Infinity;
    for (const rec of recs) {
        let d;
        try { d = V.dist2(rec.handle.position, pos); } catch (_) { continue; }
        if (d < bestD) { bestD = d; best = rec; }
    }
    return best ? { rec: best, distance: bestD } : null;
}

function isDead(handle) {
    try { return !!handle.dead; } catch (_) { return true; }
}

function safeId(h) {
    try { const id = h.id; return (id === undefined || id === null) ? null : id; } catch (_) { return null; }
}

function safeName(p) {
    try { return p.nickname || `#${p.id}`; } catch (_) { return "?"; }
}

function safeOn(name, fn) {
    try { return Events.on(name, fn); } catch (_) { return null; }
}

/** Build n NPC defs in a ring around a point (ground plane). */
function ring(center, n, radius, extra = {}) {
    const out = [];
    for (let i = 0; i < n; i++) {
        const a = (Math.PI * 2 * i) / n;
        out.push({ pos: V.v3(center.x + Math.cos(a) * radius, center.y + Math.sin(a) * radius, center.z), rot: V.yaw((a * 180) / Math.PI + 180), ...extra });
    }
    return out;
}

/** Build n NPC defs along a line from `from` towards `to` (spread evenly). */
function line(from, to, n, extra = {}) {
    const out = [];
    for (let i = 0; i < n; i++) {
        const t = n === 1 ? 0.5 : i / (n - 1);
        out.push({ pos: V.v3(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, from.z + (to.z - from.z) * t), ...extra });
    }
    return out;
}

module.exports = { NpcManager, ring, line };
