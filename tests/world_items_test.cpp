// world_items_test: unit tests for the shared world items (src/game/world_items.h): the item codec and the Lua line
// format, collectible names, the host's take arbitration (its own take, a guest's, two at once, late and unknown takes,
// despawns are no takes), the guest's pairing (same spot, same name a few metres off, the type must match, nearest
// first, moves to the host's spot), hiding of the guest's own new drops (and never his own swap drop or the items his
// world had from the start), hiding when the host's item is taken, the guest's take and its refusal (the weapons he
// gained go back), and a host-guest round trip over the wire format. Prints PASS/FAIL per check; exits non-zero on any
// failure.
#include <cstdio>
#include <string>
#include <vector>

#include "game/world_items.h"
#include "net/wire.h"

using namespace mc::game::items;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-72s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

Item It(uint32_t key, float x, float y, uint8_t type = kTypeWeapon, const std::string& name = "-") {
    Item it;
    it.key = key;
    it.type = type;
    it.x = x;
    it.y = y;
    it.z = 5;
    it.name = name;
    return it;
}

size_t CountCmds(const std::vector<Cmd>& cmds, Cmd::Kind k, uint32_t id = 0) {
    size_t n = 0;
    for (const Cmd& c : cmds) n += (c.kind == k && (id == 0 || c.id == id)) ? 1 : 0;
    return n;
}

void TestCodec() {
    Item a = It(0x12345678u, 1.5f, -2.25f, kTypeInventory, "item_medkit_a");
    mc::net::ByteWriter w;
    WriteItem(w, a);
    mc::net::ByteReader r(w.Span());
    Item b;
    Check(ReadItem(r, b) && b.key == a.key && b.type == a.type && b.x == a.x && b.y == a.y && b.name == a.name,
          "an item survives the wire");
    mc::net::ByteReader shortR(w.Data(), 7);
    Item c;
    Check(!ReadItem(shortR, c), "a short record is refused");

    Item p;
    Check(ParseItem(FormatItem(a), p) && p.key == a.key && p.type == a.type && p.name == a.name, "Lua line round trip");
    Check(!ParseItem("0 63 1 2 3 x", p), "key 0 is refused");
    Check(!ParseItem("5 63 nan 2 3 x", p), "a bad position is refused");
    Check(ParseItem("5 63 1 2 3", p) && p.name == "-", "a missing name reads as unknown");
    Check(SafeName("a b\"c") == "a_b_c" && SafeName("") == "-" && SafeName(std::string(90, 'x')).size() == kMaxName,
          "names are made safe and cut");

    std::vector<Item> many;
    for (uint32_t i = 1; i <= 40; ++i) many.push_back(It(i, float(i), 0, kTypeWeapon, std::string(kMaxName, 'n')));
    auto bufs = PackStates(many);
    size_t total = 0;
    bool small = true;
    for (const auto& b2 : bufs) {
        total += b2[0];
        small &= b2.size() <= 1000;
    }
    Check(total == 40 && small && bufs.size() == 3, "40 items: three messages under 1000 bytes",
          std::to_string(bufs.size()) + " messages");

    Check(CollectibleName("lh_pickup_cc_04_ennio_salieri_bv01"), "a cigarette card name is a collectible name");
    Check(!CollectibleName("lh_pickup_") && !CollectibleName("ammo_box") && !CollectibleName("lh_pickup_a b") &&
              !CollectibleName("lh_pickup_X"),
          "other names are not");
    Check(SharedType(kTypeWeapon) && SharedType(kTypeInventory) && !SharedType(8) && !SharedType(54),
          "weapons and inventory items are shared, nothing else");
}

