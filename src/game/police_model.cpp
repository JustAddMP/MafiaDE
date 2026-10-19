#include "game/police_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mc::game::police {

float Dist(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool SameState(const WantedState& a, const WantedState& b) {
    if (a.flags != b.flags || a.level != b.level) return false;
    if (!a.Wanted()) return true;  // no zone: its centre means nothing
    return Dist(a.zone, b.zone) < kZoneMove;
}

std::string Describe(const WantedState& s) {
    if (!(s.flags & kInWorld)) return "not in a world";
    char buf[192];
    if (!s.Wanted()) {
        std::snprintf(buf, sizeof buf, "clean%s%s%s", (s.flags & kSeen) ? " seen" : "",
                      (s.flags & kFining) ? " fining" : "", (s.flags & kArresting) ? " arresting" : "");
        return buf;
    }
    std::snprintf(buf, sizeof buf, "wanted %s level %u%s%s%s%s%s at %.1f,%.1f,%.1f", s.Hunt() ? "hunt" : "search",
                  s.level, (s.flags & kSeen) ? " seen" : "", (s.flags & kDecaying) ? " decaying" : "",
                  (s.flags & kFining) ? " fining" : "", (s.flags & kArresting) ? " arresting" : "",
                  (s.flags & kOffencesOff) ? " offences-off" : "", s.zone.x, s.zone.y, s.zone.z);
    return buf;
}

namespace {

bool Finite(float v, float lim) { return std::isfinite(v) && std::fabs(v) <= lim; }
constexpr float kMaxCoord = 100000.0f;

}  // namespace

void WriteState(net::ByteWriter& w, const WantedState& s, uint16_t seq) {
    w.U8(kStateVersion);
    w.U16(seq);
    w.U8(s.flags);
    w.U8(std::min<uint8_t>(s.level, kMaxLevel));
    w.F32(s.zone.x);
    w.F32(s.zone.y);
    w.F32(s.zone.z);
}

bool ReadState(net::ByteReader& r, WantedState& s, uint16_t& seq) {
    const uint8_t ver = r.U8();
    seq = r.U16();
    WantedState t;
    t.flags = r.U8();
    t.level = r.U8();
    t.zone.x = r.F32();
    t.zone.y = r.F32();
    t.zone.z = r.F32();
    if (r.Failed() || ver < 1 || t.level > kMaxLevel || !Finite(t.zone.x, kMaxCoord) ||
        !Finite(t.zone.y, kMaxCoord) || !Finite(t.zone.z, kMaxCoord))
        return false;
    s = t;
    return true;
}

bool ParseStateLine(const char* rest, WantedState& s) {
    unsigned flags = 0, level = 0;
    float x = 0, y = 0, z = 0;
    if (!rest || std::sscanf(rest, "%u %u %f %f %f", &flags, &level, &x, &y, &z) != 5) return false;
    if (flags > kAllFlags || level > 255 || !Finite(x, kMaxCoord) || !Finite(y, kMaxCoord) || !Finite(z, kMaxCoord))
        return false;
    s.flags = static_cast<uint8_t>(flags);
    s.level = static_cast<uint8_t>(std::min<unsigned>(level, kMaxLevel));
    s.zone = {x, y, z};
    return true;
}

namespace {

constexpr const char* kEventNames[] = {
    "NONE",
    "PLAYER_ENTER_BLUEZONE",
    "PLAYER_EXIT_BLUEZONE",
    "PLAYER_ENTER_REDZONE",
    "PLAYER_EXIT_REDZONE",
    "BLUEZONE_DECAY",
    "POLICE_ZONE_DECAYED",
    "PLAYER_FINE",
    "POLICE_FINE_STARTED",
    "POLICE_FINE_FINISHED",
    "POLICE_ARREST_STARTED",
    "POLICE_ARREST_ESCAPED",
    "POLICE_ARREST_ARRESTED",
    "PLAYER_AREST",
    "PLAYER_DEATH",
    "POLICE_ACTIVITY_ENDED",
    "POLICE_WITNESS_STATE_STARTED",
    "POLICE_REDZONE_STARTED",
    "POLICE_BLUEZONE_STARTED",
    "OFFENDER",
    "OFFENDER_UPDATE",
    "RECOGNIZED_BY_POLICE",
    "POLICE_SEARCH",
    "ARRESTED",
    "LOST",
    "WANTED",
};
static_assert(sizeof(kEventNames) / sizeof(kEventNames[0]) == static_cast<size_t>(Event::Last));

}  // namespace

const char* EventName(Event e) {
    const auto i = static_cast<size_t>(e);
    return i < static_cast<size_t>(Event::Last) ? kEventNames[i] : "?";
}

Event EventFromName(const std::string& name) {
    for (size_t i = 1; i < static_cast<size_t>(Event::Last); ++i)
        if (name == kEventNames[i]) return static_cast<Event>(i);
    return Event::None;
}

bool IsOffenceEvent(Event e) {
    switch (e) {
        case Event::EnterSearch:
        case Event::EnterHunt:
        case Event::RedStarted:
        case Event::BlueStarted:
        case Event::Witness:
        case Event::Offender:
        case Event::OffenderUpdate:
        case Event::BecameWanted:
            return true;
        default:
            return false;
    }
}

void WriteEvent(net::ByteWriter& w, const EventMsg& e) {
    w.U8(static_cast<uint8_t>(e.kind));
    w.U32(e.arg);
}

bool ReadEvent(net::ByteReader& r, EventMsg& e) {
    const uint8_t k = r.U8();
    const uint32_t arg = r.U32();
    if (r.Failed() || k == 0 || k >= static_cast<uint8_t>(Event::Last)) return false;
    e.kind = static_cast<Event>(k);
    e.arg = arg;
    return true;
}

void WriteChase(net::ByteWriter& w, const Chase& c) {
    w.U8(static_cast<uint8_t>((c.active ? 1 : 0) | (c.hunt ? 2 : 0)));
    w.U8(std::min<uint8_t>(c.level, kMaxLevel));
    w.U8(c.cops);
    w.U8(c.seers);
    w.F32(c.nearest);
}

bool ReadChase(net::ByteReader& r, Chase& c) {
    const uint8_t f = r.U8();
    Chase t;
    t.level = r.U8();
    t.cops = r.U8();
    t.seers = r.U8();
    t.nearest = r.F32();
    if (r.Failed() || t.level > kMaxLevel || !Finite(t.nearest, kMaxCoord) || t.nearest < 0) return false;
    t.active = (f & 1) != 0;
    t.hunt = (f & 2) != 0;
    c = t;
    return true;
}

// ---- Sender -------------------------------------------------------------------------------------------------------------
bool Sender::Due(double now, const WantedState& s, bool& reliable) {
    const bool changed = !any_ || !SameState(s, last_);
    if (changed) {
        if (now - lastAt_ < kMinGap) return false;
        reliable = true;
    } else {
        const double beat = s.Wanted() ? kHeartbeatWanted : kHeartbeatClean;
        if (now - lastAt_ < beat) return false;
        reliable = false;
    }
    last_ = s;
    any_ = true;
    lastAt_ = now;
    ++seq_;
    return true;
}

void Sender::Reset() {
    any_ = false;
    lastAt_ = -1e9;
}

Event Transition(const WantedState& before, const WantedState& after) {
    if (!(before.flags & kInWorld) || !(after.flags & kInWorld)) return Event::None;
    if (!before.Wanted() && after.Wanted()) return Event::BecameWanted;
    if (before.Wanted() && !after.Wanted()) return Event::LostPolice;
    return Event::None;
}

// ---- HostPolice -----------------------------------------------------------------------------------------------------------
void HostPolice::OnState(uint8_t id, const WantedState& s, double now) {
    if (id == 0) return;
    Guest& g = guests_[id];
    g.st = s;
    g.stateAt = now;
}

void HostPolice::OnEvent(uint8_t id, const EventMsg& e, double now) {
    if (id == 0 || !IsOffenceEvent(e.kind)) return;
    Guest& g = guests_[id];
    (void)now;
    g.offence = true;
}

void HostPolice::SetPuppet(uint8_t id, const Vec3& pos, double now) {
    if (id == 0) return;
    Guest& g = guests_[id];
    g.puppet = pos;
    g.puppetAt = now;
}

void HostPolice::SetReport(uint8_t id, uint8_t cops, uint8_t seers, float nearest, double now) {
    auto it = guests_.find(id);
    if (it == guests_.end()) return;
    it->second.cops = cops;
    it->second.seers = std::min(seers, cops);
    it->second.nearest = std::isfinite(nearest) && nearest >= 0 ? nearest : 0;
    it->second.reportAt = now;
}

void HostPolice::RemovePeer(uint8_t id) { guests_.erase(id); }

const HostPolice::Guest* HostPolice::Find(uint8_t id) const {
    auto it = guests_.find(id);
    return it == guests_.end() ? nullptr : &it->second;
}

void HostPolice::Tick(double now, std::vector<HostCmd>& cmds, std::vector<ChaseOut>& chases) {
    for (auto& [id, g] : guests_) {
        const bool fresh = now - g.stateAt < kStateStale;
        const bool pupFresh = now - g.puppetAt < kPuppetStale;
        const float d = Dist(host_, g.puppet);
        g.isNear = pupFresh && (g.isNear ? d <= kNearLeave : d <= kNearEnter);
        const bool wanted = fresh && g.st.Wanted() && (g.st.flags & kInWorld);
        if (g.target) {
            if (!g.isNear || !fresh) {
                g.target = false;
            } else if (!wanted) {
                if (g.cleanSince < 0) g.cleanSince = now;
                if (now - g.cleanSince >= kReleaseHold) g.target = false;
            } else {
                g.cleanSince = -1;
            }
            if (!g.target) {
                g.announcedLevel = 0;
                g.announcedHunt = false;
                g.cleanSince = -1;
            }
        } else if (wanted && g.isNear) {
            g.target = true;
            g.targetSince = now;
            g.cleanSince = -1;
            ++g.targets;
        }
        if (g.target) {
            const bool hunt = g.st.Hunt();
            const uint8_t level = std::max<uint8_t>(g.st.level, 1);
            // an offence from his puppet: when he becomes a target, when his level or zone type goes up, and when his
            // game reports a new offence (at most every kAnnounceGap)
            const bool escalated = wanted && (level > g.announcedLevel || (hunt && !g.announcedHunt));
            const bool repeat = g.offence && now - g.announceAt >= kAnnounceGap;
            if ((escalated && now - g.announceAt >= 1.0) || repeat) {
                cmds.push_back({HostCmd::Type::Announce, id, level, hunt});
                g.announceAt = now;
                g.announcedLevel = std::max(g.announcedLevel, level);
                g.announcedHunt = g.announcedHunt || hunt;
                g.offence = false;
                ++g.announces;
            }
            cmds.push_back({HostCmd::Type::Target, id, level, hunt});
        }
        if (!g.target) g.offence = false;
        const bool reportFresh = now - g.reportAt < kReportStale;
        const bool active = g.target && reportFresh && g.cops > 0;
        if (active != g.chaseSent || (active && now - g.chaseAt >= kChaseInterval)) {
            ChaseOut o;
            o.id = id;
            o.chase.active = active;
            o.chase.hunt = active && g.st.Hunt();
            o.chase.level = active ? std::max<uint8_t>(g.st.level, 1) : 0;
            o.chase.cops = active ? g.cops : 0;
            o.chase.seers = active ? g.seers : 0;
            o.chase.nearest = active ? g.nearest : 0;
            o.reliable = active != g.chaseSent;
            chases.push_back(o);
            g.chaseSent = active;
            g.chaseAt = now;
        }
    }
}

// ---- GuestPolice ----------------------------------------------------------------------------------------------------------
const char* ZoneWhyName(ZoneWhy w) {
    switch (w) {
        case ZoneWhy::Seen: return "seen";
        case ZoneWhy::Refresh: return "refresh";
        case ZoneWhy::Revive: return "revive";
    }
    return "?";
}

void GuestPolice::OnChase(const Chase& c, double now) {
    chase_ = c;
    chaseAt_ = now;
    if (c.active) activeAt_ = now;
}

void GuestPolice::SetLocal(const WantedState& own) {
    own_ = own;
    if (own.Wanted() && (own.flags & kInWorld)) lastWanted_ = own;
}

void GuestPolice::OnOwnEvent(Event e, double now) {
    switch (e) {
        case Event::PlayerFine:
        case Event::FineStarted:
        case Event::FineFinished:
        case Event::ArrestStarted:
        case Event::Arrested:
        case Event::PlayerArrest:
        case Event::HumanArrested:
        case Event::PlayerDeath:
            endedAt_ = now;
            break;
        default:
            break;
    }
}

bool GuestPolice::Chased(double now) const { return chase_.active && now - chaseAt_ < kChaseStale; }

void GuestPolice::Tick(double now, std::vector<GuestCmd>& out) {
    const bool active = Chased(now);
    if (active) activeAt_ = now;
    const bool tail = !active && now - activeAt_ < kForceHold;  // the chase ended a moment ago
    const uint8_t level = std::clamp<uint8_t>(chase_.level, 1, kMaxLevel);
    uint8_t want = 0;
    if (active)
        want = level;
    else if (tail)
        want = forced_;
    if (want != forced_) {
        out.push_back({GuestCmd::Type::ForceMin, want, active && chase_.hunt});
        forced_ = want;
    }
    // our zone does not decay while the host's police are on us (and kForceHold after); a red zone follows us while
    // one of them saw us or is close, as it does when our own police see us
    const bool hold = active || (tail && held_);
    const bool follow = hold && active && chase_.hunt && own_.Hunt() &&
                        (chase_.seers > 0 || (chase_.nearest > 0 && chase_.nearest <= kFollowNear));
    if (hold != held_ || follow != follow_ || (hold && now - holdAt_ >= kHoldRefresh)) {
        GuestCmd c;
        c.type = GuestCmd::Type::Hold;
        c.hold = hold;
        c.follow = follow;
        c.refresh = hold == held_ && follow == follow_;
        if (!c.refresh && hold && !held_) ++holds_;
        out.push_back(c);
        held_ = hold;
        follow_ = follow;
        holdAt_ = hold ? now : -1e9;
    }
    if (active && own_.Wanted()) wantedInChase_ = true;
    if (!active && !tail) wantedInChase_ = false;
    if (!(own_.flags & kDecaying)) refreshed_ = false;
    // a zone of our own
    const bool inWorld = (own_.flags & kInWorld) != 0;
    const bool offOk = !(own_.flags & kOffencesOff);
    const bool policeEnded = now - endedAt_ < kEndedKeep;  // fined, arrested or dead: the zone ended for good
    if (!active || !inWorld || now - zoneAt_ < kZoneRetry) return;
    GuestCmd z;
    z.type = GuestCmd::Type::Zone;
    z.level = level;
    z.hunt = chase_.hunt;
    bool start = false;
    if (!own_.Wanted() && chase_.seers > 0) {
        z.why = ZoneWhy::Seen;
        start = true;
    } else if (!own_.Wanted() && wantedInChase_ && chase_.cops > 0 && offOk && !policeEnded &&
               !(lastWanted_.flags & (kFining | kArresting))) {
        z.why = ZoneWhy::Revive;
        z.level = std::max(level, std::clamp<uint8_t>(lastWanted_.level, 1, kMaxLevel));
        z.hunt = chase_.hunt || lastWanted_.Hunt();
        start = true;
    } else if (own_.Wanted() && (own_.flags & kDecaying) && !refreshed_ && chase_.cops > 0 && offOk &&
               !(own_.flags & (kFining | kArresting))) {
        z.why = ZoneWhy::Refresh;
        z.level = std::max(level, std::clamp<uint8_t>(own_.level, 1, kMaxLevel));
        z.hunt = chase_.hunt || own_.Hunt();
        refreshed_ = true;
        start = true;
    }
    if (!start) return;
    out.push_back(z);
    zoneAt_ = now;
    ++zones_;
}

void GuestPolice::Reset(std::vector<GuestCmd>& out) {
    if (forced_) out.push_back({GuestCmd::Type::ForceMin, 0, false});
    if (held_) {
        GuestCmd c;
        c.type = GuestCmd::Type::Hold;
        out.push_back(c);
    }
    forced_ = 0;
    held_ = follow_ = false;
    holdAt_ = -1e9;
    chase_ = {};
    chaseAt_ = -1e9;
    activeAt_ = -1e9;
    zoneAt_ = -1e9;
    endedAt_ = -1e9;
    wantedInChase_ = refreshed_ = false;
    lastWanted_ = {};
}

}  // namespace mc::game::police
