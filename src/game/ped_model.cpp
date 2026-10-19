#include "game/ped_model.h"

#include "net/correction.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mc::game::ped {
namespace {

namespace motion = net::motion;

constexpr float kMaxOffset = 1023.0f;
constexpr float kPi = 3.14159265f;

bool Finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

int16_t Q16(float v, float scale) {
    const float q = std::round(v * scale);
    return static_cast<int16_t>(std::clamp(q, -32767.0f, 32767.0f));
}

float Ground(const Vec3& a, const Vec3& b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

void Normalise2(float h[2]) {
    const float l = std::hypot(h[0], h[1]);
    if (l > 1e-4f && std::isfinite(l)) {
        h[0] /= l;
        h[1] /= l;
    } else {
        h[0] = 0;
        h[1] = 1;
    }
}

bool Same(const PedState& a, const PedState& b) {
    return a.net == b.net && a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.pos.z == b.pos.z && a.vel.x == b.vel.x &&
           a.vel.y == b.vel.y && a.heading[0] == b.heading[0] && a.heading[1] == b.heading[1] && a.flags == b.flags &&
           a.gait == b.gait;
}

// heading <-> a yaw quaternion (replica::Sample carries rotations; the heading is interpolated as one)
void HeadingToQuat(const float h[2], float q[4]) {
    const float yaw = std::atan2(h[1], h[0]);
    q[0] = 0;
    q[1] = 0;
    q[2] = std::sin(yaw * 0.5f);
    q[3] = std::cos(yaw * 0.5f);
}

void QuatToHeading(const float q[4], float h[2]) {
    const float yaw = 2.0f * std::atan2(q[2], q[3]);
    h[0] = std::cos(yaw);
    h[1] = std::sin(yaw);
}

}  // namespace

// ---- wire --------------------------------------------------------------------------------------------------------------
std::vector<std::vector<uint8_t>> PackStates(uint32_t authMs, const std::vector<PedState>& states,
                                             size_t maxPerPacket) {
    std::vector<std::vector<uint8_t>> out;
    maxPerPacket = std::clamp<size_t>(maxPerPacket, 1, 255);
    net::ByteWriter w;
    size_t n = 0;
    Vec3 origin;
    auto flush = [&] {
        if (n == 0) return;
        std::vector<uint8_t> buf = w.Take();
        buf[kStatesHeader - 1] = static_cast<uint8_t>(n);
        out.push_back(std::move(buf));
        w.Clear();
        n = 0;
    };
    for (const PedState& s : states) {
        if (!Finite(s.pos) || !Finite(s.vel)) continue;
        const bool fits = n > 0 && std::fabs(s.pos.x - origin.x) < kMaxOffset &&
                          std::fabs(s.pos.y - origin.y) < kMaxOffset && std::fabs(s.pos.z - origin.z) < kMaxOffset;
        if (n > 0 && (!fits || n >= maxPerPacket)) flush();
        if (n == 0) {
            origin = s.pos;
            w.U32(authMs);
            w.F32(origin.x);
            w.F32(origin.y);
            w.F32(origin.z);
            w.U8(0);  // count, patched in flush
        }
        float h[2] = {s.heading[0], s.heading[1]};
        Normalise2(h);
        w.U16(s.net);
        w.I16(Q16(s.pos.x - origin.x, kPosScale));
        w.I16(Q16(s.pos.y - origin.y, kPosScale));
        w.I16(Q16(s.pos.z - origin.z, kPosScale));
        w.I8(motion::PackSnorm8(h[0]));
        w.I8(motion::PackSnorm8(h[1]));
        w.I16(Q16(s.vel.x, kVelScale));
        w.I16(Q16(s.vel.y, kVelScale));
        w.U8(s.flags);
        w.U8(s.gait);
        ++n;
    }
    flush();
    return out;
}

bool ReadStates(net::ByteReader& r, uint32_t& authMs, std::vector<PedState>& out) {
    out.clear();
    authMs = r.U32();
    Vec3 o;
    o.x = r.F32();
    o.y = r.F32();
    o.z = r.F32();
    const uint8_t n = r.U8();
    if (r.Failed() || !Finite(o)) return false;
    out.reserve(n);
    for (uint8_t i = 0; i < n; ++i) {
        PedState s;
        s.net = r.U16();
        s.pos.x = o.x + r.I16() / kPosScale;
        s.pos.y = o.y + r.I16() / kPosScale;
        s.pos.z = o.z + r.I16() / kPosScale;
        s.heading[0] = motion::UnpackSnorm8(r.I8());
        s.heading[1] = motion::UnpackSnorm8(r.I8());
        Normalise2(s.heading);
        s.vel.x = r.I16() / kVelScale;
        s.vel.y = r.I16() / kVelScale;
        s.flags = r.U8();
        s.gait = r.U8();
        if (r.Failed() || s.net == 0 || s.gait > 3) return false;
        out.push_back(s);
    }
    return true;
}

PedState Quantised(const PedState& s) {
    std::vector<PedState> one{s};
    const auto packets = PackStates(0, one, 1);
    if (packets.empty()) return s;
    net::ByteReader r(packets[0]);
    uint32_t ms = 0;
    std::vector<PedState> back;
    if (!ReadStates(r, ms, back) || back.empty()) return s;
    return back[0];
}

std::vector<uint8_t> PackSpawns(const std::vector<PedSpawnInfo>& spawns) {
    net::ByteWriter w;
    const size_t n = std::min(spawns.size(), kMaxSpawnsPerMessage);
    w.U8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
        const PedSpawnInfo& s = spawns[i];
        float h[2] = {s.st.heading[0], s.st.heading[1]};
        Normalise2(h);
        w.U16(s.net);
        w.U8(s.cls);
        w.U32(s.look);
        w.F32(s.st.pos.x);
        w.F32(s.st.pos.y);
        w.F32(s.st.pos.z);
        w.I8(motion::PackSnorm8(h[0]));
        w.I8(motion::PackSnorm8(h[1]));
        w.U8(s.st.flags);
    }
    return w.Take();
}

