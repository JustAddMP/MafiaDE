// car_swap_test: unit tests for the swapped-car machine (src/game/car_swap.h): when a real car hidden behind a copy
// comes back. Scenarios: the host parks and gets out (his puppet climbs out first), gets back in before and after
// the CarParked, the local player in the copy, NPCs in it, the CarParked lost or before the exit, a destroyed car, a
// copy that failed to spawn, a load while swapped, the real car gone, the session ending. Prints PASS/FAIL per
// check; exits non-zero on any failure.
#include <cstdio>
#include <string>

#include "game/car_swap.h"

using namespace mc::game::carswap;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

// A swapped car as the Lua glue sees it, stepped at 60 Hz.
struct Sim {
    Machine m;
    double now = 100;
    uint32_t bits = kOrigValid | kCopyValid | kRemoteDriver;
    float speed = 8;
    double parkedAt = -1;
    bool first = true;
    Output last;
    void Set(uint32_t b, bool on) { bits = on ? (bits | b) : (bits & ~b); }
    Output Step() {
        Input in;
        in.now = now;
        in.bits = bits | (first ? kBegin : 0);
        in.copySpeed = speed;
        in.parkedAt = parkedAt;
        first = false;
        last = m.Step(in);
        return last;
    }
    // runs for `s` seconds; returns the first action taken (None if none) and when
    Action Run(double s, double* at = nullptr) {
        const double end = now + s;
        while (now < end) {
            now += 1.0 / 60;
            const Output o = Step();
            if (o.action != Action::None) {
                if (at) *at = now;
                return o.action;
            }
        }
        return Action::None;
    }
};

std::string Why(const Sim& s) { return std::string(PhaseName(s.last.phase)) + ": " + s.last.why; }

void TestHostParks() {
    Sim s;
    Check(s.Run(5) == Action::None && s.m.phase() == Phase::Driven && !s.last.lockCopy, "driven: nothing happens");
    // host gets out: his state no longer drives it; his puppet is still in the copy for 2.5 s (exit animation)
    s.Set(kRemoteDriver, false);
    s.Set(kRemoteAboard, true);  // seat 9: getting out
    s.Set(kPuppetInside, true);
    s.speed = 0.1f;
    const bool out = s.Run(0.5) == Action::None && s.m.phase() == Phase::Parking && s.last.lockCopy;
    Check(out, "getting out: parking, locked", Why(s));
    s.Set(kRemoteAboard, false);  // his state says on foot now
    s.parkedAt = s.now;            // CarParked arrives (1 s after the stop on his game)
    const bool waits = s.Run(1.5) == Action::None;
    Check(waits, "puppet still in the copy: waits", Why(s));
    s.Set(kPuppetInside, false);
    double at = 0;
    const double t0 = s.now;
    Check(s.Run(1, &at) == Action::Restore && at - t0 < 0.05, "puppet out + parked: restore at once");
    Check(s.m.phase() == Phase::Done && s.Run(2) == Action::None, "done: no second action");
}

void TestParkedBeforeExit() {
    Sim s;
    s.Run(2);
    s.parkedAt = s.now;  // reliable CarParked overtook the (unreliable) state that shows him on foot
    s.Run(0.5);
    s.Set(kRemoteDriver, false);
    s.speed = 0;
    double at = 0;
    const double t0 = s.now;
    Check(s.Run(1, &at) == Action::Restore && at - t0 < 0.05, "CarParked shortly before the exit still counts");
}

void TestStaleParked() {
    Sim s;
    s.parkedAt = s.now;  // an old park, then he drove on for a while
    s.Run(10);
    s.Set(kRemoteDriver, false);
    s.speed = 0;
    double at = 0;
    const double t0 = s.now;
    const Action a = s.Run(30, &at);
    Check(a == Action::Restore && at - t0 >= kParkFallback - 0.05 && at - t0 < kParkFallback + 0.1,
          "an older CarParked does not count: fallback after 10 s still", std::to_string(at - t0));
}

void TestBackInBeforePark() {
    Sim s;
    s.Run(2);
    s.Set(kRemoteDriver, false);
    s.speed = 0;
    Check(s.Run(3) == Action::None && s.m.phase() == Phase::Parking, "out, no CarParked yet: waits");
    s.Set(kRemoteDriver, true);  // gets back in
    s.speed = 5;
    Check(s.Run(30) == Action::None && s.m.phase() == Phase::Driven && !s.last.lockCopy,
          "back in before the restore: driven again, no restore");
    s.Set(kRemoteDriver, false);
    s.speed = 0;
    Check(s.Run(1) == Action::None, "out again: waits for the CarParked");
    s.parkedAt = s.now;
    Check(s.Run(1) == Action::Restore, "parks again: restored on that CarParked");
}

