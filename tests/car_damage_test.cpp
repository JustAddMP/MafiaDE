// car_damage_test: unit tests for the car damage sync logic (src/game/car_damage.h): quantizing, the wire format,
// the authority's diff, the raise-only merge a copy applies, repair detection, what a copy reports, and the Book
// (both roles) through whole scenarios: the driver dents his car, the copy follows; a guest shoots the host's tyres;
// a repair at a garage; a car that changes hands; a copy bumped while pinned; an explosion. Prints PASS/FAIL per
// check; exits non-zero on any failure.
#include <cstdio>
#include <string>

#include "game/car_damage.h"

using namespace mc::game::car_damage;

namespace {

int g_failures = 0;

std::pair<uint8_t, uint8_t> P(int i, int v) { return {static_cast<uint8_t>(i), static_cast<uint8_t>(v)}; }

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

Snapshot Car(size_t parts = 10, size_t wheels = 4) {
    Snapshot s;
    std::vector<int32_t> types;
    for (size_t i = 0; i < parts; ++i) types.push_back(static_cast<int32_t>(i % 5 + 1));
    s.layout = Layout(types);
    s.parts.assign(parts, 0);
    s.wheels.assign(wheels, kWheelOk);
    return s;
}

Update RoundTrip(const Update& u, bool* ok = nullptr) {
    const auto b = Write(u);
    Update back;
    const bool good = Read(b.data(), b.size(), back);
    if (ok) *ok = good;
    return back;
}

void TestQuantize() {
    Check(Quantize(1.0f, 1.0f) == 0, "intact part is 0");
    Check(Quantize(0.0f, 1.0f) == kMaxDamage, "empty part is 254");
    Check(Quantize(0.5f, 2.0f) == 191, "a quarter left is 191", std::to_string(Quantize(0.5f, 2.0f)));
    Check(Quantize(5.0f, 1.0f) == 0 && Quantize(-1.0f, 1.0f) == kMaxDamage, "out of range clamps");
    Check(Quantize(0.5f, 0.0f) == 0 && Quantize(NAN, 1.0f) == 0, "bad maximum / energy reads as intact");
    const float e = EnergyOf(127, 300.0f);
    Check(Quantize(e, 300.0f) == 127, "EnergyOf inverts Quantize", std::to_string(e));
    Check(EnergyOf(kGone, 5.0f) == 0.0f && EnergyOf(kMaxDamage, 5.0f) == 0.0f, "gone / empty -> energy 0");
}

void TestRef() {
    CarRef r;
    Check(CarRef::Parse("3:56330", r) && r.kind == CarRef::kPlayer && r.owner == 3 && r.id == 56330, "parse player key");
    Check(r.Text() == "3:56330", "player key text");
    Check(CarRef::Parse("t17", r) && r.kind == CarRef::kTraffic && r.id == 17 && r.Text() == "t17", "parse traffic net");
    Check(CarRef::Parse("h99850", r) && r.kind == CarRef::kHostTraffic && r.id == 99850, "parse host traffic id");
    Check(!CarRef::Parse("t70000", r) && !CarRef::Parse("x1", r) && !CarRef::Parse("3:", r) && !CarRef::Parse("300:5", r) &&
              !CarRef::Parse("3:0", r) && !CarRef::Parse("", r),
          "bad refs rejected");
}

void TestWire() {
    Update u;
    CarRef::Parse("1:4242", u.ref);
    u.epoch = 7;
    u.flags = kBurning;
    u.burnLeft = 3.25f;
    u.layout = 0xDEADBEEF;
    u.partCount = 40;
    u.wheels = {kWheelOk, kWheelBurst, kWheelOff, kWheelOk, kWheelBurst};
    u.parts = {P(0, 10), P(7, kGone), P(39, 254)};
    u.full = true;
    bool ok = false;
    Update b = RoundTrip(u, &ok);
    Check(ok, "update round trip reads");
    Check(b.ref == u.ref && b.epoch == 7 && b.flags == kBurning && b.full && !b.repaired, "ref, epoch, flags");
    Check(b.burnLeft > 3.24f && b.burnLeft < 3.26f, "burn time (1/100 s)", std::to_string(b.burnLeft));
    Check(b.layout == u.layout && b.partCount == 40 && b.wheels == u.wheels, "layout, count, wheels (2 bits each)");
    Check(b.parts == u.parts, "parts");
    Check(Write(u).size() == 1 + 1 + 4 + 2 + 1 + 2 + 4 + 1 + 1 + 2 + 1 + 6, "compact size",
          std::to_string(Write(u).size()));
    // traffic ref, out-of-range part dropped on write
    Update t;
    CarRef::Parse("t300", t.ref);
    t.partCount = 2;
    t.parts = {P(1, 5), P(9, 9)};
    t.repaired = true;
    Update tb = RoundTrip(t, &ok);
    Check(ok && tb.ref.kind == CarRef::kTraffic && tb.ref.id == 300 && tb.parts.size() == 1 && tb.repaired,
          "traffic ref, part beyond count dropped");
    // malformed
    auto bytes = Write(u);
    Update x;
    Check(!Read(bytes.data(), bytes.size() - 1, x), "truncated message rejected");
    bytes[0] = 7;
    Check(!Read(bytes.data(), bytes.size(), x), "unknown ref kind rejected");
    auto req = WriteRequest(u.ref);
    CarRef rr;
    Check(ReadRequest(req.data(), req.size(), rr) && rr == u.ref && req.size() == 6, "request round trip");
}

void TestDiffMerge() {
    Snapshot a = Car(), b = Car();
    Check(Diff(a, b).empty(), "no change, no diff");
    b.parts[2] = 1;  // rounding from 0 counts (leaving intact)
    b.parts[3] = 0;
    Check(Diff(a, b).size() == 1, "leaving intact is sent");
    a.parts[4] = 100;
    b.parts[4] = 101;
    Check(Diff(a, b).size() == 1, "one quantum of change is rounding");
    b.parts[4] = 110;
    b.parts[5] = kGone;
    Check(Diff(a, b).size() == 3, "real changes sent");
    Snapshot other = Car(12);
    other.parts[0] = 3;
    Check(Diff(a, other).size() == 1, "other layout: every damaged part");

    Snapshot into = Car();
    into.parts[1] = 50;
    Update u;
    u.layout = into.layout;
    u.partCount = 10;
    u.parts = {P(1, 20), P(2, 90), P(3, kGone)};
    u.wheels = {kWheelOk, kWheelBurst};
    u.flags = kBurning;
    u.burnLeft = 4;
    Check(MergeUp(into, u), "merge rises");
    Check(into.parts[1] == 50 && into.parts[2] == 90 && into.parts[3] == kGone, "raise only: lower value kept higher");
    Check(into.wheels[1] == kWheelBurst && (into.flags & kBurning) && into.burnLeft == 4, "wheels and flags merged");
    Update lower = u;
    lower.parts = {P(2, 40), P(3, 10)};
    lower.wheels = {kWheelOk, kWheelOk};
    lower.flags = 0;
    Check(!MergeUp(into, lower), "lower values change nothing");
    Check(into.parts[3] == kGone && into.wheels[1] == kWheelBurst && (into.flags & kBurning), "gone stays gone");
    Update other2 = Full(CarRef{}, 0, Car(12));
    Check(MergeUp(into, other2) && into.parts.size() == 12, "full of another layout replaces");
}

void TestRepairPlanExcess() {
    Snapshot was = Car(), now = Car();
    was.parts[0] = 200;
    now.parts[0] = 190;
    Check(!Repaired(was, now), "small drop is no repair");
    now.parts[0] = 20;
    Check(Repaired(was, now), "big drop is a repair");
    now = was;
    was.parts[1] = kGone;
    Check(Repaired(was, now), "gone part back is a repair");
    now = was;
    was.wheels[2] = kWheelBurst;
    Check(Repaired(was, now), "tyre back is a repair");
    now = was;
    was.flags = kExploded;
    Check(Repaired(was, now), "explosion undone is a repair");
    now = was;
    Check(!Repaired(was, now), "same is no repair");

    Snapshot local = Car(), auth = Car();
    auth.parts[0] = 100;
    auth.parts[1] = 2;  // within tolerance
    auth.parts[2] = kGone;
    auth.wheels[3] = kWheelBurst;
    auth.flags = kBurning;
    auth.burnLeft = 6;
    local.parts[3] = 80;
    Applied done;
    Apply a = Plan(local, auth, done);
    Check(a.parts.size() == 2 && a.parts[0].first == 0 && a.parts[0].second == 100 && a.parts[1].second == kGone,
          "plan raises parts beyond tolerance");
    Check(a.wheels.size() == 1 && a.wheels[0].first == 3 && a.wheels[0].second == kWheelBurst, "plan bursts the tyre");
    Check(a.burn && a.burnLeft == 6 && !a.explode && !a.repair, "plan starts the fire");
    Apply again = Plan(local, auth, done);
    Check(again.Empty(), "nothing set twice while the game has not taken it");
    auth.parts[0] = 150;
    auth.flags = kExploded;
    Apply more = Plan(local, auth, done);
    Check(more.parts.size() == 1 && more.parts[0].second == 150 && more.explode, "raised again / explodes");
    Snapshot localOther = Car(11);
    Applied d2;
    Apply lo = Plan(localOther, auth, d2);
    Check(lo.parts.empty() && lo.explode, "other layout: no parts, explosion still");

    Update ex = Excess(CarRef{}, 0, local, auth);
    Check(ex.parts.size() == 1 && ex.parts[0].first == 3 && ex.parts[0].second == 80, "excess: our own dent");
    Check(Empty(Excess(CarRef{}, 0, auth, auth)), "no excess when equal");
    Snapshot burst = auth;
    burst.wheels[0] = kWheelOff;
    Update ew = Excess(CarRef{}, 0, burst, auth);
    Check(!Empty(ew) && ew.wheels.size() == 4 && ew.wheels[0] == kWheelOff && ew.parts.empty(), "excess: our wheel");
}

// ---- Book scenarios -------------------------------------------------------------------------------------------------
struct Net {
    // two games: A (player 0, the driver / host) and B (player 1, a copy)
    Book a, b;
    double now = 100;
    Outbox oa, ob;
    // deliver everything A and B sent (A broadcasts to B and vice versa; reports / requests / answers to the other)
    void Deliver() {
        for (const auto& u : oa.broadcast) b.OnDamage(RoundTrip(u), 0, now);
        for (const auto& [to, u] : oa.to) if (to == 1) b.OnDamage(RoundTrip(u), 0, now);
        for (const auto& [to, u] : oa.reports) if (to == 1) b.OnReport(RoundTrip(u), 0, now);
        for (const auto& [to, r] : oa.requests) if (to == 1) b.OnRequest(r, 0);
        for (const auto& u : ob.broadcast) a.OnDamage(RoundTrip(u), 1, now);
        for (const auto& [to, u] : ob.to) if (to == 0) a.OnDamage(RoundTrip(u), 1, now);
        for (const auto& [to, u] : ob.reports) if (to == 0) a.OnReport(RoundTrip(u), 1, now);
        for (const auto& [to, r] : ob.requests) if (to == 0) a.OnRequest(r, 1);
    }
};

// What the game does with an Apply: the parts / wheels / flags take the value (a repair clears the car).
void Carry(const Outbox& o, uint32_t ent, Snapshot& s) {
    for (const Apply& a : o.applies) {
        if (a.entity != ent) continue;
        if (a.repair) {
            std::fill(s.parts.begin(), s.parts.end(), uint8_t{0});
            std::fill(s.wheels.begin(), s.wheels.end(), kWheelOk);
            s.flags = 0;
        }
        for (const auto& [i, v] : a.parts) s.parts[i] = v;
        for (const auto& [i, v] : a.wheels) s.wheels[i] = v;
        if (a.burn) {
            s.flags |= kBurning;
            s.burnLeft = a.burnLeft;
        }
        if (a.explode) s.flags = kExploded;
    }
}

CarInput In(const char* ref, uint32_t ent, Role role, int auth, const Snapshot& s, bool quiet = false) {
    CarInput in;
    CarRef::Parse(ref, in.ref);
    in.entity = ent;
    in.role = role;
    in.authPeer = auth;
    in.local = s;
    in.read = true;
    in.quiet = quiet;
    return in;
}

void Step(Net& n, Snapshot& carA, uint32_t entA, Role roleA, Snapshot& carB, uint32_t entB, Role roleB,
          bool quietB = false, const char* ref = "0:5000") {
    n.oa = {};
    n.ob = {};
    n.a.Round({In(ref, entA, roleA, roleA == Role::Copy ? 1 : -1, carA)}, n.now, n.oa);
    n.b.Round({In(ref, entB, roleB, roleB == Role::Copy ? 0 : -1, carB, quietB)}, n.now, n.ob);
    Carry(n.oa, entA, carA);
    Carry(n.ob, entB, carB);
    n.Deliver();
    n.now += 0.25;
}

void TestBookDriverDents() {
    Net n;
    Snapshot a = Car(), b = Car();
    for (int i = 0; i < 3; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
    Check(n.b.Known(CarRef{CarRef::kPlayer, 0, 5000}), "copy asked and got the state", std::to_string(n.b.GetStats().requests));
    a.parts[4] = 120;
    a.wheels[1] = kWheelBurst;
    for (int i = 0; i < 3; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
    Check(b.parts[4] == 120 && b.wheels[1] == kWheelBurst, "copy shows the driver's dent and burst tyre");
    const uint64_t sent = n.a.GetStats().sent;
    for (int i = 0; i < 10; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
    Check(n.a.GetStats().sent == sent, "nothing sent while nothing changes");
    Check(n.b.GetStats().reports == 0, "copy reports nothing it got from the authority");
    // the game rounds the copy's part slightly differently: no report, no re-set
    b.parts[4] = 122;
    for (int i = 0; i < 4; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
    Check(n.b.GetStats().reports == 0 && a.parts[4] == 120, "rounding is no own damage");
    // keyframe after fullEvery
    n.now += 16;
    const uint64_t fulls = n.a.GetStats().fulls;
    Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
    Check(n.a.GetStats().fulls == fulls + 1, "keyframe of a damaged car");
}

void TestBookCopyShot() {
    Net n;
    Snapshot a = Car(), b = Car();
    for (int i = 0; i < 3; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
    // B shoots A's car on his own game: its tyre bursts and a door is dented there
    b.wheels[2] = kWheelBurst;
    b.parts[6] = 90;
    for (int i = 0; i < 6; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
    Check(n.b.GetStats().reports == 1, "copy reported its own damage once", std::to_string(n.b.GetStats().reports));
    Check(a.wheels[2] == kWheelBurst && a.parts[6] == 90, "the authority's car took it");
    Snapshot auth;
    n.b.Known(CarRef{CarRef::kPlayer, 0, 5000}, &auth);
    Check(auth.wheels[2] == kWheelBurst && auth.parts[6] == 90, "and sent it back to everyone");
    for (int i = 0; i < 6; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
    Check(n.b.GetStats().reports == 1, "no report loop");
}

void TestBookQuiet() {
    Net n;
    Snapshot a = Car(), b = Car();
    for (int i = 0; i < 3; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
    // the pinned copy scrapes a wall: its own bumps are not reported; once they add up it is repaired and set again
    a.parts[0] = 60;
    for (int i = 0; i < 3; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy, true);
    b.parts[1] = 20;
    for (int i = 0; i < 4; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy, true);
    Check(n.b.GetStats().reports == 0 && a.parts[1] == 0, "small bumps while pinned: not reported");
    b.parts[2] = 80;
    for (int i = 0; i < 8; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy, true);
    Check(n.b.GetStats().repairs == 1, "repaired once they added up", std::to_string(n.b.GetStats().repairs));
    Check(b.parts[0] == 60 && b.parts[1] == 0 && b.parts[2] == 0, "and set to the authority's again", b.Describe() + " p0=" + std::to_string(b.parts[0]));
    // a tyre burst while pinned is still reported
    b.wheels[0] = kWheelBurst;
    for (int i = 0; i < 4; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy, true);
    Check(a.wheels[0] == kWheelBurst, "tyre burst on a pinned copy still reported");
}

void TestBookRepair() {
    Net n;
    Snapshot a = Car(), b = Car();
    a.parts[3] = 200;
    a.wheels[0] = kWheelBurst;
    for (int i = 0; i < 4; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
    Check(b.parts[3] == 200, "copy damaged");
    // the driver has it repaired at a garage
    a = Car();
    a.parts[5] = 10;
    for (int i = 0; i < 8; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
    uint16_t ep = 0;
    n.a.Known(CarRef{CarRef::kPlayer, 0, 5000}, nullptr, &ep);
    Check(ep == 1, "repair moved the epoch on", std::to_string(ep));
    Check(n.b.GetStats().repairs == 1, "copy repaired itself");
    Check(b.parts[3] == 0 && b.wheels[0] == kWheelOk && b.parts[5] == 10, "copy matches the repaired car");
    Check(n.b.GetStats().reports == 0, "the repair is not reported back as own damage");
}

void TestBookHandOver() {
    Net n;
    Snapshot a = Car(), b = Car();
    a.parts[1] = 100;
    for (int i = 0; i < 4; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
    // B takes the wheel of the parked car: his game holds it now; A's real car is hidden behind a fresh copy (33)
    Snapshot freshCopy = Car();
    b.parts[2] = 150;  // he crashes it
    for (int i = 0; i < 6; ++i) Step(n, freshCopy, 33, Role::Copy, b, 22, Role::Authority);
    Check(freshCopy.parts[1] == 100 && freshCopy.parts[2] == 150, "A's fresh copy shows B's car", freshCopy.Describe());
    // B parks, A's own car (11, still with the old damage) comes back where the copy stood
    for (int i = 0; i < 6; ++i) Step(n, a, 11, Role::Copy, b, 22, Role::Authority);
    Check(a.parts[2] == 150 && a.parts[1] == 100, "A's own car back: it gets the damage B's game holds");
    Check(n.a.GetStats().reports == 0, "nothing reported back");
}

void TestBookExplosion() {
    Net n;
    Snapshot a = Car(), b = Car();
    for (int i = 0; i < 3; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
    a.flags = kBurning;
    a.burnLeft = 5;
    for (int i = 0; i < 3; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
    Check((b.flags & kBurning) && b.burnLeft == 5, "copy burns too", std::to_string(b.burnLeft));
    a.flags = kExploded;
    for (int i = 0; i < 3; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
    Check(b.flags & kExploded, "copy blown up with it");
    Check(n.b.GetStats().explosions == 1 && n.b.GetStats().reports == 0, "once, nothing reported back");
}

// The one explode routine (car_damage.lua D.Explode) as the test sees it: the guard first, then one blast picture.
int ExplodeRoutine(ExplodeOnce& once, uint32_t entity, int& blasts) {
    if (!once.First(entity)) return 0;
    ++blasts;
    return 1;
}

void TestExplodeOnce() {
    ExplodeOnce once;
    Check(once.First(22) && !once.First(22) && once.Done(22), "first ask per entity goes through, the next is refused");
    Check(once.First(33) && !once.First(0) && once.Size() == 2, "per entity; entity 0 never");
    once.Clear();
    Check(once.First(22), "a new world starts over");

    Check(HoldFire(5.0f) == 5.0f && HoldFire(1.0f) == kHoldFire && HoldFire(0.0f) == kHoldFire &&
              HoldFire(-2.0f) == kHoldFire && HoldFire(NAN) == kHoldFire,
          "a held countdown never falls below the hold");

    // a burning car: the copy burns, its countdown is held (never a blast of its own), the authority's is not
    Net n;
    Snapshot a = Car(), b = Car();
    for (int i = 0; i < 3; ++i) Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
    a.flags = kBurning;
    a.burnLeft = 0.6f;
    bool burnApply = false, held = false, authorityHeld = false;
    for (int i = 0; i < 4; ++i) {
        Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
        for (const Apply& ap : n.ob.applies)
            if (ap.burn) burnApply = ap.copy;
        for (uint32_t e : n.ob.holdFire) held |= e == 22;
        for (uint32_t e : n.oa.holdFire) authorityHeld |= e == 11;
    }
    Check(burnApply && held && !authorityHeld, "copy's fire shown and held, the authority's runs");

    // the authority's car explodes: car damage's exploded flag (an Apply with explode on the copy) and the explosions
    // sync's CarExploded both reach the copy's game; whichever comes first blows it up, the other does nothing
    a.flags = kExploded;
    bool explodeApply = false;
    for (int i = 0; i < 3; ++i) {
        Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
        for (const Apply& ap : n.ob.applies)
            if (ap.explode) explodeApply = ap.copy && ap.entity == 22;
    }
    Check(explodeApply, "the copy's explosion comes as a copy apply (the one routine, never native)");
    {
        ExplodeOnce g;
        int blasts = 0;
        ExplodeRoutine(g, 22, blasts);  // car damage's flag first
        ExplodeRoutine(g, 22, blasts);  // then CarExploded
        Check(blasts == 1, "flag first, CarExploded second: one explosion");
    }
    {
        ExplodeOnce g;
        int blasts = 0;
        ExplodeRoutine(g, 22, blasts);  // CarExploded first
        ExplodeRoutine(g, 22, blasts);  // then car damage's flag
        ExplodeRoutine(g, 22, blasts);  // and a keyframe repeating it
        Check(blasts == 1, "CarExploded first, the flag second: one explosion");
    }
    {
        // the hidden real car coming back wrecked is another entity: set exploded once too
        ExplodeOnce g;
        int blasts = 0;
        ExplodeRoutine(g, 22, blasts);
        ExplodeRoutine(g, 11, blasts);
        ExplodeRoutine(g, 11, blasts);
        Check(blasts == 2, "the copy and the real car behind it: once each");
    }
    bool stillHeld = false;
    for (int i = 0; i < 2; ++i) {
        Step(n, a, 11, Role::Authority, b, 22, Role::Copy);
        for (uint32_t e : n.ob.holdFire) stillHeld |= e == 22;
    }
    Check(!stillHeld && n.b.GetStats().reports == 0, "an exploded copy is not held, nothing reported back");
}

void TestBookTraffic() {
    // the host's own book keeps his traffic cars by entity (h...), the guest's by net (t...): the sync translates
    Book host, guest;
    Outbox oh, og;
    Snapshot hc = Car(), gc = Car();
    hc.parts[0] = 77;
    CarInput hi = In("h99850", 99850, Role::Authority, -1, hc);
    host.Round({hi}, 100, oh);
    Check(oh.broadcast.size() == 1 && oh.broadcast[0].ref.kind == CarRef::kHostTraffic, "host traffic sent by entity");
    CarInput gi = In("t17", 4242, Role::Copy, 0, gc);
    guest.Round({gi}, 100, og);
    Check(og.requests.size() == 1 && og.requests[0].first == 0 && og.requests[0].second.kind == CarRef::kTraffic,
          "guest asks the host for net 17");
    Update u = oh.broadcast[0];
    u.ref = CarRef{CarRef::kTraffic, 0, 17};  // what the network edge does
    guest.OnDamage(RoundTrip(u), 0, 100.1);
    og = {};
    guest.Round({gi}, 100.25, og);
    Check(og.applies.size() == 1 && og.applies[0].entity == 4242 && og.applies[0].parts.size() == 1,
          "guest's traffic copy gets the dent");
}

void TestStale() {
    // a copy whose entity carries damage the car does not (our car back after the driver repaired it elsewhere)
    Book b;
    Outbox o;
    Snapshot stale = Car();
    stale.parts[0] = 200;
    Update full = Full(CarRef{CarRef::kPlayer, 2, 77}, 3, Car());
    b.OnDamage(full, 2, 10);
    b.Round({In("2:77", 50, Role::Copy, 2, stale)}, 10, o);
    Check(o.applies.size() == 1 && o.applies[0].repair, "stale entity repaired first");
    Check(o.reports.empty(), "and its old damage not reported");
}

}  // namespace

int main() {
    TestQuantize();
    TestRef();
    TestWire();
    TestDiffMerge();
    TestRepairPlanExcess();
    TestBookDriverDents();
    TestBookCopyShot();
    TestBookQuiet();
    TestBookRepair();
    TestBookHandOver();
    TestBookExplosion();
    TestExplodeOnce();
    TestBookTraffic();
    TestStale();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
