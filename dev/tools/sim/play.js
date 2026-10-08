/**
 * Plays the whole coop-story campaign through the real server scripts with
 * fake players, no game needed. Exercises every step type, deaths, late
 * joiners, disconnects, abort/restart and a hot reload.
 *
 *   node play.js            run everything
 *   node play.js --verbose  print the server log while running
 *   node play.js --no-npc   simulate a server build without the NPC API
 *   node play.js --chapter 7
 */

const path = require("path");
const { createWorld, dist2 } = require("./mock.js");

const RES = path.resolve(__dirname, "../../../Framework/code/projects/MafiaMP/resources/coop-story");
const argv = process.argv.slice(2);
const VERBOSE = argv.includes("--verbose");
const NPC = !argv.includes("--no-npc");
const ONLY = argv.includes("--chapter") ? parseInt(argv[argv.indexOf("--chapter") + 1], 10) : null;

let failures = 0;
const print = (m) => process.stdout.write(String(m) + "\n"); // the mock replaces globalThis.console
function check(cond, msg) { if (!cond) { failures++; print(`  FAIL: ${msg}`); } }

function freshRequire(file) {
    for (const k of Object.keys(require.cache)) if (k.startsWith(RES)) delete require.cache[k];
    return require(file);
}

/** Boots the resource into a mock world and returns helpers bound to it. */
function boot(opts = {}) {
    globalThis.__COOP_TEST__ = true;
    globalThis.__COOP_TEST_MODE__ = "campaign"; // mode.json may default to story for real play
    const w = createWorld({ npcApi: NPC, quiet: !VERBOSE, strict: true });
    freshRequire(path.join(RES, "server/main.js"));
    const { CAMPAIGN } = require(path.join(RES, "server/missions/index.js"));
    w.bus.emit("resourceStart", "coop-story");
    // The runner is private to main.js; read its state through the HUD state broadcasts.
    const state = () => {
        w.advance(1100); // the HUD state is broadcast once a second
        const last = [...w.bus.toClients].reverse().find(e => e.name === "coop:state");
        return last ? JSON.parse(last.payload) : {};
    };
    const latestChapterStep = () => { const s = state(); return s.step ? { index: s.step.index, type: s.step.type, objective: s.step.objective, progress: s.step.progress } : null; };
    return { w, CAMPAIGN, state, latestChapterStep };
}

/** Teleports a player (and the car they sit in) next to a point. */
function moveTo(w, p, target, dx = 0, dy = 0) {
    p.position = new w.Vector3(target.x + dx, target.y + dy, target.z);
    if (p._vehicle) p._vehicle.position = new w.Vector3(target.x + dx, target.y + dy, target.z);
}

const { plateFor } = require(path.join(RES, "server/lib/runner.js"));
function vehicleByPlate(w, tag) {
    return w.vehicles.find(v => !v._destroyed && v.licensePlate === plateFor(tag));
}

function aliveEnemies(w) {
    return w.humans.filter(h => !h._destroyed && !h.dead && h._role !== "ally");
}

/**
 * Solves one step for the crew. Returns false if it does not know how.
 * `mission` is the mission object, `s` the step, `idx` its index.
 */
