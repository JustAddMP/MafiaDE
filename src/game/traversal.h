#pragma once
// Traversal: how a player gets over and onto things (jumps, vaults, ledges, drops, ladders, a slide into cover) and
// what he keeps doing (sneaking, carrying a body, being on a ladder), for his puppet on the other games
// (docs/game-api.md 17, docs/protocol.md "PlayerState" action and pose blocks). Pure logic, no game calls:
// tests/traversal_test.cpp.
//
//   LocalDetector  sender: from the HumanMessages the Lua glue saw for our player since the last sample (USE_FENCE,
//                  USE_LADDER, PICKUP_BODY, DROP_BODY, PLAYER_USED_MEDKIT), his height and vertical speed, his
//                  cover flag and the getters IsStealthMove / IsCarryingBody: the moves to send (player_actions.h
//                  kinds, through actions::Merger into the action block) and his pose (the pose block).
//   Catchup        receiver: while his puppet plays a move (its PS report says busy 2) the locomotion controller is
//                  paused (core net/remote_motion.h, it sees no copy) and the puppet is drawn along its player's
//                  path instead, height included; for a while after the move, a snap the controller asks for is
//                  turned into a quick blend (a vault or a ladder ends with the puppet a few metres off), and the
//                  height difference is blended away (walking cannot climb a ledge).
#include "game/player_actions.h"
#include "net/remote_motion.h"

#include <cstdint>

