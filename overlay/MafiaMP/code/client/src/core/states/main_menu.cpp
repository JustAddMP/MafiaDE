#include "main_menu.h"
#include "states.h"

#include <utils/safe_win32.h>
#include <utils/states/machine.h>

#include <cstring>
#include <nlohmann/json.hpp>
#include <string>

#include "../../game/helpers/controls.h"

#include "../application.h"
#include "../story_host.h"

#include <external/imgui/widgets/corner_text.h>
#include <fmt/format.h>
#include <logging/logger.h>

namespace MafiaMP::Core::States {
    MainMenuState::MainMenuState() {}

    MainMenuState::~MainMenuState() {}

    int32_t MainMenuState::GetId() const {
        return StateIds::MainMenu;
    }

    const char *MainMenuState::GetName() const {
        return "MainMenu";
    }

    bool MainMenuState::OnEnter(Framework::Utils::States::Machine *) {
        // Reset the states
        _shouldProceedOfflineDebug = false;
        _shouldProceedConnection   = false;

        // Story host: the genuine campaign is running under us. Never take the controls or show the
        // connect page; connect to the configured server by ourselves (retrying while it is down).
        _storyAuto = IsStoryHost() || StoryHost::HasAutoConnect();
        if (_storyAuto) {
            _storyRetryAt = static_cast<double>(GetTickCount64()) / 1000.0 + 1.0;
            if (!IsStoryHost()) {
                gApplication->LockControls(true); // a guest in free ride waits for the connection like before
            }
            return true;
        }

        // Lock controls
        gApplication->LockControls(true);

        // Grab the view from the application
        auto const view = gApplication->GetWebManager()->GetView(gApplication->GetMainMenuViewId());
        if (!view) {
            return false;
        }

        view->Display(true);
        view->Focus(true);

        // Bind the event listeners
        view->AddEventListener("RUN_SANDBOX", [this](std::string eventPayload) {
            _shouldProceedOfflineDebug = true;
        });

        view->AddEventListener("EXIT_APP", [this](std::string eventPayload) {
            TerminateProcess(GetCurrentProcess(), 0);
        });

        view->AddEventListener("CONNECT_TO_HOST", [this](std::string eventPayload) {
            auto const parsedPayload = nlohmann::json::parse(eventPayload);

            // Make sure the payload is valid
            if (!parsedPayload.count("host") || !parsedPayload.count("port")) {
                Framework::Logging::GetLogger("StateMachine")->critical("Invalid payload for CONNECT_TO_HOST event");
                return;
            }

            // Make sure the port is valid
            if (!parsedPayload["port"].is_number()) {
                Framework::Logging::GetLogger("StateMachine")->critical("Invalid port for CONNECT_TO_HOST event");
                return;
            }

            // Make sure the host is valid
            if (!parsedPayload["host"].is_string() || parsedPayload["host"].get<std::string>().empty()) {
                Framework::Logging::GetLogger("StateMachine")->critical("Invalid host for CONNECT_TO_HOST event");
                return;
            }

            // Update the application state for further usage
            Framework::Integrations::Client::CurrentState newApplicationState = gApplication->GetCurrentState();
            newApplicationState.host                                          = parsedPayload["host"];
            newApplicationState.port                                          = parsedPayload["port"];
            newApplicationState.nickname                                      = "Player";
            if (gApplication->GetPresence()->IsInitialized()) {
                discord::User currUser {};
                gApplication->GetPresence()->GetUserManager().GetCurrentUser(&currUser);
                const char* username = currUser.GetUsername();
                if (username && strlen(username) > 0) {
                    newApplicationState.nickname = username;
                }
            }
            gApplication->SetCurrentState(newApplicationState);

            // Request transition to next state (session connection)
            _shouldProceedConnection = true;
        });
        return true;
    }

    bool MainMenuState::OnExit(Framework::Utils::States::Machine *) {
        if (_storyAuto) {
            if (!IsStoryHost()) {
                gApplication->LockControls(false);
            }
            return true;
        }

        // Grab the view from the application
        auto const view = gApplication->GetWebManager()->GetView(gApplication->GetMainMenuViewId());
        if (!view) {
            return false;
        }

        // Unbind the event listeners
        view->RemoveEventListener("RUN_SANDBOX");
        view->RemoveEventListener("CONNECT_TO_HOST");
        view->RemoveEventListener("EXIT_APP");

        // Hide the view
        view->Display(false);
        view->Focus(false);

        // Unlock controls
        gApplication->LockControls(false);
        return true;
    }

    bool MainMenuState::OnUpdate(Framework::Utils::States::Machine *machine) {
        if (_storyAuto) {
            const double now = static_cast<double>(GetTickCount64()) / 1000.0;
            const bool story       = IsStoryHost();
            const std::string host = story ? StoryHost::GetServerHost() : StoryHost::GetAutoHost();
            const int port         = story ? StoryHost::GetServerPort() : StoryHost::GetAutoPort();
            const std::string nick = story ? StoryHost::GetNickname() : StoryHost::GetAutoNickname();
            // The widget is drawn after this function returned: capture a finished string by value.
            const std::string status = fmt::format("{}: connecting to {}:{} (retrying every 5 s; F8: connect <host> <port>)", story ? "STORY HOST" : "AUTO-CONNECT", host, port);
            gApplication->GetImGUI()->PushWidget([status]() {
                Framework::External::ImGUI::Widgets::DrawCornerText(Framework::External::ImGUI::Widgets::CORNER_RIGHT_TOP, status);
            });
            if (now < _storyRetryAt) {
                return false;
            }
            _storyRetryAt = now + 5.0; // if the connection state bounces back here, try again in 5 s
            Framework::Integrations::Client::CurrentState state = gApplication->GetCurrentState();
            state.host                                          = host;
            state.port                                          = port;
            state.nickname                                      = nick;
            gApplication->SetCurrentState(state);
            Framework::Logging::GetLogger("Story")->info("[Story] auto-connecting to {}:{} as \"{}\"", state.host, state.port, state.nickname);
            machine->RequestNextState(StateIds::SessionConnection);
            return true;
        }

        auto const view = gApplication->GetWebManager()->GetView(gApplication->GetMainMenuViewId());
        if (!view) {
            return false;
        }

        if (_shouldProceedOfflineDebug) {
            machine->RequestNextState(StateIds::SessionOfflineDebug);
        }
        if (_shouldProceedConnection) {
            machine->RequestNextState(StateIds::SessionConnection);
        }
        return _shouldProceedConnection || _shouldProceedOfflineDebug;
    }
} // namespace MafiaMP::Core::States
