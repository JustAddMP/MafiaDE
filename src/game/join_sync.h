#pragma once
// How long a joining or reconnecting guest waits until his world is complete (docs/protocol.md "Join and
// reconnect"). On a guest, from the moment the session became active (a join, or back after a lost connection), every
// system's handler marks the first time it got its part of the world (join::Mark, one line in each handler):
//
//   player     the first PlayerState of every other player (key: his id)
//   mission    the host's MissionState
//   worldenv   the host's WorldEnv (time of day, weather)
//   doors      the host's DoorSnapshot (asked for 3 s after we are in the world)
//   npcs       the first NpcStates (optional: the host may have no NPCs near anyone)
//   traffic    the first TrafficSpawn (optional: only when the host's traffic applies here)
//   police     the first PoliceState of every other player (key: his id)
//   cardamage  the first CarDamage answer for every car copy that asked for one in the window (key: the car)
//
// Trace JN ev=<system> after=<s> per first arrival, then JN ev=complete after=<s> once every required part is here,
// or JN ev=complete after=<s> missing=<...> when something required has not come 15 s after we were in the world (and
// ev=complete late=1 missing=- if it comes later). Log: "join: world complete in X s" / "join: still missing ... after 15 s".
// The guest sends the times to the host in JoinReport 0x0207 (reliable), which logs and traces them too.
//
// Tracker is the game-free logic (tests/join_sync_test.cpp); JoinSync the glue (render thread).
#include "net/session.h"
#include "net/wire.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mc::game {

namespace join {

constexpr uint16_t kMsgJoinReport = 0x0207;  // guest -> host, reliable
constexpr double kWindowS = 15.0;            // s after we are in the world (and the session is active)

enum class System : uint8_t { Player, Mission, WorldEnv, Doors, Npcs, Traffic, Police, CarDamage, kCount };
constexpr size_t kSystems = static_cast<size_t>(System::kCount);

inline const char* Name(System s) {
    static const char* const kNames[kSystems] = {"player", "mission", "worldenv", "doors",
                                                 "npcs",   "traffic", "police",   "cardamage"};
    return static_cast<size_t>(s) < kSystems ? kNames[static_cast<size_t>(s)] : "?";
}
// not waited for: the host may simply have none for us
inline bool Optional(System s) { return s == System::Npcs || s == System::Traffic; }
// one arrival per key is expected (players, cars), not one per system
inline bool PerKey(System s) { return s == System::Player || s == System::Police || s == System::CarDamage; }

// Render thread (every handler): the first arrival of a system's part (key: a player id or a car, "" for one per
// system). Expect: a part that must arrive too (a car copy that asked for its damage). Both are cheap and safe to
// call at any time; outside a join window they do nothing visible.
void Mark(System s, const std::string& key = {});
void Mark(System s, uint32_t key);
void Expect(System s, const std::string& key);

class Tracker {
public:
    struct Event {
        enum Kind { First, Complete, Missing } kind = First;
        System sys = System::Player;
        std::string key;
        double after = 0;      // s since the start of the window (Begin)
        bool late = false;     // complete only after the missing report
        std::string missing;   // Missing: required parts not seen ("doors,player:2")
        std::string none;      // optional systems that sent nothing ("npcs,traffic")
    };

    // The session became inactive (a lost host, the end): nothing is tracked until the next Begin; marks from now
    // on are kept for it (a handler can run in the same Poll that makes the session active, before Begin).
    void Reset() {
        *this = Tracker();
    }
    // The session became active. others: the other players' ids at that moment (each must send his PlayerState and
    // his PoliceState).
    void Begin(double now, bool reconnect, const std::vector<uint32_t>& others) {
        begun_ = true;
        reconnect_ = reconnect;
        begin_ = now;
        for (uint32_t id : others) {
            const std::string k = std::to_string(id);
            Need(System::Player, k);
            Need(System::Police, k);
        }
        Need(System::Mission, {});
        Need(System::WorldEnv, {});
        Need(System::Doors, {});
    }
    // We are in a loaded world (the 15 s start counting from the later of this and Begin).
    void InWorld(double now) {
        if (inWorldAt_ < 0) inWorldAt_ = now;
    }
    void Expect(System s, const std::string& key) {
        if (begun_ && !reported_ && !complete_) Need(s, key);
    }
    void Mark(System s, const std::string& key, double now) {
        const size_t i = static_cast<size_t>(s);
        if (i >= kSystems) return;
        const std::string k = PerKey(s) ? key : std::string();
        auto& seen = seen_[i];
        if (seen.count(k)) return;
        seen[k] = now;
        if (first_[i] < 0 || now < first_[i]) first_[i] = now;
        pending_.push_back({s, k});
    }

