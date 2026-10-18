#include "game/npc_owner.h"

#include "net/wire.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace mc::game::npcown {

// ---- wire ---------------------------------------------------------------------------------------------------------------
bool SafeWeapon(const std::string& s) {
    if (s.empty() || s.size() > 48) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}

bool ParseState(const std::string& fields, NpcState& s) {
    std::istringstream in(fields);
    double key = 0, tag = 0;  // u32 (Lua writes them as integers)
    unsigned hp = 0, flags = 0, ai = 0, shots = 0;
    std::string weapon, profile;
    in >> key >> s.x >> s.y >> s.z >> s.hx >> s.hy >> hp >> flags >> ai >> tag >> weapon >> profile >> shots;
    if (!in && !in.eof()) return false;
    if (!(key >= 0 && key <= 4294967295.0) || !std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.z))
        return false;
    s.key = static_cast<uint32_t>(key);
    s.hx = std::isfinite(s.hx) ? std::clamp(s.hx, -1.0f, 1.0f) : 0.0f;
    s.hy = std::isfinite(s.hy) ? std::clamp(s.hy, -1.0f, 1.0f) : 0.0f;
    s.hp = static_cast<uint16_t>(std::min(hp, 65535u));
    s.flags = static_cast<uint8_t>(flags & 0xFF);
    s.ai = static_cast<uint8_t>(ai & 0xFF);
    s.tag = tag >= 0 && tag <= 4294967295.0 ? static_cast<uint32_t>(tag) : 0u;
    s.weapon = SafeWeapon(weapon) ? weapon : "-";
    s.profile = profile.empty() ? 0 : _strtoui64(profile.c_str(), nullptr, 10);
    s.shots = static_cast<uint8_t>(shots & 0xFF);
    return true;
}

std::string FormatState(const NpcState& s) {
    char buf[208];
    std::snprintf(buf, sizeof buf, "%u %.2f %.2f %.2f %.3f %.3f %u %u %u %u %s %llu %u", s.key, s.x, s.y, s.z, s.hx,
                  s.hy, static_cast<unsigned>(s.hp), static_cast<unsigned>(s.flags), static_cast<unsigned>(s.ai),
                  s.tag, SafeWeapon(s.weapon) ? s.weapon.c_str() : "-", static_cast<unsigned long long>(s.profile),
                  static_cast<unsigned>(s.shots));
    return buf;
}

void WriteState(net::ByteWriter& w, const NpcState& s) {
    w.U32(s.key);
    w.F32(s.x);
    w.F32(s.y);
    w.F32(s.z);
    w.I8(static_cast<int8_t>(std::lround(std::clamp(s.hx, -1.0f, 1.0f) * 127)));
    w.I8(static_cast<int8_t>(std::lround(std::clamp(s.hy, -1.0f, 1.0f) * 127)));
    w.U16(s.hp);
    w.U8(s.flags);
    w.U8(s.ai);
    w.U32(s.tag);
    w.Str(SafeWeapon(s.weapon) ? s.weapon : "-");
    w.U64(s.profile);
    w.U8(s.shots);
}

bool ReadState(net::ByteReader& r, NpcState& s) {
    s.key = r.U32();
    s.x = r.F32();
    s.y = r.F32();
    s.z = r.F32();
    s.hx = r.I8() / 127.0f;
    s.hy = r.I8() / 127.0f;
    s.hp = r.U16();
    s.flags = r.U8();
    s.ai = r.U8();
    s.tag = r.U32();
    s.weapon = r.Str(48);
    if (!SafeWeapon(s.weapon)) s.weapon = "-";
    s.profile = r.U64();
    s.shots = r.U8();
    return !r.Failed() && std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.z);
}

std::vector<std::vector<uint8_t>> PackStates(const std::vector<NpcState>& states) {
    std::vector<std::vector<uint8_t>> out;
    for (size_t at = 0; at < states.size();) {
        net::ByteWriter w;
        w.U8(0);  // count, patched below
        size_t n = 0;
        for (; at + n < states.size() && n < kPerPacket && w.Size() + kMaxNpcBytes <= kMaxStatesBytes; ++n)
            WriteState(w, states[at + n]);
        std::vector<uint8_t> buf = w.Take();
        buf[0] = static_cast<uint8_t>(n);
        at += n;
        out.push_back(std::move(buf));
    }
    return out;
}