bool ReadSpawns(net::ByteReader& r, std::vector<PedSpawnInfo>& out) {
    out.clear();
    const uint8_t n = r.U8();
    if (r.Failed() || n > kMaxSpawnsPerMessage) return false;
    for (uint8_t i = 0; i < n; ++i) {
        PedSpawnInfo s;
        s.net = r.U16();
        s.cls = r.U8();
        s.look = r.U32();
        s.st.pos.x = r.F32();
        s.st.pos.y = r.F32();
        s.st.pos.z = r.F32();
        s.st.heading[0] = motion::UnpackSnorm8(r.I8());
        s.st.heading[1] = motion::UnpackSnorm8(r.I8());
        Normalise2(s.st.heading);
        s.st.flags = r.U8();
        s.st.net = s.net;
        if (r.Failed() || s.net == 0 || !Finite(s.st.pos)) return false;
        out.push_back(s);
    }
    return true;
}

std::vector<uint8_t> PackGone(const std::vector<uint16_t>& nets) {
    net::ByteWriter w;
    const size_t n = std::min<size_t>(nets.size(), 255);
    w.U8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) w.U16(nets[i]);
    return w.Take();
}

bool ReadGone(net::ByteReader& r, std::vector<uint16_t>& out) {
    out.clear();
    const uint8_t n = r.U8();
    for (uint8_t i = 0; i < n && !r.Failed(); ++i) out.push_back(r.U16());
    return !r.Failed();
}

std::vector<uint8_t> PackDeath(uint16_t net, const Vec3& pos) {
    net::ByteWriter w;
    w.U16(net);
    w.F32(pos.x);
    w.F32(pos.y);
    w.F32(pos.z);
    return w.Take();
}

bool ReadDeath(net::ByteReader& r, uint16_t& net, Vec3& pos) {
    net = r.U16();
    pos.x = r.F32();
    pos.y = r.F32();
    pos.z = r.F32();
    return !r.Failed() && net != 0 && Finite(pos);
}

std::vector<uint8_t> PackKill(uint16_t net) {
    net::ByteWriter w;
    w.U16(net);
    return w.Take();
}

bool ReadKill(net::ByteReader& r, uint16_t& net) {
    net = r.U16();
    return !r.Failed() && net != 0;
}

// ---- small rules -------------------------------------------------------------------------------------------------------
uint8_t GaitOf(float speed, uint8_t prev) {
    static const float kUp[3] = {0.5f, 2.4f, 4.8f};       // stand -> walk, walk -> run, run -> sprint
    static const float kDown[4] = {0, 0.3f, 2.0f, 4.3f};  // walk -> stand, run -> walk, sprint -> run
    if (!std::isfinite(speed)) return 0;
    uint8_t g = prev > 3 ? 0 : prev;
    while (g < 3 && speed >= kUp[g]) ++g;
    while (g > 0 && speed < kDown[g]) --g;
    return g;
}

bool InView(const Vec3& cam, const Vec3& dir, const Vec3& p, float halfAngleDeg, float nearDist, float farDist) {
    const float dx = p.x - cam.x, dy = p.y - cam.y, dz = p.z - cam.z;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d < nearDist) return true;
    if (d > farDist) return false;
    const float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dl < 1e-4f) return true;
    const float c = (dx * dir.x + dy * dir.y + dz * dir.z) / (d * dl);
    return c >= std::cos(halfAngleDeg * kPi / 180.0f);
}

net::motion::LocomotionTuning LocomotionForPed() {
    const net::correction::Tuning c = net::correction::Tuning::ForNpc();
    net::motion::LocomotionTuning t;
    t.nudgeStart = c.deadZone;
    t.nudgeGain = c.gain;
    t.nudgeSpeedShare = c.speedShare;
    t.nudgeRestSpeed = c.restSpeed;
    t.snapDist = c.snapDist;
    t.snapHeight = c.snapHeight;
    t.stuckDist = c.stuckDist;
    t.stuckTime = c.stuckTime;
    t.standSpeed = c.standSpeed;
    t.walkSpeed = 1.3f;  // a pedestrian's stroll (calibrated at run time from the copy's measured speed)
    return t;
}

