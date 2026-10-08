#include "session_connection.h"
#include "states.h"

#include "../application.h"
#include "../story_host.h"

#include <logging/logger.h>

#include <external/imgui/widgets/corner_text.h>

#include <utils/states/machine.h>

namespace MafiaMP::Core::States {
    SessionConnectionState::SessionConnectionState() {}

    SessionConnectionState::~SessionConnectionState() {}

    int32_t SessionConnectionState::GetId() const {
        return StateIds::SessionConnection;
    }

    const char *SessionConnectionState::GetName() const {
        return "SessionConnection";
    }

    bool SessionConnectionState::OnEnter(Framework::Utils::States::Machine *machine) {
        const auto appState = MafiaMP::Core::gApplication->GetCurrentState();
        if (!IsStoryHost()) {
            gApplication->LockControls(true); // the story host keeps playing while connecting
        }

        if (const auto result = MafiaMP::Core::gApplication->ConnectToServer(appState.host, appState.port, ""); !result) {
            Framework::Logging::GetInstance()->Get("SessionConnectionState")->error("Connection to server failed: {}", result.GetError().message);
            machine->RequestNextState(StateIds::MainMenu);
            return true;
        }
        return true;
    }

    bool SessionConnectionState::OnExit(Framework::Utils::States::Machine *) {
        if (!IsStoryHost()) {
            gApplication->LockControls(false);
        }
        return true;
    }

    bool SessionConnectionState::OnUpdate(Framework::Utils::States::Machine *) {
        gApplication->GetImGUI()->PushWidget([&]() {
            Framework::External::ImGUI::Widgets::DrawCornerText(Framework::External::ImGUI::Widgets::CORNER_RIGHT_TOP, "CONNECTING...");
        });
        return false;
    }
} // namespace MafiaMP::Core::States
