#include "game/menu.h"

#include "core/config.h"
#include "core/compat.h"
#include "core/hook.h"
#include "core/log.h"
#include "core/mem.h"
#include "game/swf_patch.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mc::game::menu {
namespace {

// ---- engine functions (patterns verified unique, docs/re-notes.md "Menus") ------------------------------------
// LoadProcess::BeginSWFLoading(this, File* in, ...): reads the movie header from `in` (a Scaleform File). It takes
// at least six arguments (the 5th/6th are read from the stack), so the detour forwards eight.
using BeginLoadFn = uint64_t (*)(void* self, void* file, void* a3, void* a4, void* a5, uint64_t a6, void* a7,
                                 void* a8);
// The game's handler object for ExternalInterface.call("AttachIface", ...): Call(this, args, argc, ret, a5, view)
// (the 6th argument, read from the stack, is the movie view). args/ret are the engine's Flash values (0x18 bytes:
// vtable, data @+8, type @+0x10). The detour forwards eight arguments.
using CallFn = uint64_t (*)(void* self, void* args, uint32_t argc, void* ret, void* a5, void* a6, void* a7, void* a8);
using ValueCtorStringFn = void* (*)(void* value, const char* s);
using ValueAssignFn = void* (*)(void* dst, const void* src);
using ValueDtorFn = void (*)(void* value);
using ValueGetStringFn = const char* (*)(const void* value);

constexpr const char* kPatBeginLoad =
    "48 89 5C 24 20 55 56 57 41 54 41 57 48 83 EC 40 48 8B 02 48 8B F1 48 8B CA 4D 8B F9 49 8B E8 48 8B DA FF 50 20";
constexpr const char* kPatAttachIface =
    "48 89 54 24 10 48 89 4C 24 08 53 48 83 EC 60 48 8B 9C 24 98 00 00 00 4C 89 64 24 48 45 8B E0";
constexpr const char* kPatValueCtorString =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B DA C7 41 10 00 00 00 00 48 8D 05 ?? ?? ?? ?? BA 06 00 00 00";
constexpr const char* kPatValueGetString = "40 57 48 83 EC 20 83 79 10 06 75 4B 48 8B 79 08 48 85 FF 74 42";
constexpr const char* kPatValueAssign =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 33 D2 E8 ?? ?? ?? ?? 48 8B 43 08 48 89 47 08 8B 43 10 89 47 10";
constexpr const char* kPatValueDtor =
    "40 53 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 33 D2 48 89 01 48 8B D9 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 03 48 83 "
    "C4 20";

constexpr int kValueSize = 0x18;
constexpr int kTypeBool = 2, kTypeInt = 3, kTypeUInt = 4, kTypeNumber = 5, kTypeString = 6;

BeginLoadFn _origBeginLoad = nullptr;
CallFn _origAttach = nullptr;
ValueCtorStringFn _valueCtorString = nullptr;
ValueAssignFn _valueAssign = nullptr;
ValueDtorFn _valueDtor = nullptr;
ValueGetStringFn _valueGetString = nullptr;

std::atomic<bool> _active{false};
std::wstring _dumpDir;
bool _dump = false;

// ---- shared state ---------------------------------------------------------------------------------------------------
std::mutex _mutex;
State _state;
std::deque<Command> _commands;
// A menu action changes the mode right away (the menu redraws in the same call), before the co-op layer has
// acted on the command; SetState must not undo that for a moment.
Mode _optimistic = Mode::Idle;
ULONGLONG _optimisticUntil = 0;

void SetOptimistic(Mode m) {  // _mutex held
    _state.mode = m;
    _optimistic = m;
    _optimisticUntil = GetTickCount64() + 3000;
}

// ---- ids shared with the patched ActionScript ----------------------------------------------------------------------
constexpr int kCoopMenuLocation = 50;  // menuLocation of our page (the game uses 0..11)
constexpr int kFreeRideId = 9;         // main_menu_page_main.FREERIDE: our entry goes after it
enum ItemId : int {
    kItemCoop = 100,  // root entry, opens our page
    kItemHost = 101,
    kItemJoinFriend = 102,
    kItemJoinIp = 103,
    kItemLeave = 104,
    kItemBack = 105,
    kItemInvite = 106,
    kItemSetPassword = 107,
    kItemClearPassword = 108,
};

// ---- a Scaleform File backed by memory (same virtual layout as the engine's file class, vtable exe+0x4F6EAA8:
// dtor, GetFilePath, IsValid, IsWritable, Tell, LTell, GetLength, LGetLength, GetErrorCode, Write, Read, SkipBytes,
// BytesAvailable, Flush, Seek, LSeek, Rewind, ChangeSize, CopyFromStream, Close; ref count at +8) -----------------
using Bytes = std::shared_ptr<const std::vector<uint8_t>>;

class MemFile {
public:
    MemFile(std::string path, Bytes data) : _path(std::move(path)), _bytes(std::move(data)), _data(*_bytes) {}
    virtual ~MemFile() = default;
    virtual const char* GetFilePath() { return _path.c_str(); }
    virtual bool IsValid() { return true; }
    virtual bool IsWritable() { return false; }
    virtual int Tell() { return static_cast<int>(_pos); }
    virtual int64_t LTell() { return static_cast<int64_t>(_pos); }
    virtual int GetLength() { return static_cast<int>(_data.size()); }
    virtual int64_t LGetLength() { return static_cast<int64_t>(_data.size()); }
    virtual int GetErrorCode() { return 0; }
    virtual int Write(const uint8_t*, int) { return -1; }
    virtual int Read(uint8_t* buf, int n) {
        if (n <= 0) return -1;
        const size_t left = _data.size() - _pos;
        const size_t c = left < static_cast<size_t>(n) ? left : static_cast<size_t>(n);
        if (c) std::memcpy(buf, _data.data() + _pos, c);
        _pos += c;
        return static_cast<int>(c);
    }
    virtual int SkipBytes(int n) {
        const int64_t before = static_cast<int64_t>(_pos);
        LSeek(n, 1);
        return static_cast<int>(static_cast<int64_t>(_pos) - before);
    }
    virtual int BytesAvailable() { return static_cast<int>(_data.size() - _pos); }
    virtual bool Flush() { return true; }
    virtual int Seek(int off, int origin) { return static_cast<int>(LSeek(off, origin)); }
    virtual int64_t LSeek(int64_t off, int origin) {
        int64_t p = origin == 0   ? off
                    : origin == 1 ? static_cast<int64_t>(_pos) + off
                                  : static_cast<int64_t>(_data.size()) + off;
        if (p < 0) p = 0;
        if (p > static_cast<int64_t>(_data.size())) p = static_cast<int64_t>(_data.size());
        _pos = static_cast<size_t>(p);
        return p;
    }
    virtual int Rewind() { return Seek(0, 0); }
    virtual bool ChangeSize(int) { return false; }
    virtual int CopyFromStream(void*, int) { return -1; }
    virtual bool Close() { return true; }

