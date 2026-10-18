// explosion_test: unit tests for explosions, fires and blown-up cars on every game (src/game/explosion_model.h):
// the Explosion / CarExploded wire format (round trips, malformed messages refused), the inbox (a message relayed
// twice is shown once, a restarted sender), the zones (blast and fire reach and time), the damage guard (every
// victim x attacker case: a remote picture's damage undone, our own unnamed damage counted as ours, the rest kept)
// and the detector (one explosion from many HEAR_SOUND reports, ours by sound owner / throw / grenade message /
// shot / chain, an NPC's, someone else's, the kind and fire flag, one throw one explosion), and a whole exchange
// walked through: thrower, receiver, nobody hurt twice.
// Prints PASS/FAIL per check; exits non-zero on any failure.
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "game/explosion_model.h"

using namespace mc::game::boom;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-70s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

Vec3 P(float x, float y, float z = 0) { return {x, y, z}; }

Sound Snd(double t, int type, Vec3 pos, Owner o = Owner::Unknown, uint32_t npc = 0, std::string obj = {}) {
    Sound s;
    s.t = t;
    s.type = type;
    s.pos = pos;
    s.owner = o;
    s.npc = npc;
    s.object = std::move(obj);
    return s;
}

Throw Thr(double t, std::string weapon, Vec3 pos) {
    Throw th;
    th.t = t;
    th.weapon = std::move(weapon);
    th.pos = pos;
    return th;
}

std::vector<Seen> Run(Detector& d, double now, uint32_t& seq) {
    std::vector<Seen> out;
    d.Tick(now, seq, out);
    return out;
}

