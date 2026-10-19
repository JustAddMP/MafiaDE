#pragma once
// Player motion: how a remote player's stream becomes his puppet's and his car copy's commands on this game
// (docs/protocol.md "Driving the puppet and the car copy"). Pure logic, no game calls, deterministic: the same code
// runs in player_sync.cpp and in tests/player_motion_test.cpp (a recorded path against a simulated AI walker and a
// simulated car copy).
//
//   PlayerTrack  one remote player's stream through core's shared replication layer (net/replica.h): one
//                PlayoutClock (adaptive playout delay) and two snapshot tracks, his body (key 0) and the car he drives
//                (key 1; cleared when he gets into another car or out, so two cars are never blended). Present() =
//                Hermite between the snapshots around the render time, 250 ms extrapolation, then held, moved forward
//                by the playout delay (the "now" lead) - what player_sync.cpp's own Interpolate / Present did.
//   FootDriver   the puppet on foot: core's LocomotionController with the gait his game reports (gaitHint) and the
//                Mafia DE tuning (the AI's own walk / run / sprint speeds), plus the rest blend: a standing player's
//                puppet is not walked the last bit, it stands and is slid onto his spot slowly (0.15 m/s at most)
//                from 0.1 m off.
//   CarDriver    the car copy he drives: core's VehicleController (pin policy: pinned only when its physics does not
//                keep up, retried after 1.5-8 s) with the car correction preset (net/correction.h ForCar: nothing
//                inside 0.5 m, a blend hidden in the motion beyond, a snap from 4 m) and a speed fix: when the copy's
//                own speed is off the driver's it is given his speed (SetSpeed) without a move.
#include <cstdint>

#include "net/correction.h"
#include "net/remote_motion.h"
#include "net/replica.h"

