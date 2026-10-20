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
    // the game's AI human (the puppets' first guesses, calibrated at run time from the copy's measured speed); the
    // FootDriver's position fix replaces the controller's nudge fields
    net::motion::LocomotionTuning t = pmotion::FootTuning();
    t.snapDist = c.snapDist;
    t.snapHeight = c.snapHeight;
    t.stuckDist = c.stuckDist;
    t.stuckTime = c.stuckTime;
    t.standSpeed = c.standSpeed;
    return t;
}

const char* const kCopyProfiles[kCopyProfileCount] = {
    "characters/traffic/traffic_base_ambient_combined",  // 0.16.0's one copy profile (and npc_sync's civilian one)
    "characters/game/game_attractivemen",
    "characters/game/game_attractivegirls",
    "characters/game/game_blackmen",
    "characters/game/game_blackwomen",
    "characters/traffic/traffic_workers_male",
};

// ---- the host's sender -------------------------------------------------------------------------------------------------
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

PedAuthority::PedAuthority(const AuthorityConfig& cfg) : cfg_(cfg), im_(InterestConfig(cfg)) {}

void PedAuthority::SetPlayers(const std::vector<PlayerView>& players, uint8_t host, uint8_t me) {
    me_ = me;
    host_ = host;
    std::set<uint8_t> present;
    if (Shares()) {
        for (const PlayerView& p : players) {
            if (!Finite(p.pos) || p.id == me) continue;
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
    }
    // a guest shares nothing: no receiver, and whatever was registered (we hosted before?) goes
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (!present.count(it->first)) {
            im_.RemovePeer(it->first);
            it = peers_.erase(it);
        } else {
            ++it;
        }
    }
    if (!Shares()) {
        for (auto& [id, e] : peds_) {
            if (e.net == 0) continue;
            im_.RemoveEntity(e.net);
            byNet_.erase(e.net);
            e.net = 0;
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
    now_ = now;
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
            // riding something (a tram, a train): faster than any gait for rideRounds rounds in a row
            const float raw = std::hypot(v.x, v.y);
            if (dt <= 1.0f && raw > cfg_.rideSpeed && Ground(lp.pos, e.lastPos) < 30.0f) {
                if (++e.fastRounds >= cfg_.rideRounds) {
                    if (now >= e.rideUntil) ++rides_;
                    e.rideUntil = now + cfg_.rideHold;
                }
            } else {
                e.fastRounds = 0;
            }
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
        const net::interest::Vec3 ip{lp.pos.x, lp.pos.y, lp.pos.z}, iv{e.vel.x, e.vel.y, 0};
        const bool riding = now < e.rideUntil;
        if (Shares() && !riding) {
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
        } else if (e.net != 0) {  // it rides something (or we are no host): not sent any more
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
    if (!Shares()) return;
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
        // deaths: once per admitted pedestrian that is dead (a guest admitted later gets the dead flag in its state)
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

size_t PedAuthority::OwnedCount() const {
    return byNet_.size();
}

size_t PedAuthority::RidingCount() const {
    size_t n = 0;
    for (const auto& [id, e] : peds_)
        if (now_ < e.rideUntil) ++n;
    return n;
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

// ---- a guest's receiver ------------------------------------------------------------------------------------------------
const char* SpawnFailName(SpawnFail w) {
    switch (w) {
        case SpawnFail::Create: return "create_failed";
        case SpawnFail::NoProfile: return "no_profile";
        case SpawnFail::Reset: return "spawner_reset";
        case SpawnFail::Timeout: return "timeout";
        default: return "other";
    }
}

SpawnFail SpawnFailOf(const std::string& why) {
    for (int i = 0; i < static_cast<int>(SpawnFail::Count); ++i) {
        const auto w = static_cast<SpawnFail>(i);
        if (why == SpawnFailName(w)) return w;
    }
    return SpawnFail::Other;
}

const char* WhyName(RecvCmd::Why w) {
    switch (w) {
        case RecvCmd::Why::LagSnap: return "lag";
        case RecvCmd::Why::Unresponsive: return "unresponsive";
        case RecvCmd::Why::CreateFailing: return "create_failing";
        case RecvCmd::Why::CopiesLost: return "copies_lost";
        case RecvCmd::Why::Body: return "body";
        case RecvCmd::Why::Surplus: return "surplus";
        case RecvCmd::Why::Handover: return "handover";
        case RecvCmd::Why::Cancelled: return "cancelled";
        default: return "-";
    }
}

PedReceiver::PedReceiver(const ReceiverConfig& cfg) : cfg_(cfg) {
    stats_.profilesLive = static_cast<uint32_t>(kCopyProfileCount);
}

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
        if (r.goneAt < 0) r.goneAt = -1e9;  // let go at the next tick
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

void PedReceiver::SetReport(uint32_t key, const CopyReport& rep, double now) {
    auto it = remotes_.find(key);
    if (it == remotes_.end() || it->second.local == 0) return;
    Remote& r = it->second;
    r.rep = rep;
    r.repAt = now;
    r.haveRep = true;
    if (rep.dead && !r.dead) r.diedHere = true;
    auto c = copies_.find(r.local);
    if (c != copies_.end() && Finite(rep.pos)) {
        c->second.pos = rep.pos;
        c->second.havePos = true;
        if (rep.dead) c->second.body = true;
    }
}

void PedReceiver::Bind(Remote& r, uint32_t local, How how) {
    r.how = how;
    r.local = local;
    r.loading = false;
    r.haveRep = false;
    r.diedHere = r.diedHereTold = false;
    r.killed = false;
    r.lagSince = -1;
    r.fresh = true;
    r.foot.Reset();
    Copy& c = copies_[local];
    c.key = Key(r.auth, r.net);
    c.shown = true;
    if (r.haveTarget) {
        c.pos = r.lastTarget.pos;
        c.havePos = true;
    }
}

void PedReceiver::Spawned(uint32_t key, uint32_t local, double now, uint8_t profile) {
    if (local == 0) return;
    uint8_t prof = profile;
    auto a = asked_.find(key);
    if (a != asked_.end()) {
        if (prof >= kCopyProfileCount) prof = a->second;
        asked_.erase(a);
    }
    if (prof < kCopyProfileCount)
        profiles_[prof].worked = true;
    else
        prof = 0;
    createFails_ = 0;
    if (copies_.count(local)) return;  // told twice
    auto it = remotes_.find(key);
    if (it != remotes_.end() && it->second.loading && it->second.local == 0 && it->second.goneAt < 0) {
        Bind(it->second, local, How::Spawned);
        copies_[local].profile = prof;
        return;
    }
    // its pedestrian went (or got another copy) meanwhile: a free copy for the pool
    Copy& c = copies_[local];
    c.profile = prof;
    c.shown = true;
    c.freeAt = now;
    if (it != remotes_.end() && it->second.loading) it->second.loading = false;
}

void PedReceiver::ProfileFailed(uint8_t profile, double now) {
    if (profile >= kCopyProfileCount) return;
    Profile& p = profiles_[profile];
    if (p.worked || p.dropped) return;
    if (now - p.lastFailAt <= 1.0) return;  // the same load's other requests
    p.lastFailAt = now;
    if (++p.fails < cfg_.profileFails) return;
    size_t live = 0;
    for (const Profile& q : profiles_)
        if (!q.dropped) ++live;
    if (live <= 1) return;  // the last one stays
    p.dropped = true;
    ++stats_.profilesDropped;
}

void PedReceiver::SpawnFailed(uint32_t key, double now, SpawnFail why) {
    ++stats_.failsBy[static_cast<size_t>(why)];
    ++stats_.spawnFails;
    if (why == SpawnFail::Create) ++createFails_;
    auto a = asked_.find(key);
    if (a != asked_.end()) {
        if (why == SpawnFail::NoProfile) ProfileFailed(a->second, now);
        asked_.erase(a);
    }
    auto it = remotes_.find(key);
    if (it == remotes_.end()) return;
    Remote& r = it->second;
    if (r.local != 0) return;
    r.loading = false;
    r.how = How::None;
    r.retryAt = now + (why == SpawnFail::Reset       ? cfg_.resetRetry
                       : why == SpawnFail::NoProfile ? cfg_.profileRetry
                                                     : cfg_.spawnRetry);
}

void PedReceiver::LocalGone(uint32_t local, double now) {
    auto c = copies_.find(local);
    if (c == copies_.end()) return;
    ++stats_.lost;
    lostAt_.push_back(now);
    if (c->second.key != 0) {
        auto rt = remotes_.find(c->second.key);
        if (rt != remotes_.end() && rt->second.local == local) {
            Remote& r = rt->second;
            r.how = How::None;
            r.local = 0;
            r.haveRep = false;
            r.diedHere = r.diedHereTold = false;
            r.lagSince = -1;
        }
    }
    copies_.erase(c);
}

void PedReceiver::SpawnerWasReset(double now) {
    resetAt_ = now;  // a reset of ours not before spawnerResetGap from now
    resetWanted_ = false;
    createFails_ = 0;
    lostAt_.clear();
    for (auto& [key, r] : remotes_)
        if (r.local == 0 && !r.loading && r.retryAt > now + cfg_.resetRetry) r.retryAt = now + cfg_.resetRetry;
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
    copies_.clear();
    asked_.clear();
    createFails_ = 0;
    lostAt_.clear();
    resetWanted_ = false;
    // the profiles' record stays: whether a profile loads is a fact of this game, not of a world
}

bool PedReceiver::IsFree(uint32_t local) const {
    auto it = copies_.find(local);
    return it != copies_.end() && it->second.key == 0;
}

uint8_t PedReceiver::ProfileFor(uint16_t net) const {
    // the live looks; one that failed once (never worked) rests a while: each try of a look that does not load
    // stops the spawner for its whole load timeout
    uint8_t live[kCopyProfileCount], ready[kCopyProfileCount];
    size_t n = 0, m = 0;
    for (size_t i = 0; i < kCopyProfileCount; ++i) {
        const Profile& p = profiles_[i];
        if (p.dropped) continue;
        live[n++] = static_cast<uint8_t>(i);
        if (p.worked || p.fails == 0 || now_ - p.lastFailAt >= cfg_.profileRest) ready[m++] = static_cast<uint8_t>(i);
    }
    if (m > 0) return ready[net % m];
    return n == 0 ? 0 : live[net % n];
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

void PedReceiver::Unbind(Remote& r, double now, std::vector<RecvCmd>& out, bool remove, RecvCmd::Why why) {
    const uint32_t key = Key(r.auth, r.net);
    if (r.loading) {  // asked for, not there yet: the request is dropped (a copy made meanwhile joins the pool)
        RecvCmd c;
        c.type = RecvCmd::Type::Remove;
        c.why = RecvCmd::Why::Cancelled;
        c.key = key;
        out.push_back(c);
        r.loading = false;
        asked_.erase(key);
    }
    if (r.local != 0) {
        auto it = copies_.find(r.local);
        if (it != copies_.end()) {
            Copy& cp = it->second;
            const bool body = cp.body || r.dead || r.diedHere || (r.haveRep && r.rep.dead);
            if (r.haveRep && Finite(r.rep.pos)) {
                cp.pos = r.rep.pos;
                cp.havePos = true;
            }
            if (remove || body) {
                RecvCmd c;
                c.type = RecvCmd::Type::Remove;
                c.why = body && why == RecvCmd::Why::None ? RecvCmd::Why::Body : why;
                c.key = key;
                c.local = r.local;
                out.push_back(c);
                copies_.erase(it);
                ++stats_.removes;
            } else {
                cp.key = 0;
                cp.shown = true;
                cp.freeAt = now;
            }
        }
    }
    r.how = How::None;
    r.local = 0;
    r.haveRep = false;
    r.diedHere = r.diedHereTold = false;
    r.lagSince = -1;
}

void PedReceiver::Tick(double now, float dt, std::vector<RecvCmd>& out) {
    now_ = now;
    for (auto& [id, a] : auths_) a.stream.Update(now);
    // 1. gone / stale pedestrians: their copies go to the pool (a body is removed)
    for (auto it = remotes_.begin(); it != remotes_.end();) {
        Remote& r = it->second;
        const bool stale = now - r.lastRecv > cfg_.stale;
        if (stale && r.goneAt < 0) r.goneAt = now;
        if (r.goneAt >= 0 || stale) {
            if (r.local != 0 || r.loading) Unbind(r, now, out);
            if (now - r.lastRecv > cfg_.forget || r.goneAt < -1e8 || (r.goneAt >= 0 && now - r.goneAt > cfg_.forget)) {
                it = remotes_.erase(it);
                continue;
            }
        }
        // a spawn that never came back
        if (r.loading && now - r.spawnAskedAt > cfg_.spawnTimeout) {
            r.loading = false;
            r.retryAt = now + cfg_.spawnRetry;
            asked_.erase(it->first);
            ++stats_.spawnFails;
            ++stats_.failsBy[static_cast<size_t>(SpawnFail::Timeout)];
        }
        ++it;
    }
    // 1b. the copy spawner's health: CreateObject failing again and again, or every copy gone at once (the spawner
    //     went and took them along): a new spawner, and the pedestrians waiting for a copy are asked for again soon.
    //     Only while no copy of it is left, free ones included (a new spawner's despawn of the old one would take
    //     living copies along).
    {
        lostAt_.erase(std::remove_if(lostAt_.begin(), lostAt_.end(),
                                     [&](double t) { return now - t > cfg_.massLossWindow; }),
                      lostAt_.end());
        if (!resetWanted_ && copies_.empty()) {
            if (static_cast<int>(lostAt_.size()) >= cfg_.massLoss) {
                resetWanted_ = true;
                resetWhy_ = RecvCmd::Why::CopiesLost;
            } else if (createFails_ >= cfg_.spawnerFails) {
                resetWanted_ = true;
                resetWhy_ = RecvCmd::Why::CreateFailing;
            }
        }
        if (resetWanted_ && now - resetAt_ >= cfg_.spawnerResetGap) {
            RecvCmd c;
            c.type = RecvCmd::Type::ResetSpawner;
            c.why = resetWhy_;
            out.push_back(c);
            ++stats_.spawnerResets;
            resetAt_ = now;
            resetWanted_ = false;
            createFails_ = 0;
            lostAt_.clear();
            for (auto& [key, r] : remotes_)
                if (r.local == 0 && !r.loading && r.retryAt > now) r.retryAt = now + cfg_.resetRetry;
        }
    }
    // 2. targets of the live ones
    std::vector<Remote*> live;
    for (auto& [key, r] : remotes_) {
        r.dist = 1e9f;
        if (r.goneAt >= 0) continue;
        PedState t;
        double tT = 0;
        if (!Target(r, now, t, &tT)) continue;
        r.lastTarget = t;
        r.lastTargetT = tT;
        r.haveTarget = true;
        r.dist = DistToMe(t.pos);
        live.push_back(&r);
    }
    // 3. every living pedestrian near us without a copy gets one, nearest first: a free pool copy (put there and
    //    shown), else a new one (while the pool has room and the spawner is not busy), else - every copy in use -
    //    the copy of a pedestrian much farther away (a handover)
    size_t loading = 0;
    for (const auto& [key, r] : remotes_)
        if (r.loading) ++loading;
    std::vector<Remote*> want;
    for (Remote* r : live)
        if (!r->dead && r->local == 0 && !r->loading && now >= r->retryAt && r->dist <= cfg_.spawnRadius &&
            now - r->firstSeen >= cfg_.spawnWait)
            want.push_back(r);
    std::sort(want.begin(), want.end(), [](const Remote* a, const Remote* b) {
        if (a->dist != b->dist) return a->dist < b->dist;
        return Key(a->auth, a->net) < Key(b->auth, b->net);
    });
    for (Remote* rp : want) {
        Remote& r = *rp;
        const uint32_t key = Key(r.auth, r.net);
        // a free copy: the one nearest its spot, an out-of-sight one before one in view
        uint32_t best = 0;
        float bestScore = 1e30f;
        if (cfg_.poolKeep > 0) {
            for (const auto& [local, c] : copies_) {
                if (c.key != 0 || c.body) continue;
                float score = c.havePos ? Ground(c.pos, r.lastTarget.pos) : 500.0f;
                if (c.shown && c.havePos && Seen(c.pos)) score += 1000.0f;
                if (score < bestScore) {
                    bestScore = score;
                    best = local;
                }
            }
        }
        if (best != 0) {
            Bind(r, best, How::Reused);
            RecvCmd c;
            c.type = RecvCmd::Type::Reuse;
            c.key = key;
            c.local = best;
            c.target = r.lastTarget;
            out.push_back(c);
            ++stats_.reuses;
            continue;
        }
        if (copies_.size() + loading < cfg_.maxCopies) {
            if (loading >= cfg_.maxLoading) continue;
            r.loading = true;
            r.spawnAskedAt = now;
            RecvCmd c;
            c.type = RecvCmd::Type::Spawn;
            c.key = key;
            c.target = r.lastTarget;
            c.profile = ProfileFor(r.net);
            asked_[key] = c.profile;
            out.push_back(c);
            ++loading;
            ++stats_.spawnsAsked;
            continue;
        }
        // the pool is full: the farthest copied pedestrian, if it is far enough and out of sight
        Remote* far = nullptr;
        for (Remote* o : live) {
            if (o->local == 0 || o->dead || o->diedHere || o->dist <= cfg_.coverRadius ||
                o->dist <= r.dist + cfg_.handoverMargin)
                continue;
            if (o->haveRep && Seen(o->rep.pos)) continue;
            if (!far || o->dist > far->dist) far = o;
        }
        if (!far) continue;
        const uint32_t local = far->local;
        far->how = How::None;
        far->local = 0;
        far->haveRep = false;
        far->lagSince = -1;
        Bind(r, local, How::Reused);
        RecvCmd c;
        c.type = RecvCmd::Type::Reuse;
        c.why = RecvCmd::Why::Handover;
        c.key = key;
        c.local = local;
        c.target = r.lastTarget;
        out.push_back(c);
        ++stats_.reuses;
        ++stats_.handovers;
    }
    // 4. free copies are hidden once nobody looks (at the latest after hideGrace); more free ones than poolKeep: the
    //    farthest go for good
    {
        size_t freeN = 0;
        for (const auto& [local, c] : copies_)
            if (c.key == 0) ++freeN;
        std::vector<std::pair<float, uint32_t>> byFar;
        for (const auto& [local, c] : copies_)
            if (c.key == 0) byFar.push_back({c.havePos ? DistToMe(c.pos) : 1e9f, local});
        std::sort(byFar.rbegin(), byFar.rend());
        for (const auto& [d, local] : byFar) {
            Copy& c = copies_.at(local);
            const bool surplus = freeN > cfg_.poolKeep;
            const bool unseen = !c.havePos || !Seen(c.pos);
            if (c.body || surplus) {
                if (!c.body && c.shown && !unseen && now - c.freeAt < cfg_.hideGrace) continue;
                RecvCmd cmd;
                cmd.type = RecvCmd::Type::Remove;
                cmd.why = c.body ? RecvCmd::Why::Body : RecvCmd::Why::Surplus;
                cmd.local = local;
                out.push_back(cmd);
                copies_.erase(local);
                --freeN;
                ++stats_.removes;
                continue;
            }
            if (c.shown && (unseen || now - c.freeAt >= cfg_.hideGrace)) {
                RecvCmd cmd;
                cmd.type = RecvCmd::Type::Hide;
                cmd.local = local;
                out.push_back(cmd);
                c.shown = false;
                ++stats_.hides;
            }
        }
    }
    // 5. drive the copies (and deaths)
    for (Remote* lp : live) {
        Remote& r = *lp;
        if (r.local == 0) continue;
        const uint32_t key = Key(r.auth, r.net);
        const PedState& tgt = r.lastTarget;
        if (r.dead) {
            if (!r.killed && !(r.haveRep && r.rep.dead)) {
                RecvCmd c;
                c.type = RecvCmd::Type::Kill;
                c.key = key;
                c.local = r.local;
                c.target = tgt;
                out.push_back(c);
                ++stats_.kills;
            }
            r.killed = true;
            auto cp = copies_.find(r.local);
            if (cp != copies_.end()) cp->second.body = true;
            continue;
        }
        if (r.diedHere) continue;  // ours died here: the host is told (PedKill), its death follows
        net::motion::LocomotionInput in;
        in.target = tgt.pos;
        in.targetVel = tgt.vel;
        in.heading[0] = tgt.heading[0];
        in.heading[1] = tgt.heading[1];
        in.sprint = tgt.gait >= 3;
        in.gaitHint = static_cast<int8_t>(tgt.gait);
        in.haveCopy = r.haveRep && now - r.repAt <= cfg_.reportFresh;
        in.copy = r.rep.pos;
        in.copyHeading[0] = r.rep.heading[0];
        in.copyHeading[1] = r.rep.heading[1];
        in.dt = dt;
        RecvCmd c;
        c.type = RecvCmd::Type::Drive;
        c.key = key;
        c.local = r.local;
        c.target = tgt;
        c.lc = r.foot.Update(in);
        // a copy that keeps its gap behind a moving target (it walks at the target's pace, the multiplier and the
        // nudges do not move it): snapped; lagging again soon after: removed, the pedestrian gets another copy
        using Fix = net::motion::LocomotionCommand::Fix;
        // a copy that just came (spawned where its pedestrian was when it was asked for - one profile load and the
        // queue ago - or a pool copy put there a moment ago) and is more than freshSnap off: put at its target at once
        // (it only just appeared: nobody sees the jump) instead of walking it in
        if (r.fresh && in.haveCopy) {
            r.fresh = false;
            if (c.lc.fix != Fix::Snap && c.lc.error > cfg_.freshSnap) {
                const float sx = tgt.pos.x - r.rep.pos.x, sy = tgt.pos.y - r.rep.pos.y;
                c.lc.fix = Fix::Snap;
                c.lc.nudge = {};
                c.lc.reissue = true;
                c.lc.moveTo.x += sx;
                c.lc.moveTo.y += sy;
                r.foot.Reset();
            }
        }
        const float tspeed = std::hypot(tgt.vel.x, tgt.vel.y);
        if (in.haveCopy && c.lc.fix != Fix::Snap && tspeed >= cfg_.lagMinSpeed && c.lc.error > cfg_.lagDist) {
            if (r.lagSince < 0 || c.lc.error < r.lagRef - cfg_.lagClose) {
                r.lagSince = now;
                r.lagRef = c.lc.error;
            } else if (now - r.lagSince >= cfg_.lagTime) {
                r.lagSince = -1;
                if (now - r.lagSnapAt <= cfg_.lagRespawn) {
                    ++stats_.unresponsive;
                    r.retryAt = now;
                    Unbind(r, now, out, true, RecvCmd::Why::Unresponsive);
                    continue;
                }
                r.lagSnapAt = now;
                // walk on from the target's spot (the walk point was picked from the old one)
                const float sx = tgt.pos.x - r.rep.pos.x, sy = tgt.pos.y - r.rep.pos.y;
                c.lc.fix = Fix::Snap;
                c.lc.nudge = {};
                c.lc.reissue = true;
                c.lc.moveTo.x += sx;
                c.lc.moveTo.y += sy;
                c.why = RecvCmd::Why::LagSnap;
                r.foot.Reset();
                ++stats_.lagSnaps;
            }
        } else {
            r.lagSince = -1;
        }
        if (c.lc.fix == Fix::Snap) ++stats_.snaps;
        r.lastErr = c.lc.error;
        out.push_back(c);
    }
    // 6. stats
    ReceiverStats& st = stats_;
    st.known = static_cast<uint32_t>(live.size());
    st.nearby = st.nearReplica = st.copies = st.free = st.loading = 0;
    for (const Remote* lp : live) {
        if (!lp->dead && lp->dist <= cfg_.coverRadius) {
            ++st.nearby;
            if (lp->local != 0) ++st.nearReplica;
        }
    }
    for (const auto& [key, r] : remotes_)
        if (r.loading) ++st.loading;
    for (const auto& [local, c] : copies_) {
        if (c.key != 0)
            ++st.copies;
        else
            ++st.free;
    }
    st.profilesLive = 0;
    for (const Profile& p : profiles_)
        if (!p.dropped) ++st.profilesLive;
}

std::vector<ReplicaInfo> PedReceiver::Replicas(double now) const {
    std::vector<ReplicaInfo> out;
    for (const auto& [key, r] : remotes_) {
        if (r.local == 0 || r.goneAt >= 0) continue;
        ReplicaInfo i;
        i.key = key;
        i.how = r.how;
        i.local = r.local;
        i.target = r.lastTarget;
        i.targetT = r.lastTargetT;
        i.copy = r.rep;
        i.haveCopy = r.haveRep && now - r.repAt <= 1.0;
        i.dead = r.dead || r.diedHere || (r.haveRep && r.rep.dead);
        i.err = i.haveCopy ? Ground(r.rep.pos, r.lastTarget.pos) : 0;
        out.push_back(i);
    }
    return out;
}

}  // namespace mc::game::ped
