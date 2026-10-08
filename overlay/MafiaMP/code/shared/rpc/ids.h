#pragma once

namespace MafiaMP::Shared::RPC {
    // RPC4 identifiers for transient events and commands. Senders Signal() by these names; receivers
    // RegisterSlot() the same name. The argument wire format (the order of BitStream reads/writes) is
    // documented at each handler/send site.
    //
    // Continuous per-entity state (health, skin, vehicle properties, seating) is NOT here: it lives
    // on the replicated entity and syncs via the DeltaSerializer, not via RPC.

    // Human events (relayed from the acting client to others; the first field is the actor NetworkID)
    constexpr const char *kHumanShoot     = "MafiaMP::HumanShoot";
    constexpr const char *kHumanReload    = "MafiaMP::HumanReload";
    constexpr const char *kHumanDeath     = "MafiaMP::HumanDeath";
    constexpr const char *kHumanAddWeapon = "MafiaMP::HumanAddWeapon";
    // NPC death reported by its simulating client: <human NetworkID><killer NetworkID or 0><damageType>
    constexpr const char *kHumanNpcDeath = "MafiaMP::HumanNpcDeath";

    // Vehicle events (player resolved from the sender; carries the vehicle NetworkID)
    constexpr const char *kVehiclePlayerEnter = "MafiaMP::VehiclePlayerEnter";
    constexpr const char *kVehiclePlayerLeave = "MafiaMP::VehiclePlayerLeave";

    // Commands
    constexpr const char *kSetEnvironment = "MafiaMP::SetEnvironment";
    constexpr const char *kSpawnCar       = "MafiaMP::SpawnCar";

    // --- Story host world mirror (see client core/mirror/world_mirror.h) ---
    // Client -> server, empty: the sender runs the genuine campaign and wants to mirror its world.
    // The first connection to say so becomes the story host; later ones are refused.
    constexpr const char *kStoryHostHello = "MafiaMP::StoryHostHello";
    // Story host -> server: the campaign's current weather set and clock, relayed to the guests.
    // Wire: <RakString weatherSet><float dayTimeHours>
    constexpr const char *kStoryEnvironment = "MafiaMP::StoryEnvironment";
    // Client -> server: <uint8 kind (0 human, 1 car)><uint32 token><uint64 modelHash (human) |
    // RakString modelName (car)><float x><float y><float z><float qw><float qx><float qy><float qz>.
    // The token is client-chosen and comes back in kMirrorSpawned and in the entity construction.
    constexpr const char *kMirrorSpawn = "MafiaMP::MirrorSpawn";
    // Server -> host: <uint32 token><uint64 NetworkID or 0 when refused><uint8 kind>
    constexpr const char *kMirrorSpawned = "MafiaMP::MirrorSpawned";
    // Client -> server: <uint64 NetworkID> of a mirrored entity whose game object is gone.
    constexpr const char *kMirrorDespawn = "MafiaMP::MirrorDespawn";
} // namespace MafiaMP::Shared::RPC