void TestWire() {
    Explosion e;
    e.seq = 0x12345678;
    e.kind = Kind::Molotov;
    e.flags = kFire | kNpcOwner;
    e.pos = P(-943.5f, -233.25f, 4.5f);
    e.weapon = "exp_molotov";
    e.npc = 0x0000A208;
    e.object = "lh_barrel_a_v1-03";
    const auto buf = PackExplosion(e);
    mc::net::ByteReader r(buf.data(), buf.size());
    Explosion b;
    const bool ok = ReadExplosion(r, b);
    Check(ok && b.seq == e.seq && b.kind == e.kind && b.flags == e.flags && b.pos.x == e.pos.x && b.pos.y == e.pos.y &&
              b.pos.z == e.pos.z && b.weapon == e.weapon && b.npc == e.npc && b.object == e.object,
          "Explosion round trip");
    Check(buf.size() < 120, "Explosion is small", std::to_string(buf.size()) + " bytes");

    Explosion u;  // unknown weapon, no object
    u.seq = 1;
    u.pos = P(1, 2, 3);
    const auto ub = PackExplosion(u);
    mc::net::ByteReader ur(ub.data(), ub.size());
    Explosion ue;
    Check(ReadExplosion(ur, ue) && ue.weapon.empty() && ue.object.empty() && ue.kind == Kind::Unknown,
          "Explosion without weapon and object");

    Explosion bad = e;  // an unsafe name is not sent (empty instead)
    bad.weapon = "exp grenade\n";
    bad.object = std::string(80, 'a');
    const auto bb = PackExplosion(bad);
    mc::net::ByteReader br(bb.data(), bb.size());
    Explosion be;
    Check(ReadExplosion(br, be) && be.weapon.empty() && be.object.empty(), "unsafe names are dropped by the sender");

    {  // a bad kind is refused
        mc::net::ByteWriter w;
        w.U32(1);
        w.U8(9);
        w.U8(0);
        w.F32(0);
        w.F32(0);
        w.F32(0);
        w.Str("");
        w.U32(0);
        w.Str("");
        mc::net::ByteReader rr(w.Span());
        Explosion x;
        Check(!ReadExplosion(rr, x), "Explosion with an unknown kind is refused");
    }
    {  // NaN coordinates are refused
        mc::net::ByteWriter w;
        w.U32(1);
        w.U8(1);
        w.U8(0);
        w.F32(0);
        w.F32(std::numeric_limits<float>::quiet_NaN());
        w.F32(0);
        w.Str("exp_grenade");
        w.U32(0);
        w.Str("");
        mc::net::ByteReader rr(w.Span());
        Explosion x;
        Check(!ReadExplosion(rr, x), "Explosion with a NaN coordinate is refused");
    }
    {  // a weapon with a space (written by someone else) is refused
        mc::net::ByteWriter w;
        w.U32(1);
        w.U8(1);
        w.U8(0);
        w.F32(0);
        w.F32(0);
        w.F32(0);
        w.Str("exp grenade");
        w.U32(0);
        w.Str("");
        mc::net::ByteReader rr(w.Span());
        Explosion x;
        Check(!ReadExplosion(rr, x), "Explosion with an unsafe weapon name is refused");
    }
    {  // truncated
        mc::net::ByteReader rr(buf.data(), buf.size() - 3);
        Explosion x;
        Check(!ReadExplosion(rr, x), "truncated Explosion is refused");
    }

    CarBlast c;
    c.what = CarWhat::Shared;
    c.key = "1:43018";
    c.pos = P(10, 20, 1);
    const auto cb = PackCarBlast(c);
    mc::net::ByteReader cr(cb.data(), cb.size());
    CarBlast cc;
    Check(ReadCarBlast(cr, cc) && cc.what == CarWhat::Shared && cc.key == c.key && cc.pos.y == 20.0f,
          "CarExploded round trip (shared car)");
    c.what = CarWhat::Traffic;
    c.key = "17";
    const auto tb = PackCarBlast(c);
    mc::net::ByteReader tr(tb.data(), tb.size());
    Check(ReadCarBlast(tr, cc) && cc.what == CarWhat::Traffic && cc.key == "17", "CarExploded round trip (traffic)");
    c.key = "";
    const auto eb = PackCarBlast(c);
    mc::net::ByteReader er(eb.data(), eb.size());
    Check(!ReadCarBlast(er, cc), "CarExploded without a key is refused");
    {
        mc::net::ByteWriter w;
        w.U8(2);
        w.Str("1:2");
        w.F32(0);
        w.F32(0);
        w.F32(0);
        mc::net::ByteReader rr(w.Span());
        Check(!ReadCarBlast(rr, cc), "CarExploded with an unknown what is refused");
    }

    Check(KindOfWeapon("exp_grenade") == Kind::Grenade && KindOfWeapon("exp_molotov") == Kind::Molotov &&
              KindOfWeapon("exp_satchel") == Kind::Satchel && KindOfWeapon("rev_alf_a_v1") == Kind::Unknown &&
              KindOfWeapon("") == Kind::Unknown,
          "weapon names -> kinds");
    Check(std::string(WeaponOfKind(Kind::Molotov)) == "exp_molotov" &&
              std::string(WeaponOfKind(Kind::Car)) == "exp_grenade",
          "kinds -> the weapon a receiver shows");
    Check(SafeName("lh_barrel-01.v2:a/b", 32) && !SafeName("", 32) && !SafeName("a b", 32) &&
              !SafeName("abc", 2) && !SafeName("x\n", 8),
          "SafeName");
}

void TestInbox() {
    Inbox in;
    Check(in.Accept(1, 100), "inbox: first message");
    Check(!in.Accept(1, 100), "inbox: the same message again (relayed twice) is dropped");
    Check(in.Accept(2, 100), "inbox: the same seq from another sender");
    Check(in.Accept(1, 99), "inbox: an older seq not seen yet (out of order)");
    for (uint32_t s = 101; s < 300; ++s) in.Accept(1, s);
    Check(in.Accept(1, 5), "inbox: a restarted sender's low seq");
    in.Forget(2);
    Check(in.Accept(2, 100), "inbox: forgotten after he left");
}

