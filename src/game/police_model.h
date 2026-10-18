#pragma once
// Police in co-op, the part without the game (docs/protocol.md "Police", docs/game-api.md 22): the wire format of the
// police messages, when a player's own wanted state is sent, what the host's game does about a wanted guest (his
// puppet becomes a target of the host's police while he is near the host) and what a guest's game does when the
// host's police chase him (his own wanted level is held at least at the chase's level, a zone of his own is started
// when the host's police see him and his game has none). Pure data in, decisions out: no Lua, no session, no OS calls,
// so tests/police_test.cpp runs it without the game. game/police_sync.cpp feeds it from the Lua glue
// (police_sync.lua) and the session. Single-threaded (the caller locks).
//
// Who owns what: every player's wanted state is his own game's (his crimes, his police zone, his fines and arrests;
// the game keeps one zone for its player). It is broadcast so every game knows it; nothing of it is copied into
// another player's game. Only the host's police react to a guest's puppet (the guest is in the host's world there,
// and the host's police are the ones shown to a guest near the host, docs/game-api.md 19).
#include "net/wire.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mc::game::police {

// ---- protocol (0x0260-0x0263) --------------------------------------------------------------------------------------
constexpr uint16_t kMsgState = 0x0260;  // every player -> everyone (Broadcast): his own wanted state
constexpr uint16_t kMsgEvent = 0x0261;  // every player -> everyone (Broadcast): one police event of his game
constexpr uint16_t kMsgChase = 0x0262;  // host -> one guest (SendTo): the host's police chase his puppet / stopped
constexpr uint16_t kMsgReserved = 0x0263;  // reserved for police (not sent)

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
float Dist(const Vec3& a, const Vec3& b);  // 3D, metres

// WantedState flags
enum Flags : uint8_t {
    kWanted = 0x01,      // his game has an active police zone (GetActiveZones returned one)
    kHunt = 0x02,        // ... of type HUNT (red: chased, shot at); else SEARCH (blue: the police look for him)
    kSeen = 0x04,        // the police see him now (IsSeenByPolice)
    kDecaying = 0x08,    // the zone decays (BLUEZONE_DECAY seen and nothing new since): he is losing them
    kFining = 0x10,      // a fine is going on (POLICE_FINE_STARTED .. FINISHED)
    kArresting = 0x20,   // an arrest is going on (POLICE_ARREST_STARTED .. ESCAPED / ARRESTED)
    kOffencesOff = 0x40, // offences are disabled on his game (AreOffencesDisabled: a mission switched them off)
    kInWorld = 0x80,     // he is in a world (the rest means something)
};
constexpr uint8_t kAllFlags = 0xFF;
constexpr uint8_t kMaxLevel = 16;  // escalation levels seen: small integers (the game's police tables); clamp

struct WantedState {
    uint8_t flags = 0;
    uint8_t level = 0;  // the zone's escalation level (GetZoneInfo), 0 = none
    Vec3 zone;          // the zone's centre (the lose-the-cops search area), 0 when none
    bool Wanted() const { return (flags & kWanted) != 0; }
    bool Hunt() const { return (flags & (kWanted | kHunt)) == (kWanted | kHunt); }
};
constexpr float kZoneMove = 10.0f;  // m: a zone centre that moved less than this is the same state
bool SameState(const WantedState& a, const WantedState& b);
std::string Describe(const WantedState& s);  // "wanted hunt level 2 seen at 1.0,2.0,3.0" / "clean"

// PoliceState: u8 version (1), u16 seq, u8 flags, u8 level, f32 zone x y z. Readers take version >= 1 and ignore
// bytes after these (later versions append).
constexpr uint8_t kStateVersion = 1;
constexpr size_t kStateBytes = 1 + 2 + 1 + 1 + 12;
void WriteState(net::ByteWriter& w, const WantedState& s, uint16_t seq);
bool ReadState(net::ByteReader& r, WantedState& s, uint16_t& seq);

// The Lua glue's "PS flags level x y z" (after "PS "): false when malformed or out of range.
bool ParseStateLine(const char* rest, WantedState& s);

