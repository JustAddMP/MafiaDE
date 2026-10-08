#include <utils/safe_win32.h>
#include <cstdlib>
#include <MinHook.h>
#include <utils/hooking/hook_function.h>

#include "../application_module.h"
#include "../story_host.h"

#include "sdk/patterns.h"
#include "sdk/mafia/ui/c_game_gui_2_module.h"
#include "sdk/mafia/ui/menu/c_save_menu.h"
#include "sdk/ue/c_string.h"

#include <logging/logger.h>

typedef void(__fastcall *C_InitDone__Init_MafiaFramework_t)(void *_this);
C_InitDone__Init_MafiaFramework_t C_InitDone__Init_MafiaFramework_original = nullptr;
void __fastcall C_InitDone__MafiaFramework(void *_this) {
    C_InitDone__Init_MafiaFramework_original(_this);

    // Register a late atexit handler to prevent the game's broken cleanup from crashing.
    // The game has an atexit ordering bug: its RefCountManager cleanup runs before
    // BehaviorProfile cleanup, leaving a NULL allocator array that causes an access
    // violation. Since this runs in LIFO order, registering here ensures we terminate
    // the process before those handlers run.
    std::atexit([]() {
        TerminateProcess(GetCurrentProcess(), 0);
    });

    MafiaMP::Core::gApplicationModule = new MafiaMP::Core::ApplicationModule();
}

int __fastcall C_CommandLine__FindCommand(void *_this, const char *command) {
    if (strstr(command, "NoMy2K") || strstr(command, "SkipLoadingPrompt") /*|| strstr(command, "fastRender")*/) {
        return 1;
    }
    return -1;
}

// This hooks should be disabled on release build - We should instead find a proper way to replace menu and bypass it
typedef void(__fastcall *C_MainMenu__QueueMenuSequenceScreen_t)(void *_this, int sequence, bool cannotSkip);
C_MainMenu__QueueMenuSequenceScreen_t C_MainMenu__QueueMenuSequenceScreen_original = nullptr;
void __fastcall C_MainMenu__QueueMenuSequenceScreen(void *_this, int sequence, bool cannotSkip) {
    Framework::Logging::GetLogger("Hooks")->debug("[MainMenu::QueueMenuSequenceScreen]: sequence {}, cannotSkip {}", sequence, cannotSkip);

    if (MafiaMP::Core::IsStoryHost()) {
        // Story host: the vanilla main menu runs (New Game / Chapters). A chapter named in
        // story_chapter.txt is launched directly instead, once; later menu visits (mission end,
        // quit to menu) show the vanilla menu again.
        static bool chapterLaunched = false;
        const auto &chapter         = MafiaMP::Core::StoryHost::GetChapterSavePath();
        Framework::Logging::GetLogger("Story")->info("[Story] MainMenu::QueueMenuSequenceScreen sequence {} cannotSkip {}", sequence, cannotSkip);
        if (sequence == -1 && !chapter.empty() && !chapterLaunched) {
            chapterLaunched = true;
            Framework::Logging::GetLogger("Story")->info("[Story] Loading chapter save \"{}\" from story_chapter.txt", chapter);
            auto menu     = SDK::mafia::ui::GetGameGui2Module();
            auto saveMenu = reinterpret_cast<SDK::mafia::ui::menu::C_MenuSave *>((uint64_t)menu->GetMainMenu());
            SDK::ue::C_String savePath(chapter.c_str());
            saveMenu->OpenDebugLoadChapterString(savePath, false);
            return;
        }
        C_MainMenu__QueueMenuSequenceScreen_original(_this, sequence, cannotSkip);
        return;
    }

    if (sequence == -1) {
        auto menu     = SDK::mafia::ui::GetGameGui2Module();
        auto saveMenu = reinterpret_cast<SDK::mafia::ui::menu::C_MenuSave *>((uint64_t)menu->GetMainMenu());
        SDK::ue::C_String freeride("02_lost_heaven/lh_freeride_extreme.sav");
        saveMenu->OpenDebugLoadChapterString(freeride, false);
    }
}