void TestZones() {
    Zones z;
    Zone blast;
    blast.origin = Origin::Remote;
    blast.pos = P(0, 0);
    blast.t = 10;
    blast.id = 7;
    blast.from = 1;
    z.Add(blast);
    Check(z.Find(Origin::Remote, P(5, 0), 10.5, kDmgExplosion) != nullptr, "blast zone covers 5 m at once");
    Check(z.Find(Origin::Remote, P(kBlastRadius + 1, 0), 10.5, kDmgExplosion) == nullptr, "blast zone: not beyond it");
    Check(z.Find(Origin::Remote, P(5, 0), 10 + kBlastTime + 0.5, kDmgExplosion) == nullptr,
          "blast zone: explosion damage later is not its");
    Check(z.Find(Origin::Remote, P(5, 0), 10 - 0.3, kDmgExplosion) != nullptr,
          "blast zone: damage a moment before the zone (event order) counts");
    Check(z.Find(Origin::Remote, P(5, 0), 9.0, kDmgExplosion) == nullptr, "blast zone: a second before does not");
    Check(z.Find(Origin::Own, P(5, 0), 10.5, kDmgExplosion) == nullptr, "zones: origin is kept apart");
    Check(z.Find(Origin::Remote, P(5, 0), 14.0, kDmgFire) != nullptr, "blast zone: things it set alight burn a while");
    Check(z.Find(Origin::Remote, P(5, 0), 30.0, kDmgFire) == nullptr, "blast zone: but not half a minute");
    Check(z.Find(Origin::Remote, P(5, 0), 10.5, 0) == nullptr, "zones: bullets are never an explosion's");

    Zone fire;
    fire.origin = Origin::Remote;
    fire.fire = true;
    fire.pos = P(100, 0);
    fire.t = 20;
    z.Add(fire);
    Check(z.Find(Origin::Remote, P(105, 0), 40, kDmgFire) != nullptr, "fire zone burns for half a minute");
    Check(z.Find(Origin::Remote, P(100 + kFireRadius + 1, 0), 30, kDmgFire) == nullptr, "fire zone: not beyond it");
    Check(z.Find(Origin::Remote, P(105, 0), 20 + kFireTime + 1, kDmgFire) == nullptr, "fire zone: out after kFireTime");

    Zone newer = blast;
    newer.t = 11;
    newer.id = 8;
    z.Add(newer);
    const Zone* f = z.Find(Origin::Remote, P(1, 0), 11.2, kDmgExplosion);
    Check(f && f->id == 8, "zones: the newest covering one is found");
    z.Expire(11 + kBlastTime + 10.5);
    Check(z.Count(Origin::Remote) == 1, "zones: blasts expire, the fire stays", std::to_string(z.Count()));
    z.Expire(20 + kFireTime + 6);
    Check(z.Count() == 0, "zones: the fire expires too");
    for (int i = 0; i < 200; ++i) {
        Zone q;
        q.t = i;
        z.Add(q);
    }
    Check(z.Count() <= 64, "zones: bounded");
}

