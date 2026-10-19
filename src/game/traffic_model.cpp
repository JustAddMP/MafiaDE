#include "game/traffic_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mc::game::traffic {
namespace {

namespace motion = net::motion;

constexpr float kSqrt2 = 1.41421356237f;
constexpr float kMaxOffset = 32767.0f / kPosScale;  // m

float Dist(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float Dist2D(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

int16_t Q16(float v, float scale) {
    if (!std::isfinite(v)) return 0;
    return static_cast<int16_t>(std::clamp(std::lround(v * scale), -32767l, 32767l));
}

void WriteVel(net::ByteWriter& w, const Vec3& v) {
    w.I16(Q16(v.x, kVelScale));
    w.I16(Q16(v.y, kVelScale));
    w.I16(Q16(v.z, kVelScale));
}

Vec3 ReadVel(net::ByteReader& r) {
    Vec3 v;
    v.x = r.I16() / kVelScale;
    v.y = r.I16() / kVelScale;
    v.z = r.I16() / kVelScale;
    return v;
}

// Quantised fields equal: nothing new to send.
bool Same(const CarState& a, const CarState& b) {
    return a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.pos.z == b.pos.z && a.vel.x == b.vel.x &&
           a.vel.y == b.vel.y && a.vel.z == b.vel.z && PackQuat(a.rot) == PackQuat(b.rot) &&
           motion::PackSnorm8(a.steer) == motion::PackSnorm8(b.steer) &&
           motion::PackUnorm8(a.power) == motion::PackUnorm8(b.power) &&
           motion::PackUnorm8(a.brake) == motion::PackUnorm8(b.brake) && a.flags == b.flags;
}

}  // namespace

// ---- wire -------------------------------------------------------------------------------------------------------
uint32_t PackQuat(const float qin[4]) {
    float q[4];
    float len = 0;
    for (int i = 0; i < 4; ++i) {
        q[i] = std::isfinite(qin[i]) ? qin[i] : 0;
        len += q[i] * q[i];
    }
    len = std::sqrt(len);
    if (len < 1e-6f) {
        q[0] = q[1] = q[2] = 0;
        q[3] = 1;
    } else {
        for (float& v : q) v /= len;
    }
    int big = 0;
    for (int i = 1; i < 4; ++i)
        if (std::fabs(q[i]) > std::fabs(q[big])) big = i;
    const float sign = q[big] < 0 ? -1.0f : 1.0f;
    uint32_t out = static_cast<uint32_t>(big) << 30;
    int shift = 20;
    for (int i = 0; i < 4; ++i) {
        if (i == big) continue;
        const float v = std::clamp(q[i] * sign * kSqrt2, -1.0f, 1.0f);  // [-1/sqrt2, 1/sqrt2] -> [-1, 1]
        const auto u = static_cast<uint32_t>(std::lround((v + 1.0f) * 0.5f * 1023.0f));
        out |= (u & 0x3FFu) << shift;
        shift -= 10;
    }
    return out;
}

void UnpackQuat(uint32_t v, float q[4]) {
    const int big = static_cast<int>(v >> 30);
    int shift = 20;
    float sum = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == big) continue;
        const float u = static_cast<float>((v >> shift) & 0x3FFu) / 1023.0f;
        q[i] = (u * 2.0f - 1.0f) / kSqrt2;
        sum += q[i] * q[i];
        shift -= 10;
    }
    q[big] = std::sqrt(std::max(0.0f, 1.0f - sum));
}

bool SafeToken(const std::string& s, size_t max) {
    if (s.empty() || s.size() > max) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}

CarState Quantised(const CarState& s) {
    CarState q = s;
    auto qp = [](float v) { return std::isfinite(v) ? std::round(v * kPosScale) / kPosScale : 0.0f; };
    q.pos = {qp(s.pos.x), qp(s.pos.y), qp(s.pos.z)};
    q.vel = {Q16(s.vel.x, kVelScale) / kVelScale, Q16(s.vel.y, kVelScale) / kVelScale,
             Q16(s.vel.z, kVelScale) / kVelScale};
    UnpackQuat(PackQuat(s.rot), q.rot);
    q.steer = motion::UnpackSnorm8(motion::PackSnorm8(s.steer));
    q.power = motion::UnpackUnorm8(motion::PackUnorm8(s.power));
    q.brake = motion::UnpackUnorm8(motion::PackUnorm8(s.brake));
    return q;
}

std::vector<std::vector<uint8_t>> PackStates(uint32_t hostMs, const std::vector<CarState>& states,
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
    for (const CarState& s : states) {
        if (!Finite(s.pos)) continue;
        const bool fits = n > 0 && std::fabs(s.pos.x - origin.x) < kMaxOffset &&
                          std::fabs(s.pos.y - origin.y) < kMaxOffset && std::fabs(s.pos.z - origin.z) < kMaxOffset;
        if (n > 0 && (!fits || n >= maxPerPacket)) flush();
        if (n == 0) {
            origin = s.pos;
            w.U32(hostMs);
            w.F32(origin.x);
            w.F32(origin.y);
            w.F32(origin.z);
            w.U8(0);  // count, patched in flush
        }
        w.U16(s.net);
        w.I16(Q16(s.pos.x - origin.x, kPosScale));
        w.I16(Q16(s.pos.y - origin.y, kPosScale));
        w.I16(Q16(s.pos.z - origin.z, kPosScale));
        WriteVel(w, s.vel);
        w.U32(PackQuat(s.rot));
        w.I8(motion::PackSnorm8(s.steer));
        w.U8(motion::PackUnorm8(s.power));
        w.U8(motion::PackUnorm8(s.brake));
        w.U8(s.flags);
        ++n;
    }
    flush();
    return out;
}

