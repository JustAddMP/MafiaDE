/**
 * Server -> client presentation layer.
 *
 * Everything the players see goes through here so the mission scripts never
 * talk to clients directly. Two channels are used:
 *   - Events.emitAllClients(name, payload)  -> every client's Events.on(name)
 *   - player.emit(name, jsonString)          -> one client's Events.on(name)
 * The client script (client/main.js) turns these into game HUD calls and
 * updates the CEF overlay (client/hud.html).
 *
 * Nothing here throws: a player who is mid-disconnect has no peer and the
 * chat builtins raise on that.
 */

const TAG = "[COOP]";

function toJson(payload) {
    return JSON.stringify(payload === undefined ? {} : payload);
}

function all(name, payload) {
    try { Events.emitAllClients(name, payload || {}); } catch (e) { console.warn(`${TAG} emitAllClients(${name}) failed: ${e}`); }
}

function one(player, name, payload) {
    try { player.emit(name, toJson(payload)); } catch (e) { console.warn(`${TAG} player.emit(${name}) failed: ${e}`); }
}

const hud = {
    /** Plain chat line to everyone (server notice). */
    say(text) {
        try { Chat.sendToAll(`${TAG} ${text}`); } catch (e) { console.warn(`${TAG} sendToAll failed: ${e}`); }
    },
    tell(player, text) {
        try { Chat.sendToPlayer(player, `${TAG} ${text}`); } catch (e) { console.warn(`${TAG} sendToPlayer failed: ${e}`); }
    },

    /** Big chapter title card. */
    title(title, subtitle) { all("coop:title", { title, subtitle }); },
    hideTitle() { all("coop:hideTitle", {}); },

    /** Freeride-style notification in the game HUD + overlay objective line. */
    objective(text, hint) { all("coop:objective", { text, hint: hint || "" }); },

    /** Short banner (freeride banner movie). */
    banner(title, text) { all("coop:banner", { title, text }); },

    /** Mission complete / failed screen. */
    missionEnd(title, text, seconds) { all("coop:missionEnd", { title, text, seconds: seconds || 5 }); },

    countdown(seconds) { all("coop:countdown", { seconds }); },

    fade(direction, duration) { all("coop:fade", { direction, duration: duration || 1.0 }); },

    /** Periodic overlay state: mission, step, timer, team. */
    state(state) { all("coop:state", state); },

    /** Per-player distance/direction hint for the current waypoint. */
    waypoint(player, payload) { one(player, "coop:waypoint", payload); },

    /** Welcome/help for a single player. */
    help(player, lines) {
        for (const l of lines) { try { Chat.sendToPlayer(player, l); } catch (_) {} }
    },
};

module.exports = hud;