void TestJudge() {
    Zones z;
    Zone remote;
    remote.origin = Origin::Remote;
    remote.pos = P(0, 0);
    remote.t = 100;
    remote.from = 1;
    remote.id = 3;
    z.Add(remote);
    Zone own;
    own.origin = Origin::Own;
    own.pos = P(300, 0);
    own.t = 100;
    own.id = 9;
    own.fire = true;
    z.Add(own);
    const double t = 100.3;

    auto V = [&](Victim v, Enemy e, int dtype, Vec3 p, double now = 100.3) { return Judge(z, v, e, dtype, p, now).verdict; };
    // inside the remote picture
    Check(V(Victim::LocalPlayer, Enemy::None, kDmgExplosion, P(3, 0)) == Verdict::Undo,
          "remote picture hurts us (no attacker): undone");
    Check(V(Victim::LocalPlayer, Enemy::Puppet, kDmgExplosion, P(3, 0)) == Verdict::Undo,
          "remote picture hurts us (a puppet named): undone");
    Check(V(Victim::Npc, Enemy::None, kDmgExplosion, P(3, 0)) == Verdict::Undo, "remote picture hurts an NPC: undone");
    Check(V(Victim::Npc, Enemy::Other, kDmgFire, P(3, 0), 104) == Verdict::Undo, "remote picture's fire: undone");
    Check(V(Victim::NpcCopy, Enemy::NpcCopy, kDmgExplosion, P(3, 0)) == Verdict::Undo,
          "a copy's explosion picture on a copy: undone");
    Check(V(Victim::Npc, Enemy::Us, kDmgExplosion, P(3, 0)) == Verdict::Keep,
          "our own grenade in the same place: kept (named us)");
    Check(V(Victim::LocalPlayer, Enemy::OurNpc, kDmgExplosion, P(3, 0)) == Verdict::Keep,
          "our own NPC's grenade there: kept (this game's fight)");
    Check(V(Victim::LocalPlayer, Enemy::None, 0, P(3, 0)) == Verdict::Keep, "a bullet there: kept");
    Check(V(Victim::LocalPlayer, Enemy::None, kDmgExplosion, P(40, 0)) == Verdict::Keep, "outside the picture: kept");
    Check(V(Victim::LocalPlayer, Enemy::None, kDmgExplosion, P(3, 0), 110) == Verdict::Keep,
          "long after the picture: kept");
    // inside our own explosion
    Check(V(Victim::Puppet, Enemy::None, kDmgExplosion, P(303, 0)) == Verdict::Ours,
          "our blast, no attacker, on a puppet: ours (sent as a Hit)");
    Check(V(Victim::NpcCopy, Enemy::Other, kDmgFire, P(303, 0), 120) == Verdict::Ours,
          "our molotov's fire on a host NPC copy: ours (NpcHit)");
    Check(V(Victim::Npc, Enemy::None, kDmgFire, P(303, 0), 120) == Verdict::Ours, "our fire on an NPC: ours");
    Check(V(Victim::Puppet, Enemy::Us, kDmgExplosion, P(303, 0)) == Verdict::Keep,
          "our blast naming us: kept (the hit paths know it is ours)");
    Check(V(Victim::Puppet, Enemy::OurNpc, kDmgExplosion, P(303, 0)) == Verdict::Keep,
          "an NPC's blast in our zone: kept (the NPC's)");
    Check(V(Victim::LocalPlayer, Enemy::None, kDmgExplosion, P(303, 0)) == Verdict::Keep,
          "our own blast hurting us: kept");
    Check(V(Victim::Puppet, Enemy::Puppet, kDmgExplosion, P(303, 0)) == Verdict::Keep,
          "our zone, a puppet named, no remote picture: kept");

    // both: a remote picture newer than our own explosion at the same spot
    Zone remote2;
    remote2.origin = Origin::Remote;
    remote2.pos = P(300, 0);
    remote2.t = 101;
    remote2.from = 2;
    z.Add(remote2);
    Check(V(Victim::Npc, Enemy::None, kDmgExplosion, P(302, 0), 101.2) == Verdict::Undo,
          "remote picture newer than ours, no attacker: undone");
    Zone own2 = own;
    own2.t = 101.5;
    own2.fire = false;
    z.Add(own2);
    Check(V(Victim::Npc, Enemy::None, kDmgExplosion, P(302, 0), 101.6) == Verdict::Ours,
          "our blast newer than the picture, no attacker: ours");
    Check(V(Victim::Npc, Enemy::Puppet, kDmgExplosion, P(302, 0), 101.6) == Verdict::Undo,
          "a puppet named while both cover it: undone (a puppet never decides damage here)");
    const Judgement j = Judge(z, Victim::Npc, Enemy::None, kDmgExplosion, P(1, 0), t);
    Check(j.zone && j.zone->origin == Origin::Remote && j.zone->id == 3, "the judgement names its zone");
    Check(VictimFromName("local") == Victim::LocalPlayer && VictimFromName("copy") == Victim::NpcCopy &&
              VictimFromName("puppet") == Victim::Puppet && VictimFromName("x") == Victim::Npc &&
              EnemyFromName("none") == Enemy::None && EnemyFromName("npc") == Enemy::OurNpc &&
              EnemyFromName(nullptr) == Enemy::Other && OwnerFromName("us") == Owner::Us &&
              OwnerFromName("?") == Owner::Unknown,
          "names from the Lua side");
}