bool ReadStates(net::ByteReader& r, uint32_t& hostMs, std::vector<CarState>& out) {
    out.clear();
    hostMs = r.U32();
    Vec3 o;
    o.x = r.F32();
    o.y = r.F32();
    o.z = r.F32();
    const uint8_t n = r.U8();
    if (r.Failed() || !Finite(o) || n > kMaxPerPacket * 2) return false;
    out.reserve(n);
    for (uint8_t i = 0; i < n; ++i) {
        CarState s;
        s.net = r.U16();
        s.pos.x = o.x + r.I16() / kPosScale;
        s.pos.y = o.y + r.I16() / kPosScale;
        s.pos.z = o.z + r.I16() / kPosScale;
        s.vel = ReadVel(r);
        UnpackQuat(r.U32(), s.rot);
        s.steer = motion::UnpackSnorm8(r.I8());
        s.power = motion::UnpackUnorm8(r.U8());
        s.brake = motion::UnpackUnorm8(r.U8());
        s.flags = r.U8();
        if (r.Failed() || s.net == 0) return false;
        out.push_back(s);
    }
    return true;
}

std::vector<uint8_t> PackSpawns(const std::vector<SpawnInfo>& spawns) {
    net::ByteWriter w;
    const size_t n = std::min(spawns.size(), kMaxSpawnsPerMessage);
    w.U8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
        const SpawnInfo& s = spawns[i];
        w.U16(s.net);
        w.U8(s.kind);
        w.Str(SafeToken(s.model, 64) ? s.model : "-");
        w.I16(s.color[0]);
        w.I16(s.color[1]);
        w.Str(SafeToken(s.plate, 16) ? s.plate : "");
        w.F32(s.st.pos.x);
        w.F32(s.st.pos.y);
        w.F32(s.st.pos.z);
        w.U32(PackQuat(s.st.rot));
        WriteVel(w, s.st.vel);
        w.U8(s.st.flags);
    }
    return w.Take();
}

bool ReadSpawns(net::ByteReader& r, std::vector<SpawnInfo>& out) {
    out.clear();
    const uint8_t n = r.U8();
    if (r.Failed() || n > kMaxSpawnsPerMessage) return false;
    for (uint8_t i = 0; i < n; ++i) {
        SpawnInfo s;
        s.net = r.U16();
        s.kind = r.U8();
        s.model = r.Str(64);
        s.color[0] = r.I16();
        s.color[1] = r.I16();
        s.plate = r.Str(16);
        s.st.pos.x = r.F32();
        s.st.pos.y = r.F32();
        s.st.pos.z = r.F32();
        UnpackQuat(r.U32(), s.st.rot);
        s.st.vel = ReadVel(r);
        s.st.flags = r.U8();
        s.st.net = s.net;
        if (r.Failed() || s.net == 0 || !SafeToken(s.model, 64) || s.model == "-" || !Finite(s.st.pos) ||
            s.kind > kPolice)
            return false;
        if (!s.plate.empty() && !SafeToken(s.plate, 16)) s.plate.clear();
        out.push_back(std::move(s));
    }
    return true;
}

std::vector<uint8_t> PackDespawns(const std::vector<uint16_t>& nets) {
    net::ByteWriter w;
    const size_t n = std::min<size_t>(nets.size(), 255);
    w.U8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) w.U16(nets[i]);
    return w.Take();
}

bool ReadDespawns(net::ByteReader& r, std::vector<uint16_t>& out) {
    out.clear();
    const uint8_t n = r.U8();
    for (uint8_t i = 0; i < n && !r.Failed(); ++i) out.push_back(r.U16());
    return !r.Failed();
}

std::vector<uint8_t> PackBump(const BumpInfo& b) {
    net::ByteWriter w;
    w.U16(b.net);
    w.F32(b.error);
    w.F32(b.copySpeed);
    w.F32(b.targetSpeed);
    return w.Take();
}

bool ReadBump(net::ByteReader& r, BumpInfo& out) {
    out.net = r.U16();
    out.error = r.F32();
    out.copySpeed = r.F32();
    out.targetSpeed = r.F32();
    return !r.Failed() && out.net != 0 && std::isfinite(out.error) && std::isfinite(out.copySpeed) &&
           std::isfinite(out.targetSpeed);
}

