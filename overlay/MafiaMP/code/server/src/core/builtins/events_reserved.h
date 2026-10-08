#pragma once

// MafiaMP-specific reserved scripting event names, raised through Events::EmitReserved. Framework
// events (resourceStart, serverStop, ...) live in the framework; only the mod's own go here.
namespace MafiaMP::Scripting::Events {
    // (player: Player)
    constexpr const char *kPlayerConnect    = "playerConnect";
    constexpr const char *kPlayerDisconnect = "playerDisconnect";
    constexpr const char *kPlayerDied       = "playerDied";

    // (vehicle: Vehicle, player: Player, seatIndex: number) / (vehicle: Vehicle, player: Player)
    constexpr const char *kVehiclePlayerEnter = "vehiclePlayerEnter";
    constexpr const char *kVehiclePlayerLeave = "vehiclePlayerLeave";

    // (human: Human, killer: Player | Human | undefined, damageType: number) — an NPC died.
    constexpr const char *kHumanDied = "humanDied";
    // (human: Human) — an NPC is about to be destroyed by Human.destroy(); the handle is still live.
    constexpr const char *kHumanDestroyed = "humanDestroyed";
} // namespace MafiaMP::Scripting::Events
