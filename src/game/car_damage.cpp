#include "game/car_damage.h"

#include "net/wire.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mc::game::car_damage {

uint8_t Quantize(float energy, float max) {
    if (!std::isfinite(energy) || !std::isfinite(max) || max <= 0) return 0;
    float d = 1.0f - energy / max;
    d = std::clamp(d, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(d * kMaxDamage));
}

float EnergyOf(uint8_t d, float max) {
    if (d >= kMaxDamage) return 0.0f;
    return max * (1.0f - static_cast<float>(d) / kMaxDamage);
}

bool Snapshot::Damaged() const {
    if (flags) return true;
    for (uint8_t p : parts)
        if (p) return true;
    for (uint8_t w : wheels)
        if (w) return true;
    return false;
}

std::string Snapshot::Describe() const {
    int dmg = 0, gone = 0, maxd = 0, burst = 0, off = 0;
    for (uint8_t p : parts) {
        if (p == kGone)
            ++gone;
        else if (p)
            maxd = std::max<int>(maxd, p);
        if (p) ++dmg;
    }
    for (uint8_t w : wheels) {
        if (w == kWheelBurst) ++burst;
        if (w == kWheelOff) ++off;
    }
    char buf[192];
    std::snprintf(buf, sizeof buf, "parts %d/%zu damaged (worst %d%%, %d gone), tyres %d burst, %d wheels off%s%s", dmg,
                  parts.size(), maxd * 100 / kMaxDamage, gone, burst, off, (flags & kBurning) ? ", burning" : "",
                  (flags & kExploded) ? ", exploded" : "");
    return buf;
}

uint32_t Layout(const std::vector<int32_t>& types) {
    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 16777619u;
        }
    };
    mix(static_cast<uint32_t>(types.size()));
    for (int32_t t : types) mix(static_cast<uint32_t>(t));
    return h ? h : 1;
}

// ---- CarRef -----------------------------------------------------------------------------------------------------------
bool CarRef::operator<(const CarRef& o) const {
    if (kind != o.kind) return kind < o.kind;
    if (owner != o.owner) return owner < o.owner;
    return id < o.id;
}

std::string CarRef::Text() const {
    char buf[32];
    if (kind == kPlayer)
        std::snprintf(buf, sizeof buf, "%u:%u", owner, id);
    else
        std::snprintf(buf, sizeof buf, "%c%u", kind == kTraffic ? 't' : 'h', id);
    return buf;
}

bool CarRef::Parse(const std::string& s, CarRef& out) {
    if (s.size() < 2 || s.size() > 24) return false;
    char* end = nullptr;
    if (s[0] == 't' || s[0] == 'h') {
        const unsigned long v = std::strtoul(s.c_str() + 1, &end, 10);
        if (!end || *end || v == 0 || v > 0xFFFFFFFFul || (s[0] == 't' && v > 0xFFFF)) return false;
        out = CarRef{s[0] == 't' ? kTraffic : kHostTraffic, 0, static_cast<uint32_t>(v)};
        return true;
    }
    const unsigned long o = std::strtoul(s.c_str(), &end, 10);
    if (!end || *end != ':' || o > 255) return false;
    const char* rest = end + 1;
    const unsigned long g = std::strtoul(rest, &end, 10);
    if (!end || *end || end == rest || g == 0 || g > 0xFFFFFFFFul) return false;
    out = CarRef{kPlayer, static_cast<uint8_t>(o), static_cast<uint32_t>(g)};
    return true;
}

// ---- wire ---------------------------------------------------------------------------------------------------------------
namespace {

void WriteRef(net::ByteWriter& w, const CarRef& r) {
    if (r.kind == CarRef::kPlayer) {
        w.U8(0);
        w.U8(r.owner);
        w.U32(r.id);
    } else {
        w.U8(1);
        w.U16(static_cast<uint16_t>(r.id));
    }
}

bool ReadRef(net::ByteReader& r, CarRef& out) {
    const uint8_t kind = r.U8();
    if (kind == 0) {
        out.kind = CarRef::kPlayer;
        out.owner = r.U8();
        out.id = r.U32();
    } else if (kind == 1) {
        out.kind = CarRef::kTraffic;
        out.owner = 0;
        out.id = r.U16();
    } else {
        return false;
    }
    return !r.Failed() && out.id != 0;
}

}  // namespace

