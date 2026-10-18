#pragma once
// Visible one-shot actions of a player (punches, a counter, a takedown, a throw, a melee hit reaction, and the
// traversal moves: jump, vault, climb up, drop down, landing, ladders, a slide into cover; a medkit, picking up and
// dropping a body; a door kick, a lock pull, a door opened by hand) for his puppet on the other games
// (docs/protocol.md "PlayerState", action block). They ride at the end of every PlayerState as a sequence counter
// plus the kind of the latest action, so a lost or reordered state does not lose one: the next state carries the
// same counter, and the receiver plays an action once for each counter step it sees (only the latest kind when it
// missed several). Pure logic, header-only: tests/player_actions_test.cpp.
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
    // traversal moves (game/traversal.h LocalDetector: from HumanMessages USE_FENCE / USE_LADDER and his vertical
    // speed). The puppet follows its player's path while it plays one (IsMove) and catches up without a snap after.
    Jump = 7,        // he went up fast without a fence or ladder (there is no jump message or getter)
    Vault = 8,       // USE_FENCE, about level after it: over a fence, a low wall
    ClimbUp = 9,     // USE_FENCE and he rose: up onto a ledge / a high wall
    DropDown = 10,   // he fell without jumping (stepped off a ledge)
    Land = 11,       // a hard landing after a long fall
    LadderUp = 12,   // USE_LADDER, going up
    LadderDown = 13, // USE_LADDER, going down
    CoverSlide = 14, // into cover at a run
    // in place
    Medkit = 15,     // PLAYER_USED_MEDKIT
    PickupBody = 16, // PICKUP_BODY (or IsCarryingBody went on)
    DropBody = 17,   // DROP_BODY (or IsCarryingBody went off)
    // doors (game/door_sync: these also tell it that a door change next to our player is his)
    DoorKick = 18,   // DOOR_KICK: he kicked a door open (the door itself: door_sync, docs/game-api.md 21)
    Lockpick = 19,   // LOCKPICK_START / PLAYER_CUSTOMLOCKPICK: he started the lock pull on a door
    DoorOpen = 20,   // DOOR_OPEN: he opened a door by hand
};
constexpr uint8_t kLastAction = 20;

inline const char* Name(Action a) {
    switch (a) {
        case Action::None: return "none";
        case Action::LightPunch: return "light";
        case Action::HeavyPunch: return "heavy";
        case Action::Counter: return "counter";
        case Action::Takedown: return "takedown";
        case Action::Throw: return "throw";
        case Action::HitReact: return "hitreact";
        case Action::Jump: return "jump";
        case Action::Vault: return "vault";
        case Action::ClimbUp: return "climbup";
        case Action::DropDown: return "dropdown";
        case Action::Land: return "land";
        case Action::LadderUp: return "ladderup";
        case Action::LadderDown: return "ladderdown";
        case Action::CoverSlide: return "coverslide";
        case Action::Medkit: return "medkit";
        case Action::PickupBody: return "pickupbody";
        case Action::DropBody: return "dropbody";
        case Action::DoorKick: return "doorkick";
        case Action::Lockpick: return "lockpick";
        case Action::DoorOpen: return "dooropen";
    }
    return "none";
}

// A traversal move: the puppet changes place while it plays it (the receiver's PS report says busy 2: the puppet
// follows its player's path, game/traversal.h Catchup), as opposed to an action played on the spot.
inline bool IsMove(Action a) { return a >= Action::Jump && a <= Action::CoverSlide; }

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

// Sender side: our state's action block from two sources, the Lua glue's counter (punches, throws ...: the glue
// counts them, M.LocalAction) and the moves the C++ side detects (game/traversal.h LocalDetector). The block's
// counter goes up by every step of the glue's counter and by one for every detected move; the kind is the latest of
// them (a punch and a move in the same sample: the move plays, the punch is a missed step on the receiver).
class Merger {
public:
    State Update(const State& lua, Action detected) {
        if (!have_) {
            have_ = true;
            luaSeen_ = lua.seq;
            out_ = lua;
        } else {
            const uint8_t d = static_cast<uint8_t>(lua.seq - luaSeen_);
            if (d != 0 && d < 128) {
                out_.seq = static_cast<uint8_t>(out_.seq + d);
                out_.last = lua.last;
            }
            luaSeen_ = lua.seq;
        }
        if (detected != Action::None) {
            out_.seq = static_cast<uint8_t>(out_.seq + 1);
            out_.last = detected;
        }
        return out_;
    }
    void Reset() { have_ = false; }

private:
    bool have_ = false;
    uint8_t luaSeen_ = 0;
    State out_;
};

// ---- the pose block: what he keeps doing (after the action block) ------------------------------------------------
// Wire (optional, after the action block): u8 bits, then u32 body if bits & kCarry. A state from a build without it
// ends before the block: no bits, still valid; an older reader stops before it. Unknown bits are dropped.
constexpr uint8_t kStealth = 1;  // IsStealthMove (the crouched sneak walk, IsStalkMove, is flags bit kCrouch)
constexpr uint8_t kCarry = 2;    // IsCarryingBody
constexpr uint8_t kLadder = 4;   // on a ladder (from USE_LADDER until he stops climbing, game/traversal.h)
constexpr uint8_t kPoseBits = kStealth | kCarry | kLadder;

struct Pose {
    uint8_t bits = 0;
    uint32_t body = 0;  // kCarry: the body he carries, as the host numbers its NPCs (npc_sync.lua key); 0 = unknown
    bool operator==(const Pose& o) const { return bits == o.bits && body == o.body; }
    bool operator!=(const Pose& o) const { return !(*this == o); }
};

inline void WritePose(net::ByteWriter& w, const Pose& p) {
    const uint8_t bits = p.bits & kPoseBits;
    w.U8(bits);
    if (bits & kCarry) w.U32(p.body);
}

// A truncated block (carry bit without its body) fails the reader.
inline void ReadPose(net::ByteReader& r, Pose& p) {
    p = {};
    if (r.AtEnd()) return;
    p.bits = r.U8() & kPoseBits;
    if (p.bits & kCarry) p.body = r.U32();
}

}  // namespace mc::game::actions
