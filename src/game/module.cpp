// The Mafia DE game layer, as JustAddMP core sees it (external/core/include/mc/game_module.h): the Lua bridge and
// player sync (game/lua.h, game/player_sync.h) and the CO-OP entries in the game's menus (game/menu.h).
#include "mc/game_module.h"

#include "core/config.h"
#include "core/log.h"
#include "core/trace.h"
#include "dll/ui_input.h"
#include "game/characters.h"
#include "game/lua.h"
#include "game/menu.h"
#include "game/mission_sync.h"
#include "game/car_damage_sync.h"
#include "game/npc_sync.h"
#include "game/player_sync.h"
#include "game/save_guard.h"
#include "game/sectors_guard.h"
#include "game/traffic_sync.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace mc::game {
namespace {

// A key/button of ours held while one of our windows is in the foreground and no core panel takes input.
bool OurWindowHasInput() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || (GetWindowThreadProcessId(fg, &pid), pid != GetCurrentProcessId())) return false;
    return !ui_input::Blocking();
}

// The enter-vehicle control (keyboard E, pad Y).
bool EnterVehicleHeld() {
    if (!OurWindowHasInput()) return false;
    constexpr uint16_t kPadY = 0x8000;  // XINPUT_GAMEPAD_Y
    return (GetAsyncKeyState('E') & 0x8000) != 0 || (ui_input::PadButtons() & kPadY) != 0;
}

