#include "game/peer_pause.h"

#include <algorithm>
#include <mutex>

namespace mc::game::peer_pause {

const char* ReasonName(Reason r) {
    switch (r) {
    case Reason::None: return "resumed";
    case Reason::NoTick: return "no script tick";
    case Reason::PausedTick: return "the game's paused tick";
    }
    return "?";
}

const char* QuietName(Quiet q) {
    switch (q) {
    case Quiet::None: return "none";
    case Quiet::Paused: return "paused";
    case Quiet::Grace: return "grace";
    }
    return "?";
}

void Write(net::ByteWriter& w, const Msg& m) {
    w.U8(m.paused ? 1 : 0);
    w.U8(static_cast<uint8_t>(m.paused ? m.reason : Reason::None));
    w.U32(m.pausedMs);
}

bool Read(net::ByteReader& r, Msg& m) {
    const uint8_t flags = r.U8();
    const uint8_t reason = r.U8();
    const uint32_t ms = r.U32();
    if (r.Failed()) return false;
    m.paused = (flags & 1) != 0;
    m.reason = m.paused ? (reason == 2 ? Reason::PausedTick : Reason::NoTick) : Reason::None;
    m.pausedMs = ms;
    return true;
}

void LocalDetector::Reset() {
    paused_ = false;
    reason_ = Reason::None;
    since_ = sentAt_ = 0;
    resumedAt_ = -1e9;
}

bool LocalDetector::Update(double now, double lastTickAt, bool pausedTick, double pausedTickSince, bool inWorld,
                           Msg& out) {
    const bool stalled = lastTickAt > 0 && now - lastTickAt > kStallAfter;
    const bool pausedOnly = lastTickAt > 0 && pausedTick && now - pausedTickSince > kStallAfter;
    const bool want = inWorld && (stalled || pausedOnly);
    if (want && !paused_) {
        paused_ = true;
        reason_ = stalled ? Reason::NoTick : Reason::PausedTick;
        since_ = stalled ? lastTickAt : pausedTickSince;
        sentAt_ = now;
        out = {true, reason_, static_cast<uint32_t>(std::max(0.0, now - since_) * 1000.0)};
        return true;
    }
    if (want && paused_) {
        if (now - sentAt_ < kRepeat) return false;
        sentAt_ = now;
        out = {true, reason_, static_cast<uint32_t>(std::max(0.0, now - since_) * 1000.0)};
        return true;
    }
    if (!want && paused_) {
        paused_ = false;
        resumedAt_ = now;
        out = {false, Reason::None, static_cast<uint32_t>(std::max(0.0, now - since_) * 1000.0)};
        reason_ = Reason::None;
        return true;
    }
    return false;
}

bool Peers::Set(uint8_t id, bool paused, double now) {
    Peer& p = peers_[id];
    if (p.paused == paused) return false;
    p.paused = paused;
    if (paused)
        p.since = now;
    else
        p.resumedAt = now;
    return true;
}

Quiet Peers::Get(uint8_t id, double now) const {
    auto it = peers_.find(id);
    if (it == peers_.end()) return Quiet::None;
    if (it->second.paused) return Quiet::Paused;
    return now - it->second.resumedAt < kGrace ? Quiet::Grace : Quiet::None;
}

void Peers::Remove(uint8_t id) { peers_.erase(id); }

void Peers::Clear() {
    peers_.clear();
    localResumedAt_ = -1e9;
}

Quiet Peers::Local(double now) const { return now - localResumedAt_ < kGrace ? Quiet::Grace : Quiet::None; }

namespace {
std::mutex& Mu() {
    static std::mutex* m = new std::mutex;  // never destroyed (threads may still tick during process exit)
    return *m;
}
Peers& Reg() {
    static Peers* p = new Peers;
    return *p;
}
}  // namespace

bool SetRemote(uint8_t id, bool paused, double now) {
    std::lock_guard lock(Mu());
    return Reg().Set(id, paused, now);
}

Quiet Remote(uint8_t id, double now) {
    std::lock_guard lock(Mu());
    return Reg().Get(id, now);
}

void RemoveRemote(uint8_t id) {
    std::lock_guard lock(Mu());
    Reg().Remove(id);
}

void ClearRemotes() {
    std::lock_guard lock(Mu());
    Reg().Clear();
}

void SetLocalResumed(double now) {
    std::lock_guard lock(Mu());
    Reg().LocalResumed(now);
}

Quiet Local(double now) {
    std::lock_guard lock(Mu());
    return Reg().Local(now);
}

Quiet For(uint8_t id, double now) {
    std::lock_guard lock(Mu());
    const Quiet r = Reg().Get(id, now);
    if (r != Quiet::None) return r;  // Paused or Grace (the local side is never Paused: we would not be ticking)
    return Reg().Local(now);
}

}  // namespace mc::game::peer_pause