std::vector<MatchPair> MatchNearest(const std::vector<Vec3>& remotes, const std::vector<bool>& remoteSeen,
                                    const std::vector<Vec3>& locals, const std::vector<bool>& localSeen, float nearDist,
                                    float farDist) {
    std::vector<MatchPair> cand;
    for (size_t i = 0; i < remotes.size(); ++i) {
        for (size_t j = 0; j < locals.size(); ++j) {
            const float d = Ground(remotes[i], locals[j]);
            if (!std::isfinite(d) || std::fabs(remotes[i].z - locals[j].z) > 4.0f) continue;
            const bool unseen = !(i < remoteSeen.size() && remoteSeen[i]) && !(j < localSeen.size() && localSeen[j]);
            if (d <= nearDist || (unseen && d <= farDist)) cand.push_back({i, j, d});
        }
    }
    std::sort(cand.begin(), cand.end(), [](const MatchPair& a, const MatchPair& b) {
        if (a.dist != b.dist) return a.dist < b.dist;
        if (a.remote != b.remote) return a.remote < b.remote;
        return a.local < b.local;
    });
    std::vector<bool> rUsed(remotes.size()), lUsed(locals.size());
    std::vector<MatchPair> out;
    for (const MatchPair& p : cand) {
        if (rUsed[p.remote] || lUsed[p.local]) continue;
        rUsed[p.remote] = lUsed[p.local] = true;
        out.push_back(p);
    }
    return out;
}

// ---- authority ---------------------------------------------------------------------------------------------------------
namespace {
net::interest::Config InterestConfig(const AuthorityConfig& cfg) {
    net::interest::Config c;
    auto& k = c.kinds[static_cast<int>(net::interest::Kind::Npc)];
    k.enterRadius = cfg.enterRadius;
    k.leaveRadius = cfg.leaveRadius;
    k.nearDistance = cfg.nearDistance;
    k.nearInterval = cfg.nearInterval;
    k.farInterval = cfg.farInterval;
    k.restSpeed = 0;
    k.maxInterval = 2.0;
    k.alwaysRelevant = false;
    c.bytesPerSecond = cfg.bytesPerSecond;
    c.sendToOwner = false;
    return c;
}
}  // namespace

PedAuthority::PedAuthority(const AuthorityConfig& cfg) : cfg_(cfg), im_(InterestConfig(cfg)), region_(cfg.region) {}

void PedAuthority::SetPlayers(const std::vector<PlayerView>& players, uint8_t host, uint8_t me) {
    me_ = me;
    host_ = host;
    std::vector<net::authority::PlayerSpot> spots;
    std::set<uint8_t> present;
    for (const PlayerView& p : players) {
        if (!Finite(p.pos)) continue;
        spots.push_back({p.id, p.pos, p.inWorld});
        if (p.id == me) continue;
        present.insert(p.id);
        auto [it, fresh] = peers_.try_emplace(p.id);
        if (fresh) im_.AddPeer(p.id);
        Peer& peer = it->second;
        if (p.inWorld) {
            peer.view = p.pos;
            peer.hasView = true;
            const net::interest::Vec3 v{p.pos.x, p.pos.y, p.pos.z};
            im_.SetPeerViewpoint(p.id, v);
        } else if (peer.hasView) {
            peer.hasView = false;
            im_.SetPeerViewpoints(p.id, {});
        }
    }
    region_.SetPlayers(spots, host);
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (!present.count(it->first)) {
            im_.RemovePeer(it->first);
            it = peers_.erase(it);
        } else {
            ++it;
        }
    }
}

void PedAuthority::RemovePeer(uint8_t peer) {
    im_.RemovePeer(peer);
    peers_.erase(peer);
}

uint16_t PedAuthority::NewNet() {
    for (int i = 0; i < 65536; ++i) {
        const uint16_t n = nextNet_++;
        if (nextNet_ == 0) nextNet_ = 1;
        if (n != 0 && !byNet_.count(n)) {
            bool admitted = false;
            for (const auto& [k, p] : peers_)
                if (p.admitted.count(n)) admitted = true;
            if (!admitted) return n;
        }
    }
    return 0;
}

void PedAuthority::SetPeds(const std::vector<LocalPed>& peds, double now) {
    std::set<uint32_t> seen;
    for (const LocalPed& lp : peds) {
        if (lp.id == 0 || !Finite(lp.pos) || seen.count(lp.id)) continue;
        seen.insert(lp.id);
        auto [it, fresh] = peds_.try_emplace(lp.id);
        Ped& e = it->second;
        // measured velocity (ground plane): smoothed over the rounds; a jump (respawn, teleport) resets it
        if (e.haveLast && now - e.lastAt > 1e-3) {
            const float dt = static_cast<float>(now - e.lastAt);
            Vec3 v{(lp.pos.x - e.lastPos.x) / dt, (lp.pos.y - e.lastPos.y) / dt, 0};
            if (Ground(lp.pos, e.lastPos) > 5.0f || dt > 1.0f || std::hypot(v.x, v.y) > 12.0f) {
                e.vel = {};
            } else {
                const float k = std::min(1.0f, dt / 0.12f);
                e.vel.x += (v.x - e.vel.x) * k;
                e.vel.y += (v.y - e.vel.y) * k;
            }
        }
        e.haveLast = true;
        e.lastPos = lp.pos;
        e.lastAt = now;
        e.p = lp;
        if (lp.flags & kDead) e.vel = {};
        e.gait = (lp.flags & kDead) ? 0 : GaitOf(std::hypot(e.vel.x, e.vel.y), e.gait);
        e.owner = (fresh || e.owner == net::kNoPlayer) ? region_.AuthorityAt(lp.pos) : region_.Keep(e.owner, lp.pos);
        const net::interest::Vec3 ip{lp.pos.x, lp.pos.y, lp.pos.z}, iv{e.vel.x, e.vel.y, 0};
        if (e.owner == me_) {
            if (e.net == 0) {
                e.net = NewNet();
                if (e.net != 0) {
                    byNet_[e.net] = lp.id;
                    net::interest::EntityDesc d;
                    d.id = e.net;
                    d.owner = me_;
                    d.kind = net::interest::Kind::Npc;
                    d.pos = ip;
                    d.vel = iv;
                    d.hasVelocity = true;
                    d.sizeBytes = static_cast<uint32_t>(kStateBytes);
                    im_.AddEntity(d);
                }
            } else {
                im_.UpdateEntity(e.net, ip, iv);
            }
        } else if (e.net != 0) {  // its region went to another player: not ours to send any more
            im_.RemoveEntity(e.net);
            byNet_.erase(e.net);
            e.net = 0;
        }
    }
    for (auto it = peds_.begin(); it != peds_.end();) {
        if (!seen.count(it->first)) {
            if (it->second.net != 0) {
                im_.RemoveEntity(it->second.net);
                byNet_.erase(it->second.net);
            }
            it = peds_.erase(it);
        } else {
            ++it;
        }
    }
}

