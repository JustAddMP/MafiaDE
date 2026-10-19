#include "game/npc_replica.h"

#include "net/wire.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace mc::game::npcrep {

namespace {
int16_t Q16(float v, float scale) {
    const float q = std::round(v * scale);
    return static_cast<int16_t>(std::clamp(q, -32767.0f, 32767.0f));
}
int8_t Q8(float v) {
    if (!std::isfinite(v)) return 0;
    return static_cast<int8_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 127));
}
bool Finite3(float x, float y, float z) { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }

// heading <-> a yaw quaternion (so the replica layer's nlerp turns it smoothly)
void YawQuat(float hx, float hy, float q[4]) {
    const float a = std::atan2(hy, hx) * 0.5f;
    q[0] = 0;
    q[1] = 0;
    q[2] = std::sin(a);
    q[3] = std::cos(a);
}
void QuatYaw(const float q[4], float& hx, float& hy) {
    const float a = 2.0f * std::atan2(q[2], q[3]);
    hx = std::cos(a);
    hy = std::sin(a);
}
}  // namespace

// ---- wire ----------------------------------------------------------------------------------------------------------------
std::vector<std::vector<uint8_t>> PackStates(uint32_t timeMs, const std::vector<Fast>& npcs) {
    std::vector<std::vector<uint8_t>> out;
    std::vector<const Fast*> left;
    for (const Fast& f : npcs)
        if (Finite3(f.x, f.y, f.z)) left.push_back(&f);
    while (!left.empty()) {
        const float ox = std::round(left[0]->x), oy = std::round(left[0]->y), oz = std::round(left[0]->z);
        std::vector<const Fast*> in, rest;
        for (const Fast* f : left) {
            const bool fits = std::fabs(f->x - ox) <= kMaxOffset && std::fabs(f->y - oy) <= kMaxOffset &&
                              std::fabs(f->z - oz) <= kMaxOffset;
            (fits && in.size() < kMaxPerStates ? in : rest).push_back(f);
        }
        net::ByteWriter w;
        w.U32(timeMs);
        w.F32(ox);
        w.F32(oy);
        w.F32(oz);
        w.U8(static_cast<uint8_t>(in.size()));
        for (const Fast* f : in) {
            w.U16(f->net);
            w.I16(Q16(f->x - ox, 100));
            w.I16(Q16(f->y - oy, 100));
            w.I16(Q16(f->z - oz, 100));
            w.I8(Q8(f->hx));
            w.I8(Q8(f->hy));
            w.I16(Q16(std::isfinite(f->vx) ? f->vx : 0, 100));
            w.I16(Q16(std::isfinite(f->vy) ? f->vy : 0, 100));
            w.U16(f->hp);
            w.U8(f->flags);
            w.U8(f->shots);
            w.U8(f->gait);
        }
        out.push_back(w.Take());
        left.swap(rest);
    }
    return out;
}

bool ReadStates(net::ByteReader& r, StatesMsg& out) {
    StatesMsg m;
    m.timeMs = r.U32();
    const float ox = r.F32(), oy = r.F32(), oz = r.F32();
    const uint8_t n = r.U8();
    if (r.Failed() || !Finite3(ox, oy, oz) || n > kMaxPerStates) return false;
    m.npcs.resize(n);
    for (Fast& f : m.npcs) {
        f.net = r.U16();
        f.x = ox + r.I16() / 100.0f;
        f.y = oy + r.I16() / 100.0f;
        f.z = oz + r.I16() / 100.0f;
        f.hx = r.I8() / 127.0f;
        f.hy = r.I8() / 127.0f;
        f.vx = r.I16() / 100.0f;
        f.vy = r.I16() / 100.0f;
        f.hp = r.U16();
        f.flags = r.U8();
        f.shots = r.U8();
        f.gait = static_cast<uint8_t>(std::min<int>(r.U8(), 3));
    }
    if (r.Failed()) return false;
    out = std::move(m);
    return true;
}

