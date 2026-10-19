#include "game/save_guard.h"

#include "core/config.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/mem.h"
#include "game/save_paths.h"

#include <windows.h>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

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
std::atomic<uint32_t> _abortBurst{0};    // failures: load requests of the abort id more than 5 s apart
std::atomic<uint32_t> _droppedFor{0};    // the failure whose cached entry data was dropped
std::atomic<ULONGLONG> _abortAt{0};      // when the game last asked for the free roam state before the quest
std::atomic<uint32_t> _substituted{0};   // reads of a real save served from the session save instead
uintptr_t _exe = 0;
std::mutex _mu;
std::string _scratch;  // ...\MafiaCoop\coop-saves\<pid> (no trailing slash)

std::string ToLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

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
        if (GetTickCount64() - _abortAt.load() > 5000) ++_abortBurst;
        _abortAt = GetTickCount64();
        MC_LOG_INFO("savesys: the game leaves the mission (back to the free roam state before the quest)%s",
                    _block.load() ? ": it gets the session's checkpoint instead" : "");
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
    // any slot: the menu's own FREE RIDE load asks for slot 2, our swapped (replay-style) loads for slot 1; after one
    // of them the other found no entry and crashed the same way (0.9.1)
    if (loading && name == _dbgId && self == _dbgSelf && _dbgEntry) {
        MC_LOG_INFO("savesys: chapter load of '%s' (slot %u): the storage only knows its first save (slot %u), using its "
                    "entry %p",
                    name.c_str(), slot, _dbgSlot, _dbgEntry);
        return _dbgEntry;
    }
    MC_LOG_WARN("savesys: no save '%s' in slot %u (storage %p)", name.c_str(), slot, self);
    return r;
}

// A failed mission (or a death the game handles itself) in a chapter loaded through the chapter loader: the game asks
// for "freeroam_before_quest_activated" in the story storage (aslot, slot 2), a save of the player's own story,
// outside the co-op session (docs/re-notes.md 7). The load handler uses the entry the storage's lookup returns; when
// that entry already holds data (exe+0x2E81EFB: entry->vf58() non-null) no file is read, and an entry keeps the data
// of its first read in the process. So serving the file from the session's checkpoint (CreateFileADetour) is not
// enough once the game has read that save before (a mission failed in single player before hosting). While the save
// slots are blocked (a co-op chapter, a guest) the lookup of that id in a slot storage (aslot / bslot share this
// lookup, exe+0x2E74C80) returns the chapter loader's entry instead: the save of this session we loaded last.
constexpr const char* kAbortId = "freeroam_before_quest_activated";
constexpr const char* kPatSlotFind =
    "48 89 5C 24 18 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 20 33 ED 41 83 CF FF 89 6C 24 60 4D 8B E0 "
    "48 8B 59 20 4C 8B E9 48 3B 59 28";
FindFn _origSlotFind = nullptr;
std::atomic<uint32_t> _abortSwaps{0};
std::atomic<bool> _directFail{false};     // fail_load = direct | freeride (default: a Free Ride stop first)
std::atomic<void*> _failEntry{nullptr};  // the slot entry a direct failed-mission load used

void* SlotFindDetour(void* self, uint32_t slot, void* id) {
    const std::string name = Str(id);  // read before the call: the callee releases the string
    void* r = _origSlotFind(self, slot, id);
    if (!_block.load() || name != kAbortId) return r;
    if (r && _directFail.load()) {
        // One load straight back to the checkpoint: the game's own entry, with the data it cached from an earlier read
        // dropped (entry->vf58() = [entry+0x18]), so the handler reads the save file again, and that read is served
        // from the session's checkpoint (CreateFileADetour). Only at the first lookup of a failure (the game asks
        // several times per load and loads twice per failure, 0.3 s apart; dropping the data it had just read crashed
        // the next lookup's user, exe+0x1A90AB6). The dropped buffer is left alone (a small leak, never freed under
        // the game); after the load the entry is emptied again (ForgetFailEntry) so that a later failed mission
        // outside co-op reads the player's own save again.
        const uint32_t n = ++_abortSwaps;
        const uint32_t burst = _abortBurst.load();
        if (_droppedFor.exchange(burst) != burst) {
            void** data = reinterpret_cast<void**>(static_cast<uint8_t*>(r) + 0x18);
            MC_LOG_INFO("savesys: lookup #%u of '%s' (slot %u): entry %p, cached data %p dropped: the session's "
                        "checkpoint is read instead",
                        n, name.c_str(), slot, r, *data);
            *data = nullptr;
            _failEntry = r;
        }
        return r;
    }
    std::lock_guard lock(_dbgMu);
    if (!_dbgEntry) {
        MC_LOG_WARN("savesys: lookup of '%s' in a co-op session, but no chapter load seen: the game's own save is used",
                    name.c_str());
        return r;
    }
    const uint32_t n = ++_abortSwaps;
    if (n <= 20 || n % 100 == 0)
        MC_LOG_INFO("savesys: lookup #%u of '%s' (slot %u): the chapter loader's entry %p (this session's save) "
                    "instead of %p",
                    n, name.c_str(), slot, _dbgEntry, r);
    return _dbgEntry;
}

