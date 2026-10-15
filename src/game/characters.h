#pragma once
// The characters a player can appear as (docs/game-api.md 12, docs/PLAN.md "F"): a curated list of story characters
// with their outfits, each a spawn profile that was checked in game as the local player (model manager override slot)
// and as a puppet (runtime spawner). The model string a player picks (MafiaCoop.ini `model =`, --model, the CO-OP
// panel's CHARACTER/OUTFIT rows) is an alias from this list; a spawn profile path or a decimal profile hash also
// works. Anything else falls back to the default character for the player's id.
#include "net/session.h"

#include <string>
#include <vector>

namespace mc::game::characters {

struct Outfit {
    const char* alias;    // model string, e.g. "paulie_suit"
    const char* name;     // shown in the panel, e.g. "SUIT"
    const char* profile;  // spawn profile; nullptr = Tommy as the story dresses him (no override)
};

struct Character {
    const char* name;  // shown in the panel, e.g. "PAULIE"
    std::vector<Outfit> outfits;
};

const std::vector<Character>& List();

// The alias of the default character for a player id: host "tommy", then "paulie", "sam", "salieri".
std::string DefaultModel(net::PlayerId id);

// Finds a model string in the list (alias, or one of its profiles). False for anything else.
bool Find(const std::string& model, size_t* character, size_t* outfit);

// "PAULIE / SUIT" for the overlay and logs; "CUSTOM" for a profile that is not in the list.
std::string DisplayName(const std::string& model);

// What a model string looks like in game: a spawn profile (path or decimal hash), or "tommy" for the story's Tommy.
// Known aliases map to their profile, profile paths and decimal hashes pass through (checked for safe characters),
// anything else ("" included) gives the default for the player id.
std::string Resolve(const std::string& model, net::PlayerId id);

// True for a string that can be passed to the game as a spawn profile: [A-Za-z0-9_/.-], 1..96 bytes.
bool SafeProfile(const std::string& s);

}  // namespace mc::game::characters
