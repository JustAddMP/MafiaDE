#pragma once
// Co-op connection management, driven from Mod::Tick on the render thread.
// Modes (from launcher args / MafiaCoop.ini, see README-dev.md):
//   --host                 host: UDP on --port (27960) plus a Steam lobby (P2P) when Steam is available
//   --join <ip[:port]>     guest over UDP
//   --join-lobby <id>      guest over Steam (also "+connect_lobby <id>" on the game's command line)
//   --transport udp|steam  restrict the host to one transport; with steam, --join <id> is a lobby id
//   --name <name>          player name (default: Steam persona name, else the Windows user name)
// Steam: the game memory-loads its own (old) steam_api, so we LoadLibrary the on-disk steam_api64.dll, call its
// SteamAPI_Init a few seconds after the first frame and pump its SteamAPI_RunCallbacks ourselves every frame.
#include "render/overlay.h"

#include <string>
#include <vector>

namespace mc {

class Coop {
public:
    static void Init();
    static void Tick(float dt);
    static void Shutdown();  // leave the session cleanly (process exit)
    // Lines for the on-screen status panel (empty = draw nothing).
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

} // namespace mc
