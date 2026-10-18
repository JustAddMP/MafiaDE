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

Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Mul(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 Lerp(const Vec3& a, const Vec3& b, float t) { return Add(a, Mul(Sub(b, a), t)); }

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
    net::ByteWriter w(96);
    w.U32(t.seq);
    w.U8(static_cast<uint8_t>(t.kind));
    w.U8(t.flags);
    WriteVec(w, t.origin);
    WriteVec(w, t.vel);
    w.F32(t.gravity);
    w.U16(t.delayMs);
    w.U16(t.flightMs);
    WriteVec(w, t.landing);
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
    t.flightMs = r.U16();
    t.landing = ReadVec(r);
    t.weapon = r.Str(32);
    if (r.Failed() || kind > boom::kMaxKind || !Finite(t.origin) || !Finite(t.vel) || !Finite(t.landing)) return false;
    if (Len(t.vel) > kMaxSpeed || !std::isfinite(t.gravity) || t.gravity < 0.5f || t.gravity > 50.0f) return false;
    if (!t.weapon.empty() && !boom::SafeName(t.weapon, 32)) return false;
    t.kind = static_cast<Kind>(kind);
    t.flags &= (kFire | kFromEntity | kHasLanding);
    if (!(t.flags & kHasLanding)) t.flightMs = 0;
    out = std::move(t);
    return true;
}

std::vector<uint8_t> PackFlight(const FlightMsg& f) {
    const size_t n = std::min(f.points.size(), kMaxPointsPerMsg);
    net::ByteWriter w(8 + n * 15);
    w.U32(f.seq);
    w.U8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
        w.U16(f.points[i].tMs);
        w.U8(f.points[i].flags);
        WriteVec(w, f.points[i].pos);
    }
    return std::vector<uint8_t>(w.Span().begin(), w.Span().end());
}

bool ReadFlight(net::ByteReader& r, FlightMsg& out) {
    FlightMsg f;
    f.seq = r.U32();
    const uint8_t n = r.U8();
    if (r.Failed() || n == 0 || n > kMaxPointsPerMsg) return false;
    for (uint8_t i = 0; i < n; ++i) {
        Point p;
        p.tMs = r.U16();
        p.flags = r.U8() & (kBounce | kEnd);
        p.pos = ReadVec(r);
        if (r.Failed() || !Finite(p.pos)) return false;
        f.points.push_back(p);
    }
    out = std::move(f);
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
    const float inv = static_cast<float>(1.0 / dt);
    Vec3 v = Mul(Sub(p2, p1), inv);
    v.z += 0.5f * g * static_cast<float>(dt);
    return v;
}

