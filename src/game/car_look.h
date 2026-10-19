#pragma once
// A car's look on every game (docs/protocol.md "Car look", docs/game-api.md 24 "Car look"). No game code here: the
// reads and writes are in car_look_sync.cpp (native: the installed tuning items) and car_look.lua (the script
// bindings); this is the part that can be unit-tested without the game (tests/car_look_test.cpp): what a look holds,
// the CarLook message (0x0253), the line form the Lua glue uses, and what a copy has to change to show a look.
//
// Why: a copy of another player's car was spawned with ObtainSpecificCar(model) only, so the game picked its own
// variant, colours and plate for it (0.15.0 test: a black closed smith_v12 sedan, plate ZK-299, was a light-blue open
// smith_v12 with plate NY 858 on the other game; its deform parts differed too, so car damage could only sync tyres
// and the explosion). What makes the variant (found statically, docs/re-notes.md 13, NOT checked in game): the
// vehicles table has one row per model (smith_v12 has one), and the open / closed body is not a table row either -
// the look flags HasVehicleLookFlag reads (CONVERTIBLE_CLOSED / _OPENED, HARDTOP, ...) are the row's. A car's own
// body comes from its installed tuning items (entity + 0x1B20: one slot per tuning part, each holding the table item
// installed there; the traffic generator picks them per car, car_traffic_tuning), which InstallTuningItems sets.
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "game/car_damage.h"  // CarRef: the car key (player cars) / the traffic net id

namespace mc::game::car_look {

constexpr uint16_t kMsgLook = 0x0253;  // CarLook: a look, or a request for one (op byte)
constexpr size_t kMaxModel = 64, kMaxPlate = 16, kMaxPainting = 64, kMaxTuning = 32;
constexpr uint8_t kUnknown8 = 255;

struct Look {
    std::string model;             // GetVehicleModelName (the vehicles table name)
    int16_t color[2] = {-1, -1};   // GetColor(1), GetColor(2) (-1 unknown)
    std::string plate;             // GetSPZText ("" unknown); may hold spaces ("NY 858")
    int16_t tint = -1;             // GetWindowTint (-1 unknown)
    int16_t interior = -1;         // GetInteriorColorsSet (-1 unknown)
    uint8_t dirt = kUnknown8;      // GetDirty x 250 (0..250; 255 unknown)
    uint8_t rust = kUnknown8;      // GetRust x 250 (0..250; 255 unknown)
    std::string painting;          // GetPainting ("" none / unknown)
    bool tuningKnown = false;      // the tuning slots could be read (mc_car_tuning)
    std::vector<uint16_t> tuning;  // the installed tuning item ids, slot order

