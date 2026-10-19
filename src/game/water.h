#pragma once
// Water: is our player swimming or under water (docs/game-api.md 25 "Water and motorbikes"), for the pose block
// (player_actions.h kSwim, kUnderwater) and so that the traversal detector does not read his strokes, dives and the
// climb out of the water as jumps and drops (game/traversal.h Sample::inWater). Pure logic, no game calls:
// tests/water_test.cpp.
//
// What the game tells (static, UNVERIFIED in game): the human message IN_DEEP_WATER (HumanMessages 458852; the game's
// own OnEventInDeepWater node in common_human_events.lua listens for it) when he gets into deep water. Whether it
// repeats while he swims is not known, and there is no "out of the water" message. The bindings have no swim getter
// for a human (C_WrapperHuman / C_WrapperPlayer lists, docs/re-notes.md appendix); the glue probes a few names on the
// player anyway and hands over the answer when one exists (`getter`): then it rules. Without one: the height where he
// went in is the surface; well below it he is under water; well above it for a moment (he climbed out onto a shore, a
// pier, a ladder), in a vehicle, or after a teleport he is out again; when the message did repeat, its stopping ends
// the swim too.
#include <cstdint>

namespace mc::game::water {

enum class State : uint8_t { Dry = 0, Swim = 1, Under = 2 };
const char* Name(State s);

struct Tuning {
    float underDepth = 1.2f;  // m below the surface: under water
    float underBack = 0.8f;   // m below the surface: back at the surface (hysteresis)
    float exitRise = 0.9f;    // m above the surface ...
    float exitTime = 0.4f;    // s ... for this long: out of the water
    float repeatGap = 3.0f;   // s: messages this close count as repeating; then none for 2 x this = out
    float teleport = 25.0f;   // m of height in one sample: a respawn or a load, out
};

struct Sample {
    double t = 0;            // s, any steady clock
    float z = 0;             // his height (m)
    bool onFoot = true;      // false: in a vehicle (a boat, a sinking car: the vehicle is what is sent)
    bool deepWater = false;  // IN_DEEP_WATER for him since the last sample
    int getter = -1;         // a swim getter's answer: -1 none, 0 dry, 1 swimming, 2 under water
};

struct Change {
    bool changed = false;
    State from = State::Dry, to = State::Dry;
    const char* why = "";
};

class Tracker {
public:
    explicit Tracker(const Tuning& t = {}) : t_(t) {}
    Change Update(const Sample& s);
    State Now() const { return state_; }
    bool InWater() const { return state_ != State::Dry; }
    float Surface() const { return surface_; }  // the height taken for the surface (valid while in water)
    int Messages() const { return messages_; }   // IN_DEEP_WATER seen this time in the water
    void Reset();

private:
    Change Set(State to, const char* why);
    Tuning t_;
    State state_ = State::Dry;
    float surface_ = 0;
    double lastMsg_ = -1e9;
    double highT_ = -1;  // since when he is above surface + exitRise (-1 not)
    int repeats_ = 0;
    int messages_ = 0;
    bool haveZ_ = false;
    float lastZ_ = 0;
};

}  // namespace mc::game::water
