// model_change_test: unit tests for character changes without a puppet per step (src/game/model_change.h): the
// picker's Outbox (sent once stable, when the panel closes, at once outside a world) and the receiver's Gate (a lone
// change at once, a burst collapsed into its last value, a step back cancels). Replays the two bursts of the 0.15.0
// two-PC test (2026-10-19) through the Gate as an old sender sent them. Prints PASS/FAIL per check; exits non-zero on
// any failure.
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "game/model_change.h"

using namespace mc::game::modelchange;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

struct Look {
    std::string model, look;
    bool operator==(const Look& o) const { return model == o.model && look == o.look; }
};

// Runs the gate from t0 to t1 in 1/60 s ticks with the offers given (time, model); returns the models applied.
std::vector<std::string> Run(Gate<Look>& g, const std::vector<std::pair<double, std::string>>& offers, double t0,
                             double t1) {
    std::vector<std::string> applied;
    size_t next = 0;
    for (double t = t0; t <= t1; t += 1.0 / 60) {
        while (next < offers.size() && offers[next].first <= t) {
            g.Offer({offers[next].second, offers[next].second + "_look"}, t);
            ++next;
        }
        Look v;
        if (g.Poll(t, &v)) applied.push_back(v.model);
    }
    return applied;
}

std::string Join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ",") + x;
    return s;
}

void TestOutbox() {
    // panel open: five steps 0.5 s apart, then the player stays: one send, 1 s after the last step
    Outbox o;
    int sends = 0;
    double sentAt = -1;
    for (double t = 0; t <= 6; t += 1.0 / 60) {
        o.PanelShown(t);
        for (int i = 0; i < 5; ++i)
            if (t >= i * 0.5 && t - 1.0 / 60 < i * 0.5) o.Picked(t);
        const char* why = nullptr;
        if (o.Due(t, true, &why)) {
            ++sends;
            sentAt = t;
        }
    }
    Check(sends == 1, "outbox: five steps with the panel open give one send", std::to_string(sends));
    Check(sentAt > 2.99 && sentAt < 3.05, "outbox: sent 1 s after the last step", std::to_string(sentAt));
    Check(o.LastSteps() == 5, "outbox: the send folds the five steps", std::to_string(o.LastSteps()));

    // panel closed 0.3 s after the last step: sent once the panel is gone (0.25 s without a frame)
    Outbox c;
    c.PanelShown(0);
    c.Picked(0);
    c.PanelShown(0.3);
    Check(!c.Due(0.31, true), "outbox: not yet while the panel is open");
    const char* why = nullptr;
    Check(!c.Due(0.5, true), "outbox: 0.2 s after the last panel frame: still open");
    const bool due = c.Due(0.56, true, &why);
    Check(due && why && std::string(why) == "panel-closed", "outbox: panel closed: sent at once", why ? why : "-");
    Check(!c.Due(0.6, true), "outbox: only once");

    // outside a world (the lobby): at once
    Outbox l;
    l.PanelShown(0);
    l.Picked(0);
    Check(l.Due(0, false, &why) && std::string(why) == "no-world", "outbox: lobby (no world): at once");

    // a join sends the current pick: the pending one is folded into it
    Outbox j;
    j.PanelShown(0);
    j.Picked(0);
    j.SentWithOther();
    j.PanelShown(0.1);
    Check(!j.Pending() && !j.Due(5, true), "outbox: a pick sent with a join is not sent again");
    Check(!Outbox().Due(0, true), "outbox: nothing picked, nothing sent");
}

void TestGate() {
    {
        Gate<Look> g;
        auto a = Run(g, {{0.0, "billy"}}, 0, 3);
        Check(Join(a) == "billy", "gate: a lone change applies", Join(a));
        Gate<Look> h;
        h.Offer({"billy", ""}, 0);
        Look v;
        Check(h.Poll(0, &v) && v.model == "billy", "gate: a lone change applies in the same tick");
    }
    {
        // session 1, 20:50:55-57 on the guest (old sender, every step): BILLY, TOMMY, BILLY, BERTONE, NORMAN
        Gate<Look> g;
        auto a = Run(g, {{0.0, "billy"}, {0.92, ""}, {1.34, "billy"}, {1.52, "bertone"}, {2.08, "norman"}}, 0, 6);
        Check(Join(a) == "billy,norman", "gate: 0.15.0 burst 1: two puppets instead of five", Join(a));
    }
    {
        // session 1, 20:51:12-14: BERTONE, NORMAN, SERGIO, MORELLO, SERGIO (puppet seated in the guest's car)
        Gate<Look> g;
        g.Offer({"x", ""}, -30);
        Look v;
        g.Poll(-30, &v);
        auto a = Run(g, {{0.0, "bertone"}, {0.52, "norman"}, {1.02, "sergio"}, {1.28, "morello"}, {1.63, "sergio"}},
                     0, 6);
        Check(Join(a) == "bertone,sergio", "gate: 0.15.0 burst 2: two puppets instead of five", Join(a));
    }
    {
        // new sender: one PlayerModel per change, changes far apart: each applies at once
        Gate<Look> g;
        auto a = Run(g, {{0.0, "paulie"}, {10.0, "sam"}, {25.0, "salieri"}}, 0, 30);
        Check(Join(a) == "paulie,sam,salieri", "gate: changes far apart apply one by one", Join(a));
    }
    {
        // a step away and back while the first is applied: nothing more to do
        Gate<Look> g;
        auto a = Run(g, {{0.0, "paulie"}, {0.5, "sam"}, {0.8, "paulie"}}, 0, 5);
        Check(Join(a) == "paulie", "gate: away and back again cancels the change", Join(a));
        Check(!g.Waiting() && g.Applied().model == "paulie", "gate: nothing waits afterwards");
    }
    {
        // the same value again (a resend on a join) changes nothing
        Gate<Look> g;
        auto a = Run(g, {{0.0, "paulie"}, {0.3, "paulie"}, {5.0, "paulie"}}, 0, 8);
        Check(Join(a) == "paulie", "gate: the same value again is no change", Join(a));
    }
    {
        // the burst value is held until it is stable for 1 s, and Newest() shows it meanwhile (lobby list)
        Gate<Look> g;
        g.Offer({"a", ""}, 0);
        Look v;
        g.Poll(0, &v);
        g.Offer({"b", ""}, 0.5);
        Check(!g.Poll(0.6, &v), "gate: inside a burst: held");
        Check(g.Newest() && g.Newest()->model == "b", "gate: Newest() is the held value");
        double held = 0;
        int skipped = -1;
        Check(!g.Poll(1.49, &v), "gate: held for kApplyStable");
        Check(g.Poll(1.51, &v, &held, &skipped) && v.model == "b", "gate: applied once stable");
        Check(held > 1.0 && skipped == 0, "gate: held time and skipped count reported",
              std::to_string(held) + " / " + std::to_string(skipped));
    }
}

}  // namespace

int main() {
    TestOutbox();
    TestGate();
    std::printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
