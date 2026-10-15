#pragma once
// The game's save system as co-op sees it (docs/re-notes.md 7):
//   - every save load the game requests is logged with its save id, slot and caller;
//   - a guest in a co-op session, and the host of a lobby CHAPTER session, never write into their own save slots
//     (the host of a STORY session continues his own story and keeps saving it): while Block(true) is set, every save
//     file the game writes goes to a scratch tree (%TEMP%\MafiaCoop\coop-saves\<pid>) and is read back from there
//     (a Retry after a death loads the checkpoint of this session); his real Free Ride and story saves stay as they
//     were. The scratch tree is cleared each time blocking starts.
// Install() once at start; Block() from any thread.
#include <cstdint>
#include <string>

namespace mc::game::save_guard {

void Install();
void Block(bool on);    // keep the game's save writes out of the real slots (a guest in a co-op session)
bool Blocking();
// The scratch tree while blocking ("" otherwise): its gamesaves\ holds the checkpoints written in this session
// (the host's checkpoint scan reads it next to the real save folder).
std::string ScratchDir();
uint32_t Redirected();  // save writes sent to the scratch tree since start
// Loads of "freeroam_before_quest_activated" the game requested since start: in a chapter loaded through the
// menu's chapter loader (replay flags) that is how the game answers a failed mission (it goes back to the free
// roam state from before the quest, a different world); the story sync then reloads the checkpoint instead.
uint32_t MissionAborts();

}  // namespace mc::game::save_guard