void TestHostOwnTake() {
    HostBook h;
    h.OnRound(0, 0, 0, 5, {It(10, 1, 0), It(11, 50, 0)});
    Check(h.States().size() == 2 && h.States()[0].key == 10, "host sends its items, nearest first");
    h.OnGone(0.5, 10);  // vanished next to the host's player
    auto g = h.TakeGones();
    Check(g.size() == 1 && g[0].key == 10 && g[0].by == kHost && g[0].reason == GoneReason::Taken && g[0].to == kNobody,
          "an item gone next to the host was taken by him: everyone hears it");
    h.OnGone(0.6, 11);  // vanished 50 m away
    g = h.TakeGones();
    Check(g.size() == 1 && g[0].key == 11 && g[0].reason == GoneReason::Forget, "an item gone far away despawned: forget");
    Check(h.Count() == 0 && h.States().empty(), "neither is sent any more");
    h.OnTake(1.0, 2, 10);  // a guest's take arrives after the host's
    g = h.TakeGones();
    Check(g.size() == 1 && g[0].to == 2 && g[0].by == kHost && h.Counters().refused == 1,
          "a guest's later take is refused: only he hears who has it");
    Check(h.TakeCmds().empty(), "nothing to hide on the host for a refused take");
}

void TestHostGuestTakes() {
    HostBook h;
    h.OnRound(0, 0, 0, 5, {It(20, 30, 0), It(21, 31, 0, kTypeInventory)});
    h.OnTake(0.2, 1, 20);
    auto g = h.TakeGones();
    auto c = h.TakeCmds();
    Check(g.size() == 1 && g[0].by == 1 && g[0].to == kNobody, "guest 1's take is granted and broadcast");
    Check(CountCmds(c, Cmd::Kind::Hide, 20) == 1, "the host hides its own item for him");
    PlayerId by = kNobody;
    Check(h.Taken(20, &by) && by == 1, "the host remembers who has it");
    // two guests at the same time: the first that reaches the host wins
    h.OnTake(0.21, 2, 20);
    g = h.TakeGones();
    Check(g.size() == 1 && g[0].to == 2 && g[0].by == 1, "guest 2, a moment later, is refused (guest 1 has it)");
    h.OnTake(0.25, 1, 20);
    Check(h.TakeGones().empty(), "a repeated take by the winner changes nothing");
    // the hidden entity vanishes from the host's world next round: no second event
    h.OnRound(0.5, 30, 0, 5, {It(21, 31, 0, kTypeInventory)});
    h.OnGone(0.5, 20);
    Check(h.TakeGones().empty(), "our own hide is not reported as a take");
    Check(h.States().size() == 1 && h.States()[0].key == 21, "the taken item is not sent any more");
    // a take of an item the host never had (or forgot): his, and everyone else hides theirs
    h.OnTake(0.6, 2, 99);
    g = h.TakeGones();
    Check(g.size() == 1 && g[0].by == 2 && g[0].to == kNobody && h.Counters().unknownTakes == 1,
          "a take of an unknown item is granted");
    h.OnTake(0.7, 1, 99);
    g = h.TakeGones();
    Check(g.size() == 1 && g[0].to == 1 && g[0].by == 2, "... and a second take of it is refused");
    // the host's player standing on a guest-taken item: its vanishing is no host take
    h.OnRound(1.0, 31, 0, 5, {It(21, 31, 0, kTypeInventory)});
    h.OnTake(1.1, 1, 21);
    h.TakeGones();
    h.OnGone(1.2, 21);
    Check(h.TakeGones().empty() && h.Counters().ours == 0, "an item hidden for a guest never counts as the host's");
    // taken items are forgotten after a while (a late take is then granted again: nobody has it any more)
    h.OnRound(1.0 + kKeepTaken + 1, 0, 0, 5, {});
    Check(!h.Taken(20), "a taken item is forgotten after kKeepTaken");
    // items are kept while out of range, forgotten later
    HostBook h2;
    h2.OnRound(0, 0, 0, 5, {It(30, 5, 0)});
    h2.OnRound(1, 0, 0, 5, {});
    Check(h2.Count() == 1 && h2.States().empty(), "out of range: kept, not sent");
    h2.OnRound(kForgetLocal + 2, 0, 0, 5, {});
    Check(h2.Count() == 0, "out of range for long: forgotten");
    // not shared types are ignored
    HostBook h3;
    h3.OnRound(0, 0, 0, 5, {It(40, 1, 0, 8)});
    Check(h3.Count() == 0, "a human is no item");
}