std::vector<uint8_t> Write(const Update& u) {
    net::ByteWriter w(32 + u.parts.size() * 2);
    WriteRef(w, u.ref);
    w.U16(u.epoch);
    uint8_t flags = u.flags & (kExploded | kBurning);
    if (u.full) flags |= kMsgFull;
    if (u.repaired) flags |= kMsgRepaired;
    w.U8(flags);
    const float bl = std::clamp(std::isfinite(u.burnLeft) ? u.burnLeft : 0.0f, 0.0f, 655.0f);
    w.U16(static_cast<uint16_t>(std::lround(bl * 100.0f)));
    w.U32(u.layout);
    w.U8(u.partCount);
    const size_t nw = std::min(u.wheels.size(), kMaxWheels);
    w.U8(static_cast<uint8_t>(nw));
    for (size_t i = 0; i < nw; i += 4) {
        uint8_t b = 0;
        for (size_t k = 0; k < 4 && i + k < nw; ++k) b |= static_cast<uint8_t>((u.wheels[i + k] & 3) << (2 * k));
        w.U8(b);
    }
    size_t n = 0;
    for (const auto& p : u.parts)
        if (p.first < u.partCount) ++n;
    n = std::min(n, kMaxParts);
    w.U8(static_cast<uint8_t>(n));
    size_t put = 0;
    for (const auto& p : u.parts) {
        if (p.first >= u.partCount || put >= n) continue;
        w.U8(p.first);
        w.U8(p.second);
        ++put;
    }
    return w.Take();
}

bool Read(const uint8_t* data, size_t len, Update& u) {
    net::ByteReader r(data, len);
    u = Update{};
    if (!ReadRef(r, u.ref)) return false;
    u.epoch = r.U16();
    const uint8_t flags = r.U8();
    u.flags = flags & (kExploded | kBurning);
    u.full = (flags & kMsgFull) != 0;
    u.repaired = (flags & kMsgRepaired) != 0;
    u.burnLeft = r.U16() / 100.0f;
    u.layout = r.U32();
    u.partCount = r.U8();
    const uint8_t nw = r.U8();
    if (r.Failed() || nw > kMaxWheels || u.partCount > kMaxParts) return false;
    u.wheels.resize(nw);
    for (size_t i = 0; i < nw; i += 4) {
        const uint8_t b = r.U8();
        for (size_t k = 0; k < 4 && i + k < nw; ++k) {
            const uint8_t v = (b >> (2 * k)) & 3;
            u.wheels[i + k] = v > kWheelOff ? kWheelOff : v;
        }
    }
    const uint8_t n = r.U8();
    if (r.Failed() || n > u.partCount) return false;
    u.parts.reserve(n);
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t idx = r.U8(), v = r.U8();
        if (idx >= u.partCount) return false;
        u.parts.emplace_back(idx, v);
    }
    return !r.Failed();
}

std::vector<uint8_t> WriteRequest(const CarRef& ref) {
    net::ByteWriter w(8);
    WriteRef(w, ref);
    return w.Take();
}

bool ReadRequest(const uint8_t* data, size_t len, CarRef& ref) {
    net::ByteReader r(data, len);
    return ReadRef(r, ref);
}

// ---- diff, merge, repair --------------------------------------------------------------------------------------------
std::vector<std::pair<uint8_t, uint8_t>> Diff(const Snapshot& was, const Snapshot& now) {
    std::vector<std::pair<uint8_t, uint8_t>> out;
    const size_t n = std::min(now.parts.size(), kMaxParts);
    const bool same = was.layout == now.layout && was.parts.size() == now.parts.size();
    for (size_t i = 0; i < n; ++i) {
        const uint8_t v = now.parts[i];
        if (!same) {
            if (v) out.emplace_back(static_cast<uint8_t>(i), v);
            continue;
        }
        const uint8_t o = was.parts[i];
        if (v == o) continue;
        // small changes are rounding: only a step of 2 quanta, or reaching / leaving intact, empty or gone, counts
        const bool edge = v == 0 || o == 0 || v >= kMaxDamage || o >= kMaxDamage;
        if (edge || std::abs(static_cast<int>(v) - static_cast<int>(o)) >= 2) out.emplace_back(static_cast<uint8_t>(i), v);
    }
    return out;
}