std::vector<std::vector<uint8_t>> PackInfos(const std::vector<Info>& infos) {
    std::vector<std::vector<uint8_t>> out;
    for (size_t at = 0; at < infos.size();) {
        net::ByteWriter w;
        w.U8(0);
        size_t n = 0;
        for (; at + n < infos.size() && n < 255 && w.Size() + kMaxInfoBytes <= 1000; ++n) {
            const Info& i = infos[at + n];
            w.U16(i.net);
            w.U32(i.key);
            w.U8(i.ai);
            w.U32(i.tag);
            w.Str(npcown::SafeWeapon(i.weapon) ? i.weapon : "-");
            w.U64(i.profile);
            w.U8(static_cast<uint8_t>(i.cls));
        }
        std::vector<uint8_t> buf = w.Take();
        buf[0] = static_cast<uint8_t>(n);
        at += n;
        out.push_back(std::move(buf));
    }
    return out;
}

bool ReadInfos(net::ByteReader& r, std::vector<Info>& out) {
    const uint8_t n = r.U8();
    std::vector<Info> v(n);
    for (Info& i : v) {
        i.net = r.U16();
        i.key = r.U32();
        i.ai = r.U8();
        i.tag = r.U32();
        i.weapon = r.Str(48);
        if (!npcown::SafeWeapon(i.weapon)) i.weapon = "-";
        i.profile = r.U64();
        const uint8_t c = r.U8();
        i.cls = c <= 2 ? static_cast<NameClass>(c) : NameClass::Unknown;
    }
    if (r.Failed()) return false;
    out = std::move(v);
    return true;
}

std::vector<std::vector<uint8_t>> PackLeaves(const std::vector<uint16_t>& nets) {
    std::vector<std::vector<uint8_t>> out;
    for (size_t at = 0; at < nets.size();) {
        const size_t n = std::min<size_t>(255, nets.size() - at);
        net::ByteWriter w;
        w.U8(static_cast<uint8_t>(n));
        for (size_t i = 0; i < n; ++i) w.U16(nets[at + i]);
        out.push_back(w.Take());
        at += n;
    }
    return out;
}

bool ReadLeaves(net::ByteReader& r, std::vector<uint16_t>& out) {
    const uint8_t n = r.U8();
    std::vector<uint16_t> v(n);
    for (uint16_t& x : v) x = r.U16();
    if (r.Failed()) return false;
    out = std::move(v);
    return true;
}

// ---- rules ---------------------------------------------------------------------------------------------------------------
NameClass ClassOfName(const std::string& name) {
    static const std::string kPrefix = "behavior_object_prime-";
    if (name.compare(0, kPrefix.size(), kPrefix) != 0) return NameClass::Unknown;
    // behavior_object_prime-<hex>-<suffix>
    size_t i = kPrefix.size(), hex = 0;
    while (i < name.size() && std::isxdigit(static_cast<unsigned char>(name[i]))) {
        ++i;
        ++hex;
    }
    if (hex == 0 || i >= name.size() || name[i] != '-') return NameClass::Unknown;
    const std::string suffix = name.substr(i + 1);
    // nothing after it, or another generated name (the puppet factory's driver made it): no mission spawner
    if (suffix.empty() || suffix.compare(0, kPrefix.size() - 1, "behavior_object_prime") == 0)
        return NameClass::Generator;
    return NameClass::Spawner;
}

bool IsAmbientHuman(bool playerOrCopy, bool inVehicle, int aiType, NameClass cls) {
    return !playerOrCopy && !inVehicle && aiType == 0 && cls != NameClass::Spawner;
}

bool SkipRelayedHit(HitRole role, bool alive, bool hostile) { return role == HitRole::Own && alive && hostile; }

uint8_t GaitOf(float speed, uint8_t previous) {
    if (!std::isfinite(speed)) return 0;
    if (previous == 0 ? speed < 0.5f : speed < 0.35f) return 0;
    uint8_t g = previous == 0 ? 1 : previous;
    if (g == 1 && speed > 2.5f) g = 2;
    if (g == 2 && speed < 2.1f) g = 1;
    if (g == 2 && speed > 5.3f) g = 3;
    if (g == 3 && speed < 4.7f) g = speed < 2.1f ? 1 : 2;
    if (previous == 0 && speed > 5.3f) g = 3;
    return g;
}