void TestGuestPairing() {
    GuestBook g;
    g.SetSelf(1);
    // the host's items: two weapons, a medkit
    g.OnHostItem(0, It(100, 10, 0, kTypeWeapon, "w_a"));
    g.OnHostItem(0, It(101, 20, 0, kTypeWeapon, "w_b"));
    g.OnHostItem(0, It(102, 30, 0, kTypeInventory));
    // ours: one at the same spot, one 3 m off with the same name, the medkit's spot holds a weapon, a far one
    g.OnRound(0.1, 0, 0, 5, 60, {It(7, 10.2f, 0, kTypeWeapon, "w_a"), It(8, 23, 0, kTypeWeapon, "w_b"),
                                 It(9, 30, 0.1f, kTypeWeapon), It(10, 80, 0, kTypeWeapon, "w_a")});
    Check(g.PartnerOf(100) == 7, "same spot: paired");
    Check(g.PartnerOf(101) == 8, "same name, 3 m off: paired (loose)");
    Check(g.PartnerOf(102) == 0 && g.HostOf(9) == 0, "another type at the spot: not paired");
    Check(g.HostOf(10) == 0, "far away: not paired");
    auto c = g.TakeCmds();
    Check(CountCmds(c, Cmd::Kind::Move, 8) == 1 && CountCmds(c, Cmd::Kind::Move, 7) == 0,
          "the 3 m one is put on the host's spot; the close one is left");
    g.OnRound(0.6, 0, 0, 5, 60.5, {It(7, 10.2f, 0, kTypeWeapon, "w_a"), It(8, 23, 0, kTypeWeapon, "w_b"),
                                   It(9, 30, 0.1f, kTypeWeapon), It(10, 80, 0, kTypeWeapon, "w_a")});
    c = g.TakeCmds();
    Check(CountCmds(c, Cmd::Kind::Move) == 0, "the move is not asked again while the host's spot stays");
    g.OnHostItem(1.0, It(101, 25, 0, kTypeWeapon, "w_b"));  // the host's item was kicked along
    g.OnRound(1.1, 0, 0, 5, 61, {It(8, 20, 0, kTypeWeapon, "w_b")});
    c = g.TakeCmds();
    Check(CountCmds(c, Cmd::Kind::Move, 8) == 1 && g.PartnerOf(101) == 8, "it moved on the host: ours follows");
    Check(g.Paired() == 2 && g.Unshown() == 1, "two shown, the medkit has none here");

    // nearest first: two of ours near one host item
    GuestBook n;
    n.OnHostItem(0, It(200, 0, 0));
    n.OnRound(0, 50, 0, 5, 60, {It(1, 1.2f, 0), It(2, 0.4f, 0)});
    Check(n.PartnerOf(200) == 2, "of two candidates the nearer is taken");

    // a host item without states for kRelease: unpaired, ours stays (no hide: it is no new drop)
    n.OnRound(kRelease + 1, 50, 0, 5, 60 + kRelease + 1, {It(1, 1.2f, 0), It(2, 0.4f, 0)});
    Check(n.PartnerOf(200) == 0 && n.HostOf(2) == 0 && n.HostCount() == 0, "a host item without states is released");
}

