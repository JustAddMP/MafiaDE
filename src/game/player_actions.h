#pragma once
// Visible one-shot actions of a player (punches, a counter, a takedown, a throw, a melee hit reaction) for his
// puppet on the other games (docs/protocol.md "PlayerState", action block). They ride at the end of every
// PlayerState as a sequence counter plus the kind of the latest action, so a lost or reordered state does not lose
// one: the next state carries the same counter, and the receiver plays an action once for each counter step it
// sees (only the latest kind when it missed several). Pure logic, header-only: tests/player_actions_test.cpp.
#include "net/wire.h"

#include <cstdint>
#include <cstring>

namespace mc::game::actions {

enum class Action : uint8_t {
    None = 0,
    LightPunch = 1,  // MELEE_LIGHT_ATTACK on our player (key Q tapped)
    HeavyPunch = 2,  // MELEE_HEAVY_ATTACK (Q held: charged punch)
    Counter = 3,     // MELEE_COUNTER_ATTACK (Left Alt: dodge + counter punch)
    Takedown = 4,    // MELEE_COMBAT_TAKEDOWN / MELEE_STEALTH_TAKEDOWN by our player
    Throw = 5,       // WEAPON_THROWN: a grenade, molotov or knife left our player's hand
    HitReact = 6,    // MELEE_HIT_REACT on our player: an NPC punched him on his own game
};
constexpr uint8_t kLastAction = 6;

inline const char* Name(Action a) {
    switch (a) {
        case Action::None: return "none";
        case Action::LightPunch: return "light";
        case Action::HeavyPunch: return "heavy";
        case Action::Counter: return "counter";
        case Action::Takedown: return "takedown";
        case Action::Throw: return "throw";
        case Action::HitReact: return "hitreact";
    }
    return "none";
}

// A kind from the wire or from the Lua sample; anything unknown is None (newer senders may add kinds).
inline Action FromCode(unsigned v) { return v >= 1 && v <= kLastAction ? static_cast<Action>(v) : Action::None; }

struct State {
    uint8_t seq = 0;              // actions done so far, mod 256; a change = new action(s)
    Action last = Action::None;   // the latest one
};

// Wire (appended after the rest of PlayerState, vehicle block included): u8 seq, u8 kind.
inline void Write(net::ByteWriter& w, const State& s) {
    w.U8(s.seq);
    w.U8(static_cast<uint8_t>(s.last));
}

// Reads the block if the state has one. A state from a build without it ends before: {0, None}, still valid.
// A truncated block (one byte) fails the reader.
inline void Read(net::ByteReader& r, State& s) {
    s = {};
    if (r.AtEnd()) return;
    s.seq = r.U8();
    s.last = FromCode(r.U8());
}

// Receiver side: one per remote player. Feed it the counter of every state it is driven from (the interpolated
// one: it only moves forward); returns the action to play now, None when nothing new.
class Tracker {
public:
    struct Result {
        Action play = Action::None;
        int missed = 0;  // counter steps besides the one played (states with them never arrived or were skipped)
    };
    Result Update(const State& s) {
        Result res;
        if (!have_) {  // the first state seen: whatever he did before is history
            have_ = true;
            seen_ = s.seq;
            return res;
        }
        const uint8_t d = static_cast<uint8_t>(s.seq - seen_);
        if (d == 0 || d >= 128) return res;  // nothing new, or an older state (counter went back): ignore
        seen_ = s.seq;
        res.play = s.last;
        res.missed = d - 1;
        return res;
    }
    void Reset() { have_ = false; }  // he left / rejoined: adopt the next counter without playing
    bool Have() const { return have_; }
    uint8_t Seen() const { return seen_; }

private:
    bool have_ = false;
    uint8_t seen_ = 0;
};

}  // namespace mc::game::actions