void TestLocalInCopy() {
    Sim s;
    s.Run(1);
    // the guest rode along as passenger: still in when the host gets out
    s.Set(kRemoteDriver, false);
    s.Set(kLocalInside, true);
    s.speed = 0;
    s.parkedAt = s.now;
    const bool sits = s.Run(40) == Action::None && !s.last.lockCopy;
    Check(sits, "we sit in the copy: no restore, not locked", Why(s));
    // he takes the wheel (shuffled over / got in at the driver's door)
    s.Set(kLocalDriver, true);
    s.speed = 10;
    Check(s.Run(20) == Action::None && s.m.phase() == Phase::Driven, "we drive the copy: driven");
    // he parks it: our game sends the CarParked (parkedAt), gets out
    s.speed = 0.1f;
    s.Set(kLocalDriver, false);
    Check(s.Run(1.2) == Action::None, "still inside after parking: waits");
    s.parkedAt = s.now;
    Check(s.Run(1) == Action::None, "our CarParked sent, still inside: waits");
    s.Set(kLocalInside, false);
    Check(s.Run(1) == Action::Restore, "out of the copy after our CarParked: restore");
}

void TestNpcInside() {
    Sim s;
    s.Run(1);
    s.Set(kRemoteDriver, false);
    s.Set(kOtherInside, true);
    s.speed = 0;
    s.parkedAt = s.now;
    double at = 0;
    const double t0 = s.now;
    const Action a = s.Run(20, &at);
    Check(a == Action::Restore && at - t0 >= kOtherWait - 0.05 && at - t0 < kOtherWait + 0.1,
          "an NPC in the copy is waited for 8 s", std::to_string(at - t0));
}

void TestNoParkedMessage() {
    Sim s;
    s.Run(1);
    s.Set(kRemoteDriver, false);
    s.speed = 3;  // rolls on
    double at = 0;
    double t0 = s.now;
    const Action a = s.Run(40, &at);
    Check(a == Action::Restore && at - t0 >= kHardCap - 0.05 && at - t0 < kHardCap + 0.1,
          "never at rest, no CarParked: restored after 20 s", std::to_string(at - t0));
    Sim r;
    r.Run(1);
    r.Set(kRemoteDriver, false);
    r.speed = 0;
    t0 = r.now;
    const Action b = r.Run(40, &at);
    Check(b == Action::Restore && at - t0 >= kParkFallback - 0.05 && at - t0 < kParkFallback + 0.1,
          "standing still, no CarParked: 10 s", std::to_string(at - t0));
}

void TestWrecked() {
    Sim s;
    s.Run(1);
    s.Set(kRemoteDriver, false);
    s.speed = 0;
    s.parkedAt = s.now;
    s.Set(kWrecked, true);
    Check(s.Run(1) == Action::RestoreWrecked, "destroyed on the driver's game: restore + explode");
    // the host died at the wheel: his puppet stays in, then the host's game reloads
    Sim d;
    d.Run(1);
    d.Set(kRemoteDriver, false);
    d.Set(kPuppetInside, true);
    d.speed = 0;
    d.parkedAt = d.now;
    d.Set(kWrecked, true);
    Check(d.Run(30) == Action::None, "puppet stuck in the wreck: waits");
    d.Set(kWorldGone, true);
    Check(d.Run(1) == Action::RestoreHere, "then the load: shown where it is");
}

void TestCopyFailed() {
    Sim s;
    s.Set(kCopyValid, false);
    s.Set(kCopyPending, true);
    Check(s.Run(3) == Action::None, "copy spawning: waits");
    s.Set(kCopyPending, false);
    Check(s.Run(1) == Action::Restore, "copy failed: restore at once (driver or not)");
    Sim p;
    p.Set(kCopyValid, false);
    p.Set(kCopyPending, true);
    p.Set(kRemoteDriver, false);
    p.Run(1);
    Check(p.last.lockCopy && p.m.phase() == Phase::Driven, "spawning, nobody drives: locked, no parking yet");
}

void TestLoad() {
    Sim s;
    s.Run(1);
    s.Set(kWorldGone, true);
    Check(s.Run(1) == Action::RestoreHere, "load while driven: real car shown where it is");
    Sim g;
    g.Run(1);
    g.Set(kWorldGone, true);
    g.Set(kOrigValid, false);
    Check(g.Run(1) == Action::Forget, "load, real car gone with the old world: forget");
}