namespace mc::game::pmotion {

namespace motion = net::motion;
namespace replica = net::replica;
using motion::Vec3;

// ---- tuning ---------------------------------------------------------------------------------------------------------
// The Mafia DE puppet: LocomotionTuning with the game's AI speeds as first guesses (calibrated at run time; the
// 0.15.0 traces measured walk 1.3, run 2.2, sprint 3.3 m/s at multiplier 1) and a stop distance equal to the start
// distance (a standing player's puppet within 0.6 m stands; the rest blend puts it on his spot).
motion::LocomotionTuning FootTuning();

struct RestTuning {
    float deadZone = 0.1f;   // m: a standing puppet this close is left alone
    float gain = 4.0f;       // 1/s (the simulation: 2 left the slowest AI's stops at 0.30 m p95)
    float speed = 0.15f;     // m/s: the most it slides while standing (the correction preset's rest speed)
    float share = 0.35f;     // ... or this share of the copy's own speed while it still moves (brakes after his stop)
    float standSpeed = 0.35f;// m/s: a target slower than this stands
};

// ---- the stream -----------------------------------------------------------------------------------------------------
class PlayerTrack {
public:
    static constexpr uint32_t kFoot = 0, kCar = 1;
    explicit PlayerTrack(const net::playout::PlayoutTuning& p = {});
    void Reset();
    // once per received state: its sample time (sender clock, s) and local arrival time
    void Observe(double senderTime, double localTime);
    // his body in that state (bits: anything discrete, e.g. his mode); false when not newer
    bool PushFoot(double t, const Vec3& pos, const Vec3& vel, uint32_t bits = 0);
    // the car he drives in that state; carKey: any id of that car (another key clears the car track first)
    bool PushCar(double t, uint64_t carKey, const Vec3& pos, const Vec3& vel, const float rot[4]);
    // he is not in a car in that state: the next car starts a fresh track
    void NoCar();
    void Update(double localNow);
    // Present(): where he is now - the render time moved forward by the playout delay plus the one-way trip (the
    // playout clock's offset contains the smallest one-way delay, so without it the copy trails him by that long:
    // 0.35 m at 25 m/s)
    replica::Out Foot(double localNow) const;
    replica::Out Car(double localNow) const;
    // the one-way trip to add to the lead (s; the game sets rtt / 2 of the session, clamped to 0..0.15)
    void SetOneWay(double s);
    double OneWay() const { return oneWay_; }
    double Lead() const;  // s: the lead now (playout delay + one-way trip)
    const net::playout::PlayoutClock& Clock() const { return stream_.Clock(); }
    double Delay() const { return stream_.Delay(); }
    double RenderTime(double localNow) const { return stream_.RenderTime(localNow); }
    uint32_t Dropped() const { return stream_.Dropped(); }

private:
    net::replica::Stream stream_;
    uint64_t carKey_ = 0;
    double oneWay_ = 0;
    bool haveCar_ = false;
};

// ---- on foot --------------------------------------------------------------------------------------------------------
struct FootFixTuning {
    float multReissue = 0.15f;   // a speed multiplier this far from the one last given re-issues the walk (the glue
    float reissueMin = 0.1f;     //    gives it with each walk command only), at most every this many s
    float stopAhead = 0.35f;     // s: a stop walks along the copy's own line of motion at most this long at its
                                 //    velocity (beyond the AI's command lag), never to a point it already passed
    float brakeMin = 0.5f;       // m/s: a copy this much faster than him while level with him or ahead ...
    float brakeBehind = 0.2f;    // m: ... (not more than this behind him along its motion) is held back to his speed,
    float brakeShare = 0.7f;     //    at most this share of its own speed (its legs still run: some foot sliding)
};

class FootDriver {
public:
    explicit FootDriver(const motion::LocomotionTuning& t = FootTuning(), const RestTuning& r = {},
                        const FootFixTuning& f = {});
    // in.gaitHint: his reported mode (0 idle, 1 walk, 2 run, 3 sprint). On top of core's LocomotionController:
    //  - the position fix is capped by the faster of him and the copy (a copy still running after he stopped is
    //    pulled back within 35 % of its own speed, hidden in its motion; the controller caps by his speed only);
    //  - standing: the rest blend (dead zone 0.1 m, 0.15 m/s, more while the copy still brakes) instead of a walk;
    //  - a stop (gait Stand, reissue) has moveTo = where the copy comes to rest by itself (its position moved on by
    //    its velocity for the response lag): the glue's stop is a walk to that point, never back to his spot;
    //  - a changed speed multiplier re-issues the walk at once (a brake or a speed-up reaches the AI 0.1 s later,
    //    not at the next 0.35 s re-aim).
    motion::LocomotionCommand Update(const motion::LocomotionInput& in);
    // The command as it was really applied, when the caller changed it after Update (game/traversal.h Catchup turns
    // a snap after a move into a blend, adds a height part): the copy's own walking speed is measured without it.
    void Applied(const motion::LocomotionCommand& c);
    void Reset() {
        loco_.Reset();
        haveLast_ = false;
        issuedMult_ = -1;
    }
    const motion::LocomotionController& Loco() const { return loco_; }
    bool Resting() const { return resting_; }        // the last update slid a standing puppet onto his spot
    float RestMetres() const { return restMetres_; }  // m slid by the rest blend so far
    float CopySpeed() const;                          // the copy's own walking speed (m/s, smoothed)
    uint32_t MultReissues() const { return multReissues_; }
    bool Braking() const { return braking_; }          // the last update held back a copy that ran on
    float BrakeMetres() const { return brakeMetres_; }  // m held back so far

private:
    motion::LocomotionController loco_;
    motion::LocomotionTuning t_;
    RestTuning r_;
    FootFixTuning f_;
    bool resting_ = false;
    float restMetres_ = 0;
    bool haveLast_ = false;
    Vec3 last_, lastNudge_;
    float vel_[2] = {0, 0};  // the copy's own walking velocity (smoothed, our nudges left out)
    float issuedMult_ = -1;  // the multiplier of the walk command last given
    float sinceIssue_ = 1e9f;
    uint32_t multReissues_ = 0;
    bool braking_ = false;
    float brakeMetres_ = 0;
};

// ---- his car --------------------------------------------------------------------------------------------------------
struct CarFixTuning {
    float rotDeadDeg = 3.0f;     // deg: a copy inside the position dead zone is turned only beyond this
    float speedDead = 1.0f;      // m/s: the copy is given the driver's speed when it is off by more than this ...
    float speedShare = 0.12f;    // ... or this share of his speed
    float speedEvery = 0.25f;    // s: at most this often without a move
    // His car slides (a skid, a spin, a crash: its velocity is this far off its heading, above skidSpeed): the copy,
    // whose speed can only be given along its heading, is blended at skidShare of his speed instead of the preset's
    // 30 % - a sliding car hides a sliding correction.
    float skidDeg = 15.0f;
    float skidSpeed = 2.0f;
    float skidShare = 1.0f;
};

// The correction preset for a player's car copy: core's ForCar (nothing inside 0.5 m, snap from 4 m) with a blend
// of gain 5 / s at most 50 % of his speed (ForCar: 3 / s, 30 %): a copy whose steering acts ~0.15 s late drifts wide
// in every turn, and 30 % left it 1.1-1.2 m off at p95 in the simulation (tests/player_motion_test.cpp). UNVERIFIED in
// game how visible the sideways slide is.
net::correction::Tuning CarPreset();

// The forward direction (ground plane, unit) of a car with rotation q (x y z w) as Mafia DE reports it: the car
// frame's +y rotated by the conjugate of q (checked against the velocity of 13517 moving TX rows of the 0.15.0
// traces: median 0.0 deg, p10 / p90 -3.7 / +3.7 deg). False when it is (nearly) vertical.
bool CarForward(const float q[4], float out[2]);

struct CarCommand {
    motion::VehicleCommand vc;  // fix / nudge / rot / power / pinned / vel ... as core's VehicleController
    bool giveSpeed = false;     // fix None but the copy's speed is off: give it vc.vel (SetSpeed) without a move
    net::correction::Result corr;  // the car preset's verdict (Blend: vc.nudge is its delta)
    bool skid = false;          // his car slides (CarFixTuning skidDeg): blended at skidShare
    bool skidStart = false;     // ... from this update on (for the log)
};

class CarDriver {
public:
    explicit CarDriver(const motion::VehicleTuning& v = {}, const CarFixTuning& f = {},
                       const net::correction::Tuning& c = CarPreset());
    CarCommand Update(const motion::VehicleInput& in);
    void Reset();  // another car / no longer driven
    const motion::VehicleController& Veh() const { return veh_; }
    int Pins() const { return veh_.Pins(); }
    int Retries() const { return veh_.Retries(); }
    uint32_t SpeedFixes() const { return speedFixes_; }
    bool Skid() const { return skid_; }  // the last update saw his car slide (skidShare applied)

private:
    motion::VehicleController veh_;
    motion::VehicleTuning vt_;
    net::correction::Corrector fix_;
    net::correction::Tuning ct_;  // the preset (ForCar)
    CarFixTuning f_;
    float sinceSpeed_ = 1e9f;
    uint32_t speedFixes_ = 0;
    bool skid_ = false, wasSkid_ = false;
};

}  // namespace mc::game::pmotion
