#include "human.h"

#include "events_reserved.h"
#include "player.h"
#include "vehicle.h"

#include "core/server.h"

#include "shared/rpc/ids.h"

#include <core_modules.h>
#include <integrations/server/scripting/module.h>
#include <logging/logger.h>
#include <networking/network_peer.h>
#include <networking/replication/replication_manager.h>
#include <scripting/node_engine.h>

#include <mafianet/BitStream.h>

#include <algorithm>
#include <cctype>

namespace MafiaMP::Scripting {

namespace {
    using Shared::Entities::NpcCommand;
    using Shared::Entities::NpcMoveMode;

    template <typename ArgsBuilder>
    void EmitHumanEvent(const std::string &eventName, ArgsBuilder &&buildArgs) {
        auto server = MafiaMP::Server::_serverRef;
        if (!server)
            return;

        auto scriptingModule = server->GetScriptingModule();
        if (!scriptingModule)
            return;

        auto *engine          = scriptingModule->GetEngine();
        auto *resourceManager = scriptingModule->GetResourceManager();
        if (!engine || !resourceManager || !engine->IsInitialized())
            return;

        v8::Isolate *isolate = engine->GetIsolate();
        v8::Locker locker(isolate);
        v8::Isolate::Scope isolateScope(isolate);
        v8::HandleScope handleScope(isolate);
        v8::Local<v8::Context> context = engine->GetContext();
        v8::Context::Scope contextScope(context);

        std::vector<v8::Local<v8::Value>> args = buildArgs(isolate);
        resourceManager->GetEvents().EmitReserved(isolate, context, eventName, args);
    }