void TestOrigGone() {
    Sim s;
    s.Run(1);
    s.Set(kOrigValid, false);
    Check(s.Run(1) == Action::Forget, "real car gone (despawned): forget");
}

void TestSessionOver() {
    Sim s;
    s.Run(1);
    s.Set(kRemoteDriver, false);  // puppets are removed with the session
    s.Set(kSessionOver, true);
    s.speed = 0;
    double at = 0;
    const double t0 = s.now;
    Check(s.Run(5, &at) == Action::Restore && at - t0 < 0.1, "session over, copy empty: restore at once");
    Sim l;
    l.Run(1);
    l.Set(kRemoteDriver, false);
    l.Set(kLocalDriver, true);
    l.Set(kLocalInside, true);
    l.Set(kSessionOver, true);
    Check(l.Run(30) == Action::None, "session over while we drive the copy: waits");
    l.Set(kLocalDriver, false);
    l.speed = 0;
    Check(l.Run(1) == Action::None, "parked, still inside: waits");
    l.Set(kLocalInside, false);
    Check(l.Run(1) == Action::Restore, "got out: restore (no CarParked needed)");
}

void TestBeginResets() {
    Sim s;
    s.Run(1);
    s.Set(kOrigValid, false);
    s.Run(1);
    Check(s.m.phase() == Phase::Done, "done after forget");
    s.Set(kOrigValid, true);
    s.first = true;  // swapped again
    s.Step();
    Check(s.m.phase() == Phase::Driven, "begin: a fresh machine");
}

void TestChanged() {
    Sim s;
    s.Step();
    const bool c1 = s.last.changed;
    s.now += 0.016;
    s.Step();
    Check(c1 && !s.last.changed, "changed only when the reason changes");
}

}  // namespace

namespace {

// ---- three players in one copy: driver + two passengers (a remote one and us, or two remote ones) ----
void TestThreeInCopy() {
    Sim s;
    s.Set(kRemoteAboard, true);  // another player rides along in it
    s.Set(kPuppetInside, true);
    s.Set(kLocalInside, true);   // and so do we
    s.Run(2);
    Check(s.m.phase() == Phase::Driven, "3 in the copy: driven");
    // the driver gets out and parks (CarParked); one passenger (a remote player) and we still sit in it
    s.Set(kRemoteDriver, false);
    s.speed = 0;
    s.parkedAt = s.now;
    Check(s.Run(30) == Action::None, "3 in the copy: driver out, two passengers still in: waits", Why(s));
    // we get out; the other passenger is still in
    s.Set(kLocalInside, false);
    Check(s.Run(30) == Action::None && s.last.lockCopy, "3 in the copy: we are out, a remote passenger stays: waits, locked",
          Why(s));
    // the remote passenger slides over to the wheel and drives off: driven again
    s.Set(kRemoteAboard, false);
    s.Set(kRemoteDriver, true);
    s.speed = 9;
    Check(s.Run(5) == Action::None && s.m.phase() == Phase::Driven, "3 in the copy: a passenger takes the wheel: driven");
    // he parks it and gets out: now it is empty: restore
    s.speed = 0;
    s.Set(kRemoteDriver, false);
    s.Set(kPuppetInside, false);
    s.parkedAt = s.now;
    Check(s.Run(3) == Action::Restore, "3 in the copy: empty after the last CarParked: restore", Why(s));
}

// Two remote passengers getting out one after the other while the driver already left: never restored (despawned)
// with anyone in it.
void TestTwoRemotePassengers() {
    Sim s;
    s.Set(kRemoteAboard, true);
    s.Set(kPuppetInside, true);
    s.Run(1);
    s.Set(kRemoteDriver, false);
    s.speed = 0;
    s.parkedAt = s.now;
    // passenger A is out (his state no longer names the car) but passenger B's puppet still sits in it
    Check(s.Run(20) == Action::None, "2 remote passengers: both in: waits");
    s.Set(kRemoteAboard, false);  // no state names it any more, but B's puppet is still seated (his exit not seen yet)
    Check(s.Run(3) == Action::None, "2 remote passengers: a puppet still inside: waits");
    s.Set(kPuppetInside, false);
    Check(s.Run(3) == Action::Restore, "2 remote passengers: all out: restore");
}

}  // namespace

int main() {
    TestHostParks();
    TestParkedBeforeExit();
    TestStaleParked();
    TestBackInBeforePark();
    TestLocalInCopy();
    TestNpcInside();
    TestNoParkedMessage();
    TestWrecked();
    TestCopyFailed();
    TestLoad();
    TestOrigGone();
    TestSessionOver();
    TestBeginResets();
    TestChanged();
    TestThreeInCopy();
    TestTwoRemotePassengers();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
