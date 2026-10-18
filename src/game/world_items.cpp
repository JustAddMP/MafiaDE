#include "game/world_items.h"

#include "net/wire.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <utility>

namespace mc::game::items {
namespace {

constexpr size_t kMaxStatesBytes = 1000;                       // payload limit (as NpcStates)
constexpr size_t kMaxItemBytes = 4 + 1 + 12 + 1 + kMaxName;  // one item with the longest name

float Dist(float ax, float ay, float az, float bx, float by, float bz) {
    const float dx = ax - bx, dy = ay - by, dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float Dist(const Item& a, const Item& b) { return Dist(a.x, a.y, a.z, b.x, b.y, b.z); }

bool NameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '.';
}

}  // namespace

bool SharedType(uint8_t type) { return type == kTypeWeapon || type == kTypeInventory; }

std::string SafeName(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (out.size() >= kMaxName) break;
        out += NameChar(c) ? c : '_';
    }
    return out.empty() ? "-" : out;
}

bool ParseItem(const std::string& fields, Item& it) {
    std::istringstream in(fields);
    double key = 0;
    unsigned type = 0;
    std::string name;
    in >> key >> type >> it.x >> it.y >> it.z;
    if (in.fail()) return false;
    in >> name;
    if (!(key > 0 && key <= 4294967295.0) || type > 255 || !std::isfinite(it.x) || !std::isfinite(it.y) ||
        !std::isfinite(it.z))
        return false;
    it.key = static_cast<uint32_t>(key);
    it.type = static_cast<uint8_t>(type);
    it.name = SafeName(name);
    return true;
}

std::string FormatItem(const Item& it) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%u %u %.2f %.2f %.2f %s", it.key, static_cast<unsigned>(it.type), it.x, it.y, it.z,
                  SafeName(it.name).c_str());
    return buf;
}

void WriteItem(net::ByteWriter& w, const Item& it) {
    w.U32(it.key);
    w.U8(it.type);
    w.F32(it.x);
    w.F32(it.y);
    w.F32(it.z);
    w.Str(SafeName(it.name));
}

bool ReadItem(net::ByteReader& r, Item& it) {
    it.key = r.U32();
    it.type = r.U8();
    it.x = r.F32();
    it.y = r.F32();
    it.z = r.F32();
    std::string name = r.Str(kMaxName);
    if (r.Failed() || it.key == 0 || !std::isfinite(it.x) || !std::isfinite(it.y) || !std::isfinite(it.z)) return false;
    it.name = SafeName(name);
    return true;
}

std::vector<std::vector<uint8_t>> PackStates(const std::vector<Item>& items) {
    std::vector<std::vector<uint8_t>> out;
    for (size_t at = 0; at < items.size();) {
        net::ByteWriter w;
        w.U8(0);  // count, patched below
        size_t n = 0;
        for (; at + n < items.size() && n < kPerPacket && w.Size() + kMaxItemBytes <= kMaxStatesBytes; ++n)
            WriteItem(w, items[at + n]);
        std::vector<uint8_t> buf = w.Take();
        buf[0] = static_cast<uint8_t>(n);
        at += n;
        out.push_back(std::move(buf));
    }
    return out;
}

