#include <Windows.h>

#include <launcher/project.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace {
    // The game imports dinput8.dll. Third-party script hooks (NOMAD ScriptHook) ship a proxy
    // dinput8.dll in the game folder, and because the PE loader searches the game directory before
    // System32 that proxy would be loaded into the MafiaMP process and hook the same engine
    // functions MafiaMP does. Always resolve dinput8.dll to the system copy so the game folder can
    // keep its ScriptHook installed without any file renaming.
    HMODULE LoadSystemDInput8(const char *library) {
        std::string name(library ? library : "");
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name != "dinput8.dll" && name != "dinput8") {
            return nullptr;
        }
        wchar_t systemDir[MAX_PATH] = {};
        if (GetSystemDirectoryW(systemDir, MAX_PATH) == 0) {
            return nullptr;
        }
        const std::wstring path = std::wstring(systemDir) + L"\\dinput8.dll";
        return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
} // namespace

int main(void) {
    Framework::Launcher::ProjectConfiguration config;
    config.destinationDllName = L"MafiaMPClient.dll";
    config.executableName     = L"mafiadefinitiveedition.exe";
    config.name               = "MafiaMP";
    config.platform           = Framework::Launcher::ProjectPlatform::STEAM;
    config.steamAppId         = 1030840;
    config.verifyGameIntegrity = true;
    config.supportedGameVersions = {3168979183};

#ifdef FW_DEBUG
    config.allocateDeveloperConsole = true;
    config.developerConsoleTitle = L"mafiamp: dev-console";
#endif

    Framework::Launcher::Project project(config);
    project.SetLibraryLoader([](const char *library) -> HMODULE { return LoadSystemDInput8(library); });
    if (!project.Launch()) {
        return 1;
    }
    return 0;
}
