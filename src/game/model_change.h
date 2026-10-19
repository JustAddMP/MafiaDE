#pragma once
// Character changes without a puppet per step (the 0.15.0 two-PC test, 2026-10-19: every step through the pause
// menu's CHARACTER row went out as a PlayerModel at once, and the other game released and respawned the puppet for
// each one: BILLY, TOMMY, BILLY, BERTONE, NORMAN in 2 s, one of them seated in the guest's car).
//   - Outbox (the player who picks): a pick goes out once it has stayed the same for kSendStable s, or at once when
//     the panel with the CHARACTER rows is closed (no panel frame for kPanelGone s), or at once outside a loaded world
//     (the lobby: no puppets anywhere to respawn, and the lobby list should follow quickly).
//   - Gate (every other game, per remote player): what his puppet is made of. A change that comes after a quiet time
//     (no other change for kQuiet s) applies at once; inside a burst (an older sender, or a pick and a story look
//     change close together) the newest value applies once it has stayed unchanged for kApplyStable s. A value that
//     goes back to what is applied cancels the wait.
// Pure logic, header-only: tests/model_change_test.cpp; used by player_sync.cpp (SetModel, OnPlayerModel, MainTick).
#include <cstdint>

namespace mc::game::modelchange {

constexpr double kSendStable = 1.0;   // s: a pick goes out once unchanged this long (panel still open)
constexpr double kPanelGone = 0.25;   // s without a frame of the CHARACTER rows: the panel is closed
constexpr double kQuiet = 2.0;        // s: a change with no other change this long before it applies at once
constexpr double kApplyStable = 1.0;  // s: inside a burst, the newest value applies once unchanged this long

class Outbox {
public:
    // The player stepped the CHARACTER / OUTFIT row (the setting is already saved).
    void Picked(double now) {
        pending_ = true;
        pickedAt_ = now;
        ++steps_;
    }
    // A frame of the panel that shows the CHARACTER rows (pause menu CO-OP panel or the lobby).
    void PanelShown(double now) { panelAt_ = now; }
    bool PanelOpen(double now) const { return panelAt_ >= 0 && now - panelAt_ < kPanelGone; }
    bool Pending() const { return pending_; }
    // True once per pick (or run of picks): it goes out now. inWorld: the local player is in a loaded world.
    // why (optional): "stable", "panel-closed" or "no-world".
    bool Due(double now, bool inWorld, const char** why = nullptr) {
        if (!pending_) return false;
        const char* w = nullptr;
        if (!inWorld)
            w = "no-world";
        else if (!PanelOpen(now))
            w = "panel-closed";
        else if (now - pickedAt_ >= kSendStable)
            w = "stable";
        if (!w) return false;
        if (why) *why = w;
        pending_ = false;
        lastSteps_ = steps_;
        steps_ = 0;
        return true;
    }
    // Panel steps folded into the last Due (1 = a single step).
    int LastSteps() const { return lastSteps_; }
    // Something else sends our PlayerModel now (a player joined, our story look changed): it carries the current
    // pick, so a pending one is sent with it.
    void SentWithOther() {
        if (!pending_) return;
        pending_ = false;
        lastSteps_ = steps_;
        steps_ = 0;
    }
    void Reset() { *this = Outbox(); }

private:
    bool pending_ = false;
    double pickedAt_ = 0;
    double panelAt_ = -1;
    int steps_ = 0;
    int lastSteps_ = 0;
};

// T: a value with ==, e.g. {model, look}.
template <class T>
class Gate {
public:
    // A PlayerModel arrived (or the same value again: nothing changes).
    void Offer(const T& v, double now) {
        if (hasPending_ && pending_ == v) return;
        if (!hasPending_ && hasApplied_ && applied_ == v) return;
        const bool quiet = lastOfferAt_ < 0 || now - lastOfferAt_ >= kQuiet;
        lastOfferAt_ = now;
        if (hasPending_) ++skipped_;  // the waiting value is replaced: it is never shown
        if (hasApplied_ && applied_ == v) {  // back to what is shown: nothing to do
            hasPending_ = false;
            return;
        }
        pending_ = v;
        hasPending_ = true;
        pendingAt_ = now;
        immediate_ = quiet;
    }
    // True when the puppet should be made of a new value now (*out = it, *heldS = how long it waited).
    bool Poll(double now, T* out, double* heldS = nullptr, int* skipped = nullptr) {
        if (!hasPending_) return false;
        if (!immediate_ && now - pendingAt_ < kApplyStable) return false;
        applied_ = pending_;
        hasApplied_ = true;
        hasPending_ = false;
        immediate_ = false;
        if (out) *out = applied_;
        if (heldS) *heldS = now - pendingAt_;
        if (skipped) *skipped = skipped_;
        skipped_ = 0;
        return true;
    }
    bool HasApplied() const { return hasApplied_; }
    const T& Applied() const { return applied_; }
    bool Waiting() const { return hasPending_; }
    // What the player picked last (for the lobby list): the waiting value, else the applied one.
    const T* Newest() const { return hasPending_ ? &pending_ : hasApplied_ ? &applied_ : nullptr; }

private:
    T applied_{}, pending_{};
    bool hasApplied_ = false, hasPending_ = false, immediate_ = false;
    double pendingAt_ = 0, lastOfferAt_ = -1;
    int skipped_ = 0;
};

}  // namespace mc::game::modelchange