// ---- the save and profile files (Documents\My Games\Mafia Definitive Edition\Data\<id>\...) ---------------------------
// The game reads and writes them with CreateFileA from its file layer (paths with mixed / and \). Which file goes
// where is save_paths.h's table (tested by tests/save_paths_test.cpp). While Block() is on, every access to a
// redirected file (the saves below "gamesaves", the slot profiles aslot/bslot/cslot.pf, the progress profiles
// garage.pf / my2k_game_stats.pf / profile_data_storage_persistantdata.pf, unknown *.pf) goes to a scratch copy of
// the tree instead: writes always, reads when the scratch file exists (so a Retry reads the checkpoint written in this
// session), deletes and copies likewise. The real files are never opened for writing. The settings
// (gamesettings.pf, key bindings, language, audio, video, map filter, Saves\videoconfig.cfg) are the player's own and
// left alone. Up to 0.15.0 only the slots and their profiles were redirected: a co-op chapter rewrote garage.pf on
// host and guest (the in-game test of 0.15.0).
// After the session (Block(false)) the game still holds the progress blocks it had in memory (the profile is read at
// start; UNVERIFIED whether a later save load re-reads them), so a progress file the session wrote stays held for the
// rest of the process: its writes go to <scratch>-held\profiles\... (cleared when a later start finds the process
// gone), and the player's real garage changes again only after a restart of the game.
std::set<std::string> _held;           // progress files (lower-case names) a co-op session of this process wrote
std::atomic<bool> _anyHeld{false};
std::atomic<uint32_t> _profileRedirects{0};

struct Routed {
    std::string target;  // "" = the real file
    save_paths::Mapped m;
    bool held = false;   // after the session (the held tree)
};

