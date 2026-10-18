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

int main() {
    TestFirstIcon();
    TestHumanIcon();
    TestPosIcon();
    TestSession();
    TestNames();
    TestNames2();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
