#pragma once
// How a puppet is made to play one of its player's actions (a punch, a throw, a traversal move: game/player_actions.h)
// and how the glue finds out which way works on this game. The 0.15.0 test showed that `AnimPlay` (an AI command,
// docs/game-api.md 17) is accepted on a puppet but no punch shows, an idle fidget starts ~2 s late and the throw is
// over after 0.14 s. The exe offers other ways (PlayAnimation's direct path, DesirePlayAnimation, script control);
// which one shows the animation is UNVERIFIED, so the glue tries them in a fixed order and measures each try: the
// puppet's hands are read (GetBonePos) before and during the animation, and a hand that moved well away from where it
// rested means the animation played. Two tries without movement move on to the next way; one try with movement
// keeps that way for the group. Pure logic, header-only: tests/anim_methods_test.cpp; the Lua side is
// player_sync.lua M.PlayAction / M.ActionProbe (through mc_anim_method / mc_anim_report).
#include <cstdint>

namespace mc::game::anim_methods {

// In the order they are tried (the most likely first, docs/game-api.md 17 "Why no punch showed").
enum class Method : uint8_t {
    Direct = 0,      // h:PlayAnimation(name, false): played by the human's animation component when it knows the
                     // name (no AI command queue), else queued like AnimPlay
    DirectNoTrans,   // h:QueueFullbodyNoTransition(true) first (a flag on that same component), then PlayAnimation
    Desire,          // h:DesirePlayAnimation(name, 100): the AnimPlay command with a desirability for the AI to weigh
    Script,          // h:ScriptControl(true, "DesireMainScript") (the game's scriptControlActivate node), then AnimPlay;
                     // ScriptControl(false, ...) when it is over
    ScriptCombat,    // SetCombatMode(true) + script control + AnimPlay (melee animations are "alert" ones)
    AnimPlay,        // h:AnimPlay(name, false) alone (0.13.0 - 0.15.0)
};
constexpr int kMethodCount = 6;

inline const char* Name(Method m) {
    switch (m) {
        case Method::Direct: return "direct";
        case Method::DirectNoTrans: return "direct_notrans";
        case Method::Desire: return "desire";
        case Method::Script: return "script";
        case Method::ScriptCombat: return "script_combat";
        case Method::AnimPlay: return "animplay";
    }
    return "?";
}

// -1 when the name is not a method.
inline int Parse(const char* s) {
    if (!s) return -1;
    for (int i = 0; i < kMethodCount; ++i) {
        const char* n = Name(static_cast<Method>(i));
        const char* a = s;
        while (*a && *n && *a == *n) { ++a; ++n; }
        if (*a == 0 && *n == 0) return i;
    }
    return -1;
}

// Actions are tried per group: what works for a punch need not work for a throw or a vault.
enum class Group : uint8_t { Melee = 0, Throw, Move, Other };
constexpr int kGroupCount = 4;

inline Group GroupOf(int kind) {
    if (kind == 5) return Group::Throw;                  // Action::Throw
    if ((kind >= 1 && kind <= 4) || kind == 6) return Group::Melee;  // punches, counter, takedown, hit reaction
    if (kind >= 7 && kind <= 14) return Group::Move;     // traversal moves
    return Group::Other;                                 // medkit, bodies, doors
}

inline const char* Name(Group g) {
    switch (g) {
        case Group::Melee: return "melee";
        case Group::Throw: return "throw";
        case Group::Move: return "move";
        case Group::Other: return "other";
    }
    return "?";
}

// ---- judging one try ------------------------------------------------------------------------------------------
// What the glue measured while the animation had its time: the largest distance either hand got from where it was
// just before the call (in the puppet's own frame: forward, right, up from its position, so turning or a nudge does
// not count), how far the puppet itself moved (a walk still running swings the arms) and whether the bones could be
// read at all.
struct Probe {
    bool bonesOk = false;
    float maxHand = 0;    // m
    float rootMove = 0;   // m, on the ground
    float firstMoveT = -1;  // s after the call when a hand first moved kStartMove away, -1 never
};

constexpr float kMoved = 0.20f;      // m: a punch or throw moves the hand 0.4-0.7 m, a fidget ~0.2 m
constexpr float kStill = 0.08f;      // m: standing, breathing, the idle sway
constexpr float kStartMove = 0.12f;  // m: the animation has started (firstMoveT; the start latency in the log)
constexpr float kWalking = 0.35f;    // m: the puppet moved this much: arm swing of a walk, cannot tell

enum class Verdict : uint8_t { Moved = 0, Still, Unknown };

inline const char* Name(Verdict v) {
    switch (v) {
        case Verdict::Moved: return "moved";
        case Verdict::Still: return "still";
        case Verdict::Unknown: return "unknown";
    }
    return "?";
}

inline Verdict Judge(const Probe& p) {
    if (!p.bonesOk) return Verdict::Unknown;
    if (p.maxHand >= kMoved) return p.rootMove > kWalking ? Verdict::Unknown : Verdict::Moved;
    if (p.rootMove > kWalking) return Verdict::Unknown;
    return p.maxHand < kStill ? Verdict::Still : Verdict::Unknown;
}

// ---- the order of tries ---------------------------------------------------------------------------------------
enum class Event : uint8_t {
    None = 0,
    Works,     // this way showed movement: kept for the group
    Switched,  // this way showed nothing (twice), or could not be judged (3 times): the next one
    Wrapped,   // past the last way: back to the first, the next animation name in the list (round + 1)
    Lost,      // the way that worked showed nothing 3 times in a row: tried again from the next one
};

inline const char* Name(Event e) {
    switch (e) {
        case Event::None: return "none";
        case Event::Works: return "works";
        case Event::Switched: return "switched";
        case Event::Wrapped: return "wrapped";
        case Event::Lost: return "lost";
    }
    return "?";
}

constexpr int kStillToSwitch = 2;    // tries without movement before the next way
constexpr int kUnknownToSwitch = 3;  // tries that could not be judged before the next way (none confirmed yet)
constexpr int kStillToLose = 3;      // a confirmed way that then shows nothing this often in a row

struct Ladder {
    int idx = 0;        // current method
    int works = -1;     // the method confirmed by movement, -1 none yet
    int still = 0;      // tries in a row without movement with the current method
    int unknown = 0;    // tries in a row that could not be judged
    int round = 0;      // how often all ways were tried (the glue then starts with the next animation name)
    int tries = 0;      // all tries of this group

