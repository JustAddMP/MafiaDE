#pragma once
// Native co-op entries in the game's own menus (docs/game-api.md "Menus").
//
// The main menu and pause menu are Scaleform AS3 movies (/gui/main_menu_main.gfx, /gui/pause_menu_game.gfx). When
// the game loads one of them we patch its ActionScript (game/swf_patch.h):
//   - main menu: a "CO-OP" entry after FREE RIDE that opens a CO-OP page (host / join friend / join by IP / ...),
//     built by the menu's own code, so it looks, sounds and navigates like every other entry (mouse, keyboard,
//     gamepad);
//   - pause menu (Game page): a fifth "CO-OP" entry (the slot the Xbox build uses for "switch account").
// The patched code talks to us through ExternalInterface.call("AttachIface", "MafiaCoop", <request>, ...): the
// game routes that name to its AttachIface handler, which we hook; our requests never reach the original.
//
// Threads: the Flash callbacks run on GameMainThread, the co-op layer on the render thread. They exchange a small
// State (render -> Flash) and a Command queue (Flash -> render) under a mutex.
#include <string>

namespace mc::game::menu {

enum class Command {
    Host,          // main menu CO-OP > HOST CO-OP GAME (or pause panel)
    Leave,         // STOP HOSTING / LEAVE CO-OP
    JoinFriend,    // JOIN FRIEND
    JoinByIp,      // JOIN BY IP (opens our text input)
    Invite,        // INVITE FRIENDS (Steam overlay)
    PausePanel,    // pause menu > CO-OP
};

enum class Mode { Idle, Hosting, Joining, Connected };

struct State {
    Mode mode = Mode::Idle;
    int players = 0;
};

// Installs the hooks (pattern scans; anything missing disables the feature, logged). Any thread, once.
void Install(const std::wstring& dllDir);
bool Active();

// Render thread.
void SetState(const State& s);
bool PopCommand(Command* out);

const char* CommandName(Command c);

}  // namespace mc::game::menu
