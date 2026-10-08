#include <utils/safe_win32.h>

#include <MinHook.h>
#include <utils/hooking/hook_function.h>
#include <utils/hooking/hooking.h>

#include "sdk/c_player_teleport_module.h"
#include "sdk/entities/c_actor.h"
#include "sdk/entities/c_car.h"
#include "sdk/entities/c_player_2.h"
#include "sdk/ue/game/humanai/c_character_controller.h"

#include "game/helpers/controls.h"

#include "core/application.h"

#include "sdk/mafia/ui/c_game_gui_2_module.h"
#include <sdk/c_entity_message_damage.h>
#include <logging/logger.h>

#include <core/modules/human.h>
#include <core/story_host.h>

#include <core_modules.h>
#include <networking/replication/replication_manager.h>

// The replicated human whose game ped carries this entity id (C_Entity::GetId, falling back to the
// 16-bit GUID), or 0. Resolved by scanning our own humans rather than the game entity list, so no
// game lookup with an unknown contract is needed.
static uint64_t FindHumanNetworkIdByEntityId(unsigned int entityId) {
    auto *repl = Framework::CoreModules::GetReplication();
    if (!repl || entityId == 0) {
        return 0;
    }
    uint64_t byId = 0, byGuid = 0;
    repl->ForEachEntity([&](Framework::Networking::Replication::NetworkEntity *e) {
        auto *human = dynamic_cast<MafiaMP::Core::Modules::Human *>(e);
        if (!human || !human->human) {
            return;
        }
        if (byId == 0 && human->human->GetId() == entityId) {
            byId = human->GetNetworkID();
        }
        if (byGuid == 0 && human->human->GetGUID() == static_cast<uint16_t>(entityId)) {
            byGuid = human->GetNetworkID();
        }
    });
    return byId != 0 ? byId : byGuid;
}

typedef void(__fastcall *C_Human2__SetupDeath_t)(SDK::C_Human2 *_this, SDK::C_EntityMessageDamage *);
C_Human2__SetupDeath_t C_Human2__SetupDeath_original = nullptr;
void __fastcall C_Human2__SetupDeath(SDK::C_Human2 *pThis, SDK::C_EntityMessageDamage *entityMsgDamage) {
    // Is the local player ?
    if (pThis == MafiaMP::Game::Helpers::Controls::GetLocalPlayer()) {
        Framework::Logging::GetLogger("Hooks")->debug("LocalPlayer just died");

        // If the local player is in a car, we just get him out first
        SDK::C_Car *currentCar = pThis ? (SDK::C_Car *)pThis->GetOwner() : nullptr;
        if (currentCar) {
            SDK::C_Actor *someActor = *(SDK::C_Actor **)((uint64_t)currentCar + 0xA8);
            pThis->GetCharacterController()->TriggerActorAction(someActor, SDK::E_AA_LEAVE_CAR, 0, true, false);
        }

        MafiaMP::Core::gApplication->GetLocalPlayerEvents().Died();
        // The story host plays the real campaign: the game's own death flow (mission failed,
        // checkpoint reload) must run, so only the report is ours.
        if (MafiaMP::Core::IsStoryHost()) {
            Framework::Logging::GetLogger("Story")->info("[Story] Local player died; letting the game's death flow run");
            return C_Human2__SetupDeath_original(pThis, entityMsgDamage);
        }
        return;
    }

    // An NPC this client simulates: tell the server who died and who did it, then let the game play
    // the death here. Observer copies are invulnerable and never reach this.
    if (auto *npc = MafiaMP::Core::Modules::Human::GetByPed(pThis); npc && npc->IsSimulatingNpc() && !npc->deathReported) {
        npc->deathReported      = true;
        const uint64_t killerId = entityMsgDamage ? FindHumanNetworkIdByEntityId(entityMsgDamage->m_iSourceEntityID) : 0;
        const int damageType    = entityMsgDamage ? static_cast<int>(entityMsgDamage->m_iDamageType) : 0;
        Framework::Logging::GetLogger("NPC")->info("[NPC] Human {} died on this client (killer {}, damage type {})", npc->GetNetworkID(), killerId, damageType);
        MafiaMP::Core::gApplication->GetLocalPlayerEvents().HumanDied(npc->GetNetworkID(), killerId, damageType);
    }

    // If not we just process as intended
    return C_Human2__SetupDeath_original(pThis, entityMsgDamage);
}

static InitFunction init(
    []() {
        // Hook the local player death so we can actually respawn it without the blue screen of the death
        const auto C_Human2__SetupDeath_addr = hook::pattern("48 8B C4 55 56 41 56 48 8D 68 ? 48 81 EC ? ? ? ? C7 45 ? ? ? ? ?").get_first();
        MH_CreateHook((LPVOID)C_Human2__SetupDeath_addr, (PBYTE)C_Human2__SetupDeath, reinterpret_cast<void **>(&C_Human2__SetupDeath_original));

    // Make sure C_AICommand_AimAt::C_AICommand_AimAt always try to use position and not entity
    // TODO make it pattern based
    hook::nop(0x0000001427DC07C, 0x4F);
    },
    "Human");
