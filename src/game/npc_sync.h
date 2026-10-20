#pragma once
// Phase 4 (B3): mission NPCs in co-op, one authority per NPC: the host (docs/protocol.md "NPCs", "Held NPCs",
// docs/host-authority.md, docs/game-api.md 15, the pure logic in npc_replica.h / npc_hold.h, the game side in
// npc_sync.lua):
//   - Host: the authority for its NPCs. Each round (15 Hz) of the Lua glue lists the shared NPCs near any guest (not
//     ambient pedestrians, those are ped_sync's); the publisher (npc_replica.h) sends each guest the ones near him, as
//     often as their distance says, as compact time-stamped states (NpcStates2, slow fields in NpcInfo, NpcLeave), and
//     every NPC death at once (NpcDeath).
//   - Guest: pairs each host NPC with its own NPC (same spot; else the nearest of the same kind within 40 m), stops that
//     NPC's own AI and DRIVES it from the host's states (interpolated, a locomotion controller + correction per replica,
//     a command per tick to the Lua glue, its position reported back); a host NPC without a partner gets a spawned copy,
//     driven the same way. Health and death follow the host's; the guest's hits go to the host (NpcHit), where they are
//     replayed as real shots from the guest's puppet; the guest's damage from host NPCs comes from the host.
//   - Held NPCs (npc_hold.h): an NPC with a job that only the guest's game has (no host NPC pairs with it for 5 s) is
//     never sent anywhere: it is hidden with its AI interrupted until a host NPC pairs with it (then it is that NPC's
//     replica), and shown again when the session ends. (The guest-owned NPCs of 0.15 / 0.16 are retired.)
// Threads: messages on the render thread; Lua on GameMainThread (tick callback); a mutex for the hand-over.
#include "net/session.h"
#include "render/overlay.h"

#include <vector>

namespace mc::game {

class NpcSync {
public:
    static void Install();  // once, after the Lua bridge
    static void Attach(net::Session& session);
    static void Detach();
    static void OnPlayerLeft(net::PlayerId id);  // host: his interest records go
    static void Tick(float dt);
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