// ---- authority side ------------------------------------------------------------------------------------------------------
size_t PeerOut::Bytes() const {
    size_t b = 0;
    if (!leaves.empty()) b += 1 + 2 * leaves.size();
    for (const Info& i : infos) b += 21 + i.weapon.size();
    if (!infos.empty()) b += 1;
    for (const Group& g : states) b += kStatesHeader + kFastBytes * g.npcs.size();
    return b;
}

Publisher::Publisher(const PublisherTuning& t) : t_(t) { Reset(); }

void Publisher::Reset() {
    net::interest::Config cfg;
    cfg.bytesPerSecond = t_.budget;
    cfg.sendToOwner = false;
    net::interest::KindConfig& k = cfg.kinds[static_cast<int>(net::interest::Kind::Npc)];
    k.enterRadius = t_.enter;
    k.leaveRadius = t_.leave;
    k.nearDistance = t_.nearDist;
    k.nearInterval = t_.nearInterval;
    k.farInterval = t_.farInterval;
    k.restSpeed = t_.idleSpeed;
    k.restIntervalScale = t_.idleScale;
    k.maxInterval = t_.maxInterval;
    k.priority = net::interest::Priority::Normal;
    im_ = std::make_unique<net::InterestManager>(cfg);
    ents_.clear();
    nets_.clear();
    nextNet_ = 1;
    peers_.clear();
    gone_.clear();
    truth_.clear();
}

void Publisher::SetPeers(const std::vector<PlayerId>& peers) {
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (std::find(peers.begin(), peers.end(), it->first) == peers.end()) {
            im_->RemovePeer(it->first);
            it = peers_.erase(it);
        } else {
            ++it;
        }
    }
    for (PlayerId p : peers) {
        if (peers_.count(p)) continue;
        peers_[p];
        im_->AddPeer(p);
        im_->SetPeerBudget(p, t_.budget);
    }
}

void Publisher::SetViewpoints(PlayerId peer, const std::vector<Vec3>& points) {
    auto it = peers_.find(peer);
    if (it == peers_.end()) return;
    it->second.views.clear();
    for (const Vec3& v : points) it->second.views.push_back({v.x, v.y, v.z});
    im_->SetPeerViewpoints(peer, it->second.views);
}

uint16_t Publisher::Allocate() {
    for (int tries = 0; tries < 65536; ++tries) {
        const uint16_t n = nextNet_++;
        if (nextNet_ == 0) nextNet_ = 1;
        if (n != 0 && !nets_.count(n)) return n;
    }
    return 0;
}

uint16_t Publisher::NetOf(uint32_t key) const {
    auto it = ents_.find(key);
    return it == ents_.end() ? 0 : it->second.net;
}

uint64_t Publisher::SentBytes(PlayerId peer) const {
    auto it = peers_.find(peer);
    return it == peers_.end() ? 0 : it->second.bytes;
}

void Publisher::Remove(uint32_t key) {
    auto it = ents_.find(key);
    if (it == ents_.end()) return;
    im_->RemoveEntity(key);
    gone_.push_back(key);  // (each receiver that had it keeps its info, with the net id, until the leave goes out)
    nets_.erase(it->second.net);
    ents_.erase(it);
}

