// map_markers_test: unit tests for the other players' map markers (src/game/map_markers.h): which icon a player gets
// and what it follows in every state (on foot, in a car, no puppet, hidden, puppet re-created, human icon refused, no
// position yet), when a position icon moves, a whole session walk-through, and the name lines for the Lua side.
// Prints PASS/FAIL per check; exits non-zero on any failure.
#include <cstdio>
#include <string>

#include "game/map_markers.h"

using namespace mc::game::mapmark;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

std::string Str(const Plan& p) { return std::string(TargetName(p.want)) + "/" + ActionName(p.act); }

void Expect(const Input& in, Target want, Action act, const char* name) {
    const Plan p = Decide(in);
    Check(p.want == want && p.act == act, name,
          "got " + Str(p) + ", want " + TargetName(want) + "/" + ActionName(act));
}

Input At(float x, float y, float z) {
    Input in;
    in.hasPos = true;
    in.x = x;
    in.y = y;
    in.z = z;
    return in;
}

// the marker's state as player_sync.lua keeps it, updated from a plan the way the Lua side carries it out
struct Marker {
    Icon icon = Icon::None;
    Target target = Target::None;
    float lx = 0, ly = 0, lz = 0;
    int creates = 0, removes = 0, moves = 0, retargets = 0;
};

Action Step(Marker& m, Input in) {
    in.icon = m.icon;
    in.target = m.target;
    in.lastX = m.lx;
    in.lastY = m.ly;
    in.lastZ = m.lz;
    const Plan p = Decide(in);
    switch (p.act) {
        case Action::CreateHuman: m.icon = Icon::Human; ++m.creates; break;
        case Action::CreatePos: m.icon = Icon::Pos; ++m.creates; break;
        case Action::ReplaceWithHuman: m.icon = Icon::Human; ++m.creates; ++m.removes; break;
        case Action::ToPuppet: ++m.retargets; break;
        case Action::ToPos: ++m.retargets; break;
        case Action::MovePos: ++m.moves; break;
        default: break;
    }
    m.target = p.want;
    if (p.want == Target::Pos && p.act != Action::Keep) m.lx = in.x, m.ly = in.y, m.lz = in.z;
    return p.act;
}

void TestFirstIcon() {
    Input in = At(10, 20, 3);
    in.puppet = true;
    Expect(in, Target::Puppet, Action::CreateHuman, "first icon: puppet on foot -> human icon on it");
    in.puppet = false;
    Expect(in, Target::Pos, Action::CreatePos, "first icon: no puppet -> position icon");
    Input none;
    Expect(none, Target::None, Action::Keep, "first icon: no puppet, no position -> nothing");
    Input nopos;
    nopos.puppet = true;
    Expect(nopos, Target::Puppet, Action::CreateHuman, "first icon: puppet but no state position -> on the puppet");
    in.puppet = true;
    in.humanFailed = true;
    Expect(in, Target::Pos, Action::CreatePos, "first icon: human icon refused -> position icon instead");
    nopos.humanFailed = true;
    Expect(nopos, Target::None, Action::Keep, "first icon: human refused, no position -> wait");
}

void TestHumanIcon() {
    Input in = At(10, 20, 3);
    in.icon = Icon::Human;
    in.target = Target::Puppet;
    in.puppet = true;
    Expect(in, Target::Puppet, Action::Keep, "human icon on the puppet stays");
    in.puppetChanged = true;
    Expect(in, Target::Puppet, Action::ToPuppet, "puppet re-created -> icon moves to the new one");
    in.puppetChanged = false;
    in.puppet = false;  // gets in a car / hidden / gone
    Expect(in, Target::Pos, Action::ToPos, "puppet not shown (car, cutscene, gone) -> to his position");
    in.target = Target::Pos;
    in.lastX = 10, in.lastY = 20, in.lastZ = 3;
    Expect(in, Target::Pos, Action::Keep, "at his position, he did not move -> keep");
    in.x = 10.5f;
    Expect(in, Target::Pos, Action::Keep, "moved 0.5 m -> keep (below kMoveEps)");
    in.x = 40;
    Expect(in, Target::Pos, Action::MovePos, "moved 30 m -> move the icon");
    in.puppet = true;
    Expect(in, Target::Puppet, Action::ToPuppet, "puppet shown again -> icon back on the puppet (no second icon)");
    in.puppet = false;
    in.hasPos = false;
    Expect(in, Target::Pos, Action::Keep, "no position any more -> the icon stays where it is");
}

