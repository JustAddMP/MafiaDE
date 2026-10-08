/**
 * coop-story - client script.
 *
 * Receives presentation events from the server (see server/lib/hud.js) and
 * turns them into:
 *   - the game's own HUD calls (UI.* builtins: banners, notifications,
 *     mission end screen, countdown, fader), and
 *   - a lightweight CEF overlay (hud.html) with the objective, timer, waypoint
 *     distance and the team list.
 *
 * Keys:
 *   F4  open the control panel (buttons for every command; Esc closes it)
 *   F5  toggle ready (lobby)
 *   F6  start / continue the campaign (host)
 *   F7  toggle the overlay
 *   F10 print your position to chat (for authoring: /wai)
 *   (F8 is MafiaMP's dev console, F9 disconnects, F1 unlocks controls)
 */

const TAG = "[COOP]";
const PENDING_MAX = 50;

let hudView = null;
let hudReady = false;
let overlayVisible = true;
const pending = []; // overlay events queued until the page is ready

// --- helpers ---------------------------------------------------------------

/** UI.* builtins format strings into game Lua; the client DLL escapes them, this just keeps them short. */
function clean(s) {
    return String(s === undefined || s === null ? "" : s)
        .replace(/[\r\n]+/g, " ")
        .slice(0, 160);
}

function overlay(name, payload) {
    if (hudView === null) return;
    if (!hudReady) {
        if (pending.length >= PENDING_MAX) pending.shift();
        pending.push([name, payload]);
        return;
    }
    try { Web.emit(hudView, name, payload || {}); } catch (e) { console.warn(`${TAG} overlay emit failed: ${e}`); }
}

function game(fn) {
    try { fn(); } catch (e) { console.warn(`${TAG} UI call failed: ${e}`); }
}

function screenSize() {
    try {
        const size = (typeof Web.getScreenSize === "function") ? Web.getScreenSize() : null;
        if (size && size.width && size.height) return size;
    } catch (_) {}
    return { width: 1920, height: 1080 };
}

// --- overlay ----------------------------------------------------------------

function createOverlay() {
    try {
        const { width, height } = screenSize();
        hudView = Web.createView("fw://resources/coop-story/client/hud.html", {
            x: 0, y: 0, width, height, zIndex: 5, visible: true, focus: false,
        });
        console.log(`${TAG} overlay view ${hudView} created (${width}x${height})`);
    } catch (e) {
        console.warn(`${TAG} could not create overlay: ${e}`);
        hudView = null;
    }
}

Events.on("browserDocumentReady", (d) => {
    const viewId = d && d.viewId;
    if (viewId === hudView) {
        hudReady = true;
        while (pending.length) { const [n, p] = pending.shift(); overlay(n, p); }
        overlay("coop:visible", { visible: overlayVisible });
    }
});

Events.on("browserLoadingFailed", (d) => {
    const viewId = d && d.viewId;
    if (viewId === hudView) {
        console.warn(`${TAG} overlay failed to load: ${d.description}`);
        pending.length = 0;
    } else if (viewId === panelView) {
        console.warn(`${TAG} panel failed to load: ${d.description}`);
        setPanel(false);
    }
});

// --- server -> client events -----------------------------------------------

Events.on("coop:title", (d) => {
    d = d || {};
    setPanel(false); // a chapter is starting: hand control back to the game whatever opened the panel
    // The game's title card neither auto-hides nor can be hidden from here without the right Lua
    // arguments (it stays on screen for good), so chapter titles are drawn by the overlay instead.
    game(() => UI.displayBannerMessage(clean(d.title), clean(d.subtitle)));
    overlay("coop:title", d);
});

Events.on("coop:hideTitle", () => {
    // intentionally a no-op, see coop:title
});

Events.on("coop:objective", (d) => {
    d = d || {};
    game(() => UI.showNotification("OBJECTIVE", clean(d.text), 0));
    overlay("coop:objective", d);
});

Events.on("coop:banner", (d) => {
    d = d || {};
    if (d.title) game(() => UI.displayBannerMessage(clean(d.title), clean(d.text)));
    overlay("coop:banner", d);
});

Events.on("coop:missionEnd", (d) => {
    d = d || {};
    game(() => UI.displayMissionExit(clean(d.title), clean(d.text), Math.max(1, Math.round(d.seconds || 5))));
    overlay("coop:missionEnd", d);
});

Events.on("coop:countdown", (d) => {
    d = d || {};
    game(() => UI.startCountdown(Math.max(1, Math.round(d.seconds || 3))));
});

// In this engine "FadeIn" fades the black overlay IN (screen goes dark). A fade that is not cleared
// leaves the player staring at a black screen, so the only thing ever done here is clearing the fader.
Events.on("coop:fade", () => {
    game(() => UI.faderReset());
});

