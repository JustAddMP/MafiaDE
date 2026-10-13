// mafiacoop.dll entry. DllMain only signals the launcher and spawns the init thread; all setup happens there.
#include "core/config.h"
#include "core/crash.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/mem.h"
#include "dll/mod.h"
#include "render/present_hook.h"

#include <windows.h>
#include <cstdio>
#include <string>

namespace mc {
namespace {

HMODULE _module = nullptr;

std::wstring ModuleDir(HMODULE mod) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring s(path, n);
    size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

struct WindowSearch {
    DWORD pid;
    HWND found;
};

BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM lp) {
    auto* search = reinterpret_cast<WindowSearch*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (rc.right - rc.left < 320 || rc.bottom - rc.top < 200) return TRUE;  // skip splash/tool windows
    search->found = hwnd;
    return FALSE;
}

HWND WaitForGameWindow(DWORD timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    WindowSearch search{GetCurrentProcessId(), nullptr};
    while (GetTickCount64() - start < timeoutMs) {
        EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&search));
        if (search.found) return search.found;
        Sleep(250);
    }
    return nullptr;
}

void LogEnvironment(const std::wstring& dir) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto& main = core::mem::MainModule();
    MC_LOG_INFO("MafiaCoop native, built " __DATE__ " " __TIME__);
    MC_LOG_INFO("pid %lu, exe %s", GetCurrentProcessId(), Narrow(exe).c_str());
    MC_LOG_INFO("main module base 0x%llx size 0x%zx timestamp 0x%08x, .text 0x%llx+0x%zx",
                static_cast<unsigned long long>(main.base), main.size, main.timeDateStamp,
                static_cast<unsigned long long>(main.text.start), main.text.size);
    MC_LOG_INFO("dll dir %s", Narrow(dir).c_str());
    char appId[32] = {};
    GetEnvironmentVariableA("SteamAppId", appId, sizeof(appId));
    MC_LOG_INFO("env SteamAppId=%s", appId[0] ? appId : "(unset)");
}

// One log per game instance: --log <tag> -> mafiacoop-<tag>.log. Without it the first instance writes
// mafiacoop.log and any further concurrent instance mafiacoop-<pid>.log (detected with a named mutex that the
// first instance keeps for its lifetime), so two games on one PC never rotate or interleave each other's log.
std::wstring LogFileName() {
    wchar_t tag[64] = {};
    DWORD n = GetEnvironmentVariableW(L"MAFIACOOP_LOG", tag, 64);
    if (n > 0 && n < 64) return std::wstring(L"mafiacoop-") + tag + L".log";
    HANDLE m = CreateMutexW(nullptr, FALSE, L"Local\\MafiaCoopPrimaryLog");  // intentionally never closed
    if (m && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);
        return L"mafiacoop-" + std::to_wstring(GetCurrentProcessId()) + L".log";
    }
    return L"mafiacoop.log";
}

DWORD WINAPI InitThread(LPVOID) {
    const std::wstring dir = ModuleDir(_module);
    core::log::Init(dir + L"\\logs", LogFileName().c_str());
    LogEnvironment(dir);

    auto& cfg = core::Config::Get();
    if (cfg.Load(dir + L"\\MafiaCoop.ini")) {
        MC_LOG_INFO("config: loaded %s (%zu keys)", Narrow(cfg.Path()).c_str(), cfg.Values().size());
    } else {
        MC_LOG_INFO("config: %s not found, using defaults", Narrow(cfg.Path()).c_str());
    }
    for (const char* key : {"host", "join", "join_lobby", "transport", "name", "port", "log", "steam"}) {
        if (cfg.Has(key)) MC_LOG_INFO("config: %s = %s", key, cfg.GetString(key).c_str());
    }

    core::crash::Install(dir + L"\\logs");
    if (!core::hook::Init()) return 1;

    const DWORD timeoutMs = static_cast<DWORD>(cfg.GetInt("window_timeout_s", 300)) * 1000;
    MC_LOG_INFO("init: waiting for the game window");
    HWND hwnd = WaitForGameWindow(timeoutMs);
    if (!hwnd) {
        MC_LOG_ERROR("init: game window did not appear within %lu s; not hooking", timeoutMs / 1000);
        return 1;
    }
    char title[128] = {}, cls[128] = {};
    GetWindowTextA(hwnd, title, sizeof(title));
    GetClassNameA(hwnd, cls, sizeof(cls));
    MC_LOG_INFO("init: game window %p class '%s' title '%s'", hwnd, cls, title);

    Mod::Init();
    if (!render::InstallPresentHook(&Mod::Tick, &Mod::OnResize, &Mod::OnBeforeResize)) {
        MC_LOG_ERROR("init: Present hook failed; mod inactive");
        return 1;
    }
    MC_LOG_INFO("init: done");
    return 0;
}

} // namespace
} // namespace mc

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        mc::_module = module;
        DisableThreadLibraryCalls(module);
        // Tell the launcher the dll is in (event created by MafiaCoop.exe; absent when injected otherwise).
        wchar_t name[64];
        swprintf_s(name, L"Local\\MafiaCoopInjected-%lu", GetCurrentProcessId());
        if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name)) {
            SetEvent(ev);
            CloseHandle(ev);
        }
        if (HANDLE t = CreateThread(nullptr, 0, mc::InitThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