Update Full(const CarRef& ref, uint16_t epoch, const Snapshot& s) {
    Update u;
    u.ref = ref;
    u.epoch = epoch;
    u.flags = s.flags;
    u.burnLeft = s.burnLeft;
    u.layout = s.layout;
    u.partCount = static_cast<uint8_t>(std::min(s.parts.size(), kMaxParts));
    u.wheels.assign(s.wheels.begin(), s.wheels.begin() + std::min(s.wheels.size(), kMaxWheels));
    for (size_t i = 0; i < u.partCount; ++i)
        if (s.parts[i]) u.parts.emplace_back(static_cast<uint8_t>(i), s.parts[i]);
    u.full = true;
    return u;
}

bool MergeUp(Snapshot& into, const Update& u) {
    bool rose = false;
    const bool fresh = into.parts.empty() && into.layout == 0;
    if (fresh || (u.full && into.layout != u.layout)) {
        rose = !fresh;
        into.layout = u.layout;
        into.parts.assign(u.partCount, 0);
        into.wheels.clear();
    }
    if (into.layout == u.layout) {
        if (into.parts.size() < u.partCount) into.parts.resize(u.partCount, 0);
        for (const auto& [i, v] : u.parts) {
            if (i >= into.parts.size()) continue;
            uint8_t& cur = into.parts[i];
            if (cur == kGone) continue;
            if (v == kGone || v > cur) {
                cur = v;
                rose = true;
            }
        }
    }
    if (into.wheels.size() < u.wheels.size()) into.wheels.resize(u.wheels.size(), kWheelOk);
    for (size_t i = 0; i < u.wheels.size(); ++i)
        if (u.wheels[i] > into.wheels[i]) {
            into.wheels[i] = u.wheels[i];
            rose = true;
        }
    const uint8_t add = u.flags & static_cast<uint8_t>(~into.flags) & (kExploded | kBurning);
    if (add) {
        into.flags |= add;
        rose = true;
    }
    if (u.flags & kBurning) into.burnLeft = (add & kBurning) ? u.burnLeft : std::min(into.burnLeft, u.burnLeft);
    return rose;
}

bool Repaired(const Snapshot& was, const Snapshot& now) {
    if ((was.flags & kExploded) && !(now.flags & kExploded)) return true;
    if (was.layout == now.layout && was.parts.size() == now.parts.size()) {
        for (size_t i = 0; i < now.parts.size(); ++i) {
            const uint8_t o = was.parts[i], v = now.parts[i];
            if (o == kGone && v != kGone) return true;
            if (o != kGone && v != kGone && o >= v + 24) return true;
        }
    }
    const size_t nw = std::min(was.wheels.size(), now.wheels.size());
    for (size_t i = 0; i < nw; ++i)
        if (now.wheels[i] < was.wheels[i]) return true;
    return false;
}

Apply Plan(const Snapshot& local, const Snapshot& auth, Applied& done) {
    Apply a;
    if (local.layout == auth.layout && local.parts.size() == auth.parts.size()) {
        if (done.parts.size() != auth.parts.size()) done.parts.assign(auth.parts.size(), 0);
        for (size_t i = 0; i < auth.parts.size(); ++i) {
            const uint8_t want = auth.parts[i], have = local.parts[i];
            if (want == kGone) {
                if (have != kGone && done.parts[i] != kGone) {
                    a.parts.emplace_back(static_cast<uint8_t>(i), kGone);
                    done.parts[i] = kGone;
                }
            } else if (have != kGone && want > have + kTolerance && want > done.parts[i]) {
                a.parts.emplace_back(static_cast<uint8_t>(i), want);
                done.parts[i] = want;
            }
        }
    }
    const size_t nw = std::min(local.wheels.size(), auth.wheels.size());
    if (done.wheels.size() < nw) done.wheels.resize(nw, kWheelOk);
    for (size_t i = 0; i < nw; ++i)
        if (auth.wheels[i] > local.wheels[i] && auth.wheels[i] > done.wheels[i]) {
            a.wheels.emplace_back(static_cast<uint8_t>(i), auth.wheels[i]);
            done.wheels[i] = auth.wheels[i];
        }
    if ((auth.flags & kExploded) && !(local.flags & kExploded)) {
        if (!done.explode) a.explode = done.explode = true;
    } else if ((auth.flags & kBurning) && !(local.flags & (kBurning | kExploded)) && !done.burn) {
        a.burn = done.burn = true;
        a.burnLeft = std::max(auth.burnLeft, 0.5f);
    }
    return a;
}