void Publisher::Round(uint32_t timeMs, double now, const std::vector<Item>& items, bool complete, PlayerId owner) {
    std::set<uint32_t> seen;
    for (const Item& item : items) {
        const NpcState& s = item.s;
        if (!Finite3(s.x, s.y, s.z)) continue;
        seen.insert(s.key);
        auto it = ents_.find(s.key);
        if (it == ents_.end()) {
            const uint16_t net = Allocate();
            if (net == 0) continue;
            Ent e;
            e.net = net;
            e.owner = item.owner;
            net::interest::EntityDesc d;
            d.id = s.key;
            d.owner = item.owner;
            d.kind = net::interest::Kind::Npc;
            d.pos = {s.x, s.y, s.z};
            d.hasVelocity = true;
            d.sizeBytes = static_cast<uint32_t>(kFastBytes);
            if (!im_->AddEntity(d)) continue;
            nets_[net] = s.key;
            it = ents_.emplace(s.key, e).first;
        }
        Ent& e = it->second;
        if (e.owner != item.owner) {
            e.owner = item.owner;
            im_->SetEntityOwner(s.key, item.owner);
        }
        e.peerMask = item.peerMask;
        // velocity from the last round (smoothed); none across a jump or for the dead
        float vx = e.fast.vx, vy = e.fast.vy;
        if (e.lastMs != 0 && timeMs != e.lastMs) {
            const double dt = static_cast<int32_t>(timeMs - e.lastMs) / 1000.0;
            const float dx = s.x - e.lastX, dy = s.y - e.lastY;
            if (dt > 0.004 && dt < 1.0 && std::hypot(dx, dy) < t_.teleport) {
                const float rx = static_cast<float>(dx / dt), ry = static_cast<float>(dy / dt);
                const float a = e.haveVel ? 0.5f : 1.0f;
                vx += (rx - vx) * a;
                vy += (ry - vy) * a;
                e.haveVel = true;
            } else if (dt >= 1.0 || std::hypot(dx, dy) >= t_.teleport) {
                vx = vy = 0;
                e.haveVel = false;
            }
        }
        if (s.flags & kDead) vx = vy = 0;
        e.lastX = s.x;
        e.lastY = s.y;
        e.lastZ = s.z;
        e.lastMs = timeMs;
        Fast& f = e.fast;
        f.net = e.net;
        f.x = s.x;
        f.y = s.y;
        f.z = s.z;
        f.hx = s.hx;
        f.hy = s.hy;
        f.vx = vx;
        f.vy = vy;
        f.hp = s.hp;
        f.flags = s.flags;
        f.shots = s.shots;
        f.gait = (s.flags & kDead) ? 0 : GaitOf(std::hypot(vx, vy), f.gait);
        Info& i = e.info;
        i.net = e.net;
        i.key = s.key;
        i.ai = s.ai;
        i.tag = s.tag;
        i.weapon = npcown::SafeWeapon(s.weapon) ? s.weapon : "-";
        i.profile = s.profile;
        i.cls = item.cls;
        e.timeMs = timeMs;
        e.seen = now;
        im_->UpdateEntity(s.key, {s.x, s.y, s.z}, {vx, vy, 0});
    }
    if (complete) {
        std::vector<uint32_t> drop;
        for (const auto& [key, e] : ents_)
            if (e.owner == owner && !seen.count(key)) drop.push_back(key);
        for (uint32_t key : drop) Remove(key);
    }
}

std::vector<PeerOut> Publisher::Tick(double now) {
    std::vector<uint32_t> stale;
    for (const auto& [key, e] : ents_)
        if (now - e.seen > t_.forgetAfter) stale.push_back(key);
    for (uint32_t key : stale) Remove(key);
    im_->Tick(now);
    std::vector<PeerOut> out;
    std::set<uint32_t> truthKeys;
    for (auto& [peer, know] : peers_) {
        PeerOut po;
        po.peer = peer;
        auto leave = [&](uint32_t key) {
            auto h = know.has.find(key);
            if (h == know.has.end()) return;
            po.leaves.push_back(h->second.net);
            know.has.erase(h);
            know.deadSent.erase(key);
        };
        for (uint32_t key : gone_) leave(key);
        for (const net::interest::Event& ev : im_->Events())
            if (ev.peer == peer && ev.type == net::interest::EventType::Leave) leave(ev.entity);
        // no longer for him (a relay that now goes to another guest)
        std::vector<uint32_t> masked;
        for (const auto& [key, info] : know.has) {
            auto e = ents_.find(key);
            if (e != ents_.end() && !(e->second.peerMask & (1u << (peer & 31)))) masked.push_back(key);
        }
        for (uint32_t key : masked) leave(key);
        std::map<uint32_t, size_t> groupOf;
        for (const net::interest::SendItem& si : im_->SendList(peer)) {
            auto it = ents_.find(si.entity);
            if (it == ents_.end()) continue;
            const Ent& e = it->second;
            if (!(e.peerMask & (1u << (peer & 31)))) continue;
            if (e.fast.flags & kDead) {
                double& last = know.deadSent[si.entity];
                if (last > 0 && now - last < t_.deadInterval) continue;
                last = now;
            }
            auto h = know.has.find(si.entity);
            if (h == know.has.end() || !(h->second == e.info)) {
                po.infos.push_back(e.info);
                know.has[si.entity] = e.info;
            }
            auto g = groupOf.find(e.timeMs);
            if (g == groupOf.end()) {
                g = groupOf.emplace(e.timeMs, po.states.size()).first;
                po.states.push_back({e.timeMs, {}});
            }
            po.states[g->second].npcs.push_back(e.fast);
            if (truthKeys.insert(si.entity).second)
                truth_.push_back({si.entity, e.timeMs, e.fast.x, e.fast.y, e.fast.z, e.fast.vx, e.fast.vy, e.fast.gait});
        }
        know.bytes += po.Bytes();
        out.push_back(std::move(po));
    }
    gone_.clear();
    return out;
}