// The aim control (right mouse button, pad left trigger). The game itself does not tell scripts whether the player
// aims (docs/game-api.md 10), so this is read from the input.
bool AimHeld() {
    if (!OurWindowHasInput()) return false;
    return (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 || ui_input::PadLeftTrigger() > 60;
}

// The CO-OP panel's CHARACTER and OUTFIT rows (docs/PLAN.md "F"). CHARACTER steps through DEFAULT (the character
// for your player id: host Tommy, then Paulie, Sam, Salieri) and the list in game/characters.cpp; OUTFIT through that
// character's outfits. Every step applies at once: you change in the world while browsing, and the others see it.
void CharacterOptions(std::vector<PanelOption>& out) {
    const std::string model = PlayerSync::Model();
    size_t c = 0, o = 0;
    if (model.empty()) {
        out.push_back({"CHARACTER", "DEFAULT"});
        return;
    }
    if (!characters::Find(model, &c, &o)) {
        out.push_back({"CHARACTER", "CUSTOM"});
        return;
    }
    const auto& ch = characters::List()[c];
    out.push_back({"CHARACTER", ch.name});
    if (ch.outfits.size() > 1) out.push_back({"OUTFIT", ch.outfits[o].name});
}

void CharacterStep(size_t index, int delta) {
    const auto& list = characters::List();
    const std::string model = PlayerSync::Model();
    size_t c = 0, o = 0;
    const bool known = !model.empty() && characters::Find(model, &c, &o);
    if (index == 0) {  // CHARACTER: DEFAULT, then every character (its first outfit)
        const int n = static_cast<int>(list.size()) + 1;
        int pos = known ? static_cast<int>(c) + 1 : 0;
        if (!known && !model.empty()) pos = delta > 0 ? 0 : 1;  // CUSTOM: next is the first character
        pos = ((pos + delta) % n + n) % n;
        PlayerSync::SetModel(pos == 0 ? std::string() : list[static_cast<size_t>(pos - 1)].outfits[0].alias);
    } else if (index == 1 && known) {  // OUTFIT
        const auto& outfits = list[c].outfits;
        const int n = static_cast<int>(outfits.size());
        const int pos = ((static_cast<int>(o) + delta) % n + n) % n;
        PlayerSync::SetModel(outfits[static_cast<size_t>(pos)].alias);
    }
}

// ---- CO-OP SETTINGS rows (saved in MafiaCoop.ini; read where they are used, so a change applies at once) ----
struct Toggle {
    const char* label;
    const char* key;
    bool def;
};
constexpr Toggle kToggles[] = {
    {"SHOW NAME TAGS", "name_tags", true},     // names over the other players (DrawHud)
    {"MAP MARKERS", "map_markers", true},      // the other players on the minimap (player_sync.lua)
    {"FRIENDLY FIRE", "friendly_fire", true},  // players can hurt each other (docs/protocol.md "Phase 6")
};

void SettingsRows(std::vector<PanelOption>& out) {
    for (const Toggle& t : kToggles) out.push_back({t.label, core::Config::Get().GetBool(t.key, t.def) ? "ON" : "OFF"});
}

void SettingsStep(size_t index, int) {
    if (index >= std::size(kToggles)) return;
    const Toggle& t = kToggles[index];
    const bool on = !core::Config::Get().GetBool(t.key, t.def);
    core::Config::Get().Set(t.key, on ? "1" : "0");
    MC_LOG_INFO("settings: %s = %d", t.key, on ? 1 : 0);
}

// ---- lobby modes (the index travels on the wire: keep the order) ----
// After STORY: one mode per chapter (MissionSync::Chapters, kModeFirstChapter + i).
enum LobbyModeId { kModeFreeRide = 0, kModeStory = 1, kModeFirstChapter = 2 };

// "mm_110_omerta" -> "CHAPTER 10: OMERTA" (number = position in the story; name from the quest id)
std::string ChapterLabel(size_t index, const std::string& quest) {
    size_t at = quest.find('_', 3);
    std::string name = at == std::string::npos ? quest : quest.substr(at + 1);
    for (char& c : name) c = c == '_' ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return "CHAPTER " + std::to_string(index + 1) + ": " + name;
}
constexpr const char* kFreeRideSave = "freeroam_test";  // GetLoadedSaveId() in Free Ride (docs/game-api.md 11)

// ---- name tags (docs/game-api.md 13): the other players' names over their puppets ----
constexpr float kTagFull = 35.0f;  // m: fully visible up to here
constexpr float kTagGone = 90.0f;  // m: faded out here

uint32_t WithAlpha(uint32_t argb, float a) {
    const auto al = static_cast<uint32_t>(static_cast<float>(argb >> 24) * std::clamp(a, 0.0f, 1.0f));
    return (argb & 0x00FFFFFF) | (al << 24);
}

void DrawTags(render::DrawList& d, float w, float h) {
    if (!core::Config::Get().GetBool("name_tags", true)) return;
    std::vector<PlayerSync::NameTag> tags;
    PlayerSync::GetTags(tags);
    const float s = h / 1080.0f;
    for (const auto& t : tags) {
        if (t.dist > kTagGone || t.x < -1.1f || t.x > 1.1f || t.y < -1.1f || t.y > 1.1f) continue;
        const float a = t.dist <= kTagFull ? 1.0f : 1.0f - (t.dist - kTagFull) / (kTagGone - kTagFull);
        const float px = std::clamp(30.0f * s * 10.0f / std::max(t.dist, 1.0f), 17.0f * s, 30.0f * s);
        std::string name = t.name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        const float tw = render::DrawList::TextWidth(name, px);
        const float cx = (t.x + 1.0f) * 0.5f * w, cy = (1.0f - t.y) * 0.5f * h;
        const float x = cx - tw * 0.5f, y = cy - px;
        d.Rect(x - px * 0.35f, y - px * 0.08f, tw + px * 0.7f, px * 1.16f, WithAlpha(0x8C000000, a));
        d.Rect(x - px * 0.35f, y + px * 1.04f, tw + px * 0.7f, std::max(1.0f, px * 0.06f), WithAlpha(0xFFD1AC62, a));
        d.Text(x, y, name, px, WithAlpha(0xFFE9E5DB, a));
    }
}

net::Session* _session = nullptr;  // render thread: the current session (OnSessionStart .. OnSessionEnd)

// The reconnect banner (docs/protocol.md "Reconnect"): guest: we lost the host and core rejoins him; everyone else: a
// player lost his connection and his slot is held. "" = none.
std::string ReconnectNotice() {
    if (!_session) return {};
    char buf[160];
    if (_session->IsReconnecting()) {
        std::snprintf(buf, sizeof buf, "CONNECTION LOST - RECONNECTING... %d S",
                      static_cast<int>(std::ceil(_session->ReconnectLeftS())));
        return buf;
    }
    const auto dropped = _session->DroppedPlayers();
    if (dropped.empty()) return {};
    std::string name = dropped[0].info.name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::snprintf(buf, sizeof buf, "%s LOST THE CONNECTION - WAITING FOR %s TO RECONNECT... %d S", name.c_str(),
                  dropped.size() > 1 ? "THEM" : "THE PLAYER", static_cast<int>(std::ceil(dropped[0].leftS)));
    return buf;
}

// A line over the game while the story sync needs a moment (guest: held during the host's cutscene, loading his
// checkpoint), or while a connection is being re-established.
void DrawNotice(render::DrawList& d, float w, float h) {
    std::string text = ReconnectNotice();
    if (text.empty()) text = MissionSync::HudNotice();
    if (text.empty()) return;
    const float s = h / 1080.0f, px = 26.0f * s;
    const float tw = render::DrawList::TextWidth(text, px);
    const float x = (w - tw) * 0.5f, y = h * 0.82f;
    d.Rect(x - px * 0.6f, y - px * 0.3f, tw + px * 1.2f, px * 1.6f, 0xB4000000);
    d.Rect(x - px * 0.6f, y + px * 1.3f, tw + px * 1.2f, std::max(1.0f, px * 0.08f), 0xFFD1AC62);
    d.Text(x, y, text, px, 0xFFE9E5DB);
}

class MafiaModule final : public GameModule {
public:
    void Init(const std::wstring& dllDir, const std::string& logTag) override {
        // Lua bridge: hooks the script tick (GameMainThread); player sync runs on it. Failure only disables it.
        if (lua::Install(dllDir, logTag)) {
            PlayerSync::Install();
            MissionSync::Install();
            NpcSync::Install();
            TrafficSync::Install();
            CarDamageSync::Install();  // after traffic: its Lua reads player_sync's and traffic_sync's cars
        }
        menu::Install(dllDir);
        save_guard::Install();  // after the menu hooks: those must be in place before the first menu movie loads
        sectors_guard::Install();  // makes the friend-brain puppets safe (player_sync.lua M.Ally)
    }
    void RenderTick(float dt) override { lua::PollRender(dt); }

    void OnSessionStart(net::Session& session) override {
        _session = &session;
        PlayerSync::Attach(session);
        MissionSync::Attach(session);
        NpcSync::Attach(session);
        TrafficSync::Attach(session);
        CarDamageSync::Attach(session);
    }
    void OnSessionEnd() override {
        PlayerSync::Detach();
        MissionSync::Detach();
        NpcSync::Detach();
        TrafficSync::Detach();
        CarDamageSync::Detach();
        _session = nullptr;
    }
    void OnPlayerLeft(net::PlayerId id) override {
        PlayerSync::OnPlayerLeft(id);
        NpcSync::OnPlayerLeft(id);
        TrafficSync::OnPlayerLeft(id);
        CarDamageSync::OnPlayerLeft(id);
    }

    // ---- reconnect (core: docs/protocol.md "Reconnect"; ours: docs/protocol.md "Reconnect") ----
    // A player lost his connection: his puppet goes (as when he leaves) until he is back; the
    // host's syncs see him as new then (joinSeq) and send him the world again.
    void OnPlayerDropped(net::PlayerId id) override {
        MC_LOG_INFO("reconnect: player %u lost the connection: his puppet is removed while his slot is held", id);
        core::trace::Line("RCN", "ev=dropped id=%u", id);
        PlayerSync::OnPlayerLeft(id);
        NpcSync::OnPlayerLeft(id);      // his own NPCs' mirrors go; he sends them again when back
        TrafficSync::OnPlayerLeft(id);  // his traffic copies are spawned again for him when back
        CarDamageSync::OnPlayerLeft(id);  // his copies ask for the cars' damage again when back
    }
    // Guest: we lost the host. The game keeps running (no reload, no menu); the puppets go (PlayerSync, when the
    // session turns inactive), the story keeps the host's point (MissionSync), our NPC copies are dropped (NpcSync).
    void OnConnectionLost() override {
        MC_LOG_WARN("reconnect: connection to the host lost (%s): the game keeps running, rejoining for %.0f s",
                    _session ? _session->ReconnectReason().c_str() : "?",
                    _session ? _session->ReconnectLeftS() : 0.0);
        core::trace::Line("RCN", "ev=lost why=%s", _session ? _session->ReconnectReason().c_str() : "?");
        MissionSync::OnConnectionLost();
    }
    void OnReconnected(bool sameSlot) override {
        MC_LOG_INFO("reconnect: back in the host's session as player %u (%s) after %d attempt(s); the host re-sends "
                    "the world, our puppets come back",
                    _session ? _session->LocalId() : 0, sameSlot ? "same slot" : "new slot",
                    _session ? _session->ReconnectAttempts() : 0);
        core::trace::Line("RCN", "ev=back same=%d local=%u attempts=%d", sameSlot ? 1 : 0,
                          _session ? _session->LocalId() : 0, _session ? _session->ReconnectAttempts() : 0);
        MissionSync::OnReconnected();
    }
    void SessionTick(float dt) override {
        PlayerSync::Tick(dt, EnterVehicleHeld(), AimHeld());
        MissionSync::Tick(dt);
        NpcSync::Tick(dt);
        TrafficSync::Tick(dt);
        CarDamageSync::Tick(dt);
    }

    int PlayerHealth(net::PlayerId id) override { return PlayerSync::Health(id); }
    const char* MessageName(uint16_t id) override {  // names in the traffic statistics (core NetMonitor)
        switch (id) {
        case 0x0100: return "PlayerState";
        case 0x0101: return "CarParked";
        case 0x0102: return "PlayerModel";
        case 0x0200: return "WorldEnv";
        case 0x0201: return "MissionState";
        case 0x0202: return "HostSave";
        case 0x0203: return "Cutscene";
        case 0x0205: return "MissionFail";
        case 0x0206: return "FailVerdict";
        case 0x0210: return "NpcStates";
        case 0x0211: return "NpcDeath";
        case 0x0220: return "TrafficSpawn";
        case 0x0221: return "TrafficDespawn";
        case 0x0222: return "TrafficStates";
        case 0x0223: return "TrafficBump";
        case 0x0250: return "CarDamage";
        case 0x0251: return "CarDamageReport";
        case 0x0252: return "CarDamageRequest";
        case 0x0400: return "Hit";
        case 0x0410: return "NpcHit";
        case 0x0420: return "GuestNpcStates";
        case 0x0421: return "GuestNpcEnd";
        case 0x0422: return "HostNpcHit";
        case 0x0423: return "NpcClaim";
        default: return nullptr;
        }
    }
    void AppendStatus(std::vector<render::OverlayLine>& out) override {
        PlayerSync::GetStatus(out);
        MissionSync::GetStatus(out);
        NpcSync::GetStatus(out);
        TrafficSync::GetStatus(out);
        CarDamageSync::GetStatus(out);
    }
    void SetMenuState(const MenuState& state) override { menu::SetState(state); }
    bool PopMenuCommand(MenuCommand* out) override { return menu::PopCommand(out); }
    void PanelOptions(std::vector<PanelOption>& out) override { CharacterOptions(out); }
    void PanelOptionStep(size_t index, int delta) override { CharacterStep(index, delta); }

    void SettingsOptions(std::vector<PanelOption>& out) override { SettingsRows(out); }
    void SettingsOptionStep(size_t index, int delta) override { SettingsStep(index, delta); }

    void LobbyModes(std::vector<LobbyMode>& out) override {
        out.push_back({"FREE RIDE", true, ""});
        out.push_back({"STORY", true, ""});  // the host continues his story, guests load his checkpoint (MissionSync)
        const auto& chapters = MissionSync::Chapters();
        for (size_t i = 0; i < chapters.size(); ++i) out.push_back({ChapterLabel(i, chapters[i]), true, ""});
    }
    bool StartMode(int mode) override {
        if (PlayerSync::InWorld()) {
            MC_LOG_INFO("lobby: start of mode %d, but we are in the world already", mode);
            return false;
        }
        if (mode == kModeStory) return MissionSync::StartStory();
        if (mode >= kModeFirstChapter) {
            const auto& chapters = MissionSync::Chapters();
            const size_t i = static_cast<size_t>(mode - kModeFirstChapter);
            return i < chapters.size() && MissionSync::StartChapter(chapters[i]);
        }
        if (mode != kModeFreeRide) return false;
        MissionSync::StopFollowing();
        return menu::RequestLoad("free_ride");  // the main menu's own FREE RIDE (menu_parent.LoadGame)
    }
    bool InWorld() override { return PlayerSync::InWorld(); }
    bool LobbyCanOpen() override { return menu::MainMenuShown(); }  // not over the title or a loading screen
    int CurrentMode() override {
        if (!PlayerSync::InWorld()) return -1;
        return PlayerSync::SaveId() == kFreeRideSave ? kModeFreeRide : kModeStory;
    }
    std::string PlayerCharacter(net::PlayerId id) override { return PlayerSync::CharacterName(id); }

    void DrawHud(render::DrawList& d, float w, float h) override {
        DrawTags(d, w, h);
        DrawNotice(d, w, h);
    }
};

}  // namespace
}  // namespace mc::game

namespace mc {

GameModule& GetGameModule() {
    static game::MafiaModule module;
    return module;
}

}  // namespace mc
