// car_look_test: unit tests for the car look sync (src/game/car_look.h): the CarLook message (0x0253) both ways, the
// line form the Lua glue uses (plates with spaces), what a copy must change, and the Book through whole scenarios: the
// driver's car is copied with its colours, plate and tuning; a look that changes; a copy that does not take it; the
// host's traffic answered on request; nothing ever goes back to the car itself. Prints PASS/FAIL per check; exits
// non-zero on any failure.
#include <cstdio>
#include <string>

#include "game/car_look.h"

using namespace mc::game::car_look;
using mc::game::car_damage::CarRef;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

Look Sedan() {
    Look l;
    l.model = "smith_v12";
    l.color[0] = 3;
    l.color[1] = 7;
    l.plate = "ZK-299";
    l.tint = 1;
    l.interior = 2;
    l.dirt = 50;
    l.rust = 0;
    l.tuningKnown = true;
    l.tuning = {12, 40, 77};
    return l;
}

Look OpenCar() {  // what ObtainSpecificCar gave the other game in the 0.15.0 test
    Look l;
    l.model = "smith_v12";
    l.color[0] = 21;
    l.color[1] = 21;
    l.plate = "NY 858";
    l.tint = 0;
    l.interior = 1;
    l.dirt = 0;
    l.rust = 0;
    l.tuningKnown = true;
    l.tuning = {12, 41, 77};
    return l;
}

Message RoundTrip(const Message& m, bool* ok = nullptr) {
    const auto b = Write(m);
    Message back;
    const bool good = Read(b.data(), b.size(), back);
    if (ok) *ok = good;
    return back;
}

void TestWire() {
    Message m;
    CarRef::Parse("1:4242", m.ref);
    m.seq = 513;
    m.look = Sedan();
    m.look.plate = "NY 858";
    m.look.painting = "paint_curves02---d";
    bool ok = false;
    Message b = RoundTrip(m, &ok);
    Check(ok, "look round trip reads");
    Check(b.op == Message::kLook && b.ref == m.ref && b.seq == 513, "op, ref, seq");
    Check(b.look == m.look, "the whole look", b.look.Describe());
    Message q;
    q.op = Message::kRequest;
    CarRef::Parse("t17", q.ref);
    b = RoundTrip(q, &ok);
    Check(ok && b.op == Message::kRequest && b.ref.kind == CarRef::kTraffic && b.ref.id == 17, "request round trip");
    Check(Write(q).size() == 4, "a request is 4 bytes", std::to_string(Write(q).size()));
    // unknown tuning travels as unknown
    m.look.tuningKnown = false;
    m.look.tuning.clear();
    b = RoundTrip(m, &ok);
    Check(ok && !b.look.tuningKnown && b.look.tuning.empty(), "tuning unknown");
    // truncated / bad bodies
    auto bytes = Write(m);
    bytes.pop_back();
    Message bad;
    Check(!Read(bytes.data(), bytes.size(), bad), "truncated look rejected");
    const uint8_t op9[] = {9, 0, 1, 1, 0, 0, 0};
    Check(!Read(op9, sizeof op9, bad), "unknown op rejected");
    m.look.model.clear();
    bytes = Write(m);
    Check(!Read(bytes.data(), bytes.size(), bad), "a look without model rejected");
    // a sender's oversized strings are cut (never more than the limits)
    m.look = Sedan();
    m.look.plate = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    b = RoundTrip(m, &ok);
    Check(ok && b.look.plate.size() == kMaxPlate, "a long plate is cut to 16", b.look.plate);
}

void TestLine() {
    Look l = OpenCar();
    l.painting = "paint 1%";
    const std::string s = ToLine(l);
    Check(s.find("NY%20858") != std::string::npos, "a plate's space is escaped", s);
    Look back;
    Check(FromLine(s, back) && back == l, "line round trip", back.Describe());
    Look u;
    u.model = "bolt_ace";
    Check(FromLine(ToLine(u), back) && back == u && ToLine(u).find(" - ") != std::string::npos, "unknowns as '-'",
          ToLine(u));
    u.tuningKnown = true;
    Check(FromLine(ToLine(u), back) && back.tuningKnown && back.tuning.empty(), "known and no tuning as '.'",
          ToLine(u));
    Look dash;
    dash.model = "x";
    dash.plate = "-";
    Check(FromLine(ToLine(dash), back) && back.plate == "-", "a plate that is '-' survives", ToLine(dash));
    Check(!FromLine("smith_v12 3 7", back), "too few tokens rejected");
    Check(!FromLine("smith_v12 3 x ZK-299 1 2 50 0 - 1,2", back), "a bad number rejected");
    Check(!FromLine("smith_v12 3 7 ZK-299 1 2 50 0 - 1,a", back), "a bad tuning id rejected");
    Check(!FromLine("- 3 7 ZK-299 1 2 50 0 - .", back), "no model rejected");
}