function solveStep(ctx, mission, s, idx, players) {
    const { w } = ctx;
    const crew = players.filter(p => p._connected);
    const board = (tag) => { const v = vehicleByPlate(w, tag); check(v, `vehicle ${tag} exists for step ${idx + 1}`); if (!v) return null; for (const p of crew) if (p._vehicle !== v) w.enterVehicle(p, v); return v; };
    const killAll = () => { for (const h of w.humans) if (!h._destroyed && !h.dead) w.killHuman(h, crew[0]); };
    const inNpcMode = NPC;
    switch (s.type) {
        case "board": board(s.vehicle); w.advance(300); return true;
        case "goto": {
            if (s.vehicle) board(s.vehicle);
            const who = s.who === "any" ? [crew[0]] : crew;
            w.advance(500);
            if (s.enemies && inNpcMode) killAll();
            who.forEach((p, i) => moveTo(w, p, s.target, i, i));
            w.advance(300);
            return true;
        }
        case "wait": w.advance(s.seconds * 1000 + 200); return true;
        case "survive": if (s.stayIn) board(s.stayIn); if (s.enemies && inNpcMode) { w.advance(500); killAll(); } w.advance(s.seconds * 1000 + 200); return true;
        case "together": { for (const p of crew) w.leaveVehicle(p); crew.forEach((p, i) => moveTo(w, p, s.target, i * 2, 0)); w.advance(300); return true; }
        case "convoy": {
            for (let i = 0; i < s.vehicles.length; i++) {
                const v = vehicleByPlate(w, s.vehicles[i]); check(v, `convoy vehicle ${s.vehicles[i]}`); if (!v) return false;
                const driver = crew[i % crew.length];
                w.leaveVehicle(driver); w.enterVehicle(driver, v);
                moveTo(w, driver, s.target, i * 3, 0);
                w.advance(300);
                if (s.waves && inNpcMode) { w.advance(20000); killAll(); }
            }
            w.advance(300);
            return true;
        }
        case "race": {
            crew.forEach((p, i) => { const v = vehicleByPlate(w, s.vehicles[i % s.vehicles.length]); w.leaveVehicle(p); w.enterVehicle(p, v); });
            for (let lap = 0; lap < (s.laps || 1); lap++) {
                for (const gate of s.gates) {
                    crew.forEach((p, i) => moveTo(w, p, gate, i, 0));
                    w.advance(300);
                    crew.forEach((p, i) => moveTo(w, p, { x: gate.x + 200, y: gate.y + 200, z: gate.z }, i, 0));
                    w.advance(300);
                }
            }
            return true;
        }
        case "combat": {
            const wavesLen = (s.waves || []).length;
            const lastAt = wavesLen ? Math.max(...s.waves.map(x => x.at || 0)) : 0;
            if (!inNpcMode) { w.advance((s.fallbackSeconds || 40) * 1000 + 300); return true; }
            w.advance(2500);
            killAll();
            w.advance(lastAt * 1000 + 1500);
            killAll();
            w.advance(500);
            if (s.kills) { w.advance(500); }
            return true;
        }
        case "defend": {
            if (s.area) crew.forEach((p, i) => moveTo(w, p, s.area, i, 0));
            const total = s.seconds * 1000;
            for (let t = 0; t < total; t += 5000) { w.advance(5000); if (inNpcMode) killAll(); }
            w.advance(700);
            return true;
        }
        case "escort": {
            crew.forEach((p, i) => moveTo(w, p, s.target, i * 2, 0));
            if (inNpcMode) {
                if (s.enemies) { w.advance(500); for (const h of w.humans) if (!h._destroyed && !h.dead && h._role === "enemy") w.killHuman(h, crew[0]); }
                // ally follows the crew (mock NPC movement is 5 m/s); wait until it arrives
                for (let i = 0; i < 40; i++) {
                    const ally = w.humans.find(h => !h._destroyed && !h.dead && h._role === "ally");
                    if (!ally || dist2(ally.position, s.target) <= (s.radius || 12) - 2) break;
                    w.advance(5000);
                }
                w.advance(1500);
            } else {
                w.advance(300);
            }
            return true;
        }
        case "stealth": {
            // approach the target directly: guards are in a ring further out than the detection radius
            const who = s.who === "any" ? [crew[0]] : crew;
            who.forEach((p, i) => moveTo(w, p, s.target, i * 0.5, 0));
            w.advance(400);
            return true;
        }
        case "killTarget": {
            if (!inNpcMode) { crew.forEach((p, i) => moveTo(w, p, s.target.pos, i, 0)); w.advance((s.fallbackSeconds || 15) * 1000 + 500); return true; }
            w.advance(500);
            killAll();
            w.advance(300);
            return true;
        }
        case "chase": {
            if (!inNpcMode) { const end = s.path[s.path.length - 1]; crew.forEach((p, i) => moveTo(w, p, end, i, 0)); w.advance(300); return true; }
            w.advance(2500); // runner has started on the path
            const runner = w.humans.find(h => !h._destroyed && !h.dead && h._role === "target");
            check(runner, "chase runner exists");
            if (runner) moveTo(w, crew[0], runner.position, 1, 0);
            w.advance(300);
            return true;
        }
        default: return false;
    }
}

/** Tags mock NPC records with their role by watching createHuman calls through the npcs log lines. */
function tagRoles(w) {
    for (const line of w.lines.splice(0)) {
        const m = /spawned (\w+) #(\d+)/.exec(line);
        if (m) { const h = w.humans.find(x => x.id === parseInt(m[2], 10)); if (h) h._role = m[1]; }
    }
}

