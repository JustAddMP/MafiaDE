// player_actions_test: unit tests for the action and pose blocks of PlayerState (src/game/player_actions.h): the wire
// encoding (round trip, a state from a build without the block, a truncated block, unknown kinds and pose bits, the
// traversal kinds), the sender's merge of the glue's counter with detected moves, the receiver's tracker
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
              FromCode(7) == Action::Jump && FromCode(17) == Action::DropBody && FromCode(18) == Action::None &&
              FromCode(255) == Action::None,
          "codes from the Lua sample");
    bool named = true;
    for (unsigned k = 1; k <= kLastAction; ++k) named &= std::string(Name(static_cast<Action>(k))) != "none";
    Check(named, "every kind has a name");
    Check(!IsMove(Action::HitReact) && IsMove(Action::Jump) && IsMove(Action::Vault) && IsMove(Action::LadderDown) &&
              IsMove(Action::CoverSlide) && !IsMove(Action::Medkit) && !IsMove(Action::PickupBody) &&
              !IsMove(Action::None),
          "traversal moves vs. actions on the spot");
    // a receiver from before the traversal kinds read kinds 7.. as unknown: the step moved its counter, it played
    // nothing; ours reads kinds beyond its own the same way
    unk = StateBytes(nullptr);
    unk.push_back(10);
    unk.push_back(40);
    Check(ReadState(unk, back) && back.seq == 10 && back.last == Action::None, "a kind beyond ours reads as none");
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

// The pose block after the action block: round trip, absent (older sender), truncated, unknown bits.
void TestPose() {
    auto bytes = [](const State& a, const Pose* p) {
        ByteWriter w;
        w.U8(42);
        w.F32(1.5f);
        Write(w, a);
        if (p) WritePose(w, *p);
        return w.Take();
    };
    auto read = [](const std::vector<uint8_t>& b, State& a, Pose& p) {
        ByteReader r(b.data(), b.size());
        if (r.U8() != 42 || r.F32() != 1.5f) return false;
        Read(r, a);
        ReadPose(r, p);
        return r.Ok() && r.AtEnd();
    };
    const State act{9, Action::Vault};
    bool ok = true;
    for (unsigned bits = 0; bits < 8; ++bits) {
        const Pose p{static_cast<uint8_t>(bits), (bits & kCarry) ? 0xDEADBEEFu : 0u};
        State a;
        Pose q;
        ok &= read(bytes(act, &p), a, q) && q == p && a.seq == 9 && a.last == Action::Vault;
    }
    Check(ok, "pose round trip, every bit combination");
    const Pose plain{kStealth | kLadder, 0};
    Check(bytes(act, &plain).size() == bytes(act, nullptr).size() + 1, "no body: the block is one byte");
    const Pose carry{kCarry, 7};
    Check(bytes(act, &carry).size() == bytes(act, nullptr).size() + 5, "carrying: one byte plus the body");
    {
        State a;
        Pose q{kCarry, 5};
        Check(read(bytes(act, nullptr), a, q) && q.bits == 0 && q.body == 0, "a state without the pose block: no pose");
    }
    {
        std::vector<uint8_t> b = bytes(act, nullptr);
        b.push_back(kCarry);
        b.push_back(1);  // body cut short
        ByteReader r(b.data(), b.size());
        r.U8();
        r.F32();
        State a;
        Pose q;
        Read(r, a);
        ReadPose(r, q);
        Check(r.Failed(), "a truncated pose block fails the state");
    }
    {
        std::vector<uint8_t> b = bytes(act, nullptr);
        b.push_back(0xF8 | kStealth);  // bits from a newer build
        State a;
        Pose q;
        Check(read(b, a, q) && q.bits == kStealth, "unknown pose bits are dropped");
    }
    {
        // an older reader (action block only) is not upset by the pose block
        const std::vector<uint8_t> b = bytes(act, &carry);
        ByteReader r(b.data(), b.size());
        r.U8();
        r.F32();
        State a;
        Read(r, a);
        Check(r.Ok() && r.Remaining() == 5 && a.last == Action::Vault, "an older reader stops before the pose block");
    }
}

// The sender's merge of the glue's counter and the detected moves.
void TestMerger() {
    Merger m;
    State out = m.Update({30, Action::LightPunch}, Action::None);
    Check(out.seq == 30 && out.last == Action::LightPunch, "the first sample takes the glue's counter as it is");
    out = m.Update({30, Action::LightPunch}, Action::None);
    Check(out.seq == 30, "nothing new: the counter stays");
    out = m.Update({30, Action::LightPunch}, Action::Jump);
    Check(out.seq == 31 && out.last == Action::Jump, "a detected move counts one step");
    out = m.Update({31, Action::HeavyPunch}, Action::None);
    Check(out.seq == 32 && out.last == Action::HeavyPunch, "a glue step after it still counts (its own counter)");
    out = m.Update({33, Action::Counter}, Action::Vault);
    Check(out.seq == 35 && out.last == Action::Vault, "both in one sample: all steps, the move is the latest");
    out = m.Update({33, Action::Counter}, Action::None);
    Check(out.seq == 35, "... and nothing replays");
    m.Update({120, Action::LightPunch}, Action::None);  // the glue went ahead (a burst of punches)
    out = m.Update({200, Action::LightPunch}, Action::None);
    m.Update({255, Action::LightPunch}, Action::None);
    out = m.Update({1, Action::LightPunch}, Action::None);
    Check(out.seq == static_cast<uint8_t>(35 + 87 + 80 + 55 + 2), "the glue's counter wraps at 256");
    // through to a receiver: every step is seen
    Merger s;
    Tracker t;
    t.Update(s.Update({0, Action::None}, Action::None));
    int plays = 0, moves = 0;
    for (int i = 1; i <= 20; ++i) {
        const State st = s.Update({static_cast<uint8_t>(i / 2), Action::LightPunch},
                                  i % 3 == 0 ? Action::LadderUp : Action::None);
        const Tracker::Result r = t.Update(st);
        if (r.play != Action::None) ++plays;
        if (r.play == Action::LadderUp) ++moves;
    }
    char d[64];
    std::snprintf(d, sizeof d, "plays %d moves %d", plays, moves);
    // 10 punches + 6 moves, 3 of them in the same sample as a punch (the move plays, the punch is a missed step)
    Check(plays == 13 && moves == 6, "punches and moves through the merger and the tracker", d);
}

}  // namespace

int main() {
    TestWire();
    TestTracker();
    TestLossyLink();
    TestPose();
    TestMerger();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
