// The Mafia DE game layer, as JustAddMP core sees it (external/core/include/mc/game_module.h): the Lua bridge and
// player sync (game/lua.h, game/player_sync.h) and the CO-OP entries in the game's menus (game/menu.h).
#include "mc/game_module.h"

#include "core/config.h"
#include "core/log.h"
#include "dll/ui_input.h"
#include "game/characters.h"
#include "game/lua.h"
#include "game/menu.h"
#include "game/player_sync.h"

#include <windows.h>
#include <algorithm>
#include <cmath>

namespace mc::game {
namespace {

// A key/button of ours held while one of our windows is in the foreground and no core panel takes input.
bool OurWindowHasInput() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || (GetWindowThreadProcessId(fg, &pid), pid != GetCurrentProcessId())) return false;
    return !ui_input::Blocking();
}

// The enter-vehicle control (keyboard E, pad Y).
bool EnterVehicleHeld() {
    if (!OurWindowHasInput()) return false;
    constexpr uint16_t kPadY = 0x8000;  // XINPUT_GAMEPAD_Y
    return (GetAsyncKeyState('E') & 0x8000) != 0 || (ui_input::PadButtons() & kPadY) != 0;
}

// The aim control (right mouse button, pad left trigger). The game itself does not tell scripts whether the player
// aims (docs/game-api.md 10), so this is read from the input.
bool AimHeld() {
    if (!OurWindowHasInput()) return false;
    return (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 || ui_input::PadLeftTrigger() > 60;
}

// The CO-OP panel's CHARACTER and OUTFIT rows (docs/PLAN.md "F"). CHARACTER steps through DEFAULT (the character
// for your player id: host Tommy, then Paulie, Sam, Salieri) and the list in game/characters.cpp; OUTFIT through that
// character's outfits. Every step applies at once: you change in the world while browsing, and the others see it.
void CharacterOptions(std::vector<PanelOption>& out) {
    const std::string model = PlayerSync::Model();
    size_t c = 0, o = 0;
    if (model.empty()) {
        out.push_back({"CHARACTER", "DEFAULT"});
        return;
    }
    if (!characters::Find(model, &c, &o)) {
        out.push_back({"CHARACTER", "CUSTOM"});
        return;
    }
    const auto& ch = characters::List()[c];
    out.push_back({"CHARACTER", ch.name});
    if (ch.outfits.size() > 1) out.push_back({"OUTFIT", ch.outfits[o].name});
}

void CharacterStep(size_t index, int delta) {
    const auto& list = characters::List();
    const std::string model = PlayerSync::Model();
    size_t c = 0, o = 0;
    const bool known = !model.empty() && characters::Find(model, &c, &o);
    if (index == 0) {  // CHARACTER: DEFAULT, then every character (its first outfit)
        const int n = static_cast<int>(list.size()) + 1;
        int pos = known ? static_cast<int>(c) + 1 : 0;
        if (!known && !model.empty()) pos = delta > 0 ? 0 : 1;  // CUSTOM: next is the first character
        pos = ((pos + delta) % n + n) % n;
        PlayerSync::SetModel(pos == 0 ? std::string() : list[static_cast<size_t>(pos - 1)].outfits[0].alias);
    } else if (index == 1 && known) {  // OUTFIT
        const auto& outfits = list[c].outfits;
        const int n = static_cast<int>(outfits.size());
        const int pos = ((static_cast<int>(o) + delta) % n + n) % n;
        PlayerSync::SetModel(outfits[static_cast<size_t>(pos)].alias);
    }
}

// ---- CO-OP SETTINGS rows (saved in MafiaCoop.ini; read where they are used, so a change applies at once) ----
struct Toggle {
    const char* label;
    const char* key;
    bool def;
};
constexpr Toggle kToggles[] = {
    {"SHOW NAME TAGS", "name_tags", true},     // names over the other players (DrawHud)
    {"MAP MARKERS", "map_markers", true},      // the other players on the minimap (player_sync.lua)
    {"FRIENDLY FIRE", "friendly_fire", true},  // players can hurt each other (docs/protocol.md "Phase 6")
};

void SettingsRows(std::vector<PanelOption>& out) {
    for (const Toggle& t : kToggles) out.push_back({t.label, core::Config::Get().GetBool(t.key, t.def) ? "ON" : "OFF"});
}

void SettingsStep(size_t index, int) {
    if (index >= std::size(kToggles)) return;
    const Toggle& t = kToggles[index];
    const bool on = !core::Config::Get().GetBool(t.key, t.def);
    core::Config::Get().Set(t.key, on ? "1" : "0");
    MC_LOG_INFO("settings: %s = %d", t.key, on ? 1 : 0);
}

// ---- lobby modes (the index travels on the wire: keep the order) ----
enum LobbyModeId { kModeFreeRide = 0, kModeStory = 1 };
constexpr const char* kFreeRideSave = "freeroam_test";  // GetLoadedSaveId() in Free Ride (docs/game-api.md 11)

// ---- name tags (docs/game-api.md 13): the other players' names over their puppets ----
constexpr float kTagFull = 35.0f;  // m: fully visible up to here
constexpr float kTagGone = 90.0f;  // m: faded out here

uint32_t WithAlpha(uint32_t argb, float a) {
    const auto al = static_cast<uint32_t>(static_cast<float>(argb >> 24) * std::clamp(a, 0.0f, 1.0f));
    return (argb & 0x00FFFFFF) | (al << 24);
}

void DrawTags(render::DrawList& d, float w, float h) {
    if (!core::Config::Get().GetBool("name_tags", true)) return;
    std::vector<PlayerSync::NameTag> tags;
    PlayerSync::GetTags(tags);
    const float s = h / 1080.0f;
    for (const auto& t : tags) {
        if (t.dist > kTagGone || t.x < -1.1f || t.x > 1.1f || t.y < -1.1f || t.y > 1.1f) continue;
        const float a = t.dist <= kTagFull ? 1.0f : 1.0f - (t.dist - kTagFull) / (kTagGone - kTagFull);
        const float px = std::clamp(30.0f * s * 10.0f / std::max(t.dist, 1.0f), 17.0f * s, 30.0f * s);
        std::string name = t.name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        const float tw = render::DrawList::TextWidth(name, px);
        const float cx = (t.x + 1.0f) * 0.5f * w, cy = (1.0f - t.y) * 0.5f * h;
        const float x = cx - tw * 0.5f, y = cy - px;
        d.Rect(x - px * 0.35f, y - px * 0.08f, tw + px * 0.7f, px * 1.16f, WithAlpha(0x8C000000, a));
        d.Rect(x - px * 0.35f, y + px * 1.04f, tw + px * 0.7f, std::max(1.0f, px * 0.06f), WithAlpha(0xFFD1AC62, a));
        d.Text(x, y, name, px, WithAlpha(0xFFE9E5DB, a));
    }
}

class MafiaModule final : public GameModule {
public:
    void Init(const std::wstring& dllDir, const std::string& logTag) override {
        // Lua bridge: hooks the script tick (GameMainThread); player sync runs on it. Failure only disables it.
        if (lua::Install(dllDir, logTag)) PlayerSync::Install();
        menu::Install(dllDir);
    }
    void RenderTick(float dt) override { lua::PollRender(dt); }

    void OnSessionStart(net::Session& session) override { PlayerSync::Attach(session); }
    void OnSessionEnd() override { PlayerSync::Detach(); }
    void OnPlayerLeft(net::PlayerId id) override { PlayerSync::OnPlayerLeft(id); }
    void SessionTick(float dt) override { PlayerSync::Tick(dt, EnterVehicleHeld(), AimHeld()); }

    int PlayerHealth(net::PlayerId id) override { return PlayerSync::Health(id); }
    void AppendStatus(std::vector<render::OverlayLine>& out) override { PlayerSync::GetStatus(out); }
    void SetMenuState(const MenuState& state) override { menu::SetState(state); }
    bool PopMenuCommand(MenuCommand* out) override { return menu::PopCommand(out); }
    void PanelOptions(std::vector<PanelOption>& out) override { CharacterOptions(out); }
    void PanelOptionStep(size_t index, int delta) override { CharacterStep(index, delta); }

    void SettingsOptions(std::vector<PanelOption>& out) override { SettingsRows(out); }
    void SettingsOptionStep(size_t index, int delta) override { SettingsStep(index, delta); }

    void LobbyModes(std::vector<LobbyMode>& out) override {
        out.push_back({"FREE RIDE", true, ""});
        out.push_back({"STORY", false, "NEXT VERSION"});  // continue the host's save / pick a chapter: next batch
    }
    bool StartMode(int mode) override {
        if (mode != kModeFreeRide) return false;
        if (PlayerSync::InWorld()) {
            MC_LOG_INFO("lobby: Free Ride start, but we are in the world already");
            return false;
        }
        return menu::RequestLoad("free_ride");  // the main menu's own FREE RIDE (menu_parent.LoadGame)
    }
    bool InWorld() override { return PlayerSync::InWorld(); }
    bool LobbyCanOpen() override { return menu::MainMenuShown(); }  // not over the title or a loading screen
    int CurrentMode() override {
        if (!PlayerSync::InWorld()) return -1;
        return PlayerSync::SaveId() == kFreeRideSave ? kModeFreeRide : kModeStory;
    }
    std::string PlayerCharacter(net::PlayerId id) override { return PlayerSync::CharacterName(id); }

    void DrawHud(render::DrawList& d, float w, float h) override { DrawTags(d, w, h); }
};

}  // namespace
}  // namespace mc::game

namespace mc {

GameModule& GetGameModule() {
    static game::MafiaModule module;
    return module;
}

}  // namespace mc