// ---- receiving side ------------------------------------------------------------------------------------------------------
net::correction::Tuning NpcFix() {
    net::correction::Tuning t = net::correction::Tuning::ForNpc();
    t.deadZone = 0.3f;
    t.speedShare = 0.5f;
    return t;
}

net::motion::LocomotionTuning NpcLoco(const net::correction::Tuning& fix) {
    net::motion::LocomotionTuning l;
    l.nudgeStart = fix.deadZone;
    l.nudgeGain = fix.gain;
    l.nudgeSpeedShare = fix.speedShare;
    l.nudgeRestSpeed = fix.restSpeed;
    l.standSpeed = fix.standSpeed;
    // snaps: the Corrector's (far, height, stuck, teleport); the controller keeps only "blocked"
    l.snapDist = 1e9f;
    l.snapHeight = 1e9f;
    l.stuckTime = 1e9f;
    return l;
}

ReplicaTuning::ReplicaTuning() {
    loco = NpcLoco(fix);
    stream.track.maxExtrapolate = 0.5;  // a lost state of a slow track is bridged, then held
    stream.track.maxLead = 3.0f;        // m: a slow track is led further (its own interval, below)
}

ReplicaSet::ReplicaSet(const ReplicaTuning& t) : t_(t), loco_(t.loco), stream_(t.stream) {}

void ReplicaSet::Reset() {
    stream_.Reset();
    infos_.clear();
    byKey_.clear();
    tracks_.clear();
    reps_.clear();
    haveClock_ = false;
    clockMs_ = 0;
    lastNow_ = 0;
}

double ReplicaSet::SenderTime(uint32_t ms) {
    clockMs_ = haveClock_ ? clockMs_ + static_cast<int32_t>(ms - lastRawMs_) : ms;
    lastRawMs_ = ms;
    haveClock_ = true;
    return static_cast<double>(clockMs_) / 1000.0;
}

void ReplicaSet::OnStates(const StatesMsg& m, double now) {
    const double st = SenderTime(m.timeMs);
    stream_.Observe(st, now);
    for (const Fast& f : m.npcs) {
        net::replica::Sample s;
        s.t = st;
        s.pos = {f.x, f.y, f.z};
        s.vel = {f.vx, f.vy, 0};
        YawQuat(f.hx, f.hy, s.rot);
        s.hasRot = true;
        s.bits = static_cast<uint32_t>(f.flags) | (static_cast<uint32_t>(f.gait) << 8) |
                 (static_cast<uint32_t>(f.shots) << 16);
        if (!stream_.Push(f.net, s)) continue;
        Track& tr = tracks_[f.net];
        if (tr.lastT >= 0) {
            const double d = st - tr.lastT;
            if (d > 0 && d < 3.0) tr.interval = tr.interval <= 0 ? d : tr.interval + (d - tr.interval) * 0.3;
        }
        tr.lastT = st;
        tr.hp = f.hp;
        tr.flags = f.flags;
    }
}

