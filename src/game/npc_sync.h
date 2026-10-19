#pragma once
// Phase 4 (B3): mission NPCs in co-op, one authority per NPC (docs/protocol.md "NPCs", "Guest-owned NPCs",
// docs/game-api.md 15, the pure logic in npc_replica.h / npc_owner.h, the game side in npc_sync.lua):
//   - Host: the authority for its NPCs. Each round (15 Hz) of the Lua glue lists the shared NPCs near any guest (not
//     ambient pedestrians, those are ped_sync's); the publisher (npc_replica.h) sends each guest the ones near him, as
//     often as their distance says, as compact time-stamped states (NpcStates2, slow fields in NpcInfo, NpcLeave), and
//     every NPC death at once (NpcDeath).
//   - Guest: pairs each host NPC with its own NPC (same spot; else the nearest of the same kind within 40 m), stops that
//     NPC's own AI and DRIVES it from the host's states (interpolated, a locomotion controller + correction per replica,
//     a command per tick to the Lua glue, its position reported back); a host NPC without a partner gets a spawned copy,
//     driven the same way. Health and death follow the host's; the guest's hits go to the host (NpcHit), where they are
//     replayed as real shots from the guest's puppet; the guest's damage from host NPCs comes from the host.
//   - Guest-owned NPCs (npc_owner.h): NPCs only a guest's game has go guest -> host the same compact way and are
//     shown there as copies driven the same way; the host's hits on them go back to the guest; when the host's own game
//     spawns that NPC, it is paired again and the host is authoritative for it.
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
    static void OnPlayerLeft(net::PlayerId id);  // host: that guest's own NPCs go
    static void Tick(float dt);
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
