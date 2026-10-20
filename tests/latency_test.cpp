// latency_test: game/latency.h - which system a message id belongs to, the lanes and send priorities Mafia DE gives
// its messages (docs/protocol.md "Lanes and priorities"), and the LAT bookkeeping (received -> applied) over a real
// loopback session (core stamps every app message with the sender's session clock).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "game/latency.h"
#include "net/session.h"
#include "net/udp_transport.h"

using namespace mc;
using namespace mc::game;

namespace {
int g_fail = 0;
void Check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("%s  %-52s %s\n", ok ? "PASS" : "FAIL", what, detail.c_str());
    if (!ok) ++g_fail;
}
}  // namespace

int main() {
    using latency::SystemOf;
    Check(SystemOf(0x0100) == latency::kPlayer && SystemOf(0x0222) == latency::kTraffic &&
              SystemOf(0x0220) == latency::kTrafficEvent && SystemOf(0x0231) == latency::kDoor &&
              SystemOf(0x0243) == latency::kExplosion && SystemOf(0x0250) == latency::kCarDamage &&
              SystemOf(0x0261) == latency::kPolice && SystemOf(0x0270) == latency::kItem &&
              SystemOf(0x0273) == latency::kItemEvent && SystemOf(0x0202) == latency::kWorld &&
              SystemOf(0x0210) == latency::kNpc && SystemOf(0x0420) == latency::kNpc &&
              SystemOf(0x0423) == latency::kNpcEvent && SystemOf(0x0400) == latency::kHit &&
              SystemOf(0x7000) == latency::kCount,
          "message ids -> systems");
    for (int s = 0; s < latency::kCount; ++s)
        if (!latency::Name(static_cast<latency::System>(s)) || !*latency::Name(static_cast<latency::System>(s)))
            Check(false, "every system has a name");

    net::UdpTransport ht, gt;
    net::SessionConfig cfg;
    net::Session host(ht, cfg), guest(gt, cfg);
    latency::Configure(guest);
    using net::Priority;
    Check(guest.LaneOf(0x0100) == 0 && guest.LaneOf(0x0202) == 1 && guest.LaneOf(0x0232) == 2 &&
              guest.LaneOf(0x0262) == 3 && guest.LaneOf(0x0271) == 4 && guest.LaneOf(0x0250) == 0 &&
              guest.LaneOf(0x0240) == 0 && guest.LaneOf(0x0211) == 0,
          "lanes: world, doors, police, items apart; the rest in one order");
    Check(guest.PriorityOf(0x0100) == Priority::Critical && guest.PriorityOf(0x0242) == Priority::Critical &&
              guest.PriorityOf(0x0231) == Priority::Critical && guest.PriorityOf(0x0232) == Priority::Bulk &&
              guest.PriorityOf(0x0222) == Priority::Bulk && guest.PriorityOf(0x0270) == Priority::Bulk &&
              guest.PriorityOf(0x0272) == Priority::Critical && guest.PriorityOf(0x0261) == Priority::Critical &&
              guest.PriorityOf(0x0202) == Priority::Bulk && guest.PriorityOf(0x0210) == Priority::Normal &&
              guest.PriorityOf(0x0250) == Priority::Normal,
          "priorities: player state and events critical, bulk thinned first");
    Check(guest.LaneOf(0x0208) == 1 && guest.LaneOf(0x0209) == 1 && guest.PriorityOf(0x0208) == Priority::Normal &&
              guest.PriorityOf(0x0209) == Priority::Normal && latency::SystemOf(0x0209) == latency::kWorld,
          "the leash (LeashConfig / LeashEvent): world lane, normal priority");

    // loopback: the host's handler side sees the guest's door event with its age, Applied writes it into the summary
    latency::Attach(&host);
    if (!host.Host(0)) {
        Check(false, "host");
        return 1;
    }
    latency::Configure(guest);
    guest.Join("127.0.0.1:" + std::to_string(ht.BoundPort()));
    int got = 0;
    host.On(0x0230, [&](net::PlayerId, net::ByteReader&) { ++got; });
    // phase 1: the door system's tick runs every loop (in the world): three door events counted in the stats
    latency::Applied(latency::kDoor);
    DWORD until = GetTickCount() + 5000;
    bool sent = false;
    while (GetTickCount() < until && got < 3) {
        host.Poll();
        guest.Poll();
        if (!sent && guest.State() == net::SessionState::Connected && guest.ClockSynced()) {
            const uint8_t b = 1;
            for (int i = 0; i < 3; ++i) guest.SendToHost(0x0230, std::span<const uint8_t>(&b, 1));
            guest.Flush();
            sent = true;
        }
        latency::Applied(latency::kDoor);
        Sleep(1);
    }
    Sleep(5);
    latency::Applied(latency::kDoor);
    const std::string sum = latency::Summary();
    Check(got == 3 && sum.find("door") != std::string::npos && sum.find("(3)") != std::string::npos &&
              sum.find("waited") == std::string::npos,
          "received -> applied: summary per system", sum);
    Check(latency::Summary().empty(), "summary starts over");

    // phase 2: the tick stops (menus / loading screen) while two events arrive, and runs again 1.2 s later: they
    // waited for the game, so they are counted apart and stay out of the latency stats
    const uint8_t b = 1;
    for (int i = 0; i < 2; ++i) guest.SendToHost(0x0230, std::span<const uint8_t>(&b, 1));
    guest.Flush();
    until = GetTickCount() + 1200;
    while (GetTickCount() < until) {
        host.Poll();
        guest.Poll();
        Sleep(1);
    }
    latency::Applied(latency::kDoor);
    const std::string held = latency::Summary();
    Check(got == 5 && held.find("waited for the game's tick (left out): door 2 (max 1.") != std::string::npos &&
              held.find("p50") == std::string::npos,
          "tick stopped: held messages counted apart, not in the stats", held);
    // ticking again: back in the stats
    latency::Applied(latency::kDoor);
    guest.SendToHost(0x0230, std::span<const uint8_t>(&b, 1));
    guest.Flush();
    until = GetTickCount() + 2000;
    while (GetTickCount() < until && got < 6) {
        host.Poll();
        guest.Poll();
        latency::Applied(latency::kDoor);
        Sleep(1);
    }
    latency::Applied(latency::kDoor);
    const std::string again = latency::Summary();
    Check(got == 6 && again.find("door") != std::string::npos && again.find("(1)") != std::string::npos &&
              again.find("waited") == std::string::npos,
          "tick running again: in the stats", again);
    latency::Attach(nullptr);
    Check(!host.onAppMessage, "detached: hook removed");
    std::printf("%s\n", g_fail ? "FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