PedState PedAuthority::StateOf(const Ped& p) const {
    PedState s;
    s.net = p.net;
    s.pos = p.p.pos;
    s.vel = p.vel;
    s.heading[0] = p.p.heading[0];
    s.heading[1] = p.p.heading[1];
    s.flags = static_cast<uint8_t>(p.p.flags & (kDead | kPanic | kCrouch));
    if (p.gait >= 2) s.flags |= kRunning;
    s.gait = p.gait;
    return s;
}

float PedAuthority::Dist(const Peer& peer, uint16_t net) const {
    auto it = byNet_.find(net);
    if (it == byNet_.end() || !peer.hasView) return 1e9f;
    return Ground(peds_.at(it->second).p.pos, peer.view);
}

void PedAuthority::Tick(double now, uint32_t authMs, std::vector<PeerOut>& out, std::vector<Truth>* truths) {
    out.clear();
    im_.Tick(now);
    for (const auto& e : im_.Events()) {
        auto it = peers_.find(static_cast<uint8_t>(e.peer));
        if (it == peers_.end()) continue;
        const auto net = static_cast<uint16_t>(e.entity);
        if (e.type == net::interest::EventType::Enter)
            it->second.relevant.insert(net);
        else
            it->second.relevant.erase(net);
    }
    std::set<uint16_t> truthDone;
    for (auto& [key, p] : peers_) {
        PeerOut po;
        po.peer = key;
        std::vector<uint16_t> gone;
        for (auto it = p.admitted.begin(); it != p.admitted.end();) {
            if (!p.relevant.count(it->first) || !byNet_.count(it->first)) {
                gone.push_back(it->first);
                it = p.admitted.erase(it);
            } else {
                ++it;
            }
        }
        // admission: the nearest relevant pedestrians, at most maxPerPeer
        std::vector<std::pair<float, uint16_t>> cand;
        for (uint16_t net : p.relevant)
            if (!p.admitted.count(net) && byNet_.count(net)) cand.push_back({Dist(p, net), net});
        std::sort(cand.begin(), cand.end());
        std::vector<uint16_t> added;
        for (size_t ci = 0; ci < cand.size() && p.admitted.size() < cfg_.maxPerPeer; ++ci) {
            p.admitted[cand[ci].second] = Sent{};
            added.push_back(cand[ci].second);
        }
        for (size_t at = 0; at < gone.size(); at += 255) {
            std::vector<uint16_t> part(gone.begin() + static_cast<std::ptrdiff_t>(at),
                                       gone.begin() + static_cast<std::ptrdiff_t>(std::min(at + 255, gone.size())));
            po.gones.push_back(PackGone(part));
        }
        po.gone = static_cast<uint32_t>(gone.size());
        std::vector<PedSpawnInfo> spawns;
        for (uint16_t net : added) {
            const Ped& e = peds_.at(byNet_.at(net));
            PedSpawnInfo s;
            s.net = net;
            s.look = e.p.look;
            s.st = StateOf(e);
            spawns.push_back(s);
        }
        for (size_t at = 0; at < spawns.size(); at += kMaxSpawnsPerMessage) {
            std::vector<PedSpawnInfo> part(
                spawns.begin() + static_cast<std::ptrdiff_t>(at),
                spawns.begin() + static_cast<std::ptrdiff_t>(std::min(at + kMaxSpawnsPerMessage, spawns.size())));
            po.spawns.push_back(PackSpawns(part));
            im_.ChargeBytes(key, static_cast<uint32_t>(po.spawns.back().size()));
        }
        po.spawned = static_cast<uint32_t>(spawns.size());
        // deaths: once per admitted pedestrian that is dead (a receiver admitted later gets the dead flag in its state)
        for (auto& [net, sent] : p.admitted) {
            const Ped& e = peds_.at(byNet_.at(net));
            const bool dead = (e.p.flags & kDead) != 0;
            if (dead && !(sent.q.flags & kDead) && sent.at > -1e8) {
                po.deaths.push_back(PackDeath(net, e.p.pos));
                ++p.stats.deaths;
            }
        }
        // states: the interest manager's due list, admitted ones only, the new ones always, unchanged ones now and then
        std::vector<PedState> states;
        std::set<uint16_t> due;
        for (const auto& item : im_.SendList(key)) due.insert(static_cast<uint16_t>(item.entity));
        for (auto& [net, sent] : p.admitted) {
            const bool fresh = std::find(added.begin(), added.end(), net) != added.end();
            const Ped& e = peds_.at(byNet_.at(net));
            const bool deadNow = (e.p.flags & kDead) && !(sent.q.flags & kDead);
            if (!fresh && !deadNow && !due.count(net)) continue;
            const PedState st = StateOf(e);
            const PedState q = Quantised(st);
            if (!fresh && !deadNow && Same(q, sent.q) && now - sent.at < cfg_.refresh) {
                ++po.skipped;
                continue;
            }
            sent = Sent{q, now};
            states.push_back(st);
            if (truths && truthDone.insert(net).second) truths->push_back({net, e.p.id, e.p.pos});
        }
        po.states = PackStates(authMs, states, cfg_.maxPerPacket);
        po.sent = static_cast<uint32_t>(states.size());
        p.stats.relevant = static_cast<uint32_t>(p.relevant.size());
        p.stats.admitted = static_cast<uint32_t>(p.admitted.size());
        p.stats.spawned += po.spawned;
        p.stats.gone += po.gone;
        p.stats.states += po.sent;
        p.stats.skipped += po.skipped;
        for (const auto* list : {&po.spawns, &po.gones, &po.deaths, &po.states})
            for (const auto& b : *list) p.stats.bytes += b.size();
        if (!po.spawns.empty() || !po.gones.empty() || !po.states.empty() || !po.deaths.empty() || po.skipped)
            out.push_back(std::move(po));
    }
}

