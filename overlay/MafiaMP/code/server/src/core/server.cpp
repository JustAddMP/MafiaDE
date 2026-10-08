#include "server.h"

#include "core/builtins/builtins.h"

#include <scripting/builtins/builtins.h>
#include "core/builtins/chat.h"
#include "core/builtins/human.h"
#include "core/builtins/player.h"
#include "core/builtins/vehicle.h"

#include "shared/entities/human_entity.h"
#include "shared/entities/register_entities.h"
#include "shared/entities/vehicle_entity.h"
#include "shared/rpc/ids.h"

#include <core_modules.h>
#include <logging/logger.h>
#include <networking/network_server.h>
#include <networking/replication/delegation.h>
#include <networking/replication/replication_manager.h>

#include <scripting/node_engine.h>
#include <scripting/resource/resource_manager.h>

#include <mafianet/BitStream.h>
#include <mafianet/string.h>
#include <v8pp/convert.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace MafiaMP {
    namespace {
        // Default human skin/spawn-profile until the game assigns one.
        constexpr uint64_t kDefaultSkin = 335218123840277515ULL;

        Framework::Networking::Replication::ReplicationManager *Replication() {
            return Framework::CoreModules::GetReplication();
        }

        // Resolve the human a connection looks through (its avatar).
        Shared::Entities::HumanEntity *ViewerHuman(MafiaNet::PeerGuid guid) {
            auto *repl = Replication();
            return repl ? dynamic_cast<Shared::Entities::HumanEntity *>(repl->GetViewer(guid)) : nullptr;
        }

        template <typename T>
        T *ResolveEntity(uint64_t networkId) {
            auto *repl = Replication();
            return repl ? dynamic_cast<T *>(repl->GetEntityByNetworkID(networkId)) : nullptr;
        }

        // --- Raw RPC4 event handlers ---

        // Wire: <shooter NetworkID><aimPos><aimDir><unk0><unk1>. Relayed to everyone but the shooter.
        void OnHumanShoot(MafiaNet::BitStream *bs, MafiaNet::Packet *packet, void *) {
            uint64_t shooterId = 0;
            glm::vec3 aimPos {}, aimDir {};
            bool unk0 = false, unk1 = false;
            bs->Read(shooterId);
            bs->Read(aimPos);
            bs->Read(aimDir);
            bs->Read(unk0);
            bs->Read(unk1);

            auto *net = static_cast<Framework::Networking::NetworkServer *>(Framework::CoreModules::GetNetworkPeer());
            if (!net) {
                return;
            }
            MafiaNet::BitStream out;
            out.Write(shooterId);
            out.Write(aimPos);
            out.Write(aimDir);
            out.Write(unk0);
            out.Write(unk1);
            net->SignalExcept(Shared::RPC::kHumanShoot, out, packet->guid);
        }

        // Wire: <shooter NetworkID><unk0>. Relayed to everyone but the shooter.
        void OnHumanReload(MafiaNet::BitStream *bs, MafiaNet::Packet *packet, void *) {
            uint64_t shooterId = 0;
            int unk0           = 0;
            bs->Read(shooterId);
            bs->Read(unk0);

            auto *net = static_cast<Framework::Networking::NetworkServer *>(Framework::CoreModules::GetNetworkPeer());
            if (!net) {
                return;
            }
            MafiaNet::BitStream out;
            out.Write(shooterId);
            out.Write(unk0);
            net->SignalExcept(Shared::RPC::kHumanReload, out, packet->guid);
        }

        // Wire: empty. The dying player is resolved from the sender.
        void OnHumanDeath(MafiaNet::BitStream *, MafiaNet::Packet *packet, void *) {
            if (auto *human = ViewerHuman(MafiaNet::ToPeerGuid(packet->guid))) {
                Scripting::Player::EventPlayerDied(human->GetNetworkID());
            }
        }

        // Wire: <human NetworkID><killer NetworkID or 0><damageType>. Sent by the client simulating an
        // NPC when the game kills its ped; only that client may report it.
        void OnHumanNpcDeath(MafiaNet::BitStream *bs, MafiaNet::Packet *packet, void *) {
            uint64_t humanId  = 0;
            uint64_t killerId = 0;
            int damageType    = 0;
            bs->Read(humanId);
            bs->Read(killerId);
            bs->Read(damageType);

            auto *human = ResolveEntity<Shared::Entities::HumanEntity>(humanId);
            if (!human || !human->isNpc) {
                return;
            }
            const auto sender = MafiaNet::ToPeerGuid(packet->guid);
            if (human->ownerGUID != sender) {
                Framework::Logging::GetLogger("Scripting")->warn("[NPC] Death of human {} reported by {} which is not its simulator; ignored", humanId, static_cast<uint64_t>(sender));
                return;
            }
            if (human->dead) {
                return;
            }
            human->dead                = true;
            human->data._healthPercent = 0.0f;
            human->healthRevision++;
            human->ForceState();
            if (killerId != 0 && !ResolveEntity<Shared::Entities::HumanEntity>(killerId)) {
                killerId = 0;
            }
            Scripting::Human::EventHumanDied(humanId, killerId, damageType);
        }

        // Wire: <vehicle NetworkID><seatIndex>.
        void OnVehiclePlayerEnter(MafiaNet::BitStream *bs, MafiaNet::Packet *packet, void *) {
            uint64_t vehicleId = 0;
            int seatIndex      = 0;
            bs->Read(vehicleId);
            bs->Read(seatIndex);

            auto *player  = ViewerHuman(MafiaNet::ToPeerGuid(packet->guid));
            auto *vehicle = ResolveEntity<Shared::Entities::VehicleEntity>(vehicleId);
            if (!player || !vehicle) {
                return;
            }
            if (seatIndex >= 0 && seatIndex < Shared::Entities::VehicleEntity::kMaxSeats) {
                vehicle->seats[seatIndex] = player->GetNetworkID();
                if (seatIndex == 0) {
                    // The driver's client becomes authoritative for the vehicle.
                    vehicle->SetOwner(MafiaNet::ToPeerGuid(packet->guid));
                }
            }
            Scripting::Vehicle::EventVehiclePlayerEnter(vehicleId, player->GetNetworkID(), seatIndex);
        }

        // Wire: <vehicle NetworkID>.
        void OnVehiclePlayerLeave(MafiaNet::BitStream *bs, MafiaNet::Packet *packet, void *) {
            uint64_t vehicleId = 0;
            bs->Read(vehicleId);

            auto *player  = ViewerHuman(MafiaNet::ToPeerGuid(packet->guid));
            auto *vehicle = ResolveEntity<Shared::Entities::VehicleEntity>(vehicleId);
            if (!player || !vehicle) {
                return;
            }
            const uint64_t playerId = player->GetNetworkID();
            for (int i = 0; i < Shared::Entities::VehicleEntity::kMaxSeats; ++i) {
                if (vehicle->seats[i] == playerId) {
                    vehicle->seats[i] = 0;
                    if (i == 0) {
                        // A mirrored car lives in the host's game: it goes back to the host, who keeps
                        // reporting what the game does with it, never to the server.
                        auto *server = Server::_serverRef;
                        if (server && server->IsMirrored(vehicleId) && server->GetStoryHostGuid() != MafiaNet::UNASSIGNED_PEER_GUID) {
                            vehicle->SetOwner(server->GetStoryHostGuid());
                        }
                        else {
                            vehicle->SetOwner(MafiaNet::UNASSIGNED_PEER_GUID); // back to the server
                        }
                    }
                }
            }
            Scripting::Vehicle::EventVehiclePlayerLeave(vehicleId, playerId);
        }

        // Wire: <modelName>.
        void OnSpawnCar(MafiaNet::BitStream *bs, MafiaNet::Packet *, void *) {
            MafiaNet::RakString modelName;
            if (!bs->Read(modelName)) {
                return;
            }
            auto *engine = Replication();
            if (!engine) {
                return;
            }
            if (auto *vehicle = dynamic_cast<Shared::Entities::VehicleEntity *>(engine->CreateEntity(Shared::Entities::VehicleTypeId()))) {
                vehicle->modelName       = modelName.C_String();
                vehicle->streaming.range = Shared::Entities::VehicleEntity::kStreamRange;
            }
        }

        // --- Story host world mirror ---
        // Server-side ceiling on what one host may mirror; the host caps itself lower (see the
        // client's WorldMirror), this only bounds a misbehaving one.
        constexpr size_t kMirrorMaxEntities = 192;

        enum class MirrorKind : uint8_t { Human = 0, Car = 1 };

        // Wire: empty. The first connection to send it becomes the story host.
        void OnStoryEnvironment(MafiaNet::BitStream *bs, MafiaNet::Packet *packet, void *) {
            auto *server = Server::_serverRef;
            if (!server || !server->IsStoryHost(MafiaNet::ToPeerGuid(packet->guid))) {
                return;
            }
            MafiaNet::RakString weather;
            float dayTime = -1.0f;
            bs->Read(weather);
            bs->Read(dayTime);
            const auto &env = server->GetEnvironment();
            if (weather.GetLength() > 0 && env.weatherSet != weather.C_String()) {
                Framework::Logging::GetLogger("Story")->info("[Story] Host weather set \"{}\"", weather.C_String());
                server->SetWeatherSet(weather.C_String());
            }
            if (dayTime >= 0.0f && std::fabs(env.dayTimeHours - dayTime) > 0.01f) {
                server->SetDayTimeHours(dayTime);
            }
        }

        void OnStoryHostHello(MafiaNet::BitStream *, MafiaNet::Packet *packet, void *) {
            auto *server      = Server::_serverRef;
            const auto sender = MafiaNet::ToPeerGuid(packet->guid);
            if (!server) {
                return;
            }
            if (!ViewerHuman(sender)) {
                Framework::Logging::GetLogger("Story")->warn("[Story] Hello from {} before its avatar exists; ignored", static_cast<uint64_t>(sender));
                return;
            }
            if (server->GetStoryHostGuid() == MafiaNet::UNASSIGNED_PEER_GUID) {
                server->SetStoryHost(sender);
                Framework::Logging::GetLogger("Story")->info("[Story] Connection {} is now the story host", static_cast<uint64_t>(sender));
            }
            else if (server->IsStoryHost(sender)) {
                Framework::Logging::GetLogger("Story")->info("[Story] Story host {} said hello again", static_cast<uint64_t>(sender));
            }
            else {
                Framework::Logging::GetLogger("Story")->warn("[Story] Connection {} wants to be the story host but {} already is; refused", static_cast<uint64_t>(sender), static_cast<uint64_t>(server->GetStoryHostGuid()));
            }
        }

        void ReplyMirrorSpawned(MafiaNet::RakNetGUID target, uint32_t token, uint64_t networkId, MirrorKind kind) {
            auto *net = static_cast<Framework::Networking::NetworkServer *>(Framework::CoreModules::GetNetworkPeer());
            if (!net) {
                return;
            }
            // Wire: <token><NetworkID or 0><kind>
            MafiaNet::BitStream out;
            out.Write(token);
            out.Write(networkId);
            out.Write(static_cast<uint8_t>(kind));
            net->GetRPC()->Signal(Shared::RPC::kMirrorSpawned, &out, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, target, false, false);
        }

        // Wire: <kind><token><modelHash | modelName><x><y><z><qw><qx><qy><qz>. Host only.
        void OnMirrorSpawn(MafiaNet::BitStream *bs, MafiaNet::Packet *packet, void *) {
            uint8_t kindRaw = 0;
            uint32_t token  = 0;
            bs->Read(kindRaw);
            bs->Read(token);
            const auto kind = static_cast<MirrorKind>(kindRaw);

            uint64_t modelHash = 0;
            MafiaNet::RakString modelName;
            if (kind == MirrorKind::Human) {
                bs->Read(modelHash);
            }
            else {
                bs->Read(modelName);
            }
            glm::vec3 pos {};
            float qw = 1.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f;
            bs->Read(pos.x);
            bs->Read(pos.y);
            bs->Read(pos.z);
            bs->Read(qw);
            bs->Read(qx);
            bs->Read(qy);
            bs->Read(qz);

            auto *server      = Server::_serverRef;
            const auto sender = MafiaNet::ToPeerGuid(packet->guid);
            if (!server || !server->IsStoryHost(sender)) {
                Framework::Logging::GetLogger("Story")->warn("[Mirror] Spawn from {} which is not the story host; refused", static_cast<uint64_t>(sender));
                ReplyMirrorSpawned(packet->guid, token, 0, kind);
                return;
            }
            auto *engine = Replication();
            if (!engine) {
                return;
            }
            if (server->MirroredCount() >= kMirrorMaxEntities) {
                Framework::Logging::GetLogger("Story")->warn("[Mirror] Mirror cap of {} entities reached; spawn (token {}) refused", kMirrorMaxEntities, token);
                ReplyMirrorSpawned(packet->guid, token, 0, kind);
                return;
            }

            const glm::quat rot(qw, qx, qy, qz);
            Framework::Networking::Replication::NetworkEntity *created = nullptr;
            if (kind == MirrorKind::Human) {
                auto *human = dynamic_cast<Shared::Entities::HumanEntity *>(engine->CreateEntity(Shared::Entities::HumanTypeId()));
                if (!human) {
                    ReplyMirrorSpawned(packet->guid, token, 0, kind);
                    return;
                }
                human->isNpc           = true;
                human->mirrorToken     = token;
                human->modelHash       = modelHash != 0 ? modelHash : kDefaultSkin;
                human->nickname        = "";
                human->playerIndex     = 0xFFFF;
                human->position        = pos;
                human->rotation        = rot;
                human->streaming.range = Shared::Entities::HumanEntity::kStreamRange;
                human->ownerGUID       = sender;
                // Pinned: the host is the only peer with this ped, so the delegation must never move it.
                engine->Delegation().Pin(human, sender);
                created = human;
                Framework::Logging::GetLogger("Story")->info("[Mirror] Human {} mirrored from the host (token {}, profile {}) at ({:.1f}, {:.1f}, {:.1f})", human->GetNetworkID(), token, human->modelHash, pos.x, pos.y, pos.z);
            }
            else {
                auto *vehicle = dynamic_cast<Shared::Entities::VehicleEntity *>(engine->CreateEntity(Shared::Entities::VehicleTypeId()));
                if (!vehicle) {
                    ReplyMirrorSpawned(packet->guid, token, 0, kind);
                    return;
                }
                vehicle->mirrorToken     = token;
                vehicle->modelName       = modelName.C_String();
                vehicle->position        = pos;
                vehicle->rotation        = rot;
                vehicle->streaming.range = Shared::Entities::VehicleEntity::kStreamRange;
                vehicle->ownerGUID       = sender;
                created = vehicle;
                Framework::Logging::GetLogger("Story")->info("[Mirror] Vehicle {} mirrored from the host (token {}, model {}) at ({:.1f}, {:.1f}, {:.1f})", vehicle->GetNetworkID(), token, vehicle->modelName, pos.x, pos.y, pos.z);
            }
            server->AddMirrored(created->GetNetworkID());
            ReplyMirrorSpawned(packet->guid, token, created->GetNetworkID(), kind);
        }

        // Wire: <NetworkID>. Host only; the game object behind a mirrored entity is gone.
        void OnMirrorDespawn(MafiaNet::BitStream *bs, MafiaNet::Packet *packet, void *) {
            uint64_t networkId = 0;
            bs->Read(networkId);
            auto *server      = Server::_serverRef;
            const auto sender = MafiaNet::ToPeerGuid(packet->guid);
            if (!server || !server->IsStoryHost(sender)) {
                return;
            }
            if (!server->IsMirrored(networkId)) {
                Framework::Logging::GetLogger("Story")->warn("[Mirror] Despawn of {} which is not a mirrored entity; ignored", networkId);
                return;
            }
            Framework::Logging::GetLogger("Story")->info("[Mirror] Entity {} despawned by the host", networkId);
            server->RemoveMirrored(networkId);
            server->QueueEntityDestroy(networkId);
        }
    } // namespace

    void Server::PostInit() {
        _serverRef = this;

        // Every resource runs in its own isolate; drop our per-isolate JS class wrappers with it.
        Framework::Scripting::Builtins::AddUnregisterHook([](v8::Isolate *isolate) {
            Scripting::Human::UnregisterIsolate(isolate);
            Scripting::Player::UnregisterIsolate(isolate);
            Scripting::Vehicle::UnregisterIsolate(isolate);
        });

        // Register the replicated entity types so the server can construct them.
        Shared::Entities::RegisterEntities();

        // Event arguments are copied between resource isolates through ValueTransfer; without these
        // a Human/Player/Vehicle handle arrives in a resource as the framework base class (no
        // nickname, health, vehicle, ...). Bases first: the newest registration is tried first.
        Framework::Scripting::Builtins::RegisterHandleTransfer<Scripting::Vehicle>("MafiaMP::Vehicle");
        Framework::Scripting::Builtins::RegisterHandleTransfer<Scripting::Human>("MafiaMP::Human");
        Framework::Scripting::Builtins::RegisterHandleTransfer<Scripting::Player>("MafiaMP::Player");

        if (auto *repl = Framework::CoreModules::GetReplication()) {
            // Mafia is Z-up (the nametag lifts a head position by adding to z), so relevance has to
            // be measured on the XY ground plane; on the default XZ plane it compares altitudes
            // instead of distances. The margin keeps a car pacing a player at the edge of its range
            // from constructing and destroying once per tick, and the look-ahead leans interest down
            // the road being driven so the model loader gets its head start.
            repl->SetInterestGroundPlaneXY(true);
            repl->SetInterestStreamOutMargin(50.0f);
            repl->SetInterestLookaheadSeconds(1.0f);
            repl->SetInterestRebuildInterval(100);

            // Humans get no budget: the server caps at 10 players, so any cap that could bind would
            // be hiding people. Vehicles are script-spawnable without bound, so they get the cap.
            repl->SetInterestBudget(Shared::Entities::VehicleEntity::kTypeName, Shared::Entities::VehicleEntity::kInterestBudget);

            // NPCs are simulated by the nearest client (HumanEntity::GetDelegationPolicy); log every
            // handover so an in-game test can tell who is running which ped.
            repl->Delegation().AddSimulatorChangedHandler([](Framework::Networking::Replication::NetworkEntity *entity, MafiaNet::PeerGuid previous, MafiaNet::PeerGuid current, Framework::Networking::Replication::DelegationChange) {
                Framework::Logging::GetLogger("Scripting")->info("[NPC] Human {} simulator {} -> {}", entity->GetNetworkID(), static_cast<uint64_t>(previous), static_cast<uint64_t>(current));

                // A simulator that dropped between the fatal hit and its kHumanNpcDeath leaves the
                // NPC at zero health but alive on paper; going dormant is where that is settled.
                auto *human = dynamic_cast<Shared::Entities::HumanEntity *>(entity);
                if (current == MafiaNet::UNASSIGNED_PEER_GUID && human && human->isNpc && !human->dead && human->data._healthPercent <= 0.0f) {
                    human->dead = true;
                    human->healthRevision++;
                    human->ForceState();
                    Framework::Logging::GetLogger("Scripting")->info("[NPC] Human {} went dormant at zero health; treating as dead", human->GetNetworkID());
                    Scripting::Human::EventHumanDied(human->GetNetworkID(), 0, 0);
                }
            });
        }

        InitNetworkingMessages();
    }

    void Server::PostUpdate() {
        FlushPendingDestroys();
    }

    bool Server::QueueEntityDestroy(uint64_t networkId) {
        return _pendingDestroy.insert(networkId).second;
    }

    void Server::FlushPendingDestroys() {
        if (_pendingDestroy.empty()) {
            return;
        }
        auto *repl = Replication();
        // Take the batch first: a destruction can raise callbacks that queue more.
        std::vector<uint64_t> batch(_pendingDestroy.begin(), _pendingDestroy.end());
        _pendingDestroy.clear();
        for (const uint64_t networkId : batch) {
            auto *entity = repl ? repl->GetEntityByNetworkID(networkId) : nullptr;
            _mirrored.erase(networkId);
            if (!entity) {
                continue; // already gone (disconnect, double queue from a stale handle)
            }
            repl->DestroyEntity(entity);
        }
    }

    void Server::PreShutdown() {}

    void Server::SetStoryHost(MafiaNet::PeerGuid guid) {
        _storyHostGuid = guid;
    }

    void Server::AddMirrored(uint64_t networkId) {
        _mirrored.insert(networkId);
    }

    void Server::RemoveMirrored(uint64_t networkId) {
        _mirrored.erase(networkId);
    }

    void Server::DestroyAllMirrored() {
        if (_mirrored.empty()) {
            return;
        }
        Framework::Logging::GetLogger("Story")->info("[Mirror] Destroying {} mirrored entities", _mirrored.size());
        for (const uint64_t networkId : _mirrored) {
            QueueEntityDestroy(networkId);
        }
        _mirrored.clear();
    }

    // Bridge framework chat into the gamemode's scripting events (the framework parses and resolves
    // the sender; we surface it to JS with the mod's Player handle).
    void Server::OnChatMessage(uint64_t senderId, const std::string &text) {
        Scripting::Chat::EventChatMessage(senderId, text);
    }

    void Server::OnChatCommand(uint64_t senderId, const std::string &text, const std::string &command, const std::vector<std::string> &args) {
        Scripting::Chat::EventChatCommand(senderId, text, command, args);
    }

    void Server::SetWeatherSet(const std::string &weatherSet) {
        _environment.weatherSet = weatherSet;
        SendEnvironment(MafiaNet::UNASSIGNED_RAKNET_GUID, true);
    }

    void Server::SetDayTimeHours(float dayTimeHours) {
        _environment.dayTimeHours = dayTimeHours;
        SendEnvironment(MafiaNet::UNASSIGNED_RAKNET_GUID, true);
    }

    void Server::SendEnvironment(MafiaNet::RakNetGUID target, bool broadcast) const {
        auto *net = GetNetworkingEngine()->GetNetworkServer();
        if (!net) {
            return;
        }
        // Wire: <optional weatherSet><optional dayTimeHours>
        MafiaNet::BitStream bs;
        const std::optional<MafiaNet::RakString> weather(MafiaNet::RakString(_environment.weatherSet.c_str()));
        const std::optional<float> dayTime(_environment.dayTimeHours);
        bs.Write(weather.has_value());
        if (weather) {
            bs.Write(*weather);
        }
        bs.Write(dayTime.has_value());
        if (dayTime) {
            bs.Write(*dayTime);
        }
        net->GetRPC()->Signal(Shared::RPC::kSetEnvironment, &bs, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, target, broadcast, false);
    }

    void Server::InitNetworkingMessages() {
        InitRPCs();
    }

    void Server::OnPlayerConnect(const Framework::Integrations::Server::PlayerConnectionData &info) {
        auto *engine = Replication();
        auto *net    = GetNetworkingEngine()->GetNetworkServer();
        auto *repl   = net ? net->GetReplicationManager() : nullptr;
        if (!engine || !repl) {
            return;
        }

        // Create the player's avatar, own it, populate its spawn metadata, and make it this
        // connection's viewer.
        auto *human = dynamic_cast<Shared::Entities::HumanEntity *>(engine->CreateEntity(Shared::Entities::HumanTypeId()));
        if (!human) {
            return;
        }
        human->ownerGUID       = info.guid;
        human->modelHash       = kDefaultSkin;
        human->nickname        = info.nickname;
        human->playerIndex     = info.playerIndex;
        human->streaming.range = Shared::Entities::HumanEntity::kStreamRange;
        repl->SetViewer(info.guid, human);

        SendEnvironment(MafiaNet::ToGuid(info.guid), false);
        Scripting::Player::EventPlayerConnected(human->GetNetworkID());
    }

    void Server::OnPlayerDisconnect(MafiaNet::PeerGuid guid) {
        if (IsStoryHost(guid)) {
            Framework::Logging::GetLogger("Story")->info("[Story] The story host {} disconnected", static_cast<uint64_t>(guid));
            DestroyAllMirrored();
            _storyHostGuid = MafiaNet::UNASSIGNED_PEER_GUID;
        }
        if (auto *human = ViewerHuman(guid)) {
            Scripting::Player::EventPlayerDisconnected(human->GetNetworkID());
        }
    }

    void Server::InitRPCs() {
        auto *rpc = GetNetworkingEngine()->GetNetworkServer()->GetRPC();
        // Gameplay RPCs received from clients.
        rpc->RegisterSlot(Shared::RPC::kHumanShoot, &OnHumanShoot, nullptr, 0);
        rpc->RegisterSlot(Shared::RPC::kHumanReload, &OnHumanReload, nullptr, 0);
        rpc->RegisterSlot(Shared::RPC::kHumanDeath, &OnHumanDeath, nullptr, 0);
        rpc->RegisterSlot(Shared::RPC::kHumanNpcDeath, &OnHumanNpcDeath, nullptr, 0);
        rpc->RegisterSlot(Shared::RPC::kVehiclePlayerEnter, &OnVehiclePlayerEnter, nullptr, 0);
        rpc->RegisterSlot(Shared::RPC::kVehiclePlayerLeave, &OnVehiclePlayerLeave, nullptr, 0);
        rpc->RegisterSlot(Shared::RPC::kSpawnCar, &OnSpawnCar, nullptr, 0);
        // Story host world mirror.
        rpc->RegisterSlot(Shared::RPC::kStoryHostHello, &OnStoryHostHello, nullptr, 0);
        rpc->RegisterSlot(Shared::RPC::kStoryEnvironment, &OnStoryEnvironment, nullptr, 0);
        rpc->RegisterSlot(Shared::RPC::kMirrorSpawn, &OnMirrorSpawn, nullptr, 0);
        rpc->RegisterSlot(Shared::RPC::kMirrorDespawn, &OnMirrorDespawn, nullptr, 0);
    }

    void Server::ModuleRegister(Framework::Scripting::Engine *engine) {
        auto *nodeEngine = dynamic_cast<Framework::Scripting::NodeEngine *>(engine);
        if (!nodeEngine) {
            return;
        }

        v8::Isolate *isolate = nodeEngine->GetIsolate();
        v8::Locker locker(isolate);
        v8::Isolate::Scope isolateScope(isolate);
        v8::HandleScope handleScope(isolate);
        v8::Local<v8::Context> context = nodeEngine->GetContext();
        v8::Context::Scope contextScope(context);

        MafiaMP::Scripting::Builtins::Register(isolate, context->Global());
    }
} // namespace MafiaMP