void ReplicaSet::OnInfos(const std::vector<Info>& infos) {
    for (const Info& i : infos) {
        auto old = infos_.find(i.net);
        if (old != infos_.end() && old->second.key != i.key) {  // the net id stands for another NPC now
            byKey_.erase(old->second.key);
            reps_.erase(old->second.key);
            stream_.Remove(i.net);
            tracks_.erase(i.net);
        }
        auto prev = byKey_.find(i.key);
        if (prev != byKey_.end() && prev->second != i.net) {  // that NPC came back under another net id
            infos_.erase(prev->second);
            stream_.Remove(prev->second);
            tracks_.erase(prev->second);
        }
        infos_[i.net] = i;
        byKey_[i.key] = i.net;
    }
}

void ReplicaSet::OnLeaves(const std::vector<uint16_t>& nets) {
    for (uint16_t n : nets) {
        auto it = infos_.find(n);
        if (it != infos_.end()) {
            auto k = byKey_.find(it->second.key);
            if (k != byKey_.end() && k->second == n) byKey_.erase(k);
            infos_.erase(it);
        }
        stream_.Remove(n);
        tracks_.erase(n);
    }
}

std::vector<uint32_t> ReplicaSet::Update(double now) {
    const double dt = lastNow_ > 0 ? std::clamp(now - lastNow_, 0.0, 0.5) : 0.0;
    lastNow_ = now;
    stream_.Update(now);
    std::vector<uint32_t> keys;
    for (uint32_t n : stream_.Forget(now - t_.forget)) {
        tracks_.erase(static_cast<uint16_t>(n));
        auto it = infos_.find(static_cast<uint16_t>(n));
        if (it != infos_.end()) keys.push_back(it->second.key);
    }
    // a slow track (a far or idle NPC, 1-2 Hz) is shown further in the past by its own interval, so it is
    // interpolated like a near one instead of extrapolated and corrected at every state; the lead brings it to now
    const double streamIv = std::max(stream_.Clock().Interval(), 0.0);
    for (auto& [n, tr] : tracks_) {
        const double want = std::clamp(tr.interval - streamIv, 0.0, t_.maxExtra);
        const double step = t_.extraRate * dt;
        if (!tr.extraSet && tr.interval > 0) {  // the first interval of a new track: taken at once
            tr.extra = want;
            tr.extraSet = true;
        }
        tr.extra += std::clamp(want - tr.extra, -step, step);
    }
    // controllers of replicas no longer reported
    for (auto it = reps_.begin(); it != reps_.end();) {
        if (it->second.haveReport && now - it->second.report.at > 5.0)
            it = reps_.erase(it);
        else
            ++it;
    }
    return keys;
}

// Where the NPC is now: the state at the render time moved on by `lead` s along its velocity and, a share of it, its
// acceleration (from the velocities of the two samples around the render time, or the newest two), bounded: a start
// or a stop of a slow track (2 Hz) is followed instead of overshot / trailed by a whole interval. A decelerating NPC
// does not reverse. At most maxLead metres.
void ReplicaSet::Lead(const net::replica::SnapshotTrack& track, net::replica::Out& o, double lead) const {
    if (lead <= 0 || o.held) return;
    float ax = 0, ay = 0;
    if (t_.accelShare > 0 && track.Size() >= 2) {
        size_t i = track.Size() - 2;  // newest pair (extrapolating, or the render time is in the last interval)
        if (o.fromT < o.toT)
            for (size_t k = 0; k + 1 < track.Size(); ++k)
                if (track.Get(k).t == o.fromT) {
                    i = k;
                    break;
                }
        const net::replica::Sample& a = track.Get(i);
        const net::replica::Sample& b = track.Get(i + 1);
        const double dt = b.t - a.t;
        if (dt > 0.02 && dt < 1.5) {
            ax = static_cast<float>((b.vel.x - a.vel.x) / dt) * t_.accelShare;
            ay = static_cast<float>((b.vel.y - a.vel.y) / dt) * t_.accelShare;
            const float al = std::hypot(ax, ay);
            if (al > t_.maxAccel) {
                ax *= t_.maxAccel / al;
                ay *= t_.maxAccel / al;
            }
        }
    }
    const float L = static_cast<float>(lead);
    float dx = o.vel.x * L + 0.5f * ax * L * L, dy = o.vel.y * L + 0.5f * ay * L * L;
    float nvx = o.vel.x + ax * L, nvy = o.vel.y + ay * L;
    if (nvx * o.vel.x + nvy * o.vel.y < 0) {  // it stops within the lead: where it stops, standing
        const float v2 = o.vel.x * o.vel.x + o.vel.y * o.vel.y, a2 = ax * ax + ay * ay;
        const float tStop = a2 > 1e-6f ? std::sqrt(v2 / a2) : 0.0f;
        dx = o.vel.x * tStop + 0.5f * ax * tStop * tStop;
        dy = o.vel.y * tStop + 0.5f * ay * tStop * tStop;
        nvx = nvy = 0;
    }
    const float dz = o.vel.z * L;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz), cap = track.Tuning().maxLead;
    const float k = len > cap ? cap / len : 1.0f;
    o.pos.x += dx * k;
    o.pos.y += dy * k;
    o.pos.z += dz * k;
    o.vel.x = nvx;
    o.vel.y = nvy;
}

