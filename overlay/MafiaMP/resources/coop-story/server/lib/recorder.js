/**
 * In-game route/marker recorder for authoring missions.
 *
 * The server scripting sandbox blocks `fs`, so recorded data is printed to the
 * server console as JSON (and echoed to the recording player). Paste it into a
 * mission file under server/missions/.
 *
 * Commands (any player):
 *   /mark <name>          print your current position + rotation as a marker
 *   /route start <name>   begin recording a route
 *   /route add [label]    append your current position to the route
 *   /route undo           drop the last point
 *   /route dump           print the route as JSON
 *   /route stop           finish and print
 */

const { fmt } = require("./vec.js");

const routes = new Map(); // playerId -> { name, points: [] }

function snapshot(player) {
    const p = player.position;
    const r = player.rotation;
    return {
        x: +p.x.toFixed(3), y: +p.y.toFixed(3), z: +p.z.toFixed(3),
        rot: { w: +r.w.toFixed(4), x: +r.x.toFixed(4), y: +r.y.toFixed(4), z: +r.z.toFixed(4) },
    };
}

function cmdMark(player, args) {
    const name = args[0] || "marker";
    const s = snapshot(player);
    const line = `"${name}": { pos: v3(${s.x}, ${s.y}, ${s.z}), rot: quat(${s.rot.w}, ${s.rot.x}, ${s.rot.y}, ${s.rot.z}) },`;
    console.log(`[COOP-RECORDER] ${line}`);
    Chat.sendToPlayer(player, `[REC] ${name}: ${fmt(s)} (printed to server console)`);
}

function cmdRoute(player, args) {
    const sub = (args[0] || "").toLowerCase();
    const key = player.id;
    const cur = routes.get(key);

    switch (sub) {
        case "start": {
            const name = args[1] || "route";
            routes.set(key, { name, points: [] });
            Chat.sendToPlayer(player, `[REC] Recording route "${name}". Use /route add at each waypoint.`);
            return;
        }
        case "add": {
            if (!cur) return Chat.sendToPlayer(player, "[REC] No route. /route start <name> first.");
            const s = snapshot(player);
            s.label = args.slice(1).join(" ") || `wp${cur.points.length + 1}`;
            cur.points.push(s);
            Chat.sendToPlayer(player, `[REC] #${cur.points.length} ${s.label}: ${fmt(s)}`);
            return;
        }
        case "undo": {
            if (!cur || cur.points.length === 0) return Chat.sendToPlayer(player, "[REC] Nothing to undo.");
            cur.points.pop();
            Chat.sendToPlayer(player, `[REC] Removed last point (${cur.points.length} left).`);
            return;
        }
        case "dump":
        case "stop": {
            if (!cur) return Chat.sendToPlayer(player, "[REC] No route being recorded.");
            const js = cur.points
                .map(p => `    { label: "${p.label}", pos: v3(${p.x}, ${p.y}, ${p.z}) },`)
                .join("\n");
            console.log(`[COOP-RECORDER] route "${cur.name}" (${cur.points.length} points):\nconst ${cur.name.replace(/[^a-zA-Z0-9_]/g, "_")} = [\n${js}\n];`);
            Chat.sendToPlayer(player, `[REC] Route "${cur.name}" with ${cur.points.length} points printed to server console.`);
            if (sub === "stop") routes.delete(key);
            return;
        }
        default:
            Chat.sendToPlayer(player, "[REC] Usage: /route start <name> | add [label] | undo | dump | stop");
    }
}

module.exports = { cmdMark, cmdRoute };
