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
//
// The friend brain switch (player_sync.lua M.Ally, SwitchBrain(1), about 2 s after a puppet spawns) removes a human
// icon that is on the puppet: SwitchBrain -> exe 0x2B81D70 -> 0x2B820D0 -> 0x3060200 looks the human up in the
// navigation module's entity map and unregisters its human icon (0x30602B0, the same routine as UnregisterId) when
// the old brain held a navigation id (brain +0x11C). On a guest the host's puppet (a Tommy profile, AI type 1) lost
// its icon this way within 2 s (0.15.0 test: the id was gone, ChangeIconTargetToPos returned false, nothing on the
// minimap); the host's copy of a guest (AI type 0) kept it. So the human icon goes on a puppet only once its brain
// switch is done (Input::brainPending: until then the icon stays at his position), and an icon on a puppet is checked
// once a second against the game (GetIdByScriptWrapper, CheckProbe): a lost one is made anew, twice at most per puppet.
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
    bool brainPending = false;   // his puppet's friend brain switch is still to come (it would remove an icon on it)
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

// The once-a-second check of an icon on a puppet: GetIdByScriptWrapper(puppet) gives the navigation id of the icon on
// that entity (0 = none). found < 0: the check could not be made (no such call, an error).
enum class Probe : uint8_t {
    Unknown,  // not checked (found < 0)
    Ok,       // the game has our icon on the puppet
    Lost,     // it has none / another one: ours is gone, make it anew (no UnregisterId: the id is not the game's now)
    GiveUp,   // lost kMaxLosses times on this puppet: it gets no icon of its own, his icon stays at his position
};
constexpr int kMaxLosses = 2;      // icons lost on one puppet before it gets no more
constexpr float kProbePeriod = 1;  // s between checks of an icon on a puppet (and from putting it there to the first)
Probe CheckProbe(uint32_t id, double found, int lossesBefore);
const char* ProbeName(Probe p);

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
