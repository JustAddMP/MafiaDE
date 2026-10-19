#pragma once
// Per-system latency for the sync trace (docs/sync-trace.md "LAT", docs/protocol.md "Latency budget"). Every app
// message carries core's send stamp (the sender's session clock, kept through the host's relay), so its age at
// arrival is known (net::Session::MessageAgeMs). This adds the hand-off to the game's script tick, where the system
// really applies it:
//
//   Received(sys, from)  render thread, for every app message (Attach hooks Session::onAppMessage; SystemOf maps the
//                        message id to its system): age at arrival, now
//   Applied(sys)         GameMainThread, when the system's tick takes what arrived (every message of sys received so
//                        far is applied now)
//
// and writes a trace record per applied message: LAT sys=<name> from=<id> age=<ms> hand=<ms> apply=<ms> held=<0|1>
// (apply = age + hand: the sender's send -> this game applying it). State streams (player, NPC, traffic, item states)
// are written one in four. tools/sync_compare.py turns them into p50 / p95 / max per system. Also kept as running
// stats for the log (Summary, every net_log_interval).
// held=1: the system's tick had not run for over a second (or not yet since Attach) when it took the message - a
// guest in the menus or on a loading screen (WorldEnv arrives in the menu and is applied once the world is loaded).
// That wait is the game's, not the network's: held messages are counted apart (Summary: "waited for the game's
// tick", count and max) and left out of the latency stats, here and in sync_compare.py.
#include <cstdint>
#include <string>

#include "net/session.h"

namespace mc::game::latency {

enum System : uint8_t {
    kPlayer,     // PlayerState 0x0100 (stream)
    kWorld,      // WorldEnv / mission state / cutscene / save / fail 0x0200-0x020F
    kNpc,        // NpcStates 0x0210, guest NPC states 0x0420 (stream)
    kNpcEvent,   // NPC deaths, hits, claims, ends
    kTraffic,    // traffic car states 0x0222 (stream)
    kTrafficEvent,  // traffic spawn / despawn / bump
    kDoor,       // 0x0230-0x0233
    kExplosion,  // 0x0240-0x024F
    kCarDamage,  // 0x0250-0x0252
    kPolice,     // 0x0260-0x0262
    kItem,       // item states 0x0270 (stream)
    kItemEvent,  // item takes, gone, collectibles
    kHit,        // Hit 0x0400, CarParked, PlayerModel
    kPed,        // pedestrian states 0x0280 (stream)
    kPedEvent,   // pedestrian spawn / gone / death / kill 0x0281-0x0287
    kCount
};
const char* Name(System s);
System SystemOf(uint16_t msgId);  // kCount: not one of ours

// Lanes and send priorities of our messages (docs/protocol.md "Lanes and priorities"); on host and guests alike.
void Configure(net::Session& session);

void Attach(net::Session* session);  // OnSessionStart / OnSessionEnd (nullptr)
void Received(System s, net::PlayerId from);
void Applied(System s);
// One log line with p50 / max apply latency per system since the last call ("" when nothing arrived).
std::string Summary();

}  // namespace mc::game::latency