    // A human handle of the right flavour: Player for a connection's avatar, Human for an NPC.
    v8::Local<v8::Value> WrapHuman(v8::Isolate *isolate, uint64_t networkId) {
        auto *repl   = Framework::CoreModules::GetReplication();
        auto *entity = repl ? dynamic_cast<Shared::Entities::HumanEntity *>(repl->GetEntityByNetworkID(networkId)) : nullptr;
        if (!entity) {
            return v8::Undefined(isolate);
        }
        if (entity->streaming.isViewer) {
            return v8pp::class_<Player>::create_object(isolate, networkId);
        }
        return v8pp::class_<Human>::create_object(isolate, networkId);
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

Human::Human(uint64_t networkId): Framework::Scripting::Builtins::Player(networkId) {
    if (!ResolveHuman()) {
        throw std::runtime_error("Entity handle is not a Human!");
    }
}

Shared::Entities::HumanEntity *Human::ResolveHuman() const {
    return dynamic_cast<Shared::Entities::HumanEntity *>(Resolve());
}

void Human::EventHumanDied(uint64_t networkId, uint64_t killerId, int damageType) {
    Framework::Logging::GetLogger("Scripting")->info("[NPC] Human {} died (killer {}, damage type {})", networkId, killerId, damageType);
    EmitHumanEvent(Events::kHumanDied, [&](v8::Isolate *isolate) {
        std::vector<v8::Local<v8::Value>> args;
        args.push_back(v8pp::class_<Human>::create_object(isolate, networkId));
        args.push_back(killerId != 0 ? WrapHuman(isolate, killerId) : v8::Local<v8::Value>(v8::Undefined(isolate)));
        args.push_back(v8::Integer::New(isolate, damageType));
        return args;
    });
}

void Human::EventHumanDestroyed(uint64_t networkId) {
    EmitHumanEvent(Events::kHumanDestroyed, [&](v8::Isolate *isolate) {
        std::vector<v8::Local<v8::Value>> args;
        args.push_back(v8pp::class_<Human>::create_object(isolate, networkId));
        return args;
    });
}

std::string Human::ToString() const {
    std::ostringstream ss;
    ss << "Human{ id: " << _id << " }";
    return ss.str();
}

bool Human::IsAiming() const {
    auto *h = ResolveHuman();
    return h && h->data.weaponData.isAiming;
}

bool Human::IsFiring() const {
    auto *h = ResolveHuman();
    return h && h->data.weaponData.isFiring;
}

void Human::AddWeapon(int weaponId, int ammo) {
    auto *net = Framework::CoreModules::GetNetworkPeer();
    if (!net) {
        return;
    }
    // Wire: <human NetworkID><weaponId><ammo>
    MafiaNet::BitStream bs;
    uint64_t id = _id;
    bs.Write(id);
    bs.Write(weaponId);
    bs.Write(ammo);
    net->GetRPC()->Signal(Shared::RPC::kHumanAddWeapon, &bs, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, MafiaNet::UNASSIGNED_RAKNET_GUID, true, false);
}

Framework::Scripting::Builtins::Vector3 Human::GetAimDir() const {
    auto *h = ResolveHuman();
    if (!h) return Framework::Scripting::Builtins::Vector3(0, 0, 0);
    const auto dir = h->data.weaponData.aimDir;
    return Framework::Scripting::Builtins::Vector3(dir.x, dir.y, dir.z);
}

Framework::Scripting::Builtins::Vector3 Human::GetAimPos() const {
    auto *h = ResolveHuman();
    if (!h) return Framework::Scripting::Builtins::Vector3(0, 0, 0);
    const auto pos = h->data.weaponData.aimPos;
    return Framework::Scripting::Builtins::Vector3(pos.x, pos.y, pos.z);
}

float Human::GetHealth() const {
    auto *h = ResolveHuman();
    return h ? h->data._healthPercent : 0.0f;
}

void Human::SetHealth(float health) {
    // Health is owner-reported state; the server's word still wins. The revision bump is what makes
    // the owner apply it (see HumanEntity::healthRevision), ForceState is what delivers it.
    if (auto *h = ResolveHuman()) {
        if (h->isNpc && h->dead) {
            Framework::Logging::GetLogger("Scripting")->warn("[NPC] cannot revive a dead NPC {}; destroy and respawn", _id);
            return;
        }
        h->data._healthPercent = std::clamp(health, 0.0f, 100.0f);
        h->healthRevision++;
        h->ForceState();
    }
}

uint16_t Human::GetWeaponId() const {
    auto *h = ResolveHuman();
    return h ? h->data.weaponData.currentWeaponId : 0;
}

std::string Human::GetNickname() const {
    auto *h = ResolveHuman();
    return h ? h->nickname : "";
}

v8::Local<v8::Value> Human::GetVehicle(v8::Isolate *isolate) const {
    const uint64_t carId = GetVehicleId();
    if (carId == 0) {
        return v8::Undefined(isolate);
    }
    Vehicle *vehicle = new Vehicle(carId);
    return Vehicle::GetClass(isolate).import_external(isolate, vehicle);
}

uint64_t Human::GetVehicleId() const {
    auto *h = ResolveHuman();
    if (!h) return 0;
    const uint64_t carId = h->data.carPassenger.carId;
    if (carId == 0) return 0;
    auto *repl = Framework::CoreModules::GetReplication();
    return (repl && repl->GetEntityByNetworkID(carId)) ? carId : 0;
}

int Human::GetVehicleSeatIndex() const {
    auto *h = ResolveHuman();
    if (!h || GetVehicleId() == 0) return -1;
    return h->data.carPassenger.seatId;
}

// --- NPC surface ---

bool Human::IsNpc() const {
    auto *h = ResolveHuman();
    return h && h->isNpc;
}

bool Human::IsDead() const {
    auto *h = ResolveHuman();
    return h && h->dead;
}

bool Human::Destroy() {
    auto *h      = ResolveHuman();
    auto *server = MafiaMP::Server::_serverRef;
    if (!h || !server) {
        return false;
    }
    if (!h->isNpc) {
        Framework::Logging::GetLogger("Scripting")->warn("Human.destroy(): {} is a player avatar; it is destroyed on disconnect", _id);
        return false;
    }
    // Deferred (see Server::QueueEntityDestroy); a second destroy() before the flush, including one
    // from a humanDestroyed handler, is ignored.
    if (!server->QueueEntityDestroy(_id)) {
        return false;
    }
    Framework::Logging::GetLogger("Scripting")->info("[NPC] Destroying human {}", _id);
    EventHumanDestroyed(_id);
    return true;
}

template <typename Mutator>
void Human::MutateOrders(Mutator &&mutate) {
    auto *h = ResolveHuman();
    if (!h) {
        return;
    }
    if (!h->isNpc) {
        Framework::Logging::GetLogger("Scripting")->warn("Human {} is a player avatar; orders apply to NPCs only", _id);
        return;
    }
    mutate(*h);
    Framework::Logging::GetLogger("Scripting")->info("[NPC] Human {} order {} target {} pos ({:.1f}, {:.1f}, {:.1f}) stop {:.1f} {}", _id, CommandName(h->GetCommand()), h->aiTargetId, h->aiTargetPos.x, h->aiTargetPos.y, h->aiTargetPos.z, h->aiStopRadius,
        h->aiMoveMode == static_cast<uint8_t>(NpcMoveMode::Run) ? "run" : "walk");
    h->ForceState();
}

void Human::GoTo(const Framework::Scripting::Builtins::Vector3 &pos, bool run) {
    MutateOrders([&](Shared::Entities::HumanEntity &h) {
        h.aiCommand   = static_cast<uint8_t>(NpcCommand::GoTo);
        h.aiTargetId  = 0;
        h.aiTargetPos = pos.vec();
        h.aiMoveMode  = static_cast<uint8_t>(run ? NpcMoveMode::Run : NpcMoveMode::Walk);
    });
}

void Human::Attack(Framework::Scripting::Builtins::Entity *target) {
    if (!target || target->GetId() == _id) {
        return;
    }
    MutateOrders([&](Shared::Entities::HumanEntity &h) {
        h.aiCommand  = static_cast<uint8_t>(NpcCommand::Attack);
        h.aiTargetId = target->GetId();
        h.aiMoveMode = static_cast<uint8_t>(NpcMoveMode::Run);
    });
}

void Human::Follow(Framework::Scripting::Builtins::Entity *target) {
    if (!target || target->GetId() == _id) {
        return;
    }
    MutateOrders([&](Shared::Entities::HumanEntity &h) {
        h.aiCommand  = static_cast<uint8_t>(NpcCommand::Follow);
        h.aiTargetId = target->GetId();
    });
}

void Human::Flee(const Framework::Scripting::Builtins::Vector3 &from) {
    MutateOrders([&](Shared::Entities::HumanEntity &h) {
        h.aiCommand   = static_cast<uint8_t>(NpcCommand::Flee);
        h.aiTargetId  = 0;
        h.aiTargetPos = from.vec();
        h.aiMoveMode  = static_cast<uint8_t>(NpcMoveMode::Run);
    });
}

void Human::ClearOrders() {
    MutateOrders([&](Shared::Entities::HumanEntity &h) {
        h.aiCommand  = static_cast<uint8_t>(NpcCommand::None);
        h.aiTargetId = 0;
    });
}

std::string Human::GetOrder() const {
    auto *h = ResolveHuman();
    return CommandName(h ? h->GetCommand() : NpcCommand::None);
}

uint64_t Human::GetOrderTargetId() const {
    auto *h = ResolveHuman();
    return h ? h->aiTargetId : 0;
}

Framework::Scripting::Builtins::Vector3 Human::GetOrderTargetPos() const {
    auto *h = ResolveHuman();
    return Framework::Scripting::Builtins::Vector3(h ? h->aiTargetPos : glm::vec3(0.0f));
}

std::string Human::GetMoveMode() const {
    auto *h = ResolveHuman();
    return (h && h->aiMoveMode == static_cast<uint8_t>(NpcMoveMode::Walk)) ? "walk" : "run";
}

void Human::SetMoveMode(std::string mode) {
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (mode != "walk" && mode != "run") {
        Framework::Logging::GetLogger("Scripting")->warn("Human.moveMode: expected \"walk\" or \"run\", got \"{}\"", mode);
        return;
    }
    MutateOrders([&](Shared::Entities::HumanEntity &h) {
        h.aiMoveMode = static_cast<uint8_t>(mode == "walk" ? NpcMoveMode::Walk : NpcMoveMode::Run);
    });
}

float Human::GetStopRadius() const {
    auto *h = ResolveHuman();
    return h ? h->aiStopRadius : 0.0f;
}

void Human::SetStopRadius(float radius) {
    MutateOrders([&](Shared::Entities::HumanEntity &h) {
        h.aiStopRadius = std::max(0.5f, radius);
    });
}

v8pp::class_<Human> &Human::GetClass(v8::Isolate *isolate) {
    auto &_class = _classes[isolate];
    if (!_class) {
        // v8pp inherit<Player> requires Player registered first.
        Framework::Scripting::Builtins::Player::GetClass(isolate);

        _class = std::make_unique<v8pp::class_<Human>>(isolate);
        _class->inherit<Framework::Scripting::Builtins::Player>()
            .auto_wrap_objects(true)
            .ctor<uint64_t>()
            .function("toString", &Human::ToString)
            .function("addWeapon", &Human::AddWeapon)
            .function("destroy", &Human::Destroy)
            .function("attack", &Human::Attack)
            .function("follow", &Human::Follow)
            .function("clearOrders", &Human::ClearOrders)
            .property("aiming", &Human::IsAiming)
            .property("firing", &Human::IsFiring)
            .property("aimDir", &Human::GetAimDir)
            .property("aimPos", &Human::GetAimPos)
            .property("health", &Human::GetHealth, &Human::SetHealth)
            .property("weaponId", &Human::GetWeaponId)
            .property("nickname", &Human::GetNickname)
            .property("vehicleSeatIndex", &Human::GetVehicleSeatIndex)
            .property("isNpc", &Human::IsNpc)
            .property("dead", &Human::IsDead)
            .property("order", &Human::GetOrder)
            .property("orderTargetId", &Human::GetOrderTargetId)
            .property("orderTargetPos", &Human::GetOrderTargetPos)
            .property("moveMode", &Human::GetMoveMode, &Human::SetMoveMode)
            .property("stopRadius", &Human::GetStopRadius, &Human::SetStopRadius);

        // goTo(pos: Vector3, run?: boolean) — the trailing flag is optional, which v8pp's fixed
        // arity binding cannot express.
        _class->prototype_function("goTo", [](const v8::FunctionCallbackInfo<v8::Value> &info) {
            auto *isolate = info.GetIsolate();
            auto *self    = v8pp::class_<Human>::unwrap_object(isolate, info.This());
            if (!self) {
                return;
            }
            auto *pos = info.Length() >= 1 ? v8pp::class_<Framework::Scripting::Builtins::Vector3>::unwrap_object(isolate, info[0]) : nullptr;
            if (!pos) {
                isolate->ThrowException(v8::Exception::TypeError(v8pp::to_v8(isolate, "goTo(position: Vector3, run?: boolean)")));
                return;
            }
            const bool run = info.Length() >= 2 ? info[1]->BooleanValue(isolate) : true;
            self->GoTo(*pos, run);
        });

        // flee(from?: Vector3) — without a point the NPC flees from where it stands now.
        _class->prototype_function("flee", [](const v8::FunctionCallbackInfo<v8::Value> &info) {
            auto *isolate = info.GetIsolate();
            auto *self    = v8pp::class_<Human>::unwrap_object(isolate, info.This());
            if (!self) {
                return;
            }
            auto *from = info.Length() >= 1 ? v8pp::class_<Framework::Scripting::Builtins::Vector3>::unwrap_object(isolate, info[0]) : nullptr;
            if (from) {
                self->Flee(*from);
            }
            else {
                self->Flee(self->GetPosition());
            }
        });

        // vehicle returns a wrapped Vehicle handle and needs the isolate, so it stays bespoke.
        auto vehicleGetter = v8::FunctionTemplate::New(isolate, [](const v8::FunctionCallbackInfo<v8::Value> &info) {
            auto *self = v8pp::class_<Human>::unwrap_object(info.GetIsolate(), info.This());
            if (self) {
                info.GetReturnValue().Set(self->GetVehicle(info.GetIsolate()));
            }
        });
        _class->accessor_property("vehicle", vehicleGetter, v8::Local<v8::FunctionTemplate>(), {});
    }
    return *_class;
}

void Human::Register(v8::Isolate *isolate, v8::Local<v8::Object> global) {
    v8pp::class_<Human> &cls = GetClass(isolate);
    auto ctx                 = isolate->GetCurrentContext();
    global->Set(ctx, v8pp::to_v8(isolate, "Human"), cls.js_function_template()->GetFunction(ctx).ToLocalChecked()).Check();
}

} // namespace MafiaMP::Scripting
