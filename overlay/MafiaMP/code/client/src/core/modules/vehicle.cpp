#include <utils/safe_win32.h>

#include <cstring>
#include <unordered_set>

#include "vehicle.h"

#include "core/application.h"
#include "core/mirror/world_mirror.h"
#include "core/story_host.h"

#include "human.h"

#include "sdk/ue/game/vehicle/c_vehicle.h"
#include "sdk/mafia/framework/c_mafia_framework.h"
#include "sdk/mafia/framework/c_mafia_framework_interfaces.h"

#include <core_modules.h>
#include <networking/replication/entity_registry.h>
#include <networking/replication/replication_manager.h>
#include <networking/replication/replication_manager.h>

#include <cstring>
#include <utility>

namespace MafiaMP::Core::Modules {
    namespace {
        // Fires when the streamed game car is ready; binds it to its ClientVehicle.
        void OnVehicleRequestFinish(Game::Streaming::EntityTrackingInfo *info, bool success) {
            if (!success) {
                return;
            }
            auto car = reinterpret_cast<SDK::C_Car *>(info->GetEntity());
            if (!car) {
                return;
            }
            car->GameInit();
            car->Activate();
            car->Unlock();

            auto *self = static_cast<Vehicle *>(info->GetNetworkEntity());
            if (!self) {
                return;
            }

            SDK::ue::sys::math::C_Quat newRot      = {self->rotation.x, self->rotation.y, self->rotation.z, self->rotation.w};
            SDK::ue::sys::math::C_Vector newPos    = {self->position.x, self->position.y, self->position.z};
            SDK::ue::sys::math::C_Matrix transform = {};
            transform.Identity();
            transform.SetRot(newRot);
            transform.SetPos(newPos);
            car->GetVehicle()->SetVehicleMatrix(transform, SDK::ue::sys::core::E_TransformChangeType::DEFAULT);
            car->GetVehicle()->SetBrake(self->data.brake, true);

            self->car = car;
            // The replicated state arrived before the game car existed; apply it now. With per-field
            // deltas, unchanged fields are never re-sent, so a late ApplyRemote is the only chance to
            // catch up the car's configuration.
            if (self->IsOwner()) {
                self->Frame();
            }
            else {
                self->ApplyRemote();
            }
        }

        void OnVehicleReturned(Game::Streaming::EntityTrackingInfo *info, bool wasCreated) {
            if (!info) {
                return;
            }
            auto car = reinterpret_cast<SDK::C_Car *>(info->GetEntity());
            if (wasCreated && car) {
                car->Deactivate();
                car->GameDone();
                car->Release();
            }
        }
    } // namespace

    void Vehicle::OnConstructed() {
        // Story host: a car mirrored from this very game. The car exists here already; adopt it
        // rather than requesting a second one.
        if (IsStoryHost() && mirrorToken != 0) {
            if (Mirror::WorldMirror::TryAdoptVehicle(this)) {
                return;
            }
            adopted = true;
            Framework::Logging::GetLogger("Mirror")->warn("[Mirror] Vehicle {} carries unknown mirror token {}; asking for its removal", GetNetworkID(), mirrorToken);
            Mirror::WorldMirror::RequestDespawn(GetNetworkID());
            return;
        }

        info = Core::gApplication->GetEntityFactory()->RequestVehicle(modelName);
        interpolator.GetPosition()->SetCompensationFactor(1.5f);
        info->SetRequestFinishCallback(&OnVehicleRequestFinish);
        info->SetReturnCallback(&OnVehicleReturned);
        info->SetNetworkEntity(this);
    }

