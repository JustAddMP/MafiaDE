// The Mafia DE game layer, as JustAddMP core sees it (external/core/include/mc/game_module.h): the Lua bridge and
// player sync (game/lua.h, game/player_sync.h) and the CO-OP entries in the game's menus (game/menu.h).
#include "mc/game_module.h"

#include "dll/ui_input.h"
#include "game/characters.h"
#include "game/lua.h"
#include "game/menu.h"
#include "game/player_sync.h"

#include <windows.h>

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
};

}  // namespace
}  // namespace mc::game

namespace mc {

GameModule& GetGameModule() {
    static game::MafiaModule module;
    return module;
}

}  // namespace mc
