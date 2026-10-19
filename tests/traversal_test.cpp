// traversal_test: unit tests for src/game/traversal.h. The sender's LocalDetector on recorded-like sample runs (a
// jump, a vault, a climb up a ledge, ladders up and down, a drop with a hard landing, a slide into cover, medkit and
// body messages with the getter edges, the pose bits, in a car) and the receiver's Catchup with the real locomotion
// controller (core net/remote_motion.h): the puppet followed along its player's path during a move, a snap after a
// move turned into a blend, the height blended away, normal snaps again after the recovery time, a stuck "move"
// report, melee busy left alone. Prints PASS/FAIL per check; exits non-zero on any failure. Built by build.cmd
// (out\traversal_test.exe), run by tools\make_release.cmd.
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "game/traversal.h"

using namespace mc::game;
using actions::Action;
using traversal::Catchup;
using traversal::LocalDetector;
using traversal::Sample;
namespace motion = mc::net::motion;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-66s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

constexpr double kDt = 1.0 / 60;

// Runs a detector over `secs` of samples at 60 Hz; `at(t, s)` fills each sample (t from 0). Returns every action
// with the time it came.
struct Hit {
    double t;
    Action a;
};
std::vector<Hit> Run(LocalDetector& d, double secs, const std::function<void(double, Sample&)>& at,
                     double t0 = 100.0) {
    std::vector<Hit> out;
    for (int i = 0; i * kDt <= secs; ++i) {
        const double t = i * kDt;
        Sample s;
        s.t = t0 + t;
        at(t, s);
        const Action a = d.Update(s);
        if (a != Action::None) out.push_back({t, a});
    }
    return out;
}

std::string Names(const std::vector<Hit>& h) {
    std::string s;
    for (const Hit& x : h) {
        char b[48];
        std::snprintf(b, sizeof b, "%s%s@%.2f", s.empty() ? "" : " ", actions::Name(x.a), x.t);
        s += b;
    }
    return s.empty() ? "-" : s;
}

bool Only(const std::vector<Hit>& h, std::initializer_list<Action> want) {
    if (h.size() != want.size()) return false;
    size_t i = 0;
    for (Action a : want)
        if (h[i++].a != a) return false;
    return true;
}

// A ballistic jump starting at t0 with vz0 (gravity 9.81), z and the (smoothed-like) vz.
void Ballistic(double t, double t0, float vz0, float& z, float& vz) {
    if (t < t0) {
        z = 0;
        vz = 0;
        return;
    }
    const float u = static_cast<float>(t - t0);
    const float tEnd = 2 * vz0 / 9.81f;
    if (u >= tEnd) {
        z = 0;
        vz = 0;
        return;
    }
    z = vz0 * u - 0.5f * 9.81f * u * u;
    vz = vz0 - 9.81f * u;
}