    void Vehicle::DeallocReplica(MafiaNet::Connection_RM3 *) {
        if (adopted) {
            // The car is the game's; only the mirror bookkeeping goes.
            Mirror::WorldMirror::OnEntityGone(this);
            delete this;
            return;
        }
        if (info) {
            // The server refuses to destroy an occupied car; should one still arrive, nobody may keep
            // a pointer to the car about to be returned.
            if (car) {
                if (auto *repl = Framework::CoreModules::GetReplication()) {
                    repl->ForEachEntity([&](Framework::Networking::Replication::NetworkEntity *e) {
                        if (auto *human = dynamic_cast<Human *>(e)) {
                            human->OnCarReleased(car);
                        }
                    });
                }
            }
            Core::gApplication->GetEntityFactory()->ReturnEntity(info);
            info = nullptr;
        }
        delete this;
    }

    void Vehicle::SerializeFields(Framework::Networking::Replication::FieldSerializer &fields) {
        VehicleEntity::SerializeFields(fields);
        if (!fields.Writing() && !IsOwner()) {
            ApplyRemote();
        }
    }


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

    bool Vehicle::CaptureGuarded() {
        __try {
            ReadLocal();
            return true;
        }
        __except (ReportCaptureFault(GetExceptionInformation(), "car")) {
            return false;
        }
    }

    void Vehicle::Frame() {
        if (!car) {
            return;
        }
        if (adopted) {
            if (!Mirror::WorldMirror::IsPresent(car)) {
                car = nullptr; // the game deleted its car; the mirror releases the entity
                return;
            }
            if (!CaptureGuarded()) {
                auto *game = reinterpret_cast<SDK::C_Entity *>(car);
                Framework::Logging::GetLogger("Mirror")->warn("[Mirror] Capturing mirrored car {} faulted; the game object is dropped and ignored from now on", GetNetworkID());
                car = nullptr;
                Mirror::WorldMirror::Quarantine(game, "capture faulted");
            }
            return;
        }
        if (IsOwner()) {
            ReadLocal();
        }
        else {
            const auto vehiclePos = car->GetPos();
            const auto vehicleRot = car->GetRot();
            const auto newPos     = interpolator.GetPosition()->UpdateTargetValue({vehiclePos.x, vehiclePos.y, vehiclePos.z});
            const auto newRot     = interpolator.GetRotation()->UpdateTargetValue({vehicleRot.w, vehicleRot.x, vehicleRot.y, vehicleRot.z});

            SDK::ue::sys::math::C_Matrix transform = {};
            transform.Identity();
            transform.SetRot({newRot.x, newRot.y, newRot.z, newRot.w});
            transform.SetPos({newPos.x, newPos.y, newPos.z});
            car->GetVehicle()->SetVehicleMatrix(transform, SDK::ue::sys::core::E_TransformChangeType::DEFAULT);
        }
    }

