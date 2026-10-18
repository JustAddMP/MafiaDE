#pragma once
// Car signals: the lights, indicators, horn, siren and beacon of a car, as one byte on the wire (PlayerState vehicle
// block, docs/protocol.md "PlayerState") and as the raw bits the game keeps in the car's vehicle part
// (docs/re-notes.md 9 "Lights, horn, siren"). No game code here: src/game/lua.cpp reads and writes the game's fields,
// player_sync.cpp/.lua carry the byte, tests/car_signals.cpp checks the packing.
//
// What each game bit means was found statically (setters and their callers), NOT checked in game:
//   vehicle part + 0xD00 (dword, the lights block at +0xCF0, field +0x10):
//     0x01  light group 1: set with group 2 by SetLightState(true, true), kept when the car is parked: taken to be
//           the side / parking lights
//     0x02  light group 2: the one the game notifies a listener about, cleared when the car is parked: taken to be
//           the headlights
//     0x08, 0x10  set by one setter (exe+0x1C0AB60, r8b selects which), copied to a trailer with the brake and
//           reverse bits (0x04, 0x20) and set from a controls struct as a pair: the two indicators (which one is left
//           is not known; both = hazard)
//     0x40  beacon (C_WrapperCar::SetBeaconLightOn)
//   vehicle part + 0xA88 (byte): horn (C_WrapperCar::SetHornOn and the driver's controls, exe+0x1C0A820)
//   vehicle part + 0x119C (dword) bit 0x2000000: siren (C_WrapperCar::SetSirenOn, exe+0x1C0DFC0)
// Brake and reverse lights (0x04, 0x20) are not carried: the copy makes them itself from the brake and gear it gets.
#include <cstdint>
#include <string>

namespace mc::game::car_signals {

// wire bits
enum : uint8_t {
    kPark = 0x01,      // side / parking lights (game lights bit 0x01)
    kHead = 0x02,      // headlights (game lights bit 0x02)
    kIndA = 0x04,      // indicator A (game lights bit 0x08)
    kIndB = 0x08,      // indicator B (game lights bit 0x10)
    kBeacon = 0x10,    // beacon lights (game lights bit 0x40)
    kSiren = 0x20,     // siren
    kHorn = 0x40,      // horn sounding
    kValid = 0x80,     // the driver's game could read them; without it the rest is 0 and the copy is left alone
};
constexpr uint8_t kLightBits = kPark | kHead | kIndA | kIndB;  // set on the copy through the lights block
constexpr uint8_t kAll = kLightBits | kBeacon | kSiren | kHorn;

// game bits (vehicle part offsets in lua.cpp)
constexpr uint32_t kGamePark = 0x01, kGameHead = 0x02, kGameIndA = 0x08, kGameIndB = 0x10, kGameBeacon = 0x40;
constexpr uint32_t kGameSiren = 0x2000000;  // in vehicle part + 0x119C

// The wire byte from what the game holds (lights dword, horn byte, siren dword).
inline uint8_t FromGame(uint32_t lights, uint8_t horn, uint32_t sirenWord) {
    uint8_t s = kValid;
    if (lights & kGamePark) s |= kPark;
    if (lights & kGameHead) s |= kHead;
    if (lights & kGameIndA) s |= kIndA;
    if (lights & kGameIndB) s |= kIndB;
    if (lights & kGameBeacon) s |= kBeacon;
    if (sirenWord & kGameSiren) s |= kSiren;
    if (horn) s |= kHorn;
    return s;
}

// The game lights bits a wire byte stands for (park, head, indicators, beacon).
inline uint32_t GameLights(uint8_t s) {
    uint32_t l = 0;
    if (s & kPark) l |= kGamePark;
    if (s & kHead) l |= kGameHead;
    if (s & kIndA) l |= kGameIndA;
    if (s & kIndB) l |= kGameIndB;
    if (s & kBeacon) l |= kGameBeacon;
    return l;
}

// A byte from the network: without the valid bit nothing else counts.
inline uint8_t Sanitize(uint8_t s) { return (s & kValid) ? s : 0; }

// "head+park ind=A horn siren beacon", "off", or "unknown" (not valid).
inline std::string Describe(uint8_t s) {
    if (!(s & kValid)) return "unknown";
    std::string out;
    auto add = [&out](const char* w) {
        if (!out.empty()) out += ' ';
        out += w;
    };
    if (s & (kPark | kHead)) add((s & kHead) && (s & kPark) ? "lights=park+head" : (s & kHead) ? "lights=head" : "lights=park");
    if ((s & kIndA) && (s & kIndB))
        add("ind=hazard");
    else if (s & kIndA)
        add("ind=A");
    else if (s & kIndB)
        add("ind=B");
    if (s & kHorn) add("horn");
    if (s & kSiren) add("siren");
    if (s & kBeacon) add("beacon");
    return out.empty() ? "off" : out;
}

// What changed from `a` to `b`: "head on, horn off", "" when nothing did, "valid" / "lost" for the valid bit.
inline std::string Changes(uint8_t a, uint8_t b) {
    static const struct {
        uint8_t bit;
        const char* name;
    } kNames[] = {{kPark, "park"},  {kHead, "head"},   {kIndA, "indA"}, {kIndB, "indB"},
                  {kBeacon, "beacon"}, {kSiren, "siren"}, {kHorn, "horn"}};
    if ((a ^ b) & kValid) return (b & kValid) ? "valid" : "lost";
    std::string out;
    for (const auto& n : kNames) {
        if (!((a ^ b) & n.bit)) continue;
        if (!out.empty()) out += ", ";
        out += n.name;
        out += (b & n.bit) ? " on" : " off";
    }
    return out;
}

// Wire: one byte at the very end of the PlayerState vehicle block (after the gear). A state from a game without it
// (the block ends at the gear) reads as 0 = unknown, and an older reader never gets to the extra byte.
template <class Writer>
void Write(Writer& w, uint8_t s) {
    w.U8(Sanitize(s));
}
template <class Reader>
uint8_t Read(Reader& r) {
    return r.AtEnd() ? 0 : Sanitize(r.U8());
}

}  // namespace mc::game::car_signals
