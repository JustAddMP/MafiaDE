#include "game/explosion_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mc::game::boom {

float Dist(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

namespace {

constexpr float kMaxCoord = 100000.0f;

bool Finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) <= kMaxCoord &&
           std::fabs(v.y) <= kMaxCoord && std::fabs(v.z) <= kMaxCoord;
}

void WriteVec(net::ByteWriter& w, const Vec3& v) {
    w.F32(v.x);
    w.F32(v.y);
    w.F32(v.z);
}

Vec3 ReadVec(net::ByteReader& r) {
    Vec3 v;
    v.x = r.F32();
    v.y = r.F32();
    v.z = r.F32();
    return v;
}

}  // namespace

const char* KindName(Kind k) {
    switch (k) {
        case Kind::Grenade: return "grenade";
        case Kind::Molotov: return "molotov";
        case Kind::Satchel: return "dynamite";
        case Kind::Object: return "object";
        case Kind::Car: return "car";
        default: return "unknown";
    }
}

Kind KindOfWeapon(std::string_view w) {
    if (w == "exp_grenade") return Kind::Grenade;
    if (w == "exp_molotov") return Kind::Molotov;
    if (w == "exp_satchel") return Kind::Satchel;
    return Kind::Unknown;
}

const char* WeaponOfKind(Kind k) {
    switch (k) {
        case Kind::Molotov: return "exp_molotov";
        case Kind::Satchel: return "exp_satchel";
        default: return "exp_grenade";
    }
}

bool SafeName(std::string_view s, size_t max) {
    if (s.empty() || s.size() > max) return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-' || c == '.' || c == ':' || c == '/';
        if (!ok) return false;
    }
    return true;
}

std::vector<uint8_t> PackExplosion(const Explosion& e) {
    net::ByteWriter w(96);
    w.U32(e.seq);
    w.U8(static_cast<uint8_t>(e.kind));
    w.U8(e.flags);
    WriteVec(w, e.pos);
    w.Str(SafeName(e.weapon, 32) ? e.weapon : std::string());
    w.U32(e.npc);
    w.Str(SafeName(e.object, 64) ? e.object : std::string());
    w.U32(e.throwSeq);
    return std::vector<uint8_t>(w.Span().begin(), w.Span().end());
}

bool ReadExplosion(net::ByteReader& r, Explosion& out) {
    Explosion e;
    e.seq = r.U32();
    const uint8_t kind = r.U8();
    e.flags = r.U8();
    e.pos = ReadVec(r);
    e.weapon = r.Str(32);
    e.npc = r.U32();
    e.object = r.Str(64);
    if (r.Ok() && r.Remaining() > 0) e.throwSeq = r.U32();  // optional (older senders end before it); 1-3 bytes fail
    if (r.Failed() || kind > kMaxKind || !Finite(e.pos)) return false;
    if (!e.weapon.empty() && !SafeName(e.weapon, 32)) return false;
    if (!e.object.empty() && !SafeName(e.object, 64)) return false;
    e.kind = static_cast<Kind>(kind);
    e.flags &= (kFire | kNpcOwner);
    out = std::move(e);
    return true;
}

std::vector<uint8_t> PackCarBlast(const CarBlast& c) {
    net::ByteWriter w(48);
    w.U8(static_cast<uint8_t>(c.what));
    w.Str(SafeName(c.key, 24) ? c.key : std::string());
    WriteVec(w, c.pos);
    return std::vector<uint8_t>(w.Span().begin(), w.Span().end());
}

bool ReadCarBlast(net::ByteReader& r, CarBlast& out) {
    CarBlast c;
    const uint8_t what = r.U8();
    c.key = r.Str(24);
    c.pos = ReadVec(r);
    if (r.Failed() || what > 1 || !SafeName(c.key, 24) || !Finite(c.pos)) return false;
    c.what = static_cast<CarWhat>(what);
    out = std::move(c);
    return true;
}

// ---- Inbox ------------------------------------------------------------------------------------------------------------
bool Inbox::Accept(uint32_t from, uint32_t seq) {
    auto& q = seen_[from];
    if (std::find(q.begin(), q.end(), seq) != q.end()) return false;
    q.push_back(seq);
    if (q.size() > kKeep) q.pop_front();
    return true;
}

void Inbox::Forget(uint32_t from) { seen_.erase(from); }

// ---- Zones ------------------------------------------------------------------------------------------------------------
void Zones::Add(const Zone& z) {
    zones_.push_back(z);
    if (zones_.size() > kMax) zones_.erase(zones_.begin());
}