    Method Current() const { return static_cast<Method>(idx); }
    bool Confirmed() const { return works == idx; }

    Event Advance() {
        still = unknown = 0;
        idx = (idx + 1) % kMethodCount;
        if (idx == 0) {
            ++round;
            return Event::Wrapped;
        }
        return Event::Switched;
    }

    // Force a method (the dev console); confirmed only when it is the one that worked.
    void Set(int m) {
        if (m < 0 || m >= kMethodCount) return;
        idx = m;
        still = unknown = 0;
    }

    Event Report(Verdict v) {
        ++tries;
        switch (v) {
            case Verdict::Moved:
                still = unknown = 0;
                if (works != idx) {
                    works = idx;
                    return Event::Works;
                }
                return Event::None;
            case Verdict::Still:
                unknown = 0;
                ++still;
                if (works == idx) {
                    if (still < kStillToLose) return Event::None;
                    works = -1;
                    Advance();
                    return Event::Lost;
                }
                return still >= kStillToSwitch ? Advance() : Event::None;
            case Verdict::Unknown:
                if (works == idx) return Event::None;  // no evidence against a way that worked
                ++unknown;
                return unknown >= kUnknownToSwitch ? Advance() : Event::None;
        }
        return Event::None;
    }
};

}  // namespace mc::game::anim_methods