void TestDetector() {
    {
        LocalDetector d;
        const auto h = Run(d, 3, [](double t, Sample& s) { Ballistic(t, 0.5, 3.8f, s.z, s.vz); s.speed = 3; });
        Check(Only(h, {Action::Jump}) && h[0].t >= 0.49 && h[0].t < 0.6 && !d.Airborne(),
              "a jump: one jump, no landing for a short hop", Names(h));
    }
    {
        LocalDetector d;
        const auto h = Run(d, 3, [](double t, Sample& s) {
            s.z = 0.4f;  // walking on a terrace: flat, no vertical speed
            s.vz = 0.3f * static_cast<float>(std::sin(t * 6));
            s.speed = 4;
        });
        Check(h.empty(), "walking about: nothing", Names(h));
    }
    {  // vault over a fence: USE_FENCE at 0.5 s, up 0.4 m and down again within 0.7 s, fast vertical speed meanwhile
        LocalDetector d;
        const auto h = Run(d, 3, [](double t, Sample& s) {
            if (std::fabs(t - 0.5) < kDt / 2) s.events = traversal::kEvFence;
            if (t > 0.5 && t < 1.2) {
                const float u = static_cast<float>((t - 0.5) / 0.7);
                s.z = 0.4f * std::sin(u * 3.14159f);
                s.vz = 0.4f * 3.14159f / 0.7f * std::cos(u * 3.14159f) * 1.6f;  // up to ~2.9 m/s
            }
            s.speed = 3;
        });
        Check(Only(h, {Action::Vault}) && std::fabs(h[0].t - 0.8) < 0.03,
              "a vault: one vault after the classify time, no jump from its speed", Names(h));
    }
    {  // climb up onto a 1.8 m ledge: USE_FENCE, then up 1.8 m in 0.8 s
        LocalDetector d;
        const auto h = Run(d, 3, [](double t, Sample& s) {
            if (std::fabs(t - 0.5) < kDt / 2) s.events = traversal::kEvFence;
            if (t > 0.5) {
                const float u = static_cast<float>(std::min(1.0, (t - 0.5) / 0.8));
                s.z = 1.8f * u;
                s.vz = t < 1.3 ? 2.25f : 0;
            }
        });
        Check(Only(h, {Action::ClimbUp}) && h[0].t < 0.8, "a climb up a ledge: climbup before the classify time is up",
              Names(h));
    }
    {  // a vault that ends in a drop (a wall with a long way down behind): the vault, then a drop after the quiet time
        LocalDetector d;
        const auto h = Run(d, 4, [](double t, Sample& s) {
            if (std::fabs(t - 0.5) < kDt / 2) s.events = traversal::kEvFence;
            if (t > 1.8 && t < 2.6) {
                s.vz = -9.81f * static_cast<float>(t - 1.8);
                s.z = -4.9f * static_cast<float>((t - 1.8) * (t - 1.8));
            } else if (t >= 2.6) {
                s.z = -3.1f;
            }
        });
        Check(Only(h, {Action::Vault, Action::DropDown, Action::Land}), "a vault, then a long drop: drop and landing",
              Names(h));
    }
    {  // ladder up: USE_LADDER, then 1 m/s up for 4 s, then off at the top
        LocalDetector d;
        bool onDuring = true, offAfter = false;
        const auto h = Run(d, 7, [&](double t, Sample& s) {
            if (std::fabs(t - 0.5) < kDt / 2) s.events = traversal::kEvLadder;
            if (t > 0.5 && t < 4.5) {
                s.z = static_cast<float>(t - 0.5);
                s.vz = 1;
            } else if (t >= 4.5) {
                s.z = 4;
            }
            if (t > 0.6 && t < 4.4) onDuring &= d.OnLadder() && (d.CurrentPose().bits & actions::kLadder);
            if (t > 5.5) offAfter = !d.OnLadder() && !(d.CurrentPose().bits & actions::kLadder);
        });
        Check(Only(h, {Action::LadderUp}) && onDuring && offAfter,
              "ladder up: one ladderup, on the ladder while climbing, off after", Names(h));
    }
    {  // ladder down, fast (2.5 m/s, faster than a fall start): no drop
        LocalDetector d;
        const auto h = Run(d, 4, [](double t, Sample& s) {
            if (std::fabs(t - 0.5) < kDt / 2) s.events = traversal::kEvLadder;
            if (t > 0.5 && t < 2.5) {
                s.z = -2.5f * static_cast<float>(t - 0.5);
                s.vz = -2.6f;
            } else if (t >= 2.5) {
                s.z = -5;
            }
        });
        Check(Only(h, {Action::LadderDown}), "ladder down: one ladderdown, no drop from its speed", Names(h));
    }
    {  // stepping off a 1 m ledge: a drop, no hard landing
        LocalDetector d;
        const auto h = Run(d, 3, [](double t, Sample& s) {
            if (t > 1 && t < 1.45) {
                s.vz = -9.81f * static_cast<float>(t - 1);
                s.z = -4.9f * static_cast<float>((t - 1) * (t - 1));
            } else if (t >= 1.45) {
                s.z = -1;
            }
            s.speed = 1.5f;
        });
        Check(Only(h, {Action::DropDown}) && !d.Airborne(), "a drop off a ledge: dropdown, no hard landing", Names(h));
    }
    {
        LocalDetector d;
        const auto h = Run(d, 2, [](double t, Sample& s) {
            s.speed = 4.5f;
            s.cover = t > 1;
        });
        LocalDetector e;
        const auto g = Run(e, 2, [](double t, Sample& s) {
            s.speed = 1.0f;
            s.cover = t > 1;
        });
        LocalDetector f;
        const auto k = Run(f, 2, [](double, Sample& s) {
            s.speed = 4.5f;
            s.cover = true;  // already in cover at the first sample
        });
        Check(Only(h, {Action::CoverSlide}) && g.empty() && k.empty(),
              "into cover at a run: a slide; walking in, or already there: nothing",
              Names(h) + " / " + Names(g) + " / " + Names(k));
    }
    {  // medkit; body pickup by message and getter (one), carried with its key, dropped by the getter only
        LocalDetector d;
        actions::Pose carried;
        const auto h = Run(d, 4, [&](double t, Sample& s) {
            if (std::fabs(t - 0.5) < kDt / 2) s.events = traversal::kEvMedkit;
            if (std::fabs(t - 1.0) < kDt / 2) s.events = traversal::kEvPickup;
            s.carrying = t > 1.3 && t < 3;
            s.body = s.carrying ? 4711 : 0;
            if (t > 2 && t < 2.9) carried = d.CurrentPose();
        });
        Check(Only(h, {Action::Medkit, Action::PickupBody, Action::DropBody}) && (carried.bits & actions::kCarry) &&
                  carried.body == 4711 && !(d.CurrentPose().bits & actions::kCarry),
              "medkit, a pickup (message + getter = one), carrying with its key, a drop", Names(h));
    }
    {  // two in one sample: the second comes with the next sample
        LocalDetector d;
        const auto h = Run(d, 1, [](double t, Sample& s) {
            if (std::fabs(t - 0.5) < kDt / 2) s.events = traversal::kEvMedkit | traversal::kEvDrop;
        });
        Check(Only(h, {Action::Medkit, Action::DropBody}) && h.size() == 2 && std::fabs(h[1].t - h[0].t - kDt) < 1e-6,
              "two in one sample: one now, one with the next sample", Names(h));
    }
    {
        LocalDetector d;
        bool stealth = false;
        bool inCar = true;
        const auto h = Run(d, 2, [&](double t, Sample& s) {
            s.stealth = true;
            s.carrying = true;
            s.onFoot = t < 1;
            if (std::fabs(t - 1.5) < kDt / 2) s.events = traversal::kEvMedkit | traversal::kEvFence;
            if (t > 0.5 && t < 0.9) stealth = (d.CurrentPose().bits & actions::kStealth) != 0;
            if (t > 1.1) inCar &= d.CurrentPose().bits == 0;
        });
        Check(stealth && inCar && h.empty(), "sneak bit; in a car: no pose and nothing detected (the first sample's "
              "carrying is no pickup)", Names(h));
    }
}

