#include "ui.h"

#include <fmt/format.h>

#include <string>

#include "../../core/application.h"

// These shit takes variadic arguments that we haven't reversed yet so w're forced to call it through lua
namespace MafiaMP::Game::Helpers::UI {
    // Every string that reaches these helpers is interpolated into Lua source, so it has to be a
    // valid double-quoted Lua literal whatever the caller passes (scripts send player names and
    // free text here).
    static std::string LuaQuote(char const *text) {
        std::string out;
        if (!text) {
            return out;
        }
        for (const char *p = text; *p; ++p) {
            switch (*p) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\r': break;
            case '\n': out += ' '; break;
            case '\0': break;
            default: out += *p; break;
            }
        }
        return out;
    }

    static void Run(const std::string &command) {
        Core::gApplication->GetLuaVM()->ExecuteString(command.c_str());
    }

    void DisplayBannerMessage(char const *title, char const *content) {
        Run(fmt::format("game.hud:SendMessageMovie(\"HUD\", \"OnShowFreerideBanner\", \"{}\", \"{}\")", LuaQuote(title), LuaQuote(content)));
    }

    void DisplayGenericMessage(char const *title, char const *content) {
        Run(fmt::format("game.hud:SendMessageMovie(\"HUD\", \"OnShowGenericMessage\", \"{}\", \"{}\")", LuaQuote(title), LuaQuote(content)));
    }

    void DisplayTitleCard(char const *title, char const *content) {
        Run(fmt::format("game.hud:ShowTitleCard(\"{}\", \"{}\", true)", LuaQuote(title), LuaQuote(content)));
    }

    void DisplayNote(char const* title, char const* content) {
        Run(fmt::format("game.hud:SendMessageMovie(\"HUD\", \"OnShowNote\", true, \"{}\", \"{}\")", LuaQuote(title), LuaQuote(content)));
    }

    void HideTitleCard() {
        Run("game.hud:HideTitleCard()");
    }

    void ToggleLoadSpinner(bool toggle) {
        Run(fmt::format("game.hud:ToggleSaveLoadSpinner({})", toggle));
    }

    void ShowNotification(char const *title, char const *content, int color) {
        Run(fmt::format("game.hud:SendMessageMovie(\"HUD\", \"OnShowFreerideNotification\", \"{}\", \"{}\", \"{}\")", LuaQuote(title), LuaQuote(content), color));
    }

    void HideNotification() {
        Run("game.hud:SendMessageMovie(\"HUD\", \"OnHideFreerideNotification\")");
    }

    void DisplayMissionExit(char const *title, char const *content, int time) {
        Run(fmt::format("game.hud:ShowMissionExit(\"{}\", \"{}\", {})", LuaQuote(title), LuaQuote(content), time));
    }

    void StartCountdown(int time) {
        Run(fmt::format("game.hud:StartCountDown({})", time));
    }
}