namespace mc::game::traversal {

// Events the Lua glue saw for our player since the last sample (a bit each, from HumanMessages).
enum Event : uint32_t {
    kEvFence = 1,   // USE_FENCE: a vault or a climb started
    kEvLadder = 2,  // USE_LADDER
    kEvPickup = 4,  // PICKUP_BODY
    kEvDrop = 8,    // DROP_BODY
    kEvMedkit = 16, // PLAYER_USED_MEDKIT
    kEvDeepWater = 32, // IN_DEEP_WATER (not a move: game/water.h tells whether he swims)
};

struct DetectorTuning {
    float classifyTime = 0.3f;     // s after USE_FENCE / USE_LADDER: how far he rose tells which move it is
    float climbRise = 0.6f;        // m: USE_FENCE and he rose this much (within classifyTime): a climb up a ledge
    float ladderRise = 0.15f;      // m: USE_LADDER, this much up or down tells the direction (else up)
    float moveQuiet = 1.2f;        // s after a fence / ladder start: no jump / drop detected from his speed
    float jumpSpeed = 2.0f;        // m/s up: a jump (stairs and slopes stay well below)
    float jumpRearm = 1.0f;        // m/s: his vertical speed must have been below this before (a new jump)
    float fallSpeed = 2.5f;        // m/s down without a jump before: he dropped off something
    float landEnd = 0.8f;          // m/s down: the fall is over below this
    float hardFall = 5.0f;         // m/s down at some point: the landing is a hard one (played)
    float minAir = 0.3f;           // s: shorter airtimes get no landing
    float ladderStill = 0.3f;      // m/s: on a ladder but no faster than this vertically ...
    float ladderStillTime = 0.6f;  // s: ... for this long (after ladderMin): off the ladder
    float ladderMin = 0.8f;        // s: on a ladder at least this long
    float ladderMax = 15.0f;       // s: and at most this long
    float slideSpeed = 2.5f;       // m/s: into cover at least this fast: a slide
    float bodyDedupe = 1.0f;       // s: a body message and the getter edge within this are one pickup / drop
    // Getting on or off a vehicle (a motorbike's quick mount was sent as a jump with vz 9-31 m/s in the 0.15.0 test)
    // and out of the water: his position snaps or climbs, which is no jump or drop.
    float exitQuiet = 1.0f;        // s after he is on foot again (out of a vehicle or the water): no jump / drop
    float maxJumpSpeed = 8.0f;     // m/s up: faster is a snap (mounting, a teleport), not a jump (one is 3-5)
    float snapFall = 12.0f;        // m/s down at the start of a fall: a snap (getting off, a teleport), not a drop
};

struct Sample {
    double t = 0;          // s, any steady clock
    float z = 0;           // his height (m)
    float vz = 0;          // his vertical speed (m/s, smoothed)
    float speed = 0;       // his ground speed (m/s)
    bool onFoot = true;    // false: in a car (nothing is detected, the pose is cleared)
    uint32_t events = 0;   // Event bits since the last sample
    bool cover = false;    // IsInCover
    bool stealth = false;  // IsStealthMove
    bool carrying = false; // IsCarryingBody
    uint32_t body = 0;     // the carried body's key (npc_sync numbering), 0 = unknown
    bool inWater = false;  // he swims or is under water (game/water.h): no jumps or drops, no ladder
};

class LocalDetector {
public:
    explicit LocalDetector(const DetectorTuning& t = {}) : t_(t) {}
    // One sample; returns the move or action to send now (None: nothing new). At most one per sample: a second one
    // in the same sample waits for the next sample.
    actions::Action Update(const Sample& s);
    actions::Pose CurrentPose() const { return pose_; }
    bool OnLadder() const { return ladder_; }
    bool Airborne() const { return air_; }
    int Snaps() const { return snaps_; }
    void Reset();

private:
    void Queue(actions::Action a);
    DetectorTuning t_;
    actions::Action queued_[4] = {};
    int nQueued_ = 0;
    bool have_ = false;
    float prevVz_ = 0;
    bool prevCover_ = false, prevCarry_ = false;
    // fence / ladder start waiting for its classification
    enum class Pending : uint8_t { None, Fence, Ladder } pending_ = Pending::None;
    double pendT_ = 0;
    float pendZ_ = 0;
    double moveT_ = -1e9;  // the latest fence / ladder start
    double exitT_ = -1e9;  // when he was last in a vehicle or in the water
    bool wasAway_ = false; // the previous sample was in a vehicle or in the water
    int snaps_ = 0;        // vertical snaps not taken for a jump or drop
    // airborne (jump or drop)
    bool air_ = false;
    double airT_ = 0;
    float airMinVz_ = 0;
    bool sawFall_ = false;  // a jump: he is on the way down (the apex is no landing)
    // ladder
    bool ladder_ = false;
    double ladderT_ = 0, stillT_ = -1;
    // body messages vs. the getter edge
    double pickupT_ = -1e9, dropT_ = -1e9;
    actions::Pose pose_;
};

struct CatchupTuning {
    float followGain = 10.0f;    // 1/s: during a move the puppet is drawn this fast toward its player (x y z)
    float recoverTime = 1.5f;    // s after a move: snaps become blends ...
    float recoverSnapDist = 8.0f;    // m: ... up to this horizontal error
    float recoverSnapHeight = 6.0f;  // m: ... and this height difference
    float blendGain = 6.0f;      // 1/s: the blend toward the target
    float zDead = 0.25f;         // m: smaller height differences after a move are left to the ground
    float walkFree = 0.9f;       // m: after a move, horizontal errors up to this are left to the walk
    float maxMove = 20.0f;       // s: a move reported for longer is no move any more (stuck report): normal again
};

class Catchup {
public:
    explicit Catchup(const CatchupTuning& t = {}) : t_(t) {}
    struct Info {
        bool moving = false;      // the puppet plays a move: followed
        bool recovering = false;  // within recoverTime after one
        bool softened = false;    // a snap turned into a blend in this update
        bool started = false, ended = false;  // a move started / ended in this update
    };
    // `moving`: the puppet's PS report says it plays a move; `haveCopy`: that report is fresh (copy below known).
    // `lc` is the controller's command for this tick (the controller saw no copy while moving); it is changed in
    // place: while moving a follow nudge (x y z, no walk re-aim, no turn), after a move the snap softening and the
    // height blend.
    Info Apply(bool moving, bool haveCopy, const net::motion::Vec3& copy, const net::motion::Vec3& target, float dt,
               net::motion::LocomotionCommand& lc);
    void Reset();
    int Moves() const { return moves_; }
    int Softened() const { return softened_; }

private:
    CatchupTuning t_;
    bool moving_ = false;
    float moveT_ = 0;
    float sinceMove_ = 1e9f;
    int moves_ = 0, softened_ = 0;
};

}  // namespace mc::game::traversal