Update Excess(const CarRef& ref, uint16_t epoch, const Snapshot& local, const Snapshot& auth) {
    Update u;
    u.ref = ref;
    u.epoch = epoch;
    u.layout = local.layout;
    u.partCount = static_cast<uint8_t>(std::min(local.parts.size(), kMaxParts));
    if (local.layout == auth.layout && local.parts.size() == auth.parts.size()) {
        for (size_t i = 0; i < u.partCount; ++i) {
            const uint8_t have = local.parts[i], want = auth.parts[i];
            if (want == kGone) continue;
            if (have == kGone || have > want + kTolerance) u.parts.emplace_back(static_cast<uint8_t>(i), have);
        }
    }
    u.wheels.assign(local.wheels.size(), kWheelOk);
    bool anyWheel = false;
    for (size_t i = 0; i < local.wheels.size(); ++i) {
        const uint8_t a = i < auth.wheels.size() ? auth.wheels[i] : kWheelOk;
        if (local.wheels[i] > a) {
            u.wheels[i] = local.wheels[i];
            anyWheel = true;
        }
    }
    if (!anyWheel) u.wheels.clear();
    u.flags = local.flags & static_cast<uint8_t>(~auth.flags) & (kExploded | kBurning);
    if (u.flags & kBurning) u.burnLeft = local.burnLeft;
    return u;
}

float HoldFire(float left, float hold) {
    return std::isfinite(left) && left > hold ? left : hold;
}

bool Empty(const Update& u) {
    if (!u.parts.empty() || u.flags) return false;
    for (uint8_t w : u.wheels)
        if (w) return false;
    return true;
}

// ---- Book ---------------------------------------------------------------------------------------------------------------
namespace {

Snapshot FromUpdate(const Update& u) {
    Snapshot s;
    s.layout = u.layout;
    s.parts.assign(u.partCount, 0);
    for (const auto& [i, v] : u.parts)
        if (i < s.parts.size()) s.parts[i] = v;
    s.wheels = u.wheels;
    s.flags = u.flags;
    s.burnLeft = u.burnLeft;
    return s;
}

std::string PartsText(const std::vector<std::pair<uint8_t, uint8_t>>& parts, size_t max = 12) {
    std::string s;
    char buf[16];
    for (size_t i = 0; i < parts.size() && i < max; ++i) {
        std::snprintf(buf, sizeof buf, "%s%u:%u", i ? "," : "", parts[i].first, parts[i].second);
        s += buf;
    }
    if (parts.size() > max) s += ",...";
    return s.empty() ? "-" : s;
}

// how far the parts of an Excess() lie above `auth` (quanta, summed; a gone part counts as 64)
int Over(const Update& excess, const Snapshot& auth) {
    int s = 0;
    for (const auto& [i, v] : excess.parts) {
        const int a = i < auth.parts.size() ? auth.parts[i] : 0;
        s += v == kGone ? 64 : std::max(0, static_cast<int>(v) - a);
    }
    return s;
}

}  // namespace

bool Book::Known(const CarRef& ref, Snapshot* auth, uint16_t* epoch) const {
    auto it = cars_.find(ref);
    if (it == cars_.end()) return false;
    const Car& c = it->second;
    if (auth) *auth = c.role == Role::Authority ? c.sent : c.auth;
    if (epoch) *epoch = c.epoch;
    return c.role == Role::Authority ? c.haveSent : c.haveAuth;
}

void Book::OnDamage(const Update& u, int from, double now) {
    Car& c = cars_[u.ref];
    if (c.seen == 0) c.seen = now;
    ++stats_.received;
    if (c.role == Role::Authority && c.entity) {
        // two games drive the same car at once (docs/protocol.md "Conflicts"): take the other one's damage too
        if (u.epoch > c.epoch) c.epoch = u.epoch;
        if (MergeUp(c.raise, u)) c.haveRaise = true;
        return;
    }
    if (u.epoch > c.epoch || (c.haveAuth == false && u.epoch == c.epoch && u.repaired)) {
        c.epoch = u.epoch;
        c.auth = FromUpdate(u);
        c.haveAuth = u.full;
        return;
    }
    if (u.epoch < c.epoch) return;  // from before a repair
    MergeUp(c.auth, u);
    if (u.full) c.haveAuth = true;
    (void)from;
}