void TestDiff() {
    Change c = Diff(OpenCar(), Sedan());
    Check(c.color && c.plate && c.tint && c.interior && c.dirt && !c.rust && !c.painting, "what differs", c.Describe());
    Check(c.tuning && c.tuningIds.size() == 1 && c.tuningIds[0] == 40, "only the tuning item the copy lacks",
          c.Describe());
    Check(Diff(Sedan(), Sedan()).Empty(), "same look: nothing");
    Look unknown;
    unknown.model = "smith_v12";
    Check(Diff(OpenCar(), unknown).Empty(), "what the car's game could not read is left alone");
    Look have = OpenCar();
    have.tuningKnown = false;
    c = Diff(have, Sedan());
    Check(c.tuning && c.tuningIds.size() == 3, "copy's tuning unknown: every item installed", c.Describe());
}

struct Two {
    Book a, b;  // A (player 0) has the car; B (player 1) a copy
    double now = 50;
    Outbox oa, ob;
    void Deliver() {
        for (const auto& m : oa.broadcast) b.OnLook(RoundTrip(m), 0, now);
        for (const auto& [to, m] : oa.to) if (to == 1) b.OnLook(RoundTrip(m), 0, now);
        for (const auto& [to, m] : oa.requests) if (to == 1) b.OnRequest(RoundTrip(m).ref, 0, now);
        for (const auto& m : ob.broadcast) a.OnLook(RoundTrip(m), 1, now);
        for (const auto& [to, m] : ob.to) if (to == 0) a.OnLook(RoundTrip(m), 1, now);
        for (const auto& [to, m] : ob.requests) if (to == 0) a.OnRequest(RoundTrip(m).ref, 1, now);
    }
};

CarInput In(const char* ref, uint32_t ent, Role role, int owner, const Look& have, bool read = true) {
    CarInput in;
    CarRef::Parse(ref, in.ref);
    in.entity = ent;
    in.role = role;
    in.owner = owner;
    in.have = have;
    in.read = read;
    return in;
}

// what the game does with an Apply on B's copy: `takes` false = the game refuses the tuning (it stays)
void Carry(const Outbox& o, Look& copy, bool takesTuning = true) {
    for (const Apply& a : o.applies) {
        const Change& c = a.change;
        if (c.color) {
            copy.color[0] = a.want.color[0];
            copy.color[1] = a.want.color[1];
        }
        if (c.plate) copy.plate = a.want.plate;
        if (c.tint) copy.tint = a.want.tint;
        if (c.interior) copy.interior = a.want.interior;
        if (c.dirt) copy.dirt = a.want.dirt;
        if (c.rust) copy.rust = a.want.rust;
        if (c.painting) copy.painting = a.want.painting;
        if (c.tuning && takesTuning) copy.tuning = a.want.tuning;
    }
}

void Step(Two& t, const Look& car, Look& copy, bool takesTuning = true, const char* ref = "0:5000") {
    t.oa = {};
    t.ob = {};
    t.a.Round({In(ref, 11, Role::Source, -1, car)}, t.now, t.oa);
    t.b.Round({In(ref, 22, Role::Copy, 0, copy)}, t.now, t.ob);
    Carry(t.ob, copy, takesTuning);
    t.Deliver();
    t.now += 0.5;
}

void TestBookCopy() {
    Two t;
    Look car = Sedan(), copy = OpenCar();
    Step(t, car, copy);
    Check(t.oa.broadcast.size() == 1, "the car's game sends its look once");
    Step(t, car, copy);
    Check(copy == car, "the copy shows the car's look", copy.Describe());
    const uint64_t sent = t.a.GetStats().sent;
    for (int i = 0; i < 8; ++i) Step(t, car, copy);
    Check(t.a.GetStats().sent == sent, "nothing sent while nothing changes");
    Check(t.b.GetStats().sent == 0 && t.b.GetStats().answers == 0, "the copy's game never sends a look back");
    Check(t.b.GetStats().applied == 1, "set once", std::to_string(t.b.GetStats().applied));
    // the driver changes it (a paint shop): sent again, the copy follows
    car.color[0] = 9;
    for (int i = 0; i < 4; ++i) Step(t, car, copy);
    Check(copy.color[0] == 9 && t.a.GetStats().sent == sent + 1, "a change is sent and shown");
    // the source never takes a look (B's broadcast would be ignored by its role anyway)
    Message echo;
    CarRef::Parse("0:5000", echo.ref);
    echo.seq = 99;
    echo.look = OpenCar();
    t.a.OnLook(echo, 1, t.now);
    Outbox o;
    t.a.Round({In("0:5000", 11, Role::Source, -1, car)}, t.now, o);
    Check(o.applies.empty(), "the car itself is never changed by a look");
}