bool ReplicaSet::TargetOfNet(uint16_t net, const Track& tr, double now, Target& out) const {
    const net::replica::SnapshotTrack* track = stream_.Track(net);
    auto info = infos_.find(net);
    if (!track || track->Empty() || info == infos_.end()) return false;
    const double rt = stream_.RenderTime(now) - tr.extra;
    const double lead = std::max(0.0, stream_.Delay()) + tr.extra;
    net::replica::Out o = track->At(rt);
    if (!o.valid) return false;
    Lead(*track, o, lead);
    out.info = info->second;
    out.pos = o.pos;
    out.vel = o.vel;
    if (o.hasRot) QuatYaw(o.rot, out.hx, out.hy);
    out.flags = static_cast<uint8_t>(o.bits & 0xFF) | (tr.flags & kDead);  // a death shows at once
    out.gait = static_cast<uint8_t>((o.bits >> 8) & 0xFF);
    out.shots = static_cast<uint8_t>((o.bits >> 16) & 0xFF);
    out.hp = tr.hp;
    if (out.flags & kDead) {
        out.vel = {};
        out.gait = 0;
    }
    const double arrived = stream_.LastArrival(net);
    out.age = arrived >= 0 ? now - arrived : 1e9;
    out.renderMs = (rt + lead) * 1000.0;
    return true;
}

bool ReplicaSet::TargetOf(uint32_t key, double now, Target& out) const {
    auto k = byKey_.find(key);
    if (k == byKey_.end()) return false;
    auto tr = tracks_.find(k->second);
    if (tr == tracks_.end()) return false;
    return TargetOfNet(k->second, tr->second, now, out);
}

std::vector<Target> ReplicaSet::Targets(double now) const {
    std::vector<Target> out;
    for (const auto& [key, n] : byKey_) {
        auto tr = tracks_.find(n);
        if (tr == tracks_.end()) continue;
        Target t;
        if (TargetOfNet(n, tr->second, now, t)) out.push_back(std::move(t));
    }
    return out;
}

void ReplicaSet::OnReport(const Report& r) {
    if (!Finite3(r.pos.x, r.pos.y, r.pos.z)) return;
    auto it = reps_.try_emplace(r.key, loco_, t_.fix).first;
    Rep& rep = it->second;
    if (rep.haveReport && rep.kind != r.kind) {  // another body now (a copy instead of the paired NPC): start over
        rep.loco.Reset();
        rep.fix.Reset();
        rep.snapTimes.clear();
        rep.refused = false;
        rep.haveLast = false;
    }
    rep.kind = r.kind;
    rep.report = r;
    rep.haveReport = true;
}