typedef volatile signed __int32 *(__fastcall *C_MainMenu__RunGame_t)(void *pThis, const char *, const char *);
C_MainMenu__RunGame_t C_MainMenu__RunGame_original = nullptr;
volatile signed __int32 *__fastcall C_MainMenu__RunGame(void *pThis, const char *mission, const char *part) {
    Framework::Logging::GetLogger("Hooks")->debug("[MainMenu::RunGame]: mission {}, part {}", mission, part);
    if (MafiaMP::Core::IsStoryHost()) {
        // Discovery: the mission/part pair the menu starts, so chapter names can be collected.
        Framework::Logging::GetLogger("Story")->info("[Story] MainMenu::RunGame mission \"{}\" part \"{}\"", mission ? mission : "(null)", part ? part : "(null)");
    }
    return C_MainMenu__RunGame_original(pThis, mission, part);
}

// Discovery: every chapter save the game (or we) load by path.
typedef void(__fastcall *C_MenuSave__OpenDebugLoadChapterString_t)(void *pThis, SDK::ue::C_String *savePath, bool unk);
C_MenuSave__OpenDebugLoadChapterString_t C_MenuSave__OpenDebugLoadChapterString_original = nullptr;
void __fastcall C_MenuSave__OpenDebugLoadChapterString(void *pThis, SDK::ue::C_String *savePath, bool unk) {
    const char *path = savePath ? savePath->c_str() : nullptr;
    if (MafiaMP::Core::IsStoryHost()) {
        Framework::Logging::GetLogger("Story")->info("[Story] MenuSave::OpenDebugLoadChapterString path \"{}\" unk {}", path ? path : "(null)", unk);
    }
    else {
        Framework::Logging::GetLogger("Hooks")->debug("[MenuSave::OpenDebugLoadChapterString]: path {}, unk {}", path ? path : "(null)", unk);
    }
    C_MenuSave__OpenDebugLoadChapterString_original(pThis, savePath, unk);
}


static InitFunction init([]() {
    MH_CreateHook((LPVOID)SDK::gPatterns.C_InitDone_MafiaFramework, (PBYTE)C_InitDone__MafiaFramework, reinterpret_cast<void **>(&C_InitDone__Init_MafiaFramework_original));

    // Disable the loading intro
    hook::return_function(SDK::gPatterns.LoadIntro);

    // Bypass the main menu and auto load the freeride chapter (the story host lets it run instead)
    const auto C_MainMenu__QueueMenuSequenceScreenAddr = hook::pattern("48 8B C4 48 89 58 ? 57 48 83 EC ? 48 8B D9 89 50 ?").get_first();
    MH_CreateHook((LPVOID)C_MainMenu__QueueMenuSequenceScreenAddr, (PBYTE)C_MainMenu__QueueMenuSequenceScreen, reinterpret_cast<void **>(&C_MainMenu__QueueMenuSequenceScreen_original));

    // Log every chapter save loaded by path
    MH_CreateHook((LPVOID)SDK::gPatterns.C_MenuSave__OpenDebugLoadChapterString, (PBYTE)C_MenuSave__OpenDebugLoadChapterString, reinterpret_cast<void **>(&C_MenuSave__OpenDebugLoadChapterString_original));

    // Listen for the game run calls
    const auto C_MainMenu__RunGame_Addr = hook::get_pattern("40 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 ? 48 81 EC ? ? ? ? 4C 8B 35");
    MH_CreateHook((LPVOID)C_MainMenu__RunGame_Addr, (PBYTE)C_MainMenu__RunGame, reinterpret_cast<void **>(&C_MainMenu__RunGame_original));

    // Skip loading prompt & debug stuff
    MH_CreateHook((LPVOID)SDK::gPatterns.C_CommandLine__FindCommand, (PBYTE)C_CommandLine__FindCommand, nullptr);
    },
    "ModInit");
