#pragma once
// Which of the game's files a co-op session may change (save_guard.cpp, docs/re-notes.md 7 "Profile files").
// Pure logic, no game or Windows calls: unit-tested by tests/save_paths_test.cpp.
//
// The game writes, under Documents\My Games\Mafia Definitive Edition (all with CreateFileA, mixed / and \):
//   Data\<steamid>\gamesaves\<profile>\<slot>\saveN.sav   the save slots (aslot = story, bslot = Free Ride)
//   Data\<steamid>\profiles\<profile>\*.pf                 the profile: XML C_SaveDataUnwrapped, one file per block
//   Saves\videoconfig.cfg, Saves\launcconfig.cfg            window / launcher settings
// Per file, while the save slots are blocked (a guest in a co-op session, the host of a lobby CHAPTER session):
//   Save          gamesaves\...                      REDIRECT  (the checkpoints of the session, since 0.4)
//   SlotProfile   aslot.pf, bslot.pf, cslot.pf       REDIRECT  (the slot's script data: chapter, weapons; 0.9.1)
//   Progress      garage.pf                          REDIRECT + HELD (the cars in the player's garage, with colours)
//                 my2k_game_stats.pf                 REDIRECT + HELD (play stats, e.g. m_LockpickedCars)
//                 profile_data_storage_persistantdata.pf  REDIRECT + HELD (script data kept across saves; what
//                                                    exactly it holds (collectibles? unlocks?) is UNVERIFIED)
//   Setting       gamesettings.pf, gameinputmodule.pf (key bindings), profile_language.pf,
//                 profile_audiosettings.pf, profile_videosettings.pf, map_filter.pf (the map's filter buttons),
//                 Saves\videoconfig.cfg, Saves\launcconfig.cfg   LEFT ALONE: the player's own settings, changed
//                                                    in co-op on purpose
//   UnknownProfile any other *.pf under profiles\    REDIRECT + HELD (not seen on disk; a patch or DLC block we do
//                                                    not know: kept out of the player's profile to be safe)
//   None          everything else (steam_autocloud.vdf, game data, our own scratch tree)  LEFT ALONE
// HELD: the game keeps these blocks in memory for the whole process (the profile is read at start; whether a
// later save load re-reads garage.pf is UNVERIFIED), so after the session has written one of them, the game's copy
// may hold the co-op changes. Its later writes of that file in this process go to the held tree too, until the
// game restarts (save_guard: "<scratch>-held").
// Achievements are Steam's (ISteamUserStats), not a file: not handled here.
#include <string>

namespace mc::game::save_paths {

enum class Kind { None, Save, SlotProfile, Progress, Setting, UnknownProfile };

struct Mapped {
    Kind kind = Kind::None;
    std::string file;  // file name, lower case ("garage.pf")
    std::string sub;   // the path from "\gamesaves\" or "\profiles\" on, original case, backslashes only
};

// Classifies one path as the game passes it to CreateFileA / DeleteFileA / CopyFileExA.
Mapped Classify(const std::string& path);
// Kept out of the player's real files while the slots are blocked.
bool Redirected(Kind k);
// Also kept out after the session, for the rest of the process, once the session wrote it.
bool HeldAfterSession(Kind k);
// root + sub, with no doubled backslashes ("" when sub is empty).
std::string Join(const std::string& root, const std::string& sub);
// The scratch path for `path` under `root` ("" = the real file is used). Not for paths already in our own tree.
std::string Target(const std::string& path, const std::string& root);
const char* KindName(Kind k);
// Why this file is redirected or left alone (for the log).
const char* Why(const Mapped& m);

}  // namespace mc::game::save_paths
