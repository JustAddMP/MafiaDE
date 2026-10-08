#include <utils/safe_win32.h>

#include <cstring>
#include <unordered_set>

#include "human.h"

#include "vehicle.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include <external/imgui/widgets/nametag.h>

#include "core/application.h"
#include "core/mirror/world_mirror.h"
#include "core/story_host.h"

#include "game/helpers/controls.h"
#include "game/helpers/human.h"
#include "game/overrides/character_controller.h"
#include "sdk/c_game.h"
#include "sdk/c_player_teleport_module.h"
#include "sdk/entities/c_car.h"
#include "sdk/entities/c_player_2.h"
#include "sdk/ue/game/anim/e_wanim_behavior_var.h"
#include "sdk/ue/game/human/c_behavior_character.h"
#include "sdk/ue/game/humanai/c_character_state_handler.h"
#include "sdk/ue/game/vehicle/c_vehicle.h"
#include "sdk/ue/game/camera/c_game_camera.h"
#include "sdk/wrappers/c_human_2_car_wrapper.h"

#include "shared/entities/register_entities.h"
#include "shared/rpc/ids.h"

#include <core_modules.h>
#include <logging/logger.h>
#include <networking/network_peer.h>
#include <networking/replication/entity_registry.h>
#include <networking/replication/replication_manager.h>

#include <mafianet/BitStream.h>

namespace MafiaMP::Core::Modules {
    namespace {
        // Observers of a dead NPC: switch their copy of the ped into the game's death handler (plays
        // the death on every client) or merely freeze it where it stands. The death handler has not
        // been exercised on a demigod observer ped, so it is off until an in-game trial says it is
        // safe; the gamemode despawns the corpse from humanDied either way.
        constexpr bool kObserverEntersDeathHandler = false;

        // How long a requested ped may stay unloaded before the spawn profile is reported as suspect.
        constexpr auto kSpawnWarnDelay = std::chrono::seconds(10);

        Human *ResolveHuman(uint64_t networkId) {
            auto *world = Framework::CoreModules::GetReplication();
            return world ? dynamic_cast<Human *>(world->GetEntityByNetworkID(networkId)) : nullptr;
        }

        // Fires when the streamed game ped is ready; binds it to its ClientHuman.
        void OnHumanRequestFinish(Game::Streaming::EntityTrackingInfo *info, bool success) {
            CreateNetCharacterController = false;
            if (!success) {
                return;
            }
            auto human = reinterpret_cast<SDK::C_Human2 *>(info->GetEntity());
            if (!human) {
                return;
            }
            human->GameInit();
            human->Activate();

            auto *self = static_cast<Human *>(info->GetNetworkEntity());
            if (!self) {
                return;
            }

            SDK::ue::sys::math::C_Vector newPos    = {self->position.x, self->position.y, self->position.z};
            SDK::ue::sys::math::C_Quat newRot      = {self->rotation.x, self->rotation.y, self->rotation.z, self->rotation.w};
            SDK::ue::sys::math::C_Matrix transform = {};
            transform.Identity();
            transform.SetRot(newRot);
            transform.SetPos(newPos);
            human->SetTransform(transform);

            self->human          = human;
            self->charController = reinterpret_cast<MafiaMP::Game::Overrides::CharacterController *>(human->GetCharacterController());
            assert(MafiaMP::Game::Overrides::CharacterController::IsInstanceOfClass(self->charController));

            if (self->isNpc) {
                // Immune until proven otherwise: a dormant NPC gets no delta (and so no ApplyRemote)
                // until something changes. If we were elected while the ped was loading, take the
                // simulation now (vulnerable, forced health applied); Frame() below then runs it.
                human->GetHumanScript()->SetDemigod(true);
                human->GetHumanScript()->SetInvulnerabilityByScript(true);
                Framework::Logging::GetLogger("NPC")->info("[NPC] Human {} ped ready (profile {})", self->GetNetworkID(), self->modelHash);
                if (self->IsSimulatingNpc()) {
                    self->OnSimulationChanged(true);
                    self->ApplyForcedHealth();
                }
            }

            // Apply whatever state has arrived so far.
            self->Frame();
        }

