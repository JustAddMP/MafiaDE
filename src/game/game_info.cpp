// Mafia: Definitive Edition descriptor for JustAddMP core (external/core/include/mc/game_info.h). Linked into
// MafiaCoop.exe, mafiacoop_proxy.dll and mafiacoop.dll, so it must stay free of other dependencies. The file names
// here must match what build.cmd produces.
#include "mc/game_info.h"

namespace mc {
namespace {

// Game builds every pattern/offset in docs/re-notes.md was derived from, identified by the CRC32 of
// mafiadefinitiveedition.exe. The launcher checks the exe against this list before starting the game.
constexpr GameBuild kBuilds[] = {
    {0xBCE2C8EFu, "Steam build 16502642"},  // = 3168979183 decimal, 111434896 bytes
};

constexpr GameInfo kInfo = {
    .modName = "MafiaCoop",
    // Bump when the wire format or sync changes: the session handshake derives its build number from it.
    .modVersion = "0.16.0",
    .launcherName = L"MafiaCoop.exe",
    .dllName = L"mafiacoop.dll",
    .iniName = L"MafiaCoop.ini",
    .logPrefix = L"mafiacoop",
    .envPrefix = "MAFIACOOP_",
    .defaultPort = 27960,
    .steamStatus = "Mafia co-op",
    .gameName = "Mafia: Definitive Edition",
    .gameExe = L"mafiadefinitiveedition.exe",
    .steamAppId = 1030840,
    .steamInstallDir = L"Mafia Definitive Edition",
    // The exe has no working single-instance check (docs/re-notes.md 3.2); /multiinst skips its leftover Mafia II
    // launcher mutex, so a second instance (two games on one PC for testing) can start.
    .gameArgs = L"/multiinst",
    .builds = kBuilds,
    .buildCount = sizeof(kBuilds) / sizeof(kBuilds[0]),
    .proxyBuiltName = L"mafiacoop_proxy.dll",
    .proxyPointerName = L"mafiacoop_path.txt",
    .proxyMarker = "MafiaCoop-proxy-dll:v1 (version.dll forwarder that loads mafiacoop.dll)",
    // Update check (core 0.13): https://justaddmp.com/api/version/mafia-de, the newest GitHub release of this repo.
    .updateId = "mafia-de",
};

}  // namespace

const GameInfo& GetGameInfo() { return kInfo; }

}  // namespace mc
