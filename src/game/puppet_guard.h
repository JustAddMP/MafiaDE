#pragma once
// Puppet guard: two safety nets for a remote player's puppet (docs/game-api.md 25 "Water and motorbikes"). Pure
// logic, no game calls: tests/puppet_guard_test.cpp.
//
//   FastFoot  receiver: his state says "on foot" but he moves faster than anyone runs (about 12 m/s; a sprint is
//             7-8): he rides something this game does not see as a vehicle (a motorbike before 0.15.x was sent like
//             that, up to 32 m/s; any vehicle the sender cannot read). His puppet cannot follow such a run (it lags,
//             gets snapped, sprints and "jumps" around): it is hidden at his position until he is slow again.
//   Shield    receiver: a puppet only dies when its player's state says so (health 0 from his game). Everything that
//             can kill a human locally - drowning, a fall, fire, an explosion this game only shows as a picture, the
//             car copy it sits in blowing up or sinking - is kept away from it: while one of these hazards is there
//             (and for `hold` s after) the glue makes the puppet a demigod (SetDemigod, docs/game-api.md 5; a demigod
//             gets no DAMAGE events, which is why puppets are not demigods all the time: the hits of the local player
//             on a puppet on foot come from those events). His being down always wins: the network death must land.
#include <cstdint>
#include <string>

namespace mc::game::puppet_guard {

// ---- FastFoot --------------------------------------------------------------------------------------------------------
struct FastTuning {
    float on = 12.0f;      // m/s on foot (ground speed) ...
    float onTime = 0.25f;  // s ... for this long: hidden
    float off = 9.0f;      // m/s: below this ...
    float offTime = 0.75f; // s ... for this long: shown again
};

class FastFoot {
public:
    struct Out {
        bool hide = false;     // hide the puppet now
        bool changed = false;  // hide changed with this update
        float peak = 0;        // the highest speed seen while hidden (for the log)
    };
    explicit FastFoot(const FastTuning& t = {}) : t_(t) {}
    // onFoot: his state is on foot (not in a vehicle, not swimming); speed: his ground speed (m/s)
    Out Update(double t, bool onFoot, float speed);
    bool Hidden() const { return hide_; }
    int Times() const { return times_; }  // how often it hid him so far
    void Reset();

private:
    FastTuning t_;
    bool hide_ = false;
    double since_ = -1;  // since when the condition for the other state holds (-1 not)
    float peak_ = 0;
    int times_ = 0;
};

// ---- Shield ----------------------------------------------------------------------------------------------------------
// What threatens the puppet now (bits; the glue gathers them every tick, player_sync.lua M.GuardTick).
enum Hazard : uint32_t {
    kSeat = 1,         // it sits in a car copy (crash, explosion, the copy sinking: the game kills occupants)
    kWater = 2,        // its player swims or is under water (pose bits; an AI human drowns in deep water)
    kInWater = 4,      // the puppet itself got IN_DEEP_WATER lately
    kFall = 8,         // falling fast (its player in the air going down, or the puppet itself)
    kFire = 16,        // it burns (FIRE damage lately)
    kBlast = 32,       // a picture of another game's explosion went off near it
    kHidden = 64,      // hidden (fast on foot, under water, no seat): nobody sees it, nothing may hurt it
    kDown = 128,       // its player is down or dead: no shield (the network death must kill it)
};
std::string HazardText(uint32_t bits);  // "seat+water", "none"

struct ShieldOut {
    bool on = false;
    bool changed = false;
    uint32_t hazards = 0;  // what holds it on now (0 while it only holds on for `hold`)
};

class Shield {
public:
    explicit Shield(float hold = 1.5f) : hold_(hold) {}
    ShieldOut Update(double t, uint32_t hazards);
    bool On() const { return on_; }
    uint32_t Last() const { return last_; }  // the hazards when it was last on
    void Reset();

private:
    float hold_;
    bool on_ = false;
    double lastT_ = -1e9;
    uint32_t last_ = 0;
};

// ---- SeatLoop -------------------------------------------------------------------------------------------------------
// receiver: a puppet thrown off the motorbike its player rides, again and again. In the 0.17.0 one-PC runs of 01:59
// and 02:19 the game threw the host's puppet off its bike copy ~0.1 s after every seat command (163 and 192 times in
// one ride), and the glue put it back at once each time: on and off ten times a second. N get-offs within `window` s
// are a loop: the glue stops seating it for that ride and perches it on the saddle (player_sync.lua M.SeatBike).
class SeatLoop {
public:
    explicit SeatLoop(int n = 3, double window = 2.0) : n_(n), window_(window) {}
    // a get-off at t (s): true when it makes a loop (the n-th within the window; every later one too)
    bool Off(double t);
    int Recent() const { return recent_; }  // get-offs within the window up to the last Off
    void Reset();

private:
    int n_;
    double window_;
    double times_[8] = {};
    int count_ = 0, recent_ = 0;
};

}  // namespace mc::game::puppet_guard