const Zone* Zones::Find(Origin origin, const Vec3& pos, double now, int dtype, double before) const {
    const Zone* best = nullptr;
    for (const Zone& z : zones_) {
        if (z.origin != origin || z.t >= before) continue;
        const double dt = now - z.t;
        if (dt < -kEarly) continue;
        const float d = Dist(z.pos, pos);
        bool covers = false;
        if (dtype == kDmgExplosion) {
            covers = d <= kBlastRadius && dt <= kBlastTime;
        } else if (dtype == kDmgFire) {
            covers = (z.fire && d <= kFireRadius && dt <= kFireTime) || (d <= kBlastRadius && dt <= kBlastTime + 5.0);
        }
        if (covers && (!best || z.t > best->t)) best = &z;
    }
    return best;
}

void Zones::Expire(double now) {
    zones_.erase(std::remove_if(zones_.begin(), zones_.end(),
                                [now](const Zone& z) {
                                    const double keep = z.fire ? kFireTime + 5.0 : kBlastTime + 10.0;
                                    return now - z.t > keep;
                                }),
                 zones_.end());
}

void Zones::Remove(Origin origin, double t, const Vec3& pos) {
    zones_.erase(std::remove_if(zones_.begin(), zones_.end(),
                                [&](const Zone& z) {
                                    return z.origin == origin && z.t == t && z.pos.x == pos.x && z.pos.y == pos.y &&
                                           z.pos.z == pos.z;
                                }),
                 zones_.end());
}

size_t Zones::Count(Origin o) const {
    return static_cast<size_t>(std::count_if(zones_.begin(), zones_.end(), [o](const Zone& z) { return z.origin == o; }));
}

// ---- the damage guard -------------------------------------------------------------------------------------------------
const char* VictimName(Victim v) {
    switch (v) {
        case Victim::LocalPlayer: return "local";
        case Victim::Puppet: return "puppet";
        case Victim::NpcCopy: return "copy";
        default: return "npc";
    }
}

const char* EnemyName(Enemy e) {
    switch (e) {
        case Enemy::Us: return "us";
        case Enemy::OurNpc: return "npc";
        case Enemy::Puppet: return "puppet";
        case Enemy::NpcCopy: return "copy";
        case Enemy::None: return "none";
        default: return "other";
    }
}

const char* VerdictName(Verdict v) {
    switch (v) {
        case Verdict::Undo: return "undo";
        case Verdict::Ours: return "ours";
        default: return "keep";
    }
}

Victim VictimFromName(const char* s) {
    if (!s) return Victim::Npc;
    if (!std::strcmp(s, "local")) return Victim::LocalPlayer;
    if (!std::strcmp(s, "puppet")) return Victim::Puppet;
    if (!std::strcmp(s, "copy")) return Victim::NpcCopy;
    return Victim::Npc;
}

Enemy EnemyFromName(const char* s) {
    if (!s) return Enemy::Other;
    if (!std::strcmp(s, "us")) return Enemy::Us;
    if (!std::strcmp(s, "npc")) return Enemy::OurNpc;
    if (!std::strcmp(s, "puppet")) return Enemy::Puppet;
    if (!std::strcmp(s, "copy")) return Enemy::NpcCopy;
    if (!std::strcmp(s, "none")) return Enemy::None;
    return Enemy::Other;
}

Judgement Judge(const Zones& zones, Victim victim, Enemy enemy, int dtype, const Vec3& pos, double now) {
    Judgement j;
    if (dtype != kDmgExplosion && dtype != kDmgFire) return j;
    const Zone* remote = zones.Find(Origin::Remote, pos, now, dtype);
    const Zone* own = zones.Find(Origin::Own, pos, now, dtype);
    // an attacker this game does not simulate as a fighter of its own: nobody, a puppet (a picture of another
    // player), a spawned NPC copy (a picture of another game's NPC), something else (the explosion itself)
    const bool foreign = enemy == Enemy::None || enemy == Enemy::Puppet || enemy == Enemy::NpcCopy ||
                         enemy == Enemy::Other;
    const bool unnamed = enemy == Enemy::None || enemy == Enemy::Other;
    if (remote && foreign && (!own || remote->t >= own->t || !unnamed)) {
        j.verdict = Verdict::Undo;
        j.zone = remote;
        return j;
    }
    if (own && unnamed && victim != Victim::LocalPlayer) {
        j.verdict = Verdict::Ours;
        j.zone = own;
        return j;
    }
    return j;
}