    // +8: the engine's AddRef/Release work on this. It never reaches zero: the object (a few dozen bytes) is leaked
    // on purpose rather than trusting the engine's release path with our allocation; the movie bytes are shared
    // through the cache below, so a menu loaded many times costs one buffer.
    volatile long refCount = 1 << 24;

private:
    std::string _path;
    Bytes _bytes;
    const std::vector<uint8_t>& _data;
    size_t _pos = 0;
};

// Patched movies by path, reused while the original bytes are the same (FNV-1a of the original).
struct CacheEntry {
    uint64_t hash = 0;
    Bytes data;
};
std::mutex _cacheMutex;
std::unordered_map<std::string, CacheEntry> _cache;

uint64_t Fnv1a(const std::vector<uint8_t>& d) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : d) h = (h ^ b) * 1099511628211ull;
    return h;
}

struct GameFile {  // the engine's file, called through its own vtable
    void** vt;
    const char* Path() { return reinterpret_cast<const char* (*)(void*)>(vt[1])(this); }
    int Read(uint8_t* b, int n) { return reinterpret_cast<int (*)(void*, uint8_t*, int)>(vt[10])(this, b, n); }
};

// ---- ActionScript patches ---------------------------------------------------------------------------------------------
// Emits ExternalInterface.call("AttachIface", "MafiaCoop", request, <extra args pushed by the caller>).
struct EiCall {
    swf::Asm& a;
    uint32_t call;
    EiCall(swf::Asm& as, swf::Abc& abc, const char* request) : a(as) {
        call = abc.Name("call");
        a.GetLex(abc.QName(swf::ns::kPackage, "flash.external", "ExternalInterface"));
        a.PushString("AttachIface").PushString("MafiaCoop").PushString(request);
    }
    void Finish(uint32_t extraArgs) { a.CallProp(call, 3 + extraArgs); }
};

