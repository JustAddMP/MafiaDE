#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace SDK {
    class C_Entity;
} // namespace SDK

namespace Framework::Networking::Replication {
    class NetworkEntity;
} // namespace Framework::Networking::Replication

namespace MafiaMP::Core::Modules {
    class Human;
    class Vehicle;
} // namespace MafiaMP::Core::Modules

namespace MafiaMP::Core::Mirror {
    // Story-host world mirror (client side; see shared/rpc/ids.h for the wire format).
    //
    // The host runs the genuine campaign, so every human and car its game spawns exists only in
    // its process. Every kScanInterval this walks the game's entity list and, for each human/car
    // that is not already network-backed, asks the server to create a host-owned entity for it
    // (kMirrorSpawn). When that entity's construction comes back carrying the same token, the
    // existing game object is ADOPTED by the replicated entity instead of a new ped/car being
    // requested: the host then only captures the game object's state and streams it upstream as
    // the owner, exactly like its own avatar. Guests see the mirrored entities as ordinary remote
    // NPCs/vehicles. When the game object disappears, kMirrorDespawn removes the entity.
    class WorldMirror final {
      public:
        enum class Kind : uint8_t {
            Human = 0,
            Car   = 1,
        };

        // Spawn profile used for every mirrored human: the real profile of a game ped cannot yet be
        // read back, and an unknown profile would leave the guest without a ped at all.
        static constexpr uint64_t kDefaultHumanProfile = 335218123840277515ULL;

        // Mirror radius around the host's ped. Humans stay under HumanEntity::kStreamRange (250 m)
        // so the server's interest always constructs them back to the host for adoption; cars sit
        // under VehicleEntity::kStreamRange (350 m).
        static constexpr float kHumanRange = 240.0f;
        static constexpr float kCarRange   = 300.0f;
        // A mirrored object this far beyond its range is released again (hysteresis).
        static constexpr float kReleaseMargin = 60.0f;

        static constexpr size_t kMaxHumans = 96;
        static constexpr size_t kMaxCars   = 64;

        static constexpr auto kScanInterval       = std::chrono::milliseconds(250);
        // A game object is only mirrored once it has existed this long: spawner pools create and
        // delete prototype peds within a few hundred milliseconds, and a ped is not fully set up
        // (controller, state handler, script object) on the frame it appears.
        static constexpr auto kAdoptDelay         = std::chrono::milliseconds(700);
        static constexpr auto kConstructionWarnAt = std::chrono::seconds(10);

        // Registers the kMirrorSpawned slot. Once, from Application::InitNetworkingMessages.
        static void Install();

        // Per frame from the application; no-op unless this client is the story host.
        static void Update();

        // The connection went away: forget everything (the server destroys what it mirrored).
        static void Reset();

        // Called from Human/Vehicle::OnConstructed on the host when the construction carries a
        // mirror token: bind the game object waiting under that token. False when nothing waits
        // (the mirror was reset since the spawn was asked for).
        static bool TryAdoptHuman(Modules::Human *human);
        static bool TryAdoptVehicle(Modules::Vehicle *vehicle);

        // Called from DeallocReplica: the replicated entity is gone (despawn acknowledged, server
        // script destroyed it, or disconnect). The game object, if it still exists, is left alone
        // and not mirrored again until the game itself drops it.
        static void OnEntityGone(Framework::Networking::Replication::NetworkEntity *entity);

        // Ask the server to drop a mirrored entity.
        static void RequestDespawn(uint64_t networkId);

        // Is this game object still in the game's entity list right now? Adopted entities must ask
        // before touching their ped/car every frame: ambient spawners create and delete objects
        // between two scans, and a deleted object is freed memory. The answer is rebuilt at most
        // once per frame from the entity list (cheap: one virtual call per entity).
        static bool IsPresent(const SDK::C_Entity *game);

        // Capturing this mirrored object faulted (half-initialised game object): drop it and never
        // mirror it again. Called from the guarded capture in Human/Vehicle::Frame.
        static void Quarantine(SDK::C_Entity *game, const char *why);

        // kMirrorSpawned arrived: the server's answer to a spawn.
        static void OnMirrorSpawned(uint32_t token, uint64_t networkId, Kind kind);

        static size_t TrackedHumans() {
            return _humanCount;
        }
        static size_t TrackedCars() {
            return _carCount;
        }

      private:
        struct Entry {
            Kind kind          = Kind::Human;
            uint32_t token     = 0;
            SDK::C_Entity *game = nullptr;
            // Known once kMirrorSpawned arrives (or the construction, whichever is first).
            uint64_t networkId = 0;
            // The replicated entity once constructed and adopted.
            Framework::Networking::Replication::NetworkEntity *entity = nullptr;
            std::chrono::steady_clock::time_point requestedAt {};
            bool constructionWarned = false;
        };

        static void SendHello();
        static void Scan();
        // Sends the game's weather set + clock to the server when they changed (or every 2 s).
        static void ReportEnvironment();
        // Rebuilds _present from the entity list and releases mirrored objects that vanished.
        static void RefreshPresence();
        static void Spawn(Kind kind, SDK::C_Entity *game, uint64_t modelHash, const char *modelName);
        // Unbind the replicated entity from its (gone or released) game object and drop the entry.
        static void Release(SDK::C_Entity *game, const char *why);
        static void Detach(Entry &entry);
        static const char *ResolveCarModelName(SDK::C_Entity *car);
        static void LoadKnownCarModels();

        static inline std::unordered_map<SDK::C_Entity *, Entry> _byGame;
        static inline std::unordered_map<uint32_t, SDK::C_Entity *> _byToken;
        // Game objects not to mirror (refused by the server, or their entity was destroyed by the
        // server while the object lives on). Cleared when the object leaves the entity list.
        static inline std::unordered_set<SDK::C_Entity *> _ignored;
        static inline std::unordered_set<std::string> _knownCarModels;
        static inline bool _knownCarModelsLoaded = false;

        static inline uint32_t _nextToken = 1;
        static inline bool _helloSent     = false;
        static inline bool _humanCapLogged = false;
        static inline bool _carCapLogged   = false;
        static inline size_t _humanCount   = 0;
        static inline size_t _carCount     = 0;
        static inline std::chrono::steady_clock::time_point _lastScan {};
        static inline std::unordered_set<const SDK::C_Entity *> _present;
        static inline std::string _lastWeather;
        static inline float _lastDayTime = -1.0f;
        static inline std::chrono::steady_clock::time_point _lastEnvReport {};
        // When each candidate game object was first seen (pruned when it leaves the entity list).
        static inline std::unordered_map<SDK::C_Entity *, std::chrono::steady_clock::time_point> _firstSeen;
        static inline bool _presenceDirty = true;
    };
} // namespace MafiaMP::Core::Mirror
