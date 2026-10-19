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

// ---- host -------------------------------------------------------------------------------------------------------
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

HostTraffic::HostTraffic(const HostConfig& cfg) : cfg_(cfg), im_(InterestConfig(cfg)) {}

uint16_t HostTraffic::NewNet() {
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

void HostTraffic::SetCars(const std::vector<HostCar>& cars) {
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

void HostTraffic::SetView(uint32_t peer, const Vec3& pos, double now) {
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

void HostTraffic::RemovePeer(uint32_t peer) {
    im_.RemovePeer(peer);
    peers_.erase(peer);
}

uint32_t HostTraffic::KeyOf(uint16_t net) const {
    auto it = cars_.find(net);
    return it == cars_.end() ? 0 : it->second.c.key;
}

uint16_t HostTraffic::NetOf(uint32_t key) const {
    auto it = byKey_.find(key);
    return it == byKey_.end() ? 0 : it->second;
}

const HostPeerStats* HostTraffic::Stats(uint32_t peer) const {
    auto it = peers_.find(peer);
    return it == peers_.end() ? nullptr : &it->second.stats;
}

float HostTraffic::Dist(const Peer& p, uint16_t net) const {
    auto it = cars_.find(net);
    if (it == cars_.end() || !p.hasView) return 1e9f;
    return traffic::Dist(p.view, it->second.c.st.pos);
}

void HostTraffic::Tick(double now, uint32_t hostMs, std::vector<PeerOut>& out) {
    out.clear();
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
}

// ---- guest ------------------------------------------------------------------------------------------------------
namespace {
net::playout::PlayoutTuning TrafficPlayout(const GuestConfig& cfg) {
    net::playout::PlayoutTuning t;
    t.startDelay = cfg.interpDelay;
    return t;
}
}  // namespace

GuestTraffic::GuestTraffic(const GuestConfig& cfg) : cfg_(cfg), playout_(TrafficPlayout(cfg)) {}

void GuestTraffic::OnSpawn(const SpawnInfo& s, double now) {
    if (s.net == 0) return;
    Car& c = cars_[s.net];
    if (c.haveInfo && c.copy && c.info.model != s.model) {
        pendingRemove_.push_back(s.net);
        c.copy = false;
    }
    if (c.released && c.despawnAt < 0 && c.haveInfo) return;  // still player sync's car: a new spawn comes after
    c.info = s;
    c.info.st.net = s.net;
    c.haveInfo = true;
    c.released = false;
    c.despawnAt = -1;
    c.snaps.clear();
    c.lastRecv = now;
    c.haveLast = false;
    ++stats_.spawns;
}

void GuestTraffic::OnDespawn(uint16_t net, double now) {
    auto it = cars_.find(net);
    if (it == cars_.end()) return;
    ++stats_.despawns;
    Car& c = it->second;
    if (c.released) {
        cars_.erase(it);
        return;
    }
    if (c.copy) {
        c.despawnAt = now + cfg_.despawnGrace;
    } else {
        cars_.erase(it);
    }
}

void GuestTraffic::OnStates(uint32_t hostMs, const std::vector<CarState>& states, double now) {
    if (!haveClock_) {
        clockMs_ = hostMs;
        haveClock_ = true;
    } else {
        clockMs_ += static_cast<int32_t>(hostMs - lastRawMs_);  // wraps every 49.7 days
    }
    lastRawMs_ = hostMs;
    const double t = static_cast<double>(clockMs_) / 1000.0;
    playout_.Observe(t, now);
    offset_ = playout_.Offset();
    haveOffset_ = true;
    for (const CarState& s : states) {
        auto it = cars_.find(s.net);
        if (it == cars_.end() || !it->second.haveInfo || it->second.despawnAt >= 0) continue;
        Car& c = it->second;
        if (!c.snaps.empty() && t <= c.snaps.back().t) {
            ++stats_.dropsOld;
            continue;
        }
        c.snaps.push_back({t, s});
        while (c.snaps.size() > 8) c.snaps.pop_front();
        c.lastRecv = now;
        ++stats_.states;
    }
}

void GuestTraffic::SetLocal(const Vec3& me, bool haveHost, const Vec3& host) {
    me_ = me;
    haveMe_ = Finite(me);
    if (!haveHost || !haveMe_ || !Finite(host)) {
        hostMode_ = false;
        return;
    }
    const float d = Dist2D(me, host);
    if (hostMode_ && d > cfg_.hostFar) hostMode_ = false;
    if (!hostMode_ && d < cfg_.hostNear) hostMode_ = true;
}

void GuestTraffic::SetReport(uint16_t net, const CopyReport& r, double now) {
    auto it = cars_.find(net);
    if (it == cars_.end() || !Finite(r.pos) || !Finite(r.vel)) return;
    it->second.rep = r;
    it->second.repAt = now;
}

void GuestTraffic::Release(uint16_t net) {
    auto it = cars_.find(net);
    if (it == cars_.end()) return;
    it->second.released = true;
    it->second.copy = false;
}

void GuestTraffic::Gone(uint16_t net) {
    auto it = cars_.find(net);
    if (it == cars_.end()) return;
    it->second.copy = false;
    it->second.vc.Reset();
    it->second.repAt = -1e9;
}

void GuestTraffic::Clear() {
    cars_.clear();
    pendingRemove_.clear();
    hostPaused_ = false;
    quietUntil_ = -1e9;
    lastTickAt_ = -1;
    stats_.frozen = false;
}

void GuestTraffic::SetHostPaused(bool paused, double now) {
    if (paused == hostPaused_) return;
    hostPaused_ = paused;
    stats_.frozen = paused;
    if (paused) {
        ++stats_.pauses;
        for (auto& [net, c] : cars_) c.frozen = false;  // each copy takes its pose at its next tick
        return;
    }
    quietUntil_ = std::max(quietUntil_, now + cfg_.quietAfter);
    for (auto& [net, c] : cars_) {
        c.frozen = false;
        c.lastRecv = std::max(c.lastRecv, now);  // no state came while he was paused: not stale for that
        c.lastErr = 1e9f;                        // no knock before the copy was seen on its target again
        c.vc.Reset();                            // it stood on purpose: no "too slow" verdict carried over
    }
}

bool GuestTraffic::Present(const Car& c, double now, CarState& out) const {
    if (!c.haveInfo) return false;
    if (c.snaps.empty()) {
        out = c.info.st;
        return true;
    }
    const double rt = playout_.RenderTime(now);
    const Snap* later = &c.snaps.back();
    if (rt <= c.snaps.front().t) {
        out = c.snaps.front().s;
        later = &c.snaps.front();
    } else if (rt >= c.snaps.back().t) {
        out = c.snaps.back().s;
        const float k = static_cast<float>(std::min(rt - c.snaps.back().t, cfg_.maxExtrapolate));
        out.pos.x += out.vel.x * k;
        out.pos.y += out.vel.y * k;
        out.pos.z += out.vel.z * k;
    } else {
        for (size_t i = 0; i + 1 < c.snaps.size(); ++i) {
            const Snap& a = c.snaps[i];
            const Snap& b = c.snaps[i + 1];
            if (rt < a.t || rt >= b.t) continue;
            later = &b;
            if (traffic::Dist(a.s.pos, b.s.pos) > cfg_.teleport) {
                out = rt - a.t < (b.t - a.t) * 0.5 ? a.s : b.s;
                break;
            }
            const float u = static_cast<float>((rt - a.t) / std::max(1e-6, b.t - a.t));
            out = b.s;
            auto lerp = [u](float x, float y) { return x + (y - x) * u; };
            out.pos = net::playout::Hermite(a.s.pos, a.s.vel, b.s.pos, b.s.vel, b.t - a.t, u);
            out.vel = {lerp(a.s.vel.x, b.s.vel.x), lerp(a.s.vel.y, b.s.vel.y), lerp(a.s.vel.z, b.s.vel.z)};
            motion::Nlerp(a.s.rot, b.s.rot, u, out.rot);
            break;
        }
    }
    // the time the interpolation stands behind, moved forward along the velocity (bounded): where it is now
    const float lead = static_cast<float>(std::max(0.0, playout_.Delay()));
    Vec3 d = {out.vel.x * lead, out.vel.y * lead, out.vel.z * lead};
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const float k = len > cfg_.maxLead ? cfg_.maxLead / len : 1.0f;
    out.pos.x += d.x * k;
    out.pos.y += d.y * k;
    out.pos.z += d.z * k;
    out.flags = c.snaps.back().s.flags;
    out.steer = later->s.steer;
    out.power = later->s.power;
    out.brake = later->s.brake;
    out.net = c.info.net;
    return true;
}

bool GuestTraffic::Target(uint16_t net, double now, CarState& out) const {
    auto it = cars_.find(net);
    return it != cars_.end() && now - it->second.lastRecv <= cfg_.stale && Present(it->second, now, out);
}

float GuestTraffic::DistToMe(const Car& c, double) const {
    if (!haveMe_) return 1e9f;
    const Vec3& p = c.snaps.empty() ? c.info.st.pos : c.snaps.back().s.pos;
    return Dist2D(me_, p);
}

bool GuestTraffic::CopyInfo(uint16_t net, float& err, bool& pinned, int& pins) const {
    auto it = cars_.find(net);
    if (it == cars_.end() || !it->second.copy) return false;
    err = it->second.lastErr;
    pinned = it->second.vc.Pinned();
    pins = it->second.vc.Pins();
    return true;
}

void GuestTraffic::Tick(double now, float dt, std::vector<GuestCmd>& out, std::vector<GuestBump>& bumps) {
    out.clear();
    bumps.clear();
    if (lastTickAt_ > 0 && now - lastTickAt_ > cfg_.localGap) {
        // our own game stood (paused, loading): the copies were left behind while the host's cars drove on
        ++stats_.localGaps;
        quietUntil_ = std::max(quietUntil_, now + cfg_.quietAfter);
        for (auto& [net, c] : cars_) c.lastErr = 1e9f;
    }
    lastTickAt_ = now;
    playout_.Update(now);  // the copies' render clock toward the adaptive delay
    auto drop = [&out](uint16_t net, bool grace) {
        GuestCmd c;
        c.type = GuestCmd::Type::Remove;
        c.net = net;
        c.grace = grace;
        out.push_back(c);
    };
    for (uint16_t n : pendingRemove_) drop(n, false);
    pendingRemove_.clear();
    // lifetimes
    for (auto it = cars_.begin(); it != cars_.end();) {
        Car& c = it->second;
        const double age = hostPaused_ ? 0.0 : now - c.lastRecv;  // a paused host sends nothing: not stale for that
        if (c.released) {
            if (age > cfg_.forget) {
                it = cars_.erase(it);
            } else {
                ++it;
            }
            continue;
        }
        if (c.despawnAt >= 0 && now >= c.despawnAt) {
            if (c.copy) drop(it->first, true);
            it = cars_.erase(it);
            continue;
        }
        if (age > cfg_.forget) {
            if (c.copy) drop(it->first, false);
            it = cars_.erase(it);
            continue;
        }
        if (age > cfg_.stale && c.copy) {
            drop(it->first, false);
            c.copy = false;
            c.vc.Reset();
        }
        ++it;
    }
    // which cars have a copy
    size_t copies = 0;
    for (auto& [net, c] : cars_) {
        if (!c.copy) continue;
        if (!hostMode_ || !haveMe_ || (c.despawnAt < 0 && DistToMe(c, now) > cfg_.dropRadius)) {
            drop(net, false);
            c.copy = false;
            c.vc.Reset();
            continue;
        }
        ++copies;
    }
    if (hostMode_ && haveMe_) {
        std::vector<std::pair<float, uint16_t>> cand;
        for (auto& [net, c] : cars_) {
            if (c.copy || c.released || c.despawnAt >= 0 || !c.haveInfo || now - c.lastRecv > 1.0) continue;
            const float d = DistToMe(c, now);
            if (d <= cfg_.addRadius) cand.push_back({d, net});
        }
        std::sort(cand.begin(), cand.end());
        for (const auto& [d, net] : cand) {
            if (copies >= cfg_.maxCopies) {
                // full: the farthest copy gives way to a car at less than half its distance (one per tick)
                uint16_t far = 0;
                float farD = -1;
                for (auto& [n2, c2] : cars_) {
                    if (!c2.copy || c2.despawnAt >= 0) continue;
                    const float d2 = DistToMe(c2, now);
                    if (d2 > farD) {
                        farD = d2;
                        far = n2;
                    }
                }
                if (far == 0 || d >= farD * 0.5f || farD < 120) break;
                drop(far, false);
                cars_[far].copy = false;
                cars_[far].vc.Reset();
                --copies;
            }
            Car& c = cars_[net];
            GuestCmd cmd;
            cmd.type = GuestCmd::Type::Add;
            cmd.net = net;
            cmd.info = &c.info;
            Present(c, now, cmd.target);
            out.push_back(cmd);
            c.copy = true;
            c.vc.Reset();
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
    for (auto& [net, c] : cars_) {
        if (!c.copy || c.despawnAt >= 0) continue;
        c.accDt += dt;
        ++c.tick;
        const bool due = DistToMe(c, now) <= cfg_.nearDrive || c.tick % std::max(1, cfg_.farDriveEvery) == 0;
        if (!due) continue;
        GuestCmd cmd;
        cmd.type = GuestCmd::Type::Drive;
        cmd.net = net;
        if (!Present(c, now, cmd.target)) continue;
        if (hostPaused_) {
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
        if (cmd.vc.justPinned) ++stats_.pins;
        if (cmd.vc.fix == motion::VehicleCommand::Fix::Snap) ++stats_.snaps;
        if (vi.haveCopy) {
            if (!vi.teleported && cmd.vc.error > cfg_.bumpError && c.lastErr < 1.0f && now - c.bumpAt > cfg_.bumpEvery) {
                c.bumpAt = now;
                if (now < quietUntil_) {
                    ++stats_.quietBumps;  // the host's pause or ours: not a knock (put back all the same)
                } else {
                    bumps.push_back({net, cmd.vc.error, cmd.vc.cspeed, cmd.vc.tspeed});
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
    stats_.known = static_cast<uint32_t>(cars_.size());
    stats_.live = 0;
    stats_.copies = 0;
    for (const auto& [net, c] : cars_) {
        if (now - c.lastRecv <= cfg_.stale && !c.released) ++stats_.live;
        if (c.copy) ++stats_.copies;
    }
    if (nErr) {
        stats_.avgErr = sumErr / static_cast<float>(nErr);
        stats_.maxErr = maxErr;
    }
}

}  // namespace mc::game::traffic