bool Need(uint32_t mn, const char* what, std::string* err) {
    if (mn) return true;
    if (err) *err = std::string("name not found: ") + what;
    return false;
}

// /gui/main_menu_main.gfx, class pages::main_menu_page_main.
bool _verbose = false;  // menu_debug = 1: log every request

bool PatchMainMenu(swf::Abc& abc, std::string* err) {
    const int cls = abc.FindClass("pages", "main_menu_page_main");
    if (cls < 0) return *err = "class pages::main_menu_page_main not found", false;
    const int build = abc.TraitMethod(cls, "BuildMenu");
    const int activate = abc.TraitMethod(cls, "ActivateButton");
    if (build < 0 || activate < 0) return *err = "BuildMenu/ActivateButton not found", false;
    const uint32_t menuLocation = abc.TraitName(cls, "menuLocation"), previous = abc.TraitName(cls, "PreviousMenus"),
                   updateMenu = abc.TraitName(cls, "UpdateMenu"), back = abc.TraitName(cls, "Back"),
                   setPageTitle = abc.TraitName(cls, "setPageTitle"),
                   infoBarBack = abc.TraitName(cls, "setupInfoBarBack");
    if (!Need(menuLocation, "menuLocation", err) || !Need(previous, "PreviousMenus", err) ||
        !Need(updateMenu, "UpdateMenu", err) || !Need(back, "Back", err) || !Need(setPageTitle, "setPageTitle", err) ||
        !Need(infoBarBack, "setupInfoBarBack", err))
        return false;
    const uint32_t buttonData = abc.QName(swf::ns::kPackage, "components", "ButtonData");
    const uint32_t idx = abc.RuntimeIndexName(), length = abc.Name("length"), split = abc.Name("split"),
                   push = abc.Name("push"), splice = abc.Name("splice"), id = abc.Name("id"),
                   menuParent = abc.Name("menu_parent"), playSound = abc.Name("PlaySound"),
                   soundIds = abc.QName(swf::ns::kPackage, "", "SoundIDs"), select = abc.Name("SELECT");

    // BuildMenu(vec): let C++ describe additions for the menu being built ("build", menuLocation), one per line:
    //   T|<title>               setPageTitle(true, title)
    //   P|<id>|<label>          vec.push(new ButtonData(id, label))
    //   A|<afterId>|<id>|<label> insert after the entry with id afterId (append if absent)
    {
        swf::Asm a(abc);
        const uint32_t base = abc.LocalCount(build);
        const uint32_t rLines = base, rI = base + 1, rParts = base + 2, rJ = base + 3, rOp = base + 4;
        const int loop = a.Label(), end = a.Label(), notT = a.Label(), notP = a.Label(), find = a.Label(),
                  found = a.Label(), insert = a.Label();
        auto newButton = [&](int idField, int labelField) {
            a.FindPropStrict(buttonData);
            a.GetLocal(rParts).PushInt(idField).GetProperty(idx).Op(swf::op::kConvertU);
            a.GetLocal(rParts).PushInt(labelField).GetProperty(idx).Op(swf::op::kConvertS);
            a.ConstructProp(buttonData, 2);
        };
        a.Op(swf::op::kGetLocal0).Op(swf::op::kPushScope);
        EiCall ei(a, abc, "build");
        a.Op(swf::op::kGetLocal0).GetProperty(menuLocation);
        ei.Finish(1);
        a.Op(swf::op::kConvertS);
        a.PushString("\n").CallProp(split, 1).SetLocal(rLines);
        a.PushInt(0).SetLocal(rI);
        a.LoopHead(loop);
        a.GetLocal(rI).GetLocal(rLines).GetProperty(length).Jump(swf::op::kIfGe, end);
        a.GetLocal(rLines).GetLocal(rI).GetProperty(idx).Op(swf::op::kConvertS).PushString("|").CallProp(split, 1);
        a.SetLocal(rParts);
        a.GetLocal(rI).Op(swf::op::kIncrementI).SetLocal(rI);
        a.GetLocal(rParts).PushInt(0).GetProperty(idx).Op(swf::op::kConvertS).SetLocal(rOp);
        // T
        a.GetLocal(rOp).PushString("T").Jump(swf::op::kIfNe, notT);
        a.Op(swf::op::kGetLocal0).Op(swf::op::kPushTrue);
        a.GetLocal(rParts).PushInt(1).GetProperty(idx).Op(swf::op::kConvertS);
        a.CallPropVoid(setPageTitle, 2).Jump(swf::op::kJump, loop);
        // P
        a.Bind(notT).GetLocal(rOp).PushString("P").Jump(swf::op::kIfNe, notP);
        a.Op(swf::op::kGetLocal1);
        newButton(1, 2);
        a.CallPropVoid(push, 1).Jump(swf::op::kJump, loop);
        // A
        a.Bind(notP).GetLocal(rOp).PushString("A").Jump(swf::op::kIfNe, loop);
        a.PushInt(0).SetLocal(rJ);
        a.LoopHead(find);
        a.GetLocal(rJ).Op(swf::op::kGetLocal1).GetProperty(length).Jump(swf::op::kIfGe, insert);
        a.Op(swf::op::kGetLocal1).GetLocal(rJ).GetProperty(idx).GetProperty(id);
        a.GetLocal(rParts).PushInt(1).GetProperty(idx).Op(swf::op::kConvertU).Jump(swf::op::kIfEq, found);
        a.GetLocal(rJ).Op(swf::op::kIncrementI).SetLocal(rJ).Jump(swf::op::kJump, find);
        a.Bind(found).GetLocal(rJ).Op(swf::op::kIncrementI).SetLocal(rJ);
        a.Bind(insert).Op(swf::op::kGetLocal1).GetLocal(rJ).PushInt(0);
        newButton(2, 3);
        a.CallPropVoid(splice, 3).Jump(swf::op::kJump, loop);
        a.Bind(end).Op(swf::op::kPopScope);
        const auto code = a.Finish();
        if (code.empty() || !abc.Prepend(build, code, 10, 5)) return *err = "BuildMenu patch failed", false;
    }

    // ActivateButton(data): ids >= 100 are ours. C++ acts on "select" and answers what the menu should do:
    //   "open" -> enter our page, "back" -> Back(), "refresh" -> rebuild the current page, else stay.
    {
        swf::Asm a(abc);
        const uint32_t rRes = abc.LocalCount(activate);
        const int skip = a.Label(), n1 = a.Label(), n2 = a.Label(), n3 = a.Label();
        a.Op(swf::op::kGetLocal0).Op(swf::op::kPushScope);
        a.Op(swf::op::kGetLocal1).GetProperty(id).PushInt(kItemCoop).Jump(swf::op::kIfLt, skip);
        a.Op(swf::op::kGetLocal0).GetProperty(menuParent).GetLex(soundIds).GetProperty(select).CallPropVoid(playSound, 1);
        EiCall ei(a, abc, "select");
        a.Op(swf::op::kGetLocal1).GetProperty(id);
        ei.Finish(1);
        a.Op(swf::op::kConvertS).SetLocal(rRes);
        a.GetLocal(rRes).PushString("open").Jump(swf::op::kIfNe, n1);
        a.Op(swf::op::kGetLocal0).CallPropVoid(infoBarBack, 0);
        a.Op(swf::op::kGetLocal0).GetProperty(previous).Op(swf::op::kGetLocal0).GetProperty(menuLocation).CallPropVoid(push, 1);
        a.Op(swf::op::kGetLocal0).PushInt(kCoopMenuLocation).SetProperty(menuLocation);
        a.Op(swf::op::kGetLocal0).CallPropVoid(updateMenu, 0).Op(swf::op::kReturnVoid);
        a.Bind(n1).GetLocal(rRes).PushString("back").Jump(swf::op::kIfNe, n2);
        a.Op(swf::op::kGetLocal0).CallProp(back, 0).Op(swf::op::kPop).Op(swf::op::kReturnVoid);
        a.Bind(n2).GetLocal(rRes).PushString("refresh").Jump(swf::op::kIfNe, n3);
        a.Op(swf::op::kGetLocal0).CallPropVoid(updateMenu, 0).Op(swf::op::kReturnVoid);
        a.Bind(n3).Op(swf::op::kReturnVoid);
        a.Bind(skip).Op(swf::op::kPopScope);
        const auto code = a.Finish();
        if (code.empty() || !abc.Prepend(activate, code, 8, 1)) return *err = "ActivateButton patch failed", false;
    }
    return true;
}