void TestPosIcon() {
    Input in = At(5, 5, 0);
    in.icon = Icon::Pos;
    in.target = Target::Pos;
    in.lastX = 5, in.lastY = 5;
    Expect(in, Target::Pos, Action::Keep, "position icon, no puppet -> keep");
    in.puppet = true;
    Expect(in, Target::Puppet, Action::ReplaceWithHuman, "position icon, puppet now -> replaced by the human icon");
    in.humanFailed = true;
    Expect(in, Target::Puppet, Action::ToPuppet, "position icon, human refused -> the position icon follows the puppet");
    in.target = Target::Puppet;
    Expect(in, Target::Puppet, Action::Keep, "position icon on the puppet stays");
    in.puppet = false;
    Expect(in, Target::Pos, Action::ToPos, "position icon on the puppet, puppet hidden -> to his position");
}

void TestSession() {
    Marker m;
    Input in;  // joined: no state yet
    Step(m, in);
    Check(m.icon == Icon::None && m.creates == 0, "session: nothing before his first state");
    in = At(1000, 2000, 10);  // far away, no puppet yet
    Step(m, in);
    Check(m.icon == Icon::Pos && m.target == Target::Pos, "session: far, no puppet -> position icon");
    in.x = 1010;
    Check(Step(m, in) == Action::MovePos, "session: he drives on -> moved");
    in.puppet = true;  // his puppet spawned
    Step(m, in);
    Check(m.icon == Icon::Human && m.target == Target::Puppet, "session: puppet spawned -> human icon on it");
    for (int i = 0; i < 3; ++i) {  // in and out of a car three times
        in.puppet = false;
        in.x += 50;
        Step(m, in);
        Check(m.target == Target::Pos, "session: in a car -> at the car's position");
        in.x += 50;
        Step(m, in);
        in.puppet = true;
        Step(m, in);
        Check(m.target == Target::Puppet, "session: out of the car -> on the puppet again");
    }
    in.puppet = false;  // dies, respawns
    Step(m, in);
    in.puppet = true;
    in.puppetChanged = true;  // a fresh puppet
    Step(m, in);
    Check(m.target == Target::Puppet, "session: respawned puppet -> icon on it");
    Check(m.creates == 2 && m.removes == 1, "session: two icons made, one removed in all",
          std::to_string(m.creates) + " created, " + std::to_string(m.removes) + " removed");
}

void TestNames() {
    Check(CleanName("Harry") == "Harry", "name: plain");
    Check(CleanName("  Tommy Angelo  ") == "Tommy Angelo", "name: spaces at the ends go, inside stay");
    Check(CleanName("a\nb\tc\rd") == "a b c d", "name: control characters -> spaces");
    Check(CleanName(" \n\t ").empty(), "name: only blanks -> empty");
    Check(CleanName(std::string(40, 'x')).size() == kMaxNameBytes, "name: cut to kMaxNameBytes");
    std::string u = std::string(31, 'x') + "\xc3\xa9" + "yy";  // the 2-byte e-acute straddles the limit
    const std::string cu = CleanName(u);
    Check(cu == std::string(31, 'x'), "name: never half a UTF-8 character", std::to_string(cu.size()) + " bytes");
    Check(CleanName(std::string(31, 'x') + " yyyy") == std::string(31, 'x'), "name: no space at the cut end");
    Check(NameLine(3, "Paulie") == "N 3 Paulie\n", "name line: N id name");
    Check(NameLine(4, " \n").empty(), "name line: empty name -> no line");
    Check(NameLine(7, "two\nlines") == "N 7 two lines\n", "name line: a newline cannot start a second command");
}