Events.on("coop:unstick", () => {
    const before = {
        locked: (typeof UI.areControlsLocked === "function") ? UI.areControlsLocked() : "n/a",
        panelOpen, panelReady, hudReady, controlsLockedByPanel,
        chatOpen: (typeof Chat.isOpen === "function") ? Chat.isOpen() : "n/a",
    };
    console.log(`${TAG} unstick: state before ${JSON.stringify(before)}`);
    try { if (panelView !== null) { Web.focusView(panelView, false); Web.hideView(panelView); } } catch (e) { console.warn(`${TAG} unstick panel: ${e}`); }
    try { if (hudView !== null) Web.focusView(hudView, false); } catch (e) { console.warn(`${TAG} unstick hud: ${e}`); }
    panelOpen = false;
    controlsLockedByPanel = false;
    try { if (typeof Chat.close === "function") Chat.close(); } catch (_) {}
    // Unwind the game's lock counter completely, whoever incremented it.
    let guard = 0;
    try {
        while (typeof UI.areControlsLocked === "function" && UI.areControlsLocked() && guard++ < 16) UI.lockControls(false);
    } catch (e) { console.warn(`${TAG} unstick lock: ${e}`); }
    try { UI.faderReset(); } catch (_) {}
    const after = { locked: (typeof UI.areControlsLocked === "function") ? UI.areControlsLocked() : "n/a", unlocks: guard };
    console.log(`${TAG} unstick: state after ${JSON.stringify(after)}`);
    overlay("coop:banner", { title: "UNSTICK", text: `lock ${before.locked} -> ${after.locked}, panel ${before.panelOpen}, chat ${before.chatOpen}` });
});

Events.on("coop:state", (d) => { overlay("coop:state", d || {}); panelEmit("coop:state", d || {}); });
Events.on("coop:waypoint", (d) => overlay("coop:waypoint", d || {}));
Events.on("coop:panelData", (d) => { panelData = d || panelData; panelEmit("coop:panelData", panelData); });

// --- control panel (F4) -----------------------------------------------------
// A second CEF view with buttons for every chat command. While it is open the
// view has focus, the game's controls are locked (UI.lockControls) and the
// cursor is shown. Esc or the Close button hands control back.

let panelView = null;
let panelReady = false;
let panelOpen = false;
let panelData = null;

function panelEmit(name, payload) {
    if (panelView === null || !panelReady) return;
    try { Web.emit(panelView, name, payload || {}); } catch (e) { console.warn(`${TAG} panel emit failed: ${e}`); }
}

let controlsLockedByPanel = false;
function lockControls(locked) {
    if (locked === controlsLockedByPanel) return; // the game's lock is a counter: never double-lock or over-unlock
    controlsLockedByPanel = locked;
    if (typeof UI.lockControls === "function") game(() => UI.lockControls(locked));
}

function createPanel() {
    try {
        const { width, height } = screenSize();
        panelView = Web.createView("fw://resources/coop-story/client/panel.html", {
            x: 0, y: 0, width, height, zIndex: 20, visible: false, focus: false,
        });
        Web.on(panelView, "cmd", (payload) => {
            const cmd = payload && payload.cmd ? String(payload.cmd) : "";
            if (!cmd.startsWith("/")) return;
            try { Chat.send(cmd); } catch (e) { console.warn(`${TAG} Chat.send failed: ${e}`); }
            // Every button hands control straight back to the game: a panel left open keeps the
            // controls locked, and the only way to notice is not being able to move.
            setPanel(false);
        });
        Web.on(panelView, "close", () => setPanel(false));
        Web.on(panelView, "ready", () => {
            panelReady = true;
            if (panelData) panelEmit("coop:panelData", panelData);
        });
        console.log(`${TAG} panel view ${panelView} created`);
    } catch (e) {
        console.warn(`${TAG} could not create panel: ${e}`);
        panelView = null;
    }
}

function setPanel(open) {
    if (panelView === null || open === panelOpen) return;
    try {
        if (open) {
            Web.showView(panelView);
            Web.focusView(panelView, true);
            lockControls(true);
        } else {
            Web.focusView(panelView, false);
            Web.hideView(panelView);
            lockControls(false);
        }
        panelOpen = open;
    } catch (e) {
        console.warn(`${TAG} panel toggle failed: ${e}`);
        lockControls(false);
        panelOpen = false;
    }
}

// --- keys -------------------------------------------------------------------

function bindKeys() {
    const bind = (key, fn) => { try { Key.bind(key, "down", fn); } catch (e) { console.warn(`${TAG} bind ${key} failed: ${e}`); } };
    bind("f5", () => Chat.send("/ready"));
    bind("f6", () => Chat.send("/start"));
    bind("f7", () => {
        overlayVisible = !overlayVisible;
        overlay("coop:visible", { visible: overlayVisible });
    });
    bind("f10", () => Chat.send("/wai"));
    bind("f4", () => setPanel(true)); // closing happens from inside the panel (Esc / Close), since binds pause while it has focus
}

// --- lifecycle --------------------------------------------------------------

Events.on("resourceStart", (name) => {
    if (name !== "coop-story") return;
    console.log(`${TAG} client resource started`);
    game(() => UI.faderReset()); // clear any fade left over from a previous script instance
    createOverlay();
    createPanel();
    bindKeys();
    try { Events.emitServer("coop:hello", {}); } catch (e) { console.warn(`${TAG} hello failed: ${e}`); }
});

Events.on("resourceStop", (name) => {
    if (name !== "coop-story") return;
    lockControls(false);
    if (hudView !== null) { try { Web.destroyView(hudView); } catch (_) {} }
    if (panelView !== null) { try { Web.destroyView(panelView); } catch (_) {} }
    hudView = null;
    hudReady = false;
    pending.length = 0;
    panelView = null;
    panelReady = false;
    panelOpen = false;
});
