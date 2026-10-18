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
    const DWORD until = GetTickCount() + 5000;
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
        Sleep(1);
    }
    Sleep(5);
    latency::Applied(latency::kDoor);
    const std::string sum = latency::Summary();
    Check(got == 3 && sum.find("door") != std::string::npos, "received -> applied: summary per system", sum);
    Check(latency::Summary().empty(), "summary starts over");
    latency::Attach(nullptr);
    Check(!host.onAppMessage, "detached: hook removed");
    std::printf("%s\n", g_fail ? "FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