void Book::OnReport(const Update& u, int from, double now) {
    Car& c = cars_[u.ref];
    if (c.seen == 0) c.seen = now;
    ++stats_.reportsIn;
    if (c.role != Role::Authority || !c.entity) return;  // not ours (any more): its new driver's copy reports to him
    if (u.epoch < c.epoch) return;                       // taken before our last repair
    if (MergeUp(c.raise, u)) c.haveRaise = true;
    (void)from;
}

void Book::OnRequest(const CarRef& ref, int from) {
    Car& c = cars_[ref];
    c.wantFull.insert(from);
}

void Book::OnPlayerLeft(int id) {
    for (auto& [ref, c] : cars_) {
        c.wantFull.erase(id);
        if (c.authPeer == id) c.authPeer = -1;
    }
}

void Book::Round(const std::vector<CarInput>& cars, double now, Outbox& out) {
    stats_.cars = stats_.authority = stats_.copies = stats_.damaged = 0;
    for (const CarInput& in : cars) {
        Car& c = cars_[in.ref];
        const bool rebound = c.entity != in.entity;
        if (c.role != in.role) {
            char buf[160];
            if (in.role == Role::Authority) {
                // we drive it now: everyone has at least what the last authority told them
                if (c.haveAuth) {
                    c.sent = c.auth;
                    c.haveSent = true;
                }
                c.havePrev = false;
                std::snprintf(buf, sizeof buf, "car %s: our game holds its damage now (epoch %u)", in.ref.Text().c_str(),
                              c.epoch);
            } else {
                if (c.haveSent) {
                    c.auth = c.sent;
                    c.haveAuth = true;
                }
                c.appliedEpoch = c.epoch;
                c.appliedEpochSet = true;
                c.done.Clear();
                c.reported = c.auth;
                c.excessAt = -1;
                std::snprintf(buf, sizeof buf, "car %s: player %d's game holds its damage now", in.ref.Text().c_str(),
                              in.authPeer);
            }
            if (c.entity) out.logs.emplace_back(buf);
        }
        c.role = in.role;
        if (c.authPeer != in.authPeer) {
            c.reported = c.auth;  // a new authority has not heard our reports
            c.requestAt = -1e9;
        }
        c.authPeer = in.authPeer;
        c.entity = in.entity;
        c.seen = now;
        ++stats_.cars;
        if (in.role == Role::Authority)
            ++stats_.authority;
        else
            ++stats_.copies;
        if (!in.read) continue;
        if (in.local.Damaged()) ++stats_.damaged;
        // a copy that burns here (the holder's fire shown on it, or a fire of its own): its countdown is held, it
        // never blows up by itself (ExplodeOnce)
        if (in.role == Role::Copy && (in.local.flags & kBurning) && !(in.local.flags & kExploded))
            out.holdFire.push_back(in.entity);
        if (in.role == Role::Authority)
            AuthorityRound(in, c, rebound, now, out);
        else
            CopyRound(in, c, rebound, now, out);
    }
    for (auto it = cars_.begin(); it != cars_.end();) {
        if (now - it->second.seen > cfg_.forget)
            it = cars_.erase(it);
        else
            ++it;
    }
}

