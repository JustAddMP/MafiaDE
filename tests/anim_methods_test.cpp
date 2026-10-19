// anim_methods_test: unit tests for how a puppet's action animation is played and checked (src/game/anim_methods.h):
// the action groups, judging one try from the measured hand movement, and the order in which the ways are tried
// (switch after two tries without movement, keep the one that moved, wrap round to the next animation name, lose a
// way that stops working). Prints PASS/FAIL per check; exits non-zero on any failure.
#include <cstdio>
#include <string>

#include "game/anim_methods.h"

using namespace mc::game::anim_methods;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

Probe P(float hand, float root = 0.02f, bool bones = true) {
    Probe p;
    p.bonesOk = bones;
    p.maxHand = hand;
    p.rootMove = root;
    p.firstMoveT = hand >= kStartMove ? 0.1f : -1.0f;
    return p;
}

void Groups() {
    Check(GroupOf(1) == Group::Melee && GroupOf(2) == Group::Melee && GroupOf(3) == Group::Melee &&
              GroupOf(4) == Group::Melee && GroupOf(6) == Group::Melee,
          "punches, counter, takedown, hit reaction are melee");
    Check(GroupOf(5) == Group::Throw, "a throw is its own group");
    bool moves = true;
    for (int k = 7; k <= 14; ++k) moves = moves && GroupOf(k) == Group::Move;
    Check(moves, "kinds 7-14 are traversal moves");
    Check(GroupOf(15) == Group::Other && GroupOf(18) == Group::Other && GroupOf(20) == Group::Other &&
              GroupOf(0) == Group::Other && GroupOf(99) == Group::Other,
          "medkit, bodies, doors and unknown kinds are other");
}

void Names() {
    bool round = true;
    for (int i = 0; i < kMethodCount; ++i) round = round && Parse(Name(static_cast<Method>(i))) == i;
    Check(round, "every method name parses back to its method");
    Check(Parse("direct") == 0 && Parse("animplay") == kMethodCount - 1, "first is direct, last the old AnimPlay");
    Check(Parse("dir") == -1 && Parse("directx") == -1 && Parse("") == -1 && Parse(nullptr) == -1,
          "partial, longer, empty and null names do not parse");
}

void Judging() {
    Check(Judge(P(0.5f)) == Verdict::Moved, "a hand 0.5 m off its rest: moved");
    Check(Judge(P(kMoved)) == Verdict::Moved, "exactly the moved threshold: moved");
    Check(Judge(P(0.03f)) == Verdict::Still, "3 cm (idle sway): still");
    Check(Judge(P(0.14f)) == Verdict::Unknown, "between still and moved: cannot tell");
    Check(Judge(P(0.5f, 1.0f)) == Verdict::Unknown, "hands moved but the puppet walked 1 m: cannot tell");
    Check(Judge(P(0.02f, 1.0f)) == Verdict::Unknown, "walking with still hands: cannot tell either");
    Check(Judge(P(0.5f, 0.02f, false)) == Verdict::Unknown, "no bones read: cannot tell");
}

void Order() {
    Ladder l;
    Check(l.Current() == Method::Direct && !l.Confirmed() && l.round == 0, "starts with the direct way, unconfirmed");
    Check(l.Report(Verdict::Still) == Event::None, "one still try: stays");
    Check(l.Report(Verdict::Still) == Event::Switched && l.Current() == Method::DirectNoTrans,
          "second still try: the next way");
    Check(l.Report(Verdict::Moved) == Event::Works && l.Confirmed(), "movement: that way works");
    Check(l.Report(Verdict::Moved) == Event::None, "more movement: nothing new");
    Check(l.Report(Verdict::Unknown) == Event::None && l.Report(Verdict::Unknown) == Event::None &&
              l.Report(Verdict::Unknown) == Event::None && l.Current() == Method::DirectNoTrans,
          "tries that cannot be judged never drop a way that worked");
    Check(l.Report(Verdict::Still) == Event::None && l.Report(Verdict::Still) == Event::None,
          "a working way survives two still tries");
    Check(l.Report(Verdict::Still) == Event::Lost && l.Current() == Method::Desire && !l.Confirmed(),
          "the third still try in a row loses it: the next way");
    Check(l.tries == 10, "every report counts as a try", std::to_string(l.tries));
}

void Unknowns() {
    Ladder l;
    Check(l.Report(Verdict::Unknown) == Event::None && l.Report(Verdict::Unknown) == Event::None,
          "two tries that cannot be judged: stays");
    Check(l.Report(Verdict::Unknown) == Event::Switched && l.Current() == Method::DirectNoTrans,
          "the third: the next way (so every way gets seen in a test without bones)");
    Ladder m;
    m.Report(Verdict::Still);
    m.Report(Verdict::Unknown);
    Check(m.Report(Verdict::Still) == Event::Switched, "an unknown between two still tries does not reset them");
}

void Wrap() {
    Ladder l;
    Event last = Event::None;
    for (int i = 0; i < kMethodCount * kStillToSwitch; ++i) last = l.Report(Verdict::Still);
    Check(last == Event::Wrapped && l.Current() == Method::Direct && l.round == 1,
          "nothing moved with any way: back to the first, round 1");
    for (int i = 0; i < kMethodCount * kStillToSwitch; ++i) l.Report(Verdict::Still);
    Check(l.round == 2, "and round 2 after the next pass");
}

void Forced() {
    Ladder l;
    l.Report(Verdict::Moved);
    Check(l.Confirmed(), "direct confirmed");
    l.Set(Parse("script"));
    Check(l.Current() == Method::Script && !l.Confirmed(), "the console forces script: unconfirmed");
    l.Set(-1);
    l.Set(kMethodCount);
    Check(l.Current() == Method::Script, "out of range is ignored");
    l.Set(0);
    Check(l.Confirmed(), "back to the confirmed one: still confirmed");
}

}  // namespace

int main() {
    Groups();
    Names();
    Judging();
    Order();
    Unknowns();
    Wrap();
    Forced();
    std::printf("%s (%d failure(s))\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
