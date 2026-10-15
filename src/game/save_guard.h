#pragma once
// The game's save system as co-op sees it (docs/re-notes.md 7):
//   - every save load the game requests is logged with its save id, slot and caller;
//   - a guest following the host's story never writes into his own save slots: while Block(true) is set, every save
//     file the game writes goes to a scratch tree (%TEMP%\MafiaCoop\coop-saves\<pid>) and is read back from there
//     (a Retry after a death loads the checkpoint of this session); his real Free Ride and story saves stay as they
//     were. The scratch tree is cleared each time blocking starts.
// Install() once at start; Block() from any thread.
#include <cstdint>
#include <string>

namespace mc::game::save_guard {

void Install();
void Block(bool on);    // keep the game's save writes out of the real slots (a guest following the host's story)
bool Blocking();
uint32_t Redirected();  // save writes sent to the scratch tree since start

}  // namespace mc::game::save_guard
