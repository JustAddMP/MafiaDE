#include "game/save_paths.h"

#include <cctype>
#include <cstring>

namespace mc::game::save_paths {
namespace {

struct Known {
    const char* file;
    Kind kind;
    const char* why;
};

// The profile files seen in profiles\temporaryprofile (2026-10-19) and named in the exe (block ids GameSettings,
// GameInputModule, Profile_Language, Profile_VideoSettings, Profile_AudioSettings, My2K_Game_Stats, map_filter,
// garage, Profile_Data_Storage_PersistantData).
constexpr Known kKnown[] = {
    {"aslot.pf", Kind::SlotProfile, "the story slot's script data (chapter, weapons)"},
    {"bslot.pf", Kind::SlotProfile, "the Free Ride slot's script data"},
    {"cslot.pf", Kind::SlotProfile, "the third slot's script data"},
    {"garage.pf", Kind::Progress, "the cars in the player's garage"},
    {"my2k_game_stats.pf", Kind::Progress, "the player's play stats"},
    {"profile_data_storage_persistantdata.pf", Kind::Progress, "script data kept across saves (UNVERIFIED what)"},
    {"gamesettings.pf", Kind::Setting, "the player's game settings"},
    {"gameinputmodule.pf", Kind::Setting, "the player's key bindings"},
    {"profile_language.pf", Kind::Setting, "the player's language"},
    {"profile_audiosettings.pf", Kind::Setting, "the player's audio settings"},
    {"profile_videosettings.pf", Kind::Setting, "the player's video settings"},
    {"map_filter.pf", Kind::Setting, "the player's map filter buttons"},
};

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool EndsWith(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

}  // namespace

Mapped Classify(const std::string& path) {
    Mapped m;
    std::string p = path;
    for (char& c : p)
        if (c == '/') c = '\\';
    const std::string lower = Lower(p);
    const size_t slash = lower.find_last_of('\\');
    m.file = slash == std::string::npos ? lower : lower.substr(slash + 1);
    if (lower.find("\\mafiacoop\\coop-saves\\") != std::string::npos) return m;  // our own scratch / held trees
    if (const size_t at = lower.find("\\gamesaves\\"); at != std::string::npos) {
        if (m.file.empty()) return m;  // the folder itself
        m.kind = Kind::Save;
        m.sub = p.substr(at);
        return m;
    }
    if (const size_t at = lower.find("\\profiles\\"); at != std::string::npos) {
        if (!EndsWith(m.file, ".pf")) return m;
        m.kind = Kind::UnknownProfile;
        for (const Known& k : kKnown)
            if (m.file == k.file) m.kind = k.kind;
        m.sub = p.substr(at);
        return m;
    }
    if (lower.find("\\saves\\") != std::string::npos && (m.file == "videoconfig.cfg" || m.file == "launcconfig.cfg"))
        m.kind = Kind::Setting;
    return m;
}

bool Redirected(Kind k) {
    return k == Kind::Save || k == Kind::SlotProfile || k == Kind::Progress || k == Kind::UnknownProfile;
}

bool HeldAfterSession(Kind k) { return k == Kind::Progress || k == Kind::UnknownProfile; }

std::string Join(const std::string& root, const std::string& sub) {
    if (sub.empty() || root.empty()) return {};
    std::string out = root + (sub[0] == '\\' ? "" : "\\") + sub;
    for (size_t i; (i = out.find("\\\\", 1)) != std::string::npos;) out.erase(i, 1);
    return out;
}

std::string Target(const std::string& path, const std::string& root) {
    const Mapped m = Classify(path);
    return Redirected(m.kind) ? Join(root, m.sub) : std::string();
}

const char* KindName(Kind k) {
    switch (k) {
        case Kind::Save: return "save";
        case Kind::SlotProfile: return "slot profile";
        case Kind::Progress: return "progress";
        case Kind::Setting: return "setting";
        case Kind::UnknownProfile: return "unknown profile file";
        default: return "other";
    }
}

const char* Why(const Mapped& m) {
    for (const Known& k : kKnown)
        if (m.file == k.file) return k.why;
    switch (m.kind) {
        case Kind::Save: return "a save slot's checkpoint";
        case Kind::Setting: return "the player's window / launcher settings";
        case Kind::UnknownProfile: return "a profile block we do not know (kept out to be safe)";
        default: return "not a save or profile file";
    }
}

bool KeptOut(Kind k, Scope s) {
    switch (s) {
        case Scope::All: return Redirected(k);
        case Scope::Progress: return k == Kind::Progress || k == Kind::UnknownProfile;
        default: return false;
    }
}

const char* ScopeName(Scope s) {
    switch (s) {
        case Scope::All: return "saves and profile";
        case Scope::Progress: return "profile only (his story saves stay his own)";
        default: return "nothing";
    }
}

void Guard::Load(const std::string& storage) { _story = Lower(storage) == "aslot"; }

void Guard::Want(bool block, bool host) {
    _wantBlock = block;
    _wantHost = host;
    if (block) _block = true;
    if (host) _host = true;
}

void Guard::SetBlock(bool on) {
    _block = _wantBlock = on;
}

void Guard::MainMenu() {
    if (!_wantBlock) _block = false;
    if (!_wantHost) _host = false;
}

Scope Guard::Current() const {
    if (_block) return Scope::All;
    if (_host) return _story ? Scope::Progress : Scope::All;
    return Scope::None;
}

Where Guard::Route(const Mapped& m, bool write) {
    if (KeptOut(m.kind, Current())) {
        if (write && HeldAfterSession(m.kind)) _held.insert(m.file);
        return Where::Scratch;
    }
    if (HeldAfterSession(m.kind) && IsHeld(m.file)) return Where::Held;
    return Where::Real;
}

std::string Guard::HeldList() const {
    std::string out;
    for (const std::string& f : _held) out += (out.empty() ? "" : ", ") + f;
    return out;
}

}  // namespace mc::game::save_paths
