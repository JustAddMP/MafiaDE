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
    double lastTick = 0;             // the system's last Applied call (0: none since Attach)
    uint32_t heldCount = 0;          // since the last Summary: applied after the tick had stopped (kept out of window)
    float heldMaxMs = 0;
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

bool IsStream(System s) { return s == kPlayer || s == kNpc || s == kTraffic || s == kItem || s == kPed; }

constexpr size_t kMaxPending = 512;  // a system whose tick does not run (paused) keeps only the newest
constexpr size_t kMaxWindow = 4096;
// A system's tick that has not run for this long had stopped (a guest in the menus or on a loading screen: WorldEnv
// arrives in the menu and is applied once the world has loaded). What it takes when it runs again waited for the
// game, not for the network or the hand-off: traced as held=1 and counted apart, not in the latency stats.
constexpr double kTickGapS = 1.0;

}  // namespace

const char* Name(System s) {
    static const char* const kNames[kCount] = {"player", "world",  "npc",     "npc-event", "traffic",
                                               "traffic-event", "door", "explosion", "car-damage", "police",
                                               "item",   "item-event", "hit", "ped", "ped-event"};
    return s < kCount ? kNames[s] : "?";
}

System SystemOf(uint16_t id) {
    if (id == 0x0100) return kPlayer;
    if (id == 0x0101 || id == 0x0102 || id == 0x0400) return kHit;
    if (id >= 0x0200 && id <= 0x020F) return kWorld;
    if (id == 0x0210 || id == 0x0420) return kNpc;
    if (id == 0x0212 || id == 0x0424) return kNpc;  // NpcStates2, GuestNpcStates2 (npc-authority)
    if ((id >= 0x0213 && id <= 0x0217) || (id >= 0x0425 && id <= 0x0427)) return kNpcEvent;  // NPC infos, leaves
    if (id == 0x0211 || id == 0x0410 || (id >= 0x0421 && id <= 0x042F)) return kNpcEvent;
    if (id == 0x0222 || id == 0x0226) return kTraffic;  // the host's / a guest's traffic states
    if (id >= 0x0220 && id <= 0x022F) return kTrafficEvent;  // spawn, despawn, bump (0x0224-0x0227: a guest's)
    if (id >= 0x0230 && id <= 0x023F) return kDoor;
    if (id >= 0x0240 && id <= 0x024F) return kExplosion;
    if (id >= 0x0250 && id <= 0x025F) return kCarDamage;
    if (id >= 0x0260 && id <= 0x026F) return kPolice;
    if (id == 0x0270) return kItem;
    if (id >= 0x0271 && id <= 0x027F) return kItemEvent;
    if (id == 0x0280) return kPed;
    if (id >= 0x0281 && id <= 0x0287) return kPedEvent;
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
    s.SetLane(0x0280, 0x0287, 5);  // pedestrians
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
    s.SetPriority(0x0200, 0x0200, Priority::Critical);  // WorldEnv (small, every 2 s: a newcomer's clock at once)
    s.SetPriority(0x0207, 0x0207, Priority::Normal);    // JoinReport (guest -> host, once per join / reconnect)
    s.SetPriority(0x0211, 0x0211, Priority::Critical);  // NPC death
    s.SetPriority(0x0222, 0x0222, Priority::Bulk);      // traffic car states
    s.SetPriority(0x0226, 0x0226, Priority::Bulk);      // a guest's traffic car states
    s.SetPriority(0x0210, 0x0210, Priority::Normal);    // NPC states (thinned only when far over the budget)
    s.SetPriority(0x0420, 0x0420, Priority::Normal);    // a guest's own NPC states
    s.SetPriority(0x0280, 0x0280, Priority::Bulk);      // pedestrian states (thinned first)
    s.SetPriority(0x0283, 0x0284, Priority::Critical);  // pedestrian death / kill
    s.SetPriority(0x0212, 0x0212, Priority::Normal);    // NpcStates2 (rates and budget per guest: npc_replica.h)
    s.SetPriority(0x0213, 0x0214, Priority::Normal);    // NpcInfo, NpcLeave (reliable, lane 0, before the states)
    s.SetPriority(0x0424, 0x0426, Priority::Normal);    // GuestNpcStates2, GuestNpcInfo, GuestNpcLeave
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
        s.lastTick = 0;
        s.heldCount = 0;
        s.heldMaxMs = 0;
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
    const double now = Now();
    // the tick had stopped (or never ran since Attach): everything pending arrived while it was not running
    const bool resumed = sy.lastTick == 0 || now - sy.lastTick > kTickGapS;
    sy.lastTick = now;
    if (sy.pending.empty()) return;
    const bool stream = IsStream(s);
    for (const Pending& p : sy.pending) {
        const float hand = static_cast<float>((now - p.recvAt) * 1000.0);
        const float apply = p.ageMs + hand;
        if (resumed) {
            ++sy.heldCount;
            sy.heldMaxMs = std::max(sy.heldMaxMs, apply);
        } else if (sy.window.size() < kMaxWindow) {
            sy.window.push_back(apply);
        }
        if ((!stream || sy.count % 4 == 0) && core::trace::IsOpen())
            core::trace::Line("LAT", "sys=%s from=%u age=%.1f hand=%.1f apply=%.1f held=%d", Name(s), p.from, p.ageMs,
                              hand, apply, resumed ? 1 : 0);
        ++sy.count;
    }
    sy.pending.clear();
}

std::string Summary() {
    State& st = S();
    std::lock_guard lock(st.mu);
    std::string out, held;
    for (int i = 0; i < kCount; ++i) {
        Sys& sy = st.sys[i];
        char b[96];
        if (sy.heldCount) {
            std::snprintf(b, sizeof(b), "%s%s %u (max %.1f s)", held.empty() ? "" : ", ",
                          Name(static_cast<System>(i)), sy.heldCount, sy.heldMaxMs / 1000.0f);
            held += b;
            sy.heldCount = 0;
            sy.heldMaxMs = 0;
        }
        std::vector<float>& w = sy.window;
        if (w.empty()) continue;
        std::sort(w.begin(), w.end());
        std::snprintf(b, sizeof(b), "%s%s %.0f/%.0f/%.0f ms (%zu)", out.empty() ? "" : ", ", Name(static_cast<System>(i)),
                      w[w.size() / 2], w[std::min(w.size() - 1, w.size() * 95 / 100)], w.back(), w.size());
        out += b;
        w.clear();
    }
    std::string line;
    if (!out.empty()) line = "latency (send -> applied, p50/p95/max): " + out;
    if (!held.empty())
        line += (line.empty() ? "latency: " : "; ") + std::string("waited for the game's tick (left out): ") + held;
    return line;
}

}  // namespace mc::game::latency