bool FitGravity(const Vec3& p1, double t1, const Vec3& p2, double t2, const Vec3& p3, double t3, float& g) {
    const double d1 = t2 - t1, d2 = t3 - t2;
    if (!(d1 > 0.01) || !(d2 > 0.01)) return false;
    const double v1 = (p2.z - p1.z) / d1, v2 = (p3.z - p2.z) / d2;  // vertical speed at the two midpoints
    const double a = (v2 - v1) / ((d1 + d2) * 0.5);
    const double fit = -a;
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

// ---- receiver ---------------------------------------------------------------------------------------------------------
const char* EndWhyName(EndWhy w) {
    switch (w) {
        case EndWhy::Explosion: return "explosion";
        case EndWhy::EndPoint: return "end_point";
        case EndWhy::Timeout: return "timeout";
        case EndWhy::Left: return "left";
        case EndWhy::Replaced: return "replaced";
    }
    return "?";
}

Visual PositionAt(const Flight& f, double age) {
    Visual v;
    v.from = f.from;
    v.seq = f.t.seq;
    v.kind = f.t.kind;
    v.flags = f.t.flags;
    v.age = age;
    if (age < 0) age = 0;
    const float g = f.t.gravity;
    const auto& pts = f.points;
    if (pts.empty()) {
        if ((f.t.flags & kHasLanding) && f.t.flightMs > 0 && age >= f.t.flightMs / 1000.0) {
            v.pos = f.t.landing;
            v.held = true;
            v.src = "landing";
            return v;
        }
        constexpr double kMaxArc = 3.0;  // s: an arc with no landing and no points is not carried further
        if (f.ended) {
            v.pos = Ballistic(f.t.origin, f.t.vel, g, std::min(age, kMaxArc));
            v.held = true;
            v.src = "end";
            return v;
        }
        v.held = age >= kMaxArc;
        v.pos = Ballistic(f.t.origin, f.t.vel, g, std::min(age, kMaxArc));
        return v;
    }
    const double tFirst = pts.front().tMs / 1000.0;
    if (age <= tFirst) {  // before the first point: the arc, bent toward that point
        const Vec3 at = Ballistic(f.t.origin, f.t.vel, g, age);
        const Vec3 off = Sub(pts.front().pos, Ballistic(f.t.origin, f.t.vel, g, tFirst));
        const float k = tFirst > 1e-4 ? static_cast<float>(age / tFirst) : 1.0f;
        v.pos = Add(at, Mul(off, k));
        v.src = "points";
        return v;
    }
    size_t k = 0;
    while (k + 1 < pts.size() && pts[k + 1].tMs / 1000.0 <= age) ++k;
    if (k + 1 < pts.size()) {  // between two points
        const double ta = pts[k].tMs / 1000.0, tb = pts[k + 1].tMs / 1000.0;
        const float u = tb - ta > 1e-6 ? static_cast<float>((age - ta) / (tb - ta)) : 1.0f;
        v.pos = Lerp(pts[k].pos, pts[k + 1].pos, std::clamp(u, 0.0f, 1.0f));
        v.src = "points";
        return v;
    }
    const Point& last = pts.back();
    if ((last.flags & kEnd) || f.ended || (last.flags & kBounce)) {  // gone / after a bounce: where it was
        v.pos = last.pos;
        v.held = (last.flags & kEnd) || f.ended;
        v.src = (last.flags & kEnd) || f.ended ? "end" : "points";
        return v;
    }
    const double tLast = last.tMs / 1000.0;
    double dt = age - tLast;
    if (dt > kExtrapolate) {
        dt = kExtrapolate;
        v.held = true;
    }
    Vec3 vl = BallisticVel(f.t.vel, g, tLast);
    if (pts.size() >= 2) {
        const Point& prev = pts[pts.size() - 2];
        const double tPrev = prev.tMs / 1000.0;
        if (tLast - tPrev >= 0.02 && !(prev.flags & kBounce))
            vl = BallisticVel(EstimateLaunch(prev.pos, tPrev, last.pos, tLast, g), g, tLast - tPrev);
    }
    v.pos = Ballistic(last.pos, vl, g, dt);
    v.pos.z = std::max(v.pos.z, last.pos.z - 1.5f);  // not through the ground it is about to hit
    v.src = "extrapolated";
    return v;
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
    flights_[key] = std::move(f);
    // points that came first
    std::vector<Point> early;
    orphans_.erase(std::remove_if(orphans_.begin(), orphans_.end(),
                                  [&](const Orphan& o) {
                                      if (o.from != from || o.seq != t.seq) return false;
                                      early.push_back(o.p);
                                      return true;
                                  }),
                   orphans_.end());
    if (!early.empty()) {
        FlightMsg m;
        m.seq = t.seq;
        m.points = std::move(early);
        AddPoints(from, m, now);
    }
    return true;
}

void Book::AddPoints(uint32_t from, const FlightMsg& m, double now) {
    auto it = flights_.find({from, m.seq});
    if (it == flights_.end()) {
        orphans_.erase(std::remove_if(orphans_.begin(), orphans_.end(), [now](const Orphan& o) { return now - o.at > 2.0; }),
                       orphans_.end());
        for (const Point& p : m.points)
            if (orphans_.size() < 64) orphans_.push_back({from, m.seq, now, p});
        return;
    }
    Flight& f = it->second;
    for (const Point& p : m.points) {
        auto pos = std::lower_bound(f.points.begin(), f.points.end(), p.tMs,
                                    [](const Point& a, uint16_t t) { return a.tMs < t; });
        if (pos != f.points.end() && pos->tMs == p.tMs && pos->flags == p.flags) continue;  // a copy
        if (f.points.size() >= 400) break;
        f.points.insert(pos, p);
        if (p.flags & kBounce) ++f.bounces;
        if ((p.flags & kEnd) && !f.ended) {
            f.ended = true;
            f.endedAt = now;
        }
    }
}

Ended Book::MakeEnded(const Flight& f, EndWhy why, double now) const {
    Ended e;
    e.from = f.from;
    e.seq = f.t.seq;
    e.why = why;
    e.age = now - f.t0;
    e.pos = PositionAt(f, e.age - kPlaybackDelay).pos;
    e.points = f.points.size();
    return e;
}

bool Book::EndByExplosion(uint32_t from, uint32_t throwSeq, const Vec3& pos, double now, Ended& out) {
    auto best = flights_.end();
    if (throwSeq != 0) {
        best = flights_.find({from, throwSeq});
    } else {
        float bestD = kMatchReach;
        for (auto it = flights_.begin(); it != flights_.end(); ++it) {
            if (it->first.first != from) continue;
            const Flight& f = it->second;
            float d = boom::Dist(PositionAt(f, now - f.t0 - kPlaybackDelay).pos, pos);
            if (f.t.flags & kHasLanding) d = std::min(d, boom::Dist(f.t.landing, pos));
            if (d <= bestD) bestD = d, best = it;
        }
    }
    if (best == flights_.end()) return false;
    out = MakeEnded(best->second, EndWhy::Explosion, now);
    flights_.erase(best);
    return true;
}

void Book::Tick(double now, std::vector<Ended>& out) {
    for (auto it = flights_.begin(); it != flights_.end();) {
        const Flight& f = it->second;
        EndWhy why;
        if (f.ended && now - f.endedAt > kHoldAfterEnd) why = EndWhy::EndPoint;
        else if (now - f.t0 > kMaxFlight) why = EndWhy::Timeout;
        else {
            ++it;
            continue;
        }
        out.push_back(MakeEnded(f, why, now));
        it = flights_.erase(it);
    }
    orphans_.erase(std::remove_if(orphans_.begin(), orphans_.end(), [now](const Orphan& o) { return now - o.at > 2.0; }),
                   orphans_.end());
}

void Book::Visuals(double now, std::vector<Visual>& out) const {
    for (const auto& [key, f] : flights_) out.push_back(PositionAt(f, now - f.t0 - kPlaybackDelay));
}

void Book::Forget(uint32_t from, double now, std::vector<Ended>& out) {
    for (auto it = flights_.begin(); it != flights_.end();) {
        if (it->first.first == from) {
            out.push_back(MakeEnded(it->second, EndWhy::Left, now));
            it = flights_.erase(it);
        } else {
            ++it;
        }
    }
    orphans_.erase(std::remove_if(orphans_.begin(), orphans_.end(), [from](const Orphan& o) { return o.from == from; }),
                   orphans_.end());
}

// ---- the thrower ------------------------------------------------------------------------------------------------------
Tracker::T* Tracker::Get(uint32_t seq) {
    for (T& t : throws_)
        if (t.seq == seq) return &t;
    return nullptr;
}

bool Tracker::Active(uint32_t seq) const {
    for (const T& t : throws_)
        if (t.seq == seq) return true;
    return false;
}

void Tracker::Begin(uint32_t seq, Kind kind, const std::string& weapon, double now, const Vec3& standing) {
    if (Get(seq)) return;
    if (throws_.size() >= 4) throws_.erase(throws_.begin());
    T t;
    t.seq = seq;
    t.kind = kind;
    t.weapon = weapon;
    t.begin = now;
    t.standing = standing;
    throws_.push_back(std::move(t));
}

void Tracker::Model(uint32_t seq, const Vec3& origin, const Vec3& vel, bool hasLanding, const Vec3& landing,
                    uint16_t flightMs) {
    T* t = Get(seq);
    if (!t || !Finite(origin) || !Finite(vel) || Len(vel) > kMaxSpeed) return;
    t->hasModel = true;
    t->modelOrigin = origin;
    t->modelVel = vel;
    t->modelLanding = hasLanding && Finite(landing) && flightMs > 0;
    t->modelLandingPos = landing;
    t->modelFlightMs = t->modelLanding ? flightMs : 0;
}

void Tracker::SendThrow(T& t, double now, std::vector<Out>& out) {
    Out o;
    o.type = Out::Type::Throw;
    o.reliable = true;
    ThrowMsg& m = o.t;
    m.seq = t.seq;
    m.kind = t.kind;
    m.flags = (t.kind == Kind::Molotov ? kFire : 0) | (t.fromEntity ? kFromEntity : 0);
    if (!t.fromEntity && t.modelLanding) {
        m.flags |= kHasLanding;
        m.flightMs = t.modelFlightMs;
        m.landing = t.modelLandingPos;
    }
    m.origin = t.origin;
    m.vel = t.vel;
    m.gravity = kGravity;
    m.delayMs = Ms(now - t.t0);
    m.weapon = t.weapon;
    t.sent = true;
    t.lastPointAt = now;
    out.push_back(std::move(o));
}

void Tracker::Flush(T& t, bool reliable, std::vector<Out>& out) {
    while (!t.pending.empty()) {
        Out o;
        o.type = Out::Type::Points;
        o.reliable = reliable;
        o.f.seq = t.seq;
        const size_t n = std::min(t.pending.size(), kMaxPointsPerMsg);
        o.f.points.assign(t.pending.begin(), t.pending.begin() + static_cast<std::ptrdiff_t>(n));
        t.pending.erase(t.pending.begin(), t.pending.begin() + static_cast<std::ptrdiff_t>(n));
        t.nPoints += n;
        out.push_back(std::move(o));
    }
}

void Tracker::Sample(uint32_t seq, double now, const Vec3& pos, std::vector<Out>& out) {
    T* t = Get(seq);
    if (!t || !Finite(pos)) return;
    ++t->nSamples;
    t->last = pos;
    t->lastAt = now;
    if (t->samples.size() < 6 && (t->samples.empty() || now - t->samples.back().first > 1e-4))
        t->samples.push_back({now, pos});
    if (!t->sent) {
        if (t->samples.size() < 2) return;
        const Vec3 v = EstimateLaunch(t->samples[0].second, t->samples[0].first, t->samples[1].second,
                                      t->samples[1].first, kGravity);
        if (Len(v) > kMaxSpeed) {  // a jump in the readings (the entity placed at the hand): start again from here
            t->samples.erase(t->samples.begin());
            return;
        }
        t->fromEntity = true;
        t->t0 = t->samples[0].first;
        t->origin = t->samples[0].second;
        t->vel = v;
        SendThrow(*t, now, out);
        return;
    }
    if (now - t->lastPointAt >= kPointInterval - 1e-6) {
        t->pending.push_back({Ms(now - t->t0), 0, pos});
        t->lastPointAt = now;
        Flush(*t, false, out);
    }
}

void Tracker::Bounce(uint32_t seq, double now, const Vec3& pos, std::vector<Out>& out) {
    T* t = Get(seq);
    if (!t || !Finite(pos)) return;
    ++t->nBounces;
    if (!t->sent) return;
    t->pending.push_back({Ms(now - t->t0), kBounce, pos});
    Flush(*t, false, out);
}

Summary Tracker::Summarize(const T& t, double now, const char* why) const {
    Summary s;
    s.seq = t.seq;
    s.weapon = t.weapon;
    s.sent = t.sent;
    s.fromEntity = t.fromEntity;
    s.samples = t.nSamples;
    s.points = t.nPoints;
    s.bounces = t.nBounces;
    s.flight = t.sent ? (t.nSamples ? t.lastAt : now) - t.t0 : 0;
    s.origin = t.sent ? t.origin : t.standing;
    s.last = t.nSamples ? t.last : s.origin;
    s.speed = t.sent ? Len(t.vel) : 0;
    const auto& sm = t.samples;
    float g = 0;
    if (sm.size() >= 5 && FitGravity(sm[0].second, sm[0].first, sm[2].second, sm[2].first, sm[4].second, sm[4].first, g))
        s.gravity = g;
    else if (sm.size() >= 3 && FitGravity(sm[0].second, sm[0].first, sm[1].second, sm[1].first, sm[2].second, sm[2].first, g))
        s.gravity = g;
    if (t.hasModel) {
        float yaw = 0;
        s.modelSpeed = Len(t.modelVel);
        YawPitch(t.modelVel, yaw, s.modelPitch);
    }
    if (t.sent) {
        float yaw = 0;
        YawPitch(t.vel, yaw, s.realPitch);
    }
    s.why = why;
    return s;
}

void Tracker::End(uint32_t seq, double now, const Vec3* pos, const char* why, std::vector<Out>& out,
                  std::vector<Summary>& done) {
    T* t = Get(seq);
    if (!t) return;
    if (t->sent) {
        const Vec3 at = pos && Finite(*pos) ? *pos : t->last;
        t->pending.push_back({Ms(now - t->t0), kEnd, at});
        Flush(*t, true, out);
    }
    done.push_back(Summarize(*t, now, why));
    throws_.erase(throws_.begin() + (t - throws_.data()));
}

void Tracker::Tick(double now, std::vector<Out>& out, std::vector<Summary>& done) {
    for (size_t i = 0; i < throws_.size();) {
        T& t = throws_[i];
        if (!t.sent && t.hasModel && now - t.begin >= kEntityWait) {  // no projectile found: the aim model's launch
            t.fromEntity = false;
            t.t0 = t.begin;
            t.origin = t.modelOrigin;
            t.vel = t.modelVel;
            SendThrow(t, now, out);
        }
        const char* why = nullptr;
        if (now - t.begin > kTrackMax) why = "timeout";
        else if (!t.sent && !t.hasModel && now - t.begin > 2.0) why = "no_projectile_no_model";
        if (why) {
            End(t.seq, now, nullptr, why, out, done);  // erases throws_[i]
            continue;
        }
        ++i;
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
