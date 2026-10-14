#pragma once
// MafiaCoop release version and the game builds it supports (shared by the launcher and the dll).
#include <cstdint>

namespace mc {

// Bump when the wire format or sync changes; the session handshake derives its build number from it (dll/coop.cpp).
constexpr const char* kModVersion = "0.2.0";

// Game builds every pattern/offset in docs/re-notes.md was derived from, identified by the CRC32 of
// mafiadefinitiveedition.exe. The launcher checks the exe against this list before starting the game.
struct GameBuild {
    uint32_t crc32;
    const char* label;
};
constexpr GameBuild kSupportedGameBuilds[] = {
    {0xBCE2C8EFu, "Steam build 16502642"},  // = 3168979183 decimal, 111434896 bytes
};

} // namespace mc
