// peer_pause_test: unit tests for a paused game (src/game/peer_pause.h): the PeerPaused wire format, the local
// detector (the script tick stops for the pause menu: a message on the pause, every 2 s while it lasts, and on the
// resume; nothing outside the world), and the registry the receivers ask (paused, then a 1 s grace after the resume,
// our own resume too). The 2026-10-19 two-PC test had 6-10 "knocked ... put back, reported to the host" corrections
// within 0.5 s after each of the host's pauses: replayed here as the host's pause timeline seen by a guest. Prints
// PASS/FAIL per check; exits non-zero on any failure.
#include "game/peer_pause.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::game::peer_pause;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

void TestWire() {
    net::ByteWriter w(8);
    Write(w, {true, Reason::NoTick, 8700});
    net::ByteReader r(w.Span());
    Msg m;
    Check(Read(r, m) && m.paused && m.reason == Reason::NoTick && m.pausedMs == 8700, "wire: pause round trip");
    Check(w.Size() == 6, "wire: 6 bytes", std::to_string(w.Size()));

    net::ByteWriter w2(8);
    Write(w2, {false, Reason::PausedTick, 1234});
    net::ByteReader r2(w2.Span());
    Msg m2;
    Check(Read(r2, m2) && !m2.paused && m2.reason == Reason::None && m2.pausedMs == 1234,
          "wire: a resume carries no reason, but how long it lasted");

    const uint8_t shortBody[3] = {1, 1, 0};
    net::ByteReader r3(shortBody, sizeof shortBody);
    Msg m3;
    Check(!Read(r3, m3), "wire: a short body is rejected");

    net::ByteWriter w4(16);
    Write(w4, {true, Reason::PausedTick, 5});
    w4.U32(0xDEADBEEF);  // a later addition
    net::ByteReader r4(w4.Span());
    Msg m4;
    Check(Read(r4, m4) && m4.paused && m4.reason == Reason::PausedTick, "wire: trailing bytes are tolerated");
}

void TestDetector() {
    LocalDetector d;
    Msg m;
    std::vector<Msg> sent;
    std::vector<double> sentAt;
    // 60 fps ticks for 5 s, then the pause menu for 8.7 s (no tick), then ticks again
    double lastTick = 0;
    for (double t = 1.0; t < 20.0; t += 1.0 / 60) {
        const bool ticking = t < 5.0 || t >= 13.7;
        if (ticking) lastTick = t;
        if (d.Update(t, lastTick, false, lastTick, true, m)) {
            sent.push_back(m);
            sentAt.push_back(t);
        }
    }
    Check(!sent.empty() && sent.front().paused && sent.front().reason == Reason::NoTick,
          "detector: the stopped tick is a pause");
    Check(!sentAt.empty() && sentAt.front() > 5.0 + kStallAfter - 0.02 && sentAt.front() < 5.0 + kStallAfter + 0.05,
          "detector: told 0.3 s after the last tick", sentAt.empty() ? "" : std::to_string(sentAt.front()));
    size_t repeats = 0;
    for (size_t i = 1; i < sent.size(); ++i)
        if (sent[i].paused) ++repeats;
    Check(repeats == 4, "detector: repeated every 2 s while paused (8.7 s: 4 repeats)", std::to_string(repeats));
    Check(!sent.empty() && !sent.back().paused && sent.back().pausedMs > 8600 && sent.back().pausedMs < 8800,
          "detector: the resume says how long", sent.empty() ? "" : std::to_string(sent.back().pausedMs));
    Check(!sentAt.empty() && sentAt.back() >= 13.7 && sentAt.back() < 13.75, "detector: the resume at the first tick");
    Check(!d.Paused() && d.ResumedAt() >= 13.7, "detector: running again");

    // a short hitch (0.2 s without a tick) is not a pause
    LocalDetector h;
    bool any = false;
    lastTick = 0;
    for (double t = 1.0; t < 3.0; t += 1.0 / 60) {
        if (t < 1.5 || t > 1.7) lastTick = t;
        any |= h.Update(t, lastTick, false, lastTick, true, m);
    }
    Check(!any, "detector: a 0.2 s hitch is not a pause");

    // outside the world (main menu, lobby) nothing is reported
    LocalDetector o;
    any = false;
    for (double t = 1.0; t < 5.0; t += 0.1) any |= o.Update(t, 0.9, false, 0.9, false, m);
    Check(!any, "detector: not in the world -> no pause");

    // no tick ever (before the first script tick) is not a pause
    LocalDetector z;
    Check(!z.Update(10, 0, false, 0, true, m), "detector: no tick yet -> no pause");

    // ticks through TickPaused only: a pause too, with its own reason
    LocalDetector p;
    sent.clear();
    for (double t = 1.0; t < 4.0; t += 1.0 / 60) {
        const double normal = t < 2.0 ? t : 2.0;
        if (p.Update(t, t, t >= 2.0, normal, true, m)) sent.push_back(m);
    }
    Check(!sent.empty() && sent.front().paused && sent.front().reason == Reason::PausedTick,
          "detector: only the paused tick runs -> paused");
    p.Reset();
    Check(!p.Paused(), "detector: reset");
}