// Police events (PoliceEvent kind). The game's own events (enums.PoliceEventType, RegisterForPoliceEvent) by name,
// plus the HumanMessages our player gets (OFFENDER ..., ARRESTED) and two of ours.
enum class Event : uint8_t {
    None = 0,
    EnterSearch = 1,     // PLAYER_ENTER_BLUEZONE
    ExitSearch = 2,      // PLAYER_EXIT_BLUEZONE
    EnterHunt = 3,       // PLAYER_ENTER_REDZONE
    ExitHunt = 4,        // PLAYER_EXIT_REDZONE
    ZoneDecay = 5,       // BLUEZONE_DECAY
    ZoneDecayed = 6,     // POLICE_ZONE_DECAYED
    PlayerFine = 7,      // PLAYER_FINE
    FineStarted = 8,     // POLICE_FINE_STARTED
    FineFinished = 9,    // POLICE_FINE_FINISHED
    ArrestStarted = 10,  // POLICE_ARREST_STARTED
    ArrestEscaped = 11,  // POLICE_ARREST_ESCAPED
    Arrested = 12,       // POLICE_ARREST_ARRESTED
    PlayerArrest = 13,   // PLAYER_AREST (sic)
    PlayerDeath = 14,    // PLAYER_DEATH
    ActivityEnded = 15,  // POLICE_ACTIVITY_ENDED
    Witness = 16,        // POLICE_WITNESS_STATE_STARTED
    RedStarted = 17,     // POLICE_REDZONE_STARTED
    BlueStarted = 18,    // POLICE_BLUEZONE_STARTED
    Offender = 19,       // HumanMessages.OFFENDER on our player
    OffenderUpdate = 20, // HumanMessages.OFFENDER_UPDATE
    Recognized = 21,     // HumanMessages.RECOGNIZED_BY_POLICE
    PoliceSearch = 22,   // HumanMessages.POLICE_SEARCH
    HumanArrested = 23,  // HumanMessages.ARRESTED (arg: GetArrestFinished)
    LostPolice = 24,     // ours: our state went from wanted to clean (lost them, or the zone was deleted)
    BecameWanted = 25,   // ours: our state went from clean to wanted
    Last = 26,
};
const char* EventName(Event e);              // the names above ("PLAYER_ENTER_BLUEZONE", "OFFENDER", "LOST", ...)
Event EventFromName(const std::string& name);  // Event::None when unknown
// The events that mean "he committed something": the host announces an offence from his puppet.
bool IsOffenceEvent(Event e);

// PoliceEvent: u8 kind, u32 arg.
struct EventMsg {
    Event kind = Event::None;
    uint32_t arg = 0;
};
void WriteEvent(net::ByteWriter& w, const EventMsg& e);
bool ReadEvent(net::ByteReader& r, EventMsg& e);

// PoliceChase (host -> the guest): u8 flags (1 active, 2 hunt), u8 level, u8 cops (host police near his puppet), u8
// seers (of them, those that saw it in the last seconds), f32 nearest (m, the nearest of them; 0 when none).
struct Chase {
    bool active = false;
    bool hunt = false;
    uint8_t level = 0;
    uint8_t cops = 0;
    uint8_t seers = 0;
    float nearest = 0;
};
void WriteChase(net::ByteWriter& w, const Chase& c);
bool ReadChase(net::ByteReader& r, Chase& c);

// ---- our own state: when to send it ----------------------------------------------------------------------------------
constexpr double kMinGap = 0.2;          // s between two sends of a changed state (reliable)
constexpr double kHeartbeatWanted = 1.0; // s: an unchanged state is sent again this often while wanted (unreliable)
constexpr double kHeartbeatClean = 3.0;  // ... and while clean

class Sender {
public:
    // True when `s` should be sent now; `reliable` tells how (a change: reliable; a heartbeat: unreliable). The
    // caller sends it with Seq() and calls nothing else.
    bool Due(double now, const WantedState& s, bool& reliable);
    uint16_t Seq() const { return seq_; }
    void Reset();  // session start / end: the next state is sent at once

private:
    WantedState last_;
    bool any_ = false;
    double lastAt_ = -1e9;
    uint16_t seq_ = 0;
};

// The events of a state change (BecameWanted / LostPolice), so every game logs the moment.
Event Transition(const WantedState& before, const WantedState& after);