function playChapter(ctx, n, players, opts = {}) {
    const { w, CAMPAIGN, state } = ctx;
    const mission = CAMPAIGN[n - 1];
    const host = players[0];
    print(`Chapter ${n}: ${mission.title}`);
    w.command(host, `/start ${n}`);
    w.advance(1200);
    check(state().phase === "running", `chapter ${n} started`);
    let guard = 0;
    while (guard++ < 60) {
        const cur = state();
        if (cur.phase !== "running") break;
        const st = cur.step;
        if (!st) continue;
        const idx = st.index - 1;
        const s = mission.steps[idx];
        if (!s) { check(false, `chapter ${n}: state reports step ${st.index} of mission "${cur.mission && cur.mission.title}" but mission ${mission.title} has ${mission.steps.length} steps`); break; }
        tagRoles(w);
        const ok = solveStep(ctx, mission, s, idx, players);
        check(ok, `step type ${s.type} solvable`);
        if (!ok) break;
        tagRoles(w);
        if (opts.onStep) opts.onStep(idx, s);
        let now = state();
        if (now.phase === "running" && now.step && now.step.index - 1 === idx) {
            w.advance(2000);
            now = state();
            if (now.phase === "running" && now.step && now.step.index - 1 === idx) {
                check(false, `step ${idx + 1} (${s.type}: ${s.objective}) did not complete; progress="${now.step.progress}"`);
                break;
            }
        }
    }
    w.advance(8000); // mission complete screen
    check(state().phase === "lobby", `chapter ${n} returned to lobby`);
    const completeLine = w.chat.find(c => c.text.includes(`Mission complete: ${mission.title}`));
    check(completeLine, `chapter ${n} completed`);
    w.chat.length = 0;
}