std::vector<uint8_t> WithAuthority(uint8_t authority, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out;
    out.reserve(body.size() + 1);
    out.push_back(authority);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// ---- regions ----------------------------------------------------------------------------------------------------
namespace {
net::authority::RegionTuning RegionTuningOf(const RegionConfig& cfg) {
    net::authority::RegionTuning t;
    t.hostRadius = cfg.joinRadius;
    t.releaseRadius = cfg.leaveRadius;
    return t;
}
}  // namespace

RegionFollow::RegionFollow(const RegionConfig& cfg) : cfg_(cfg), ra_(RegionTuningOf(cfg)) {}

float RegionFollow::DistTo(net::PlayerId id) const {
    const net::authority::PlayerSpot* me = nullptr;
    const net::authority::PlayerSpot* other = nullptr;
    for (const auto& p : players_) {
        if (p.id == me_) me = &p;
        if (p.id == id) other = &p;
    }
    if (!me || !other) return -1;
    return Dist2D(me->pos, other->pos);
}

bool RegionFollow::Update(net::PlayerId me, net::PlayerId host, std::span<const net::authority::PlayerSpot> players,
                          double now) {
    players_.assign(players.begin(), players.end());
    ra_.SetPlayers(players, host);
    const bool first = !init_ || me != me_;
    const net::PlayerId before = first ? net::kNoPlayer : auth_;
    if (first) {
        me_ = me;
        auth_ = me;  // our own until we are found in someone's region
        init_ = true;
    }
    if (me == host) {
        auth_ = me;  // the host's game always runs its own traffic
    } else {
        bool inWorld = false;
        for (const auto& p : players_)
            if (p.id == me) inWorld = p.inWorld;
        if (inWorld) {  // out of the world (loading, a menu): we keep what we had
            const net::PlayerId r = ra_.RegionOf(me);
            if (auth_ == me) {
                // ours: we join another's region only within joinRadius of its owner
                const float d = r == me ? -1 : DistTo(r);
                if (d >= 0 && d <= cfg_.joinRadius) auth_ = r;
            } else {
                // another's: we follow the region we are in (its owner may change), ours only beyond leaveRadius of
                // every owner (RegionOf answers us then)
                auth_ = r;
            }
        }
    }
    if (auth_ != before) changedAt_ = now;
    return auth_ != before;
}

// ---- authority -------------------------------------------------------------------------------------------------------
namespace {
net::interest::Config InterestConfig(const HostConfig& cfg) {
    net::interest::Config c;
    c.bytesPerSecond = cfg.bytesPerSecond;
    auto& k = c.kinds[static_cast<int>(net::interest::Kind::Vehicle)];
    k.enterRadius = cfg.enterRadius;
    k.leaveRadius = cfg.leaveRadius;
    k.nearDistance = 60;
    k.nearInterval = 0.05;
    k.farInterval = 0.4;
    k.restSpeed = 0.3f;
    k.restIntervalScale = 4;
    k.maxInterval = 2.0;
    return c;
}
}  // namespace

AuthorityTraffic::AuthorityTraffic(const HostConfig& cfg) : cfg_(cfg), im_(InterestConfig(cfg)) {}

uint16_t AuthorityTraffic::NewNet() {
    for (int tries = 0; tries < 65536; ++tries) {
        const uint16_t n = nextNet_++;
        if (nextNet_ == 0) nextNet_ = 1;
        if (n == 0 || cars_.count(n)) continue;
        bool used = false;
        for (const auto& [id, p] : peers_)
            if (p.admitted.count(n)) used = true;
        if (!used) return n;
    }
    return 0;
}

bool DriverWatch::Update(double now, bool driverSeen, float speed) {
    // no rounds for a while (the host's game was paused, game/peer_pause.h, or loading): that time is not time without
    // a driver; a car not yet declared starts its watch again
    const bool gap = lastAt_ >= 0 && now - lastAt_ > kOrphanGap;
    lastAt_ = now;
    if (gap && !declared_) missSince_ = slowSince_ = -1;
    if (driverSeen || !std::isfinite(speed)) {
        missSince_ = slowSince_ = -1;
        declared_ = false;
        return false;
    }
    if (missSince_ < 0) missSince_ = now;
    if (speed <= kOrphanSlow) {
        if (slowSince_ < 0) slowSince_ = now;
    } else {
        slowSince_ = -1;
    }
    if (!declared_) {
        declared_ = slowSince_ >= 0 && now - missSince_ >= kOrphanConfirm && now - slowSince_ >= kOrphanConfirm;
    } else if (speed > kOrphanDriven) {  // nobody read at the wheel, yet it speeds up: someone drives it
        declared_ = false;
        missSince_ = now;
        slowSince_ = -1;
    }
    return declared_;
}

bool CrewGetsOut(uint8_t targetFlags, float targetSpeed, float copySpeed) {
    if (!(targetFlags & kNoDriver)) return false;
    if (!std::isfinite(targetSpeed) || targetSpeed >= kOrphanSlow) return false;
    return copySpeed < kOrphanSlow;  // (no report yet: negative)
}

size_t AuthorityTraffic::MissingNow() const {
    size_t n = 0;
    for (const auto& [net, c] : cars_) n += c.drv.Missing() ? 1 : 0;
    return n;
}

size_t AuthorityTraffic::DeclaredNow() const {
    size_t n = 0;
    for (const auto& [net, c] : cars_) n += c.drv.Declared() ? 1 : 0;
    return n;
}

void AuthorityTraffic::SetCars(const std::vector<HostCar>& cars, double now) {
    for (auto& [net, c] : cars_) c.seen = false;
    for (const HostCar& in : cars) {
        if (!Finite(in.st.pos) || !SafeToken(in.model, 64)) continue;
        uint16_t net = NetOf(in.key);
        if (net == 0) {
            net = NewNet();
            if (net == 0) continue;
            net::interest::EntityDesc d;
            d.id = net;
            d.kind = net::interest::Kind::Vehicle;
            d.pos = {in.st.pos.x, in.st.pos.y, in.st.pos.z};
            d.vel = {in.st.vel.x, in.st.vel.y, in.st.vel.z};
            d.hasVelocity = true;
            d.sizeBytes = static_cast<uint32_t>(kStateBytes);
            if (!im_.AddEntity(d)) continue;
            byKey_[in.key] = net;
        }
        Car& c = cars_[net];
        c.c = in;
        c.c.st.net = net;
        c.seen = true;
        // the Lua's kNoDriver is this round's read; what is sent is the confirmed one
        const bool was = c.drv.Declared();
        const float speed = std::sqrt(in.st.vel.x * in.st.vel.x + in.st.vel.y * in.st.vel.y + in.st.vel.z * in.st.vel.z);
        const bool orphan = c.drv.Update(now, (in.st.flags & kNoDriver) == 0, speed);
        if (orphan && !was) ++orphaned_;
        if (was && !orphan) ++drivenAgain_;
        c.c.st.flags = static_cast<uint8_t>((c.c.st.flags & ~kNoDriver) | (orphan ? kNoDriver : 0));
        im_.UpdateEntity(net, {in.st.pos.x, in.st.pos.y, in.st.pos.z}, {in.st.vel.x, in.st.vel.y, in.st.vel.z});
    }
    for (auto it = cars_.begin(); it != cars_.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        im_.RemoveEntity(it->first);
        byKey_.erase(it->second.c.key);
        it = cars_.erase(it);
    }
}

void AuthorityTraffic::SetView(uint32_t peer, const Vec3& pos, double now) {
    if (!Finite(pos)) return;
    auto it = peers_.find(peer);
    if (it == peers_.end()) {
        im_.AddPeer(peer);
        it = peers_.emplace(peer, Peer{}).first;
    }
    it->second.view = pos;
    it->second.viewAt = now;
    it->second.hasView = true;
    im_.SetPeerViewpoint(peer, {pos.x, pos.y, pos.z});
}

void AuthorityTraffic::RemovePeer(uint32_t peer) {
    im_.RemovePeer(peer);
    peers_.erase(peer);
}

uint32_t AuthorityTraffic::KeyOf(uint16_t net) const {
    auto it = cars_.find(net);
    return it == cars_.end() ? 0 : it->second.c.key;
}

uint16_t AuthorityTraffic::NetOf(uint32_t key) const {
    auto it = byKey_.find(key);
    return it == byKey_.end() ? 0 : it->second;
}

const HostPeerStats* AuthorityTraffic::Stats(uint32_t peer) const {
    auto it = peers_.find(peer);
    return it == peers_.end() ? nullptr : &it->second.stats;
}

float AuthorityTraffic::Dist(const Peer& p, uint16_t net) const {
    auto it = cars_.find(net);
    if (it == cars_.end() || !p.hasView) return 1e9f;
    return traffic::Dist(p.view, it->second.c.st.pos);
}

void AuthorityTraffic::Tick(double now, uint32_t hostMs, std::vector<PeerOut>& out) {
    out.clear();
    sentStates_.clear();
    std::set<uint16_t> sentNets;
    for (auto& [key, p] : peers_) {
        if (p.hasView && now - p.viewAt > cfg_.viewTimeout) {
            p.hasView = false;
            im_.SetPeerViewpoints(key, {});
        }
    }
    im_.Tick(now);
    for (const auto& e : im_.Events()) {
        auto it = peers_.find(e.peer);
        if (it == peers_.end()) continue;
        const auto net = static_cast<uint16_t>(e.entity);
        if (e.type == net::interest::EventType::Enter)
            it->second.relevant.insert(net);
        else
            it->second.relevant.erase(net);
    }
    for (auto& [key, p] : peers_) {
        PeerOut po;
        po.peer = key;
        std::vector<uint16_t> gone;
        for (auto it = p.admitted.begin(); it != p.admitted.end();) {
            if (!p.relevant.count(it->first) || !cars_.count(it->first)) {
                gone.push_back(it->first);
                it = p.admitted.erase(it);
            } else {
                ++it;
            }
        }
        // admission: the nearest relevant cars, at most maxPerPeer; a far admitted car gives way to a near one
        std::vector<std::pair<float, uint16_t>> cand;
        for (uint16_t net : p.relevant)
            if (!p.admitted.count(net) && cars_.count(net)) cand.push_back({Dist(p, net), net});
        std::sort(cand.begin(), cand.end());
        std::vector<uint16_t> added;
        size_t ci = 0;
        for (; ci < cand.size() && p.admitted.size() < cfg_.maxPerPeer; ++ci) {
            p.admitted[cand[ci].second] = Sent{};
            added.push_back(cand[ci].second);
        }
        for (; ci < cand.size() && cand[ci].first < cfg_.evictNear; ++ci) {
            uint16_t far = 0;
            float farD = -1;
            for (const auto& [net, s] : p.admitted) {
                const float d = Dist(p, net);
                if (d > farD) {
                    farD = d;
                    far = net;
                }
            }
            if (far == 0 || farD <= cfg_.evictFar) break;
            p.admitted.erase(far);
            gone.push_back(far);
            added.erase(std::remove(added.begin(), added.end(), far), added.end());
            p.admitted[cand[ci].second] = Sent{};
            added.push_back(cand[ci].second);
        }
        // a car evicted and admitted again in one tick is not despawned
        std::vector<uint16_t> despawn;
        for (uint16_t n : gone)
            if (!p.admitted.count(n)) despawn.push_back(n);
        for (size_t at = 0; at < despawn.size(); at += 200) {
            std::vector<uint16_t> part(despawn.begin() + static_cast<std::ptrdiff_t>(at),
                                       despawn.begin() + static_cast<std::ptrdiff_t>(std::min(at + 200, despawn.size())));
            po.despawns.push_back(PackDespawns(part));
        }
        po.despawned = static_cast<uint32_t>(despawn.size());
        std::vector<SpawnInfo> spawns;
        for (uint16_t net : added) {
            const Car& c = cars_.at(net);
            SpawnInfo s;
            s.net = net;
            s.kind = c.c.kind;
            s.model = c.c.model;
            s.color[0] = c.c.color[0];
            s.color[1] = c.c.color[1];
            s.plate = c.c.plate;
            s.st = c.c.st;
            spawns.push_back(s);
            p.admitted[net] = Sent{Quantised(c.c.st), now};
        }
        for (size_t at = 0; at < spawns.size(); at += kMaxSpawnsPerMessage) {
            std::vector<SpawnInfo> part(
                spawns.begin() + static_cast<std::ptrdiff_t>(at),
                spawns.begin() + static_cast<std::ptrdiff_t>(std::min(at + kMaxSpawnsPerMessage, spawns.size())));
            po.spawns.push_back(PackSpawns(part));
            im_.ChargeBytes(key, static_cast<uint32_t>(po.spawns.back().size()));
        }
        po.spawned = static_cast<uint32_t>(spawns.size());
        // states: the interest manager's due list, admitted cars only, unchanged ones only now and then
        std::vector<CarState> states;
        for (const auto& item : im_.SendList(key)) {
            const auto net = static_cast<uint16_t>(item.entity);
            auto ad = p.admitted.find(net);
            if (ad == p.admitted.end() || std::find(added.begin(), added.end(), net) != added.end()) continue;
            const CarState& st = cars_.at(net).c.st;
            const CarState q = Quantised(st);
            if (Same(q, ad->second.q) && now - ad->second.at < cfg_.refresh) {
                ++po.skipped;
                continue;
            }
            ad->second = Sent{q, now};
            states.push_back(st);
        }
        for (const CarState& st : states) sentNets.insert(st.net);
        for (uint16_t n : added) sentNets.insert(n);
        po.states = PackStates(hostMs, states, cfg_.maxPerPacket);
        po.sent = static_cast<uint32_t>(states.size());
        p.stats.relevant = static_cast<uint32_t>(p.relevant.size());
        p.stats.admitted = static_cast<uint32_t>(p.admitted.size());
        p.stats.spawned += po.spawned;
        p.stats.despawned += po.despawned;
        p.stats.states += po.sent;
        p.stats.skipped += po.skipped;
        for (const auto& b : po.spawns) p.stats.bytes += b.size();
        for (const auto& b : po.despawns) p.stats.bytes += b.size();
        for (const auto& b : po.states) p.stats.bytes += b.size();
        if (!po.spawns.empty() || !po.despawns.empty() || !po.states.empty() || po.skipped)
            out.push_back(std::move(po));
    }
    for (uint16_t n : sentNets) {
        auto it = cars_.find(n);
        if (it != cars_.end()) sentStates_.push_back(it->second.c.st);
    }
    // sharing: a car within shareRadius of a player who has not got it (cap, budget, a new car) is unshared since now
    for (auto& [net, c] : cars_) {
        bool missing = false;
        for (const auto& [key, p] : peers_) {
            if (!p.hasView || p.admitted.count(net)) continue;
            if (traffic::Dist(p.view, c.c.st.pos) <= cfg_.shareRadius) {
                missing = true;
                break;
            }
        }
        if (!missing)
            c.unsharedSince = -1;
        else if (c.unsharedSince < 0)
            c.unsharedSince = now;
    }
}

size_t AuthorityTraffic::Unshared(const Vec3& me, float near, double now, double grace, size_t* mine) const {
    size_t n = 0, all = 0;
    for (const auto& [net, c] : cars_) {
        if (Dist2D(me, c.c.st.pos) > near) continue;
        ++all;
        if (c.unsharedSince >= 0 && now - c.unsharedSince > grace) ++n;
    }
    if (mine) *mine = all;
    return n;
}

// ---- receiver ---------------------------------------------------------------------------------------------------
namespace {
net::replica::StreamTuning TrafficStream(const GuestConfig& cfg) {
    net::replica::StreamTuning t;
    t.playout.startDelay = cfg.interpDelay;
    t.track.capacity = std::max<size_t>(2, cfg.trackSamples);
    t.track.teleportDist = cfg.teleport;
    t.track.maxExtrapolate = cfg.maxExtrapolate;
    t.track.accelTime = 0;  // velocity only (the 0.15 traffic interpolation)
    t.track.maxLead = cfg.maxLead;
    return t;
}

// A state's discrete part in one sample's bits: flags, then the driver's controls (steer, power, brake as on the wire).
uint32_t PackBits(const CarState& s) {
    return static_cast<uint32_t>(s.flags) | (static_cast<uint32_t>(static_cast<uint8_t>(motion::PackSnorm8(s.steer))) << 8) |
           (static_cast<uint32_t>(motion::PackUnorm8(s.power)) << 16) |
           (static_cast<uint32_t>(motion::PackUnorm8(s.brake)) << 24);
}

void UnpackControls(uint32_t bits, CarState& s) {
    s.steer = motion::UnpackSnorm8(static_cast<int8_t>(static_cast<uint8_t>(bits >> 8)));
    s.power = motion::UnpackUnorm8(static_cast<uint8_t>(bits >> 16));
    s.brake = motion::UnpackUnorm8(static_cast<uint8_t>(bits >> 24));
}

float Speed(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
}  // namespace

// Core's traffic preset, tuned on the recorded path (tests/traffic_test.cpp "recorded path", TRSWEEP): the copy's
// speed is set along its heading only (SetSpeed takes km/h), so an error across the road is fixed by the blend alone;
// with ForTraffic's 1 m dead zone and 30 % share the simulated copy was 2.8 m (p95) off its car in the turns. A 0.5 m
// dead zone, 8 /s and at most 70 % of its speed: 0.9 m at 30 m, 1.2 m at 150 m (the 0.15 nudge, 8 /s with no cap or
// dead zone: 0.3 m, with a copy that slid by whatever it was off every tick). The snap (8 m, a stuck copy 2 m off for
// 3 s, a target jump) stays the preset's. UNVERIFIED in game: how the copy's physics follows (the simulation's car
// goes where SetRot / SetSpeed point it, 3 % short).
net::correction::Tuning TrafficCorrection() {
    net::correction::Tuning t = net::correction::Tuning::ForTraffic();
    t.deadZone = 0.5f;
    t.gain = 8.0f;
    t.speedShare = 0.7f;
    return t;
}

motion::VehicleTuning TrafficVehicleTuning(const net::correction::Tuning& corr) {
    motion::VehicleTuning v;
    v.snapDist = corr.snapDist;  // the corrector snaps (far, stuck, teleport); the controller pins
    return v;
}

ReceiverTraffic::ReceiverTraffic(const GuestConfig& cfg)
    : cfg_(cfg), streamTuning_(TrafficStream(cfg)), vehicleTuning_(TrafficVehicleTuning(cfg.correction)) {}

ReceiverTraffic::Auth& ReceiverTraffic::AuthOf(uint8_t authority) {
    auto it = auths_.find(authority);
    if (it == auths_.end()) it = auths_.emplace(authority, Auth(streamTuning_)).first;
    return it->second;
}

const ReceiverTraffic::Auth* ReceiverTraffic::FindAuth(uint8_t authority) const {
    auto it = auths_.find(authority);
    return it == auths_.end() ? nullptr : &it->second;
}

ReceiverTraffic::Car& ReceiverTraffic::CarOf(uint32_t key) {
    auto it = cars_.find(key);
    if (it == cars_.end()) {
        it = cars_.emplace(key, Car(vehicleTuning_, cfg_.correction)).first;
        it->second.auth = RefAuthority(key);
    }
    return it->second;
}

void ReceiverTraffic::Erase(uint32_t key) {
    auto a = auths_.find(RefAuthority(key));
    if (a != auths_.end()) a->second.stream.Remove(RefNet(key));
    cars_.erase(key);
}

void ReceiverTraffic::OnSpawn(uint8_t authority, const SpawnInfo& s, double now) {
    if (s.net == 0) return;
    const uint32_t key = RefKey(authority, s.net);
    Car& c = CarOf(key);
    if (c.haveInfo && c.copy && c.info.model != s.model) {
        pendingRemove_.push_back(key);
        c.copy = false;
    }
    if (c.released && c.despawnAt < 0 && c.haveInfo) return;  // still player sync's car: a new spawn comes after
    c.info = s;
    c.info.st.net = s.net;
    c.haveInfo = true;
    c.released = false;
    c.despawnAt = -1;
    AuthOf(authority).stream.Remove(s.net);  // its states from now on
    c.lastRecv = now;
    c.haveLast = false;
    ++stats_.spawns;
}

void ReceiverTraffic::OnDespawn(uint8_t authority, uint16_t net, double now) {
    const uint32_t key = RefKey(authority, net);
    auto it = cars_.find(key);
    if (it == cars_.end()) return;
    ++stats_.despawns;
    Car& c = it->second;
    if (c.released || !c.copy) {
        Erase(key);
        return;
    }
    c.despawnAt = now + cfg_.despawnGrace;
}

void ReceiverTraffic::OnStates(uint8_t authority, uint32_t ms, const std::vector<CarState>& states, double now) {
    Auth& a = AuthOf(authority);
    if (!a.haveClock) {
        a.clockMs = ms;
        a.haveClock = true;
    } else {
        a.clockMs += static_cast<int32_t>(ms - a.lastRawMs);  // wraps every 49.7 days
    }
    a.lastRawMs = ms;
    const double t = static_cast<double>(a.clockMs) / 1000.0;
    a.stream.Observe(t, now);
    for (const CarState& s : states) {
        auto it = cars_.find(RefKey(authority, s.net));
        if (it == cars_.end() || !it->second.haveInfo || it->second.despawnAt >= 0) continue;
        net::replica::Sample smp;
        smp.t = t;
        smp.pos = s.pos;
        smp.vel = s.vel;
        for (int k = 0; k < 4; ++k) smp.rot[k] = s.rot[k];
        smp.hasRot = true;
        smp.bits = PackBits(s);
        if (!a.stream.Push(s.net, smp)) {
            ++stats_.dropsOld;
            continue;
        }
        it->second.lastRecv = now;
        ++stats_.states;
    }
}

void ReceiverTraffic::SetLocal(const Vec3& me, bool haveMe) {
    me_ = me;
    haveMe_ = haveMe && Finite(me);
}

void ReceiverTraffic::SetReport(uint32_t key, const CopyReport& r, double now) {
    auto it = cars_.find(key);
    if (it == cars_.end() || !Finite(r.pos) || !Finite(r.vel)) return;
    it->second.rep = r;
    it->second.repAt = now;
}

void ReceiverTraffic::Release(uint32_t key) {
    auto it = cars_.find(key);
    if (it == cars_.end()) return;
    it->second.released = true;
    it->second.copy = false;
}

void ReceiverTraffic::Gone(uint32_t key) {
    auto it = cars_.find(key);
    if (it == cars_.end()) return;
    it->second.copy = false;
    it->second.vc.Reset();
    it->second.fix.Reset();
    it->second.repAt = -1e9;
}

void ReceiverTraffic::Clear() {
    cars_.clear();
    for (auto& [id, a] : auths_) {
        for (uint32_t k : a.stream.Keys()) a.stream.Remove(k);
        a.paused = false;
    }
    pendingRemove_.clear();
    quietUntil_ = -1e9;
    lastTickAt_ = -1;
    stats_.frozen = false;
}

void ReceiverTraffic::RemoveAuthority(uint8_t authority) {
    for (auto it = cars_.begin(); it != cars_.end();) {
        if (it->second.auth != authority) {
            ++it;
            continue;
        }
        if (it->second.copy) pendingRemove_.push_back(it->first);
        it = cars_.erase(it);
    }
    auths_.erase(authority);
}

void ReceiverTraffic::SetPaused(uint8_t authority, bool paused, double now) {
    Auth& a = AuthOf(authority);
    if (paused == a.paused) return;
    a.paused = paused;
    if (paused) {
        ++stats_.pauses;
        for (auto& [key, c] : cars_)
            if (c.auth == authority) c.frozen = false;  // each copy takes its pose at its next tick
        return;
    }
    quietUntil_ = std::max(quietUntil_, now + cfg_.quietAfter);
    for (auto& [key, c] : cars_) {
        if (c.auth != authority) continue;
        c.frozen = false;
        c.lastRecv = std::max(c.lastRecv, now);  // no state came while it was paused: not stale for that
        c.lastErr = 1e9f;                        // no knock before the copy was seen on its target again
        c.vc.Reset();                            // it stood on purpose: no "too slow" verdict carried over
        c.fix.Reset();
    }
}

bool ReceiverTraffic::Paused(uint8_t authority) const {
    const Auth* a = FindAuth(authority);
    return a && a->paused;
}

double ReceiverTraffic::PlayoutDelay(uint8_t authority) const {
    const Auth* a = FindAuth(authority);
    return a ? a->stream.Delay() : cfg_.interpDelay;
}

std::vector<uint8_t> ReceiverTraffic::Authorities() const {
    std::vector<uint8_t> v;
    for (const auto& [id, a] : auths_) v.push_back(id);
    return v;
}

AuthorityStats ReceiverTraffic::StatsOf(uint8_t authority) const {
    const Auth* a = FindAuth(authority);
    return a ? a->stats : AuthorityStats{};
}

bool ReceiverTraffic::Present(const Car& c, uint32_t key, double now, CarState& out, double* rtMs) const {
    if (!c.haveInfo) return false;
    const Auth* a = FindAuth(c.auth);
    const net::replica::SnapshotTrack* tr = a ? a->stream.Track(RefNet(key)) : nullptr;
    if (!tr || tr->Empty()) {
        out = c.info.st;
        out.net = c.info.net;
        if (rtMs) *rtMs = -1;
        return true;
    }
    // core's Hermite interpolation at the authority's render time, moved forward by the playout delay (bounded)
    const net::replica::Out o = a->stream.Present(RefNet(key), now);
    out = CarState{};
    out.pos = o.pos;
    out.vel = o.vel;
    for (int k = 0; k < 4; ++k) out.rot[k] = o.rot[k];
    // the driver's controls of the later state of the pair, the flags of the newest (as in 0.15)
    uint32_t bits = o.bits;
    double later = o.toT;
    if (o.teleport) {  // a jump between the pair: the nearer one is shown, the controls are still the later one's
        const double rt = a->stream.RenderTime(now);
        for (size_t i = 0; i < tr->Size(); ++i) {
            if (tr->Get(i).t > rt) {
                later = tr->Get(i).t;
                break;
            }
        }
    }
    for (size_t i = tr->Size(); i-- > 0;) {
        if (tr->Get(i).t == later) {
            bits = tr->Get(i).bits;
            break;
        }
    }
    UnpackControls(bits, out);
    out.flags = static_cast<uint8_t>(tr->Newest().bits & 0xFFu);
    out.net = c.info.net;
    if (rtMs) *rtMs = (a->stream.RenderTime(now) + std::max(0.0, a->stream.Delay())) * 1000.0;
    return true;
}

bool ReceiverTraffic::Target(uint32_t key, double now, CarState& out, double* rtMs) const {
    auto it = cars_.find(key);
    return it != cars_.end() && now - it->second.lastRecv <= cfg_.stale && Present(it->second, key, now, out, rtMs);
}

float ReceiverTraffic::DistToMe(const Car& c, uint32_t key) const {
    if (!haveMe_) return 1e9f;
    const Auth* a = FindAuth(c.auth);
    const net::replica::SnapshotTrack* tr = a ? a->stream.Track(RefNet(key)) : nullptr;
    const Vec3& p = (tr && !tr->Empty()) ? tr->Newest().pos : c.info.st.pos;
    return Dist2D(me_, p);
}

bool ReceiverTraffic::CopyInfo(uint32_t key, float& err, bool& pinned, int& pins) const {
    auto it = cars_.find(key);
    if (it == cars_.end() || !it->second.copy) return false;
    err = it->second.lastErr;
    pinned = it->second.vc.Pinned();
    pins = it->second.vc.Pins();
    return true;
}

std::vector<uint32_t> ReceiverTraffic::ReportedAt(double now) const {
    std::vector<uint32_t> v;
    for (const auto& [key, c] : cars_)
        if (c.copy && c.repAt == now) v.push_back(key);
    return v;
}

void ReceiverTraffic::Tick(double now, float dt, std::vector<GuestCmd>& out, std::vector<GuestBump>& bumps) {
    out.clear();
    bumps.clear();
    if (lastTickAt_ > 0 && now - lastTickAt_ > cfg_.localGap) {
        // our own game stood (paused, loading): the copies were left behind while the authorities' cars drove on
        ++stats_.localGaps;
        quietUntil_ = std::max(quietUntil_, now + cfg_.quietAfter);
        for (auto& [key, c] : cars_) c.lastErr = 1e9f;
    }
    lastTickAt_ = now;
    for (auto& [id, a] : auths_) a.stream.Update(now);  // each authority's render clock toward its adaptive delay
    auto drop = [&out](uint32_t key, bool grace) {
        GuestCmd c;
        c.type = GuestCmd::Type::Remove;
        c.key = key;
        c.grace = grace;
        out.push_back(c);
    };
    for (uint32_t k : pendingRemove_) drop(k, false);
    pendingRemove_.clear();
    // lifetimes
    std::vector<uint32_t> erase;
    for (auto& [key, c] : cars_) {
        const double age = Paused(c.auth) ? 0.0 : now - c.lastRecv;  // a paused authority sends nothing: not stale
        if (c.released) {
            if (age > cfg_.forget) erase.push_back(key);
            continue;
        }
        if (c.despawnAt >= 0 && now >= c.despawnAt) {
            if (c.copy) drop(key, true);
            erase.push_back(key);
            continue;
        }
        if (age > cfg_.forget) {
            if (c.copy) drop(key, false);
            erase.push_back(key);
            continue;
        }
        if (age > cfg_.stale && c.copy) {
            drop(key, false);
            c.copy = false;
            c.vc.Reset();
            c.fix.Reset();
        }
    }
    for (uint32_t k : erase) Erase(k);
    // which cars have a copy: every authority's car near us (every game shows every other game's traffic near it)
    size_t copies = 0;
    for (auto& [key, c] : cars_) {
        if (!c.copy) continue;
        if (!haveMe_ || (c.despawnAt < 0 && DistToMe(c, key) > cfg_.dropRadius)) {
            drop(key, false);
            c.copy = false;
            c.vc.Reset();
            c.fix.Reset();
            continue;
        }
        ++copies;
    }
    if (haveMe_) {
        std::vector<std::pair<float, uint32_t>> cand;
        for (auto& [key, c] : cars_) {
            if (c.copy || c.released || c.despawnAt >= 0 || !c.haveInfo || now - c.lastRecv > 1.0 || Paused(c.auth))
                continue;
            const float d = DistToMe(c, key);
            if (d <= cfg_.addRadius) cand.push_back({d, key});
        }
        std::sort(cand.begin(), cand.end());
        for (const auto& [d, key] : cand) {
            if (copies >= cfg_.maxCopies) {
                // full: the farthest copy gives way to a car at less than half its distance (one per tick)
                uint32_t far = 0;
                bool haveFar = false;
                float farD = -1;
                for (auto& [k2, c2] : cars_) {
                    if (!c2.copy || c2.despawnAt >= 0) continue;
                    const float d2 = DistToMe(c2, k2);
                    if (d2 > farD) {
                        farD = d2;
                        far = k2;
                        haveFar = true;
                    }
                }
                if (!haveFar || d >= farD * 0.5f || farD < 120) break;
                drop(far, false);
                Car& f = cars_.at(far);
                f.copy = false;
                f.vc.Reset();
                f.fix.Reset();
                --copies;
            }
            Car& c = cars_.at(key);
            GuestCmd cmd;
            cmd.type = GuestCmd::Type::Add;
            cmd.key = key;
            cmd.info = &c.info;
            Present(c, key, now, cmd.target);
            out.push_back(cmd);
            c.copy = true;
            c.vc.Reset();
            c.fix.Reset();
            c.repAt = -1e9;
            c.haveLast = false;
            c.accDt = 0;
            c.lastErr = 1e9f;  // no bump before the copy was seen on its target once
            ++copies;
        }
    }
    // drive
    float sumErr = 0, maxErr = 0;
    uint32_t nErr = 0;
    bool frozenAny = false;
    for (auto& [key, c] : cars_) {
        if (!c.copy || c.despawnAt >= 0) continue;
        c.accDt += dt;
        ++c.tick;
        const bool due = DistToMe(c, key) <= cfg_.nearDrive || c.tick % std::max(1, cfg_.farDriveEvery) == 0;
        if (!due) continue;
        GuestCmd cmd;
        cmd.type = GuestCmd::Type::Drive;
        cmd.key = key;
        if (!Present(c, key, now, cmd.target)) continue;
        if (Paused(c.auth)) {
            frozenAny = true;
            // held where it stands (its last report, else its target), braked, no speed, every tick it is due
            if (!c.frozen) {
                c.frozen = true;
                const bool fresh = now - c.repAt < cfg_.reportFresh;
                c.frozenPos = fresh ? c.rep.pos : cmd.target.pos;
                for (int k = 0; k < 4; ++k) c.frozenRot[k] = fresh ? c.rep.rot[k] : cmd.target.rot[k];
            }
            cmd.target.pos = c.frozenPos;
            for (int k = 0; k < 4; ++k) cmd.target.rot[k] = c.frozenRot[k];
            cmd.target.vel = {};
            cmd.target.power = 0;
            cmd.target.brake = 1;
            cmd.target.flags = static_cast<uint8_t>((cmd.target.flags & ~kReverse) | kControls | kHandbrake);
            cmd.vc = motion::VehicleCommand{};
            cmd.vc.fix = motion::VehicleCommand::Fix::Pin;
            for (int k = 0; k < 4; ++k) cmd.vc.rot[k] = c.frozenRot[k];
            c.accDt = 0;
            c.haveLast = false;
            out.push_back(cmd);
            continue;
        }
        motion::VehicleInput vi;
        vi.target = cmd.target.pos;
        vi.targetVel = cmd.target.vel;
        for (int k = 0; k < 4; ++k) vi.targetRot[k] = cmd.target.rot[k];
        vi.haveCopy = now - c.repAt < cfg_.reportFresh;
        if (vi.haveCopy) {
            vi.copy = c.rep.pos;
            vi.copyVel = c.rep.vel;
            for (int k = 0; k < 4; ++k) vi.copyRot[k] = c.rep.rot[k];
        }
        const bool ctl = (cmd.target.flags & kControls) != 0 && (cmd.target.flags & kReverse) == 0;
        vi.power = ctl ? cmd.target.power : 0;
        vi.teleported = c.haveLast && traffic::Dist(c.lastTarget, cmd.target.pos) > cfg_.teleport;
        c.lastTarget = cmd.target.pos;
        c.haveLast = true;
        vi.dt = c.accDt;
        c.accDt = 0;
        cmd.vc = c.vc.Update(vi);
        // The position correction (core Corrector, the traffic preset): nothing inside the dead zone (the copy's own
        // physics, with the controller's velocity, carries it), beyond it a blend capped at a share of the car's
        // speed (hidden in its motion), a snap when far, stuck or the target jumped. Pinned copies stay pinned.
        if (vi.haveCopy && (cmd.vc.fix == motion::VehicleCommand::Fix::Blend ||
                            cmd.vc.fix == motion::VehicleCommand::Fix::None)) {
            const net::correction::Result r = c.fix.Update(vi.copy, vi.target, vi.targetVel, vi.dt);
            if (r.kind == net::correction::Result::Kind::Snap) {
                cmd.vc.fix = motion::VehicleCommand::Fix::Snap;
                cmd.vc.nudge = {};
                for (int k = 0; k < 4; ++k) cmd.vc.rot[k] = vi.targetRot[k];
                cmd.vc.vel = vi.targetVel;
                c.vc.Reset();
                c.fix.Reset();
                ++stats_.fixSnaps;
            } else {
                const bool none = cmd.vc.fix == motion::VehicleCommand::Fix::None;
                cmd.vc.nudge = r.kind == net::correction::Result::Kind::Blend ? r.delta : Vec3{};
                if (r.kind == net::correction::Result::Kind::Blend) ++stats_.blends;
                cmd.vc.fix = (none && r.kind == net::correction::Result::Kind::None) ? motion::VehicleCommand::Fix::None
                                                                                      : motion::VehicleCommand::Fix::Blend;
            }
        } else if (cmd.vc.fix == motion::VehicleCommand::Fix::Pin || cmd.vc.fix == motion::VehicleCommand::Fix::Snap) {
            c.fix.Reset();  // put on its target: no stuck time, no previous target carried over
        }
        {
            const float ts = Speed(cmd.target.vel);
            const float cs = vi.haveCopy ? Speed(vi.copyVel) : -1.0f;
            if (CrewGetsOut(cmd.target.flags, ts, cs)) cmd.target.flags |= kCrewOut;
        }
        if (cmd.vc.justPinned) ++stats_.pins;
        if (cmd.vc.fix == motion::VehicleCommand::Fix::Snap) ++stats_.snaps;
        if (vi.haveCopy) {
            if (!vi.teleported && cmd.vc.error > cfg_.bumpError && c.lastErr < 1.0f && now - c.bumpAt > cfg_.bumpEvery) {
                c.bumpAt = now;
                if (now < quietUntil_) {
                    ++stats_.quietBumps;  // an authority's pause or ours: not a knock (put back all the same)
                } else {
                    bumps.push_back({key, cmd.vc.error, cmd.vc.cspeed, cmd.vc.tspeed});
                    ++stats_.bumps;
                }
            }
            c.lastErr = cmd.vc.error;
            sumErr += cmd.vc.error;
            maxErr = std::max(maxErr, cmd.vc.error);
            ++nErr;
        }
        out.push_back(cmd);
    }
    stats_.frozen = frozenAny;
    // totals, and per authority: its live cars, our copies of them, the cars within viewRadius and of those the ones
    // with a copy here that reports (it exists and is driven): "shown"
    stats_.known = static_cast<uint32_t>(cars_.size());
    stats_.live = stats_.copies = 0;
    for (auto& [id, a] : auths_) {
        a.stats = AuthorityStats{};
        a.stats.paused = a.paused;
    }
    uint32_t due = 0, shown = 0;
    for (const auto& [key, c] : cars_) {
        Auth& a = AuthOf(c.auth);
        const bool live = (now - c.lastRecv <= cfg_.stale || a.paused) && !c.released && c.haveInfo;
        if (live) {
            ++stats_.live;
            ++a.stats.live;
        }
        if (c.copy) {
            ++stats_.copies;
            ++a.stats.copies;
        }
        if (live && c.despawnAt < 0 && haveMe_ && DistToMe(c, key) <= cfg_.viewRadius) {
            ++due;
            ++a.stats.due;
            if (c.copy && now - c.repAt < cfg_.reportFresh) {
                ++shown;
                ++a.stats.shown;
            }
        }
    }
    stats_.due = static_cast<uint32_t>(std::min<size_t>(due, cfg_.maxCopies));
    stats_.shown = std::min(shown, stats_.due);
    if (nErr) {
        stats_.avgErr = sumErr / static_cast<float>(nErr);
        stats_.maxErr = maxErr;
    }
}

}  // namespace mc::game::traffic