std::vector<Drive> ReplicaSet::Commands(double now, float dt) {
    std::vector<Drive> out;
    for (auto& [key, rep] : reps_) {
        if (!rep.haveReport || now - rep.report.at > t_.reportFresh) continue;
        Target tg;
        if (!TargetOf(key, now, tg)) continue;
        if (rep.report.busy || (tg.flags & (kDead | kInCar))) {  // not walked or corrected now
            rep.loco.Reset();
            rep.fix.Reset();
            rep.haveLast = false;
            continue;
        }
        Drive d;
        d.key = key;
        d.kind = rep.kind;
        d.target = tg.pos;
        d.hx = tg.hx;
        d.hy = tg.hy;
        d.renderMs = tg.renderMs;
        d.senderGait = tg.gait;
        net::motion::LocomotionInput in;
        in.target = tg.pos;
        in.targetVel = tg.vel;
        in.heading[0] = tg.hx;
        in.heading[1] = tg.hy;
        in.gaitHint = t_.gaitHint ? static_cast<int8_t>(std::min<int>(tg.gait, 3)) : int8_t{-1};
        in.haveCopy = true;
        in.copy = rep.report.pos;
        in.copyHeading[0] = rep.report.hx;
        in.copyHeading[1] = rep.report.hy;
        in.dt = dt;
        const net::correction::Result r = rep.fix.Update(rep.report.pos, tg.pos, tg.vel, dt);
        bool snapped = false;
        if (r.kind == net::correction::Result::Kind::Snap) {
            rep.loco.Reset();
            in.copy = tg.pos;  // it stands there after the snap: the walk starts from there
            d.lc = rep.loco.Update(in);
            d.lc.fix = net::motion::LocomotionCommand::Fix::Snap;
            d.lc.error = r.error;
            d.why = r.why;
            snapped = r.why != net::correction::Result::Why::Teleport;  // a teleport is the authority's, not ours
        } else {
            d.lc = rep.loco.Update(in);
            if (d.lc.fix == net::motion::LocomotionCommand::Fix::Snap) {
                d.blocked = true;
                rep.fix.Reset();
                snapped = true;
            }
        }
        if (d.lc.fix == net::motion::LocomotionCommand::Fix::Snap) ++snaps_;
        if (snapped && rep.kind == 1 && !rep.refused) {
            rep.snapTimes.push_back(now);
            rep.snapTimes.erase(std::remove_if(rep.snapTimes.begin(), rep.snapTimes.end(),
                                               [&](double t) { return now - t > t_.refuseWindow; }),
                                rep.snapTimes.end());
            if (static_cast<int>(rep.snapTimes.size()) >= t_.refuseSnaps) {
                rep.refused = true;
                d.refuse = true;
                ++refusals_;
            }
        }
        rep.last = d;
        rep.haveLast = true;
        out.push_back(d);
    }
    return out;
}

bool ReplicaSet::Record(uint32_t key, ReplicaRecord& out) const {
    auto it = reps_.find(key);
    if (it == reps_.end() || !it->second.haveLast || !it->second.haveReport) return false;
    const Rep& rep = it->second;
    out.key = key;
    out.kind = rep.kind;
    out.renderMs = rep.last.renderMs;
    out.copy = rep.report.pos;
    out.target = rep.last.target;
    out.err = std::hypot(out.copy.x - out.target.x, out.copy.y - out.target.y);
    out.fix = static_cast<uint8_t>(rep.last.lc.fix);
    out.gait = static_cast<uint8_t>(rep.last.lc.gait);
    out.senderGait = rep.last.senderGait;
    out.why = rep.last.blocked ? "blocked" : net::correction::WhyName(rep.last.why);
    return true;
}

void ReplicaSet::Forget(uint32_t key) { reps_.erase(key); }

uint16_t ReplicaSet::NetOf(uint32_t key) const {
    auto it = byKey_.find(key);
    return it == byKey_.end() ? 0 : it->second;
}

const Info* ReplicaSet::InfoOf(uint32_t key) const {
    auto it = byKey_.find(key);
    if (it == byKey_.end()) return nullptr;
    auto i = infos_.find(it->second);
    return i == infos_.end() ? nullptr : &i->second;
}

const Info* ReplicaSet::InfoOfNet(uint16_t net) const {
    auto i = infos_.find(net);
    return i == infos_.end() ? nullptr : &i->second;
}

}  // namespace mc::game::npcrep
