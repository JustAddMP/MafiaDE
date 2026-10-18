#include "game/throw_model.h"

#include <algorithm>
#include <cmath>

namespace mc::game::arc {

namespace {

constexpr float kMaxCoord = 100000.0f;
constexpr float kMaxSpeed = 80.0f;  // m/s: nobody throws faster (a car's speed added in: still far below)
constexpr double kPi = 3.14159265358979323846;

bool Finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) <= kMaxCoord &&
           std::fabs(v.y) <= kMaxCoord && std::fabs(v.z) <= kMaxCoord;
}

float Len(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 VAdd(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Mul(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

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

uint16_t Ms(double s) {
    if (!(s > 0)) return 0;
    const double ms = s * 1000.0 + 0.5;
    return ms >= 65535.0 ? 65535 : static_cast<uint16_t>(ms);
}

}  // namespace

// ---- wire -----------------------------------------------------------------------------------------------------------
std::vector<uint8_t> PackThrow(const ThrowMsg& t) {
    net::ByteWriter w(80);
    w.U32(t.seq);
    w.U8(static_cast<uint8_t>(t.kind));
    w.U8(t.flags);
    WriteVec(w, t.origin);
    WriteVec(w, t.vel);
    w.F32(t.gravity);
    w.U16(t.delayMs);
    w.U16(t.fuseMs);
    w.Str(boom::SafeName(t.weapon, 32) ? t.weapon : std::string());
    return std::vector<uint8_t>(w.Span().begin(), w.Span().end());
}

bool ReadThrow(net::ByteReader& r, ThrowMsg& out) {
    ThrowMsg t;
    t.seq = r.U32();
    const uint8_t kind = r.U8();
    t.flags = r.U8();
    t.origin = ReadVec(r);
    t.vel = ReadVec(r);
    t.gravity = r.F32();
    t.delayMs = r.U16();
    t.fuseMs = r.U16();
    t.weapon = r.Str(32);
    if (r.Failed() || kind > boom::kMaxKind || !Finite(t.origin) || !Finite(t.vel)) return false;
    if (Len(t.vel) > kMaxSpeed || !std::isfinite(t.gravity) || t.gravity < 0.5f || t.gravity > 50.0f) return false;
    if (!t.weapon.empty() && !boom::SafeName(t.weapon, 32)) return false;
    t.kind = static_cast<Kind>(kind);
    t.flags &= (kFire | kFromEntity);
    out = std::move(t);
    return true;
}

namespace {
int16_t Cm(float v) {
    const float c = std::clamp(v * 100.0f, -32767.0f, 32767.0f);
    return static_cast<int16_t>(std::lround(c));
}
}  // namespace

std::vector<uint8_t> PackFix(const FixMsg& f) {
    net::ByteWriter w(32);
    w.U32(f.seq);
    w.U16(f.tMs);
    w.U8(f.flags);
    WriteVec(w, f.pos);
    w.U16(static_cast<uint16_t>(Cm(f.vel.x)));
    w.U16(static_cast<uint16_t>(Cm(f.vel.y)));
    w.U16(static_cast<uint16_t>(Cm(f.vel.z)));
    return std::vector<uint8_t>(w.Span().begin(), w.Span().end());
}

bool ReadFix(net::ByteReader& r, FixMsg& out) {
    FixMsg f;
    f.seq = r.U32();
    f.tMs = r.U16();
    f.flags = r.U8() & (kFixBounce | kFixSettled | kFixStop | kFixTurn | kFixNoVel);
    f.pos = ReadVec(r);
    f.vel.x = static_cast<int16_t>(r.U16()) / 100.0f;
    f.vel.y = static_cast<int16_t>(r.U16()) / 100.0f;
    f.vel.z = static_cast<int16_t>(r.U16()) / 100.0f;
    if (r.Failed() || !Finite(f.pos)) return false;
    out = f;
    return true;
}

// ---- arc math -------------------------------------------------------------------------------------------------------
Vec3 Ballistic(const Vec3& o, const Vec3& v, float g, double t) {
    const float tf = static_cast<float>(t);
    return {o.x + v.x * tf, o.y + v.y * tf, o.z + v.z * tf - 0.5f * g * tf * tf};
}

Vec3 BallisticVel(const Vec3& v, float g, double t) { return {v.x, v.y, v.z - g * static_cast<float>(t)}; }

Vec3 EstimateLaunch(const Vec3& p1, double t1, const Vec3& p2, double t2, float g) {
    const double dt = t2 - t1;
    if (!(dt > 1e-4)) return {};
    Vec3 v = Mul(Sub(p2, p1), static_cast<float>(1.0 / dt));
    v.z += 0.5f * g * static_cast<float>(dt);
    return v;
}

bool FitGravity(const Vec3& p1, double t1, const Vec3& p2, double t2, const Vec3& p3, double t3, float& g) {
    const double d1 = t2 - t1, d2 = t3 - t2;
    if (!(d1 > 0.01) || !(d2 > 0.01)) return false;
    const double v1 = (p2.z - p1.z) / d1, v2 = (p3.z - p2.z) / d2;  // vertical speed at the two midpoints
    const double fit = -(v2 - v1) / ((d1 + d2) * 0.5);
    if (!std::isfinite(fit) || fit < 2.0 || fit > 30.0) return false;
    g = static_cast<float>(fit);
    return true;
}

Vec3 AimLaunch(const Vec3& d, float speed, float pitchUp) {
    float hx = d.x, hy = d.y;
    float h = std::sqrt(hx * hx + hy * hy);
    const float lim = static_cast<float>(85.0 * kPi / 180.0);
    const float pitch = std::clamp(std::atan2(d.z, h) + pitchUp, -lim, lim);
    if (h < 1e-4f) hx = 0, hy = 1, h = 1;  // straight up / down: some heading (+y)
    const float c = std::cos(pitch);
    return {speed * c * hx / h, speed * c * hy / h, speed * std::sin(pitch)};
}

void YawPitch(const Vec3& d, float& yaw, float& pitch) {
    const float h = std::sqrt(d.x * d.x + d.y * d.y);
    yaw = static_cast<float>(std::atan2(d.x, d.y) * 180.0 / kPi);
    pitch = static_cast<float>(std::atan2(d.z, h) * 180.0 / kPi);
}

// ---- the receivers' simulation -----------------------------------------------------------------------------------------
void Sim::Start(const Vec3& o, const Vec3& v, float g, bool molotov) {
    *this = Sim();
    origin = pos = o;
    vel = v;
    gravity = g;
    stopAtHit = molotov;
}

void Sim::Step(double toT, const RayFn& ray) {
    if (settled || !(toT > t)) return;
    const double dt = std::min(toT - t, kMaxStep);
    const float dtf = static_cast<float>(dt);
    if (rolling) {
        const Vec3 vh{vel.x, vel.y, 0};
        const float speed = Len(vh);
        const float after = std::max(0.0f, speed - kRollDecel * dtf);
        if (after < kSettleSpeed || speed < 1e-4f) {
            vel = {};
            settled = true;
            t += dt;
            return;
        }
        const Vec3 dir = Mul(vh, 1.0f / speed);
        const Vec3 next = VAdd(pos, Mul(dir, (speed + after) * 0.5f * dtf));
        Vec3 hit;
        int r = 0;
        if (!noRay && ray) {
            ++casts;
            const Vec3 up{0, 0, 0.1f};
            r = ray(VAdd(pos, up), VAdd(next, up), hit);
            if (r < 0) noRay = true;
        }
        if (r == 1) {  // a wall while rolling: back off it
            pos = {hit.x - dir.x * kSkin, hit.y - dir.y * kSkin, pos.z};
            vel = Mul(dir, -after * kRestitution);
            ++bounces;
        } else {
            pos = next;
            vel = Mul(dir, after);
        }
        t += dt;
        return;
    }
    const Vec3 next = Ballistic(pos, vel, gravity, dt);
    const Vec3 nvel = BallisticVel(vel, gravity, dt);
    Vec3 hit;
    int r = 0;
    if (!noRay && ray) {
        ++casts;
        r = ray(pos, next, hit);
        if (r < 0) noRay = true;
        if (r == 1 && t < kIgnoreTime && boom::Dist(hit, origin) < kIgnoreNear) r = 0;  // the thrower's own body
    }
    if (r != 1) {
        pos = next;
        vel = nvel;
        t += dt;
        if (noRay && pos.z < origin.z - 50.0f) settled = true;  // no world to land on: not carried further
        if (t > kMaxFlight) settled = true;
        return;
    }
    // a hit: how far along the step, the velocity there
    const Vec3 seg = Sub(next, pos);
    const float segLen = Len(seg);
    const float frac = segLen > 1e-5f ? std::clamp(boom::Dist(pos, hit) / segLen, 0.0f, 1.0f) : 0.0f;
    const Vec3 v = BallisticVel(vel, gravity, dt * frac);
    // ground or wall: a short probe straight down just in front of the hit (toward where it came from) finds the
    // same height on a floor, nothing that high in front of a wall
    const Vec3 dir = segLen > 1e-5f ? Mul(seg, 1.0f / segLen) : Vec3{0, 0, -1};
    const float dh = std::sqrt(dir.x * dir.x + dir.y * dir.y);
    bool ground = true;
    Vec3 n{0, 0, 1};
    if (dh > 0.2f && !(dir.z < -0.95f)) {
        const Vec3 back{hit.x - dir.x / dh * 0.1f, hit.y - dir.y / dh * 0.1f, hit.z};
        Vec3 ph;
        ++casts;
        const int pr = ray({back.x, back.y, back.z + kProbe}, {back.x, back.y, back.z - kProbe}, ph);
        ground = pr == 1 && std::fabs(ph.z - hit.z) <= 0.12f;
        if (!ground) n = {-dir.x / dh, -dir.y / dh, 0};
    }
    const float into = Dot(v, n);  // negative: moving into the surface
    const Vec3 vn = Mul(n, into), vt = Sub(v, vn);
    vel = Sub(Mul(vt, 1.0f - kFriction), Mul(vn, kRestitution));
    pos = VAdd(hit, Mul(n, kSkin));
    ++bounces;
    t += std::max(dt * frac, 0.005);
    if (stopAtHit) {
        vel = {};
        settled = true;
    } else if (ground && -into < kRollSpeed) {
        rolling = true;
        vel.z = 0;
    }
}

const char* EndWhyName(EndWhy w) {
    switch (w) {
        case EndWhy::Explosion: return "explosion";
        case EndWhy::Fuse: return "fuse";
        case EndWhy::Timeout: return "timeout";
        case EndWhy::Left: return "left";
    }
    return "?";
}

const Flight* Book::Find(uint32_t from, uint32_t seq) const {
    auto it = flights_.find({from, seq});
    return it == flights_.end() ? nullptr : &it->second;
}

bool Book::Add(uint32_t from, const ThrowMsg& t, double now) {
    const Key key{from, t.seq};
    if (flights_.count(key)) return false;
    if (flights_.size() >= kMaxFlights) {  // the oldest goes
        auto oldest = flights_.begin();
        for (auto it = flights_.begin(); it != flights_.end(); ++it)
            if (it->second.t0 < oldest->second.t0) oldest = it;
        flights_.erase(oldest);
    }
    Flight f;
    f.from = from;
    f.t = t;
    f.t0 = now - t.delayMs / 1000.0;
    f.sim.Start(t.origin, t.vel, t.gravity, t.kind == Kind::Molotov);
    flights_[key] = std::move(f);
    return true;
}

void Book::Step(double now, const RayFn& ray) {
    for (auto& [key, f] : flights_)
        if (!f.exploded) f.sim.Step(now - f.t0, ray);
}

bool Book::Fix(uint32_t from, const FixMsg& fx, double now, const RayFn& ray) {
    auto it = flights_.find({from, fx.seq});
    if (it == flights_.end()) return false;
    Flight& f = it->second;
    if (f.exploded || static_cast<int>(fx.tMs) <= f.lastFixMs) return false;  // late, or a copy
    // what is shown now (with any blend still running), to slide from
    const float k = static_cast<float>(std::clamp(1.0 - (now - f.blendAt) / kBlendTime, 0.0, 1.0));
    const Vec3 shown = VAdd(f.sim.pos, Mul(f.blend, k));
    Sim& s = f.sim;
    const bool stop = (fx.flags & (kFixSettled | kFixStop)) != 0;
    const Vec3 keepVel = s.vel;
    s.pos = fx.pos;
    s.t = fx.tMs / 1000.0;
    s.rolling = false;
    s.settled = stop;
    if (stop) {
        s.vel = {};
    } else if (fx.flags & kFixNoVel) {  // position only: our own velocity, bounced if it was still going down
        s.vel = keepVel;
        if ((fx.flags & kFixBounce) && s.vel.z < 0) s.vel.z = -s.vel.z * kRestitution;
    } else {
        s.vel = fx.vel;
    }
    if (fx.flags & kFixBounce) ++s.bounces;
    if (!stop) s.Step(now - f.t0, ray);  // from the fix's time to now
    f.lastFixMs = fx.tMs;
    ++f.fixes;
    f.blend = Sub(shown, s.pos);
    f.blendAt = now;
    return true;
}

Ended Book::MakeEnded(const Flight& f, EndWhy why, double now) const {
    Ended e;
    e.from = f.from;
    e.seq = f.t.seq;
    e.why = why;
    e.age = now - f.t0;
    e.pos = f.sim.pos;
    e.bounces = f.sim.bounces;
    e.casts = f.sim.casts;
    e.settled = f.sim.settled;
    return e;
}

bool Book::EndByExplosion(uint32_t from, uint32_t throwSeq, const Vec3& pos, double now, Ended& out) {
    auto best = flights_.end();
    if (throwSeq != 0) {
        best = flights_.find({from, throwSeq});
        if (best != flights_.end() && best->second.exploded) best = flights_.end();
    } else {
        float bestD = kMatchReach;
        for (auto it = flights_.begin(); it != flights_.end(); ++it) {
            if (it->first.first != from || it->second.exploded) continue;
            const float d = boom::Dist(it->second.sim.pos, pos);
            if (d <= bestD) bestD = d, best = it;
        }
    }
    if (best == flights_.end()) return false;
    Flight& f = best->second;
    out = MakeEnded(f, EndWhy::Explosion, now);
    out.off = boom::Dist(f.sim.pos, pos);
    f.exploded = true;  // the marker jumps to the true position for a moment
    f.explodedAt = now;
    f.boomPos = pos;
    return true;
}

void Book::Tick(double now, std::vector<Ended>& out) {
    for (auto it = flights_.begin(); it != flights_.end();) {
        const Flight& f = it->second;
        if (f.exploded) {
            if (now - f.explodedAt > kExplodedShow) it = flights_.erase(it);
            else ++it;
            continue;
        }
        const double age = now - f.t0;
        EndWhy why;
        if (f.t.fuseMs > 0 && age > f.t.fuseMs / 1000.0 + kFuseGrace) why = EndWhy::Fuse;
        else if (age > kMaxFlight) why = EndWhy::Timeout;
        else {
            ++it;
            continue;
        }
        out.push_back(MakeEnded(f, why, now));
        it = flights_.erase(it);
    }
}

void Book::Visuals(double now, std::vector<Visual>& out) const {
    for (const auto& [key, f] : flights_) {
        Visual v;
        v.from = f.from;
        v.seq = f.t.seq;
        v.kind = f.t.kind;
        v.flags = f.t.flags;
        v.age = now - f.t0;
        if (f.exploded) {
            v.pos = f.boomPos;
            v.held = true;
            v.src = "boom";
        } else {
            const float k = static_cast<float>(std::clamp(1.0 - (now - f.blendAt) / kBlendTime, 0.0, 1.0));
            v.pos = VAdd(f.sim.pos, Mul(f.blend, k));
            v.held = f.sim.settled && k <= 0.0f;
            v.src = f.sim.settled ? "settled" : f.sim.rolling ? "roll" : f.sim.noRay ? "arc" : "sim";
        }
        out.push_back(v);
    }
}

void Book::Forget(uint32_t from, double now, std::vector<Ended>& out) {
    for (auto it = flights_.begin(); it != flights_.end();) {
        if (it->first.first == from) {
            if (!it->second.exploded) out.push_back(MakeEnded(it->second, EndWhy::Left, now));
            it = flights_.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- the thrower ------------------------------------------------------------------------------------------------------
Tracker::T* Tracker::Get(uint32_t seq) {
    for (T& t : throws_)
        if (t.seq == seq) return &t;
    return nullptr;
}

const Tracker::T* Tracker::Get(uint32_t seq) const {
    for (const T& t : throws_)
        if (t.seq == seq) return &t;
    return nullptr;
}

bool Tracker::Active(uint32_t seq) const { return Get(seq) != nullptr; }

bool Tracker::Measuring(uint32_t seq) const {
    const T* t = Get(seq);
    return t && !t->following;
}

size_t Tracker::Fixes(uint32_t seq) const {
    const T* t = Get(seq);
    return t ? t->fixes : 0;
}

void Tracker::Begin(uint32_t seq, Kind kind, const std::string& weapon, double now, const Vec3& standing) {
    if (Active(seq)) return;
    if (throws_.size() >= 4) throws_.erase(throws_.begin());
    T t;
    t.seq = seq;
    t.kind = kind;
    t.weapon = weapon;
    t.begin = now;
    t.standing = standing;
    throws_.push_back(std::move(t));
}

void Tracker::Model(uint32_t seq, const Vec3& origin, const Vec3& vel) {
    T* t = Get(seq);
    if (!t || !Finite(origin) || !Finite(vel) || Len(vel) > kMaxSpeed) return;
    t->hasModel = true;
    t->modelOrigin = origin;
    t->modelVel = vel;
}

// sends throws_[i]'s Throw from what it has (samples, else the model, else nothing); then it is followed (or removed)
void Tracker::Finish(size_t i, double now, std::vector<Out>& out, std::vector<Summary>& done) {
    T& t = throws_[i];
    Summary s;
    s.seq = t.seq;
    s.weapon = t.weapon;
    s.samples = t.samples.size();
    s.after = now - t.begin;
    float yaw = 0;
    if (t.hasModel) {
        s.modelSpeed = Len(t.modelVel);
        YawPitch(t.modelVel, yaw, s.modelPitch);
    }
    ThrowMsg m;
    m.seq = t.seq;
    m.kind = t.kind;
    m.gravity = kGravity;
    m.weapon = t.weapon;
    double t0 = t.begin;
    if (t.samples.size() >= 2) {
        const auto& a = t.samples.front();
        const auto& b = t.samples.back();  // the longest baseline
        m.origin = a.second;
        m.vel = EstimateLaunch(a.second, a.first, b.second, b.first, kGravity);
        m.flags = kFromEntity;
        t0 = a.first;
        float g = 0;
        if (t.samples.size() >= 3 && FitGravity(t.samples[0].second, t.samples[0].first, t.samples[1].second,
                                                t.samples[1].first, t.samples[2].second, t.samples[2].first, g))
            s.gravity = g;
        s.why = "measured";
        s.fromEntity = true;
        t.readT = b.first;
        t.readPos = b.second;
        t.readVel = BallisticVel(m.vel, kGravity, b.first - a.first);
        t.hasRead = t.hasVel = true;
    } else if (t.hasModel) {
        m.origin = t.modelOrigin;
        m.vel = t.modelVel;
        s.why = "model";
    } else {
        s.why = "no_projectile_no_model";
        s.origin = t.standing;
        done.push_back(std::move(s));
        throws_.erase(throws_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
    if (t.kind == Kind::Molotov) m.flags |= kFire;
    m.delayMs = Ms(now - t0);
    s.sent = true;
    s.origin = m.origin;
    s.speed = Len(m.vel);
    YawPitch(m.vel, yaw, s.realPitch);
    Out o;
    o.type = Out::Type::Throw;
    o.t = std::move(m);
    o.reliable = true;
    out.push_back(std::move(o));
    done.push_back(std::move(s));
    t.following = true;  // from now on: fixes only
    t.fromEntity = s.fromEntity;
    t.t0 = t0;
    t.samples.clear();
}

void Tracker::SendFix(T& t, double now, uint8_t flags, const Vec3& pos, const Vec3& vel, std::vector<Out>& out) {
    const bool final = (flags & (kFixSettled | kFixStop)) != 0;
    if (t.fixes >= kMaxFixes || (!final && t.fixes + 1 >= kMaxFixes)) return;  // the last one is for settled / gone
    Out o;
    o.type = Out::Type::Fix;
    o.f.seq = t.seq;
    o.f.tMs = Ms(now - t.t0);
    o.f.flags = flags;
    o.f.pos = pos;
    o.f.vel = vel;
    o.reliable = final;
    ++t.fixes;
    out.push_back(std::move(o));
}

double Tracker::Sample(uint32_t seq, double now, const Vec3& pos, std::vector<Out>& out, std::vector<Summary>& done) {
    T* t = Get(seq);
    if (!t) return -1;
    if (!Finite(pos)) return kCheckInterval;
    if (!t->following) {
        if (!t->samples.empty() && now - t->samples.back().first < 1e-4) return 0;
        if (!t->samples.empty()) {
            const auto& a = t->samples.front();
            if (Len(EstimateLaunch(a.second, a.first, pos, now, kGravity)) > kMaxSpeed)
                t->samples.clear();  // a jump in the readings (the entity placed at the hand first): start again here
        }
        t->samples.push_back({now, pos});
        if (t->samples.size() < kMeasureSamples) return 0;
        Finish(static_cast<size_t>(t - throws_.data()), now, out, done);
        return Get(seq) ? kCheckInterval : -1;
    }
    if (!t->fromEntity) return -1;  // nothing to read (the glue should not have)
    const double dt = t->hasRead ? now - t->readT : 0;
    if (t->hasRead && dt < 1e-4) return 0;
    // the velocity between the last reading and this one (at this one)
    Vec3 vel{};
    if (t->hasRead) vel = BallisticVel(EstimateLaunch(t->readPos, t->readT, pos, now, kGravity), kGravity, dt);
    const bool rollingLike = t->hasRead && std::fabs(pos.z - t->readPos.z) < 0.15f && std::fabs(vel.z) < 1.0f;
    if (rollingLike) vel.z = 0;
    double next = kCheckInterval;
    if (t->pending && t->hasRead && dt <= 0.1) {  // (the reading right after it)  // a bounce / turn: its velocity from the two readings
        SendFix(*t, now, t->pending, pos, vel, out);
        t->pending = 0;
        t->still = 0;
    } else if (t->hasRead) {
        t->pending = 0;  // (a pending fix whose next reading came too late: dropped)
        const float moved = boom::Dist(pos, t->readPos);
        if (moved < kStillMove) {
            if (++t->still >= 2) {  // it lies still: the last fix, then we let it go
                SendFix(*t, now, kFixSettled, pos, {}, out);
                throws_.erase(throws_.begin() + (t - throws_.data()));
                return -1;
            }
        } else {
            t->still = 0;
            if (t->hasVel) {  // where the last reading said it would be now
                Vec3 want = rollingLike ? VAdd(t->readPos, Mul(t->readVel, static_cast<float>(dt)))
                                        : Ballistic(t->readPos, t->readVel, kGravity, dt);
                if (boom::Dist(want, pos) > kTurnDeviation) {
                    t->pending = kFixTurn;  // measure the new velocity on the next tick
                    next = 0;
                }
            }
        }
    }
    t->readT = now;
    t->readPos = pos;
    t->readVel = vel;
    t->hasRead = t->hasVel = true;
    return next;
}

double Tracker::Bounce(uint32_t seq, double now, const Vec3& pos, bool entity, std::vector<Out>& out) {
    T* t = Get(seq);
    if (!t || !t->following || !Finite(pos)) return -1;
    if (!entity || !t->fromEntity) {  // no projectile to read: where we heard it, keep your own velocity
        SendFix(*t, now, kFixBounce | kFixNoVel, pos, {}, out);
        return -1;
    }
    t->pending = kFixBounce;
    t->readT = now;  // this reading is the bounce point; the next one gives the velocity out of it
    t->readPos = pos;
    t->hasRead = true;
    t->hasVel = false;
    t->still = 0;
    return 0;
}

void Tracker::Gone(uint32_t seq, double now, std::vector<Out>& out) {
    T* t = Get(seq);
    if (!t) return;
    if (t->following && t->hasRead) SendFix(*t, now, kFixStop, t->readPos, {}, out);
    throws_.erase(throws_.begin() + (t - throws_.data()));
}

void Tracker::Tick(double now, std::vector<Out>& out, std::vector<Summary>& done) {
    for (size_t i = 0; i < throws_.size();) {
        T& t = throws_[i];
        if (!t.following) {
            if (now - t.begin >= kMeasureWindow) {
                const size_t before = throws_.size();
                Finish(i, now, out, done);  // sends; then followed, or erased
                if (throws_.size() < before) continue;
            }
        } else if (now - t.begin > kTrackMax) {
            throws_.erase(throws_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void Tracker::End(uint32_t seq, double now, std::vector<Out>& out, std::vector<Summary>& done) {
    for (size_t i = 0; i < throws_.size(); ++i) {
        if (throws_[i].seq != seq) continue;
        if (!throws_[i].following) Finish(i, now, out, done);
        T* t = Get(seq);  // the explosion ends it on every game: no more fixes
        if (t) throws_.erase(throws_.begin() + (t - throws_.data()));
        return;
    }
}

// ---- aim log ---------------------------------------------------------------------------------------------------------
bool AimLog::Take(const AimSample& s, double interval) {
    if (!on_) {
        on_ = true;
        since_ = s.t;
        n_ = 0;
        lastTaken_ = -1e9;
    }
    ++n_;
    if (s.t - lastTaken_ >= interval - 1e-9) {
        lastTaken_ = s.t;
        return true;
    }
    return false;
}

bool AimLog::Stop(double now, double& lasted, size_t& samples) {
    if (!on_) return false;
    lasted = now - since_;
    samples = n_;
    on_ = false;
    return true;
}

}  // namespace mc::game::arc