Routed Route(const char* path, bool write) {
    Routed r;
    if (!path) return r;
    const bool blocking = _block.load();
    if (!blocking && !_anyHeld.load()) return r;
    r.m = save_paths::Classify(path);
    if (!save_paths::Redirected(r.m.kind)) return r;
    std::string root;
    {
        std::lock_guard lock(_mu);
        if (blocking) {
            root = _scratch;
        } else if (save_paths::HeldAfterSession(r.m.kind) && _held.count(r.m.file)) {
            root = _scratch.empty() ? std::string() : _scratch + "-held";
            r.held = true;
        }
    }
    if (root.empty()) return r;
    const std::string target = save_paths::Join(root, r.m.sub);  // ...\coop-saves\<pid>\gamesaves\... or \profiles\...
    if (target.empty()) return r;
    if (write) {
        CreateDirectoryA(root.c_str(), nullptr);
        for (size_t i = root.size() + 1; (i = target.find('\\', i)) != std::string::npos; ++i)
            CreateDirectoryA(target.substr(0, i).c_str(), nullptr);
        if (blocking && save_paths::HeldAfterSession(r.m.kind)) {
            std::lock_guard lock(_mu);
            _held.insert(r.m.file);
            _anyHeld = true;
        }
    } else if (GetFileAttributesA(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return r;  // nothing written in this session: the real file is read
    }
    r.target = target;
    return r;
}

// One log line per profile file redirect (they are few: loads, quest starts); saves as before (first 50, every 50th).
void LogRoute(const char* what, const char* name, const Routed& r) {
    if (r.m.kind == save_paths::Kind::Save) return;
    const uint32_t n = ++_profileRedirects;
    if (n > 200 && n % 50 != 0) return;
    MC_LOG_INFO("savesys: profile %s #%u %s (%s: %s): %s -> %s", what, n,
                r.held ? "held back after the co-op session (the game may still hold its co-op changes; the real file "
                         "changes again after a restart)"
                       : "kept out of the player's profile",
                save_paths::KindName(r.m.kind), save_paths::Why(r.m), name, r.target.c_str());
}

// The session's checkpoint (SetSessionSave): <scratch>\session-checkpoint.dat (not *.sav: the host's checkpoint scan
// reads the scratch tree). Served for a read of a real save file (gamesaves\...\aslot or bslot) in the 20 s after the
// game asked for "freeroam_before_quest_activated" (docs/re-notes.md 7).
std::string SessionSaveFor(const char* path) {
    std::string lower(path);
    for (char& c : lower) c = static_cast<char>(c == '/' ? '\\' : std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("\\gamesaves\\") == std::string::npos || lower.find(".sav") == std::string::npos) return {};
    std::string scratch;
    {
        std::lock_guard lock(_mu);
        scratch = _scratch;
    }
    if (scratch.empty() || lower.find(ToLower(scratch)) == 0) return {};  // our own scratch files stay as they are
    const std::string sub = scratch + "\\session-checkpoint.dat";
    return GetFileAttributesA(sub.c_str()) == INVALID_FILE_ATTRIBUTES ? std::string() : sub;
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
    if (!write && name && _block.load() && GetTickCount64() - _abortAt.load() < 20000) {
        const std::string sub = SessionSaveFor(name);
        if (!sub.empty()) {
            const uint32_t n = ++_substituted;
            MC_LOG_INFO("savesys: read #%u of %s served from the session's checkpoint %s", n, name, sub.c_str());
            return _origCreateFileA(sub.c_str(), access, share, sa, disp, flags, tmpl);
        }
    }
    const Routed r = Route(name, write);
    if (r.target.empty()) {
        // the profile and save files written for real (diagnostics: what the game writes outside our redirect)
        if (write && name && (std::strstr(name, "gamesaves") || std::strstr(name, "profiles")) &&
            std::strstr(name, "Mafia Definitive Edition")) {
            const save_paths::Mapped m = r.m.file.empty() ? save_paths::Classify(name) : r.m;
            MC_LOG_INFO("savesys: written: %s (%s: %s%s)", m.file.c_str(), save_paths::KindName(m.kind),
                        save_paths::Why(m),
                        _block.load() && !save_paths::Redirected(m.kind) ? ", left alone in co-op" : "");
        }
        if (!write && name && _block.load() && std::strstr(name, "gamesaves"))
            MC_LOG_INFO("savesys: read of a real save in a co-op session: %s", name);
        return _origCreateFileA(name, access, share, sa, disp, flags, tmpl);
    }
    if (write) {
        const uint32_t n = ++_redirected;
        if (r.m.kind == save_paths::Kind::Save && (n <= 50 || n % 50 == 0))
            MC_LOG_INFO("savesys: save write #%u kept out of the real slot: %s -> %s", n, name, r.target.c_str());
    }
    LogRoute(write ? "write" : "read", name, r);
    return _origCreateFileA(r.target.c_str(), access, share, sa, disp, flags, tmpl);
}

BOOL WINAPI DeleteFileADetour(LPCSTR name) {
    const Routed r = Route(name, true);
    if (!r.target.empty()) {
        LogRoute("delete", name, r);
        _origDeleteFileA(r.target.c_str());
        return TRUE;  // the real file stays
    }
    return _origDeleteFileA(name);
}

BOOL WINAPI CopyFileExADetour(LPCSTR from, LPCSTR to, LPPROGRESS_ROUTINE cb, LPVOID data, LPBOOL cancel, DWORD flags) {
    const Routed dst = Route(to, true);
    if (dst.target.empty()) return _origCopyFileExA(from, to, cb, data, cancel, flags);
    const Routed src = Route(from, false);
    LogRoute("copy", to, dst);
    return _origCopyFileExA(src.target.empty() ? from : src.target.c_str(), dst.target.c_str(), cb, data, cancel,
                            flags);
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
    if (const uintptr_t a = core::mem::FindPattern(kPatSlotFind))
        core::hook::Install("SaveSys_SlotFind", reinterpret_cast<void*>(a), &SlotFindDetour, &_origSlotFind);
    else
        MC_LOG_WARN("savesys: slot lookup not found: a failed mission in co-op may load a save outside the session");
    _directFail = core::Config::Get().GetString("fail_load", "freeride") == "direct";
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
    MC_LOG_INFO("savesys: %s", on ? ("saves now go to " + dir + " (co-op session: the real save slots, slot "
                                     "profiles and progress profiles (garage, stats, persistent data) are not "
                                     "touched; settings are)").c_str()
                                  : "saves go to the real save slots again");
    if (!on) {
        std::string held;
        {
            std::lock_guard lock(_mu);
            for (const std::string& f : _held) held += (held.empty() ? "" : ", ") + f;
        }
        if (!held.empty())
            MC_LOG_INFO("savesys: held until the game restarts (the co-op session wrote them and the game may still "
                        "hold its changes): %s -> %s-held",
                        held.c_str(), dir.c_str());
    }
}

void SetSessionSave(const std::vector<uint8_t>& bytes, const std::string& id) {
    std::string dir;
    {
        std::lock_guard lock(_mu);
        dir = _scratch;
    }
    if (!_block.load() || dir.empty() || bytes.empty()) return;
    CreateDirectoryA(dir.c_str(), nullptr);
    const std::string path = dir + "\\session-checkpoint.dat";
    HANDLE h = _origCreateFileA ? _origCreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr)
                                : CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        MC_LOG_WARN("savesys: could not write the session checkpoint %s", path.c_str());
        return;
    }
    DWORD done = 0;
    WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &done, nullptr);
    CloseHandle(h);
    MC_LOG_INFO("savesys: session checkpoint: %s (%zu bytes)", id.c_str(), bytes.size());
}

bool Blocking() { return _block.load(); }
bool DirectFailLoad() { return _directFail.load(); }
void ForgetFailEntry() {
    if (void* e = _failEntry.exchange(nullptr)) {
        *reinterpret_cast<void**>(static_cast<uint8_t*>(e) + 0x18) = nullptr;
        MC_LOG_INFO("savesys: the failed-mission entry %p no longer holds the session's checkpoint", e);
    }
}
std::string ScratchDir() {
    if (!_block.load()) return {};
    std::lock_guard lock(_mu);
    return _scratch;
}
uint32_t Redirected() { return _redirected.load(); }
uint32_t MissionAborts() { return _aborts.load(); }

}  // namespace mc::game::save_guard