function main() {
    print(`coop-story simulator (NPC API ${NPC ? "on" : "off"})`);
    let ctx = boot();
    let { w } = ctx;
    const a = w.addPlayer("Alice"), b = w.addPlayer("Bob");
    w.advance(1500);
    check(ctx.state().phase === "lobby", "lobby after join");
    check(a._events.some(e => e.name === "coop:panelData"), "panel data sent on join");

    const chapters = ONLY ? [ONLY] : ctx.CAMPAIGN.map((_, i) => i + 1);
    for (const n of chapters) playChapter(ctx, n, [a, b]);

    if (!ONLY) {
        print("Edge cases");
        // Death mid-step: respawn next to the crew, no spread failure in a together step.
        w.command(a, "/start 2");
        w.advance(1500);
        w.kill(b);
        w.advance(4500);
        const alive = w.players.filter(p => p._connected);
        check(dist2(alive[0].position, alive[1].position) < 10, "downed player respawned next to the crew");
        w.advance(16000); // past the spread grace
        check(ctx.state().phase === "running", "no split-up failure after a respawn");
        // Late joiner lands with the crew and can finish the step.
        const c = w.addPlayer("Carol");
        w.advance(1500);
        check(dist2(c.position, a.position) < 10, "late joiner placed with the crew");
        // Abort during the end screen is honoured.
        w.kill(a); w.kill(b); w.kill(c);
        w.advance(500);
        check(ctx.state().phase === "ended", "team wipe fails the mission");
        w.command(a, "/abort");
        w.advance(500);
        check(ctx.state().phase === "lobby", "abort works during the end screen");
        // Host handover when the host leaves; everyone leaving mid-mission aborts.
        w.command(a, "/start 1");
        w.advance(1500);
        w.removePlayer(a);
        w.advance(500);
        check(w.chat.some(m => m.to === "Bob" && m.text.includes("now the host")), "host handed over");
        w.command(b, "/skip");
        w.advance(300);
        check(ctx.state().step && ctx.state().step.index === 2, "new host can skip");
        w.removePlayer(b); w.removePlayer(c);
        w.advance(500);
        check(ctx.state().phase === "lobby", "mission aborted when everyone left");
        // Hot reload with players connected: resource stop/start, vehicles cleaned, panel data re-sent.
        const d = w.addPlayer("Dave");
        w.advance(500);
        w.command(d, "/start 3");
        w.advance(1500);
        const before = w.vehicles.filter(v => !v._destroyed).length;
        check(before >= 3, "chapter 3 spawned its vehicles");
        w.bus.emit("resourceStop", "coop-story");
        check(w.vehicles.filter(v => !v._destroyed).length === 0, "vehicles destroyed on resource stop");
        check(w.humans.filter(h => !h._destroyed).length === 0, "NPCs destroyed on resource stop");
        w.bus.clear(); // the old runtime is gone on the real server
        freshRequire(path.join(RES, "server/main.js"));
        w.bus.emit("resourceStart", "coop-story");
        d._events.length = 0;
        w.bus.clientEmit("coop:hello", { id: d.id }, {});
        w.advance(1500);
        check(ctx.state().phase === "lobby", "lobby after hot reload");
        check(d._events.some(e => e.name === "coop:panelData"), "panel data re-sent after hello");
        // Stealth alarm path: a player walks into a guard.
        w.command(d, "/start 3");
        w.advance(1500);
        tagRoles(w);
        const guard = w.humans.find(h => !h._destroyed && h._role === "guard");
        if (NPC) check(guard, "chapter 3 has guards");
        if (guard) {
            moveTo(w, d, guard.position, 1, 0); w.advance(500);
            check(w.bus.toClients.some(e => e.name === "coop:banner" && e.payload.includes("SPOTTED")), "guards turned hostile on alarm");
            check(w.humans.some(h => !h._destroyed && !h.dead && h._order && h._order.kind === "attack"), "alerted guards attack");
        }
        w.command(d, "/abort");
        w.advance(500);
        // Unknown command, help, places, tp
        w.command(d, "/nonsense"); w.command(d, "/coophelp"); w.command(d, "/places"); w.command(d, "/tp station"); w.command(d, "/missions");
        check(w.chat.some(m => m.text.includes("Unknown command")), "unknown command answered");
        check(dist2(d.position, { x: -560.6, y: -136.9 }) < 1, "/tp works");
        if (NPC) {
            const before = w.humans.filter(h => !h._destroyed).length;
            check(before === 0, `no NPCs left after abort (found ${before})`);
            w.command(d, "/npc 3"); w.advance(200);
            check(w.humans.filter(h => !h._destroyed).length === before + 3, `/npc spawns (have ${w.humans.filter(h => !h._destroyed).length}, chat: ${w.chat.slice(-1).map(c => c.text).join("")})`);
            w.command(d, "/npcclear");
            check(w.humans.filter(h => !h._destroyed).length === before, "/npcclear removes");
        }
    }

    if (!ONLY) {
        print("Story mode");
        const d = w.players.find(p => p.nickname === "Dave");
        w.command(d, "/mode story");
        w.advance(300);
        check(w.chat.some(m => m.text.includes("Mode switched to story")), "mode switched to story");
        const e = w.addPlayer("Eve");
        w.advance(500);
        check(dist2(e.position, d.position) < 6 && dist2(e.position, { x: -985.9, y: -299.4 }) > 50, "story mode: joiner lands next to the host, not in the lobby");
        w.command(d, "/start 1");
        w.advance(1500);
        check(ctx.state().phase === "lobby", "story mode: /start refused");
        check(w.chat.some(m => m.text.includes("Story mode: the host plays")), "story mode: /start explains");
        w.command(e, "/mirror");
        check(w.chat.some(m => m.to === "Eve" && m.text.startsWith("[COOP] Mirrored:")), "/mirror answers");
        d.position = new w.Vector3(500, 600, 7);
        const f = w.addPlayer("Fred");
        w.advance(300);
        check(dist2(f.position, d.position) < 6, "story mode: joiner lands next to the host");
        d.position = new w.Vector3(-200, -300, 9);
        w.command(f, "/join");
        check(dist2(f.position, d.position) < 6, "/join teleports to the host");
        w.command(f, "/ready");
        check(w.chat.some(m => m.to === "Fred" && m.text.includes("nothing to ready up")), "story mode: ready is explained");
        check(f._weapons.length >= 2, "story mode: guest got the loadout");
        if (NPC) {
            const before = d._weapons.length;
            d._weaponIdValue = 7; Object.defineProperty(d, "weaponId", { get() { return this._weaponIdValue; }, configurable: true });
            w.advance(1200);
            check(d._weapons.some(x => x[0] === 7), "story mode: host's game weapon handed out to everyone");
            w.advance(1200);
            check(d._weapons.filter(x => x[0] === 7).length === 1, "weapon sync hands each weapon out once");
        }
        w.removePlayer(f);
        const beforeDeath = e.position;
        w.kill(e);
        w.advance(5000);
        check(dist2(e.position, beforeDeath) < 0.01, "story mode: death does not respawn via the runner");
        w.command(d, "/mode campaign");
        w.advance(300);
        check(dist2(e.position, { x: -985.9, y: -299.4 }) < 10, "back to campaign mode places the crew in the lobby");
    }

    w.restore();
    print(failures === 0 ? "ALL CHECKS PASSED" : `${failures} CHECK(S) FAILED`);
    if (failures && !VERBOSE) print("Re-run with --verbose for the server log.");
    process.exit(failures ? 1 : 0);
}

try { main(); } catch (e) { print(`CRASH: ${e.stack}`); process.exitCode = 1; }