// ---- host: wanted guests and the host's police -----------------------------------------------------------------------
constexpr double kStateStale = 6.0;    // s without a state of a guest: he is not wanted as far as we know
constexpr double kPuppetStale = 3.0;   // s without his puppet's position
constexpr float kNearEnter = 250.0f;   // m between the host and a guest's puppet: our police take him on ...
constexpr float kNearLeave = 400.0f;   // ... and leave him beyond this (the guest's game keeps its own police there)
constexpr double kReleaseHold = 3.0;   // s a target stays after the guest's game says clean (no flapping)
constexpr double kReportStale = 3.0;   // s: a chase report (cops near his puppet) older than this counts as none
constexpr double kChaseInterval = 0.5; // s between PoliceChase messages to a chased guest
constexpr double kAnnounceGap = 5.0;   // s between two offences announced from the same puppet

struct HostCmd {
    enum class Type : uint8_t {
        Target,    // police: take on guest `id`'s puppet (every tick for every target; absent = not a target)
        Announce,  // announce an offence from guest `id`'s puppet (one-shot; Lua picks the offence from level/hunt)
    };
    Type type = Type::Target;
    uint8_t id = 0;
    uint8_t level = 0;
    bool hunt = false;
};

struct ChaseOut {
    uint8_t id = 0;
    Chase chase;
    bool reliable = false;  // a change of `active` (start / end); the 2 Hz updates are unreliable
};

class HostPolice {
public:
    struct Guest {
        WantedState st;
        double stateAt = -1e9;
        Vec3 puppet;
        double puppetAt = -1e9;
        bool isNear = false;     // within kNearEnter (hysteresis to kNearLeave)
        bool target = false;     // our police take him on
        double cleanSince = -1;  // target: when his game said clean (release after kReleaseHold)
        double targetSince = 0;
        uint8_t cops = 0, seers = 0;
        float nearest = 0;
        double reportAt = -1e9;
        bool chaseSent = false;  // the last PoliceChase sent said active
        double chaseAt = -1e9;
        double announceAt = -1e9;
        uint8_t announcedLevel = 0;
        bool announcedHunt = false;
        bool offence = false;    // his game reported an offence (IsOffenceEvent) since the last announce
        uint32_t targets = 0, announces = 0;  // counters (status, trace)
    };

    void SetHost(const Vec3& pos) { host_ = pos; }
    void OnState(uint8_t id, const WantedState& s, double now);
    void OnEvent(uint8_t id, const EventMsg& e, double now);
    void SetPuppet(uint8_t id, const Vec3& pos, double now);
    void SetReport(uint8_t id, uint8_t cops, uint8_t seers, float nearest, double now);
    void RemovePeer(uint8_t id);
    void Clear() { guests_.clear(); }
    // cmds: this tick's Target list (one per target) and one-shot Announce; chases: the PoliceChase messages due.
    void Tick(double now, std::vector<HostCmd>& cmds, std::vector<ChaseOut>& chases);
    const std::map<uint8_t, Guest>& Guests() const { return guests_; }
    const Guest* Find(uint8_t id) const;

private:
    Vec3 host_;
    std::map<uint8_t, Guest> guests_;
};

// ---- guest: the host's police chase us ---------------------------------------------------------------------------------
constexpr double kChaseStale = 3.0;    // s without a PoliceChase: the chase is over (the host sends 2 Hz)
constexpr double kForceHold = 4.0;     // s our wanted level stays held after the chase ended
constexpr double kZoneRetry = 5.0;     // s between two zones we start ourselves

struct GuestCmd {
    enum class Type : uint8_t {
        ForceMin,  // game.police:ForceMinEscalationLevel(level) (0 = released)
        Zone,      // game.police:ZoneCreate(our position, hunt ? HUNT : SEARCH, level): our game has no zone yet
    };
    Type type = Type::ForceMin;
    uint8_t level = 0;
    bool hunt = false;
};

class GuestPolice {
public:
    void OnChase(const Chase& c, double now);
    void SetLocal(const WantedState& own) { own_ = own; }
    void Tick(double now, std::vector<GuestCmd>& out);
    // Session end / host gone: what to undo now (ForceMin 0 when we hold a level). Forgets the chase.
    void Reset(std::vector<GuestCmd>& out);
    uint8_t Forced() const { return forced_; }
    bool Chased(double now) const;
    const Chase& Last() const { return chase_; }
    uint32_t Zones() const { return zones_; }

private:
    Chase chase_;
    double chaseAt_ = -1e9;
    double activeAt_ = -1e9;  // the last moment the chase was active
    WantedState own_;
    uint8_t forced_ = 0;
    double zoneAt_ = -1e9;
    uint32_t zones_ = 0;
};

}  // namespace mc::game::police