void TestBookLate() {
    // the copy is made after the look went out: it asks the car's game
    Two t;
    Look car = Sedan(), copy = OpenCar();
    t.oa = {};
    t.a.Round({In("0:5000", 11, Role::Source, -1, car)}, t.now, t.oa);  // broadcast lost (B had no copy, joined later)
    Two late;
    late.a = t.a;
    late.now = t.now + 1;
    late.ob = {};
    late.b.Round({In("0:5000", 22, Role::Copy, 0, copy)}, late.now, late.ob);
    Check(late.ob.requests.size() == 1 && late.ob.requests[0].first == 0, "a copy without a look asks the car's game");
    late.Deliver();
    late.oa = {};
    late.a.Round({In("0:5000", 11, Role::Source, -1, car)}, late.now, late.oa);
    Check(late.oa.to.size() == 1 && late.oa.to[0].first == 1 && late.oa.broadcast.empty(), "and gets an answer");
    late.Deliver();
    late.now += 0.5;
    late.ob = {};
    late.b.Round({In("0:5000", 22, Role::Copy, 0, copy)}, late.now, late.ob);
    Carry(late.ob, copy);
    Check(copy == car, "then shows it");
}

void TestBookRefused() {
    // the game does not take the tuning (UNVERIFIED in game): three tries, then given up (logged), colours stay set
    Two t;
    Look car = Sedan(), copy = OpenCar();
    for (int i = 0; i < 20; ++i) Step(t, car, copy, false);
    Check(t.b.GetStats().applied == 3, "three tries", std::to_string(t.b.GetStats().applied));
    Check(t.b.GetStats().failed == 1 && copy.color[0] == 3 && copy.plate == "ZK-299", "then given up, the rest set");
    // a new copy entity of the car: tried again
    t.ob = {};
    t.now += 1;
    t.b.Round({In("0:5000", 33, Role::Copy, 0, copy)}, t.now, t.ob);
    Check(t.ob.applies.size() == 1 && t.ob.applies[0].entity == 33, "a new copy entity is tried again");
}

void TestBookOwnCopy() {
    // our own car hidden behind a copy on our own game (another player drives it): the copy takes our car's look
    Book a;
    Outbox o;
    Look car = Sedan(), copy = OpenCar();
    a.Round({In("0:5000", 11, Role::Source, -1, car), In("0:5000", 44, Role::Copy, 0, copy)}, 10, o);
    Check(o.broadcast.size() == 1 && o.applies.size() == 1 && o.applies[0].entity == 44 && o.requests.empty(),
          "our own copy of our car gets its look", o.applies.empty() ? "" : o.applies[0].change.Describe());
}

void TestBookTraffic() {
    // host traffic: never broadcast, answered when a guest asks (by entity on the host, net id on the wire)
    Book host, guest;
    Outbox oh, og;
    Look car = Sedan(), copy = OpenCar();
    guest.Round({In("t17", 4242, Role::Copy, 0, copy)}, 10, og);
    Check(og.requests.size() == 1 && og.requests[0].second.ref.kind == CarRef::kTraffic, "the guest asks for net 17");
    host.OnRequest(CarRef{CarRef::kHostTraffic, 0, 99850}, 1, 10);  // the network edge translated the net id
    const auto wanted = host.Wanted();
    Check(wanted.size() == 1 && wanted[0].id == 99850, "the host knows which car to read");
    host.Round({In("h99850", 99850, Role::Source, -1, car)}, 10, oh);
    Check(oh.broadcast.empty() && oh.to.size() == 1 && oh.to[0].first == 1, "answered to the guest only");
    Check(host.Wanted().empty(), "and not asked any more");
    Message m = oh.to[0].second;
    m.ref = CarRef{CarRef::kTraffic, 0, 17};
    guest.OnLook(RoundTrip(m), 0, 10.2);
    og = {};
    guest.Round({In("t17", 4242, Role::Copy, 0, copy)}, 10.5, og);
    Check(og.applies.size() == 1 && og.applies[0].entity == 4242, "the traffic copy gets the look");
}

bool Holds(const Outbox& o, uint32_t ent) {
    for (uint32_t e : o.holds)
        if (e == ent) return true;
    return false;
}