    std::vector<Event> Poll(double now) {
        std::vector<Event> out;
        if (!begun_) return out;
        for (const auto& [s, k] : pending_) {
            Event e;
            e.kind = Event::First;
            e.sys = s;
            e.key = k;
            e.after = std::max(0.0, seen_[static_cast<size_t>(s)][k] - begin_);
            out.push_back(e);
        }
        pending_.clear();
        if (complete_) return out;
        if (Missing().empty() && inWorldAt_ >= 0) {
            complete_ = true;
            Event e;
            e.kind = Event::Complete;
            // timed from the later of the join and our world's load: a guest who joins in the menu waits in the
            // lobby and the loading screen first (2026-10-20 test: 50.9 s, nearly all of it the lobby)
            e.after = std::max(0.0, LastRequired() - Start());
            e.late = reported_;
            e.none = NoneSeen();
            out.push_back(e);
            return out;
        }
        if (!reported_ && inWorldAt_ >= 0 && now >= std::max(begin_, inWorldAt_) + kWindowS) {
            reported_ = true;
            Event e;
            e.kind = Event::Missing;
            e.after = now - Start();
            e.missing = Missing();
            e.none = NoneSeen();
            out.push_back(e);
        }
        return out;
    }

    bool Begun() const { return begun_; }
    bool Complete() const { return complete_; }
    bool Reconnect() const { return reconnect_; }
    // s after Begin of a system's first arrival, -1 none
    double FirstAfter(System s) const {
        const double t = first_[static_cast<size_t>(s)];
        return t < 0 || !begun_ ? -1.0 : std::max(0.0, t - begin_);
    }
    // required parts not seen yet: "doors,player:2"
    std::string Missing() const {
        std::string out;
        for (const auto& [s, k] : need_) {
            if (seen_[static_cast<size_t>(s)].count(k)) continue;
            if (!out.empty()) out += ",";
            out += Name(s);
            if (!k.empty()) out += ":" + k;
        }
        return out;
    }

private:
    void Need(System s, const std::string& key) {
        if (!Optional(s)) need_.insert({s, PerKey(s) ? key : std::string()});
    }
    double Start() const { return std::max(begin_, inWorldAt_); }
    double LastRequired() const {
        double last = begin_;
        for (const auto& [s, k] : need_) {
            auto it = seen_[static_cast<size_t>(s)].find(k);
            if (it != seen_[static_cast<size_t>(s)].end()) last = std::max(last, it->second);
        }
        return last;
    }
    std::string NoneSeen() const {
        std::string out;
        for (size_t i = 0; i < kSystems; ++i) {
            if (!Optional(static_cast<System>(i)) || first_[i] >= 0) continue;
            if (!out.empty()) out += ",";
            out += Name(static_cast<System>(i));
        }
        return out;
    }

    bool begun_ = false, reconnect_ = false, complete_ = false, reported_ = false;
    double begin_ = 0, inWorldAt_ = -1;
    std::set<std::pair<System, std::string>> need_;
    std::array<std::map<std::string, double>, kSystems> seen_;
    std::array<double, kSystems> first_ = [] {
        std::array<double, kSystems> a;
        a.fill(-1.0);
        return a;
    }();
    std::vector<std::pair<System, std::string>> pending_;
};

// ---- JoinReport body: u8 flags (1 reconnect, 2 complete, 4 late), u32 afterMs, u8 n, n x (u8 system, i32 ms of its
// first arrival, -1 none), str missing (at most 200 bytes) --------------------------------------------------------------
struct Report {
    bool reconnect = false, complete = false, late = false;
    uint32_t afterMs = 0;
    std::array<int32_t, kSystems> firstMs{};
    std::string missing;
};

inline void WriteReport(net::ByteWriter& w, const Report& r) {
    w.U8(static_cast<uint8_t>((r.reconnect ? 1 : 0) | (r.complete ? 2 : 0) | (r.late ? 4 : 0)));
    w.U32(r.afterMs);
    w.U8(static_cast<uint8_t>(kSystems));
    for (size_t i = 0; i < kSystems; ++i) {
        w.U8(static_cast<uint8_t>(i));
        w.I32(r.firstMs[i]);
    }
    w.Str(r.missing.size() > 200 ? r.missing.substr(0, 200) : r.missing);
}

inline bool ReadReport(net::ByteReader& rd, Report& r) {
    const uint8_t flags = rd.U8();
    r.reconnect = flags & 1;
    r.complete = (flags & 2) != 0;
    r.late = (flags & 4) != 0;
    r.afterMs = rd.U32();
    r.firstMs.fill(-1);
    const uint8_t n = rd.U8();
    if (n > 32) return false;
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t s = rd.U8();
        const int32_t ms = rd.I32();
        if (s < kSystems) r.firstMs[s] = ms;  // systems a newer guest knows and we do not: skipped
    }
    r.missing = rd.Str(200);
    return rd.Ok();
}

}  // namespace join

class JoinSync {
public:
    static void Attach(net::Session& session);
    static void Detach();
    static void Tick(float dt);  // render thread
};

}  // namespace mc::game