bool CollectibleName(const std::string& s) {
    static const std::string kPrefix = "lh_pickup_";
    if (s.size() <= kPrefix.size() || s.size() > 64 || s.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

const char* GoneName(GoneReason r) {
    switch (r) {
    case GoneReason::Taken: return "taken";
    case GoneReason::Forget: return "forget";
    }
    return "?";
}

// ---- host ---------------------------------------------------------------------------------------------------------------------
void HostBook::Reset() {
    recs_.clear();
    gones_.clear();
    cmds_.clear();
    havePlayer_ = false;
    stats_ = {};
}

void HostBook::OnRound(double now, float px, float py, float pz, const std::vector<Item>& items) {
    px_ = px, py_ = py, pz_ = pz;
    havePlayer_ = true;
    for (auto& [key, r] : recs_) r.inRound = false;
    for (const Item& it : items) {
        if (!SharedType(it.type) || it.key == 0) continue;
        Rec& r = recs_[it.key];
        if (r.taken) continue;  // hidden for the guest who took it; the entity may linger a round
        r.it = it;
        r.seen = now;
        r.inRound = true;
        r.dist = Dist(it.x, it.y, it.z, px, py, pz);
    }
    Expire(now);
}

void HostBook::OnGone(double now, uint32_t id) {
    auto f = recs_.find(id);
    if (f == recs_.end() || f->second.taken) return;  // not ours to share, or hidden by us for a guest
    Rec& r = f->second;
    if (havePlayer_ && Dist(r.it.x, r.it.y, r.it.z, px_, py_, pz_) <= kTakeDist) {
        r.taken = true;
        r.by = kHost;
        r.takenAt = now;
        ++stats_.ours;
        gones_.push_back({id, kHost, GoneReason::Taken, kNobody});
        return;
    }
    // vanished away from our player: the game removed it (despawn distance, a script); no take
    ++stats_.forgotten;
    gones_.push_back({id, kNobody, GoneReason::Forget, kNobody});
    recs_.erase(f);
}

void HostBook::OnTake(double now, PlayerId from, uint32_t key) {
    if (from == kHost || from == kNobody || key == 0) return;
    auto f = recs_.find(key);
    if (f == recs_.end()) {
        // we never had it (or forgot it): nobody else can have it either. His; everyone else hides theirs.
        Rec& r = recs_[key];
        r.it.key = key;
        r.taken = true;
        r.by = from;
        r.takenAt = now;
        ++stats_.unknownTakes;
        gones_.push_back({key, from, GoneReason::Taken, kNobody});
        return;
    }
    Rec& r = f->second;
    if (r.taken) {
        if (r.by != from) {  // somebody was first: only he keeps it
            ++stats_.refused;
            gones_.push_back({key, r.by, GoneReason::Taken, from});
        }
        return;
    }
    r.taken = true;
    r.by = from;
    r.takenAt = now;
    ++stats_.guests;
    Cmd c;
    c.kind = Cmd::Kind::Hide;
    c.id = key;
    c.why = "taken by a guest";
    cmds_.push_back(c);
    gones_.push_back({key, from, GoneReason::Taken, kNobody});
}

void HostBook::Expire(double now) {
    for (auto it = recs_.begin(); it != recs_.end();) {
        const Rec& r = it->second;
        const bool drop = r.taken ? now - r.takenAt > kKeepTaken : now - r.seen > kForgetLocal;
        it = drop ? recs_.erase(it) : std::next(it);
    }
}

std::vector<Item> HostBook::States() const {
    std::vector<const Rec*> list;
    for (const auto& [key, r] : recs_)
        if (!r.taken && r.inRound) list.push_back(&r);
    std::sort(list.begin(), list.end(), [](const Rec* a, const Rec* b) { return a->dist < b->dist; });
    std::vector<Item> out;
    for (size_t i = 0; i < list.size() && i < kMaxSend; ++i) out.push_back(list[i]->it);
    return out;
}

std::vector<Gone> HostBook::TakeGones() { return std::exchange(gones_, {}); }
std::vector<Cmd> HostBook::TakeCmds() { return std::exchange(cmds_, {}); }

size_t HostBook::Count() const {
    size_t n = 0;
    for (const auto& [key, r] : recs_) n += r.taken ? 0 : 1;
    return n;
}

bool HostBook::Taken(uint32_t key, PlayerId* by) const {
    auto f = recs_.find(key);
    if (f == recs_.end() || !f->second.taken) return false;
    if (by) *by = f->second.by;
    return true;
}

// ---- guest --------------------------------------------------------------------------------------------------------------------
void GuestBook::Reset() {
    host_.clear();
    local_.clear();
    done_.clear();
    pending_.clear();
    takes_.clear();
    cmds_.clear();
    lastHostState_ = -1e9;
    havePlayer_ = false;
    stats_ = {};
}

void GuestBook::OnHostItem(double now, const Item& it) {
    if (!SharedType(it.type) || it.key == 0) return;
    auto d = done_.find(it.key);
    if (d != done_.end()) {
        if (now < d->second) return;  // taken or given up: a late (unreliable) state
        done_.erase(d);
    }
    HostRec& h = host_[it.key];
    if (h.local && h.it.type != it.type) Unpair(it.key);  // another item under the same id (reused slot)
    h.it = it;
    h.t = now;
    lastHostState_ = now;
}

void GuestBook::Unpair(uint32_t hostKey) {
    auto f = host_.find(hostKey);
    if (f == host_.end()) return;
    if (f->second.local) {
        auto l = local_.find(f->second.local);
        if (l != local_.end() && l->second.host == hostKey) l->second.host = 0;
    }
    f->second.local = 0;
    f->second.movedX = f->second.movedY = f->second.movedZ = 1e9f;
}

void GuestBook::OnGone(double now, uint32_t key, PlayerId by, GoneReason reason) {
    auto p = pending_.find(key);
    if (reason == GoneReason::Taken && p != pending_.end()) {
        if (by == self_) {
            ++stats_.confirmed;
        } else {  // somebody was first: what we got with it goes back
            ++stats_.lost;
            if (!p->second.gained.empty()) {
                Cmd c;
                c.kind = Cmd::Kind::Undo;
                c.names = p->second.gained;
                c.why = "player " + std::to_string(by) + " took it first";
                cmds_.push_back(c);
            }
        }
        pending_.erase(p);
    }
    auto f = host_.find(key);
    if (f != host_.end()) {
        const uint32_t loc = f->second.local;
        if (reason == GoneReason::Taken && loc && by != self_) {
            auto l = local_.find(loc);
            if (l != local_.end() && !l->second.hidden) {
                l->second.hidden = true;
                ++stats_.hiddenForHost;
                Cmd c;
                c.kind = Cmd::Kind::Hide;
                c.id = loc;
                c.why = by == kNobody ? "taken" : "taken by player " + std::to_string(by);
                cmds_.push_back(c);
            }
        }
        Unpair(key);
        host_.erase(key);
    }
    done_[key] = now + kKeepTaken;
}

void GuestBook::OnRound(double now, float px, float py, float pz, double worldAge, const std::vector<Item>& locals) {
    px_ = px, py_ = py, pz_ = pz;
    havePlayer_ = true;
    for (const Item& it : locals) {
        if (!SharedType(it.type) || it.key == 0) continue;
        auto f = local_.find(it.key);
        if (f != local_.end() && f->second.it.type != it.type) {  // the slot holds another item now
            if (f->second.host) Unpair(f->second.host);
            local_.erase(f);
            f = local_.end();
        }
        const float d = Dist(it.x, it.y, it.z, px, py, pz);
        if (f == local_.end()) {
            LocalRec r;
            r.firstSeen = now;
            r.newDrop = worldAge > kNewAfterLoad;
            r.ownDrop = d <= kOwnDrop;
            f = local_.emplace(it.key, r).first;
        }
        f->second.it = it;
        f->second.t = now;
        f->second.dist = d;
    }
    Expire(now);
    Pair(now);
    // our new weapon drops that the host's world does not have (no partner in time): hidden
    const bool hostTalks = now - lastHostState_ <= kHostQuiet;
    for (auto& [id, l] : local_) {
        if (!hostTalks || l.hidden || l.host || !l.newDrop || l.ownDrop || l.it.type != kTypeWeapon) continue;
        if (l.t != now || l.dist > kHideRange || now - l.firstSeen < kHideWait) continue;
        l.hidden = true;
        ++stats_.hidden;
        Cmd c;
        c.kind = Cmd::Kind::Hide;
        c.id = id;
        c.why = "a new drop the host's world does not have";
        cmds_.push_back(c);
    }
}

void GuestBook::Expire(double now) {
    for (auto it = host_.begin(); it != host_.end();) {
        if (now - it->second.t > kRelease) {
            Unpair(it->first);
            it = host_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = local_.begin(); it != local_.end();) {
        if (now - it->second.t > kForgetLocal) {
            if (it->second.host) Unpair(it->second.host);
            it = local_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = done_.begin(); it != done_.end();) it = now >= it->second ? done_.erase(it) : std::next(it);
    for (auto it = pending_.begin(); it != pending_.end();)
        it = now - it->second.t > kKeepTaken ? pending_.erase(it) : std::next(it);  // no answer: keep what we got
}

void GuestBook::Pair(double now) {
    struct Cand {
        float d;
        uint32_t hostKey, localId;
    };
    std::vector<Cand> cands;
    for (auto& [hk, h] : host_) {
        if (h.local) continue;
        for (auto& [lid, l] : local_) {
            if (l.host || l.hidden || l.it.type != h.it.type) continue;
            const float d = Dist(h.it, l.it);
            const bool sameName = h.it.Named() && l.it.Named() && h.it.name == l.it.name;
            if (d <= kMatchDist || (sameName && d <= kLooseDist)) cands.push_back({d, hk, lid});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.d != b.d ? a.d < b.d : (a.hostKey != b.hostKey ? a.hostKey < b.hostKey : a.localId < b.localId);
    });
    for (const Cand& c : cands) {
        HostRec& h = host_[c.hostKey];
        LocalRec& l = local_[c.localId];
        if (h.local || l.host) continue;
        h.local = c.localId;
        l.host = c.hostKey;
        ++stats_.pairs;
    }
    // paired items lie where the host's lies
    for (auto& [hk, h] : host_) {
        if (!h.local) continue;
        LocalRec& l = local_[h.local];
        if (l.hidden) continue;
        if (Dist(h.it, l.it) <= kMoveDist) continue;
        if (Dist(h.it.x, h.it.y, h.it.z, h.movedX, h.movedY, h.movedZ) <= kMoveDist) continue;  // asked already
        h.movedX = h.it.x, h.movedY = h.it.y, h.movedZ = h.it.z;
        ++stats_.moves;
        Cmd c;
        c.kind = Cmd::Kind::Move;
        c.id = h.local;
        c.x = h.it.x, c.y = h.it.y, c.z = h.it.z;
        c.why = "to the host's item " + std::to_string(hk);
        cmds_.push_back(c);
    }
    (void)now;
}

GuestBook::LocalGone GuestBook::OnLocalGone(double now, uint32_t id, const std::string& gained) {
    auto f = local_.find(id);
    if (f == local_.end()) return LocalGone::Unknown;
    LocalRec l = f->second;
    local_.erase(f);
    if (l.hidden) return LocalGone::OursHidden;
    if (!l.host) return LocalGone::LocalOnly;
    const uint32_t key = l.host;
    Unpair(key);
    if (havePlayer_ && Dist(l.it.x, l.it.y, l.it.z, px_, py_, pz_) <= kTakeDist) {
        ++stats_.takes;
        auto h = host_.find(key);
        Take t;
        t.key = key;
        t.type = l.it.type;
        t.x = l.it.x, t.y = l.it.y, t.z = l.it.z;
        takes_.push_back(t);
        pending_[key] = {gained, now};
        if (h != host_.end()) host_.erase(h);
        done_[key] = now + kKeepTaken;  // its late states must not pair it with another item of ours
        return LocalGone::Taken;
    }
    return LocalGone::Despawned;  // our game removed it (despawn distance): the host's stays, unpaired here
}

std::vector<Take> GuestBook::TakeTakes() { return std::exchange(takes_, {}); }
std::vector<Cmd> GuestBook::TakeCmds() { return std::exchange(cmds_, {}); }

uint32_t GuestBook::PartnerOf(uint32_t hostKey) const {
    auto f = host_.find(hostKey);
    return f == host_.end() ? 0 : f->second.local;
}

uint32_t GuestBook::HostOf(uint32_t localId) const {
    auto f = local_.find(localId);
    return f == local_.end() ? 0 : f->second.host;
}

bool GuestBook::Hidden(uint32_t localId) const {
    auto f = local_.find(localId);
    return f != local_.end() && f->second.hidden;
}

size_t GuestBook::Paired() const {
    size_t n = 0;
    for (const auto& [k, h] : host_) n += h.local ? 1 : 0;
    return n;
}

size_t GuestBook::Unshown() const {
    size_t n = 0;
    for (const auto& [k, h] : host_) n += h.local ? 0 : 1;
    return n;
}

}  // namespace mc::game::items
