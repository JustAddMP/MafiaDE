#include "game/door_model.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace mc::game::doors {

const char* ActName(Act a) {
    switch (a) {
        case Act::None: return "none";
        case Act::Open: return "open";
        case Act::Close: return "close";
        case Act::Kick: return "kick";
        case Act::Unlock: return "unlock";
        case Act::Lockpick: return "lockpick";
        case Act::Lock: return "lock";
        case Act::Set: return "set";
    }
    return "none";
}

Act DiffAct(uint8_t before, uint8_t after) {
    const uint8_t d = (before ^ after) & kStateMask;
    if (d == 0) return Act::None;
    if (d == kOpen) return (after & kOpen) ? Act::Open : Act::Close;
    if (d == kLocked) return (after & kLocked) ? Act::Lock : Act::Unlock;
    // both: opened and unlocked is an open (opening unlocks), anything else a set
    if ((after & kOpen) && !(after & kLocked)) return Act::Open;
    return Act::Set;
}

uint8_t StateAfter(Act act, uint8_t before, uint8_t want) {
    switch (act) {
        case Act::Open:
        case Act::Kick: return static_cast<uint8_t>((before | kOpen) & ~kLocked);
        case Act::Close: return static_cast<uint8_t>(before & ~kOpen);
        case Act::Unlock:
        case Act::Lockpick: return static_cast<uint8_t>(before & ~kLocked);
        case Act::Lock: return static_cast<uint8_t>((before & ~kOpen) | kLocked);
        case Act::Set: return static_cast<uint8_t>(want & kStateMask);
        case Act::None: break;
    }
    return before;
}

std::string StateName(uint8_t s) {
    std::string out = (s & kOpen) ? "open" : "closed";
    out += (s & kLocked) ? ",locked" : ",unlocked";
    return out;
}

float Dist(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

uint32_t NameHash(const std::string& name) {
    if (name.empty() || name == "-") return 0;
    uint32_t h = 2166136261u;
    for (unsigned char c : name) {
        h ^= static_cast<uint32_t>(std::tolower(c));
        h *= 16777619u;
    }
    return h ? h : 1;  // 0 means "no name"
}

bool Same(const Ref& a, const Ref& b) {
    if (a.name != b.name) return false;
    return Dist(a.pos, b.pos) <= (a.name ? kMatchDist : kMatchDistNoName);
}

const char* VerdictName(Verdict v) {
    switch (v) {
        case Verdict::Unknown: return "unknown";
        case Verdict::Echo: return "echo";
        case Verdict::Send: return "send";
        case Verdict::WorldKept: return "kept";
        case Verdict::Revert: return "revert";
        case Verdict::Same: return "same";
    }
    return "?";
}

const char* ReasonName(Reason r) {
    switch (r) {
        case Reason::Join: return "join";
        case Reason::Reconnect: return "reconnect";
        case Reason::World: return "world";
    }
    return "?";
}

// ---- wire ----------------------------------------------------------------------------------------------------------
namespace {

constexpr float kMaxCoord = 100000.0f;

bool Finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) < kMaxCoord &&
           std::fabs(v.y) < kMaxCoord && std::fabs(v.z) < kMaxCoord;
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

void WriteEvent(net::ByteWriter& w, const Event& e) {
    w.U16(e.seq);
    w.U8(e.by);
    w.U8(static_cast<uint8_t>(e.act));
    w.U8(e.state & kStateMask);
    w.U32(e.door.name);
    WriteVec(w, e.door.pos);
    WriteVec(w, e.from);
}

bool ReadEvent(net::ByteReader& r, Event& e) {
    e = {};
    e.seq = r.U16();
    e.by = r.U8();
    const uint8_t act = r.U8();
    e.state = r.U8();
    e.door.name = r.U32();
    e.door.pos = ReadVec(r);
    e.from = ReadVec(r);
    if (r.Failed() || act == 0 || act > kLastAct || (e.state & ~kStateMask) || !Finite(e.door.pos) || !Finite(e.from))
        return false;
    e.act = static_cast<Act>(act);
    return true;
}

void WriteSnapshot(net::ByteWriter& w, const std::vector<SnapEntry>& list) {
    const size_t n = std::min(list.size(), kMaxSnapshot);
    w.U8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
        w.U32(list[i].door.name);
        WriteVec(w, list[i].door.pos);
        w.U8(list[i].state & kStateMask);
    }
}

bool ReadSnapshot(net::ByteReader& r, std::vector<SnapEntry>& list) {
    list.clear();
    const uint8_t n = r.U8();
    if (r.Failed() || n > kMaxSnapshot) return false;
    for (uint8_t i = 0; i < n; ++i) {
        SnapEntry s;
        s.door.name = r.U32();
        s.door.pos = ReadVec(r);
        s.state = r.U8();
        if (r.Failed() || !Finite(s.door.pos) || (s.state & ~kStateMask)) {
            list.clear();
            return false;
        }
        list.push_back(s);
    }
    return true;
}

