#include "game/characters.h"

#include <cctype>

namespace mc::game::characters {
namespace {

// Checked in game 2026-10-15 (docs/game-api.md 12): every profile below loads as the local player's override and as
// a puppet. "STORY" outfits (characters/scotch/game/main/...) dress the character as the loaded story point does,
// so two games at different points can show different clothes; the named outfits look the same everywhere. Puppets
// with a story profile react to punches like any NPC; puppets with an ally or Tommy profile cannot be targeted by
// the game's melee, so a punch at them is detected from the swing (docs/game-api.md 11).
const std::vector<Character>& Build() {
    static const std::vector<Character> list = {
        {"TOMMY",
         {{"tommy", "STORY", nullptr},
          {"tommy_casual", "CASUAL", "characters/game/hero/wardrobe/game_hero_wardrobe_casual"},
          {"tommy_cab", "CAB DRIVER", "characters/game/hero/wardrobe/game_hero_wardrobe_cab_driver_flatcap"},
          {"tommy_suit", "SUIT AND HAT", "characters/game/hero/wardrobe/game_hero_wardrobe_gangster_suit_2_hat"},
          {"tommy_trench", "TRENCH COAT", "characters/game/hero/wardrobe/game_hero_wardrobe_gangster_trenchcoat"},
          {"tommy_racing", "RACING", "characters/game/hero/wardrobe/game_hero_wardrobe_racing_outfit"},
          {"tommy_sailor", "SAILOR", "characters/game/hero/wardrobe/game_hero_wardrobe_sailor"}}},
        {"PAULIE",
         {{"paulie", "STORY", "characters/scotch/game/main/game_main_paulie"},
          {"paulie_suit", "SUIT", "characters/scotch/ai/ai_ally_paulie_gangster_suit_1_no_hat"},
          {"paulie_hat", "SUIT AND HAT", "characters/scotch/ai/ai_ally_paulie_gangster_suit_2_hat"},
          {"paulie_trench", "TRENCH COAT", "characters/scotch/ai/ai_ally_paulie_trenchcoat"}}},
        {"SAM",
         {{"sam", "STORY", "characters/scotch/game/main/game_main_sam"},
          {"sam_suit", "SUIT", "characters/scotch/ai/ai_ally_sam_gangster_suit_1_no_hat"},
          {"sam_hat", "SUIT AND HAT", "characters/scotch/ai/ai_ally_sam_gangster_suit_2_hat"},
          {"sam_trench", "TRENCH COAT", "characters/scotch/ai/ai_ally_sam_trenchcoat"}}},
        {"SALIERI", {{"salieri", "STORY", "characters/scotch/game/main/game_main_salieri"}}},
        {"FRANK",
         {{"frank", "STORY", "characters/scotch/game/main/game_main_frank"},
          {"frank_suit", "SUIT", "characters/scotch/game/main/game_main_frank_suit"},
          {"frank_coat", "COAT", "characters/scotch/game/main/game_main_frank_omerta"}}},
        {"VINCENZO", {{"vincenzo", "STORY", "characters/scotch/game/main/game_main_vincenzo"}}},
        {"LUIGI", {{"luigi", "STORY", "characters/scotch/game/main/game_main_luigi"}}},
        {"RALPH", {{"ralph", "STORY", "characters/scotch/game/main/game_main_ralph"}}},
        {"SARAH", {{"sarah", "STORY", "characters/scotch/game/main/game_main_sarah"}}},
        {"MORELLO", {{"morello", "STORY", "characters/scotch/game/main/game_main_morello"}}},
        {"SERGIO", {{"sergio", "STORY", "characters/scotch/game/main/game_main_sergio_suit"}}},
        {"NORMAN", {{"norman", "STORY", "characters/scotch/game/main/game_main_norman"}}},
        {"BERTONE", {{"bertone", "STORY", "characters/scotch/game/main/game_main_bertone"}}},
        {"BILLY", {{"billy", "STORY", "characters/scotch/game/main/game_main_billy"}}},
    };
    return list;
}

const char* const kDefaults[] = {"tommy", "paulie", "sam", "salieri"};

bool Decimal(const std::string& s) {
    if (s.empty() || s.size() > 20 || s[0] < '1' || s[0] > '9') return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

}  // namespace

const std::vector<Character>& List() { return Build(); }

std::string DefaultModel(net::PlayerId id) { return kDefaults[id % (sizeof(kDefaults) / sizeof(kDefaults[0]))]; }

bool SafeProfile(const std::string& s) {
    if (s.empty() || s.size() > 96) return false;
    for (unsigned char c : s)
        if (!std::isalnum(c) && c != '_' && c != '/' && c != '.' && c != '-') return false;
    return true;
}

bool Find(const std::string& model, size_t* character, size_t* outfit) {
    const auto& list = List();
    for (size_t c = 0; c < list.size(); ++c)
        for (size_t o = 0; o < list[c].outfits.size(); ++o) {
            const Outfit& of = list[c].outfits[o];
            if (model == of.alias || (of.profile && model == of.profile)) {
                if (character) *character = c;
                if (outfit) *outfit = o;
                return true;
            }
        }
    return false;
}

std::string DisplayName(const std::string& model) {
    size_t c = 0, o = 0;
    if (!Find(model, &c, &o)) return "CUSTOM";
    const Character& ch = List()[c];
    return std::string(ch.name) + " / " + ch.outfits[o].name;
}

std::string Resolve(const std::string& model, net::PlayerId id) {
    size_t c = 0, o = 0;
    const std::string m = model.empty() ? DefaultModel(id) : model;
    if (Find(m, &c, &o)) {
        const char* p = List()[c].outfits[o].profile;
        return p ? p : "tommy";
    }
    if (SafeProfile(m) && (m.find('/') != std::string::npos || Decimal(m))) return m;  // a profile not in our list
    // unknown alias (a newer version's, or a typo): the default character for this player
    Find(DefaultModel(id), &c, &o);
    const char* p = List()[c].outfits[o].profile;
    return p ? p : "tommy";
}

}  // namespace mc::game::characters