void Book::AuthorityRound(const CarInput& in, Car& c, bool rebound, double now, Outbox& out) {
    const Snapshot& local = in.local;
    const std::string ref = in.ref.Text();
    char buf[320];
    if (rebound && c.haveSent) {
        // another entity for the same car (our own car back from behind a copy, a new copy we drive now): it gets
        // the damage everyone has been shown
        Applied d;
        Apply a = Plan(local, c.sent, d);
        if (!a.Empty()) {
            a.entity = in.entity;
            std::snprintf(buf, sizeof buf, "car %s: our entity %u for it is new here: given its damage (%s; %zu parts, "
                                           "%zu wheels%s%s)",
                          ref.c_str(), in.entity, c.sent.Describe().c_str(), a.parts.size(), a.wheels.size(),
                          a.burn ? ", burning" : "", a.explode ? ", explode" : "");
            out.logs.emplace_back(buf);
            out.trace.push_back("CDA ref=" + ref + " ent=" + std::to_string(in.entity) + " why=rebind parts=" +
                                PartsText(a.parts) + " wheels=" + std::to_string(a.wheels.size()) +
                                " burn=" + (a.burn ? "1" : "0") + " explode=" + (a.explode ? "1" : "0"));
            ++stats_.applied;
            out.applies.push_back(std::move(a));
        }
        c.havePrev = false;
    }
    if (c.haveRaise) {
        Applied d;
        Apply a = Plan(local, c.raise, d);
        c.haveRaise = false;
        c.raise = Snapshot{};
        if (!a.Empty()) {
            a.entity = in.entity;
            std::snprintf(buf, sizeof buf, "car %s: damage another game's copy took is put on our car (%zu parts, %zu "
                                           "wheels%s%s)",
                          ref.c_str(), a.parts.size(), a.wheels.size(), a.burn ? ", burning" : "",
                          a.explode ? ", explode" : "");
            out.logs.emplace_back(buf);
            out.trace.push_back("CDA ref=" + ref + " ent=" + std::to_string(in.entity) + " why=report parts=" +
                                PartsText(a.parts) + " wheels=" + std::to_string(a.wheels.size()) +
                                " burn=" + (a.burn ? "1" : "0") + " explode=" + (a.explode ? "1" : "0"));
            ++stats_.applied;
            out.applies.push_back(std::move(a));
        }
    }
    if (c.havePrev && !rebound && Repaired(c.prev, local)) {
        ++c.epoch;
        ++stats_.repairs;
        Update u = Full(in.ref, c.epoch, local);
        u.repaired = true;
        std::snprintf(buf, sizeof buf, "car %s: repaired on our game (was %s; now %s): epoch %u, everyone repairs theirs",
                      ref.c_str(), c.prev.Describe().c_str(), local.Describe().c_str(), c.epoch);
        out.logs.emplace_back(buf);
        out.trace.push_back("CDT ref=" + ref + " kind=repair ep=" + std::to_string(c.epoch) +
                            " n=" + std::to_string(u.parts.size()) + " flags=" + std::to_string(u.flags));
        out.broadcast.push_back(std::move(u));
        ++stats_.sent;
        ++stats_.fulls;
        c.sent = local;
        c.haveSent = true;
        c.fullAt = now;
        c.wantFull.clear();
        c.prev = local;
        c.havePrev = true;
        return;
    }
    if (!c.haveSent) {
        c.sent = local;
        c.haveSent = true;
        if (local.Damaged()) {
            out.broadcast.push_back(Full(in.ref, c.epoch, local));
            out.trace.push_back("CDT ref=" + ref + " kind=first ep=" + std::to_string(c.epoch) +
                                " n=" + std::to_string(out.broadcast.back().parts.size()));
            ++stats_.sent;
            ++stats_.fulls;
            c.fullAt = now;
            std::snprintf(buf, sizeof buf, "car %s: its damage from our game: %s", ref.c_str(), local.Describe().c_str());
            out.logs.emplace_back(buf);
        }
    } else {
        Update u;
        u.ref = in.ref;
        u.epoch = c.epoch;
        u.flags = local.flags;
        u.burnLeft = local.burnLeft;
        u.layout = local.layout;
        u.partCount = static_cast<uint8_t>(std::min(local.parts.size(), kMaxParts));
        u.wheels = local.wheels;
        u.parts = Diff(c.sent, local);
        const bool layoutChanged = c.sent.layout != local.layout || c.sent.parts.size() != local.parts.size();
        const bool changed = !u.parts.empty() || layoutChanged || c.sent.wheels != local.wheels ||
                             c.sent.flags != local.flags;
        if (changed) {
            if (layoutChanged) u = Full(in.ref, c.epoch, local);
            std::snprintf(buf, sizeof buf, "car %s: damage changed on our game: %s (%zu parts sent: %s)", ref.c_str(),
                          local.Describe().c_str(), u.parts.size(), PartsText(u.parts, 8).c_str());
            out.logs.emplace_back(buf);
            out.trace.push_back("CDT ref=" + ref + " kind=" + (layoutChanged ? "full" : "diff") + " ep=" +
                                std::to_string(c.epoch) + " n=" + std::to_string(u.parts.size()) + " flags=" +
                                std::to_string(u.flags) + " parts=" + PartsText(u.parts));
            for (const auto& [i, v] : u.parts)
                if (i < c.sent.parts.size() && !layoutChanged) c.sent.parts[i] = v;
            if (layoutChanged) c.sent = local;
            c.sent.wheels = local.wheels;
            c.sent.flags = local.flags;
            c.sent.burnLeft = local.burnLeft;
            out.broadcast.push_back(std::move(u));
            ++stats_.sent;
        } else if ((local.Damaged() || c.sent.Damaged()) && now - c.fullAt >= cfg_.fullEvery) {
            out.broadcast.push_back(Full(in.ref, c.epoch, c.sent));
            out.trace.push_back("CDT ref=" + ref + " kind=keyframe ep=" + std::to_string(c.epoch) +
                                " n=" + std::to_string(out.broadcast.back().parts.size()));
            ++stats_.sent;
            ++stats_.fulls;
            c.fullAt = now;
        }
    }
    for (int peer : c.wantFull) {
        out.to.emplace_back(peer, Full(in.ref, c.epoch, c.sent));
        out.trace.push_back("CDT ref=" + ref + " kind=answer to=" + std::to_string(peer) + " ep=" +
                            std::to_string(c.epoch) + " n=" + std::to_string(out.to.back().second.parts.size()));
        ++stats_.sent;
        ++stats_.fulls;
    }
    c.wantFull.clear();
    c.prev = local;
    c.havePrev = true;
}