        void OnHumanReturned(Game::Streaming::EntityTrackingInfo *info, bool wasCreated) {
            if (!info) {
                return;
            }
            auto human = reinterpret_cast<SDK::C_Human2 *>(info->GetEntity());
            if (wasCreated && human) {
                human->Deactivate();
                human->GameDone();
                human->Release();
            }
        }

        // --- Raw RPC4 event handlers ---

        // Wire: <human NetworkID><aimPos><aimDir><unk0><unk1>
        void OnHumanShoot(MafiaNet::BitStream *bs, MafiaNet::Packet *, void *) {
            uint64_t id = 0;
            glm::vec3 aimPos {}, aimDir {};
            bool unk0 = false, unk1 = false;
            bs->Read(id);
            bs->Read(aimPos);
            bs->Read(aimDir);
            bs->Read(unk0);
            bs->Read(unk1);

            auto *self = ResolveHuman(id);
            if (!self || !self->human) {
                return;
            }
            const auto wepController = self->human->GetHumanWeaponController();
            if (!wepController) {
                return;
            }
            SDK::ue::sys::math::C_Vector pos = {aimPos.x, aimPos.y, aimPos.z};
            SDK::ue::sys::math::C_Vector dir = {aimDir.x, aimDir.y, aimDir.z};
            wepController->DoShot(nullptr, &pos, &dir, unk0, unk1);
        }

        // Wire: <human NetworkID><unk0>
        void OnHumanReload(MafiaNet::BitStream *bs, MafiaNet::Packet *, void *) {
            uint64_t id = 0;
            int unk0    = 0;
            bs->Read(id);
            bs->Read(unk0);

            auto *self = ResolveHuman(id);
            if (!self || !self->human) {
                return;
            }
            const auto wepController = self->human->GetHumanWeaponController();
            if (wepController) {
                wepController->DoWeaponReloadInventory(unk0);
            }
        }

        // Wire: <human NetworkID><weaponId><ammo>
        void OnHumanAddWeapon(MafiaNet::BitStream *bs, MafiaNet::Packet *, void *) {
            uint64_t id = 0;
            int weaponId = 0, ammo = 0;
            bs->Read(id);
            bs->Read(weaponId);
            bs->Read(ammo);

            auto *self = ResolveHuman(id);
            if (!self || !self->human) {
                return;
            }
            self->lastWeaponAdded = weaponId;
            // On the story host the game itself owns the player's and the mirrored peds' inventories;
            // the broadcast only exists so that the OTHER clients' copies carry the weapon.
            if (IsStoryHost() && (self->isLocalPlayer || self->adopted)) {
                return;
            }
            self->human->GetInventoryWrapper()->AddWeapon(weaponId, ammo);
        }

    } // namespace

    void Human::OnConstructed() {
        // Story host: a human mirrored from this very game. The ped exists here already; adopt it
        // rather than requesting a second one.
        if (IsStoryHost() && mirrorToken != 0) {
            if (Mirror::WorldMirror::TryAdoptHuman(this)) {
                return;
            }
            // Nothing waits under this token (the mirror was reset in between): stay inert and have
            // the server drop the entity.
            adopted = true;
            Framework::Logging::GetLogger("Mirror")->warn("[Mirror] Human {} carries unknown mirror token {}; asking for its removal", GetNetworkID(), mirrorToken);
            Mirror::WorldMirror::RequestDespawn(GetNetworkID());
            return;
        }

        // An NPC is never this client's avatar, whoever owns it: the server may already have
        // elected us to simulate it when its construction arrives.
        if (!isNpc && IsOwner()) {
            BindLocalPlayer();
        }
        else {
            if (isNpc) {
                Framework::Logging::GetLogger("NPC")->info("[NPC] Human {} streamed in (profile {}, owner {})", GetNetworkID(), modelHash, static_cast<uint64_t>(ownerGUID));
            }
            RequestPed();
        }
    }