void TestDetector() {
    {  // many humans hear one grenade of ours: one explosion, sent once
        Zones z;
        Detector d(z);
        uint32_t seq = 1;
        d.OnThrow(Thr(10, "exp_grenade", P(0, 0)));
        for (int i = 0; i < 12; ++i) d.OnSound(Snd(13.0 + i * 0.01, kSoundExplosion, P(15.2f, 3.1f, 0.4f)));
        Check(z.Count(Origin::Own) == 1, "our throw's explosion: an own zone at once (before it is decided)");
        Check(Run(d, 13.05, seq).empty(), "not decided before the settle time");
        const auto out = Run(d, 13.3, seq);
        Check(out.size() == 1 && out[0].send && out[0].reports == 12 && out[0].e.kind == Kind::Grenade &&
                  out[0].e.weapon == "exp_grenade" && out[0].e.seq == 1 && !(out[0].e.flags & kFire),
              "12 reports -> one grenade of ours, sent", out.empty() ? "none" : out[0].why);
        Check(seq == 2 && d.PendingThrows() == 0, "the throw is used up");
        Check(z.Count(Origin::Own) == 1, "no second own zone for the same explosion");
        d.OnSound(Snd(20, kSoundExplosion, P(15, 3)));
        const auto o2 = Run(d, 20.5, seq);
        Check(o2.size() == 1 && !o2[0].send, "a second explosion without a throw is not ours", o2.empty() ? "" : o2[0].why);
    }
    {  // the sound names our player
        Zones z;
        Detector d(z);
        uint32_t seq = 50;
        d.OnSound(Snd(5, kSoundExplosion, P(1, 1), Owner::Us));
        const auto out = Run(d, 6, seq);
        Check(out.size() == 1 && out[0].send && out[0].e.seq == 50 && out[0].e.kind == Kind::Unknown &&
                  out[0].e.weapon.empty(),
              "owner us, no throw seen: sent, kind unknown");
        Check(z.Count(Origin::Own) == 1, "owner us: own zone");
    }
    {  // a molotov: the throw's weapon, fire flag
        Zones z;
        Detector d(z);
        uint32_t seq = 1;
        d.OnThrow(Thr(1, "exp_molotov", P(0, 0)));
        d.OnSound(Snd(2.1, kSoundExplosion, P(12, 0)));
        d.OnSound(Snd(2.3, kSoundFire, P(12.5f, 0)));
        const auto out = Run(d, 2.5, seq);
        Check(out.size() == 1 && out[0].send && out[0].e.kind == Kind::Molotov && (out[0].e.flags & kFire),
              "molotov: kind and fire flag");
        Check(z.Find(Origin::Own, P(13, 1), 20, kDmgFire) != nullptr, "molotov: our fire zone burns on");
    }
    {  // an unknown throw (weapon unreadable) whose explosion burns: molotov from the fire sound
        Zones z;
        Detector d(z);
        uint32_t seq = 1;
        d.OnThrow(Thr(1, "", P(0, 0)));
        d.OnSound(Snd(2.0, kSoundExplosion, P(10, 0)));
        d.OnSound(Snd(2.4, kSoundFire, P(10, 1)));
        const auto out = Run(d, 3, seq);
        Check(out.size() == 1 && out[0].send && out[0].e.kind == Kind::Molotov && out[0].e.weapon == "exp_molotov" &&
                  (out[0].e.flags & kFire),
              "unknown weapon + fire sound: a molotov");
    }
    {  // two throws, two explosions, in order
        Zones z;
        Detector d(z);
        uint32_t seq = 1;
        d.OnThrow(Thr(1, "exp_grenade", P(0, 0)));
        d.OnThrow(Thr(2, "exp_molotov", P(0, 0)));
        d.OnSound(Snd(4, kSoundExplosion, P(10, 0)));
        d.OnSound(Snd(4.1, kSoundExplosion, P(-20, 5)));
        const auto out = Run(d, 5, seq);
        Check(out.size() == 2 && out[0].send && out[1].send && out[0].e.kind == Kind::Grenade &&
                  out[1].e.kind == Kind::Molotov && out[0].e.seq == 1 && out[1].e.seq == 2,
              "two throws -> two explosions, oldest throw first");
    }
    {  // a throw too long ago or too far: not ours
        Zones z;
        Detector d(z);
        uint32_t seq = 1;
        d.OnThrow(Thr(1, "exp_grenade", P(0, 0)));
        d.OnSound(Snd(1 + kThrowWindow + 1, kSoundExplosion, P(5, 0)));
        auto out = Run(d, 1 + kThrowWindow + 2, seq);
        Check(out.size() == 1 && !out[0].send, "an explosion long after our throw is not ours");
        d.OnThrow(Thr(30, "exp_grenade", P(0, 0)));
        d.OnSound(Snd(31, kSoundExplosion, P(kThrowReach + 10, 0)));
        out = Run(d, 32, seq);
        Check(out.size() == 1 && !out[0].send && d.PendingThrows() == 1, "one far from our throw is not ours");
    }
    {  // an NPC's grenade while we have a throw pending: the NPC's (host), our throw stays pending
        Zones z;
        Detector d(z);
        uint32_t seq = 1;
        d.OnThrow(Thr(1, "exp_grenade", P(0, 0)));
        d.OnSound(Snd(2, kSoundExplosion, P(5, 0), Owner::OurNpc, 0x1208));
        const auto out = Run(d, 3, seq);
        Check(out.size() == 1 && out[0].send && (out[0].e.flags & kNpcOwner) && out[0].e.npc == 0x1208 &&
                  d.PendingThrows() == 1 && z.Count(Origin::Own) == 0,
              "our NPC's grenade: sent as the NPC's, no own zone, our throw still pending", out.empty() ? "" : out[0].why);
    }
    {  // reports of one explosion: the most telling owner wins
        Zones z;
        Detector d(z);
        uint32_t seq = 1;
        d.OnSound(Snd(2, kSoundExplosion, P(5, 0), Owner::Other));
        d.OnSound(Snd(2.01, kSoundExplosion, P(5.5f, 0), Owner::Us));
        const auto out = Run(d, 3, seq);
        Check(out.size() == 1 && out[0].send && out[0].owner == Owner::Us, "owner merged: us beats other");
    }
    {  // somebody else's (an enemy of the guest's own game, the environment)
        Zones z;
        Detector d(z);
        uint32_t seq = 1;
        d.OnSound(Snd(2, kSoundExplosion, P(5, 0), Owner::Other));
        const auto out = Run(d, 3, seq);
        Check(out.size() == 1 && !out[0].send && seq == 1, "somebody else's explosion: not sent");
    }
    {  // PLAYER_GRENADE_EXPLOSION with it
        Zones z;
        Detector d(z);
        uint32_t seq = 1;
        d.OnGrenadeMessage(7.9);
        d.OnSound(Snd(8, kSoundExplosion, P(5, 0)));
        const auto out = Run(d, 9, seq);
        Check(out.size() == 1 && out[0].send, "PLAYER_GRENADE_EXPLOSION makes it ours");
    }
    {  // a barrel we shot
        Zones z;
        Detector d(z);
        uint32_t seq = 1;
        d.OnShot(4.0, P(0, 0));
        d.OnSound(Snd(4.5, kSoundExplosion, P(30, 0), Owner::Other, 0, "lh_barrel_explosive_a-07"));
        const auto out = Run(d, 5, seq);
        Check(out.size() == 1 && out[0].send && out[0].e.kind == Kind::Object && out[0].e.object == "lh_barrel_explosive_a-07",
              "a barrel blowing up right after our shot: ours, kind object, its name");
        d.OnSound(Snd(20, kSoundExplosion, P(30, 0), Owner::Other, 0, "lh_barrel_explosive_a-08"));
        const auto o2 = Run(d, 21, seq);
        Check(o2.size() == 1 && !o2[0].send, "a barrel with no shot of ours: not ours");
    }
    {  // a chain: a barrel / car inside our grenade's blast a moment later
        Zones z;
        Detector d(z);
        uint32_t seq = 1;
        d.OnThrow(Thr(1, "exp_grenade", P(0, 0)));
        d.OnSound(Snd(3, kSoundExplosion, P(10, 0)));
        d.OnSound(Snd(4.0, kSoundExplosion, P(16, 0), Owner::Other, 0, "barrel"));
        auto out = Run(d, 5, seq);
        Check(out.size() == 2 && out[0].send && out[1].send && std::string(out[1].why) == "set off by our own explosion",
              "chain: set off by our grenade is ours", out.size() == 2 ? out[1].why : "");
        Check(z.Count(Origin::Own) == 2, "chain: an own zone for each", std::to_string(z.Count(Origin::Own)));
        d.OnSound(Snd(4.0 + kChainTime + 1, kSoundExplosion, P(16, 0), Owner::Other));
        out = Run(d, 10, seq);
        Check(out.size() == 1 && !out[0].send, "chain: not seconds later");
    }
    {  // an early guess that turns out wrong (the pending throw went to an earlier explosion) leaves no zone
        Zones z;
        Detector d(z);
        uint32_t seq = 1;
        d.OnThrow(Thr(1, "exp_grenade", P(0, 0)));
        d.OnSound(Snd(3, kSoundExplosion, P(10, 0)));
        d.OnSound(Snd(3.1, kSoundExplosion, P(-40, 0)));  // also fits the throw while it is pending
        Check(z.Count(Origin::Own) == 2, "two early guesses");
        const auto out = Run(d, 4, seq);
        Check(out.size() == 2 && out[0].send && !out[1].send && z.Count(Origin::Own) == 1,
              "the second one is not ours: its early zone is removed", std::to_string(z.Count(Origin::Own)));
    }
    {  // sounds that are not explosions
        Zones z;
        Detector d(z);
        uint32_t seq = 1;
        d.OnSound(Snd(1, kSoundBounce, P(0, 0), Owner::Us));
        d.OnSound(Snd(1, 5, P(0, 0), Owner::Us));
        Check(Run(d, 3, seq).empty() && d.Pending() == 0, "bounces and shots are no explosions");
        d.OnThrow(Thr(1, "exp_grenade", P(0, 0)));
        d.Clear();
        Check(d.PendingThrows() == 0, "Clear forgets throws");
    }
}