void TestBookOccupied() {
    // a puppet sits in our copy (or the copy seat-reload put beside his car, once he is in it): the tuning would
    // rebuild the car under him, so it waits; colours and plate are set at once
    Two t;
    Look car = Sedan(), copy = OpenCar();
    t.oa = {};
    t.a.Round({In("0:5000", 11, Role::Source, -1, car)}, t.now, t.oa);
    t.Deliver();
    bool tuningSent = false;
    for (int i = 0; i < 12; ++i) {
        t.ob = {};
        CarInput in = In("0:5000", 22, Role::Copy, 0, copy);
        in.busy = true;
        t.b.Round({in}, t.now, t.ob);
        for (const Apply& a : t.ob.applies) tuningSent = tuningSent || a.change.tuning;
        Carry(t.ob, copy);
        t.now += 0.5;
    }
    Check(!tuningSent, "no tuning set while someone sits in the copy");
    Check(copy.color[0] == car.color[0] && copy.plate == car.plate && copy.tuning != car.tuning,
          "colours and plate set at once, the tuning still the copy's own", copy.Describe());
    Check(t.b.GetStats().deferred == 1 && t.b.GetStats().failed == 0 && t.b.GetStats().applied == 1,
          "counted as waiting, not as a failed look", std::to_string(t.b.GetStats().applied));
    // he gets out: the tuning is set now, with tries of its own
    t.ob = {};
    t.b.Round({In("0:5000", 22, Role::Copy, 0, copy)}, t.now, t.ob);
    Check(t.ob.applies.size() == 1 && t.ob.applies[0].change.tuning && !t.ob.applies[0].change.color,
          "once the copy is empty its tuning is set", t.ob.applies.empty() ? "" : t.ob.applies[0].change.Describe());
    Carry(t.ob, copy);
    t.now += 0.5;
    t.ob = {};
    t.b.Round({In("0:5000", 22, Role::Copy, 0, copy)}, t.now, t.ob);
    Check(copy == car && t.ob.applies.empty() && t.b.GetStats().deferred == 0, "then the copy shows the whole look");
    // a busy copy whose look change is only colours: set at once (no rebuild)
    Look car2 = car;
    car2.color[1] = 12;
    t.oa = {};
    t.now += 2;
    t.a.Round({In("0:5000", 11, Role::Source, -1, car2)}, t.now, t.oa);
    t.Deliver();
    t.ob = {};
    CarInput in = In("0:5000", 22, Role::Copy, 0, copy);
    in.busy = true;
    t.b.Round({in}, t.now, t.ob);
    Check(t.ob.applies.size() == 1 && t.ob.applies[0].change.color && !t.ob.applies[0].change.tuning,
          "a colour change is set on an occupied copy at once");
}

void TestBookHold() {
    // a new copy of a player car: the puppet is not seated in it before its look (with the tuning) is on
    Two t;
    Look car = Sedan(), copy = OpenCar();
    t.ob = {};
    t.b.Round({In("0:5000", 22, Role::Copy, 0, copy)}, t.now, t.ob);
    Check(Holds(t.ob, 22), "a new copy without its look is held back from the puppet");
    t.oa = {};
    t.a.Round({In("0:5000", 11, Role::Source, -1, car)}, t.now, t.oa);
    t.Deliver();
    t.now += 0.5;
    t.ob = {};
    t.b.Round({In("0:5000", 22, Role::Copy, 0, copy)}, t.now, t.ob);
    Check(Holds(t.ob, 22) && !t.ob.applies.empty() && t.ob.applies[0].change.tuning, "held while its tuning is set");
    Carry(t.ob, copy);
    t.now += 0.5;
    t.ob = {};
    t.b.Round({In("0:5000", 22, Role::Copy, 0, copy)}, t.now, t.ob);
    Check(!Holds(t.ob, 22), "free once it shows the look");
    // a tuning the game refuses holds the puppet back holdMax s at most
    Two r;
    Look copy2 = OpenCar();
    bool heldEarly = false, heldLate = false;
    const double start = r.now;
    for (int i = 0; i < 20; ++i) {
        Step(r, car, copy2, false);
        const double at = r.now - 0.5 - start;
        if (at >= BookConfig{}.holdMax && Holds(r.ob, 22)) heldLate = true;
        if (at > 0.4 && at < 2 && Holds(r.ob, 22)) heldEarly = true;
    }
    Check(heldEarly && !heldLate, "a tuning that does not take holds the puppet back at most holdMax s");
    // an occupied copy is never held (he sits in it already); traffic copies are not held (no puppet seats there)
    Outbox o;
    CarInput busy = In("0:6000", 55, Role::Copy, 0, OpenCar());
    busy.busy = true;
    t.b.Round({busy, In("t9", 66, Role::Copy, 0, OpenCar())}, t.now, o);
    Check(!Holds(o, 55) && !Holds(o, 66), "occupied and traffic copies are not held");
}

}  // namespace

int main() {
    TestWire();
    TestLine();
    TestDiff();
    TestBookCopy();
    TestBookLate();
    TestBookRefused();
    TestBookOwnCopy();
    TestBookTraffic();
    TestBookOccupied();
    TestBookHold();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