// /gui/pause_menu_game.gfx, class pages::pause_menu_page_game. On PC the page has four entries; the fifth movie
// clip (entry_4_mc, Xbox "switch account") exists but is hidden. We show it as CO-OP.
bool PatchPauseMenu(swf::Abc& abc, std::string* err) {
    const int cls = abc.FindClass("pages", "pause_menu_page_game");
    if (cls < 0) return *err = "class pages::pause_menu_page_game not found", false;
    const int populate = abc.TraitMethod(cls, "PopulateNavigation");
    const int selectM = abc.TraitMethod(cls, "Select");
    if (populate < 0 || selectM < 0) return *err = "PopulateNavigation/Select not found", false;
    const uint32_t navigation = abc.TraitName(cls, "Navigation"), entries = abc.TraitName(cls, "Entries"),
                   entriesMc = abc.TraitName(cls, "entries_mc"), totalEntries = abc.TraitName(cls, "totalEntries"),
                   currentCid = abc.TraitName(cls, "current_CID"), checkPlatform = abc.TraitName(cls, "CheckPlatformID"),
                   addMouse = abc.TraitName(cls, "AddMouseSupport");
    if (!Need(navigation, "Navigation", err) || !Need(entries, "Entries", err) || !Need(entriesMc, "entries_mc", err) ||
        !Need(totalEntries, "totalEntries", err) || !Need(currentCid, "current_CID", err) ||
        !Need(checkPlatform, "CheckPlatformID", err) || !Need(addMouse, "AddMouseSupport", err))
        return false;
    const uint32_t idx = abc.RuntimeIndexName(), length = abc.Name("length"), push = abc.Name("push"),
                   visible = abc.Name("visible"), name = abc.Name("name"), entry4 = abc.Name("entry_4_mc"),
                   menuParent = abc.Name("menu_parent"), playSound = abc.Name("PlaySound"),
                   soundIds = abc.QName(swf::ns::kPackage, "", "SoundIDs"), select = abc.Name("SELECT");
    {
        swf::Asm a(abc);
        const int skip = a.Label(), named = a.Label();
        a.Op(swf::op::kGetLocal0).Op(swf::op::kPushScope);
        a.Op(swf::op::kGetLocal0).CallProp(checkPlatform, 0).Jump(swf::op::kIfTrue, skip);
        a.Op(swf::op::kGetLocal0).GetProperty(navigation).GetProperty(length).PushInt(4).Jump(swf::op::kIfNe, named);
        a.Op(swf::op::kGetLocal0).GetProperty(entriesMc).GetProperty(entry4).Op(swf::op::kPushTrue).SetProperty(visible);
        a.Op(swf::op::kGetLocal0).Op(swf::op::kGetLocal0).GetProperty(entriesMc).GetProperty(entry4).PushInt(4);
        a.CallPropVoid(addMouse, 2);
        a.Op(swf::op::kGetLocal0).GetProperty(navigation).Op(swf::op::kGetLocal0).GetProperty(entriesMc).GetProperty(entry4);
        a.CallPropVoid(push, 1);
        a.Op(swf::op::kGetLocal0).PushInt(5).SetProperty(totalEntries);
        a.Bind(named);
        a.Op(swf::op::kGetLocal0).GetProperty(navigation).GetProperty(length).PushInt(5).Jump(swf::op::kIfNe, skip);
        a.Op(swf::op::kGetLocal0).GetProperty(entries).PushInt(4).GetProperty(idx);
        EiCall ei(a, abc, "pause_label");
        ei.Finish(0);
        a.Op(swf::op::kConvertS).SetProperty(name);
        a.Bind(skip).Op(swf::op::kPopScope);
        const auto code = a.Finish();
        if (code.empty() || !abc.Prepend(populate, code, 8, 0)) return *err = "PopulateNavigation patch failed", false;
    }
    {
        swf::Asm a(abc);
        const int skip = a.Label();
        a.Op(swf::op::kGetLocal0).Op(swf::op::kPushScope);
        a.Op(swf::op::kGetLocal0).GetProperty(currentCid).PushInt(4).Jump(swf::op::kIfNe, skip);
        a.Op(swf::op::kGetLocal0).CallProp(checkPlatform, 0).Jump(swf::op::kIfTrue, skip);
        a.Op(swf::op::kGetLocal0).GetProperty(menuParent).GetLex(soundIds).GetProperty(select).CallPropVoid(playSound, 1);
        EiCall ei(a, abc, "pause");
        ei.Finish(0);
        a.Op(swf::op::kPop).Op(swf::op::kReturnVoid);
        a.Bind(skip).Op(swf::op::kPopScope);
        const auto code = a.Finish();
        if (code.empty() || !abc.Prepend(selectM, code, 8, 0)) return *err = "Select patch failed", false;
    }
    return true;
}

