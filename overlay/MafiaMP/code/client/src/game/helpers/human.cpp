#include <algorithm>
#include "human.h"

#include <glm/glm.hpp>

#include "game/overrides/character_controller.h"
#include "sdk/entities/c_car.h"
#include "sdk/entities/c_human_2.h"

namespace MafiaMP::Game::Helpers {
    uint8_t Human::GetHealthPercent(SDK::C_Human2 *human) {
        auto *script = human ? human->GetHumanScript() : nullptr;
        if (!script) {
            return 100;
        }
        float fHealth    = script->GetHealth();
        float fHealthMax = script->GetHealthMax();
        if (fHealthMax <= 0.0f) {
            return 100;
        }
        return (uint8_t)std::clamp((fHealth / fHealthMax) * 100.f, 0.0f, 100.0f);
    }

    // health is a percentage (0-100), the unit GetHealthPercent reports and the entity replicates.
    void Human::SetHealthPercent(SDK::C_Human2 *human, float health) {
        auto *script = human ? human->GetHumanScript() : nullptr;
        if (!script) {
            return;
        }
        float fHealthMax = script->GetHealthMax();
        return script->SetHealth(std::clamp((health / 100.0f) * fHealthMax, 0.0f, fHealthMax));
    }

    bool Human::PutIntoCar(MafiaMP::Game::Overrides::CharacterController *charController, SDK::C_Car *car, int seat, bool force) {
        if (!car) {
            return false;
        }

        SDK::C_Actor *act = *(SDK::C_Actor **)((uintptr_t)car + 0xA8);
        return charController->TriggerActorAction(act, SDK::E_AA_ENTER_CAR, seat, force, false);
    }

    bool Human::RemoveFromCar(MafiaMP::Game::Overrides::CharacterController *charController, SDK::C_Car *car, bool force) {
        if (!car) {
            return false;
        }

        SDK::C_Actor *act = *(SDK::C_Actor **)((uintptr_t)car + 0xA8);
        return charController->TriggerActorAction(act, SDK::E_AA_LEAVE_CAR, 0, force, false);
    }
    void Human::AddWeapon(SDK::C_Human2 *human, int weapon, int ammo) {
        if (!human)
            return;

        human->GetInventoryWrapper()->AddWeapon(weapon, ammo);
    }
} // namespace MafiaMP::Game::Helpers
