// water_test: unit tests for src/game/water.h, the sender's swim / under water tracker: into the water by the
// IN_DEEP_WATER message, a dive and back up, climbing out onto a shore, a vehicle, a teleport, repeating messages that
// stop, a getter that rules, a message while dry on land. Prints PASS/FAIL per check; exits non-zero on any failure.
// Built by build.cmd (out\water_test.exe), run by tools\make_release.cmd.
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "game/water.h"

using namespace mc::game;
using water::Sample;
using water::State;
using water::Tracker;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-66s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

constexpr double kDt = 1.0 / 60;

// Runs the tracker over `secs` at 60 Hz; `at(t, s)` fills each sample. Returns the states at the given times and the
// list of changes ("swim@1.00 under@2.30 ...").
struct Run {
    std::string changes;
    std::vector<State> at;
};
Run Go(Tracker& w, double secs, const std::function<void(double, Sample&)>& fill,
       const std::vector<double>& probes = {}) {
    Run r;
    r.at.assign(probes.size(), State::Dry);
    for (int i = 0; i * kDt <= secs; ++i) {
        const double t = i * kDt;
        Sample s;
        s.t = 50 + t;
        fill(t, s);
        const water::Change c = w.Update(s);
        if (c.changed) {
            char b[64];
            std::snprintf(b, sizeof b, "%s%s@%.2f", r.changes.empty() ? "" : " ", water::Name(c.to), t);
            r.changes += b;
        }
        for (size_t k = 0; k < probes.size(); ++k)
            if (std::fabs(t - probes[k]) < kDt / 2) r.at[k] = w.Now();
    }
    return r;
}

bool Msg(double t, double when) { return std::fabs(t - when) < kDt / 2; }

}  // namespace

int main() {
    {  // jumps off a pier (z 2) into the sea (surface 0): the message at the surface, swims, dives 3 m, comes up,
       // swims to a ladder and climbs out 2 m up
        Tracker w;
        const Run r = Go(w, 14, [](double t, Sample& s) {
            if (t < 1) s.z = 2;
            else if (t < 1.4) s.z = 2 - 5 * static_cast<float>(t - 1);  // the fall into the water
            else if (t < 4) s.z = -0.1f * static_cast<float>(std::sin(t * 3));
            else if (t < 6) s.z = -3;                                    // dived
            else if (t < 10) s.z = -0.05f;
            else if (t < 11) s.z = -0.05f + 2.05f * static_cast<float>(t - 10);  // up the ladder
            else s.z = 2;
            s.deepWater = Msg(t, 1.4);
        }, {2.0, 5.0, 8.0, 12.0});
        Check(r.at[0] == State::Swim && r.at[1] == State::Under && r.at[2] == State::Swim && r.at[3] == State::Dry,
              "pier, swim, dive, up again, out by a ladder", r.changes);
    }
    {  // swims, then drives off in a boat (a vehicle): dry at once
        Tracker w;
        const Run r = Go(w, 4, [](double t, Sample& s) {
            s.z = 0;
            s.deepWater = Msg(t, 0.5);
            s.onFoot = t < 2;
        }, {1.0, 2.5});
        Check(r.at[0] == State::Swim && r.at[1] == State::Dry, "into a boat: dry", r.changes);
    }
    {  // a respawn far away (height jumps 40 m) while swimming: dry
        Tracker w;
        const Run r = Go(w, 3, [](double t, Sample& s) {
            s.z = t < 1.5 ? 0.0f : 40.0f;
            s.deepWater = Msg(t, 0.5);
        }, {1.0, 2.0});
        Check(r.at[0] == State::Swim && r.at[1] == State::Dry, "teleport while swimming: dry", r.changes);
    }
    {  // the message repeats every second while he swims, then stops (he waded out on a gentle beach, no rise)
        Tracker w;
        const Run r = Go(w, 16, [](double t, Sample& s) {
            s.z = 0;
            for (double m = 0.5; m < 5; m += 1.0)
                if (Msg(t, m)) s.deepWater = true;
        }, {3.0, 9.0, 12.0});
        Check(r.at[0] == State::Swim && r.at[1] == State::Swim && r.at[2] == State::Dry,
              "repeating messages that stop: dry after twice the gap", r.changes);
    }
    {  // one message only, he stays in the water without repeats: still swimming 30 s later
        Tracker w;
        const Run r = Go(w, 31, [](double t, Sample& s) {
            s.z = 0.1f * static_cast<float>(std::sin(t));
            s.deepWater = Msg(t, 0.5);
        }, {30.0});
        Check(r.at[0] == State::Swim, "a single message: swims until something says otherwise", r.changes);
    }
    {  // a swim getter answers: it rules over height and messages
        Tracker w;
        const Run r = Go(w, 4, [](double t, Sample& s) {
            s.z = 5;
            s.getter = t < 1 ? 0 : t < 2 ? 1 : t < 3 ? 2 : 0;
            s.deepWater = Msg(t, 0.5);  // ignored: the getter says dry
        }, {0.6, 1.5, 2.5, 3.5});
        Check(r.at[0] == State::Dry && r.at[1] == State::Swim && r.at[2] == State::Under && r.at[3] == State::Dry,
              "a getter rules", r.changes);
    }
    {  // small waves of +-0.6 m at the surface: no dive, no climb-out
        Tracker w;
        const Run r = Go(w, 10, [](double t, Sample& s) {
            s.z = 0.6f * static_cast<float>(std::sin(t * 2));
            s.deepWater = Msg(t, 0.5);
        }, {9.0});
        Check(r.at[0] == State::Swim && r.changes == "swim@0.50", "bobbing at the surface: swims", r.changes);
    }
    {  // under water the dive hysteresis: down to 1.3 m (under), up to 1.0 m (still under), 0.5 m (surfaced)
        Tracker w;
        const Run r = Go(w, 5, [](double t, Sample& s) {
            s.z = t < 1 ? 0.0f : t < 2 ? -1.3f : t < 3 ? -1.0f : -0.5f;
            s.deepWater = Msg(t, 0.5);
        }, {1.5, 2.5, 3.5});
        Check(r.at[0] == State::Under && r.at[1] == State::Under && r.at[2] == State::Swim, "dive hysteresis",
              r.changes);
    }
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