struct MoviePatch {
    const char* path;
    bool (*fn)(swf::Abc&, std::string*);
};
// pause_menu.gfx carries its own copy of pages::pause_menu_page_game, and as the parent movie its definition is the
// one the game uses (AS3: the first definition loaded into the application domain wins), so both are patched.
const MoviePatch kPatches[] = {
    {"/gui/main_menu_main.gfx", &PatchMainMenu},
    {"/gui/pause_menu.gfx", &PatchPauseMenu},
    {"/gui/pause_menu_game.gfx", &PatchPauseMenu},
};

void Dump(const std::string& path, const std::vector<uint8_t>& data, const char* suffix) {
    if (!_dump) return;
    std::string name = path;
    for (char& ch : name)
        if (ch == '/' || ch == '\\' || ch == ':') ch = '_';
    const std::wstring out = _dumpDir + L"\\" + std::wstring(name.begin(), name.end()) +
                             std::wstring(suffix, suffix + std::strlen(suffix));
    std::ofstream f(out, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

uint64_t BeginLoadDetour(void* self, void* file, void* a3, void* a4, void* a5, uint64_t a6, void* a7, void* a8) {
    auto* gf = static_cast<GameFile*>(file);
    const char* path = gf ? gf->Path() : nullptr;
    const MoviePatch* patch = nullptr;
    for (const auto& p : kPatches)
        if (path && _stricmp(path, p.path) == 0) patch = &p;
    if (!patch) return _origBeginLoad(self, file, a3, a4, a5, a6, a7, a8);

    std::vector<uint8_t> data;
    uint8_t buf[65536];
    for (;;) {
        const int n = gf->Read(buf, sizeof(buf));
        if (n <= 0) break;
        data.insert(data.end(), buf, buf + n);
    }
    const uint64_t hash = Fnv1a(data);
    {
        std::lock_guard lock(_cacheMutex);
        auto it = _cache.find(path);
        if (it != _cache.end() && it->second.hash == hash)
            return _origBeginLoad(self, new MemFile(path, it->second.data), a3, a4, a5, a6, a7, a8);
    }
    Dump(path, data, "");
    std::vector<uint8_t> patched = data;
    std::string err;
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    if (swf::PatchMovie(patched, [&](swf::Abc& abc, std::string* e) { return patch->fn(abc, e); }, &err)) {
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        MC_LOG_INFO("menu: patched %s (%zu -> %zu bytes, %.1f ms)", path, data.size(), patched.size(),
                    1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart));
        Dump(path, patched, ".patched");
        data.swap(patched);
    } else {
        MC_LOG_ERROR("menu: could not patch %s: %s (loading it unchanged)", path, err.c_str());
    }
    auto bytes = std::make_shared<const std::vector<uint8_t>>(std::move(data));
    {
        std::lock_guard lock(_cacheMutex);
        _cache[path] = {hash, bytes};
    }
    return _origBeginLoad(self, new MemFile(path, bytes), a3, a4, a5, a6, a7, a8);
}

// ---- Flash -> C++ ----------------------------------------------------------------------------------------------------
const uint8_t* Arg(void* args, uint32_t i) { return static_cast<const uint8_t*>(args) + i * kValueSize; }
int ArgType(void* args, uint32_t i) { return *reinterpret_cast<const int*>(Arg(args, i) + 0x10); }
const char* ArgString(void* args, uint32_t i) {
    return ArgType(args, i) == kTypeString ? _valueGetString(Arg(args, i)) : nullptr;
}
double ArgNumber(void* args, uint32_t i) {
    switch (ArgType(args, i)) {
    case kTypeInt: return *reinterpret_cast<const int32_t*>(Arg(args, i) + 8);
    case kTypeUInt: return *reinterpret_cast<const uint32_t*>(Arg(args, i) + 8);
    case kTypeNumber: return *reinterpret_cast<const double*>(Arg(args, i) + 8);
    case kTypeBool: return *(Arg(args, i) + 8) ? 1 : 0;
    default: return -1;
    }
}

// The engine's string value keeps the char pointer (an unmanaged Scaleform string) and the caller converts it to an
// ActionScript string only after our handler returned, so the text must outlive the call: a small ring of buffers
// (Flash callbacks all run on GameMainThread, one at a time).
void ReturnString(void* ret, const std::string& s) {
    static std::string ring[8];
    static unsigned next = 0;
    std::string& keep = ring[next++ % 8];
    keep = s;
    alignas(16) uint8_t tmp[kValueSize] = {};
    _valueCtorString(tmp, keep.c_str());
    _valueAssign(ret, tmp);
    _valueDtor(tmp);
}

void Post(Command c) {
    std::lock_guard lock(_mutex);
    _commands.push_back(c);
    MC_LOG_INFO("menu: %s", CommandName(c));
}

std::string BuildSpec(int location) {
    State st;
    {
        std::lock_guard lock(_mutex);
        st = _state;
    }
    char buf[512];
    if (location == 0) {  // ROOT_MENU
        const char* label = st.mode == Mode::Hosting   ? "CO-OP (HOSTING)"
                            : st.mode == Mode::Joining ? "CO-OP (JOINING)"
                            : st.mode == Mode::Connected ? "CO-OP (CONNECTED)"
                                                         : "CO-OP";
        std::snprintf(buf, sizeof(buf), "A|%d|%d|%s", kFreeRideId, kItemCoop, label);
        return buf;
    }
    if (location != kCoopMenuLocation) return {};
    std::string s = "T|CO-OP";
    auto item = [&](int id, const char* label) { s += "\nP|" + std::to_string(id) + "|" + label; };
    auto passwordItems = [&] {
        item(kItemSetPassword, st.password ? "CHANGE PASSWORD" : "SET PASSWORD");
        if (st.password) item(kItemClearPassword, "CLEAR PASSWORD");
    };
    switch (st.mode) {
    case Mode::Idle:
        item(kItemHost, "HOST CO-OP GAME");
        item(kItemJoinFriend, "JOIN FRIEND");
        item(kItemJoinIp, "JOIN BY IP");
        passwordItems();
        break;
    case Mode::Hosting:
        item(kItemInvite, "INVITE FRIENDS");
        passwordItems();
        item(kItemLeave, "STOP HOSTING");
        break;
    case Mode::Joining:
    case Mode::Connected:
        item(kItemLeave, "LEAVE CO-OP");
        break;
    }
    item(kItemBack, "BACK");
    return s;
}

std::string Select(int id) {
    switch (id) {
    case kItemCoop: return "open";
    case kItemHost: {
        {
            std::lock_guard lock(_mutex);
            SetOptimistic(Mode::Hosting);  // the root entry shows it right away
        }
        Post(Command::Host);
        return "back";  // the player now picks CONTINUE / STORY as usual, with hosting on
    }
    case kItemJoinFriend: Post(Command::JoinFriend); return "";
    case kItemJoinIp: Post(Command::JoinByIp); return "";
    case kItemInvite: Post(Command::Invite); return "";
    case kItemSetPassword: Post(Command::SetPassword); return "";
    case kItemClearPassword: {
        {
            std::lock_guard lock(_mutex);
            _state.password = false;  // the rebuilt page drops the entry right away
        }
        Post(Command::ClearPassword);
        return "refresh";
    }
    case kItemLeave: {
        {
            std::lock_guard lock(_mutex);
            SetOptimistic(Mode::Idle);
        }
        Post(Command::Leave);
        return "refresh";
    }
    case kItemBack: return "back";
    default: return "";
    }
}

uint64_t AttachDetour(void* self, void* args, uint32_t argc, void* ret, void* a5, void* a6, void* a7, void* a8) {
    const char* tag = argc >= 2 ? ArgString(args, 0) : nullptr;
    if (!tag || std::strcmp(tag, "MafiaCoop") != 0) return _origAttach(self, args, argc, ret, a5, a6, a7, a8);
    const char* req = ArgString(args, 1);
    const std::string request = req ? req : "";
    std::string reply;
    if (request == "build") {
        const int location = argc >= 3 ? static_cast<int>(ArgNumber(args, 2)) : -1;
        reply = BuildSpec(location);
    } else if (request == "select") {
        const int id = argc >= 3 ? static_cast<int>(ArgNumber(args, 2)) : -1;
        reply = Select(id);
        MC_LOG_INFO("menu: main menu item %d -> '%s'", id, reply.c_str());
    } else if (request == "pause_label") {
        reply = "CO-OP";
    } else if (request == "pause") {
        Post(Command::PausePanel);
    } else {
        MC_LOG_WARN("menu: unknown request '%s'", request.c_str());
    }
    if (_verbose) MC_LOG_INFO("menu: request '%s' -> '%s'", request.c_str(), reply.c_str());
    if (ret) ReturnString(ret, reply);
    return 1;
}

template <class T>
bool Resolve(T*& out, const char* name, const char* pattern) {
    const uintptr_t a = core::mem::FindPattern(pattern);
    if (!a) {
        MC_LOG_ERROR("menu: pattern '%s' not found; native menu entries disabled", name);
        return false;
    }
    MC_LOG_INFO("menu: %s at exe+0x%llx", name, static_cast<unsigned long long>(a - core::mem::MainModule().base));
    out = reinterpret_cast<T*>(a);
    return true;
}

}  // namespace