    void Human::BindLocalPlayer() {
        isLocalPlayer = true;
        human         = Game::Helpers::Controls::GetLocalPlayer();
        if (human) {
            charController = reinterpret_cast<MafiaMP::Game::Overrides::CharacterController *>(human->GetCharacterController());
        }
        Core::gApplication->SetLocalPlayer(this);
        // Adopt the server-assigned spawn that arrived with construction. The story host stays
        // wherever its campaign put it.
        if (IsStoryHost()) {
            Framework::Logging::GetLogger("Story")->info("[Story] Local avatar {} bound{}; the server spawn is ignored in story-host mode", GetNetworkID(), human ? "" : " (no game ped yet, bound once a chapter runs)");
            return;
        }
        TeleportLocalToReplicated();
    }

    void Human::RebindLocalPlayer() {
        auto *ped = Game::Helpers::Controls::GetLocalPlayer();
        if (ped == human) {
            return;
        }
        human          = ped;
        charController = ped ? reinterpret_cast<MafiaMP::Game::Overrides::CharacterController *>(ped->GetCharacterController()) : nullptr;
        Framework::Logging::GetLogger("Story")->info("[Story] Local avatar {} {} the game's player ped", GetNetworkID(), ped ? "bound to" : "lost");
    }

    void Human::OnStateForced() {
        // A mirrored ped belongs to the host's game: nothing the server forces is applied to it.
        if (adopted) {
            return;
        }

        // SerializeForcedState applied the server's authoritative state. A forced push is made for
        // any server write (health, an NPC order, a teleport), so only a pose the server authored
        // moves the ped; its own pose echoed back must not warp it.
        ApplyForcedHealth();

        if (isLocalPlayer) {
            if (WasPoseForced()) {
                if (IsStoryHost()) {
                    Framework::Logging::GetLogger("Story")->info("[Story] Server teleport of the local avatar ignored in story-host mode");
                    return;
                }
                TeleportLocalToReplicated();
            }
            return;
        }

        if (IsSimulatingNpc()) {
            // The SetOwner RPC that preceded this push is silent; this is the first we hear of it.
            // Without a ped yet there is nothing to switch over; OnHumanRequestFinish catches up.
            if (!_simulating && human) {
                OnSimulationChanged(true);
            }
            if (WasPoseForced() && human) {
                SDK::ue::sys::math::C_Vector newPos    = {position.x, position.y, position.z};
                SDK::ue::sys::math::C_Quat newRot      = {rotation.x, rotation.y, rotation.z, rotation.w};
                SDK::ue::sys::math::C_Matrix transform = {};
                transform.Identity();
                transform.SetRot(newRot);
                transform.SetPos(newPos);
                human->SetTransform(transform);
                Framework::Logging::GetLogger("NPC")->info("[NPC] Human {} teleported by the server to ({:.1f}, {:.1f}, {:.1f})", GetNetworkID(), position.x, position.y, position.z);
            }
        }
    }

    void Human::ApplyForcedHealth() {
        if (adopted || healthRevision == _appliedHealthRevision) {
            return;
        }
        // Not consumed until a ped exists to take it: a health pushed during model load is applied
        // from OnHumanRequestFinish.
        if (!human || !(isLocalPlayer || IsSimulatingNpc())) {
            return;
        }
        _appliedHealthRevision = healthRevision;
        Game::Helpers::Human::SetHealthPercent(human, data._healthPercent);
        Framework::Logging::GetLogger(isNpc ? "NPC" : "Hooks")->info("{}Human {} health set by the server to {:.0f}%", isNpc ? "[NPC] " : "", GetNetworkID(), data._healthPercent);
    }

    void Human::TeleportLocalToReplicated() {
        if (!human) {
            return;
        }
        SDK::ue::sys::math::C_Vector newPos    = {position.x, position.y, position.z};
        SDK::ue::sys::math::C_Quat newRot      = {rotation.x, rotation.y, rotation.z, rotation.w};
        SDK::ue::sys::math::C_Matrix transform = {};
        transform.Identity();
        transform.SetRot(newRot);
        transform.SetPos(newPos);

        // TeleportPlayer preloads the world (collisions) and moves the car the player is in. It needs
        // a direction, which we derive from the rotation.
        glm::mat4 rotMatrix                 = glm::mat4_cast(rotation);
        SDK::ue::sys::math::C_Vector newDir = {-rotMatrix[1][0], rotMatrix[1][1], rotMatrix[1][2]};
        SDK::ue::C_CntPtr<uintptr_t> syncObject;
        SDK::GetPlayerTeleportModule()->TeleportPlayer(syncObject, newPos, newDir, true, true, true, false);

        human->SetTransform(transform);
    }

