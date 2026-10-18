#include "game/latency.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

#include "core/trace.h"

namespace mc::game::latency {
namespace {

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Pending {
    double recvAt;
    float ageMs;
    net::PlayerId from;
};

struct Sys {
    std::deque<Pending> pending;
    uint32_t count = 0;              // applied, for the 1-in-4 sampling of streams
    std::vector<float> window;       // apply latencies since the last Summary
};

struct State {
    std::mutex mu;
    net::Session* session = nullptr;  // render thread only
    std::array<Sys, kCount> sys;
};
State& S() {
    static State s;
    return s;
}

bool IsStream(System s) { return s == kPlayer || s == kNpc || s == kTraffic || s == kItem; }

constexpr size_t kMaxPending = 512;  // a system whose tick does not run (paused) keeps only the newest
constexpr size_t kMaxWindow = 4096;

}  // namespace

const char* Name(System s) {
    static const char* const kNames[kCount] = {"player", "world",  "npc",     "npc-event", "traffic",
                                               "traffic-event", "door", "explosion", "car-damage", "police",
                                               "item",   "item-event", "hit"};
    return s < kCount ? kNames[s] : "?";
}

System SystemOf(uint16_t id) {
    if (id == 0x0100) return kPlayer;
    if (id == 0x0101 || id == 0x0102 || id == 0x0400) return kHit;
    if (id >= 0x0200 && id <= 0x020F) return kWorld;
    if (id == 0x0210 || id == 0x0420) return kNpc;
    if (id == 0x0211 || id == 0x0410 || (id >= 0x0421 && id <= 0x042F)) return kNpcEvent;
    if (id == 0x0222) return kTraffic;
    if (id >= 0x0220 && id <= 0x022F) return kTrafficEvent;
    if (id >= 0x0230 && id <= 0x023F) return kDoor;
    if (id >= 0x0240 && id <= 0x024F) return kExplosion;
    if (id >= 0x0250 && id <= 0x025F) return kCarDamage;
    if (id >= 0x0260 && id <= 0x026F) return kPolice;
    if (id == 0x0270) return kItem;
    if (id >= 0x0271 && id <= 0x027F) return kItemEvent;
    return kCount;
}

void Configure(net::Session& s) {
    using net::Priority;
    // Lanes: systems whose reliable messages need no order with the others' (a lost or big message of one does not
    // hold back another). Lane 0 (default): players, NPCs, traffic cars, car damage, explosions, hits - they refer
    // to each other's cars and NPCs, so they stay in one order.
    s.SetLane(0x0200, 0x020F, 1);  // world / mission (the host's checkpoint save is up to 256 KB)
    s.SetLane(0x0230, 0x023F, 2);  // doors
    s.SetLane(0x0260, 0x026F, 3);  // police
    s.SetLane(0x0270, 0x027F, 4);  // items
    // Priorities: what a player does and sees first; bulk that can wait or be thinned when the link is full.
    s.SetPriority(0x0100, 0x0102, Priority::Critical);  // PlayerState, CarParked, PlayerModel
    s.SetPriority(0x0400, 0x040F, Priority::Critical);  // Hit
    s.SetPriority(0x0410, 0x0410, Priority::Critical);  // NpcHit (a guest's shot)
    s.SetPriority(0x0422, 0x0423, Priority::Critical);  // HostNpcHit, NpcClaim
    s.SetPriority(0x0240, 0x024F, Priority::Critical);  // explosions, throws
    s.SetPriority(0x0230, 0x0231, Priority::Critical);  // door event / apply
    s.SetPriority(0x0232, 0x0232, Priority::Bulk);      // door snapshot
    s.SetPriority(0x0271, 0x0273, Priority::Critical);  // item take / gone / collectible
    s.SetPriority(0x0270, 0x0270, Priority::Bulk);      // item states
    s.SetPriority(0x0260, 0x0262, Priority::Critical);  // police state / event / chase
    s.SetPriority(0x0205, 0x0206, Priority::Critical);  // mission fail / verdict
    s.SetPriority(0x0202, 0x0202, Priority::Bulk);      // the host's checkpoint save
    s.SetPriority(0x0211, 0x0211, Priority::Critical);  // NPC death
    s.SetPriority(0x0222, 0x0222, Priority::Bulk);      // traffic car states
    s.SetPriority(0x0210, 0x0210, Priority::Normal);    // NPC states (thinned only when far over the budget)
    s.SetPriority(0x0420, 0x0420, Priority::Normal);    // a guest's own NPC states
    // Car damage (0x0250-0x0252) and everything else: Normal.
}

void Attach(net::Session* session) {
    State& st = S();
    std::lock_guard lock(st.mu);
    if (st.session && st.session != session) st.session->onAppMessage = nullptr;
    st.session = session;
    if (session) {
        Configure(*session);
        session->onAppMessage = [](uint16_t id, net::PlayerId from) {
            const System s = SystemOf(id);
            if (s < kCount) Received(s, from);
        };
    }
    for (Sys& s : st.sys) {
        s.pending.clear();
        s.window.clear();
        s.count = 0;
    }
}

void Received(System s, net::PlayerId from) {
    if (s >= kCount) return;
    State& st = S();
    std::lock_guard lock(st.mu);
    if (!st.session) return;
    const float age = st.session->MessageAgeMs();
    if (age < 0) return;  // not stamped / clocks not synced yet
    Sys& sy = st.sys[s];
    sy.pending.push_back({Now(), age, from});
    while (sy.pending.size() > kMaxPending) sy.pending.pop_front();
}

void Applied(System s) {
    if (s >= kCount) return;
    State& st = S();
    std::lock_guard lock(st.mu);
    Sys& sy = st.sys[s];
    if (sy.pending.empty()) return;
    const double now = Now();
    const bool stream = IsStream(s);
    for (const Pending& p : sy.pending) {
        const float hand = static_cast<float>((now - p.recvAt) * 1000.0);
        const float apply = p.ageMs + hand;
        if (sy.window.size() < kMaxWindow) sy.window.push_back(apply);
        if ((!stream || sy.count % 4 == 0) && core::trace::IsOpen())
            core::trace::Line("LAT", "sys=%s from=%u age=%.1f hand=%.1f apply=%.1f", Name(s), p.from, p.ageMs, hand,
                              apply);
        ++sy.count;
    }
    sy.pending.clear();
}

std::string Summary() {
    State& st = S();
    std::lock_guard lock(st.mu);
    std::string out;
    for (int i = 0; i < kCount; ++i) {
        std::vector<float>& w = st.sys[i].window;
        if (w.empty()) continue;
        std::sort(w.begin(), w.end());
        char b[96];
        std::snprintf(b, sizeof(b), "%s%s %.0f/%.0f/%.0f ms (%zu)", out.empty() ? "" : ", ", Name(static_cast<System>(i)),
                      w[w.size() / 2], w[std::min(w.size() - 1, w.size() * 95 / 100)], w.back(), w.size());
        out += b;
        w.clear();
    }
    return out.empty() ? out : "latency (send -> applied, p50/p95/max): " + out;
}

}  // namespace mc::game::latency