const char* EndName(EndReason r) {
    switch (r) {
    case EndReason::Died: return "died";
    case EndReason::HandedBack: return "handed back";
    case EndReason::Gone: return "gone";
    case EndReason::Refused: return "claim refused";
    }
    return "?";
}

namespace {
float Dist(const NpcState& a, const NpcState& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}  // namespace

// ---- relay keys -----------------------------------------------------------------------------------------------------------
uint32_t RelayKey(PlayerId from, uint32_t key) {
    if ((key & 0xFFu) != kHumanType || key >= (1u << 24) || from < 1 || from > 0x0F) return 0;
    return (key & ~0xFFu) | (kRelayTypeBase + from);
}

bool IsRelayKey(uint32_t k) { return k < (1u << 24) && (k & 0xFFu) > kRelayTypeBase; }

bool DecodeRelayKey(uint32_t k, PlayerId& from, uint32_t& key) {
    if (!IsRelayKey(k)) return false;
    from = static_cast<PlayerId>((k & 0xFFu) - kRelayTypeBase);
    key = (k & ~0xFFu) | kHumanType;
    return true;
}

// ---- guest --------------------------------------------------------------------------------------------------------------
void GuestOwner::SetCopies(std::vector<uint32_t> ids) {
    copies_ = std::set<uint32_t>(ids.begin(), ids.end());
    // a copy is never ours (an id the game reused for a copy): whatever we had under it ends
    for (auto it = recs_.begin(); it != recs_.end();) {
        if (copies_.count(it->first)) {
            if (it->second.owned) End(it->first, it->second, EndReason::Gone, 0);
            it = recs_.erase(it);
        } else {
            ++it;
        }
    }
}

void GuestOwner::End(uint32_t key, Rec& r, EndReason why, uint32_t hostKey, bool byHost) {
    if (r.owned) {
        GuestEnd e;
        e.key = key;
        e.reason = why;
        e.hostKey = hostKey;
        e.hp = r.last.hp;
        e.byHost = byHost;
        ends_.push_back(e);
        changes_.push_back({key, false});
        if (why == EndReason::HandedBack) ++totalBack_;
    }
    r.owned = false;
    if (why == EndReason::Died) r.ended = true;
}

void GuestOwner::Observe(double now, const std::vector<LocalNpc>& npcs) {
    std::vector<std::pair<float, NpcState>> send;
    size_t owned = OwnedCount();
    std::vector<const LocalNpc*> order;  // nearest first: they become ours first when the cap is reached
    order.reserve(npcs.size());
    for (const LocalNpc& n : npcs) order.push_back(&n);
    std::stable_sort(order.begin(), order.end(), [](const LocalNpc* a, const LocalNpc* b) { return a->dist < b->dist; });
    for (const LocalNpc* np : order) {
        const LocalNpc& n = *np;
        if (copies_.count(n.s.key)) continue;  // feedback guard: a copy of a host NPC is the host's
        Rec& r = recs_[n.s.key];
        r.lastSeen = now;
        r.last = n.s;
        if (n.hostKey != 0) {  // paired with a host NPC: the host's game has it, the host decides
            if (r.owned) {
                End(n.s.key, r, EndReason::HandedBack, n.hostKey);
                --owned;
            }
            r.unpairedSince = -1;
            continue;
        }
        if (n.s.Dead()) {
            if (r.owned) {
                End(n.s.key, r, EndReason::Died, 0, now - r.hostHitT <= kHostHitTime);
                --owned;
            }
            r.ended = true;  // never ours later (a body)
            continue;
        }
        if (r.ended) continue;
        if (r.unpairedSince < 0) r.unpairedSince = now;
        if (!r.owned && n.s.ai != 0 && now - r.unpairedSince >= kOwnWait && owned < kMaxOwned) {
            r.owned = true;
            ++owned;
            ++totalOwned_;
            changes_.push_back({n.s.key, true});
        }
        if (r.owned) send.push_back({n.dist, n.s});
    }
    for (auto it = recs_.begin(); it != recs_.end();) {
        Rec& r = it->second;
        const double away = now - r.lastSeen;
        if (r.owned && away > kGoneAfter) End(it->first, r, EndReason::Gone, 0);
        if (away > kForget) {
            it = recs_.erase(it);
        } else {
            if (away > 0 && !r.owned) r.unpairedSince = -1;  // out of range: its wait starts again
            ++it;
        }
    }
    states_.clear();
    for (size_t i = 0; i < send.size() && i < kMaxSend; ++i) states_.push_back(send[i].second);
}

void GuestOwner::HostHit(double now, uint32_t key) {
    auto it = recs_.find(key);
    if (it != recs_.end()) it->second.hostHitT = now;
}

bool GuestOwner::Claim(uint32_t key, uint32_t) const { return Owned(key); }

void GuestOwner::Refuse(uint32_t key, uint32_t hostKey) {
    auto it = recs_.find(key);
    GuestEnd e;
    e.key = key;
    e.reason = EndReason::Refused;
    e.hostKey = hostKey;
    e.hp = it != recs_.end() ? it->second.last.hp : 0;
    ends_.push_back(e);  // it stays ours
}

bool GuestOwner::Owned(uint32_t key) const {
    auto it = recs_.find(key);
    return it != recs_.end() && it->second.owned;
}

size_t GuestOwner::OwnedCount() const {
    size_t n = 0;
    for (const auto& [k, r] : recs_) n += r.owned ? 1 : 0;
    return n;
}

void GuestOwner::Reset() {
    for (auto& [k, r] : recs_)
        if (r.owned) changes_.push_back({k, false});
    recs_.clear();
    states_.clear();
    ends_.clear();
}

// ---- host ---------------------------------------------------------------------------------------------------------------
void HostMirrors::OnState(double now, PlayerId from, const NpcState& s) {
    lastNow_ = now;
    if (from == 0) return;  // the host owns nothing of its own here
    auto k = std::make_pair(from, s.key);
    auto it = byKey_.find(k);
    if (it == byKey_.end()) {
        if (s.Dead()) return;  // a mirror starts alive
        auto e = ended_.find(k);  // a late state (unreliable) after its end (reliable): not a new mirror
        if (e != ended_.end() && now < e->second) return;
        size_t n = 0;
        for (const auto& [mid, m] : byMid_) n += (m.from == from && m.deadAt < 0) ? 1 : 0;
        if (n >= kMaxPerGuest) return;
        const uint32_t mid = nextMid_++;
        Mirror m;
        m.from = from;
        m.key = s.key;
        byMid_[mid] = m;
        it = byKey_.emplace(k, mid).first;
    }
    Mirror& m = byMid_[it->second];
    if (m.deadAt >= 0) return;  // dead stays dead
    m.last = s;
    m.t = now;
    MirrorCmd c;
    c.kind = MirrorCmd::Kind::State;
    c.mid = it->second;
    c.s = s;
    cmds_.push_back(c);
    if (s.Dead()) {
        m.deadAt = now;
        MirrorCmd d;
        d.kind = MirrorCmd::Kind::Died;
        d.mid = it->second;
        cmds_.push_back(d);
    }
}

void HostMirrors::Drop(uint32_t mid, EndReason why, uint32_t hostKey, uint16_t hp) {
    auto it = byMid_.find(mid);
    if (it == byMid_.end()) return;
    MirrorCmd c;
    c.kind = MirrorCmd::Kind::End;
    c.mid = mid;
    c.reason = why;
    c.hostKey = hostKey;
    c.hp = hp;
    cmds_.push_back(c);
    byKey_.erase({it->second.from, it->second.key});
    ended_[{it->second.from, it->second.key}] = lastNow_ + kEndedMemory;
    byMid_.erase(it);
}

void HostMirrors::OnEnd(double now, PlayerId from, const GuestEnd& e) {
    lastNow_ = now;
    auto it = byKey_.find({from, e.key});
    if (it == byKey_.end()) return;
    const uint32_t mid = it->second;
    Mirror& m = byMid_[mid];
    switch (e.reason) {
    case EndReason::Died:
        if (m.deadAt < 0) {
            m.deadAt = now;
            m.last.flags |= 1u;
            m.last.hp = 0;
            MirrorCmd d;
            d.kind = MirrorCmd::Kind::Died;
            d.mid = mid;
            cmds_.push_back(d);
        }
        break;
    case EndReason::HandedBack:
        if (e.hostKey) claimed_[e.hostKey] = now + kClaimedMemory;
        Drop(mid, EndReason::HandedBack, e.hostKey, e.hp);
        break;
    case EndReason::Gone:
        Drop(mid, EndReason::Gone, 0, e.hp);
        break;
    case EndReason::Refused:
        refused_[{mid, e.hostKey}] = now + kRefuseMemory;
        if (m.pendingHost == e.hostKey) {
            claimed_.erase(e.hostKey);
            m.pendingHost = 0;
        }
        break;
    }
}

void HostMirrors::OnPlayerLeft(PlayerId from) {
    std::vector<uint32_t> mids;
    for (const auto& [mid, m] : byMid_)
        if (m.from == from) mids.push_back(mid);
    for (uint32_t mid : mids) Drop(mid, EndReason::Gone, 0, 0);
}

void HostMirrors::SetCopies(std::vector<uint32_t> ids) { copies_ = std::set<uint32_t>(ids.begin(), ids.end()); }

void HostMirrors::ObserveNatives(double now, const std::vector<NpcState>& natives) {
    lastNow_ = now;
    for (const NpcState& s : natives) {
        if (copies_.count(s.key)) continue;  // feedback guard
        Native& n = natives_[s.key];
        if (n.lastSeen == 0) n.firstSeen = now;
        n.lastSeen = now;
        n.s = s;
    }
    for (auto it = natives_.begin(); it != natives_.end();)
        it = now - it->second.lastSeen > kNativeForget ? natives_.erase(it) : std::next(it);
    for (auto it = claimed_.begin(); it != claimed_.end();) it = now > it->second ? claimed_.erase(it) : std::next(it);
    for (auto it = refused_.begin(); it != refused_.end();) it = now > it->second ? refused_.erase(it) : std::next(it);

    struct Cand {
        float score, dist;
        bool sameSpot;
        uint32_t mid, native;
    };
    std::vector<Cand> cands;
    for (const auto& [mid, m] : byMid_) {
        if (m.pendingHost) continue;
        const bool dead = m.deadAt >= 0;
        if (!dead && now - m.t > kRelease) continue;
        for (const auto& [nk, n] : natives_) {
            if (n.lastSeen != now || n.s.Dead() || n.s.ai == 0 || now - n.firstSeen < kClaimWait) continue;
            if (claimed_.count(nk) || refused_.count({mid, nk}) || copies_.count(nk)) continue;
            const float d = Dist(m.last, n.s);
            const bool kindOk = m.last.tag == 0 || n.s.tag == 0 || m.last.tag == n.s.tag;
            const bool sameTag = m.last.tag != 0 && m.last.tag == n.s.tag;
            if (d <= kMatchDist && kindOk)
                cands.push_back({d, d, true, mid, nk});
            else if (sameTag && d <= kClaimDist && std::fabs(m.last.z - n.s.z) < kClaimDz)
                cands.push_back({d, d, false, mid, nk});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.sameSpot != b.sameSpot) return a.sameSpot;
        if (a.score != b.score) return a.score < b.score;
        return a.mid < b.mid;
    });
    std::set<uint32_t> usedMid, usedNative;
    for (const Cand& c : cands) {
        if (usedMid.count(c.mid) || usedNative.count(c.native)) continue;
        usedMid.insert(c.mid);
        usedNative.insert(c.native);
        Mirror& m = byMid_[c.mid];
        Claim cl;
        cl.to = m.from;
        cl.guestKey = m.key;
        cl.hostKey = c.native;
        cl.mid = c.mid;
        cl.dead = m.deadAt >= 0;
        claims_.push_back(cl);
        ++totalClaims_;
        claimed_[c.native] = now + kClaimedMemory;
        if (cl.dead) {
            // the guest killed this NPC before the host's game spawned it: the host's dies too, the record goes
            MirrorCmd k;
            k.kind = MirrorCmd::Kind::KillNative;
            k.mid = c.mid;
            k.hostKey = c.native;
            cmds_.push_back(k);
            Drop(c.mid, EndReason::HandedBack, c.native, 0);
        } else {
            m.pendingHost = c.native;
            m.pendingT = now;
        }
    }
}

