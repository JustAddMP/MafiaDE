/**
 * Offline mock of the MafiaMP server scripting API, enough to run the
 * coop-story resource without the game. Virtual clock: nothing waits for
 * real time; `world.advance(ms)` fires timers and intervals in order.
 *
 * Usage: const { createWorld } = require("./mock.js"); const w = createWorld({ npcApi: true });
 */

class Vector3 {
    constructor(x = 0, y = 0, z = 0) { this.x = x; this.y = y; this.z = z; }
    clone() { return new Vector3(this.x, this.y, this.z); }
    toJSON() { return { x: this.x, y: this.y, z: this.z }; }
    toString() { return `(${this.x.toFixed(1)}, ${this.y.toFixed(1)}, ${this.z.toFixed(1)})`; }
}
class Quaternion {
    constructor(w = 1, x = 0, y = 0, z = 0) { this.w = w; this.x = x; this.y = y; this.z = z; }
    toJSON() { return { w: this.w, x: this.x, y: this.y, z: this.z }; }
}

function dist2(a, b) { const dx = a.x - b.x, dy = a.y - b.y; return Math.sqrt(dx * dx + dy * dy); }

class Scheduler {
    constructor() { this.t = 1_000_000; this.seq = 0; this.timers = new Map(); }
    now() { return this.t; }
    setTimeout(fn, ms) { const id = ++this.seq; this.timers.set(id, { fn, at: this.t + Math.max(0, ms | 0), every: 0 }); return id; }
    setInterval(fn, ms) { const id = ++this.seq; const every = Math.max(1, ms | 0); this.timers.set(id, { fn, at: this.t + every, every }); return id; }
    clearTimeout(id) { this.timers.delete(id); }
    clearInterval(id) { this.timers.delete(id); }
    /** Advance virtual time, firing timers in chronological order. */
    advance(ms) {
        const end = this.t + ms;
        for (;;) {
            let nextId = null, nextAt = Infinity;
            for (const [id, tm] of this.timers) if (tm.at < nextAt || (tm.at === nextAt && id < nextId)) { nextAt = tm.at; nextId = id; }
            if (nextId === null || nextAt > end) break;
            const tm = this.timers.get(nextId);
            this.t = nextAt;
            if (tm.every) tm.at += tm.every; else this.timers.delete(nextId);
            tm.fn();
        }
        this.t = end;
    }
}

class EventBus {
    constructor(log) { this.handlers = new Map(); this.clientHandlers = new Map(); this.toClients = []; this.log = log; }
    on(name, fn) { if (!this.handlers.has(name)) this.handlers.set(name, []); this.handlers.get(name).push(fn); return () => this.off(name, fn); }
    /** Mimics the server tearing down a resource runtime: every handler of the old script instance dies. */
    clear() { this.handlers.clear(); this.clientHandlers.clear(); }
    off(name, fn) { const l = this.handlers.get(name); if (l) { const i = l.indexOf(fn); if (i >= 0) l.splice(i, 1); } }
    emit(name, ...args) { for (const fn of [...(this.handlers.get(name) || [])]) { try { fn(...args); } catch (e) { this.log(`[EVENT ERROR ${name}] ${e.stack}`); throw e; } } }
    emitAllClients(name, payload) { this.toClients.push({ name, payload: typeof payload === "string" ? payload : JSON.stringify(payload) }); }
    onClient(name, fn) { if (!this.clientHandlers.has(name)) this.clientHandlers.set(name, []); this.clientHandlers.get(name).push(fn); }
    clientEmit(name, basePlayer, data) { for (const fn of this.clientHandlers.get(name) || []) fn(basePlayer, data); }
}