// A whole exchange: guest 1 throws a grenade between a host NPC and the host's puppet on his screen. The guest's game
// decides who is hurt; the host's picture of it hurts nobody there.
void TestWalkThrough() {
    // guest (thrower)
    Zones gz;
    Detector gd(gz);
    uint32_t gseq = 1;
    gd.OnThrow(Thr(100, "exp_grenade", P(0, 0)));
    for (int i = 0; i < 5; ++i) gd.OnSound(Snd(103 + i * 0.02, kSoundExplosion, P(12, 2)));
    // damage events on the guest's game right away: the host's puppet (named attacker: us), a host NPC copy (no
    // attacker named, UNVERIFIED case), the guest himself (his own grenade)
    const Verdict puppet = Judge(gz, Victim::Puppet, Enemy::Us, kDmgExplosion, P(13, 2), 103.05).verdict;
    const Verdict copy = Judge(gz, Victim::Npc, Enemy::None, kDmgExplosion, P(11, 3), 103.05).verdict;
    const Verdict self = Judge(gz, Victim::LocalPlayer, Enemy::Us, kDmgExplosion, P(1, 0), 103.05).verdict;
    Check(puppet == Verdict::Keep && copy == Verdict::Ours && self == Verdict::Keep,
          "thrower: puppet hit kept (-> Hit), NPC copy unnamed -> ours (-> NpcHit), own hurt kept");
    std::vector<Seen> out;
    gd.Tick(103.5, gseq, out);
    Check(out.size() == 1 && out[0].send, "thrower: one Explosion sent");
    const auto wire = PackExplosion(out[0].e);

    // host (receiver)
    Inbox inbox;
    mc::net::ByteReader r(wire.data(), wire.size());
    Explosion e;
    Check(ReadExplosion(r, e) && inbox.Accept(1, e.seq), "receiver: read and accepted");
    mc::net::ByteReader r2(wire.data(), wire.size());
    Explosion e2;
    Check(ReadExplosion(r2, e2) && !inbox.Accept(1, e2.seq), "receiver: a duplicate is dropped");
    Zones hz;
    Zone rz;
    rz.origin = Origin::Remote;
    rz.pos = e.pos;
    rz.t = 203.6;  // the host's own clock
    rz.from = 1;
    rz.id = e.seq;
    hz.Add(rz);
    // the picture's (if any) damage on the host: the host himself, his NPC, the guest's puppet
    const Verdict h1 = Judge(hz, Victim::LocalPlayer, Enemy::None, kDmgExplosion, P(14, 2), 203.7).verdict;
    const Verdict h2 = Judge(hz, Victim::Npc, Enemy::None, kDmgExplosion, P(11, 3), 203.7).verdict;
    const Verdict h3 = Judge(hz, Victim::Puppet, Enemy::None, kDmgExplosion, P(1, 0), 203.7).verdict;
    // and the host's own NPC shooting the host at the same moment: a bullet, kept
    const Verdict h4 = Judge(hz, Victim::LocalPlayer, Enemy::OurNpc, 0, P(14, 2), 203.7).verdict;
    Check(h1 == Verdict::Undo && h2 == Verdict::Undo && h3 == Verdict::Undo && h4 == Verdict::Keep,
          "receiver: the picture's damage undone everywhere, the real fight kept");
}

}  // namespace

int main() {
    TestWire();
    TestInbox();
    TestZones();
    TestJudge();
    TestDetector();
    TestWalkThrough();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
