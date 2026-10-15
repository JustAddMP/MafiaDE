#include "game/save_guard.h"

#include "core/hook.h"
#include "core/log.h"
#include "core/mem.h"

#include <windows.h>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

namespace mc::game::save_guard {
namespace {

// ---- the save system's load request (docs/re-notes.md 7) --------------------------------------------------------------
//   Handle* LoadRequest(this = slot storage, Handle* out, uint32_t slot, C_String id, C_String storageName)
// (by-value strings arrive as pointers). It queues the message whose handler logs "Loading save (Id: ...)". Two
// sibling requests of the same shape (0x38-byte messages) follow every load; they are not hooked.
using RequestFn = void* (*)(void* self, void* out, uint32_t slot, void* id, void* name);
constexpr const char* kPatLoadRequest =
    "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 20 48 8B 01 48 8B FA 48 8B 6C 24 70 49 8B F1 "
    "48 8B D5 45 8B F8 4C 8B F1 FF 50 40 B9 48 00 00 00";

RequestFn _origLoad = nullptr;
// slot storage lookup (exe+0x2E6E320): Info* Find(this, uint32_t slot, C_String id) over the known saves list
using FindFn = void* (*)(void* self, uint32_t slot, void* id);
constexpr const char* kPatFind =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 30 49 8B 00 8B EA 48 8B F1 48 89 44 24 40 49 8B D0 "
    "48 8D 4C 24 40 49 8B F8 33 DB";
FindFn _origFind = nullptr;
std::atomic<bool> _block{false};
std::atomic<uint32_t> _redirected{0};
std::atomic<uint32_t> _aborts{0};
uintptr_t _exe = 0;
std::mutex _mu;
std::string _scratch;  // ...\MafiaCoop\coop-saves\<pid> (no trailing slash)

std::string Str(void* cstring) {  // a C_String passed by value: a pointer to the object, whose first field is char*
    if (!cstring) return "(null)";
    const char* s = *static_cast<const char* const*>(cstring);
    if (!s) return "";
    std::string out;
    for (size_t i = 0; i < 120 && s[i]; ++i) out += s[i];
    return out;
}

std::string Callers() {
    void* frames[8] = {};
    const USHORT n = RtlCaptureStackBackTrace(2, 8, frames, nullptr);
    std::string out;
    char buf[32];
    for (USHORT i = 0; i < n; ++i) {
        const auto a = reinterpret_cast<uintptr_t>(frames[i]);
        if (a < _exe || a >= _exe + 0x8000000) continue;
        std::snprintf(buf, sizeof buf, " exe+%llx", static_cast<unsigned long long>(a - _exe));
        out += buf;
    }
    return out;
}

// chapter loads through the "debugloadchapter" storage (FindDetour below)
std::mutex _dbgMu;
std::string _dbgId;          // save id of the chapter load in progress
ULONGLONG _dbgAt = 0;        // when it was requested
void* _dbgSelf = nullptr;    // the debugloadchapter storage and its entry (first successful lookup)
void* _dbgEntry = nullptr;
uint32_t _dbgSlot = 0;

void* LoadDetour(void* self, void* out, uint32_t slot, void* id, void* name) {
    const std::string saveId = Str(id), storage = Str(name);
    MC_LOG_INFO("savesys: load of '%s' (slot %u, %s) from%s", saveId.c_str(), slot, storage.c_str(), Callers().c_str());
    if (saveId == "freeroam_before_quest_activated") {
        ++_aborts;
        MC_LOG_INFO("savesys: the game leaves the mission (back to the free roam state before the quest)");
    }
    if (storage == "debugloadchapter") {
        std::lock_guard lock(_dbgMu);
        _dbgId = saveId;
        _dbgAt = GetTickCount64();
    }
    return _origLoad(self, out, slot, id, name);
}

// The main menu's chapter loader (our in-memory loads of the host's save) puts its save into the "debugloadchapter"
// storage. That storage keeps ONE entry, keyed by the save id of the first save loaded through it in this process; a
// later load of a save with another id finds no entry, and the load message handler dereferences the null result
// (exe+0x2E81EF8, the guest reload crash, docs/re-notes.md 7). The entry itself is reused for every such load (its
// data is replaced by each new load), so for a chapter load in progress a failed lookup in that storage gets the
// entry of the earlier one.

void* FindDetour(void* self, uint32_t slot, void* id) {
    const std::string name = Str(id);  // read before the call: the callee releases the by-value string
    void* r = _origFind(self, slot, id);
    std::lock_guard lock(_dbgMu);
    const bool loading = !_dbgId.empty() && GetTickCount64() - _dbgAt < 120000;
    if (r) {
        if (loading && name == _dbgId && !_dbgEntry) {
            _dbgSelf = self;
            _dbgEntry = r;
            _dbgSlot = slot;
        }
        return r;
    }
    if (loading && name == _dbgId && self == _dbgSelf && slot == _dbgSlot && _dbgEntry) {
        MC_LOG_INFO("savesys: chapter load of '%s': the storage only knows its first save id, using its entry %p",
                    name.c_str(), _dbgEntry);
        return _dbgEntry;
    }
    MC_LOG_WARN("savesys: no save '%s' in slot %u (storage %p)", name.c_str(), slot, self);
    return r;
}

// ---- the save files (Documents\My Games\Mafia Definitive Edition\Data\<id>\gamesaves\<profile>\<slot>\saveN.sav) ----
// The game reads and writes them with CreateFileA from its file layer (paths with mixed / and \). While Block() is
// on, every access below "gamesaves" goes to a scratch copy of the tree instead: writes always, reads when the scratch
// file exists (so a Retry reads the checkpoint written in this session), deletes and copies likewise. The real files
// are never opened for writing.
std::string Redirect(const char* path, bool write) {
    if (!path || !_block.load()) return {};
    std::string p(path);
    for (char& c : p)
        if (c == '/') c = '\\';
    std::string lower = p;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t at = lower.find("\\gamesaves\\");
    if (at == std::string::npos) return {};
    std::string scratch;
    {
        std::lock_guard lock(_mu);
        scratch = _scratch;
    }
    if (scratch.empty()) return {};
    std::string target = scratch + p.substr(at);  // ...\coop-saves\<pid>\gamesaves\...
    while (target.find("\\\\") != std::string::npos) target.replace(target.find("\\\\"), 2, "\\");
    if (write) {
        for (size_t i = scratch.size() + 1; (i = target.find('\\', i)) != std::string::npos; ++i)
            CreateDirectoryA(target.substr(0, i).c_str(), nullptr);
    } else if (GetFileAttributesA(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return {};  // nothing written in this session: the real file is read
    }
    return target;
}

using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using DeleteFileAFn = BOOL(WINAPI*)(LPCSTR);
using CopyFileExAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR, LPPROGRESS_ROUTINE, LPVOID, LPBOOL, DWORD);
CreateFileAFn _origCreateFileA = nullptr;
DeleteFileAFn _origDeleteFileA = nullptr;
CopyFileExAFn _origCopyFileExA = nullptr;

HANDLE WINAPI CreateFileADetour(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                DWORD flags, HANDLE tmpl) {
    const bool write = (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) != 0 ||
                       disp == CREATE_ALWAYS || disp == CREATE_NEW || disp == TRUNCATE_EXISTING;
    const std::string to = Redirect(name, write);
    if (to.empty()) {
        // the profile and save files written for real (diagnostics: what the game writes outside our redirect)
        if (write && name && (std::strstr(name, "gamesaves") || std::strstr(name, "profiles")) &&
            std::strstr(name, "Mafia Definitive Edition"))
            MC_LOG_INFO("savesys: written: %s", std::strrchr(name, '/') ? std::strrchr(name, '/') + 1 : name);
        return _origCreateFileA(name, access, share, sa, disp, flags, tmpl);
    }
    if (write) {
        const uint32_t n = ++_redirected;
        if (n <= 50 || n % 50 == 0) MC_LOG_INFO("savesys: save write #%u kept out of the real slot: %s -> %s", n, name, to.c_str());
    }
    return _origCreateFileA(to.c_str(), access, share, sa, disp, flags, tmpl);
}

BOOL WINAPI DeleteFileADetour(LPCSTR name) {
    const std::string to = Redirect(name, true);
    if (!to.empty()) {
        _origDeleteFileA(to.c_str());
        return TRUE;  // the real file stays
    }
    return _origDeleteFileA(name);
}

BOOL WINAPI CopyFileExADetour(LPCSTR from, LPCSTR to, LPPROGRESS_ROUTINE cb, LPVOID data, LPBOOL cancel, DWORD flags) {
    const std::string dst = Redirect(to, true);
    if (dst.empty()) return _origCopyFileExA(from, to, cb, data, cancel, flags);
    const std::string src = Redirect(from, false);
    return _origCopyFileExA(src.empty() ? from : src.c_str(), dst.c_str(), cb, data, cancel, flags);
}

void ClearScratch(const std::string& dir) {
    // remove the tree a previous block left (files first, then folders); best effort
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::string n = fd.cFileName;
        if (n == "." || n == "..") continue;
        const std::string p = dir + "\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ClearScratch(p);
            RemoveDirectoryA(p.c_str());
        } else {
            _origDeleteFileA ? _origDeleteFileA(p.c_str()) : DeleteFileA(p.c_str());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

}  // namespace

// Scratch trees of game processes that are gone (crashed, killed): removed at start.
void RemoveStale(const std::string& root) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        const DWORD pid = std::strtoul(fd.cFileName, nullptr, 10);
        if (pid == GetCurrentProcessId()) continue;
        HANDLE p = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
        if (p) {
            CloseHandle(p);
            continue;
        }
        const std::string dir = root + "\\" + fd.cFileName;
        ClearScratch(dir);
        RemoveDirectoryA(dir.c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void Install() {
    _exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (const uintptr_t a = core::mem::FindPattern(kPatLoadRequest))
        core::hook::Install("SaveSys_LoadRequest", reinterpret_cast<void*>(a), &LoadDetour, &_origLoad);
    else
        MC_LOG_WARN("savesys: load request not found (save loads are not logged)");
    if (const uintptr_t a = core::mem::FindPattern(kPatFind))
        core::hook::Install("SaveSys_Find", reinterpret_cast<void*>(a), &FindDetour, &_origFind);
    char tmp[MAX_PATH];
    const DWORD n = GetTempPathA(MAX_PATH, tmp);
    if (n && n < MAX_PATH) {
        std::lock_guard lock(_mu);
        _scratch = std::string(tmp) + "MafiaCoop";
        CreateDirectoryA(_scratch.c_str(), nullptr);
        _scratch += "\\coop-saves";
        CreateDirectoryA(_scratch.c_str(), nullptr);
        RemoveStale(_scratch);
        _scratch += "\\" + std::to_string(GetCurrentProcessId());
    }
    bool ok = false;
    if (HMODULE kb = GetModuleHandleW(L"kernelbase.dll")) {
        ok = core::hook::Install("SaveGuard_CreateFileA", reinterpret_cast<void*>(GetProcAddress(kb, "CreateFileA")),
                                 &CreateFileADetour, &_origCreateFileA);
        core::hook::Install("SaveGuard_DeleteFileA", reinterpret_cast<void*>(GetProcAddress(kb, "DeleteFileA")),
                            &DeleteFileADetour, &_origDeleteFileA);
    }
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
        core::hook::Install("SaveGuard_CopyFileExA", reinterpret_cast<void*>(GetProcAddress(k32, "CopyFileExA")),
                            &CopyFileExADetour, &_origCopyFileExA);
    if (!ok) MC_LOG_WARN("savesys: file hooks failed: a following guest's checkpoints would go into his own save slot");
}

void Block(bool on) {
    if (_block.load() == on) return;
    std::string dir;
    {
        std::lock_guard lock(_mu);
        dir = _scratch;
    }
    if (on && !dir.empty()) {
        ClearScratch(dir);
        CreateDirectoryA(dir.c_str(), nullptr);
    }
    _block = on;
    if (!on && !dir.empty()) {
        ClearScratch(dir);
        RemoveDirectoryA(dir.c_str());
    }
    MC_LOG_INFO("savesys: %s", on ? ("saves now go to " + dir + " (a guest in a co-op session: his real save slots are "
                                     "not touched)").c_str()
                                  : "saves go to the real save slots again");
}

bool Blocking() { return _block.load(); }
uint32_t Redirected() { return _redirected.load(); }
uint32_t MissionAborts() { return _aborts.load(); }

}  // namespace mc::game::save_guard
