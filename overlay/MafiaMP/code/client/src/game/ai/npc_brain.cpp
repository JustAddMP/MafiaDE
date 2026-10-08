#include <utils/safe_win32.h>

#include "npc_brain.h"

#include "core/modules/human.h"

#include "game/overrides/character_controller.h"
#include "sdk/entities/c_human_2.h"
#include "sdk/entities/human/c_human_script.h"
#include "sdk/entities/human/c_human_weapon_controller.h"
#include "sdk/ue/c_cnt_ptr.h"
#include "sdk/ue/game/humanai/c_character_state_handler.h"

#include <core_modules.h>
#include <logging/logger.h>
#include <networking/replication/replication_manager.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace MafiaMP::Game::AI {
    namespace {
        using Shared::Entities::NpcCommand;
        using Shared::Entities::NpcMoveMode;
        using StateHandler = SDK::ue::game::humanai::C_CharacterStateHandler;

        glm::vec3 ToGlm(const SDK::ue::sys::math::C_Vector &v) {
            return {v.x, v.y, v.z};
        }

        SDK::ue::sys::math::C_Vector ToSdk(const glm::vec3 &v) {
            return {v.x, v.y, v.z};
        }

        // Where an order's target entity is right now: its game ped when streamed in here, else the
        // replicated pose. Null when the target is gone.
        bool ResolveTargetPosition(uint64_t networkId, glm::vec3 &out) {
            auto *repl = Framework::CoreModules::GetReplication();
            if (!repl || networkId == 0) {
                return false;
            }
            auto *entity = repl->GetEntityByNetworkID(networkId);
            if (!entity) {
                return false;
            }
            if (auto *human = dynamic_cast<Core::Modules::Human *>(entity); human && human->human) {
                out = ToGlm(human->human->GetPos());
                return true;
            }
            out = entity->position;
            return true;
        }

        const char *CommandName(NpcCommand command) {
            switch (command) {
            case NpcCommand::GoTo: return "goTo";
            case NpcCommand::Attack: return "attack";
            case NpcCommand::Follow: return "follow";
            case NpcCommand::Flee: return "flee";
            default: return "none";
            }
        }
    } // namespace

    void NpcBrain::Reset() {
        _hasLastTick   = false;
        _moving        = false;
        _attacking     = false;
        _firing        = false;
        _fireTimer     = 0.0f;
        _aimAtTimer    = 0.0f;
        _loggedCommand = -1;
    }

    float NpcBrain::TickDelta() {
        const auto now = std::chrono::steady_clock::now();
        float dt       = 0.0f;
        if (_hasLastTick) {
            dt = std::chrono::duration<float>(now - _lastTick).count();
        }
        _lastTick    = now;
        _hasLastTick = true;
        // A hitch must not teleport the ped across the map.
        return std::clamp(dt, 0.0f, 0.1f);
    }

    void NpcBrain::Stand(Core::Modules::Human &npc) {
        npc.charController->SetDesiredHandlerType(StateHandler::E_SHT_STAND);
        npc.charController->SetMoveStateOverride(SDK::E_HumanMoveMode::E_HMM_NONE, false, 0.0f);
        npc.charController->SetAnimMoveSpeed(0.0f);
        _moving = false;
    }

    void NpcBrain::StopAttacking(Core::Modules::Human &npc) {
        if (!_attacking) {
            return;
        }
        _attacking = false;
        _firing    = false;
        _fireTimer = 0.0f;
        if (auto *weapon = npc.human->GetHumanWeaponController()) {
            weapon->SetFirePressedFlag(false);
            weapon->SetAiming(false);
        }
        SDK::ue::C_CntPtr<uintptr_t> syncObject;
        npc.human->GetHumanScript()->ScrAim(syncObject, false);
    }

    void NpcBrain::Update(Core::Modules::Human &npc) {
        const float dt       = TickDelta();
        const auto command   = npc.GetCommand();
        const bool run       = npc.aiMoveMode == static_cast<uint8_t>(NpcMoveMode::Run);
        const glm::vec3 pos  = ToGlm(npc.human->GetPos());

        if (_loggedCommand != static_cast<int>(npc.aiCommand)) {
            _loggedCommand = static_cast<int>(npc.aiCommand);
            Framework::Logging::GetLogger("NPC")->info("[NPC] Human {} running order {} (target {}, pos ({:.1f}, {:.1f}, {:.1f}))", npc.GetNetworkID(), CommandName(command), npc.aiTargetId, npc.aiTargetPos.x, npc.aiTargetPos.y, npc.aiTargetPos.z);
            // A fresh order restarts the approach hysteresis.
            _moving = false;
        }

        // --- Resolve where this order points ---
        glm::vec3 target = npc.aiTargetPos;
        bool haveTarget  = false;
        switch (command) {
        case NpcCommand::GoTo:
        case NpcCommand::Flee: haveTarget = true; break;
        case NpcCommand::Attack:
        case NpcCommand::Follow: haveTarget = ResolveTargetPosition(npc.aiTargetId, target); break;
        default: break;
        }

        if (command != NpcCommand::Attack || !haveTarget) {
            StopAttacking(npc);
        }
        if (!haveTarget) {
            Stand(npc);
            return;
        }

        glm::vec3 toTarget = target - pos;
        toTarget.z         = 0.0f;
        const float dist   = glm::length(toTarget);
        const glm::vec3 dir = dist > 0.001f ? toTarget / dist : glm::vec3(1.0f, 0.0f, 0.0f);

        // --- Decide whether to move, and which way ---
        bool move          = false;
        glm::vec3 moveDir  = dir;
        float stopDistance = 0.0f;
        switch (command) {
        case NpcCommand::GoTo:
            stopDistance = std::max(0.5f, npc.aiStopRadius);
            move         = dist > stopDistance;
            break;
        case NpcCommand::Follow:
            stopDistance = kFollowStop;
            move         = _moving ? dist > kFollowStop : dist > kFollowResume;
            break;
        case NpcCommand::Attack:
            stopDistance = kAttackStop;
            move         = _moving ? dist > kAttackStop : dist > kAttackResume;
            break;
        case NpcCommand::Flee:
            moveDir = -dir;
            move    = dist < kFleeDistance;
            break;
        default: break;
        }

        if (move) {
            const float speed = run ? kRunSpeed : kWalkSpeed;
            float step        = speed * dt;
            if (command != NpcCommand::Flee) {
                // Never overshoot the stop distance; the ped settles exactly on it.
                step = std::min(step, std::max(0.0f, dist - stopDistance));
            }
            glm::vec3 next = pos + moveDir * step;
            // No ground probe exists: keep our own height unless the target is at a plausible step
            // or slope away, then ease towards its height.
            const float dz = target.z - pos.z;
            if (command != NpcCommand::Flee && std::fabs(dz) < 3.0f && dist > 0.001f) {
                next.z = pos.z + dz * std::min(1.0f, step / dist);
            }

            npc.human->SetDir(ToSdk(glm::vec3(moveDir.x, moveDir.y, 0.0f)));
            npc.charController->SetDesiredHandlerType(StateHandler::E_SHT_MOVE);
            npc.charController->SetMoveStateOverride(run ? SDK::E_HumanMoveMode::E_HMM_RUN : SDK::E_HumanMoveMode::E_HMM_WALK, false, 0.0f);
            npc.charController->SetAnimMoveSpeed(speed);
            npc.human->SetPos(ToSdk(next));
            _moving = true;
        }
        else {
            Stand(npc);
            if (command == NpcCommand::Attack || command == NpcCommand::Follow) {
                npc.human->SetDir(ToSdk(glm::vec3(dir.x, dir.y, 0.0f)));
            }
        }

        // --- Attack: weapon, aim, bursts ---
        if (command != NpcCommand::Attack) {
            return;
        }
        auto *weapon = npc.human->GetHumanWeaponController();
        if (!weapon) {
            return;
        }
        if (!_attacking) {
            _attacking  = true;
            _firing     = false;
            _fireTimer  = 0.0f;
            _aimAtTimer = 0.0f;
        }
        if (npc.lastWeaponAdded > 0 && weapon->GetRightHandWeaponID() != npc.lastWeaponAdded) {
            weapon->DoWeaponSelectByItemId(static_cast<unsigned int>(npc.lastWeaponAdded), true);
        }

        // Aim at the target's chest from our own; the fields are what observers replay.
        const glm::vec3 aimPos = target + glm::vec3(0.0f, 0.0f, 1.2f);
        const glm::vec3 muzzle = pos + glm::vec3(0.0f, 0.0f, 1.4f);
        const glm::vec3 aimVec = aimPos - muzzle;
        const float aimLen     = glm::length(aimVec);
        npc.data.weaponData.aimPos = aimPos;
        npc.data.weaponData.aimDir = aimLen > 0.001f ? aimVec / aimLen : glm::vec3(dir.x, dir.y, 0.0f);

        _aimAtTimer -= dt;
        if (_aimAtTimer <= 0.0f) {
            _aimAtTimer = 0.25f;
            SDK::ue::C_CntPtr<uintptr_t> syncObject;
            npc.human->GetHumanScript()->ScrAimAt(syncObject, nullptr, ToSdk(aimPos), true);
        }
        SDK::ue::C_CntPtr<uintptr_t> aimSync;
        npc.human->GetHumanScript()->ScrAim(aimSync, true);
        weapon->SetAiming(true);

        if (dist <= kAttackRange && !move) {
            _fireTimer -= dt;
            if (_fireTimer <= 0.0f) {
                _firing    = !_firing;
                _fireTimer = _firing ? kFireOn : kFireOff;
            }
        }
        else {
            _firing    = false;
            _fireTimer = 0.0f;
        }
        weapon->SetFirePressedFlag(_firing);
    }
} // namespace MafiaMP::Game::AI