    void Vehicle::ReadLocal() {
        SDK::ue::sys::math::C_Vector carPos = ((SDK::C_Actor *)car)->GetPos();
        SDK::ue::sys::math::C_Quat carRot   = ((SDK::C_Actor *)car)->GetRot();
        SDK::ue::game::vehicle::C_Vehicle *vehicle = car->GetVehicle();

        SDK::ue::sys::math::C_Vector vehicleVelocity        = vehicle->GetSpeed();
        SDK::ue::sys::math::C_Vector vehicleAngularVelocity = vehicle->GetAngularSpeed();

        SDK::ue::sys::math::C_Vector4 colorPrimary, colorSecondary;
        vehicle->GetVehicleColor(&colorPrimary, &colorSecondary);

        SDK::ue::sys::math::C_Vector4 rimColor, tireColor;
        vehicle->GetWheelColor(&rimColor, &tireColor);

        SDK::ue::sys::math::C_Vector4 windowTint = vehicle->GetWindowTintColor();

        position = {carPos.x, carPos.y, carPos.z};
        rotation = {carRot.w, carRot.x, carRot.y, carRot.z};

        data.angularVelocity = {vehicleAngularVelocity.x, vehicleAngularVelocity.y, vehicleAngularVelocity.z};
        data.beaconLightsOn  = vehicle->GetBeaconLightsOn();
        data.brake           = vehicle->GetBrake();
        data.colorPrimary    = {colorPrimary.r, colorPrimary.g, colorPrimary.b, colorPrimary.a};
        data.colorSecondary  = {colorSecondary.r, colorSecondary.g, colorSecondary.b, colorSecondary.a};
        data.dirt            = vehicle->GetVehicleDirty();
        data.engineOn        = car->IsEngineOn();
        data.fuel            = car->GetActualFuel();
        data.gear            = car->GetGear();
        data.handbrake       = vehicle->GetHandbrake();
        data.hornOn          = vehicle->GetHorn();
        data.power           = vehicle->GetPower();
        if (!adopted) {
            // Ambient (game-spawned) cars carry a stale m_pRadioSound pointer that is not a radio object at
            // all; only cars the mod created or the player drives have a valid one. Mirrored cars keep
            // the radio off on guests.
            data.radioOn        = vehicle->IsRadioOn();
            data.radioStationId = vehicle->GetRadioStation();
        }
        else {
            data.radioOn = false;
        }
        data.rimColor        = {rimColor.r, rimColor.g, rimColor.b, rimColor.a};
        data.rust            = vehicle->GetVehicleRust();
        data.sirenOn         = vehicle->IsSiren();
        data.steer           = vehicle->GetSteer();
        data.tireColor       = {tireColor.r, tireColor.g, tireColor.b, tireColor.a};
        data.velocity        = {vehicleVelocity.x, vehicleVelocity.y, vehicleVelocity.z};
        data.windowTint      = {windowTint.r, windowTint.g, windowTint.b, windowTint.a};

        const char *licensePlate = vehicle->GetSPZText();
        std::strncpy(data.licensePlate.data(), licensePlate, data.licensePlate.size() - 1);
        data.licensePlate[data.licensePlate.size() - 1] = '\0';
    }

    void Vehicle::ApplyConfig() {
        if (!car) {
            return;
        }
        SDK::ue::game::vehicle::C_Vehicle *vehicle = car->GetVehicle();

        SDK::ue::sys::math::C_Vector4 colorPrimary   = {data.colorPrimary.r, data.colorPrimary.g, data.colorPrimary.b, data.colorPrimary.a};
        SDK::ue::sys::math::C_Vector4 colorSecondary = {data.colorSecondary.r, data.colorSecondary.g, data.colorSecondary.b, data.colorSecondary.a};
        SDK::ue::sys::math::C_Vector4 rimColor       = {data.rimColor.r, data.rimColor.g, data.rimColor.b, data.rimColor.a};
        SDK::ue::sys::math::C_Vector4 tireColor      = {data.tireColor.r, data.tireColor.g, data.tireColor.b, data.tireColor.a};
        SDK::ue::sys::math::C_Vector4 windowTint     = {data.windowTint.r, data.windowTint.g, data.windowTint.b, data.windowTint.a};

        vehicle->SetBeaconLightsOn(data.beaconLightsOn);
        vehicle->SetVehicleColor(&colorPrimary, &colorSecondary, false);
        car->SetVehicleDirty(data.dirt); // must go through the car or the value resets
        vehicle->SetEngineOn(data.engineOn, data.engineOn);
        car->SetActualFuel(data.fuel);
        if (std::strcmp(vehicle->GetSPZText(), data.licensePlate.data()) != 0) {
            vehicle->SetSPZText(data.licensePlate.data(), true);
        }
        if (vehicle->IsRadioOn() != data.radioOn) {
            vehicle->TurnRadioOn(data.radioOn);
        }
        if (vehicle->GetRadioStation() != data.radioStationId) {
            vehicle->ChangeRadioStation(data.radioStationId);
        }
        vehicle->SetSiren(data.sirenOn);
        vehicle->SetVehicleRust(data.rust);
        vehicle->SetWheelColor(&rimColor, &tireColor);
        vehicle->SetWindowTintColor(windowTint);
    }

