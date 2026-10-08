#include <MinHook.h>
#include <utils/hooking/hook_function.h>

#include "sdk/patterns.h"

#include "sdk/c_stream_map.h"

#include "core/story_host.h"

#include <logging/logger.h>

#include <fmt/core.h>
#include <utility>

// The story host logs these at info under "Story": the host's log is where mission/part names
// are discovered. Everywhere else they stay debug noise under "Hooks".
template <typename... Args>
static void StreamMapLog(fmt::format_string<Args...> format, Args &&...args) {
    if (MafiaMP::Core::IsStoryHost()) {
        Framework::Logging::GetLogger("Story")->info(format, std::forward<Args>(args)...);
    }
    else {
        Framework::Logging::GetLogger("Hooks")->debug(format, std::forward<Args>(args)...);
    }
}

typedef void(__fastcall *C_StreamMap__OpenGame_t)(SDK::C_StreamMap *, const char *);
C_StreamMap__OpenGame_t C_StreamMap__OpenGame_original = nullptr;
void C_StreamMap__OpenGame(SDK::C_StreamMap *pThis, const char *game) {
    C_StreamMap__OpenGame_original(pThis, game);
    StreamMapLog("C_StreamMap::OpenGame: Opened game {}", game ? game : "(null)");
}

typedef void(__fastcall *C_StreamMap__OpenMission_t)(SDK::C_StreamMap *, const char *);
C_StreamMap__OpenMission_t C_StreamMap__OpenMission_original = nullptr;
void C_StreamMap__OpenMission(SDK::C_StreamMap *pThis, const char *mission) {
    C_StreamMap__OpenMission_original(pThis, mission);
    StreamMapLog("C_StreamMap::OpenMission: Opened mission {}", mission ? mission : "(null)");
}

typedef void(__fastcall *C_StreamMap__OpenPart_t)(SDK::C_StreamMap *, const char *);
C_StreamMap__OpenPart_t C_StreamMap__OpenPart_original = nullptr;
void C_StreamMap__OpenPart(SDK::C_StreamMap *pThis, const char *part) {
    C_StreamMap__OpenPart_original(pThis, part);
    StreamMapLog("C_StreamMap::OpenPart: Opened part {}", part ? part : "(null)");
}

typedef void(__fastcall *C_StreamMap__CloseGame_t)(SDK::C_StreamMap *);
C_StreamMap__CloseGame_t C_StreamMap__CloseGame_original = nullptr;
void C_StreamMap__CloseGame(SDK::C_StreamMap *pThis) {
    const auto game = pThis->GetGame();
    C_StreamMap__CloseGame_original(pThis);
    StreamMapLog("C_StreamMap::CloseGame: Closed game {}", game ? game : "(null)");
}

typedef void(__fastcall *C_StreamMap__CloseMission_t)(SDK::C_StreamMap *);
C_StreamMap__CloseMission_t C_StreamMap__CloseMission_original = nullptr;
void C_StreamMap__CloseMission(SDK::C_StreamMap *pThis) {
    const auto mission = pThis->GetMission();
    C_StreamMap__CloseMission_original(pThis);
    StreamMapLog("C_StreamMap::CloseMission: Closed mission {}", mission ? mission : "(null)");
}

typedef void(__fastcall *C_StreamMap__ClosePart_t)(SDK::C_StreamMap *);
C_StreamMap__ClosePart_t C_StreamMap__ClosePart_original = nullptr;
void C_StreamMap__ClosePart(SDK::C_StreamMap *pThis) {
    const auto part = pThis->GetPart();
    C_StreamMap__ClosePart_original(pThis);
    StreamMapLog("C_StreamMap::ClosePart: Closed part {}", part ? part : "(null)");
}

static InitFunction init([]() {
    MH_CreateHook((LPVOID)SDK::gPatterns.C_StreamMap__OpenGame, (PBYTE)C_StreamMap__OpenGame, reinterpret_cast<void **>(&C_StreamMap__OpenGame_original));
    MH_CreateHook((LPVOID)SDK::gPatterns.C_StreamMap__OpenMission, (PBYTE)C_StreamMap__OpenMission, reinterpret_cast<void **>(&C_StreamMap__OpenMission_original));
    MH_CreateHook((LPVOID)SDK::gPatterns.C_StreamMap__OpenPart, (PBYTE)C_StreamMap__OpenPart, reinterpret_cast<void **>(&C_StreamMap__OpenPart_original));
    MH_CreateHook((LPVOID)SDK::gPatterns.C_StreamMap__CloseGame, (PBYTE)C_StreamMap__CloseGame, reinterpret_cast<void **>(&C_StreamMap__CloseGame_original));
    MH_CreateHook((LPVOID)SDK::gPatterns.C_StreamMap__CloseMission, (PBYTE)C_StreamMap__CloseMission, reinterpret_cast<void **>(&C_StreamMap__CloseMission_original));
    MH_CreateHook((LPVOID)SDK::gPatterns.C_StreamMap__ClosePart, (PBYTE)C_StreamMap__ClosePart, reinterpret_cast<void **>(&C_StreamMap__ClosePart_original));
    },
    "StreamMap");
