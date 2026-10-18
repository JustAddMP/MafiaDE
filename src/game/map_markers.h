#pragma once
// Map markers for the other players (minimap and the pause menu's world map). Pure logic, no game or Lua calls:
// unit-tested by tests/map_markers_test.cpp; player_sync.lua ("Name tags and map markers") asks it what to do with a
// player's marker (mc_mapmark, src/game/player_sync.cpp) and carries it out with game.navigation (docs/game-api.md 13).
//
// Every other player in the session has exactly one marker (one navigation id) while we are in a world:
//   - on the puppet while his puppet is alive, shown and on foot (the game's human icon follows the entity),
//   - at a position otherwise (he drives or rides: his car's spot; no puppet: not spawned yet, hidden in a cutscene,
//     respawning, being re-created): the same icon, moved to where his latest state says he is a few times a second.
// The icon is made once (RegisterHumanEntityDirect on his puppet; RegisterIconPos when he has no puppet yet) and then
// moved between the puppet and positions (ChangeIconTargetToEntityDirect / ChangeIconTargetToPos), so a puppet that
// is released never has an icon on it (the navigation module read a released puppet through a stale icon: crash in
// exe+0x243a915, docs/game-api.md 15), and a puppet seen again never needs a second icon (the game gives an entity no
// second human icon after UnregisterId). A position icon is replaced by the human icon once he has a puppet.
#include <cstdint>
#include <string>

namespace mc::game::mapmark {

enum class Icon : uint8_t { None, Human, Pos };      // the kind of icon the player has (None: no icon yet)
enum class Target : uint8_t { None, Puppet, Pos };   // what it follows
enum class Action : uint8_t {
    Keep,              // nothing to do
    CreateHuman,       // RegisterHumanEntityDirect(puppet): the icon, on his puppet
    CreatePos,         // RegisterIconPos at his position
    ReplaceWithHuman,  // UnregisterId(position icon) + RegisterHumanEntityDirect(puppet)
    ToPuppet,          // ChangeIconTargetToEntityDirect(id, puppet)
    ToPos,             // ChangeIconTargetToPos(id, position)
    MovePos,           // ChangeIconTargetToPos(id, new position): he moved
};

struct Input {
    Icon icon = Icon::None;
    Target target = Target::None;
    bool puppet = false;         // his puppet is alive, shown and on foot (it may carry the icon)
    bool puppetChanged = false;  // the icon follows a puppet entity that is not his puppet now
    bool humanFailed = false;    // RegisterHumanEntityDirect gave no icon for this puppet
    bool hasPos = false;         // we know where he is
    float x = 0, y = 0, z = 0;   // where he is (his car while he drives or rides)
    float lastX = 0, lastY = 0, lastZ = 0;  // where the icon was put last (Target::Pos)
};

struct Plan {
    Target want = Target::None;  // what the icon follows after the action
    Action act = Action::Keep;
};

constexpr float kMoveEps = 1.0f;     // m: a position icon is moved when he is this far from it
constexpr size_t kMaxNameBytes = 32; // a name on the map is cut to this (at a UTF-8 character boundary)

Plan Decide(const Input& in);

const char* IconName(Icon i);
const char* TargetName(Target t);
const char* ActionName(Action a);
Icon IconFromName(const char* s);      // "human" / "pos", anything else None
Target TargetFromName(const char* s);  // "puppet" / "pos", anything else None

// A player name as one Lua command field-tail: control characters become spaces, spaces at the ends go, at most
// kMaxNameBytes bytes (never half a UTF-8 character); "" when nothing is left.
std::string CleanName(const std::string& name);
// "N <id> <name>\n" for player_sync.lua (the name is the rest of the line), "" when the cleaned name is empty.
std::string NameLine(uint32_t id, const std::string& name);

}  // namespace mc::game::mapmark