void TestGuestHiding() {
    GuestBook g;
    g.SetSelf(1);
    // world loaded long ago; the host keeps talking (one far item)
    auto host = [&](double t) { g.OnHostItem(t, It(500, 200, 0)); };
    host(0);
    // at world load: an item that was there from the start
    g.OnRound(0, 0, 0, 5, 1.0, {It(1, 10, 0)});
    // later: a new drop 20 m away (our copy of an enemy dropped it), our own swap drop at our feet, a new medkit
    host(1);
    g.OnRound(1, 0, 0, 5, 30, {It(1, 10, 0), It(2, 20, 0), It(3, 1, 0), It(4, 15, 0, kTypeInventory)});
    Check(CountCmds(g.TakeCmds(), Cmd::Kind::Hide) == 0, "nothing is hidden at once");
    host(3);
    g.OnRound(3, 0, 0, 5, 32, {It(1, 10, 0), It(2, 20, 0), It(3, 1, 0), It(4, 15, 0, kTypeInventory)});
    Check(CountCmds(g.TakeCmds(), Cmd::Kind::Hide) == 0, "not before kHideWait");
    host(4.5);
    g.OnRound(4.5, 0, 0, 5, 33.5, {It(1, 10, 0), It(2, 20, 0), It(3, 1, 0), It(4, 15, 0, kTypeInventory)});
    auto c = g.TakeCmds();
    Check(CountCmds(c, Cmd::Kind::Hide, 2) == 1 && g.Hidden(2), "our new weapon drop without a host partner is hidden");
    Check(CountCmds(c, Cmd::Kind::Hide, 1) == 0, "an item our world had from the start is not");
    Check(CountCmds(c, Cmd::Kind::Hide, 3) == 0, "our own drop (at our feet) is not");
    Check(CountCmds(c, Cmd::Kind::Hide, 4) == 0, "an inventory item is not");
    Check(g.OnLocalGone(5, 2, "") == GuestBook::LocalGone::OursHidden, "its vanishing afterwards is our own hide");

    // a new drop that finds its host partner in time is shown, not hidden
    GuestBook p;
    p.SetSelf(1);
    p.OnHostItem(0, It(600, 200, 0));
    p.OnRound(0, 0, 0, 5, 30, {It(5, 20, 0)});
    p.OnHostItem(2, It(601, 20.5f, 0));  // the host's NPC died a moment later
    p.OnRound(2, 0, 0, 5, 32, {It(5, 20, 0)});
    p.OnHostItem(4, It(601, 20.5f, 0));
    p.OnRound(4, 0, 0, 5, 34, {It(5, 20, 0)});
    c = p.TakeCmds();
    Check(p.PartnerOf(601) == 5 && CountCmds(c, Cmd::Kind::Hide) == 0, "a drop paired in time stays");

    // with the host silent, nothing is hidden (we cannot know what his world has)
    GuestBook q;
    q.OnRound(0, 0, 0, 5, 30, {It(6, 20, 0)});
    q.OnRound(10, 0, 0, 5, 40, {It(6, 20, 0)});
    Check(CountCmds(q.TakeCmds(), Cmd::Kind::Hide) == 0, "the host silent: nothing hidden");
}

void TestGuestTakes() {
    GuestBook g;
    g.SetSelf(1);
    g.OnHostItem(0, It(700, 1, 0, kTypeWeapon, "w"));
    g.OnHostItem(0, It(701, 40, 0));
    g.OnHostItem(0, It(702, 2, 1));
    g.OnRound(0, 0, 0, 5, 60, {It(70, 1, 0, kTypeWeapon, "w"), It(71, 40, 0), It(72, 2, 1), It(73, 3, 3)});
    g.TakeCmds();
    // we take ours of 700 (next to us)
    Check(g.OnLocalGone(0.5, 70, "pistol_a") == GuestBook::LocalGone::Taken, "ours vanished next to us: our take");
    auto t = g.TakeTakes();
    Check(t.size() == 1 && t[0].key == 700, "an ItemTake for the host's item goes out");
    g.OnHostItem(0.6, It(700, 1, 0, kTypeWeapon, "w"));  // a late state of it
    g.OnRound(0.6, 0, 0, 5, 60.6, {It(71, 40, 0), It(72, 2, 1), It(73, 3, 3), It(74, 1, 0, kTypeWeapon, "w")});
    Check(g.PartnerOf(700) == 0 && g.HostOf(74) == 0, "a late state of the taken item pairs with nothing");
    g.OnGone(0.7, 700, 1, GoneReason::Taken);
    auto c = g.TakeCmds();
    Check(g.Counters().confirmed == 1 && CountCmds(c, Cmd::Kind::Undo) == 0, "the host confirms: we keep it");

    // ours of 701 despawned far from us: no take
    Check(g.OnLocalGone(1, 71, "") == GuestBook::LocalGone::Despawned && g.TakeTakes().empty(),
          "ours vanished far away: no take");
    // an item of ours without a partner vanished next to us: local only
    Check(g.OnLocalGone(1, 73, "") == GuestBook::LocalGone::LocalOnly && g.TakeTakes().empty(),
          "an unpaired item of ours: nothing to tell");

    // a race: we take ours of 702, but the host says guest 2 was first
    Check(g.OnLocalGone(2, 72, "rifle_b,pistol_c") == GuestBook::LocalGone::Taken, "we take another one");
    g.TakeTakes();
    g.OnGone(2.1, 702, 2, GoneReason::Taken);
    c = g.TakeCmds();
    Check(CountCmds(c, Cmd::Kind::Undo) == 1 && c[0].names == "rifle_b,pistol_c" && g.Counters().lost == 1,
          "refused: the weapons we gained with it are taken away");

    // the host takes an item we have a copy of: ours is hidden
    GuestBook h;
    h.SetSelf(1);
    h.OnHostItem(0, It(800, 5, 0, kTypeInventory));
    h.OnRound(0, 0, 0, 5, 60, {It(80, 5, 0, kTypeInventory)});
    h.OnGone(1, 800, kHost, GoneReason::Taken);
    c = h.TakeCmds();
    Check(CountCmds(c, Cmd::Kind::Hide, 80) == 1 && h.Counters().hiddenForHost == 1, "the host took it: ours is hidden");
    h.OnHostItem(1.2, It(800, 5, 0, kTypeInventory));
    Check(h.HostCount() == 0, "late states of it are ignored");
    // the host forgets one (despawned there): ours stays and is free
    h.OnHostItem(2, It(801, 9, 0));
    h.OnRound(2, 0, 0, 5, 62, {It(81, 9, 0)});
    h.OnGone(2.5, 801, kNobody, GoneReason::Forget);
    c = h.TakeCmds();
    Check(CountCmds(c, Cmd::Kind::Hide) == 0 && h.HostOf(81) == 0, "forgotten on the host: ours stays, unpaired");
}

