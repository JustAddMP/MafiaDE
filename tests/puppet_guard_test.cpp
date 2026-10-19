// puppet_guard_test: unit tests for src/game/puppet_guard.h. FastFoot: a player sent as on foot at motorbike speed
// (the 0.15.0 two-PC test: 10-32 m/s for 33 s) hides his puppet after a moment and shows it again when he is slow for a
// while; sprints, short spikes, a vehicle or the water never hide it. Shield: the hazards switch the puppet's demigod
// on at once and off after the hold time; his being down switches it off at once whatever else. Prints PASS/FAIL per
// check; exits non-zero on any failure. Built by build.cmd (out\puppet_guard_test.exe), run by tools\make_release.cmd.
#include <cstdio>
#include <functional>
#include <string>

#include "game/puppet_guard.h"

using namespace mc::game::puppet_guard;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-66s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

constexpr double kDt = 1.0 / 60;

// Runs FastFoot for `secs`; speed(t) and onFoot(t). Returns "hide@t show@t ..." and whether it was hidden at `probe`.
std::string RunFast(FastFoot& f, double secs, const std::function<float(double)>& speed,
                    const std::function<bool(double)>& onFoot, double probe, bool& hiddenAtProbe) {
    std::string out;
    hiddenAtProbe = false;
    for (int i = 0; i * kDt <= secs; ++i) {
        const double t = i * kDt;
        const FastFoot::Out o = f.Update(10 + t, onFoot(t), speed(t));
        if (o.changed) {
            char b[48];
            std::snprintf(b, sizeof b, "%s%s@%.2f", out.empty() ? "" : " ", o.hide ? "hide" : "show", t);
            out += b;
        }
        if (t <= probe + kDt / 2 && t > probe - kDt / 2) hiddenAtProbe = o.hide;
    }
    return out.empty() ? "-" : out;
}

}  // namespace

int main() {
    auto always = [](double) { return true; };
    {  // the 0.15.0 ride: walks to the bike, rides 20-32 m/s for 30 s, walks off
        FastFoot f;
        bool mid = false;
        const std::string s = RunFast(f, 40, [](double t) { return t < 2 ? 1.5f : t < 32 ? 25.0f : 1.5f; }, always,
                                      15, mid);
        Check(mid && s.rfind("hide@2.2", 0) == 0 && s.find("show@32.7") != std::string::npos && f.Times() == 1,
              "a bike ride sent as on foot: hidden after 0.25 s, shown 0.75 s after", s);
    }
    {  // sprinting at 8 m/s, and a 0.15 s spike to 20 m/s (a snap): never hidden
        FastFoot f;
        bool mid = false;
        const std::string s =
            RunFast(f, 10, [](double t) { return t > 5 && t < 5.15 ? 20.0f : 8.0f; }, always, 5.1, mid);
        Check(s == "-" && !mid, "a sprint and a short spike: never hidden", s);
    }
    {  // slow down to 10 m/s (between off and on): stays hidden; below 9 for 0.75 s: shown
        FastFoot f;
        bool mid = false;
        const std::string s = RunFast(f, 10, [](double t) { return t < 2 ? 20.0f : t < 5 ? 10.0f : 5.0f; }, always,
                                      4.5, mid);
        Check(mid && s.rfind("hide@0.2", 0) == 0 && s.find(" show@5.7") != std::string::npos && s.size() < 22,
              "hysteresis between 9 and 12 m/s", s);
    }
    {  // in a vehicle (or swimming) fast: not this guard's business; hidden on foot, shown the moment he is in one
        FastFoot f;
        bool mid = false;
        const std::string s = RunFast(f, 6, [](double) { return 25.0f; }, [](double t) { return t < 3; }, 4, mid);
        Check(!mid && s.rfind("hide@0.2", 0) == 0 && s.find(" show@3.0") != std::string::npos && s.size() < 22,
              "a vehicle: shown at once, never hidden in it", s);
    }
    {  // shield: on with a hazard at once, held 1.5 s after it, the reasons kept for the log
        Shield sh;
        bool on1 = sh.Update(1.0, kWater | kInWater).on;
        const bool held = sh.Update(2.0, 0).on && sh.Last() == (kWater | kInWater);
        const ShieldOut off = sh.Update(2.6, 0);
        Check(on1 && held && !off.on && off.changed, "shield: on with a hazard, held 1.5 s, then off");
        Check(HazardText(kSeat | kFall | kBlast) == "seat+fall+blast" && HazardText(0) == "none",
              "hazard names for the log", HazardText(kSeat | kFall | kBlast));
    }
    {  // his being down wins over everything: off at once (the network death must kill the puppet)
        Shield sh;
        sh.Update(1.0, kSeat);
        const ShieldOut d = sh.Update(1.1, kSeat | kWater | kDown);
        const ShieldOut back = sh.Update(1.2, kSeat);
        Check(!d.on && d.changed && back.on && back.changed, "his player down: shield off at once, on again after");
    }
    {  // nothing at all: never on
        Shield sh;
        bool any = false;
        for (int i = 0; i < 100; ++i) any |= sh.Update(i * 0.1, 0).on;
        Check(!any, "no hazard: never a demigod (its DAMAGE events carry the local player's hits)");
    }
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
