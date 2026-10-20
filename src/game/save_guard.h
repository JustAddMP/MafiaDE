#pragma once
// The game's save system as co-op sees it (docs/re-notes.md 7):
//   - every save load the game requests is logged with its save id, slot and caller;
//   - a guest in a co-op session, and the host of a lobby CHAPTER session, never write into their own save slots:
//     while Block(true) is set, every save file the game writes goes to a scratch tree
//     (%TEMP%\MafiaCoop\coop-saves\<pid>) and is read back from there (a Retry after a death loads the checkpoint of
//     this session); his real Free Ride and story saves stay as they were (so do the slots' profiles, aslot.pf /
//     bslot.pf / cslot.pf, and after 0.15.0 the progress profiles garage.pf / my2k_game_stats.pf /
//     profile_data_storage_persistantdata.pf; the settings profiles are the player's own and left alone:
//     save_paths.h has the table);
//   - the host of any other session (since the 2026-10-20 two-PC test: his garage.pf was written for real in a co-op
//     Free Ride) is kept out the same way (Want(.., host)), except in a world loaded from his story slot (a STORY
//     session: his campaign): there only the progress profiles go to the scratch tree, his story saves and aslot.pf
//     are written as in single player (save_paths::Guard);
//   - the scratch tree is cleared when keeping out starts and removed when it ends: only once the player is back at
//     the main menu after the session (Tick), as the co-op world still saves on its way out (a quit to the menu).
//     A progress profile the session wrote stays held (written to %TEMP%\...\<pid>-held) until the game restarts:
//     the game may still hold the session's changes in memory. The real files are never opened for writing while kept
//     out, so a crash in a session leaves them as they were; trees of gone processes are removed at the next start;
//   - while blocking, the game's answer to a failed mission (a load of "freeroam_before_quest_activated", a save of the
//     player's own story) is kept inside the session (SlotFindDetour, SetSessionSave; docs/re-notes.md 7).
// Install() once at start; Block() / Want() / Tick() from any thread.
#include <cstdint>
#include <string>
#include <vector>

namespace mc::game::save_guard {

void Install();
void Block(bool on);    // keep the game's save writes out of the real slots (a guest in a co-op session)
bool Blocking();        // Block is on (a guest, a CHAPTER host): the failed-mission handling of a co-op chapter
// Every session frame (mission_sync) and at the session's end (false, false): what the session needs kept out. What
// it needs is turned on at once; what it no longer needs is turned off by Tick once the main menu is up.
void Want(bool block, bool host);
// Every frame, with or without a session (module RenderTick): atMainMenu = the main menu is up (menu::AtMainMenu).
void Tick(bool atMainMenu);
// The scratch tree while saves are kept out ("" otherwise): its gamesaves\ holds the checkpoints written in this
// session (the host's checkpoint scan reads it next to the real save folder).
std::string ScratchDir();
uint32_t Redirected();  // save writes sent to the scratch tree since start
// Loads of "freeroam_before_quest_activated" the game requested since start: in a chapter loaded through the
// menu's chapter loader (replay flags) that is how the game answers a failed mission (it goes back to the free
// roam state from before the quest, a different world); the story sync then reloads the checkpoint instead.
uint32_t MissionAborts();
// The checkpoint this co-op session is at (while blocking): when the game answers a failed mission or a death with a
// load of "freeroam_before_quest_activated" (a real story save, outside the session), the save file it reads is
// served from this one instead.
void SetSessionSave(const std::vector<uint8_t>& bytes, const std::string& id);
// fail_load = direct (default): that load reads the session's checkpoint itself (one load, the checkpoint world);
// freeride: it gets the chapter loader's entry (the Free Ride start), and the story sync loads the checkpoint after it
// (0.9.1, two loads).
bool DirectFailLoad();
// After a direct failed-mission load is in the world: the slot entry it used is emptied again, so a failed mission
// outside co-op later in this process reads the player's own save again.
void ForgetFailEntry();

}  // namespace mc::game::save_guard
