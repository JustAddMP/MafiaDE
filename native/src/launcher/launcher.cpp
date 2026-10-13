// MafiaCoop.exe: finds Mafia: Definitive Edition, starts it suspended with SteamAppId set, injects
// mafiacoop.dll (from this exe's directory) and resumes it.
//
// Usage: MafiaCoop.exe [--host] [--join <ip[:port]>] [--join-lobby <id>] [--name <name>] [--log <tag>]
//                      [--<key> [value]] [--game <exe path>] [+connect_lobby <id>]
// Every --<key> [value] (except --game) is passed to the dll as environment variable MAFIACOOP_<KEY>=<value>
// ("1" for a bare flag); core::Config gives these precedence over MafiaCoop.ini.
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace mc::launcher {
namespace {

constexpr const wchar_t* kGameExe = L"mafiadefinitiveedition.exe";
constexpr const wchar_t* kAppId = L"1030840";
constexpr const wchar_t* kDllName = L"mafiacoop.dll";

FILE* _log = nullptr;

void Log(const wchar_t* fmt, ...) {
    if (!_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(_log, L"%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfwprintf(_log, fmt, args);
    va_end(args);
    fputwc(L'\n', _log);
    fflush(_log);
}

int Fail(const std::wstring& msg) {
    Log(L"ERROR: %s", msg.c_str());
    MessageBoxW(nullptr, msg.c_str(), L"MafiaCoop", MB_ICONERROR | MB_OK);
    return 1;
}

std::wstring LastErrorText(DWORD err = GetLastError()) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return L"error " + std::to_wstring(err) + (s.empty() ? L"" : L": " + s);
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return {};
    return s.substr(b, s.find_last_not_of(L" \t\r\n") - b + 1);
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring ReadTextFile(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return Widen(ss.str());
}

std::wstring NormalizePath(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    return p;
}

// --- MafiaCoop.ini (only the launcher's keys) --------------------------------------------------

std::wstring IniValue(const std::wstring& iniPath, const std::wstring& key) {
    std::wstring text = ReadTextFile(iniPath);
    std::wistringstream in(text);
    std::wstring line;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == 0xFEFF) line.erase(0, 1);
        std::wstring t = Trim(line);
        if (t.empty() || t[0] == L'#' || t[0] == L';') continue;
        size_t eq = t.find(L'=');
        if (eq == std::wstring::npos) continue;
        if (Lower(Trim(t.substr(0, eq))) != key) continue;
        std::wstring v = Trim(t.substr(eq + 1));
        if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') v = v.substr(1, v.size() - 2);
        return v;
    }
    return {};
}

// --- Steam library discovery -------------------------------------------------------------------

std::wstring RegString(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

// All "key" "value" pairs with the given key in a Valve KeyValues text file (values unescaped).
std::vector<std::wstring> VdfValues(const std::wstring& text, const std::wstring& key) {
    std::vector<std::wstring> out;
    const std::wstring needle = L"\"" + key + L"\"";
    size_t pos = 0;
    std::wstring lowerText = Lower(text);
    while ((pos = lowerText.find(Lower(needle), pos)) != std::wstring::npos) {
        pos += needle.size();
        size_t open = text.find(L'"', pos);
        size_t nl = text.find(L'\n', pos);
        if (open == std::wstring::npos || (nl != std::wstring::npos && open > nl)) continue;  // it was a block key
        std::wstring value;
        size_t i = open + 1;
        for (; i < text.size() && text[i] != L'"'; ++i) {
            if (text[i] == L'\\' && i + 1 < text.size()) ++i;
            value += text[i];
        }
        out.push_back(value);
        pos = i;
    }
    return out;
}

std::wstring FindGameViaSteam() {
    std::vector<std::wstring> steamRoots;
    for (auto [root, sub, val] : {std::tuple{HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"},
                                  std::tuple{HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"},
                                  std::tuple{HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"}}) {
        std::wstring p = RegString(root, sub, val);
        if (!p.empty()) steamRoots.push_back(NormalizePath(p));
    }
    if (steamRoots.empty()) {
        Log(L"steam: no Steam install found in the registry");
        return {};
    }
    std::vector<std::wstring> libraries;
    for (const auto& root : steamRoots) {
        Log(L"steam: root %s", root.c_str());
        libraries.push_back(root);
        std::wstring vdf = ReadTextFile(root + L"\\steamapps\\libraryfolders.vdf");
        for (auto& lib : VdfValues(vdf, L"path")) libraries.push_back(NormalizePath(lib));
    }
    for (const auto& lib : libraries) {
        std::wstring manifest = lib + L"\\steamapps\\appmanifest_" + kAppId + L".acf";
        std::wstring acf = ReadTextFile(manifest);
        if (acf.empty()) continue;
        auto dirs = VdfValues(acf, L"installdir");
        std::wstring installDir = dirs.empty() ? L"Mafia Definitive Edition" : dirs.front();
        std::wstring exe = lib + L"\\steamapps\\common\\" + installDir + L"\\" + kGameExe;
        Log(L"steam: manifest %s -> %s", manifest.c_str(), exe.c_str());
        if (FileExists(exe)) return exe;
    }
    Log(L"steam: app %s not found in %zu libraries", kAppId, libraries.size());
    return {};
}

// --- process helpers ---------------------------------------------------------------------------

std::vector<DWORD> FindProcesses(const wchar_t* exeName) {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exeName) == 0) pids.push_back(pe.th32ProcessID);
    }
    CloseHandle(snap);
    return pids;
}

// Current environment minus Steam/MafiaCoop vars, plus ours; sorted, double-NUL terminated.
std::vector<wchar_t> BuildEnvironment(const std::vector<std::pair<std::wstring, std::wstring>>& extra) {
    std::vector<std::wstring> entries;
    wchar_t* env = GetEnvironmentStringsW();
    for (const wchar_t* p = env; p && *p; p += wcslen(p) + 1) {
        std::wstring e = p;
        std::wstring name = Lower(e.substr(0, e.find(L'=', 1)));
        if (name == L"steamappid" || name == L"steamgameid" || name.rfind(L"mafiacoop_", 0) == 0) continue;
        entries.push_back(e);
    }
    if (env) FreeEnvironmentStringsW(env);
    for (auto& [k, v] : extra) entries.push_back(k + L"=" + v);
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.substr(0, a.find(L'=', 1)).c_str(), b.substr(0, b.find(L'=', 1)).c_str()) < 0;
    });
    std::vector<wchar_t> block;
    for (auto& e : entries) {
        block.insert(block.end(), e.begin(), e.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

// Loads dllPath into a process via LoadLibraryW on a new remote thread. Returns true if LoadLibraryW succeeded.
bool InjectRemoteThread(HANDLE process, void* remotePath, DWORD& err) {
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE t = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
    if (!t) {
        err = GetLastError();
        return false;
    }
    DWORD wait = WaitForSingleObject(t, 15000);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    CloseHandle(t);
    err = wait == WAIT_OBJECT_0 ? (code ? 0 : ERROR_MOD_NOT_FOUND) : WAIT_TIMEOUT;
    return wait == WAIT_OBJECT_0 && code != 0;  // exit code = low 32 bits of the HMODULE
}

} // namespace

int Run() {
    wchar_t selfPath[MAX_PATH];
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    std::wstring selfDir = selfPath;
    selfDir = selfDir.substr(0, selfDir.find_last_of(L'\\'));
    const std::wstring iniPath = selfDir + L"\\MafiaCoop.ini";
    const std::wstring dllPath = selfDir + L"\\" + kDllName;

    // ---- arguments ----
    // --key [value] -> MAFIACOOP_KEY env var for the dll; +word args (e.g. "+connect_lobby <id>", which Steam
    // passes for "Join Game") go to the game's command line unchanged.
    std::vector<std::pair<std::wstring, std::wstring>> passEnv;
    std::wstring gameOverride, logTag, gameArgs;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a.size() > 1 && a[0] == L'+') {
            gameArgs += L" " + a;
            if (i + 1 < argc && argv[i + 1][0] != L'-' && argv[i + 1][0] != L'+') gameArgs += L" " + std::wstring(argv[++i]);
            continue;
        }
        if (a.rfind(L"--", 0) != 0 || a.size() < 3) {
            return Fail(L"Unexpected argument: " + a +
                        L"\n\nUsage: MafiaCoop.exe [--host] [--join <ip>] [--join-lobby <id>] [--name <name>] "
                        L"[--<key> [value]] [--game <exe>]");
        }
        std::wstring key = a.substr(2);
        std::wstring value = L"1";
        if (i + 1 < argc && std::wstring(argv[i + 1]).rfind(L"--", 0) != 0) value = argv[++i];
        if (Lower(key) == L"game") {
            gameOverride = value;
            continue;
        }
        if (Lower(key) == L"log") logTag = value;
        std::wstring envName = L"MAFIACOOP_";
        for (wchar_t c : key) envName += (c == L'-' || c == L'.') ? L'_' : static_cast<wchar_t>(towupper(c));
        passEnv.emplace_back(envName, value);
    }
    if (argv) LocalFree(argv);

    // ---- log: one per launch when several games run (launcher-<tag>.log / launcher-<pid>.log) ----
    const std::vector<DWORD> running = FindProcesses(kGameExe);
    std::wstring logName = L"launcher.log";
    if (!logTag.empty())
        logName = L"launcher-" + logTag + L".log";
    else if (!running.empty())
        logName = L"launcher-" + std::to_wstring(GetCurrentProcessId()) + L".log";
    CreateDirectoryW((selfDir + L"\\logs").c_str(), nullptr);
    _wfopen_s(&_log, (selfDir + L"\\logs\\" + logName).c_str(), L"w, ccs=UTF-8");
    Log(L"MafiaCoop launcher, built " __DATE__ " " __TIME__);
    for (auto& [k, v] : passEnv) Log(L"arg: %s=%s", k.c_str(), v.c_str());
    if (!gameArgs.empty()) Log(L"arg: game command line extra:%s", gameArgs.c_str());

    // ---- find the game ----
    std::wstring gameExe;
    if (!gameOverride.empty()) {
        gameExe = gameOverride;
        Log(L"game: from --game: %s", gameExe.c_str());
    } else {
        gameExe = FindGameViaSteam();
        if (gameExe.empty()) {
            std::wstring p = IniValue(iniPath, L"game_path");
            if (!p.empty()) {
                p = NormalizePath(p);
                gameExe = (Lower(p).size() > 4 && Lower(p).ends_with(L".exe")) ? p : p + L"\\" + kGameExe;
                Log(L"game: from MafiaCoop.ini game_path: %s", gameExe.c_str());
            }
        }
    }
    if (gameExe.empty()) {
        return Fail(L"Could not find Mafia: Definitive Edition (Steam app 1030840).\n\nAdd a line\n"
                    L"    game_path = <folder containing mafiadefinitiveedition.exe>\nto " + iniPath);
    }
    if (!FileExists(gameExe)) return Fail(L"Game executable not found:\n" + gameExe);
    if (!FileExists(dllPath)) return Fail(L"mafiacoop.dll not found next to MafiaCoop.exe:\n" + dllPath);
    const std::wstring gameDir = gameExe.substr(0, gameExe.find_last_of(L'\\'));
    Log(L"game: %s", gameExe.c_str());

    // A second instance is allowed (two games on one PC for testing, joined over UDP 127.0.0.1). The exe has no
    // working single-instance check (docs/re-notes.md 3.2); /multiinst also skips its leftover Mafia II mutex.
    if (!running.empty()) Log(L"note: %zu game instance(s) already running; starting another", running.size());

    // ---- start suspended ----
    passEnv.emplace_back(L"SteamAppId", kAppId);
    passEnv.emplace_back(L"SteamGameId", kAppId);
    std::vector<wchar_t> env = BuildEnvironment(passEnv);
    std::wstring cmdLine = L"\"" + gameExe + L"\" /multiinst" + gameArgs;
    Log(L"command line: %s", cmdLine.c_str());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(gameExe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, env.data(), gameDir.c_str(), &si, &pi)) {
        return Fail(L"Could not start the game:\n" + gameExe + L"\n\n" + LastErrorText());
    }
    Log(L"started pid %lu (suspended)", pi.dwProcessId);

    auto abortGame = [&](const std::wstring& why) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return Fail(why);
    };

    wchar_t eventName[64];
    swprintf_s(eventName, L"Local\\MafiaCoopInjected-%lu", pi.dwProcessId);
    HANDLE loaded = CreateEventW(nullptr, TRUE, FALSE, eventName);
    if (!loaded) return abortGame(L"CreateEvent failed: " + LastErrorText());

    const size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(pi.hProcess, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath || !WriteProcessMemory(pi.hProcess, remotePath, dllPath.c_str(), pathBytes, nullptr)) {
        return abortGame(L"Could not write into the game process: " + LastErrorText());
    }

    // ---- inject ----
    // Default: queue an APC to LoadLibraryW on the still-suspended main thread. The APC is delivered when the
    // loader finishes process initialization (after kernel32 and the exe's imports are loaded, before the exe's
    // entry point), so our hooks are in place before the game runs any code. kernel32 is mapped at the same
    // address in every process of a boot session, so our LoadLibraryW address is valid there.
    // Fallback (or inject_method=thread): CreateRemoteThread(LoadLibraryW) after resuming.
    std::wstring method = Lower(IniValue(iniPath, L"inject_method"));
    if (method.empty()) method = L"apc";
    Log(L"inject: method %s, dll %s", method.c_str(), dllPath.c_str());
    auto loadLibrary = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (method == L"apc") {
        if (!QueueUserAPC(reinterpret_cast<PAPCFUNC>(loadLibrary), pi.hThread, reinterpret_cast<ULONG_PTR>(remotePath))) {
            Log(L"inject: QueueUserAPC failed (%s), will use remote thread", LastErrorText().c_str());
            method = L"thread";
        }
    }
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) return abortGame(L"ResumeThread failed: " + LastErrorText());
    Log(L"resumed");

    HANDLE waits[2] = {loaded, pi.hProcess};
    DWORD w = WaitForMultipleObjects(2, waits, FALSE, method == L"apc" ? 15000 : 0);
    if (w == WAIT_OBJECT_0 + 1) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        return Fail(L"The game exited during startup (exit code " + std::to_wstring(code) +
                    L") before mafiacoop.dll was loaded.");
    }
    if (w != WAIT_OBJECT_0) {
        if (method == L"apc") Log(L"inject: APC did not load the dll within 15 s, falling back to remote thread");
        DWORD err = 0;
        if (!InjectRemoteThread(pi.hProcess, remotePath, err) ||
            WaitForSingleObject(loaded, 5000) != WAIT_OBJECT_0) {
            return abortGame(L"Could not load mafiacoop.dll into the game: " + LastErrorText(err));
        }
        method = L"thread";
    }
    Log(L"inject: dll loaded (via %s)", method.c_str());
    CloseHandle(loaded);

    // ---- watch the first seconds: a quick exit usually means Steam restarted the game without us ----
    if (WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        Sleep(2000);
        bool relaunched = false;  // a game process that was not running before we started ours
        for (DWORD p : FindProcesses(kGameExe))
            if (std::find(running.begin(), running.end(), p) == running.end()) relaunched = true;
        Log(L"game exited early with code %lu (relaunched by Steam: %s)", code, relaunched ? L"yes" : L"no");
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return Fail(relaunched ? L"The game was restarted through Steam, so the co-op mod is not loaded. "
                                 L"Make sure Steam is running and try again."
                               : L"The game exited right after starting (exit code " + std::to_wstring(code) +
                                     L"). See logs\\mafiacoop.log.");
    }
    Log(L"game running (pid %lu); launcher done", pi.dwProcessId);
    VirtualFreeEx(pi.hProcess, remotePath, 0, MEM_RELEASE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

} // namespace mc::launcher

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int rc = mc::launcher::Run();
    if (mc::launcher::_log) fclose(mc::launcher::_log);
    return rc;
}