void WriteRequest(net::ByteWriter& w, const SnapRequest& q) {
    WriteVec(w, q.pos);
    w.U8(static_cast<uint8_t>(q.reason));
}

bool ReadRequest(net::ByteReader& r, SnapRequest& q) {
    q = {};
    q.pos = ReadVec(r);
    const uint8_t reason = r.U8();
    if (r.Failed() || !Finite(q.pos) || reason < 1 || reason > 3) return false;
    q.reason = static_cast<Reason>(reason);
    return true;
}

// ---- the table -----------------------------------------------------------------------------------------------------
Doors::Memory* Doors::FindMemory(const Ref& r) {
    Memory* best = nullptr;
    float bd = 1e30f;
    for (Memory& m : memory_) {
        if (!Same(m.ref, r)) continue;
        const float d = Dist(m.ref.pos, r.pos);
        if (d < bd) {
            bd = d;
            best = &m;
        }
    }
    return best;
}

const Doors::Memory* Doors::FindMemory(const Ref& r) const { return const_cast<Doors*>(this)->FindMemory(r); }

void Doors::Remember(const Ref& r, uint8_t state, double now, uint8_t by, bool told) {
    Memory* m = FindMemory(r);
    if (!m) {
        if (memory_.size() >= kMaxMemory) {
            auto oldest = std::min_element(memory_.begin(), memory_.end(),
                                           [](const Memory& a, const Memory& b) { return a.at < b.at; });
            memory_.erase(oldest);
        }
        memory_.push_back({});
        m = &memory_.back();
        m->ref = r;
    }
    m->state = state & kStateMask;
    m->at = now;
    m->by = by;
    m->told = told || m->told;
}

bool Doors::Find(const Ref& r, uint32_t* local) const {
    bool found = false;
    float bd = 1e30f;
    for (const auto& [id, d] : doors_) {
        if (!Same(d.ref, r)) continue;
        const float dd = Dist(d.ref.pos, r.pos);
        if (dd < bd) {
            bd = dd;
            found = true;
            if (local) *local = id;
        }
    }
    return found;
}

bool Doors::StateOf(uint32_t local, uint8_t* state) const {
    auto it = doors_.find(local);
    if (it == doors_.end()) return false;
    if (state) *state = it->second.state;
    return true;
}

bool Doors::Remembered(const Ref& r, uint8_t* state) const {
    const Memory* m = FindMemory(r);
    if (!m) return false;
    if (state) *state = m->state;
    return true;
}

const Ref* Doors::RefOf(uint32_t local) const {
    auto it = doors_.find(local);
    return it == doors_.end() ? nullptr : &it->second.ref;
}

void Doors::Push(uint32_t local, Door& d, Act act, uint8_t state, const Vec3& from, uint8_t by, const char* why,
                 double now, std::vector<Apply>& out) {
    Apply a;
    a.local = local;
    a.act = act;
    a.state = state & kStateMask;
    a.from = from;
    a.by = by;
    a.why = why;
    out.push_back(a);
    expect_[local] = {a.state, now + kEchoWindow};
    (void)d;
    ++stats_.applied;
}

void Doors::Seen(uint32_t local, const std::string& name, const Vec3& pos, uint8_t state, double now,
                 std::vector<Apply>& out) {
    Door& d = doors_[local];
    d.ref.name = NameHash(name);
    d.ref.pos = pos;
    d.state = state & kStateMask;
    d.freshAt = now;
    stats_.doors = static_cast<uint32_t>(doors_.size());
    const Memory* m = FindMemory(d.ref);
    if (m && m->state != d.state) {
        ++stats_.fromMemory;
        Push(local, d, Act::Set, m->state, pos, m->by, "memory", now, out);
    }
}

void Doors::Gone(uint32_t local) {
    doors_.erase(local);
    expect_.erase(local);
    stats_.doors = static_cast<uint32_t>(doors_.size());
}

void Doors::Fresh(uint32_t local, double now) {
    auto it = doors_.find(local);
    if (it != doors_.end()) it->second.freshAt = now;
}