void TestNames2() {
    Check(IconFromName("human") == Icon::Human && IconFromName("pos") == Icon::Pos && IconFromName("x") == Icon::None &&
              IconFromName(nullptr) == Icon::None,
          "names: icon round trip");
    Check(TargetFromName(TargetName(Target::Puppet)) == Target::Puppet &&
              TargetFromName(TargetName(Target::Pos)) == Target::Pos && TargetFromName(nullptr) == Target::None,
          "names: target round trip");
}

}  // namespace

namespace {

// ---- three other players (a session of four): one marker each, each on its own ----
// Players 1 and 2 ride in the same car (both markers at the car), player 3 walks (his on his puppet); then player 2
// gets out and walks (his goes onto his puppet), player 1 stays in the car. One icon per player throughout, made once.
void TestThreeOthers() {
    Marker m[4];
    auto foot = [](float x, float y) {
        Input in = At(x, y, 5);
        in.puppet = true;
        return in;
    };
    auto inCar = [](float x, float y) { return At(x, y, 5); };  // in a car: no puppet to carry it, the car's spot
    for (int i = 0; i < 50; ++i) {
        const float cx = 100.0f + i * 3.0f;  // the car drives along
        Step(m[1], inCar(cx, 0));
        Step(m[2], inCar(cx, 0));
        Step(m[3], foot(-50, 20));
    }
    Check(m[1].icon == Icon::Pos && m[1].target == Target::Pos && m[2].icon == Icon::Pos && m[2].target == Target::Pos,
          "3 others: both passengers' markers follow the car");
    Check(m[1].lx == m[2].lx && m[1].moves > 10 && m[2].moves > 10, "3 others: the two car markers move with it");
    Check(m[3].icon == Icon::Human && m[3].target == Target::Puppet && m[3].moves == 0,
          "3 others: the walker's marker is on his puppet");
    for (int i = 0; i < 20; ++i) {
        Step(m[1], inCar(300, 0));
        Step(m[2], foot(305, 4));  // player 2 got out next to it
        Step(m[3], foot(-50, 20));
    }
    Check(m[2].icon == Icon::Human && m[2].target == Target::Puppet && m[1].target == Target::Pos,
          "3 others: the one who got out has his marker on his puppet, the other's stays at the car");
    bool once = true;
    for (int p = 1; p <= 3; ++p) once &= m[p].creates - m[p].removes == 1;
    Check(once, "3 others: one icon per player at any time");
    Check(NameLine(1, "Paulie") != NameLine(2, "Paulie") && NameLine(3, "Sam") == "N 3 Sam\n",
          "3 others: name lines keyed by player id (two players may share a name)");
}

// ---- the friend brain switch removes a human icon on the puppet (0.15.0: the host's marker missing on the guest) ----
void TestBrainPending() {
    Input in = At(10, 20, 3);
    in.puppet = true;
    in.brainPending = true;
    Expect(in, Target::Pos, Action::CreatePos, "brain: switch to come, no icon -> position icon, not on the puppet");
    in.icon = Icon::Pos;
    in.target = Target::Pos;
    in.lastX = 10, in.lastY = 20, in.lastZ = 3;
    Expect(in, Target::Pos, Action::Keep, "brain: switch to come, position icon -> it stays (no human icon yet)");
    in.x = 15;
    Expect(in, Target::Pos, Action::MovePos, "brain: switch to come, he walks -> the position icon follows him");
    in.brainPending = false;
    Expect(in, Target::Puppet, Action::ReplaceWithHuman, "brain: switch done -> the human icon on the puppet");
    Input h = At(10, 20, 3);
    h.puppet = true;
    h.icon = Icon::Human;
    h.target = Target::Puppet;
    h.brainPending = true;
    Expect(h, Target::Pos, Action::ToPos, "brain: human icon on a puppet whose switch is to come -> off it first");
    h.target = Target::Pos;
    h.brainPending = false;
    Expect(h, Target::Puppet, Action::ToPuppet, "brain: switch done -> the same icon back on the puppet");
    Input none;
    none.puppet = true;
    none.brainPending = true;
    Expect(none, Target::None, Action::Keep, "brain: switch to come, no position -> wait (nothing on the puppet)");
}

void TestProbe() {
    Check(CheckProbe(434, 434, 0) == Probe::Ok, "probe: the game has our id on the puppet -> ok");
    Check(CheckProbe(434, -1, 0) == Probe::Unknown, "probe: no check possible -> unknown (nothing done)");
    Check(CheckProbe(434, 0, 0) == Probe::Lost, "probe: no icon on the puppet -> lost (made anew)");
    Check(CheckProbe(434, 500, 0) == Probe::Lost, "probe: another icon on the puppet -> lost");
    Check(CheckProbe(434, 0, kMaxLosses - 1) == Probe::GiveUp, "probe: lost kMaxLosses times -> give up");
    Check(CheckProbe(434, 434, 5) == Probe::Ok, "probe: ok whatever was lost before");
    Check(std::string(ProbeName(Probe::Ok)) == "ok" && std::string(ProbeName(Probe::Lost)) == "lost" &&
              std::string(ProbeName(Probe::GiveUp)) == "give_up" && std::string(ProbeName(Probe::Unknown)) == "unknown",
          "probe: names for the Lua side");
}

// The 0.15.0 guest case end to end: the host's puppet spawns, its brain is switched 2 s later. Before the fix the human
// icon went on it at once and the switch removed it; now it waits at his position, then goes on the puppet once.
// A game icon that still disappears (probe Lost) is made anew, at most kMaxLosses times, then a position icon.
void TestBrainSession() {
    Marker m;
    Input in = At(-943, -233, 3);
    Step(m, in);  // no puppet yet
    Check(m.icon == Icon::Pos, "brain session: position icon before the puppet");
    in.puppet = true;
    in.brainPending = true;
    for (int t = 0; t < 10; ++t) Step(m, in);  // 2 s at 5 Hz: spawned, switch to come
    Check(m.icon == Icon::Pos && m.target == Target::Pos && m.creates == 1,
          "brain session: puppet spawned, switch to come -> still the position icon");
    in.brainPending = false;  // M.Ally switched it
    Step(m, in);
    Check(m.icon == Icon::Human && m.target == Target::Puppet && m.creates == 2 && m.removes == 1,
          "brain session: switch done -> the human icon on the puppet (position icon replaced)");
    for (int t = 0; t < 50; ++t) Step(m, in);
    Check(m.creates == 2 && m.target == Target::Puppet, "brain session: on foot afterwards -> kept, nothing made again");
    // the game removes it anyway: the probe sees it lost, the Lua side drops it (icon None) and it is made anew
    int losses = 0;
    for (int round = 0; round < 3; ++round) {
        const Probe pr = CheckProbe(23, 0, losses);
        if (pr == Probe::Ok || pr == Probe::Unknown) break;
        ++losses;
        m.icon = Icon::None, m.target = Target::None;
        if (pr == Probe::GiveUp) in.humanFailed = true;  // pp.mkFailed: no own icon for this puppet
        Step(m, in);
        if (pr == Probe::GiveUp) break;
    }
    Check(losses == kMaxLosses, "brain session: an icon lost again and again -> given up after kMaxLosses",
          std::to_string(losses));
    Check(m.icon == Icon::Pos && m.target == Target::Pos,
          "brain session: given up -> a position icon shows him (never nothing)");
}

}  // namespace

int main() {
    TestBrainPending();
    TestProbe();
    TestBrainSession();
    TestFirstIcon();
    TestHumanIcon();
    TestPosIcon();
    TestSession();
    TestNames();
    TestNames2();
    TestThreeOthers();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