const char* CommandName(Command c) {
    switch (c) {
    case Command::Host: return "host";
    case Command::Leave: return "leave";
    case Command::JoinFriend: return "join friend";
    case Command::JoinByIp: return "join by ip";
    case Command::Invite: return "invite friends";
    case Command::PausePanel: return "pause co-op panel";
    case Command::SetPassword: return "set password";
    case Command::ClearPassword: return "clear password";
    }
    return "?";
}

void Install(const std::wstring& dllDir) {
    const auto& cfg = core::Config::Get();
    if (!cfg.GetBool("native_menu", true)) {
        MC_LOG_INFO("menu: native menu entries off (native_menu = 0)");
        return;
    }
    _dump = cfg.GetBool("menu_dump", false);
    _verbose = cfg.GetBool("menu_debug", false);
    _dumpDir = dllDir + L"\\dev\\gfx";
    if (_dump) {
        CreateDirectoryW((dllDir + L"\\dev").c_str(), nullptr);
        CreateDirectoryW(_dumpDir.c_str(), nullptr);
    }
    BeginLoadFn beginLoad = nullptr;
    CallFn attach = nullptr;
    if (!Resolve(beginLoad, "GFx_LoadProcess_BeginSWF", kPatBeginLoad) ||
        !Resolve(attach, "Flash_AttachIface_Call", kPatAttachIface) ||
        !Resolve(_valueCtorString, "FlashValue_CtorString", kPatValueCtorString) ||
        !Resolve(_valueGetString, "FlashValue_GetString", kPatValueGetString) ||
        !Resolve(_valueAssign, "FlashValue_Assign", kPatValueAssign) ||
        !Resolve(_valueDtor, "FlashValue_Dtor", kPatValueDtor)) {
        core::compat::Failed(core::compat::Feature::NativeMenu);
        return;
    }
    if (!core::hook::Install("Flash::AttachIface", reinterpret_cast<void*>(attach),
                             reinterpret_cast<void*>(&AttachDetour), reinterpret_cast<void**>(&_origAttach)) ||
        !core::hook::Install("GFx::LoadProcess::BeginSWF", reinterpret_cast<void*>(beginLoad),
                             reinterpret_cast<void*>(&BeginLoadDetour), reinterpret_cast<void**>(&_origBeginLoad))) {
        MC_LOG_ERROR("menu: hooks failed; native menu entries disabled");
        core::compat::Failed(core::compat::Feature::NativeMenu);
        return;
    }
    _active = true;
    MC_LOG_INFO("menu: native menu entries enabled (main menu CO-OP, pause menu CO-OP)");
}

bool Active() { return _active.load(); }

void SetState(const State& s) {
    std::lock_guard lock(_mutex);
    const Mode keep = _state.mode;
    _state = s;
    if (_optimisticUntil) {
        if (s.mode == _optimistic || GetTickCount64() > _optimisticUntil)
            _optimisticUntil = 0;
        else
            _state.mode = keep;
    }
}

bool PopCommand(Command* out) {
    std::lock_guard lock(_mutex);
    if (_commands.empty()) return false;
    *out = _commands.front();
    _commands.pop_front();
    return true;
}

}  // namespace mc::game::menu
