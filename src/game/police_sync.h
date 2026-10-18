#pragma once
// Phase 4: police in co-op (docs/protocol.md "Police", docs/game-api.md 20). Every player's wanted state (his police
// zone: search or hunt, its level and centre, seen by police, fines and arrests) is his own game's and is broadcast to
// the others (PoliceState, PoliceEvent). The host's police are the ones near the host (a guest near the host sees
// copies of them, traffic_sync / npc_sync): a wanted guest near the host becomes a target of the host's police (his
// puppet: offences announced from it, police near it attack it when he is hunted), and the host tells him while they
// chase him (PoliceChase): his game then holds his wanted level at least at the chase's level and starts a zone of its
// own when the host's police see him and his game has none. Far from the host his game's own police work as always.
// Arrests, fines and losing the police stay with each player's own game. game/police_model.h holds the logic.
// Threads: messages and sending on the render thread; Lua on GameMainThread (tick callback); a mutex in between.
#include "net/session.h"
#include "render/overlay.h"

#include <vector>

namespace mc::game {

class PoliceSync {
public:
    static void Install();  // once, after the Lua bridge (after TrafficSync: traffic_sync.lua asks us about chases)
    static void Attach(net::Session& session);
    static void Detach();
    static void OnPlayerLeft(net::PlayerId id);
    static void Tick(float dt);
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