void TestPeers() {
    Peers ps;
    Check(ps.Get(0, 1.0) == Quiet::None, "peers: unknown player -> none");
    Check(ps.Set(0, true, 10.0), "peers: a pause is a change");
    Check(!ps.Set(0, true, 12.0), "peers: a repeat is not");
    Check(ps.Get(0, 15.0) == Quiet::Paused, "peers: paused");
    Check(ps.Get(1, 15.0) == Quiet::None, "peers: the others are not");
    Check(ps.Set(0, false, 18.7), "peers: the resume is a change");
    Check(ps.Get(0, 19.0) == Quiet::Grace, "peers: grace 0.3 s after the resume");
    Check(ps.Get(0, 18.7 + kGrace + 0.01) == Quiet::None, "peers: none 1 s after the resume");
    ps.Set(2, true, 20);
    ps.Remove(2);
    Check(ps.Get(2, 21) == Quiet::None, "peers: a player who left is not paused");
    ps.LocalResumed(30);
    Check(ps.Local(30.5) == Quiet::Grace && ps.Local(31.1) == Quiet::None, "peers: our own resume: grace 1 s");
    ps.Set(0, true, 40);
    ps.Clear();
    Check(ps.Get(0, 41) == Quiet::None && ps.Local(30.5) == Quiet::None, "peers: cleared at the session end");

    // the global registry (what traffic / NPC / player sync ask)
    ClearRemotes();
    SetRemote(0, true, 100);
    Check(Remote(0, 101) == Quiet::Paused && For(0, 101) == Quiet::Paused, "registry: the host paused");
    SetRemote(0, false, 105);
    Check(For(0, 105.5) == Quiet::Grace && For(0, 106.1) == Quiet::None, "registry: grace after his resume");
    SetLocalResumed(200);
    Check(For(0, 200.4) == Quiet::Grace && For(3, 200.4) == Quiet::Grace, "registry: our own resume counts for all");
    Check(Local(201.5) == Quiet::None, "registry: and ends after 1 s");
    ClearRemotes();
}

// The host's pause from the guest's side, as in the two-PC test: the guest learns of it 0.3 s + one way after the
// host's last tick, holds the copies, and the knocks of the first second after the resume are not reported.
void TestTimeline() {
    LocalDetector host;
    Peers guest;
    const double oneWay = 0.03;
    double lastTick = 0;
    std::vector<std::pair<double, Msg>> inFlight;
    int knocksReported = 0, knocksQuiet = 0;
    for (double t = 0; t < 20.0; t += 1.0 / 60) {
        const bool ticking = t < 5.0 || t >= 13.7;  // the host's pause menu, 8.7 s
        if (ticking) lastTick = t;
        Msg m;
        if (host.Update(t, lastTick, false, lastTick, true, m)) inFlight.push_back({t + oneWay, m});
        for (auto it = inFlight.begin(); it != inFlight.end();) {
            if (it->first <= t) {
                guest.Set(0, it->second.paused, t);
                it = inFlight.erase(it);
            } else {
                ++it;
            }
        }
        // the guest's copies are judged knocked in the 0.5 s after the host resumes (what the test logs had)
        if (t >= 13.7 && t < 14.2 && static_cast<int>(t * 60) % 6 == 0) {
            if (guest.Get(0, t) == Quiet::None)
                ++knocksReported;
            else
                ++knocksQuiet;
        }
    }
    Check(knocksReported == 0 && knocksQuiet > 0, "timeline: no knock reported in the 0.5 s after the host's resume",
          std::to_string(knocksQuiet) + " quiet");
    Check(guest.Get(0, 20.0) == Quiet::None, "timeline: business as usual afterwards");
}

}  // namespace

int main() {
    TestWire();
    TestDetector();
    TestPeers();
    TestTimeline();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
