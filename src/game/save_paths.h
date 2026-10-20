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
//
// Who is kept out, and of what (Guard below; since the 2026-10-20 two-PC test, where the HOST's garage.pf was written
// for real at every garage visit in a co-op Free Ride: up to then only guests and lobby CHAPTER hosts were covered):
//   a guest, the host of a lobby CHAPTER (Block)         All: saves, slot profiles, progress, unknown profile files
//   the host of any other session (Host), in a world loaded from the story slot (storage "aslot": CONTINUE, his
//     checkpoints, his fail screen)                     Progress: progress + unknown profile files only; his story
//                                                       saves and aslot.pf stay real (a STORY session is his campaign)
//   the host in any other world (FREE RIDE, the chapter loader, his own Free Ride he started hosting from)
//                                                       All (a co-op Free Ride writes gamesaves\default\aslot\save1.sav
//                                                       and aslot.pf: guests' logs of 2026-10-20 show the path)
//   nobody (no session, or back at the main menu after one)   None (held files stay held, see HELD)
// Settings are never redirected. While a file is kept out the real one is never opened for writing, so a crash in a
// session leaves it as it was; the scratch tree is per process and removed at the next start.
#include <set>
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

enum class Scope { None, Progress, All };
// Kind k is kept out of the real files under scope s.
bool KeptOut(Kind k, Scope s);
const char* ScopeName(Scope s);

// Where one file access goes.
enum class Where { Real, Scratch, Held };

// The decision state of save_guard (one per process, under its mutex). Pure: tests replay logged sessions through it.
class Guard {
public:
    // A save load request (save_guard's LoadDetour) and the storage it reads: "aslot" is the player's story (CONTINUE,
    // its checkpoints, its fail screen); anything else ("debugloadchapter": FREE RIDE and our chapter loads, "bslot")
    // is not.
    void Load(const std::string& storage);
    // What the session needs (mission_sync, every session frame; (false, false) at its end): a guest or the host of
    // a lobby CHAPTER (block), the host of any session (host). Turned on at once; turned off only by MainMenu().
    void Want(bool block, bool host);
    // Block now (a CHAPTER start, before its load) or end it now (on = false; also no longer wanted).
    void SetBlock(bool on);
    // The main menu is up (the co-op world is gone, its quit save written): what is no longer wanted ends.
    void MainMenu();
    bool Block() const { return _block; }
    bool Host() const { return _host; }
    bool StoryWorld() const { return _story; }
    Scope Current() const;
    // Where an access of `m` goes now. A write kept out of a file that is HeldAfterSession marks it held: when nothing
    // is kept out any more (Current() None) its accesses go to the held tree for the rest of the process.
    Where Route(const Mapped& m, bool write);
    bool AnyHeld() const { return !_held.empty(); }
    bool IsHeld(const std::string& file) const { return _held.count(file) != 0; }
    std::string HeldList() const;  // "garage.pf, my2k_game_stats.pf"

private:
    bool _block = false, _host = false, _story = false;
    bool _wantBlock = false, _wantHost = false;
    std::set<std::string> _held;
};

}  // namespace mc::game::save_paths