// ---- the detector -----------------------------------------------------------------------------------------------------
const char* OwnerName(Owner o) {
    switch (o) {
        case Owner::Us: return "us";
        case Owner::OurNpc: return "npc";
        case Owner::Other: return "other";
        default: return "unknown";
    }
}

Owner OwnerFromName(const char* s) {
    if (!s) return Owner::Unknown;
    if (!std::strcmp(s, "us")) return Owner::Us;
    if (!std::strcmp(s, "npc")) return Owner::OurNpc;
    if (!std::strcmp(s, "other")) return Owner::Other;
    return Owner::Unknown;
}

void Detector::OnThrow(const Throw& t) {
    throws_.push_back(t);
    if (throws_.size() > 8) throws_.erase(throws_.begin());
}

void Detector::OnGrenadeMessage(double t) {
    grenadeMsgs_.push_back(t);
    if (grenadeMsgs_.size() > 8) grenadeMsgs_.erase(grenadeMsgs_.begin());
}

void Detector::OnShot(double t, const Vec3& from) {
    shots_.push_back({t, from});
    if (shots_.size() > 16) shots_.erase(shots_.begin());
}

const Throw* Detector::MatchThrow(const Vec3& pos, double t) const {
    const Throw* best = nullptr;
    for (const Throw& th : throws_) {  // the oldest pending throw that fits: throws explode in the order thrown
        const double dt = t - th.t;
        if (dt < -0.2 || dt > kThrowWindow || Dist(th.pos, pos) > kThrowReach) continue;
        if (!best || th.t < best->t) best = &th;
    }
    return best;
}

void Detector::OnSound(const Sound& s) {
    if (s.type == kSoundFire) {
        fires_.push_back({s.t, s.pos});
        if (fires_.size() > 32) fires_.erase(fires_.begin());
        return;
    }
    if (s.type != kSoundExplosion) return;
    for (Cluster& c : clusters_) {
        if (std::fabs(s.t - c.t0) <= kMergeTime && Dist(c.pos, s.pos) <= kMerge) {
            ++c.reports;
            // the most telling owner wins: us > our NPC > other > unknown
            auto rank = [](Owner o) { return o == Owner::Us ? 3 : o == Owner::OurNpc ? 2 : o == Owner::Other ? 1 : 0; };
            if (rank(s.owner) > rank(c.owner)) {
                c.owner = s.owner;
                c.npc = s.npc;
            }
            if (c.object.empty()) c.object = s.object;
            return;
        }
    }
    Cluster c;
    c.t0 = s.t;
    c.pos = s.pos;
    c.owner = s.owner;
    c.npc = s.npc;
    c.object = s.object;
    c.reports = 1;
    if (s.owner == Owner::Us || (s.owner != Owner::OurNpc && MatchThrow(s.pos, s.t))) {
        Zone z;
        z.origin = Origin::Own;
        z.pos = s.pos;
        z.t = s.t;
        const Throw* th = MatchThrow(s.pos, s.t);
        z.kind = th ? KindOfWeapon(th->weapon) : Kind::Unknown;
        z.fire = z.kind == Kind::Molotov;
        zones_.Add(z);
        c.early = true;
    }
    clusters_.push_back(std::move(c));
    if (clusters_.size() > 32) clusters_.erase(clusters_.begin());
}

void Detector::Prune(double now) {
    throws_.erase(std::remove_if(throws_.begin(), throws_.end(),
                                 [now](const Throw& t) { return now - t.t > kThrowWindow; }),
                  throws_.end());
    grenadeMsgs_.erase(std::remove_if(grenadeMsgs_.begin(), grenadeMsgs_.end(),
                                      [now](double t) { return now - t > 5.0; }),
                       grenadeMsgs_.end());
    shots_.erase(std::remove_if(shots_.begin(), shots_.end(),
                                [now](const std::pair<double, Vec3>& s) { return now - s.first > kShotWindow + 1.0; }),
                 shots_.end());
    fires_.erase(std::remove_if(fires_.begin(), fires_.end(),
                                [now](const std::pair<double, Vec3>& s) { return now - s.first > 5.0; }),
                 fires_.end());
}