uint8_t PedAuthority::OwnerOf(uint32_t id) const {
    auto it = peds_.find(id);
    return it == peds_.end() ? net::kNoPlayer : it->second.owner;
}

std::vector<LocalPed> PedAuthority::Foreign() const {
    std::vector<LocalPed> out;
    for (const auto& [id, e] : peds_)
        if (e.owner != me_ && e.owner != net::kNoPlayer && !(e.p.flags & kDead)) out.push_back(e.p);
    return out;
}

size_t PedAuthority::OwnedCount() const {
    return byNet_.size();
}

uint32_t PedAuthority::IdOf(uint16_t net) const {
    auto it = byNet_.find(net);
    return it == byNet_.end() ? 0 : it->second;
}

uint16_t PedAuthority::NetOf(uint32_t id) const {
    auto it = peds_.find(id);
    return it == peds_.end() ? 0 : it->second.net;
}

const AuthorityPeerStats* PedAuthority::Stats(uint8_t peer) const {
    auto it = peers_.find(peer);
    return it == peers_.end() ? nullptr : &it->second.stats;
}

// ---- receiver ----------------------------------------------------------------------------------------------------------
PedReceiver::PedReceiver(const ReceiverConfig& cfg) : cfg_(cfg) {}

PedReceiver::Auth& PedReceiver::AuthOf(uint8_t auth) {
    auto it = auths_.find(auth);
    if (it == auths_.end()) it = auths_.emplace(auth, Auth{net::replica::Stream(cfg_.stream)}).first;
    return it->second;
}

void PedReceiver::OnSpawn(uint8_t auth, const PedSpawnInfo& s, double now) {
    const uint32_t key = Key(auth, s.net);
    auto [it, fresh] = remotes_.try_emplace(key);
    Remote& r = it->second;
    if (fresh) {
        r.auth = auth;
        r.net = s.net;
        r.firstSeen = now;
    }
    r.info = s;
    r.haveInfo = true;
    r.lastRecv = now;
    r.goneAt = -1;
    if (s.st.flags & kDead) r.dead = true;
    ++stats_.spawns;
}

void PedReceiver::OnGone(uint8_t auth, uint16_t net, double now) {
    auto it = remotes_.find(Key(auth, net));
    if (it == remotes_.end()) return;
    if (it->second.goneAt < 0) it->second.goneAt = now;
    ++stats_.gones;
}

void PedReceiver::OnStates(uint8_t auth, uint32_t authMs, const std::vector<PedState>& states, double now) {
    Auth& a = AuthOf(auth);
    if (!a.haveClock) {
        a.clockMs = authMs;
        a.haveClock = true;
    } else {
        a.clockMs += static_cast<int32_t>(authMs - a.lastRawMs);
    }
    a.lastRawMs = authMs;
    const double t = static_cast<double>(a.clockMs) / 1000.0;
    a.stream.Observe(t, now);
    for (const PedState& s : states) {
        const uint32_t key = Key(auth, s.net);
        auto [it, fresh] = remotes_.try_emplace(key);
        Remote& r = it->second;
        if (fresh) {
            r.auth = auth;
            r.net = s.net;
            r.firstSeen = now;
        }
        net::replica::Sample smp;
        smp.t = t;
        smp.pos = s.pos;
        smp.vel = {s.vel.x, s.vel.y, 0};
        HeadingToQuat(s.heading, smp.rot);
        smp.hasRot = true;
        smp.bits = static_cast<uint32_t>(s.flags) | (static_cast<uint32_t>(s.gait) << 8);
        if (!a.stream.Push(s.net, smp)) {
            ++stats_.dropsOld;
            continue;
        }
        r.lastRecv = now;
        r.goneAt = -1;
        if (s.flags & kDead) r.dead = true;
        ++stats_.states;
    }
}

