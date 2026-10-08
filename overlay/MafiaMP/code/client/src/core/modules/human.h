#pragma once

#include "shared/entities/human_entity.h"

#include "sdk/entities/c_player_2.h"

#include "game/ai/npc_brain.h"
#include "game/overrides/character_controller.h"
#include "game/streaming/entity_tracking_info.h"

#include <mafianet/ReplicaManager3.h>

#include <utils/interpolator.h>

#include <chrono>
#include <cstdint>

namespace SDK {
    class C_Car;
} // namespace SDK

enum CarEnterStates {
    STATE_OUTSIDE,
    STATE_ENTERING,
    STATE_LEAVING,
    STATE_INSIDE
};

namespace MafiaMP::Core::Modules {
    // Client-side replicated human: a HumanEntity that also owns and drives the local game ped.
    // Remote humans apply the replicated state to their ped (interpolated); the local player reads
    // its ped back into the replicated state each frame so it serializes upstream. An NPC this
    // client has been elected to simulate is driven by NpcBrain and read back the same way.
    class Human final : public Shared::Entities::HumanEntity {
      public:
        SDK::C_Human2 *human                                 = nullptr;
        Game::Overrides::CharacterController *charController = nullptr;
        Game::Streaming::EntityTrackingInfo *info            = nullptr;
        Framework::Utils::Interpolator interpolator {};

        // Local enter/leave-vehicle state machine.
        CarEnterStates enterState = STATE_OUTSIDE;
        bool enterForced          = false;

        bool isLocalPlayer = false;

        // Last skin applied to the game ped, to detect replicated skin changes.
        uint64_t appliedSkin = 0;

        // Last weapon the server gave this human (kHumanAddWeapon); what an attacking NPC draws.
        int lastWeaponAdded = 0;

        // Set once the simulating client has reported this NPC's death, so a repeated SetupDeath
        // does not report twice.
        bool deathReported = false;

        // Story host only: this entity mirrors a ped the host's own game spawned (see
        // core/mirror/world_mirror.h). The ped is the game's: it keeps its own character controller
        // (so none of the override-only setters may be called through charController), is never
        // teleported or driven by us, and only has its state captured and streamed upstream.
        bool adopted = false;

        // --- Replica3 hooks ---
        void OnConstructed() override;
        void DeallocReplica(MafiaNet::Connection_RM3 *sourceConnection) override;
        void SerializeFields(Framework::Networking::Replication::FieldSerializer &fields) override;
        // Server overrode our state (teleport, health, NPC orders): apply it to the game ped.
        void OnStateForced() override;

        // Per-frame driver for this human.
        void Frame();

        // Teleport the local player's game ped to this entity's replicated transform.
        void TeleportLocalToReplicated();

        // Story host: the game replaces the player ped between chapters and cutscenes, and there
        // is none while the vanilla main menu runs. Re-resolve it every frame.
        void RebindLocalPlayer();

        // True on the client elected to run this NPC (never for a player's avatar).
        bool IsSimulatingNpc() const {
            return isNpc && IsOwner();
        }

        // Gaining or losing the simulation of this NPC: toggle the observer immunity and reset the AI.
        void OnSimulationChanged(bool simulating);
        // Apply a server-forced health to the ped we drive (local player or simulated NPC), once.
        void ApplyForcedHealth();

        // Called by a vehicle being returned to the game: anyone seated in it must let go first.
        void OnCarReleased(SDK::C_Car *car);

        // --- Module setup (called once from the client world) ---
        static void Install();
        static void UpdateAll();
        static Human *GetByPed(SDK::C_Human2 *ptr);

      private:
        void BindLocalPlayer();
        void RequestPed();
        void ApplyRemote();
        void ReadLocal();
        bool CaptureGuarded(); // ReadLocal under a structured-exception guard (mirrored game peds only)
        // Read the game ped back into the replicated state (pose, health, locomotion, weapon,
        // animation). Shared by the local player and a simulated NPC.
        void CapturePedState();
        void SimulateNpc();
        void DrawNametag();

        Game::AI::NpcBrain _brain {};
        bool _simulating               = false;
        uint8_t _appliedHealthRevision = 0;
        bool _spawnTimeoutLogged       = false;
        std::chrono::steady_clock::time_point _requestedAt {};
    };
} // namespace MafiaMP::Core::Modules
