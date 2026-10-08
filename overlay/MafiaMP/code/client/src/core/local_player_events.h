#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace MafiaMP::Core::Modules {
    class Vehicle;
} // namespace MafiaMP::Core::Modules

namespace MafiaMP::Core {
    // Central client-side dispatcher for local-player game events raised by the game hooks. The hooks
    // stay thin (they just detect the game event and forward here); this is the single place that
    // owns the matching net broadcast to the server, and the natural seam to also raise client-side
    // scripting events.
    class LocalPlayerEvents {
      public:
        void Died();
        void Shot(const glm::vec3 &aimPos, const glm::vec3 &aimDir, bool unk0, bool unk1);
        void Reloaded(int mode);

        // The same broadcasts for a human this client simulates but does not play (an NPC).
        void HumanShot(uint64_t humanId, const glm::vec3 &aimPos, const glm::vec3 &aimDir, bool unk0, bool unk1);
        void HumanReloaded(uint64_t humanId, int mode);
        // An NPC this client simulates was killed by the game. killerId is 0 when unknown.
        void HumanDied(uint64_t humanId, uint64_t killerId, int damageType);
        void EnteredVehicle(Modules::Vehicle *vehicle, int seatIndex);
        void LeftVehicle(Modules::Vehicle *vehicle);
    };
} // namespace MafiaMP::Core