// Getting on and off a motorbike, snaps of his height and the water (0.15.0 two-PC test: a bike's quick mount and
// dismount went out as jumps with vz 9-31 m/s).
void TestSnapsVehiclesWater() {
    {  // the mount: his height snaps up 0.6 m (smoothed vz ~15 m/s), then he is on the bike (a vehicle) for 2 s, then
       // off with a snap down; nothing is a move
        LocalDetector d;
        const auto h = Run(d, 5, [](double t, Sample& s) {
            if (t >= 1.0 && t < 1.06) {
                s.vz = 15;
                s.z = 0.6f * static_cast<float>((t - 1.0) / 0.06);
            } else if (t >= 1.06 && t < 3.0) {
                s.z = 0.6f;
                s.onFoot = false;
            } else if (t >= 3.0 && t < 3.05) {
                s.vz = -14;
                s.z = 0.6f - 0.6f * static_cast<float>((t - 3.0) / 0.05);
            }
            s.speed = t > 1 && t < 3 ? 20.0f : 0.0f;
        });
        Check(h.empty() && d.Snaps() >= 1, "getting on and off a bike: no jump, no drop (the mount is a snap)",
              Names(h) + " snaps " + std::to_string(d.Snaps()));
    }
    {  // off the bike: a hop 0.2 s later is still part of getting off (quiet); a jump 1.6 s later counts
        LocalDetector d;
        const auto h = Run(d, 5, [](double t, Sample& s) {
            s.onFoot = t >= 1.0;
            if (t < 1.0) s.z = 0.6f;
            float z1 = 0, vz1 = 0, z2 = 0, vz2 = 0;
            Ballistic(t, 1.2, 3.8f, z1, vz1);
            Ballistic(t, 2.6, 3.8f, z2, vz2);
            if (t >= 1.0) {
                s.z = z1 + z2;
                s.vz = vz1 + vz2;
            }
        });
        Check(Only(h, {Action::Jump}) && h[0].t >= 2.59 && h[0].t < 2.7,
              "off a vehicle: quiet for a moment, then jumps count again", Names(h));
    }
    {  // a snap up while on foot (a teleport, a mount the vehicle read missed): not a jump
        LocalDetector d;
        const auto h = Run(d, 3, [](double t, Sample& s) {
            if (t >= 1.0 && t < 1.05) s.vz = 18;
            s.z = t >= 1.05 ? 1.0f : 0.0f;
        });
        Check(h.empty() && d.Snaps() == 1, "a vertical speed of 18 m/s up: a snap, no jump",
              Names(h) + " snaps " + std::to_string(d.Snaps()));
    }
    {  // a snap down at the start of a "fall" (getting off something high): not a drop; a real fall still is
        LocalDetector d;
        const auto h = Run(d, 6, [](double t, Sample& s) {
            if (t >= 1.0 && t < 1.05) s.vz = -16;
            if (t >= 1.0) s.z = -1;
            if (t > 3 && t < 3.45) {
                s.vz = -9.81f * static_cast<float>(t - 3);
                s.z = -1 - 4.9f * static_cast<float>((t - 3) * (t - 3));
            } else if (t >= 3.45) {
                s.z = -2;
            }
        });
        Check(Only(h, {Action::DropDown}) && h[0].t > 3 && d.Snaps() == 1,
              "a snap down: no drop; a real fall later: a drop", Names(h));
    }
    {  // swimming: strokes and dives (vz +-3 m/s) are nothing; climbing out (2.5 m/s up) right after neither
        LocalDetector d;
        const auto h = Run(d, 6, [](double t, Sample& s) {
            s.inWater = t > 0.5 && t < 4;
            if (s.inWater) {
                s.vz = 3.0f * static_cast<float>(std::sin(t * 4));
                s.z = -0.75f * static_cast<float>(std::cos(t * 4));
                s.speed = 1.2f;
            } else if (t >= 4 && t < 4.4) {
                s.vz = 2.5f;
                s.z = static_cast<float>(t - 4) * 2.5f;
            } else if (t >= 4.4) {
                s.z = 1;
            }
        });
        Check(h.empty() && d.CurrentPose().bits == 0, "in the water and climbing out: nothing detected", Names(h));
    }
}