    void Human::RequestPed() {
        _requestedAt = std::chrono::steady_clock::now();
        info         = Core::gApplication->GetEntityFactory()->RequestHuman(modelHash);
        interpolator.GetPosition()->SetCompensationFactor(1.5f);

        info->SetBeforeSpawnCallback([](Game::Streaming::EntityTrackingInfo *) {
            CreateNetCharacterController = true;
        });
        info->SetRequestFinishCallback(&OnHumanRequestFinish);
        info->SetReturnCallback(&OnHumanReturned);
        info->SetNetworkEntity(this);
    }

    void Human::DeallocReplica(MafiaNet::Connection_RM3 *) {
        if (adopted) {
            // The ped is the game's; only the mirror bookkeeping goes.
            Mirror::WorldMirror::OnEntityGone(this);
        }
        else if (isLocalPlayer) {
            if (Core::gApplication->GetLocalPlayer() == this) {
                Core::gApplication->SetLocalPlayer(nullptr);
            }
        }
        else if (info) {
            if (isNpc) {
                Framework::Logging::GetLogger("NPC")->info("[NPC] Human {} streamed out{}", GetNetworkID(), _simulating ? " (was simulating it)" : "");
            }
            Core::gApplication->GetEntityFactory()->ReturnEntity(info);
            info = nullptr;
        }
        delete this;
    }

    void Human::SerializeFields(Framework::Networking::Replication::FieldSerializer &fields) {
        HumanEntity::SerializeFields(fields);
        // The server withholds this channel from an owner anyway; the guard covers the update that
        // was in flight when we were elected to simulate this NPC.
        if (!fields.Writing() && !isLocalPlayer && !IsSimulatingNpc() && !adopted) {
            ApplyRemote();
        }
    }

    // Structured-exception guard around the capture of a game-owned ped: a mirrored ped is the
    // game's object and can be half-built or mid-destruction in ways no pointer check catches.
    // No C++ objects live in this function (MSVC forbids unwinding across __try).