    bool operator==(const Look& o) const;
    bool operator!=(const Look& o) const { return !(*this == o); }
    std::string Describe() const;  // "smith_v12 colours 3/7 plate ZK-299 tuning 12,40,77" for the logs
};

struct Message {
    enum Op : uint8_t { kLook = 0, kRequest = 1 };
    uint8_t op = kLook;
    car_damage::CarRef ref;  // kPlayer (car key) or kTraffic (net id); kHostTraffic is never on the wire
    uint16_t seq = 0;        // kLook: the sender's look number for this car (moves on with every change)
    Look look;               // kLook
};

std::vector<uint8_t> Write(const Message& m);
bool Read(const uint8_t* data, size_t len, Message& m);  // false: malformed (dropped)

// The line form between the dll and car_look.lua: tokens without spaces (escaped: %XX for anything outside
// [A-Za-z0-9_.:-], "-" for empty / unknown):
//   model c1 c2 plate tint interior dirt rust painting tuning
// tuning: "-" unknown, "." known and empty, else ids joined by ",".
std::string ToLine(const Look& l);
bool FromLine(const std::string& line, Look& l);  // the 10 tokens (more is fine: ignored)
std::string Escape(const std::string& s);
std::string Unescape(const std::string& s);

// What a copy showing `have` must change to show `want` (only what `want` knows and `have` differs in; the model is
// not changed here: a copy of another model is spawned anew). Empty: nothing to do.
struct Change {
    bool color = false, plate = false, tint = false, interior = false, dirt = false, rust = false, painting = false,
         tuning = false;
    std::vector<uint16_t> tuningIds;  // the items to install (want's items that `have` lacks; all when unknown)
    bool Empty() const {
        return !color && !plate && !tint && !interior && !dirt && !rust && !painting && !tuning;
    }
    std::string Describe() const;  // "colours, plate, tuning 2 items"
};
Change Diff(const Look& have, const Look& want);

// ---- the per-game book ------------------------------------------------------------------------------------------------
// Who sends a car's look: the game that has the car itself (not a copy of ours): a player car's first driver (the car
// key's owner: his real car, also while it is hidden behind a copy), the host for his traffic (only when asked). Only
// copies we spawned ever take a look (a real car is never changed: the source never takes one back, so no echo).
enum class Role : uint8_t { Source, Copy };

struct CarInput {
    car_damage::CarRef ref;  // kPlayer, kTraffic (guest: a host traffic copy), kHostTraffic (host: his traffic car)
    uint32_t entity = 0;
    Role role = Role::Copy;
    int owner = -1;          // Copy: the player whose game sends the look (-1 unknown: nobody to ask)
    bool read = false;       // `have` could be read
    Look have;
};

struct Apply {
    uint32_t entity = 0;
    car_damage::CarRef ref;
    Change change;
    Look want;
    int attempt = 0;  // 1 = first try
};

struct Outbox {
    std::vector<Message> broadcast;                      // our cars' looks (player cars)
    std::vector<std::pair<int, Message>> to;             // answers to requests
    std::vector<std::pair<int, Message>> requests;       // asks for a look
    std::vector<Apply> applies;
    std::vector<std::string> logs;
    std::vector<std::string> trace;                      // "CL..." record bodies (type first)
};

struct BookConfig {
    double requestEvery = 3.0;   // s between requests while a copy has no look
    double retryEvery = 2.0;     // s between two tries of a look that did not take
    int tries = 3;               // tries of one look on one entity before giving up (logged once)
    double minSendGap = 1.0;     // s between two looks of the same car from us (a change waits that long at most)
    double forget = 30.0;        // s a car may be missing from the rounds before it is forgotten
};

class Book {
public:
    explicit Book(BookConfig cfg = {}) : cfg_(cfg) {}
    // A look arrived (from the game that has the car). A copy takes the newest (seq) of its sender; another sender's
    // replaces it too (the car changed hands: a new owner game).
    void OnLook(const Message& m, int from, double now);
    void OnRequest(const car_damage::CarRef& ref, int from, double now);
    void Round(const std::vector<CarInput>& cars, double now, Outbox& out);
    void OnPlayerLeft(int id);
    void Clear() { cars_.clear(); }
    // refs whose look was asked for and that are not in the rounds (the host's traffic: Lua reads only those)
    std::vector<car_damage::CarRef> Wanted() const;

    struct Stats {
        uint32_t cars = 0, sources = 0, copies = 0, matching = 0, waiting = 0, failed = 0;
        uint64_t sent = 0, answers = 0, requests = 0, received = 0, applied = 0;
    };
    const Stats& GetStats() const { return stats_; }

private:
    struct Car {
        double seen = 0;
        // source
        bool haveSent = false;
        Look sent;
        uint16_t seq = 0;
        double sentAt = -1e9;
        std::vector<int> askers;
        double askAt = 0;
        // copy
        bool haveWant = false;
        Look want;
        uint16_t wantSeq = 0;
        int wantFrom = -1;
        uint32_t entity = 0;
        int tries = 0;
        double triedAt = -1e9;
        double requestAt = -1e9;
        bool gaveUp = false, matchLogged = false, modelWarned = false;
    };
    BookConfig cfg_;
    std::map<car_damage::CarRef, Car> cars_;
    Stats stats_;
};

}  // namespace mc::game::car_look
