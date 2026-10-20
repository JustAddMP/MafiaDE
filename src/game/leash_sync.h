#pragma once
// The distance leash in the session (docs/host-authority.md section 3; the logic: game/leash.h).
//
//   Host: reads leash_warn_m / leash_max_m / leash_countdown_s from MafiaCoop.ini (defaults 300 / 400 / 10; read every
//         2 s, so a change applies at once) and sends them in LeashConfig 0x0208 to every guest when he joins or comes
//         back, and to everyone when they change. A guest's LeashEvent 0x0209 puts "<NAME> IS FAR AWAY (420 M)" in the
//         host's status panel (not a banner over his game).
//   Guest: every script tick the glue (player_sync.lua M.Leash) reads our spot (our car's when we sit in one), the
//         host's puppet's and what suspends the leash; game/leash.h decides; the banner goes to module.cpp's DrawNotice
//         (after the reconnect and the mission notices); a pull is carried out by M.LeashPull; every warn / countdown /
//         pulled / back goes to the host. Until the host's LeashConfig is here the defaults hold.
//   Trace LSH (docs/sync-trace.md) on every change of state or suspension and on each pull.
//
// Threads: messages and the banner on the render thread, the leash on GameMainThread (tick callback); a mutex between.
#include "net/session.h"
#include "render/overlay.h"

#include <string>
#include <vector>

namespace mc::game {

class LeashSync {
public:
    static void Install();  // once, after the Lua bridge (after PlayerSync: its glue holds M.Leash)

    // render thread
    static void Attach(net::Session& session);
    static void Detach();
    static void Tick(float dt);
    // The banner over the game ("" none): "FAR FROM THE HOST - THE STREETS THIN OUT (340 M)", ...
    static std::string HudNotice();
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
