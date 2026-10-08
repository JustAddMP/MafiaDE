#pragma once

#include <v8.h>
#include <v8pp/class.hpp>
#include <v8pp/convert.hpp>

#include "shared/entities/human_entity.h"

#include <scripting/builtins/entity.h>
#include <scripting/builtins/player.h>
#include <scripting/builtins/vector3.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <string>

namespace MafiaMP::Scripting {
    class Human: public Framework::Scripting::Builtins::Player {
      public:
        Human(uint64_t networkId);

        // Resolves the handle as a HumanEntity, or nullptr if it is gone / not a human.
        Shared::Entities::HumanEntity *ResolveHuman() const;

        // The argument is the dying NPC's NetworkID; killerId is 0 when unknown.
        static void EventHumanDied(uint64_t networkId, uint64_t killerId, int damageType);
        static void EventHumanDestroyed(uint64_t networkId);

        std::string ToString() const override;

        bool IsAiming() const;
        bool IsFiring() const;

        void AddWeapon(int weaponId, int ammo);

        Framework::Scripting::Builtins::Vector3 GetAimDir() const;
        Framework::Scripting::Builtins::Vector3 GetAimPos() const;

        float GetHealth() const;
        void SetHealth(float health);

        std::string GetNickname() const;

        v8::Local<v8::Value> GetVehicle(v8::Isolate *isolate) const;
        uint64_t GetVehicleId() const;
        int GetVehicleSeatIndex() const;

        uint16_t GetWeaponId() const;

        // --- NPC surface (no-ops / defaults on a player's avatar) ---
        bool IsNpc() const;
        bool IsDead() const;
        // Queues an NPC for removal on the next server tick (raises humanDestroyed once, now).
        // Returns false for a player avatar or an NPC already queued.
        bool Destroy();

        void GoTo(const Framework::Scripting::Builtins::Vector3 &pos, bool run);
        void Attack(Framework::Scripting::Builtins::Entity *target);
        void Follow(Framework::Scripting::Builtins::Entity *target);
        void Flee(const Framework::Scripting::Builtins::Vector3 &from);
        void ClearOrders();

        std::string GetOrder() const;
        uint64_t GetOrderTargetId() const;
        Framework::Scripting::Builtins::Vector3 GetOrderTargetPos() const;

        std::string GetMoveMode() const;
        void SetMoveMode(std::string mode);

        float GetStopRadius() const;
        void SetStopRadius(float radius);

        static void Register(v8::Isolate *isolate, v8::Local<v8::Object> global);
        static v8pp::class_<Human> &GetClass(v8::Isolate *isolate);

      private:
        // Mutates an NPC's order fields, then pushes them to its simulator (ForceState is a no-op
        // while nobody simulates it; the next handover forces the full state anyway).
        template <typename Mutator>
        void MutateOrders(Mutator &&mutate);

        inline static std::unordered_map<v8::Isolate *, std::unique_ptr<v8pp::class_<Human>>> _classes;

      public:
        // Drop the class wrapper of a disposed isolate (resource runtimes come and go).
        static void UnregisterIsolate(v8::Isolate *isolate) {
            _classes.erase(isolate);
        }
    };
} // namespace MafiaMP::Scripting
