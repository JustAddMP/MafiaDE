#pragma once

#include <networking/replication/delegation.h>
#include <networking/replication/network_entity.h>

#include "shared/modules/human_sync.hpp"

#include <mafianet/string.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace MafiaMP::Shared::Entities {
    namespace Replication = Framework::Networking::Replication;

    // What the server wants an NPC human to do. The simulating client runs it with mod-side AI.
    enum class NpcCommand : uint8_t {
        None = 0,
        GoTo,   // walk/run to aiTargetPos and stand inside aiStopRadius
        Attack, // chase aiTargetId, keep a firing distance, aim and shoot
        Follow, // stay a few metres behind aiTargetId
        Flee,   // move away from aiTargetPos
    };

    enum class NpcMoveMode : uint8_t {
        Walk = 0,
        Run,
    };

    // A replicated human (player avatar or NPC). modelHash carries the spawn profile (skin) used by
    // the client to request the game ped and re-applied when it changes; nickname/playerIndex are
    // spawn-time metadata, and HumanSync::UpdateData carries the per-tick animation/weapon/health/
    // seating state. All of it syncs through the DeltaSerializer — there are no per-property RPCs.
    //
    // An NPC (isNpc) is server-owned for its lifetime and delegated to the nearest client for
    // simulation (see delegation.h). Its orders are server fields: the simulator reads them from the
    // forced state and never echoes them back.
    class HumanEntity : public Replication::NetworkEntity {
      public:
        static constexpr const char *kTypeName = "MafiaMP::Human";

        // Interest radius. The framework's 100 m default is about two seconds of road in a car.
        static constexpr float kStreamRange = 250.0f;

        // Delegation ranges for NPCs, on the XY ground plane. Below the stream range so the simulator
        // always has the ped on screen; the gap between the two is the hand-back hysteresis.
        static constexpr float kNpcAcquireRange = 150.0f;
        static constexpr float kNpcReleaseRange = 200.0f;

        uint64_t modelHash = 0;
        std::string nickname;
        uint16_t playerIndex = 0xFFFF;
        bool isNpc           = false;
        // Non-zero for a human the story host mirrors from its own game (kMirrorSpawn): the host
        // adopts the existing game ped carrying this token instead of requesting a new one.
        uint32_t mirrorToken = 0;
        Modules::HumanSync::UpdateData data {};

        // --- NPC orders (server-decided; see NpcCommand) ---
        uint8_t aiCommand     = static_cast<uint8_t>(NpcCommand::None);
        uint64_t aiTargetId   = 0;
        glm::vec3 aiTargetPos = glm::vec3(0.0f);
        float aiStopRadius    = 1.5f;
        uint8_t aiMoveMode    = static_cast<uint8_t>(NpcMoveMode::Run);
        bool dead             = false;

        // Bumped by the server each time a script writes health, so the owner applies a forced
        // health exactly once and not again on every later forced push (which would heal a hit
        // taken since).
        uint8_t healthRevision = 0;

        uint64_t GetSpawnProfile() const {
            return modelHash;
        }
        void SetSpawnProfile(uint64_t profile) {
            modelHash = profile;
        }

        NpcCommand GetCommand() const {
            return static_cast<NpcCommand>(aiCommand);
        }

        void OnSerializeConstruction(Replication::FieldSerializer &fields) override {
            MafiaNet::RakString name(nickname.c_str());
            fields.Field(name);
            fields.Field(playerIndex);
            fields.Field(isNpc);
            fields.Field(mirrorToken);
            if (!fields.Writing()) {
                nickname = name.C_String();
            }
        }

        void SerializeFields(Replication::FieldSerializer &fields) override {
            fields.Field(modelHash);
            fields.Field(data);
            ForEachServerField([&](auto &field) {
                fields.ServerField(field);
            });
        }

        void SerializeForcedState(Replication::FieldSerializer &fields) override {
            NetworkEntity::SerializeForcedState(fields);
            fields.Field(data._healthPercent);
            ForEachServerField([&](auto &field) {
                fields.Field(field);
            });
        }

        // A seated player is meaningless without the car under them.
        Replication::NetworkEntity *GetInterestDependency() override {
            return data.carPassenger.carId != 0 ? ResolveSibling(data.carPassenger.carId) : nullptr;
        }

        // Only NPCs are delegated; a player's avatar is owned by its connection for good.
        const Replication::DelegationPolicy *GetDelegationPolicy() const override {
            static const Replication::DelegationPolicy policy = [] {
                Replication::DelegationPolicy p;
                p.acquireRange = kNpcAcquireRange;
                p.releaseRange = kNpcReleaseRange;
                return p;
            }();
            return isNpc ? &policy : nullptr;
        }

        // An NPC's simulator may move it, never remove it.
        bool CanOwnerDestroy() const override {
            return !isNpc;
        }

      private:
        // Server-decided fields, in a fixed order: carried wherever the server writes (deltas to
        // observers, forced state to the owner) and absent from the owner's upstream stream.
        template <typename Fn>
        void ForEachServerField(Fn &&fn) {
            fn(aiCommand);
            fn(aiTargetId);
            fn(aiTargetPos);
            fn(aiStopRadius);
            fn(aiMoveMode);
            fn(dead);
            fn(healthRevision);
        }
    };
} // namespace MafiaMP::Shared::Entities