    // Records where a guarded capture faulted (module + offset) so the SDK call at fault can be
    // found with the PDB; logged once per distinct address.
    static int ReportCaptureFault(EXCEPTION_POINTERS *ep, const char *what) {
        static std::unordered_set<uintptr_t> seen;
        const auto addr = reinterpret_cast<uintptr_t>(ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr);
        if (seen.insert(addr).second) {
            HMODULE mod   = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
                GetModuleFileNameA(mod, name, sizeof(name));
            }
            const char *base = std::strrchr(name, '\\');
            Framework::Logging::GetLogger("Mirror")->warn("[Mirror] {} capture fault at {}+0x{:X} (code 0x{:08X})", what, base ? base + 1 : name, addr - reinterpret_cast<uintptr_t>(mod), ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0u);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    bool Human::CaptureGuarded() {
        __try {
            ReadLocal();
            return true;
        }
        __except (ReportCaptureFault(GetExceptionInformation(), "human")) {
            return false;
        }
    }

    void Human::Frame() {
        if (adopted) {
            // The host only captures what its game does with the ped (ReadLocal reads the pose,
            // health, locomotion, weapon and seat the same way it does for the avatar). The game
            // may have deleted the ped since the last frame: never touch it without checking.
            if (!human || !Mirror::WorldMirror::IsPresent(human)) {
                human          = nullptr;
                charController = nullptr;
                return;
            }
            if (!CaptureGuarded()) {
                auto *ped = reinterpret_cast<SDK::C_Entity *>(human);
                Framework::Logging::GetLogger("Mirror")->warn("[Mirror] Capturing mirrored human {} faulted; the game object is dropped and ignored from now on", GetNetworkID());
                human          = nullptr;
                charController = nullptr;
                Mirror::WorldMirror::Quarantine(ped, "capture faulted");
            }
            return;
        }
        if (isLocalPlayer) {
            if (IsStoryHost()) {
                RebindLocalPlayer();
            }
            ReadLocal();
            return;
        }
        if (!human) {
            if (isNpc && info && !_spawnTimeoutLogged && std::chrono::steady_clock::now() - _requestedAt > kSpawnWarnDelay) {
                _spawnTimeoutLogged = true;
                Framework::Logging::GetLogger("NPC")->warn("[NPC] spawn profile {} still loading after 10 s (human {}); the profile is probably unknown to the game", modelHash, GetNetworkID());
            }
            return;
        }

        // Losing the simulation is silent (ownership arrives through normal replication), so poll.
        const bool simulating = IsSimulatingNpc();
        if (simulating != _simulating) {
            OnSimulationChanged(simulating);
        }

        if (simulating) {
            SimulateNpc();
        }
        else {
            // Drive the remote ped's vehicle entry, then its interpolated transform, then UI.
            if (charController && charController->GetCurrentStateHandlerType() != SDK::ue::game::humanai::C_CharacterStateHandler::E_SHT_CAR) {
                if (enterState == STATE_ENTERING) {
                    auto *car = Vehicle::GetByNetworkId(data.carPassenger.carId);
                    if (car && car->car) {
                        if (Game::Helpers::Human::PutIntoCar(charController, car->car, data.carPassenger.seatId, enterForced)) {
                            enterState  = STATE_INSIDE;
                            enterForced = false;
                        }
                    }
                }
                else if (isNpc && dead) {
                    // A dead NPC stays where it fell; nothing drives its pose any more.
                }
                else if (!SDK::ue::game::humanai::C_CharacterStateHandler::IsVehicleStateHandlerType(charController->GetCurrentStateHandlerType())) {
                    const auto humanPos = human->GetPos();
                    const auto humanRot = human->GetRot();
                    const auto newPos   = interpolator.GetPosition()->UpdateTargetValue({humanPos.x, humanPos.y, humanPos.z});
                    const auto newRot   = interpolator.GetRotation()->UpdateTargetValue({humanRot.w, humanRot.x, humanRot.y, humanRot.z});
                    human->SetPos({newPos.x, newPos.y, newPos.z});
                    human->SetRot({newRot.x, newRot.y, newRot.z, newRot.w});
                }
            }
        }

        // Players always carry a nametag; an NPC only when the script named it.
        if (!isNpc || !nickname.empty()) {
            DrawNametag();
        }
    }

    void Human::OnSimulationChanged(bool simulating) {
        _simulating = simulating;
        Framework::Logging::GetLogger("NPC")->info("[NPC] Human {} simulation {} on this client", GetNetworkID(), simulating ? "gained" : "lost");
        _brain.Reset();
        if (!human) {
            return;
        }
        // Observers keep the ped immune (health comes from the simulator); the simulator's copy is
        // the one that takes the bullets.
        human->GetHumanScript()->SetDemigod(!simulating);
        human->GetHumanScript()->SetInvulnerabilityByScript(!simulating);
        if (!simulating && charController) {
            charController->SetDesiredHandlerType(SDK::ue::game::humanai::C_CharacterStateHandler::E_SHT_NONE);
            charController->SetMoveStateOverride(SDK::E_HumanMoveMode::E_HMM_NONE, false, 0.0f);
            charController->SetAnimMoveSpeed(0.0f);
            if (auto *weapon = human->GetHumanWeaponController()) {
                weapon->SetFirePressedFlag(false);
                weapon->SetAiming(false);
            }
        }
    }

    void Human::OnCarReleased(SDK::C_Car *car) {
        if (adopted || !human || !car || human->GetOwner() != reinterpret_cast<SDK::C_Actor *>(car)) {
            return;
        }
        Framework::Logging::GetLogger("Hooks")->warn("Vehicle under human {} is being released while occupied; forcing it out", GetNetworkID());
        if (charController) {
            Game::Helpers::Human::RemoveFromCar(charController, car, true);
        }
        enterState  = STATE_OUTSIDE;
        enterForced = false;
        if (!isLocalPlayer) {
            data.carPassenger = {};
        }
    }

    void Human::SimulateNpc() {
        if (!human || !charController) {
            return;
        }
        CapturePedState();
        data.carPassenger = {};

        if (dead || human->IsDeath()) {
            // The corpse is the game's now; stop driving the controller and report no fire.
            charController->SetDesiredHandlerType(SDK::ue::game::humanai::C_CharacterStateHandler::E_SHT_NONE);
            charController->SetMoveStateOverride(SDK::E_HumanMoveMode::E_HMM_NONE, false, 0.0f);
            charController->SetAnimMoveSpeed(0.0f);
            data.weaponData.isFiring = false;
            data.weaponData.isAiming = false;
            return;
        }
        _brain.Update(*this);
    }

    void Human::CapturePedState() {
        const SDK::ue::sys::math::C_Vector pedPos = human->GetPos();
        const SDK::ue::sys::math::C_Quat pedRot   = human->GetRot();
        position = {pedPos.x, pedPos.y, pedPos.z};
        rotation = {pedRot.w, pedRot.x, pedRot.y, pedRot.z};

        data._healthPercent        = MafiaMP::Game::Helpers::Human::GetHealthPercent(human);
        data._charStateHandlerType = charController->GetCurrentStateHandlerType(); // null-safe: E_SHT_NONE without a handler
        data._isStalking           = charController->IsStalkMove();
        data._isSprinting          = charController->IsSprinting();
        data._sprintSpeed          = charController->GetSprintMoveSpeed();

        auto weaponController = human->GetHumanWeaponController();
        if (weaponController) {
            SDK::ue::sys::math::C_Vector aimDir;
            weaponController->GetAimDir(&aimDir);
            data.weaponData.aimDir          = {aimDir.x, aimDir.y, aimDir.z};
            data.weaponData.currentWeaponId = weaponController->GetRightHandWeaponID();
            data.weaponData.isAiming        = weaponController->IsAiming();
            data.weaponData.isFiring        = weaponController->m_bFirePressed;
        }

        auto currentHandler = charController->GetCurrentStateHandler();
        if (currentHandler) {
            auto behaviorChar = currentHandler->GetBehaviorCharacter();
            if (behaviorChar) {
                for (size_t i = 0; i < Shared::Modules::WANIM_VAR_SYNC_COUNT; ++i) {
                    data._animVars[i] = behaviorChar->GetWAnimVariable(static_cast<SDK::ue::game::anim::E_WAnimBehaviorVar>(i));
                }
            }
        }

        if (data._charStateHandlerType == SDK::ue::game::humanai::C_CharacterStateHandler::E_SHT_MOVE) {
            SDK::E_HumanMoveMode hmm = charController->GetHumanMoveMode();
            data._moveMode           = hmm != SDK::E_HumanMoveMode::E_HMM_NONE ? static_cast<uint8_t>(hmm) : (uint8_t)-1;
        }
    }

    void Human::ReadLocal() {
        if (!human || !charController) {
            return;
        }

        CapturePedState();

        // A seated ped stops updating its own transform, so GetPos() above is frozen at the point we
        // entered the car. This avatar is the connection's streaming viewer, so follow the car instead
        // — otherwise interest stays anchored at the entry point and the vehicle (and the player) get
        // streamed out after driving one stream radius away.
        if (data._charStateHandlerType == SDK::ue::game::humanai::C_CharacterStateHandler::E_SHT_CAR) {
            if (auto *car = reinterpret_cast<SDK::C_Car *>(human->GetOwner())) {
                const SDK::ue::sys::math::C_Vector carPos = car->GetPos();
                position                                  = {carPos.x, carPos.y, carPos.z};
            }

            auto human2CarWrapper = charController->GetCarHandler()->GetHuman2CarWrapper();
            const int carState    = charController->GetCarHandler()->GetCarState();
            if (human2CarWrapper && carState == 8) /* leaving */ {
                data.carPassenger = {};
            }
            else if (human2CarWrapper && (carState == 2 /* entering */ || data.carPassenger.carId == 0)) {
                // The entering sub-state can be skipped (or the car not yet resolvable during it), which
                // left the seat unreported for the whole ride; keep resolving until it is known.
                auto *car    = (SDK::C_Car *)human->GetOwner();
                auto *carEnt = car ? Vehicle::GetByCar(car) : nullptr;
                if (carEnt) {
                    data.carPassenger = {carEnt->GetNetworkID(), (int)human2CarWrapper->GetSeatID(human)};
                }
            }
        }
        else {
            data.carPassenger = {};
        }
    }

    void Human::ApplyRemote() {
        if (!human || !charController) {
            return;
        }

        // Apply a replicated skin change.
        if (modelHash != appliedSkin) {
            appliedSkin = modelHash;
            // TODO: re-skin a remote ped (local player handled by the game directly).
        }

        // Keep remote peds from dying due to client-only factors; health comes from the owner.
        human->GetHumanScript()->SetDemigod(true);
        human->GetHumanScript()->SetInvulnerabilityByScript(true);

        if (isNpc && dead) {
            // The simulator reported this NPC dead: stop driving its locomotion and weapon.
            charController->SetDesiredHandlerType(kObserverEntersDeathHandler ? SDK::ue::game::humanai::C_CharacterStateHandler::E_SHT_DEATH : SDK::ue::game::humanai::C_CharacterStateHandler::E_SHT_NONE);
            charController->SetMoveStateOverride(SDK::E_HumanMoveMode::E_HMM_NONE, false, 0.0f);
            charController->SetAnimMoveSpeed(0.0f);
            if (auto *weapon = human->GetHumanWeaponController()) {
                weapon->SetFirePressedFlag(false);
                weapon->SetAiming(false);
            }
            return;
        }

        MafiaMP::Game::Helpers::Human::SetHealthPercent(human, data._healthPercent);

        const auto desiredStateHandlerType = static_cast<SDK::ue::game::humanai::C_CharacterStateHandler::E_State_Handler_Type>(data._charStateHandlerType);

        // Leave the car if we are no longer a passenger.
        if (data.carPassenger.carId == 0 && enterState == STATE_INSIDE) {
            enterState = STATE_LEAVING;
            if (Game::Helpers::Human::RemoveFromCar(charController, (SDK::C_Car *)human->GetOwner(), false)) {
                enterState = STATE_OUTSIDE;
            }
        }

        if (SDK::ue::game::humanai::C_CharacterStateHandler::IsVehicleStateHandlerType(desiredStateHandlerType)) {
            charController->SetDesiredHandlerType(SDK::ue::game::humanai::C_CharacterStateHandler::E_SHT_NONE);
            if (desiredStateHandlerType == SDK::ue::game::humanai::C_CharacterStateHandler::E_SHT_CAR) {
                if (charController->GetCurrentStateHandlerType() != SDK::ue::game::humanai::C_CharacterStateHandler::E_SHT_CAR) {
                    if (data.carPassenger.carId > 0 && enterState == STATE_OUTSIDE) {
                        enterState = STATE_ENTERING;
                    }
                }
            }
            return;
        }

        // Set the interpolation target toward the replicated transform.
        interpolator.GetPosition()->SetTargetValue({human->GetPos().x, human->GetPos().y, human->GetPos().z}, position, MafiaMP::Core::gApplication->GetTickInterval());
        interpolator.GetRotation()->SetTargetValue({human->GetRot().w, human->GetRot().x, human->GetRot().y, human->GetRot().z}, rotation, MafiaMP::Core::gApplication->GetTickInterval());

        charController->SetDesiredHandlerType(desiredStateHandlerType);
        charController->SetStalkMoveOverride(data._isStalking);
        const auto hmm = data._moveMode != (uint8_t)-1 ? static_cast<SDK::E_HumanMoveMode>(data._moveMode) : SDK::E_HumanMoveMode::E_HMM_NONE;
        charController->SetMoveStateOverride(hmm, data._isSprinting, data._sprintSpeed);

        auto currentHandler = charController->GetCurrentStateHandler();
        if (currentHandler) {
            auto behaviorChar = currentHandler->GetBehaviorCharacter();
            if (behaviorChar) {
                for (size_t i = 0; i < Shared::Modules::WANIM_VAR_SYNC_COUNT; ++i) {
                    behaviorChar->SetWAnimVariable(static_cast<SDK::ue::game::anim::E_WAnimBehaviorVar>(i), data._animVars[i]);
                }
            }
        }

        const auto wepController = human->GetHumanWeaponController();
        if (wepController) {
            if (wepController->GetRightHandWeaponID() != data.weaponData.currentWeaponId) {
                wepController->DoWeaponSelectByItemId(data.weaponData.currentWeaponId, true);
            }
            SDK::ue::C_CntPtr<uintptr_t> syncObject2;
            human->GetHumanScript()->ScrAim(syncObject2, data.weaponData.isAiming);
            wepController->SetAiming(data.weaponData.isAiming);
            wepController->SetFirePressedFlag(data.weaponData.isFiring);
        }
    }

    void Human::DrawNametag() {
        // Capture only the stable network id, not the ped pointer: the widget runs deferred (next
        // ImGUI flush) and the human can stream out and free its ped in between, leaving a captured
        // raw pointer dangling. Re-resolve the live entity here and bail if it is gone.
        const auto networkId = GetNetworkID();
        gApplication->GetImGUI()->PushWidget([networkId]() {
            auto *self = ResolveHuman(networkId);
            if (!self || !self->human) {
                return;
            }

            const auto displaySize = ImGui::GetIO().DisplaySize;

            auto gameCamera = SDK::ue::game::camera::C_GameCamera::GetInstanceInternal();
            if (!gameCamera) {
                return;
            }
            auto camera = gameCamera->GetCamera(SDK::ue::game::camera::E_GameCameraID::CAMERA_PLAYER_MAIN);
            if (!camera) {
                return;
            }

            auto camPos                          = camera->GetPos();
            static const auto headBoneHash       = SDK::ue::sys::utils::C_HashName::ComputeHash("Head");
            SDK::ue::sys::math::C_Vector headPos = self->human->GetBoneWorldPos(headBoneHash);
            float distFromCam                    = headPos.dist(camPos);
            if (distFromCam <= 250.0f) {
                headPos.z += 0.335f + (distFromCam * 0.03f);

                SDK::ue::sys::math::C_Vector2 screenPos;
                bool onScreen = false;
                float unkFloat1, unkFloat2;
                camera->GetScreenPos(screenPos, headPos, onScreen, &unkFloat1, &unkFloat2, true);
                if (onScreen) {
                    // An NPC carries no player index; it is drawn under the name the script gave it.
                    const auto playerName = self->isNpc ? self->nickname : fmt::format("{} ({})", self->nickname.empty() ? "Player" : self->nickname, self->playerIndex);
                    Framework::External::ImGUI::Widgets::DrawNameTag(ImGui::GetBackgroundDrawList(), ImVec2(screenPos.x * displaySize.x, screenPos.y * displaySize.y), playerName.c_str(), {}, 1.0f, self->data._healthPercent);
                }
            }
        });
    }

    void Human::Install() {
        // Register the client human type for this id (overrides the server-side plain HumanEntity).
        Framework::Networking::Replication::EntityRegistry::Get().Register(Shared::Entities::HumanEntity::kTypeName, [] {
            return new Human();
        });

        auto *rpc = Framework::CoreModules::GetNetworkPeer()->GetRPC();
        // Human action RPCs received from peers.
        rpc->RegisterSlot(Shared::RPC::kHumanShoot, &OnHumanShoot, nullptr, 0);
        rpc->RegisterSlot(Shared::RPC::kHumanReload, &OnHumanReload, nullptr, 0);
        rpc->RegisterSlot(Shared::RPC::kHumanAddWeapon, &OnHumanAddWeapon, nullptr, 0);
    }

    void Human::UpdateAll() {
        auto *world = Framework::CoreModules::GetReplication();
        auto *repl  = world;
        if (!repl) {
            return;
        }
        repl->ForEachEntity([](Framework::Networking::Replication::NetworkEntity *e) {
            if (auto *human = dynamic_cast<Human *>(e)) {
                human->Frame();
            }
        });
    }

    Human *Human::GetByPed(SDK::C_Human2 *ptr) {
        auto *world = Framework::CoreModules::GetReplication();
        auto *repl  = world;
        if (!repl || !ptr) {
            return nullptr;
        }
        Human *found = nullptr;
        repl->ForEachEntity([&](Framework::Networking::Replication::NetworkEntity *e) {
            if (found) {
                return;
            }
            if (auto *human = dynamic_cast<Human *>(e); human && human->human == ptr) {
                found = human;
            }
        });
        return found;
    }
} // namespace MafiaMP::Core::Modules