void HostMirrors::Expire(double now) {
    lastNow_ = now;
    for (auto it = ended_.begin(); it != ended_.end();) it = now > it->second ? ended_.erase(it) : std::next(it);
    std::vector<uint32_t> gone;
    for (auto& [mid, m] : byMid_) {
        if (m.pendingHost && now - m.pendingT > kClaimTimeout) {  // no answer: as if refused
            refused_[{mid, m.pendingHost}] = now + kRefuseMemory;
            claimed_.erase(m.pendingHost);
            m.pendingHost = 0;
        }
        if (m.deadAt >= 0 ? now - m.deadAt > kDeadMemory : now - m.t > kRelease) gone.push_back(mid);
    }
    for (uint32_t mid : gone) Drop(mid, EndReason::Gone, 0, 0);
}

bool HostMirrors::Lookup(uint32_t mid, PlayerId& from, uint32_t& key) const {
    auto it = byMid_.find(mid);
    if (it == byMid_.end()) return false;
    from = it->second.from;
    key = it->second.key;
    return true;
}

std::set<uint32_t> HostMirrors::Duplicates(PlayerId lo, PlayerId hi) const {
    struct Cand {
        float d;
        bool sameSpot;
        uint32_t a, b;  // lo's mid, hi's mid
    };
    std::vector<Cand> cands;
    for (const auto& [ma, a] : byMid_) {
        if (a.from != lo) continue;
        for (const auto& [mb, b] : byMid_) {
            if (b.from != hi) continue;
            const float d = Dist(a.last, b.last);
            const bool kindOk = a.last.tag == 0 || b.last.tag == 0 || a.last.tag == b.last.tag;
            const bool sameTag = a.last.tag != 0 && a.last.tag == b.last.tag;
            if (d <= 2 * kMatchDist && kindOk)
                cands.push_back({d, true, ma, mb});
            else if (sameTag && d <= kClaimDist && std::fabs(a.last.z - b.last.z) < kClaimDz)
                cands.push_back({d, false, ma, mb});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) {
        if (x.sameSpot != y.sameSpot) return x.sameSpot;
        if (x.d != y.d) return x.d < y.d;
        return x.a < y.a || (x.a == y.a && x.b < y.b);
    });
    std::set<uint32_t> usedA, out;
    for (const Cand& c : cands) {
        if (usedA.count(c.a) || out.count(c.b)) continue;
        usedA.insert(c.a);
        out.insert(c.b);
    }
    return out;
}