void PedReceiver::OnDeath(uint8_t auth, uint16_t net, const Vec3& pos, double now) {
    const uint32_t key = Key(auth, net);
    auto [it, fresh] = remotes_.try_emplace(key);
    Remote& r = it->second;
    if (fresh) {
        r.auth = auth;
        r.net = net;
        r.firstSeen = now;
        r.lastRecv = now;
        r.info.st.pos = pos;
    }
    r.dead = true;
    ++stats_.deaths;
}

void PedReceiver::ForgetAuthority(uint8_t auth) {
    for (auto& [key, r] : remotes_) {
        if (r.auth != auth) continue;
        r.lastRecv = -1e9;
        if (r.goneAt < 0) r.goneAt = -1e9;  // released at the next tick
    }
    auto it = auths_.find(auth);
    if (it != auths_.end()) {
        it->second.stream.Reset();
        it->second.haveClock = false;
    }
}

void PedReceiver::SetLocal(const Vec3& me, const Camera& cam) {
    me_ = me;
    haveMe_ = Finite(me);
    cam_ = cam;
}

void PedReceiver::SetRegion(const net::authority::RegionAuthority& region, uint8_t me) {
    region_ = region;
    haveRegion_ = true;
    myId_ = me;
}

void PedReceiver::SetCandidates(const std::vector<LocalPed>& foreign) {
    cands_.clear();
    for (const LocalPed& p : foreign)
        if (!boundLocal_.count(p.id) && !hidden_.count(p.id) && Finite(p.pos)) cands_[p.id] = p;
}

void PedReceiver::SetReport(uint32_t key, const CopyReport& rep, double now) {
    auto it = remotes_.find(key);
    if (it == remotes_.end()) return;
    Remote& r = it->second;
    r.rep = rep;
    r.repAt = now;
    r.haveRep = true;
    if (rep.dead && !r.dead && r.how != How::None) r.diedHere = true;
}

void PedReceiver::Spawned(uint32_t key, uint32_t local) {
    auto it = remotes_.find(key);
    if (it == remotes_.end() || local == 0) return;
    Remote& r = it->second;
    r.loading = false;
    r.how = How::Spawned;
    r.local = local;
    r.haveRep = false;
    r.loco.Reset();
    boundLocal_[local] = key;
}

void PedReceiver::SpawnFailed(uint32_t key, double now) {
    auto it = remotes_.find(key);
    if (it == remotes_.end()) return;
    Remote& r = it->second;
    if (r.how == How::Spawned && r.local != 0) return;
    r.loading = false;
    r.how = How::None;
    r.retryAt = now + cfg_.spawnRetry;
    ++stats_.spawnFails;
}

void PedReceiver::LocalGone(uint32_t local) {
    hidden_.erase(local);
    cands_.erase(local);
    auto it = boundLocal_.find(local);
    if (it == boundLocal_.end()) return;
    auto rt = remotes_.find(it->second);
    if (rt != remotes_.end()) {
        rt->second.how = How::None;
        rt->second.local = 0;
        rt->second.haveRep = false;
        rt->second.diedHere = false;
    }
    boundLocal_.erase(it);
}

bool PedReceiver::DiedHere(uint32_t key) {
    auto it = remotes_.find(key);
    if (it == remotes_.end()) return false;
    Remote& r = it->second;
    if (!r.diedHere || r.diedHereTold || r.dead) return false;
    r.diedHereTold = true;
    return true;
}

void PedReceiver::Clear() {
    remotes_.clear();
    auths_.clear();
    cands_.clear();
    boundLocal_.clear();
    hidden_.clear();
}

void PedReceiver::ShowAll(std::vector<RecvCmd>& out) {
    for (const auto& [local, h] : hidden_) {
        RecvCmd c;
        c.type = RecvCmd::Type::Show;
        c.local = local;
        out.push_back(c);
        ++stats_.shows;
    }
    hidden_.clear();
}

double PedReceiver::PlayoutDelay(uint8_t auth) const {
    auto it = auths_.find(auth);
    return it == auths_.end() ? 0 : it->second.stream.Delay();
}

bool PedReceiver::Target(const Remote& r, double now, PedState& out, double* t) const {
    auto it = auths_.find(r.auth);
    if (it == auths_.end()) return false;
    const net::replica::Out o = it->second.stream.Present(r.net, now);
    if (!o.valid) return false;
    out.net = r.net;
    out.pos = o.pos;
    out.vel = {o.vel.x, o.vel.y, 0};
    if (o.hasRot) QuatToHeading(o.rot, out.heading);
    out.flags = static_cast<uint8_t>(o.bits & 0xFF);
    out.gait = static_cast<uint8_t>((o.bits >> 8) & 0xFF);
    if (r.dead) out.flags |= kDead;
    if (t) *t = it->second.stream.RenderTime(now) + it->second.stream.Delay();
    return true;
}

bool PedReceiver::Seen(const Vec3& p) const {
    if (!cam_.valid) return DistToMe(p) < 60.0f;
    return InView(cam_.pos, cam_.dir, p, cfg_.viewHalfAngle);
}

