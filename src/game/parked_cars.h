#pragma once
// Parked cars for late joiners and reconnects (docs/protocol.md "ParkedCars"). Every shared car that was driven and
// parked in this session is known by its CarParked (0x0101: key, model, final transform, flags). A player who joins
// later - or comes back after a lost connection (a new joinSeq) - missed those messages and did not see such a car
// until somebody drove it. The host keeps a Book of every CarParked it sent or received (a car someone drives again
// leaves it) and sends a newcomer one snapshot of it (ParkedCars 0x0104, reliable, in messages of up to kPerMessage
// cars); the receiver applies each car as if its CarParked had just come (player_sync.lua M.Parked). Pure logic and
// the wire codec, no game calls: tests/parked_cars_test.cpp.
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "net/wire.h"

namespace mc::game::parked {

constexpr uint16_t kMsgParkedCars = 0x0104;
constexpr uint8_t kVersion = 1;
constexpr uint8_t kWrecked = 1;      // flags bit 0: destroyed on its last driver's game (as CarParked)
constexpr size_t kPerMessage = 24;   // cars per message (~50 bytes each)

struct Car {
    uint8_t owner = 0;   // car key = (owner, guid): its first driver's id and the car's GUID on his game
    uint32_t guid = 0;
    std::string model;
    float t[7] = {0, 0, 0, 0, 0, 0, 1};  // x y z, quaternion x y z w
    uint8_t flags = 0;
    bool operator==(const Car& o) const;
};

using Key = std::pair<uint8_t, uint32_t>;

// A car that can go into a Lua command line and a transform the game can take: model printable ASCII without
// spaces (1..48 characters), position within 1e6 m, quaternion components within 2, all finite.
bool Valid(const Car& c);

// One message: u8 version, u8 count, count x { u8 owner, u32 guid, str model, 7 x f32, u8 flags }. At most
// kPerMessage cars are written (the rest is ignored: Split first).
void Write(net::ByteWriter& w, const std::vector<Car>& cars);
// False for a malformed message (wrong version, truncated, a car that is not Valid): nothing of it is used.
bool Read(net::ByteReader& r, std::vector<Car>& out);
// The cars in messages of at most kPerMessage (an empty list: no message).
std::vector<std::vector<Car>> Split(const std::vector<Car>& all);

// The host's record of the parked shared cars.
class Book {
public:
    // A CarParked was sent or received: the car rests there now (a newer one replaces it).
    void Parked(const Car& c);
    // Somebody drives car (owner, guid) again: no longer parked. True when it was in the book.
    bool Driven(uint8_t owner, uint32_t guid);
    std::vector<Car> Snapshot() const;  // in key order
    size_t Size() const { return cars_.size(); }
    void Clear() { cars_.clear(); }
    static constexpr size_t kMax = 512;  // a session that parked more forgets the lowest keys first (never seen)

private:
    std::map<Key, Car> cars_;
};

}  // namespace mc::game::parked