std::vector<NpcState> HostMirrors::RelayStates(PlayerId to, double now) const {
    std::map<PlayerId, std::set<uint32_t>> dupes;  // guest F > to: F's mirrors that are one of to's own
    std::vector<NpcState> out;
    for (const auto& [mid, m] : byMid_) {
        if (out.size() >= kMaxRelay) break;
        if (m.from == to || m.pendingHost) continue;
        const bool dead = m.deadAt >= 0;
        if (!dead && now - m.t > kRelayFresh) continue;
        if (m.from > to) {
            auto it = dupes.find(m.from);
            if (it == dupes.end()) it = dupes.emplace(m.from, Duplicates(to, m.from)).first;
            if (it->second.count(mid)) continue;
        }
        const uint32_t rk = RelayKey(m.from, m.key);
        if (!rk) continue;
        NpcState s = m.last;
        s.key = rk;
        out.push_back(std::move(s));
    }
    return out;
}

size_t HostMirrors::LiveCount() const {
    size_t n = 0;
    for (const auto& [mid, m] : byMid_) n += m.deadAt < 0 ? 1 : 0;
    return n;
}

void HostMirrors::Reset() {
    for (const auto& [mid, m] : byMid_) {
        MirrorCmd c;
        c.kind = MirrorCmd::Kind::End;
        c.mid = mid;
        c.reason = EndReason::Gone;
        cmds_.push_back(c);
    }
    byMid_.clear();
    byKey_.clear();
    ended_.clear();
    natives_.clear();
    claimed_.clear();
    refused_.clear();
}

}  // namespace mc::game::npcown