void Book::CopyRound(const CarInput& in, Car& c, bool rebound, double now, Outbox& out) {
    const Snapshot& local = in.local;
    const std::string ref = in.ref.Text();
    char buf[320];
    if (rebound) {
        c.done.Clear();
        c.excessAt = -1;
        c.quietExcess = 0;
        c.reported = c.auth;
        c.layoutWarned = false;
        c.fresh = true;
    }
    if (!c.haveAuth) {
        if (in.authPeer >= 0 && now - c.requestAt >= cfg_.requestEvery) {
            c.requestAt = now;
            out.requests.emplace_back(in.authPeer, in.ref);
            out.trace.push_back("CDQ ref=" + ref + " to=" + std::to_string(in.authPeer));
            ++stats_.requests;
        }
        return;
    }
    if (now < c.waitUntil) return;
    if (local.layout != c.auth.layout && !c.layoutWarned && !c.auth.parts.empty()) {
        c.layoutWarned = true;
        std::snprintf(buf, sizeof buf, "car %s: our copy's parts differ from the authority's (%zu vs %zu parts, layout "
                                       "%08x vs %08x): only tyres and the explosion are synced",
                      ref.c_str(), local.parts.size(), c.auth.parts.size(), local.layout, c.auth.layout);
        out.logs.emplace_back(buf);
    }
    // repairs: the authority's car was repaired (a newer epoch), or this entity has damage the car does not (a stale
    // entity: our own car back from behind a copy after the driver had it repaired)
    Update excess = Excess(in.ref, c.epoch, local, c.auth);
    bool repair = false;
    const char* why = "";
    const char* reason = "";
    const int over = Over(excess, c.auth);
    if (!c.appliedEpochSet) {
        c.appliedEpochSet = true;
        c.appliedEpoch = c.epoch;
    }
    if (c.appliedEpoch != c.epoch) {
        repair = true;
        why = "repaired on the authority's game";
        reason = "epoch";
    } else if (c.fresh && over >= kTolerance * 4) {
        repair = true;
        why = "this entity has damage the car does not";
        reason = "stale";
    } else if (in.quiet && !excess.parts.empty()) {
        c.quietExcess = over;
        if (over >= cfg_.quietRepairMin && now - c.quietRepairAt >= cfg_.quietRepairGap) {
            repair = true;
            why = "it took damage while it was moved by SetPos";
            reason = "quiet";
            c.quietRepairAt = now;
        }
    }
    c.fresh = false;
    if (repair && !(local.flags & kExploded)) {
        Apply a;
        a.entity = in.entity;
        a.repair = true;
        a.copy = true;
        out.applies.push_back(a);
        c.appliedEpoch = c.epoch;
        c.done.Clear();
        c.waitUntil = now + cfg_.repairWait;
        c.excessAt = -1;
        c.reported = c.auth;
        ++stats_.repairs;
        std::snprintf(buf, sizeof buf, "car %s: our copy is repaired (%s; it had %s), then set to the authority's damage "
                                       "(%s)",
                      ref.c_str(), why, local.Describe().c_str(), c.auth.Describe().c_str());
        out.logs.emplace_back(buf);
        out.trace.push_back("CDA ref=" + ref + " ent=" + std::to_string(in.entity) + " why=repair reason=" + reason + " ep=" + std::to_string(c.epoch));
        return;
    }
    c.appliedEpoch = c.epoch;
    Apply a = Plan(local, c.auth, c.done);
    if (!a.Empty()) {
        a.entity = in.entity;
        a.copy = true;
        std::snprintf(buf, sizeof buf, "car %s: damage set on our copy (%zu parts: %s; %zu wheels%s%s): the authority "
                                       "has %s",
                      ref.c_str(), a.parts.size(), PartsText(a.parts, 8).c_str(), a.wheels.size(),
                      a.burn ? ", burning" : "", a.explode ? ", blown up" : "", c.auth.Describe().c_str());
        out.logs.emplace_back(buf);
        out.trace.push_back("CDA ref=" + ref + " ent=" + std::to_string(in.entity) + " why=sync parts=" +
                            PartsText(a.parts) + " wheels=" + std::to_string(a.wheels.size()) +
                            " burn=" + (a.burn ? "1" : "0") + " explode=" + (a.explode ? "1" : "0"));
        if (a.explode) ++stats_.explosions;
        ++stats_.applied;
        out.applies.push_back(std::move(a));
    }
    // our copy's own damage: reported to the authority once it stood for a moment (not while the copy is moved by
    // SetPos: bumps from that are no real damage; tyres, the fire and the explosion are reported even then)
    Update rep = excess;
    if (in.quiet) rep.parts.clear();
    if (in.authPeer < 0 || Empty(rep)) {
        c.excessAt = -1;
        return;
    }
    // only what we did not report yet
    std::vector<std::pair<uint8_t, uint8_t>> fresh;
    for (const auto& [i, v] : rep.parts) {
        const uint8_t was = i < c.reported.parts.size() && c.reported.layout == local.layout ? c.reported.parts[i] : 0;
        if (v == kGone ? was != kGone : (was != kGone && v > was + kTolerance)) fresh.emplace_back(i, v);
    }
    bool freshWheel = false;
    for (size_t i = 0; i < rep.wheels.size(); ++i) {
        const uint8_t was = i < c.reported.wheels.size() ? c.reported.wheels[i] : kWheelOk;
        if (rep.wheels[i] > was) freshWheel = true;
    }
    const bool freshFlags = (rep.flags & static_cast<uint8_t>(~c.reported.flags)) != 0;
    if (fresh.empty() && !freshWheel && !freshFlags) {
        c.excessAt = -1;
        return;
    }
    if (c.excessAt < 0) {
        c.excessAt = now;
        return;
    }
    if (now - c.excessAt < cfg_.settle) return;
    c.excessAt = -1;
    rep.parts = fresh;
    MergeUp(c.reported, rep);
    std::snprintf(buf, sizeof buf, "car %s: our copy took damage of its own (%zu parts: %s; %s%s%s): reported to "
                                   "player %d's game",
                  ref.c_str(), rep.parts.size(), PartsText(rep.parts, 8).c_str(), freshWheel ? "tyres, " : "",
                  (rep.flags & kBurning) ? "burning, " : "", (rep.flags & kExploded) ? "exploded" : "", in.authPeer);
    out.logs.emplace_back(buf);
    out.trace.push_back("CDX ref=" + ref + " to=" + std::to_string(in.authPeer) + " ep=" + std::to_string(c.epoch) +
                        " parts=" + PartsText(rep.parts) + " wheels=" + (freshWheel ? "1" : "0") +
                        " flags=" + std::to_string(rep.flags));
    out.reports.emplace_back(in.authPeer, std::move(rep));
    ++stats_.reports;
}

}  // namespace mc::game::car_damage