function createWorld({ npcApi = true, quiet = true, strict = true } = {}) {
    const lines = [];
    const log = (m) => { lines.push(m); if (!quiet) process.stdout.write(m + "\n"); };
    const sched = new Scheduler();
    const bus = new EventBus(log);
    let nextId = 1;
    const players = [];
    const vehicles = [];
    const humans = [];
    const chat = [];

    function mkEntity(kind) {
        const e = {
            id: nextId++, kind,
            _pos: new Vector3(), _rot: new Quaternion(), _forced: 0,
            get position() { return this._pos.clone(); },
            set position(v) { this._pos = new Vector3(v.x, v.y, v.z); this._forced++; },
            get rotation() { return this._rot; },
            set rotation(r) { this._rot = r; },
        };
        return e;
    }

    const World = {
        get players() { return collection(players.filter(p => p._connected)); },
        get vehicles() { return collection(vehicles.filter(v => !v._destroyed)); },
        createVehicle(model) {
            if (typeof model !== "string") throw new TypeError("model must be a string");
            const v = mkEntity("vehicle");
            Object.assign(v, { modelName: model, engineOn: false, fuel: 100, lockState: 0, licensePlate: "", _destroyed: false, _seats: new Map() });
            v.destroy = () => { v._destroyed = true; for (const p of players) if (p._vehicle === v) p._vehicle = null; };
            vehicles.push(v);
            return v;
        },
        setWeatherSet(s) { if (typeof s !== "string") throw new TypeError("weather"); World.weather = s; },
        setDayTimeHours(h) { if (typeof h !== "number") throw new TypeError("time"); World.time = h; },
        getDayTimeHours() { return World.time; },
        getWeatherSet() { return World.weather; },
        weather: "", time: 12,
    };
    if (npcApi) {
        World.createHuman = (model, pos, rot) => {
            if (typeof model !== "string") throw new TypeError("modelHash must be a string");
            const h = mkEntity("human");
            h._pos = new Vector3(pos.x, pos.y, pos.z);
            Object.assign(h, { isNpc: true, dead: false, health: 100, modelHash: model, _destroyed: false, _order: null, _weapons: [] });
            h.addWeapon = (id, ammo) => h._weapons.push([id, ammo]);
            h.goTo = (p, run) => { h._order = { kind: "goto", pos: new Vector3(p.x, p.y, p.z), run }; };
            h.attack = (t) => { h._order = { kind: "attack", target: t }; };
            h.follow = (t) => { h._order = { kind: "follow", target: t }; };
            h.flee = (p) => { h._order = { kind: "flee", pos: new Vector3(p.x, p.y, p.z) }; };
            h.clearOrders = () => { h._order = null; };
            h.destroy = () => { if (h._destroyed) return; h._destroyed = true; bus.emit("humanDestroyed", h); };
            humans.push(h);
            return h;
        };
        World.humans = { get length() { return humans.filter(h => !h._destroyed).length; }, forEach(fn) { humans.filter(h => !h._destroyed).forEach(fn); } };
    }

    function collection(arr) {
        return {
            length: arr.length,
            forEach: (fn) => arr.forEach(fn), filter: (fn) => arr.filter(fn), find: (fn) => arr.find(fn),
            map: (fn) => arr.map(fn), some: (fn) => arr.some(fn), every: (fn) => arr.every(fn),
        };
    }

    const Chat = {
        sendToAll(text) { chat.push({ to: "*", text }); log(`[CHAT] ${text}`); },
        sendToPlayer(player, text) {
            if (!player || player.kind !== "player") throw new TypeError("not a player");
            if (!player._connected) throw new Error("player has no peer");
            chat.push({ to: player.nickname, text }); log(`[CHAT->${player.nickname}] ${text}`);
        },
    };

    const Events = {
        on: (n, f) => bus.on(n, f), off: (n, f) => bus.off(n, f), emit: (n, ...a) => bus.emit(n, ...a),
        emitAllClients: (n, p) => bus.emitAllClients(n, p), onClient: (n, f) => bus.onClient(n, f),
    };

    const g = globalThis;
    const saved = {};
    for (const [k, v] of Object.entries({ Vector3, Quaternion, World, Chat, Events })) { saved[k] = g[k]; g[k] = v; }
    saved.setTimeout = g.setTimeout; saved.setInterval = g.setInterval; saved.clearTimeout = g.clearTimeout; saved.clearInterval = g.clearInterval;
    g.setTimeout = (f, ms) => sched.setTimeout(f, ms); g.setInterval = (f, ms) => sched.setInterval(f, ms);
    g.clearTimeout = (id) => sched.clearTimeout(id); g.clearInterval = (id) => sched.clearInterval(id);
    saved.console = g.console;
    const warnings = [];
    g.console = {
        log: (...a) => log(a.join(" ")),
        warn: (...a) => { const m = a.join(" "); warnings.push(m); log(`[WARN] ${m}`); if (strict && /error|threw|failed/i.test(m) && !/NPC API unavailable/.test(m)) throw new Error(`console.warn in strict mode: ${m}`); },
        error: (...a) => { const m = a.join(" "); warnings.push(m); log(`[ERROR] ${m}`); if (strict) throw new Error(m); },
        info: (...a) => log(a.join(" ")),
    };
    const realDateNow = Date.now;
    Date.now = () => sched.now();

    function addPlayer(nickname) {
        const p = mkEntity("player");
        Object.assign(p, {
            nickname, health: 100, _connected: true, _vehicle: null, _weapons: [], _events: [],
            addWeapon(id, ammo) { this._weapons.push([id, ammo]); },
            emit(name, payload) { if (payload !== undefined && typeof payload !== "string") throw new TypeError("player.emit payload must be a string"); this._events.push({ name, payload }); },
            sendChat(t) { chat.push({ to: nickname, text: t }); },
        });
        Object.defineProperty(p, "vehicle", { get() { return this._vehicle && !this._vehicle._destroyed ? this._vehicle : undefined; } });
        Object.defineProperty(p, "vehicleSeatIndex", { get() { return this._vehicle ? 0 : -1; } });
        players.push(p);
        bus.emit("playerConnect", p);
        bus.clientEmit("coop:hello", { id: p.id }, {});
        return p;
    }
    function removePlayer(p) {
        bus.emit("playerDisconnect", p);   // still listed by World.players during the handler
        p._connected = false;
        if (p._vehicle) { p._vehicle = null; }
    }
    function enterVehicle(p, v, seat = 0) { p._vehicle = v; bus.emit("vehiclePlayerEnter", v, p, seat); }
    function leaveVehicle(p) { const v = p._vehicle; if (!v) return; p._vehicle = null; bus.emit("vehiclePlayerLeave", v, p); }
    function kill(p) { p.health = 0; bus.emit("playerDied", p); }
    function killHuman(h, killer) { if (h.dead || h._destroyed) return; h.dead = true; h.health = 0; bus.emit("humanDied", h, killer); }
    function command(p, line) {
        const parts = line.trim().split(/\s+/); const cmd = parts[0].replace(/^\//, ""); bus.emit("chatCommand", p, line, cmd, parts.slice(1));
    }

    /** Move NPCs a little towards their orders (so chase/escort steps progress). */
    function simulateNpcs(dtMs) {
        for (const h of humans) {
            if (h._destroyed || h.dead || !h._order) continue;
            const speed = 5.0 * (dtMs / 1000);
            let target = null;
            if (h._order.kind === "goto") target = h._order.pos;
            else if (h._order.kind === "attack" || h._order.kind === "follow") { try { target = h._order.target.position; } catch (_) {} }
            else if (h._order.kind === "flee") { const d = dist2(h._pos, h._order.pos) || 1; target = new Vector3(h._pos.x + (h._pos.x - h._order.pos.x) / d * 10, h._pos.y + (h._pos.y - h._order.pos.y) / d * 10, h._pos.z); }
            if (!target) continue;
            const d = dist2(h._pos, target);
            const stop = h._order.kind === "attack" ? 10 : h._order.kind === "follow" ? 3 : 0.5;
            if (d <= stop) continue;
            const step = Math.min(speed, d - stop);
            h._pos = new Vector3(h._pos.x + (target.x - h._pos.x) / d * step, h._pos.y + (target.y - h._pos.y) / d * step, target.z);
        }
    }

    function advance(ms, stepMs = 100) {
        let left = ms;
        while (left > 0) { const dt = Math.min(stepMs, left); sched.advance(dt); simulateNpcs(dt); left -= dt; }
    }

    function restore() {
        for (const [k, v] of Object.entries(saved)) g[k] = v;
        Date.now = realDateNow;
    }

    return { World, Chat, Events, bus, sched, players, vehicles, humans, chat, lines, warnings, addPlayer, removePlayer, enterVehicle, leaveVehicle, kill, killHuman, command, advance, restore, log, Vector3, dist2 };
}

module.exports = { createWorld, Vector3, Quaternion, dist2 };
