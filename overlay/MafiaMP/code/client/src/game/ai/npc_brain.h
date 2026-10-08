#pragma once

#include <chrono>
#include <cstdint>

namespace MafiaMP::Core::Modules {
    class Human;
} // namespace MafiaMP::Core::Modules

namespace MafiaMP::Game::AI {
    // Mod-side AI for a server-spawned NPC, run by the client that simulates it. The game's own AI
    // (commands, navmesh, factions) is not driven: the ped is moved in a straight line with SetPos
    // while the override character controller plays the matching locomotion, exactly the way a
    // remote player's ped is moved today. Orders come from the entity's server fields.
    class NpcBrain final {
      public:
        // Straight-line speeds, in m/s.
        static constexpr float kWalkSpeed = 1.6f;
        static constexpr float kRunSpeed  = 5.5f;

        // Follow: start walking again beyond the resume distance, stop inside the stop distance.
        static constexpr float kFollowStop   = 3.5f;
        static constexpr float kFollowResume = 6.0f;
        // Attack: close in beyond the resume distance, hold position inside the stop distance, and
        // fire while the target is within range.
        static constexpr float kAttackStop   = 9.0f;
        static constexpr float kAttackResume = 14.0f;
        static constexpr float kAttackRange  = 45.0f;
        // Flee: run until this far from the point being fled.
        static constexpr float kFleeDistance = 60.0f;

        // Fire in bursts: trigger held for kFireOn, released for kFireOff.
        static constexpr float kFireOn  = 0.4f;
        static constexpr float kFireOff = 0.8f;

        // Forget the last tick, so the first update after (re)gaining the simulation is a no-op step.
        void Reset();

        // One frame of simulation for an NPC this client owns. The ped and controller must exist.
        void Update(Core::Modules::Human &npc);

      private:
        float TickDelta();
        void Stand(Core::Modules::Human &npc);
        void StopAttacking(Core::Modules::Human &npc);

        std::chrono::steady_clock::time_point _lastTick {};
        bool _hasLastTick = false;

        bool _moving    = false;
        bool _attacking = false;
        bool _firing    = false;
        float _fireTimer  = 0.0f;
        float _aimAtTimer = 0.0f;
        int _loggedCommand = -1;
    };
} // namespace MafiaMP::Game::AI