float PedReceiver::DistToMe(const Vec3& p) const {
    return haveMe_ ? Ground(p, me_) : 1e9f;
}

void PedReceiver::ReleaseReplica(Remote& r, std::vector<RecvCmd>& out) {
    if (r.how == How::None && !r.loading) return;
    RecvCmd c;
    c.type = RecvCmd::Type::Release;
    c.key = Key(r.auth, r.net);
    c.local = r.local;
    c.how = r.loading ? How::Spawned : r.how;
    out.push_back(c);
    if (r.local) boundLocal_.erase(r.local);
    r.how = How::None;
    r.local = 0;
    r.loading = false;
    r.haveRep = false;
    r.diedHere = r.diedHereTold = false;
    ++stats_.releases;
}

void PedReceiver::Tick(double now, float dt, std::vector<RecvCmd>& out) {
    for (auto& [id, a] : auths_) a.stream.Update(now);
    // 1. gone / stale pedestrians: their replicas go (a spawned copy when nobody looks, at the latest after a grace)
    for (auto it = remotes_.begin(); it != remotes_.end();) {
        Remote& r = it->second;
        const bool stale = now - r.lastRecv > cfg_.stale;
        if (stale && r.goneAt < 0) r.goneAt = now;
        if (r.goneAt >= 0 || stale) {
            if (r.how == How::Adopted || r.loading || (r.how == How::Spawned && r.local == 0)) {
                ReleaseReplica(r, out);
            } else if (r.how == How::Spawned) {
                const bool unseen = !r.haveRep || !Seen(r.rep.pos);
                if (unseen || now - r.goneAt > cfg_.unspawnGrace) ReleaseReplica(r, out);
            }
            if (r.how == How::None && (now - r.lastRecv > cfg_.forget || r.goneAt < -1e8 ||
                                       (r.goneAt >= 0 && now - r.goneAt > cfg_.forget))) {
                it = remotes_.erase(it);
                continue;
            }
        }
        // a spawn that never came back
        if (r.loading && now - r.spawnAskedAt > 20.0) {
            r.loading = false;
            r.retryAt = now + cfg_.spawnRetry;
            ++stats_.spawnFails;
        }
        ++it;
    }
    // 2. targets of the live ones
    struct Live {
        Remote* r;
        PedState t;
        double tT;
        float d;
    };
    std::vector<Live> live;
    for (auto& [key, r] : remotes_) {
        if (r.goneAt >= 0) continue;
        PedState t;
        double tT = 0;
        if (!Target(r, now, t, &tT)) continue;
        r.lastTarget = t;
        r.lastTargetT = tT;
        live.push_back({&r, t, tT, DistToMe(t.pos)});
    }
    // 3. adoption: unmatched living authority pedestrians in our zone <-> free local humans there, nearest first
    std::vector<size_t> unmatched;
    for (size_t i = 0; i < live.size(); ++i) {
        Remote& r = *live[i].r;
        if (r.how != How::None || r.loading || r.dead || live[i].d > cfg_.zoneRadius) {
            if (r.how != How::None || r.loading) r.unmatchedSince = -1;
            continue;
        }
        if (r.unmatchedSince < 0) r.unmatchedSince = now;
        unmatched.push_back(i);
    }
    std::vector<Vec3> rPos, lPos;
    std::vector<bool> rSeen, lSeen;
    std::vector<uint32_t> lIds;
    for (size_t i : unmatched) {
        rPos.push_back(live[i].t.pos);
        rSeen.push_back(Seen(live[i].t.pos));
    }
    for (const auto& [id, p] : cands_) {
        if (DistToMe(p.pos) > cfg_.zoneRadius) continue;
        lIds.push_back(id);
        lPos.push_back(p.pos);
        lSeen.push_back(Seen(p.pos));
    }
    std::set<size_t> adoptedIdx;
    for (const MatchPair& m : MatchNearest(rPos, rSeen, lPos, lSeen, cfg_.adoptNear, cfg_.adoptFar)) {
        Live& l = live[unmatched[m.remote]];
        Remote& r = *l.r;
        const uint32_t local = lIds[m.local];
        r.how = How::Adopted;
        r.local = local;
        r.haveRep = false;
        r.unmatchedSince = -1;
        r.loco.Reset();
        boundLocal_[local] = Key(r.auth, r.net);
        cands_.erase(local);
        RecvCmd c;
        c.type = RecvCmd::Type::Adopt;
        c.key = Key(r.auth, r.net);
        c.local = local;
        c.target = l.t;
        out.push_back(c);
        ++stats_.adoptions;
        adoptedIdx.insert(m.remote);
    }
    // 4. a shortfall: spawned copies (nearest first, a while unmatched, more patience when its spot is in sight)
    size_t spawned = 0, loading = 0;
    for (const auto& [key, r] : remotes_) {
        if (r.loading) ++loading;
        if (r.loading || r.how == How::Spawned) ++spawned;
    }
    std::vector<std::pair<float, size_t>> want;
    for (size_t k = 0; k < unmatched.size(); ++k) {
        if (adoptedIdx.count(k)) continue;
        const Live& l = live[unmatched[k]];
        const Remote& r = *l.r;
        if (l.d > cfg_.spawnRadius || now < r.retryAt) continue;
        const double wait = rSeen[k] ? cfg_.spawnWaitSeen : cfg_.spawnWait;
        if (now - r.unmatchedSince < wait) continue;
        want.push_back({l.d, unmatched[k]});
    }
    std::sort(want.begin(), want.end());
    for (const auto& [d, i] : want) {
        if (spawned >= cfg_.maxSpawned || loading >= cfg_.maxLoading) break;
        Remote& r = *live[i].r;
        r.loading = true;
        r.spawnAskedAt = now;
        RecvCmd c;
        c.type = RecvCmd::Type::Spawn;
        c.key = Key(r.auth, r.net);
        c.target = live[i].t;
        out.push_back(c);
        ++spawned;
        ++loading;
        ++stats_.spawnsAsked;
    }
    // 5. surplus local humans in our zone (another's region) are hidden while nobody looks - only while that
    //    authority's pedestrians really arrive here
    if (!live.empty()) {
        for (auto it = cands_.begin(); it != cands_.end();) {
            const LocalPed& p = it->second;
            if (DistToMe(p.pos) <= cfg_.zoneRadius && !Seen(p.pos)) {
                RecvCmd c;
                c.type = RecvCmd::Type::Hide;
                c.local = p.id;
                out.push_back(c);
                hidden_[p.id] = Hidden{p.pos, -1};
                ++stats_.hides;
                it = cands_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // 6. hidden humans come back where the region is ours again (a while, out of sight), or far from us
    for (auto it = hidden_.begin(); it != hidden_.end();) {
        Hidden& h = it->second;
        bool show = DistToMe(h.pos) > cfg_.showFar;
        if (!show && haveRegion_) {
            if (region_.AuthorityAt(h.pos) == myId_) {
                if (h.oursSince < 0) h.oursSince = now;
                show = now - h.oursSince >= cfg_.showAfter && !Seen(h.pos);
            } else {
                h.oursSince = -1;
            }
        }
        if (show) {
            RecvCmd c;
            c.type = RecvCmd::Type::Show;
            c.local = it->first;
            out.push_back(c);
            ++stats_.shows;
            it = hidden_.erase(it);
        } else {
            ++it;
        }
    }
    // 7. drive the replicas (and deaths)
    for (Live& l : live) {
        Remote& r = *l.r;
        if (r.how == How::None || r.local == 0) continue;
        const uint32_t key = Key(r.auth, r.net);
        if (r.dead) {
            if (!r.killed && !(r.haveRep && r.rep.dead)) {
                RecvCmd c;
                c.type = RecvCmd::Type::Kill;
                c.key = key;
                c.local = r.local;
                c.target = l.t;
                out.push_back(c);
                ++stats_.kills;
            }
            r.killed = true;
            continue;
        }
        if (r.diedHere) continue;  // ours died here: the authority is told (PedKill), its death follows
        net::motion::LocomotionInput in;
        in.target = l.t.pos;
        in.targetVel = l.t.vel;
        in.heading[0] = l.t.heading[0];
        in.heading[1] = l.t.heading[1];
        in.sprint = l.t.gait >= 3;
        in.gaitHint = static_cast<int8_t>(l.t.gait);
        in.haveCopy = r.haveRep && now - r.repAt <= cfg_.reportFresh;
        in.copy = r.rep.pos;
        in.copyHeading[0] = r.rep.heading[0];
        in.copyHeading[1] = r.rep.heading[1];
        in.dt = dt;
        RecvCmd c;
        c.type = RecvCmd::Type::Drive;
        c.key = key;
        c.local = r.local;
        c.target = l.t;
        c.lc = r.loco.Update(in);
        if (c.lc.fix == net::motion::LocomotionCommand::Fix::Snap) ++stats_.snaps;
        r.lastErr = c.lc.error;
        out.push_back(c);
    }
    // 8. stats
    ReceiverStats& st = stats_;
    st.known = static_cast<uint32_t>(live.size());
    st.nearby = st.nearReplica = st.adopted = st.spawned = st.loading = 0;
    for (const Live& l : live) {
        const Remote& r = *l.r;
        if (!r.dead && l.d <= cfg_.coverRadius) {
            ++st.nearby;
            if (r.how != How::None && r.local != 0) ++st.nearReplica;
        }
    }
    for (const auto& [key, r] : remotes_) {
        if (r.how == How::Adopted) ++st.adopted;
        if (r.how == How::Spawned && r.local != 0) ++st.spawned;
        if (r.loading) ++st.loading;
    }
    st.hidden = static_cast<uint32_t>(hidden_.size());
}

std::vector<ReplicaInfo> PedReceiver::Replicas(double now) const {
    std::vector<ReplicaInfo> out;
    for (const auto& [key, r] : remotes_) {
        if (r.how == How::None || r.local == 0 || r.goneAt >= 0) continue;
        ReplicaInfo i;
        i.key = key;
        i.how = r.how;
        i.local = r.local;
        i.target = r.lastTarget;
        i.targetT = r.lastTargetT;
        i.copy = r.rep;
        i.haveCopy = r.haveRep && now - r.repAt <= 1.0;
        i.err = i.haveCopy ? Ground(r.rep.pos, r.lastTarget.pos) : 0;
        out.push_back(i);
    }
    return out;
}

}  // namespace mc::game::ped
