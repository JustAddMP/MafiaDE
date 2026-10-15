#pragma once
// Phase 4 (B2 + B5): the story in co-op. The host is authoritative: guests play the host's chapter and checkpoint,
// see his objectives and his cutscenes (docs/protocol.md "Phase 4", docs/game-api.md 14).
//
//   - Host: reports its chapter (quest), checkpoint (quest custom state), objective and the cutscene that plays
//     (Lua, mission_sync.lua) and sends its newest story checkpoint save file (Documents\My Games\Mafia Definitive
//     Edition\Data\<id>\gamesaves\...) to the guests whenever the game writes a new one (HostSave).
//   - Guest following the host's story: loads that save from memory through the main menu's chapter loader
//     (OpenDebugLoadChapter2 on a shipped carrier save; the file contents are swapped for the host's bytes, nothing
//     is written to disk). When the host reaches a checkpoint the guest is not at, the guest quits to the main menu
//     and loads the host's new checkpoint the same way; after each load he is placed next to the host. Objectives
//     are mirrored with the game's own HUD calls, cutscenes are played on the guest's own copy of the cutscene
//     object, stopped when the host's ends or is skipped; puppets are hidden while one plays.
//
// Threads: messages and the save file on the render thread; Lua on GameMainThread (tick callback); a mutex for the
// hand-over.
#include "net/session.h"
#include "render/overlay.h"

#include <string>
#include <vector>

namespace mc::game {

class MissionSync {
public:
    static void Install();  // once, after the Lua bridge: tick callback + the save swap hook

    // render thread
    static void Attach(net::Session& session);
    static void Detach();
    static void Tick(float dt);

    // Lobby STORY start (and JOIN GAME for a late joiner). Host: CONTINUE (his own save). Guest: the host's
    // checkpoint save (or, without one, the start of the host's chapter). False when it cannot start now.
    static bool StartStory();
    // Lobby chapter modes: everyone loads the start of chapter `quest` (a shipped mission replay save, the bytes
    // swapped into the menu's FREE RIDE load) and its quest is started at cp_010; guests then follow the host.
    static bool StartChapter(const std::string& quest);
    static const std::vector<std::string>& Chapters();  // quest ids in story order ("mm_10_chase", ...)
    // A guest started Free Ride (or the session ended): stop following the host's story.
    static void StopFollowing();

    // What the HUD shows over the game (render thread): "" or a line like "HOST IS WATCHING A CUTSCENE".
    static std::string HudNotice();
    static void GetStatus(std::vector<render::OverlayLine>& out);
};

}  // namespace mc::game