void Detector::Tick(double now, uint32_t& nextSeq, std::vector<Seen>& out) {
    for (auto it = clusters_.begin(); it != clusters_.end();) {
        Cluster& c = *it;
        if (now - c.t0 < kSettle) {
            ++it;
            continue;
        }
        Seen seen;
        seen.owner = c.owner;
        seen.reports = c.reports;
        seen.e.pos = c.pos;
        seen.e.object = c.object;
        bool ours = false;
        const Throw* th = c.owner == Owner::OurNpc ? nullptr : MatchThrow(c.pos, c.t0);
        bool msg = false;
        for (double t : grenadeMsgs_)
            if (std::fabs(t - c.t0) <= kGrenadeMsgSlack) msg = true;
        bool shot = false;
        for (const auto& s : shots_)
            if (c.t0 - s.first >= -0.1 && c.t0 - s.first <= kShotWindow && Dist(s.second, c.pos) <= kShotReach)
                shot = true;
        // an earlier explosion of ours right there (not this one's own early zone)
        const Zone* chain = nullptr;
        if (c.owner != Owner::OurNpc) {
            chain = zones_.Find(Origin::Own, c.pos, c.t0, kDmgExplosion, c.t0 - 0.05);
            if (chain && c.t0 - chain->t > kChainTime) chain = nullptr;
        }
        if (c.owner == Owner::Us) {
            ours = true, seen.why = "the sound's owner is our player";
        } else if (msg && c.owner != Owner::OurNpc) {
            ours = true, seen.why = "PLAYER_GRENADE_EXPLOSION came with it";
        } else if (th) {
            ours = true, seen.why = "our throw";
        } else if (!c.object.empty() && shot && c.owner != Owner::OurNpc) {
            ours = true, seen.why = "our shot set the object off";
        } else if (chain) {
            ours = true, seen.why = "set off by our own explosion";
        } else if (c.owner == Owner::OurNpc) {
            seen.why = "our NPC's";
        } else {
            seen.why = c.owner == Owner::Other ? "not ours: somebody else's" : "not ours: no owner, no throw of ours";
        }
        // what it was
        Kind kind = Kind::Unknown;
        std::string weapon;
        if (th && !th->weapon.empty()) {
            weapon = th->weapon;
            kind = KindOfWeapon(weapon);
        }
        if (th && ours) {
            seen.e.throwSeq = th->seq;
            seen.sinceThrow = c.t0 - th->t;
        }
        bool fire = false;
        for (const auto& f : fires_)
            if (std::fabs(f.first - c.t0) <= 2.0 && Dist(f.second, c.pos) <= kFireRadius) fire = true;
        if (kind == Kind::Unknown) {
            if (!c.object.empty()) kind = Kind::Object;
            else if (fire) kind = Kind::Molotov;
        }
        if (kind == Kind::Molotov) fire = true;
        if (weapon.empty() && kind != Kind::Unknown && kind != Kind::Object) weapon = WeaponOfKind(kind);
        seen.e.kind = kind;
        seen.e.weapon = weapon;
        seen.e.flags = fire ? kFire : 0;
        if (ours || c.owner == Owner::OurNpc) {
            seen.send = true;
            seen.e.seq = nextSeq++;
            if (!ours) {
                seen.e.flags |= kNpcOwner;
                seen.e.npc = c.npc;
            }
        }
        if (ours) {
            if (c.early) {
                // the early zone: now that the kind is known it may be a fire zone
                // (Zones has no update: a fire zone is added beside it; Find takes the newest)
                if (fire) {
                    Zone z;
                    z.origin = Origin::Own;
                    z.kind = kind;
                    z.fire = true;
                    z.pos = c.pos;
                    z.t = c.t0;
                    z.id = seen.e.seq;
                    zones_.Add(z);
                }
            } else {
                Zone z;
                z.origin = Origin::Own;
                z.kind = kind;
                z.fire = fire;
                z.pos = c.pos;
                z.t = c.t0;
                z.id = seen.e.seq;
                zones_.Add(z);
            }
            if (th) {  // one throw, one explosion
                const double tt = th->t;
                throws_.erase(std::remove_if(throws_.begin(), throws_.end(), [tt](const Throw& t) { return t.t == tt; }),
                              throws_.end());
            }
        }
        if (!ours && c.early) zones_.Remove(Origin::Own, c.t0, c.pos);  // the early guess was wrong
        out.push_back(std::move(seen));
        it = clusters_.erase(it);
    }
    Prune(now);
}

void Detector::Clear() {
    clusters_.clear();
    throws_.clear();
    grenadeMsgs_.clear();
    shots_.clear();
    fires_.clear();
}

}  // namespace mc::game::boom