    void Vehicle::ApplyRemote() {
        if (!car) {
            return;
        }

        // Set the interpolation target toward the replicated transform.
        const auto vehicleRot = car->GetRot();
        const auto vehiclePos = car->GetPos();
        interpolator.GetRotation()->SetTargetValue({vehicleRot.w, vehicleRot.x, vehicleRot.y, vehicleRot.z}, rotation, MafiaMP::Core::gApplication->GetTickInterval());
        interpolator.GetPosition()->SetTargetValue({vehiclePos.x, vehiclePos.y, vehiclePos.z}, position, MafiaMP::Core::gApplication->GetTickInterval());

        ApplyConfig();

        // Owner-authoritative physics, replicated to the other clients.
        SDK::ue::game::vehicle::C_Vehicle *vehicle = car->GetVehicle();
        vehicle->SetAngularSpeed({data.angularVelocity.x, data.angularVelocity.y, data.angularVelocity.z}, false);
        vehicle->SetBrake(data.brake, false);
        vehicle->SetGear(data.gear);
        vehicle->SetHandbrake(data.handbrake, false);
        vehicle->SetHorn(data.hornOn);
        vehicle->SetPower(data.power);
        vehicle->SetSpeed({data.velocity.x, data.velocity.y, data.velocity.z}, false, false);
        vehicle->SetSteer(data.steer);
    }

    void Vehicle::OnStateForced() {
        // A mirrored car belongs to the host's game: the server gets no say over it.
        if (adopted) {
            return;
        }
        // We own this vehicle, so the server withholds normal serialize to us; this is how it gets
        // the last word over our configuration. Apply it to the car — ReadLocal then streams the
        // corrected values back, so there is no fight with the server.
        ApplyConfig();
    }

    void Vehicle::Install() {
        Framework::Networking::Replication::EntityRegistry::Get().Register(Shared::Entities::VehicleEntity::kTypeName, [] {
            return new Vehicle();
        });
    }

    void Vehicle::UpdateAll() {
        auto *world = Framework::CoreModules::GetReplication();
        auto *repl  = world;
        if (!repl) {
            return;
        }
        repl->ForEachEntity([](Framework::Networking::Replication::NetworkEntity *e) {
            if (auto *vehicle = dynamic_cast<Vehicle *>(e)) {
                vehicle->Frame();
            }
        });
    }

    Vehicle *Vehicle::GetByCar(SDK::C_Car *carPtr) {
        auto *world = Framework::CoreModules::GetReplication();
        auto *repl  = world;
        if (!repl || !carPtr) {
            return nullptr;
        }
        Vehicle *found = nullptr;
        repl->ForEachEntity([&](Framework::Networking::Replication::NetworkEntity *e) {
            if (found) {
                return;
            }
            if (auto *vehicle = dynamic_cast<Vehicle *>(e); vehicle && vehicle->car == carPtr) {
                found = vehicle;
            }
        });
        return found;
    }

    Vehicle *Vehicle::GetByVehicle(SDK::ue::game::vehicle::C_Vehicle *vehiclePtr) {
        auto *world = Framework::CoreModules::GetReplication();
        auto *repl  = world;
        if (!repl || !vehiclePtr) {
            return nullptr;
        }
        Vehicle *found = nullptr;
        repl->ForEachEntity([&](Framework::Networking::Replication::NetworkEntity *e) {
            if (found) {
                return;
            }
            if (auto *vehicle = dynamic_cast<Vehicle *>(e); vehicle && vehicle->car && vehicle->car->GetVehicle() == vehiclePtr) {
                found = vehicle;
            }
        });
        return found;
    }

    Vehicle *Vehicle::GetByNetworkId(uint64_t networkId) {
        auto *world = Framework::CoreModules::GetReplication();
        return world ? dynamic_cast<Vehicle *>(world->GetEntityByNetworkID(networkId)) : nullptr;
    }
} // namespace MafiaMP::Core::Modules
