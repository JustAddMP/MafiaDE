// player_actions_test: unit tests for the action block of PlayerState (src/game/player_actions.h): the wire encoding
// (round trip, a state from a build without the block, a truncated block, unknown kinds), the receiver's tracker
// (first state adopts the counter, one play per step, wrap-around at 256, an older state does not replay, missed
// steps play only the latest kind) and a run of states through a lossy, reordering link. Prints PASS/FAIL per check;
// exits non-zero on any failure. Built by build.cmd (out\player_actions_test.exe), run by tools\make_release.cmd.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "game/player_actions.h"

using namespace mc::game::actions;
using mc::net::ByteReader;
using mc::net::ByteWriter;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-60s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

// The tail of a PlayerState as player_sync.cpp writes it: some earlier field, then the action block.
std::vector<uint8_t> StateBytes(const State* act) {
    ByteWriter w;
    w.U8(42);          // stands for everything before the block (the vehicle block included)
    w.F32(1.5f);
    if (act) Write(w, *act);
    return w.Take();
}

bool ReadState(const std::vector<uint8_t>& b, State& out) {
    ByteReader r(b.data(), b.size());
    if (r.U8() != 42 || r.F32() != 1.5f) return false;
    Read(r, out);
    return r.Ok() && r.AtEnd();
}

void TestWire() {
    bool ok = true;
    for (unsigned seq = 0; seq < 256; ++seq)
        for (unsigned k = 0; k <= kLastAction; ++k) {
            State s{static_cast<uint8_t>(seq), static_cast<Action>(k)};
            State back;
            ok &= ReadState(StateBytes(&s), back) && back.seq == s.seq && back.last == s.last;
        }
    Check(ok, "round trip of every counter and kind");
    const State zero;
    Check(StateBytes(nullptr).size() + 2 == StateBytes(&zero).size(), "the block is two bytes");

    State back{7, Action::Throw};
    Check(ReadState(StateBytes(nullptr), back) && back.seq == 0 && back.last == Action::None,
          "a state without the block (older build) reads as no action");

    std::vector<uint8_t> cut = StateBytes(nullptr);
    cut.push_back(5);  // counter only, kind missing
    {
        ByteReader r(cut.data(), cut.size());
        r.U8();
        r.F32();
        State s;
        Read(r, s);
        Check(r.Failed(), "a truncated block fails the state");
    }

    std::vector<uint8_t> unk = StateBytes(nullptr);
    unk.push_back(9);
    unk.push_back(200);  // a kind from a newer build
    Check(ReadState(unk, back) && back.seq == 9 && back.last == Action::None, "an unknown kind reads as none");

    // an older reader stops before the block: the two extra bytes are simply left over (player_sync.cpp's Read
    // checks Ok(), not AtEnd())
    State s{3, Action::LightPunch};
    const std::vector<uint8_t> b = StateBytes(&s);
    ByteReader r(b.data(), b.size());
    r.U8();
    r.F32();
    Check(r.Ok() && r.Remaining() == 2, "an older reader is not upset by the block");

    Check(FromCode(0) == Action::None && FromCode(1) == Action::LightPunch && FromCode(6) == Action::HitReact &&
              FromCode(7) == Action::None && FromCode(255) == Action::None,
          "codes from the Lua sample");
    Check(std::string(Name(Action::HeavyPunch)) == "heavy" && std::string(Name(Action::None)) == "none",
          "names for the log and the trace");
}

void TestTracker() {
    Tracker t;
    Tracker::Result r = t.Update({200, Action::HeavyPunch});
    Check(r.play == Action::None && t.Have() && t.Seen() == 200, "the first state adopts the counter, plays nothing");
    r = t.Update({200, Action::HeavyPunch});
    Check(r.play == Action::None, "the same counter again plays nothing");
    r = t.Update({201, Action::LightPunch});
    Check(r.play == Action::LightPunch && r.missed == 0, "one step plays its kind once");
    r = t.Update({201, Action::LightPunch});
    Check(r.play == Action::None, "... and only once");
    r = t.Update({200, Action::HeavyPunch});
    Check(r.play == Action::None && t.Seen() == 201, "an older state (counter back) does not replay");
    r = t.Update({204, Action::Throw});
    Check(r.play == Action::Throw && r.missed == 2, "missed steps: the latest kind plays, the rest are counted");
    t.Update({255, Action::Counter});
    r = t.Update({0, Action::LightPunch});
    Check(r.play == Action::LightPunch && r.missed == 0, "the counter wraps at 256");
    r = t.Update({1, Action::None});
    Check(r.play == Action::None && t.Seen() == 1, "a step with an unknown kind moves the counter, plays nothing");
    t.Reset();
    r = t.Update({90, Action::LightPunch});
    Check(r.play == Action::None && t.Seen() == 90, "after Reset the next counter is adopted again");
}

// Ten punches 0.4 s apart, states at 60 Hz, every third state lost and some pairs swapped: every punch plays once.
void TestLossyLink() {
    struct Sent {
        double t;
        State s;
    };
    std::vector<Sent> sent;
    State cur{17, Action::None};
    int done = 0;
    for (int i = 0; i < 300; ++i) {
        const double t = i / 60.0;
        if (done < 10 && t >= 0.2 + done * 0.4) {
            cur.seq = static_cast<uint8_t>(cur.seq + 1);
            cur.last = done % 3 == 2 ? Action::HeavyPunch : Action::LightPunch;
            ++done;
        }
        sent.push_back({t, cur});
    }
    std::vector<State> got;
    for (size_t i = 0; i < sent.size(); ++i) {
        if (i % 3 == 1) continue;  // lost
        got.push_back(sent[i].s);
    }
    for (size_t i = 5; i + 1 < got.size(); i += 7) std::swap(got[i], got[i + 1]);  // reordered
    Tracker t;
    t.Update({17, Action::None});
    int plays = 0, heavy = 0, missed = 0;
    for (const State& s : got) {
        const Tracker::Result r = t.Update(s);
        if (r.play != Action::None) ++plays;
        if (r.play == Action::HeavyPunch) ++heavy;
        missed += r.missed;
    }
    char d[96];
    std::snprintf(d, sizeof d, "plays %d heavy %d missed %d", plays, heavy, missed);
    Check(plays == 10 && heavy == 3 && missed == 0, "lossy, reordering link: every punch plays once", d);

    // a burst of states lost for longer than two punches: the counter still says how many were missed
    Tracker u;
    u.Update({50, Action::None});
    const Tracker::Result r = u.Update({53, Action::HeavyPunch});
    Check(r.play == Action::HeavyPunch && r.missed == 2, "a long gap: the latest plays, two counted as missed");
}

}  // namespace

int main() {
    TestWire();
    TestTracker();
    TestLossyLink();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