Verdict Doors::Changed(uint32_t local, uint8_t state, bool ours, Act act, const Vec3& from, uint8_t localPlayer,
                       double now, Event& ev, std::vector<Apply>& out) {
    auto it = doors_.find(local);
    if (it == doors_.end()) return Verdict::Unknown;
    Door& d = it->second;
    state &= kStateMask;
    const uint8_t before = d.state;
    d.freshAt = now;
    if (state == before) return Verdict::Same;
    d.state = state;
    ++stats_.changes;
    // a change we made for somebody else, showing in our poll (maybe a step of it: a kick unlocks, then opens)
    auto ex = expect_.find(local);
    if (ex != expect_.end()) {
        if (now <= ex->second.until) {
            const uint8_t want = ex->second.state;
            const uint8_t moved = before ^ state;
            if (state == want) {
                expect_.erase(ex);
                ++stats_.echoes;
                return Verdict::Echo;
            }
            if ((moved & ~(before ^ want)) == 0) {  // only bits that had to change, changed: on its way
                ++stats_.echoes;
                return Verdict::Echo;
            }
        }
        expect_.erase(ex);
    }
    if (ours) {
        ++stats_.ours;
        if (act == Act::None || act == Act::Set) act = DiffAct(before, state);
        // a kick or lockpick report that only shows the closing of a door is a close
        if (!(state & kOpen) && (before & kOpen) && (act == Act::Kick || act == Act::Open)) act = Act::Close;
        ev = {};
        ev.seq = ++seq_;
        ev.by = localPlayer;
        ev.act = act == Act::None ? Act::Set : act;
        ev.state = state;
        ev.door = d.ref;
        ev.from = from;
        Remember(d.ref, state, now, localPlayer, true);
        ++stats_.sent;
        return Verdict::Send;
    }
    ++stats_.world;
    if (host_) {
        const Act a = DiffAct(before, state);
        ev = {};
        ev.seq = ++seq_;
        ev.by = kWorld;
        ev.act = a == Act::None ? Act::Set : a;
        ev.state = state;
        ev.door = d.ref;
        ev.from = d.ref.pos;
        Remember(d.ref, state, now, kWorld, true);
        ++stats_.sent;
        return Verdict::Send;
    }
    // guest: his own world changed a door. The lock of a door the host decided is put back (his scripts are behind
    // or ahead of the host's: the host's story decides); opening and closing (NPCs walking through) stay local.
    Memory* m = FindMemory(d.ref);
    if (m && m->told && ((m->state ^ state) & kLocked)) {
        if (now - m->revertFrom > 60.0) {
            m->revertFrom = now;
            m->reverts = 0;
        }
        if (m->reverts < kMaxReverts) {
            ++m->reverts;
            const uint8_t target =
                (m->state & kLocked) ? m->state : static_cast<uint8_t>(state & ~kLocked);
            ++stats_.reverts;
            Push(local, d, Act::Set, target, d.ref.pos, kWorld, "revert", now, out);
            return Verdict::Revert;
        }
    }
    ++stats_.kept;
    return Verdict::WorldKept;
}

void Doors::Remote(const Event& ev, double now, std::vector<Apply>& out) {
    ++stats_.remote;
    Remember(ev.door, ev.state, now, ev.by, true);
    uint32_t local = 0;
    if (!Find(ev.door, &local)) {
        ++stats_.pending;
        return;
    }
    Door& d = doors_[local];
    if (d.state == (ev.state & kStateMask)) {
        expect_.erase(local);
        return;
    }
    Push(local, d, ev.act, ev.state, ev.from, ev.by, "remote", now, out);
}

bool Doors::Accept(const Event& ev, double now, uint8_t* hostState) const {
    uint32_t local = 0;
    if (!Find(ev.door, &local)) return true;  // not loaded here: nothing to decide with
    const Door& d = doors_.at(local);
    if (hostState) *hostState = d.state;
    const bool live = now - d.freshAt <= kFreshFor;
    if (ev.act == Act::Open && (d.state & kLocked) && live) return false;
    return true;
}

std::vector<SnapEntry> Doors::Snapshot(const Vec3& center, float radius, double now, size_t max) const {
    max = std::min(max, kMaxSnapshot);
    struct Cand {
        float d;
        SnapEntry e;
    };
    std::vector<Cand> live, mem;
    for (const auto& [id, d] : doors_) {
        if (now - d.freshAt > kFreshFor) continue;
        const float dd = Dist(d.ref.pos, center);
        if (dd <= radius) live.push_back({dd, {d.ref, d.state}});
    }
    std::sort(live.begin(), live.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
    std::vector<SnapEntry> out;
    for (const Cand& c : live) {
        if (out.size() >= max) break;
        out.push_back(c.e);
    }
    for (const Memory& m : memory_) mem.push_back({Dist(m.ref.pos, center), {m.ref, m.state}});
    std::sort(mem.begin(), mem.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
    for (const Cand& c : mem) {
        if (out.size() >= max) break;
        bool dup = false;
        for (const SnapEntry& e : out)
            if (Same(e.door, c.e.door)) {
                dup = true;
                break;
            }
        if (!dup) out.push_back(c.e);
    }
    return out;
}

void Doors::ApplySnapshot(const std::vector<SnapEntry>& list, double now, std::vector<Apply>& out) {
    ++stats_.snapIn;
    for (const SnapEntry& e : list) {
        Remember(e.door, e.state, now, kWorld, true);
        uint32_t local = 0;
        if (!Find(e.door, &local)) continue;
        Door& d = doors_[local];
        if (d.state == e.state) continue;
        Push(local, d, Act::Set, e.state, e.door.pos, kWorld, "snapshot", now, out);
    }
}

std::vector<uint32_t> Doors::Expire(double now) {
    std::vector<uint32_t> out;
    for (auto it = expect_.begin(); it != expect_.end();) {
        if (now > it->second.until) {
            out.push_back(it->first);
            ++stats_.unmet;
            it = expect_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

void Doors::Clear() {
    doors_.clear();
    memory_.clear();
    expect_.clear();
    stats_.doors = 0;
}

}  // namespace mc::game::doors
