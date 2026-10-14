// The Mafia DE game layer, as JustAddMP core sees it (external/core/include/mc/game_module.h): the Lua bridge and
// player sync (game/lua.h, game/player_sync.h) and the CO-OP entries in the game's menus (game/menu.h).
#include "mc/game_module.h"

#include "dll/ui_input.h"
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
};

}  // namespace
}  // namespace mc::game

namespace mc {

GameModule& GetGameModule() {
    static game::MafiaModule module;
    return module;
}

}  // namespace mc