// ---- receiver -------------------------------------------------------------------------------------------------------
// A puppet that only moves by the commands' fixes (walking is not simulated: the worst case for catching up).
struct Pup {
    motion::Vec3 p;
    int snaps = 0, softened = 0;
    void Apply(const motion::LocomotionCommand& c, const motion::Vec3& target) {
        if (c.fix == motion::LocomotionCommand::Fix::Snap) {
            p = target;
            ++snaps;
        } else if (c.fix == motion::LocomotionCommand::Fix::Nudge) {
            p.x += c.nudge.x;
            p.y += c.nudge.y;
            p.z += c.nudge.z;
        }
    }
};

float Dist(const motion::Vec3& a, const motion::Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

motion::LocomotionCommand Step(motion::LocomotionController& lc, Catchup& cu, Pup& pup, const motion::Vec3& target,
                               int busy, Catchup::Info* info = nullptr) {
    motion::LocomotionInput in;
    in.target = target;
    in.haveCopy = busy == 0;
    in.copy = pup.p;
    in.dt = static_cast<float>(kDt);
    motion::LocomotionCommand c = lc.Update(in);
    const Catchup::Info i = cu.Apply(busy == 2, busy != 1, pup.p, target, static_cast<float>(kDt), c);
    if (i.softened) ++pup.softened;
    if (info) *info = i;
    pup.Apply(c, target);
    return c;
}

void TestCatchup() {
    {  // a vault: he goes 3 m forward and 0.5 m up and down in 0.8 s while the puppet plays it
        motion::LocomotionController lc;
        Catchup cu;
        Pup pup;
        float worst = 0;
        bool noWalk = true;
        for (int i = 0; i <= 48; ++i) {
            const float u = i / 48.0f;
            const motion::Vec3 target{3 * u, 0, 0.5f * std::sin(u * 3.14159f)};
            const motion::LocomotionCommand c = Step(lc, cu, pup, target, 2);
            noWalk &= !c.reissue && !c.turn && c.fix == motion::LocomotionCommand::Fix::Nudge;
            if (i > 5) worst = std::max(worst, Dist(pup.p, target));
        }
        char d[64];
        std::snprintf(d, sizeof d, "worst %.2f m, snaps %d", worst, pup.snaps);
        Check(worst < 0.45f && pup.snaps == 0 && noWalk && cu.Moves() == 1,
              "during a move: followed along his path (height too), no walk, no snap", d);
    }
    {  // the follow did not move it (the game ignored it): the move ends 5 m off, 1.2 m below
        motion::LocomotionController lc;
        Catchup cu;
        Pup pup;
        const motion::Vec3 target{5, 0, 1.2f};
        Catchup::Info info;
        for (int i = 0; i < 30; ++i) {
            motion::LocomotionInput in;
            in.target = target;
            in.dt = static_cast<float>(kDt);
            motion::LocomotionCommand c = lc.Update(in);
            cu.Apply(true, true, pup.p, target, static_cast<float>(kDt), c);  // not applied: the puppet stays
        }
        bool ended = false;
        float atHalf = 0;
        for (int i = 0; i < 60; ++i) {
            Step(lc, cu, pup, target, 0, &info);
            ended |= info.ended;
            if (i == 0) atHalf = Dist(pup.p, target);
        }
        const float eh = std::sqrt((target.x - pup.p.x) * (target.x - pup.p.x) + (target.y - pup.p.y) * (target.y - pup.p.y));
        char d[96];
        std::snprintf(d, sizeof d, "snaps %d softened %d first tick %.2f m end %.2f m dz %.2f", pup.snaps, pup.softened,
                      atHalf, eh, target.z - pup.p.z);
        // the last 0.9 m are the walk's (not simulated here)
        Check(ended && pup.snaps == 0 && pup.softened > 0 && atHalf > 3.0f && eh < 1.0f &&
                  std::fabs(target.z - pup.p.z) < 0.26f,
              "after a move 5 m off: blended in over ticks (no snap), the rest left to the walk", d);
    }
    {  // standing at the right spot after a ledge climb, but 1.5 m too low: the height is blended away
        motion::LocomotionController lc;
        Catchup cu;
        Pup pup;
        const motion::Vec3 target{0, 0, 1.5f};
        pup.p = {0, 0, 0};
        for (int i = 0; i < 10; ++i) Step(lc, cu, pup, target, 2);
        pup.p = {0, 0, 0};  // the game put it back on the ground
        for (int i = 0; i < 60; ++i) Step(lc, cu, pup, target, 0);
        char d[64];
        std::snprintf(d, sizeof d, "dz %.2f m, snaps %d", target.z - pup.p.z, pup.snaps);
        Check(std::fabs(target.z - pup.p.z) <= 0.26f && pup.snaps == 0, "after a move: the height blended away", d);
    }
    {  // far off after a move (10 m): that stays a snap
        motion::LocomotionController lc;
        Catchup cu;
        Pup pup;
        Step(lc, cu, pup, {0, 0, 0}, 2);
        Step(lc, cu, pup, {10, 0, 0}, 0);
        Check(pup.snaps == 1 && pup.softened == 0, "after a move but 10 m off: still a snap");
    }
    {  // later than the recovery time: snaps are snaps again, no height blending
        motion::LocomotionController lc;
        Catchup cu;
        Pup pup;
        Step(lc, cu, pup, {0, 0, 0}, 2);
        for (int i = 0; i < 120; ++i) Step(lc, cu, pup, {0, 0, 0}, 0);
        pup.p = {0, 0, -1};
        motion::LocomotionCommand c = Step(lc, cu, pup, {0, 0, 0}, 0);
        const bool noZ = std::fabs(c.nudge.z) < 1e-6f;
        Step(lc, cu, pup, {5, 0, 0}, 0);
        Check(pup.snaps == 1 && pup.softened == 0 && noZ, "after the recovery time: normal again (snap, no height blend)");
    }
    {  // without any move: the catch-up changes nothing
        motion::LocomotionController a, b;
        Catchup cu;
        Pup p1, p2;
        bool same = true;
        for (int i = 0; i < 200; ++i) {
            const motion::Vec3 target{0.05f * i, 0, 0};
            motion::LocomotionInput in;
            in.target = target;
            in.haveCopy = true;
            in.copy = p1.p;
            in.dt = static_cast<float>(kDt);
            const motion::LocomotionCommand c1 = a.Update(in);
            p1.Apply(c1, target);
            const motion::LocomotionCommand c2 = Step(b, cu, p2, target, 0);
            same &= c1.fix == c2.fix && c1.reissue == c2.reissue && std::fabs(c1.nudge.x - c2.nudge.x) < 1e-6f &&
                    std::fabs(c2.nudge.z) < 1e-6f;
        }
        Check(same, "no move: the controller's commands as they are");
    }
    {  // a punch (busy 1) is not a move: the paused controller's command stays as it is
        motion::LocomotionController lc;
        Catchup cu;
        Pup pup;
        motion::LocomotionCommand c = Step(lc, cu, pup, {2, 0, 0}, 1);
        Check(c.fix == motion::LocomotionCommand::Fix::None && cu.Moves() == 0 && pup.p.x == 0,
              "a punch (busy 1): not followed, nothing changed");
    }
    {  // a report stuck on "move" for longer than maxMove: the controller takes over again
        motion::LocomotionController lc;
        traversal::CatchupTuning t;
        t.maxMove = 1.0f;
        Catchup cu(t);
        Pup pup;
        Catchup::Info info;
        for (int i = 0; i < 90; ++i) Step(lc, cu, pup, {0.5f, 0, 0}, 2, &info);
        Check(!info.moving && cu.Moves() == 1, "a move reported for too long: no longer followed");
    }
}

}  // namespace

int main() {
    TestDetector();
    TestSnapsVehiclesWater();
    TestCatchup();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