// The full loop: host and guest books talk over the wire format; both players reach for the same weapon.
void TestRoundTrip() {
    HostBook host;
    GuestBook a, b;  // guests 1 and 2
    a.SetSelf(1);
    b.SetSelf(2);
    auto ship = [](const std::vector<Item>& items) {
        std::vector<Item> got;
        for (const auto& buf : PackStates(items)) {
            mc::net::ByteReader r(buf);
            const uint8_t n = r.U8();
            for (uint8_t i = 0; i < n; ++i) {
                Item it;
                if (ReadItem(r, it)) got.push_back(it);
            }
        }
        return got;
    };
    // the host's enemy died and dropped a rifle at (10,0); each guest's copy dropped one nearby
    host.OnRound(0, 0, 0, 5, {It(9001, 10, 0, kTypeWeapon, "rifle")});
    for (const Item& it : ship(host.States())) {
        a.OnHostItem(0.05, it);
        b.OnHostItem(0.05, it);
    }
    a.OnRound(0.1, 10.5f, 0, 5, 60, {It(31, 11, 0.5f, kTypeWeapon, "rifle")});
    b.OnRound(0.1, 9.5f, 0, 5, 60, {It(41, 9.6f, 0, kTypeWeapon, "rifle")});
    Check(a.PartnerOf(9001) == 31 && b.PartnerOf(9001) == 41, "both guests pair their drop with the host's");
    a.TakeCmds();
    b.TakeCmds();
    // both grab it at once; guest 2's ItemTake reaches the host first
    a.OnLocalGone(0.5, 31, "rifle");
    b.OnLocalGone(0.5, 41, "rifle");
    auto ta = a.TakeTakes(), tb = b.TakeTakes();
    Check(ta.size() == 1 && tb.size() == 1, "both send an ItemTake");
    host.OnTake(0.55, 2, tb[0].key);
    host.OnTake(0.56, 1, ta[0].key);
    auto hc = host.TakeCmds();
    Check(CountCmds(hc, Cmd::Kind::Hide, 9001) == 1, "the host hides its rifle");
    for (const Gone& g : host.TakeGones()) {
        if (g.to == kNobody || g.to == 1) a.OnGone(0.6, g.key, g.by, g.reason);
        if (g.to == kNobody || g.to == 2) b.OnGone(0.6, g.key, g.by, g.reason);
    }
    auto ca = a.TakeCmds(), cb = b.TakeCmds();
    Check(CountCmds(ca, Cmd::Kind::Undo) == 1 && ca[0].names == "rifle", "guest 1 lost the race: his rifle goes back");
    Check(cb.empty() && b.Counters().confirmed == 1, "guest 2 keeps it");
    Check(host.Taken(9001) && host.States().empty(), "nobody sees it any more");
}

}  // namespace

int main() {
    TestCodec();
    TestHostOwnTake();
    TestHostGuestTakes();
    TestGuestPairing();
    TestGuestHiding();
    TestGuestTakes();
    TestRoundTrip();
    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
