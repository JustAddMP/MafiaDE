#include <utils/safe_win32.h>

#include "world_mirror.h"

#include "core/application.h"
#include "core/modules/human.h"
#include "core/modules/vehicle.h"
#include "core/story_host.h"

#include "game/helpers/controls.h"

#include "sdk/entities/c_actor.h"
#include "sdk/entities/c_car.h"
#include "sdk/entities/c_entity.h"
#include "sdk/entities/c_entity_list.h"
#include "sdk/entities/c_human_2.h"
#include "sdk/c_game_gfx_env_eff_module.h"
#include "sdk/ue/gfx/environmenteffects/c_gfx_environment_effects.h"
#include "sdk/ue/gfx/environmenteffects/c_weather_manager_2.h"
#include "sdk/ue/game/humanai/c_character_controller.h"
#include "sdk/ue/game/humanai/c_character_state_handler.h"
#include "sdk/entities/c_player_2.h"
#include "sdk/gamedb/tables/vehicles/s_vehicles_table_item.h"
#include "sdk/mafia/framework/c_mafia_dbs.h"
#include "sdk/mafia/framework/c_vehicles_database.h"
#include "sdk/ue/game/c_entity_wrapper_component.h"
#include "sdk/ue/sys/core/c_scene_object.h"
#include "sdk/ue/sys/core/i_component.h"
#include "sdk/ue/sys/utils/c_hash_name.h"

#include "shared/rpc/ids.h"

#include <core_modules.h>
#include <logging/logger.h>
#include <networking/network_peer.h>
#include <networking/replication/replication_manager.h>

#include <mafianet/BitStream.h>
#include <mafianet/string.h>

#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

namespace MafiaMP::Core::Mirror {
    namespace {
        constexpr const char *kLog = "Mirror";

        MafiaNet::RPC4 *RPC() {
            auto *net = gApplication ? gApplication->GetNetworkingEngine()->GetNetworkClient() : nullptr;
            return net ? net->GetRPC() : nullptr;
        }

        bool IsOnline() {
            auto *net = gApplication ? gApplication->GetNetworkingEngine()->GetNetworkClient() : nullptr;
            return net && net->GetConnectionState() == Framework::Networking::PeerState::CONNECTED;
        }

        float Distance(const SDK::ue::sys::math::C_Vector &a, const SDK::ue::sys::math::C_Vector &b) {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        const char *SceneName(SDK::C_Entity *entity) {
            auto *so = entity ? entity->GetSceneObject() : nullptr;
            const char *name = so ? so->GetName() : nullptr;
            return name ? name : "";
        }

        // Wire: <token><NetworkID or 0><kind>
        void OnMirrorSpawnedRaw(MafiaNet::BitStream *bs, MafiaNet::Packet *, void *) {
            uint32_t token     = 0;
            uint64_t networkId = 0;
            uint8_t kind       = 0;
            bs->Read(token);
            bs->Read(networkId);
            bs->Read(kind);
            WorldMirror::OnMirrorSpawned(token, networkId, static_cast<WorldMirror::Kind>(kind));
        }
    } // namespace

    void WorldMirror::Install() {
        if (auto *rpc = RPC()) {
            rpc->RegisterSlot(Shared::RPC::kMirrorSpawned, &OnMirrorSpawnedRaw, nullptr, 0);
        }
    }

    void WorldMirror::Reset() {
        _present.clear();
        _presenceDirty = true;
        _firstSeen.clear();
        if (!_byGame.empty()) {
            Framework::Logging::GetLogger(kLog)->info("[Mirror] Reset: forgetting {} mirrored objects", _byGame.size());
        }
        for (auto &[game, entry] : _byGame) {
            Detach(entry);
        }
        _byGame.clear();
        _byToken.clear();
        _ignored.clear();
        _helloSent      = false;
        _humanCapLogged = false;
        _carCapLogged   = false;
        _humanCount     = 0;
        _carCount       = 0;
    }

    void WorldMirror::SendHello() {
        auto *rpc = RPC();
        if (!rpc) {
            return;
        }
        MafiaNet::BitStream bs;
        rpc->Signal(Shared::RPC::kStoryHostHello, &bs, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, MafiaNet::UNASSIGNED_RAKNET_GUID, true, false);
        _helloSent = true;
        Framework::Logging::GetLogger("Story")->info("[Story] Told the server this client is the story host");
    }

    void WorldMirror::Update() {
        if (!IsStoryHost() || !gApplication) {
            return;
        }
        if (!IsOnline()) {
            if (_helloSent) {
                Reset();
            }
            return;
        }
        // The avatar binding is the sign that the server has this connection's viewer, which is
        // what the hello and every spawn need on the other side.
        if (!gApplication->GetLocalPlayer()) {
            return;
        }
        if (!_helloSent) {
            SendHello();
        }

        ReportEnvironment();

        // Whatever the game deleted since the last frame must be let go of before any adopted
        // entity captures it this frame.
        RefreshPresence();
        _presenceDirty = true; // the next frame rebuilds on first use, whichever side asks first

        const auto now = std::chrono::steady_clock::now();
        if (now - _lastScan < kScanInterval) {
            return;
        }
        _lastScan = now;
        Scan();
    }

    void WorldMirror::ReportEnvironment() {
        const auto now = std::chrono::steady_clock::now();
        if (now - _lastEnvReport < std::chrono::seconds(2)) {
            return;
        }
        _lastEnvReport = now;
        auto *rpc = RPC();
        if (!rpc) {
            return;
        }
        const char *weatherName = SDK::C_GameGfxEnvEffModule::GetCurrentWeatherSetName();
        std::string weather     = weatherName ? weatherName : "";
        float dayTime           = -1.0f;
        if (auto *effects = SDK::ue::gfx::environmenteffects::C_GfxEnvironmentEffects::GetInstance()) {
            if (auto *manager = effects->GetWeatherManager()) {
                dayTime = manager->GetDayTimeHours();
            }
        }
        if (weather.empty() && dayTime < 0.0f) {
            return;
        }
        const bool weatherChanged = weather != _lastWeather;
        const bool timeChanged    = std::fabs(dayTime - _lastDayTime) > 0.02f;
        if (!weatherChanged && !timeChanged) {
            return;
        }
        if (weatherChanged) {
            Framework::Logging::GetLogger("Story")->info("[Story] Weather set is now \"{}\" (clock {:.2f}); relaying to the guests", weather, dayTime);
        }
        _lastWeather = weather;
        _lastDayTime = dayTime;
        MafiaNet::BitStream bs;
        bs.Write(MafiaNet::RakString(weather.c_str()));
        bs.Write(dayTime);
        rpc->Signal(Shared::RPC::kStoryEnvironment, &bs, MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, MafiaNet::UNASSIGNED_RAKNET_GUID, true, false);
    }

    void WorldMirror::RefreshPresence() {
        _presenceDirty = false;
        _present.clear();
        auto *entityList = SDK::GetEntityList();
        if (!entityList) {
            return;
        }
        const unsigned int count = entityList->GetEntityCount();
        _present.reserve(count);
        for (unsigned int i = 0; i < count; ++i) {
            if (auto *entity = entityList->GetEntityByIndex(static_cast<int>(i))) {
                _present.insert(entity);
            }
        }
        std::vector<SDK::C_Entity *> gone;
        for (const auto &[game, entry] : _byGame) {
            if (!_present.count(game)) {
                gone.push_back(game);
            }
        }
        for (auto *game : gone) {
            Release(game, "game object gone");
        }
    }

    bool WorldMirror::IsPresent(const SDK::C_Entity *game) {
        if (!game) {
            return false;
        }
        if (_presenceDirty) {
            RefreshPresence();
        }
        return _present.count(game) != 0;
    }

    void WorldMirror::Scan() {
        auto *entityList = SDK::GetEntityList();
        if (!entityList) {
            return;
        }
        auto logger = Framework::Logging::GetLogger(kLog);

        // 1. What the game has right now.
        std::unordered_set<SDK::C_Entity *> present;
        std::vector<SDK::C_Entity *> humans, cars;
        const unsigned int count = entityList->GetEntityCount();
        for (unsigned int i = 0; i < count; ++i) {
            auto *entity = entityList->GetEntityByIndex(static_cast<int>(i));
            if (!entity) {
                continue;
            }
            const auto type = entity->GetType();
            if (type == SDK::E_EntityType::E_ENTITY_HUMAN) {
                present.insert(entity);
                humans.push_back(entity);
            }
            else if (type == SDK::E_EntityType::E_ENTITY_CAR) {
                present.insert(entity);
                cars.push_back(entity);
            }
        }

        // 2. Mirrored objects the game dropped (mission end, despawn, menu): release them.
        {
            std::vector<SDK::C_Entity *> gone;
            for (const auto &[game, entry] : _byGame) {
                if (!present.count(game)) {
                    gone.push_back(game);
                }
            }
            for (auto *game : gone) {
                Release(game, "game object gone");
            }
            std::vector<SDK::C_Entity *> forgotten;
            for (auto *game : _ignored) {
                if (!present.count(game)) {
                    forgotten.push_back(game);
                }
            }
            for (auto *game : forgotten) {
                _ignored.erase(game);
            }
            for (auto it = _firstSeen.begin(); it != _firstSeen.end();) {
                it = present.count(it->first) ? std::next(it) : _firstSeen.erase(it);
            }
        }

        // Age every candidate: nothing is mirrored before it has lived kAdoptDelay.
        const auto scanNow = std::chrono::steady_clock::now();
        auto oldEnough     = [&](SDK::C_Entity *entity) {
            auto [it, inserted] = _firstSeen.try_emplace(entity, scanNow);
            return !inserted && scanNow - it->second >= kAdoptDelay;
        };

        auto *localPed = Game::Helpers::Controls::GetLocalPlayer();
        if (!localPed) {
            return; // no chapter running
        }
        const auto origin = reinterpret_cast<SDK::C_Actor *>(localPed)->GetPos();

        // 3. Mirrored objects that wandered too far: release them to bound the entity count.
        {
            std::vector<SDK::C_Entity *> tooFar;
            for (const auto &[game, entry] : _byGame) {
                const float range = (entry.kind == Kind::Human ? kHumanRange : kCarRange) + kReleaseMargin;
                if (Distance(reinterpret_cast<SDK::C_Actor *>(game)->GetPos(), origin) > range) {
                    tooFar.push_back(game);
                }
            }
            for (auto *game : tooFar) {
                Release(game, "out of range");
            }
        }

        // 4. Game objects already carried by a replicated entity (remote players, server NPCs,
        //    remote cars, and everything adopted so far).
        std::unordered_set<SDK::C_Entity *> bound;
        if (auto *repl = Framework::CoreModules::GetReplication()) {
            repl->ForEachEntity([&](Framework::Networking::Replication::NetworkEntity *e) {
                if (auto *human = dynamic_cast<Modules::Human *>(e); human && human->human) {
                    bound.insert(reinterpret_cast<SDK::C_Entity *>(human->human));
                }
                else if (auto *vehicle = dynamic_cast<Modules::Vehicle *>(e); vehicle && vehicle->car) {
                    bound.insert(reinterpret_cast<SDK::C_Entity *>(vehicle->car));
                }
            });
        }

        // 5. New humans.
        for (auto *entity : humans) {
            if (entity == reinterpret_cast<SDK::C_Entity *>(localPed) || bound.count(entity) || _byGame.count(entity) || _ignored.count(entity)) {
                continue;
            }
            if (!oldEnough(entity)) {
                continue;
            }
            auto *ped = reinterpret_cast<SDK::C_Human2 *>(entity);
            // A ped the game is still assembling has no controller, no state handler or no script
            // object; capturing it reads through null (seen with spawner prototype peds).
            auto *controller = ped->GetCharacterController();
            if (!controller || controller->GetCurrentStateHandlerType() == SDK::ue::game::humanai::C_CharacterStateHandler::E_SHT_NONE || !ped->GetHumanScript()) {
                continue;
            }
            if (ped->IsDeath()) {
                continue;
            }
            if (Distance(ped->GetPos(), origin) > kHumanRange) {
                continue;
            }
            if (_humanCount >= kMaxHumans) {
                if (!_humanCapLogged) {
                    _humanCapLogged = true;
                    logger->warn("[Mirror] Human cap of {} reached; further humans are not mirrored", kMaxHumans);
                }
                break;
            }
            Spawn(Kind::Human, entity, kDefaultHumanProfile, nullptr);
        }

        // 6. New cars.
        for (auto *entity : cars) {
            if (bound.count(entity) || _byGame.count(entity) || _ignored.count(entity)) {
                continue;
            }
            if (!oldEnough(entity)) {
                continue;
            }
            auto *car = reinterpret_cast<SDK::C_Car *>(entity);
            if (!car->GetVehicle()) {
                continue;
            }
            if (Distance(car->GetPos(), origin) > kCarRange) {
                continue;
            }
            if (_carCount >= kMaxCars) {
                if (!_carCapLogged) {
                    _carCapLogged = true;
                    logger->warn("[Mirror] Car cap of {} reached; further cars are not mirrored", kMaxCars);
                }
                break;
            }
            const char *modelName = ResolveCarModelName(entity);
            if (!modelName) {
                logger->warn("[Mirror] Car \"{}\" has no model name known to the vehicles database; not mirrored", SceneName(entity));
                _ignored.insert(entity);
                continue;
            }
            Spawn(Kind::Car, entity, 0, modelName);
        }

        // 7. Spawns the server never constructed back to us (interest, budget): say so once.
        const auto now = std::chrono::steady_clock::now();
        for (auto &[game, entry] : _byGame) {
            if (!entry.entity && !entry.constructionWarned && now - entry.requestedAt > kConstructionWarnAt) {
                entry.constructionWarned = true;
                logger->warn("[Mirror] {} \"{}\" (token {}, entity {}) was not constructed back to the host after 10 s; the server never got its state", entry.kind == Kind::Human ? "Human" : "Car", SceneName(game), entry.token, entry.networkId);
            }
        }
    }

    void WorldMirror::Spawn(Kind kind, SDK::C_Entity *game, uint64_t modelHash, const char *modelName) {
        auto *rpc = RPC();
        if (!rpc) {
            return;
        }
        auto *actor   = reinterpret_cast<SDK::C_Actor *>(game);
        const auto pos = actor->GetPos();
        const auto rot = actor->GetRot();

        Entry entry;
        entry.kind        = kind;
        entry.token       = _nextToken++;
        entry.game        = game;
        entry.requestedAt = std::chrono::steady_clock::now();
        if (_nextToken == 0) {
            _nextToken = 1;
        }

        // Wire: <kind><token><modelHash | modelName><x><y><z><qw><qx><qy><qz>
        MafiaNet::BitStream bs;
        bs.Write(static_cast<uint8_t>(kind));
        bs.Write(entry.token);
        if (kind == Kind::Human) {
            bs.Write(modelHash);
        }
        else {
            bs.Write(MafiaNet::RakString(modelName));
        }
        bs.Write(pos.x);
        bs.Write(pos.y);
        bs.Write(pos.z);
        bs.Write(rot.w);
        bs.Write(rot.x);
        bs.Write(rot.y);
        bs.Write(rot.z);
        rpc->Signal(Shared::RPC::kMirrorSpawn, &bs, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, MafiaNet::UNASSIGNED_RAKNET_GUID, true, false);

        _byToken[entry.token] = game;
        _byGame[game]         = entry;
        (kind == Kind::Human ? _humanCount : _carCount)++;

        if (kind == Kind::Human) {
            // Discovery: the scene name and its hash, plus whatever the entity wrapper calls the
            // model, so a later pass can map game peds to real spawn profiles.
            const char *sceneName = SceneName(game);
            uint64_t wrapperHash  = 0;
            const char *wrapperModel = "";
            if (auto *so = game->GetSceneObject()) {
                for (size_t i = 0; i < so->GetComponentCount(); ++i) {
                    auto *component = so->GetComponentByIndex(i);
                    if (component && std::strstr(component->GetComponentTypeName(), "EntityWrapperComponent")) {
                        auto *wrapper = static_cast<SDK::ue::game::C_EntityWrapperComponent *>(component);
                        wrapperHash   = wrapper->GetModelNameHash().GetValue();
                        wrapperModel  = wrapper->GetModelName() ? wrapper->GetModelName() : "";
                        break;
                    }
                }
            }
            Framework::Logging::GetLogger(kLog)->info("[Mirror] Spawn human token {} scene \"{}\" (hash {}) wrapper model \"{}\" (hash {}) at ({:.1f}, {:.1f}, {:.1f}); profile sent {}", entry.token, sceneName, SDK::ue::sys::utils::C_HashName::ComputeHash(sceneName), wrapperModel, wrapperHash, pos.x, pos.y, pos.z, modelHash);
        }
        else {
            Framework::Logging::GetLogger(kLog)->info("[Mirror] Spawn car token {} scene \"{}\" model \"{}\" at ({:.1f}, {:.1f}, {:.1f})", entry.token, SceneName(game), modelName, pos.x, pos.y, pos.z);
        }
    }

    void WorldMirror::OnMirrorSpawned(uint32_t token, uint64_t networkId, Kind kind) {
        auto logger  = Framework::Logging::GetLogger(kLog);
        const auto it = _byToken.find(token);
        if (it == _byToken.end()) {
            // Reset in between (disconnect/reconnect): the server holds an entity for nothing.
            if (networkId != 0) {
                logger->warn("[Mirror] Spawned token {} (entity {}) is unknown here; asking for its removal", token, networkId);
                RequestDespawn(networkId);
            }
            return;
        }
        auto *game    = it->second;
        auto entryIt  = _byGame.find(game);
        if (entryIt == _byGame.end()) {
            _byToken.erase(it);
            return;
        }
        Entry &entry = entryIt->second;
        if (networkId == 0) {
            logger->warn("[Mirror] Server refused spawn token {} ({} \"{}\")", token, kind == Kind::Human ? "human" : "car", SceneName(game));
            _ignored.insert(game);
            (entry.kind == Kind::Human ? _humanCount : _carCount)--;
            _byToken.erase(it);
            _byGame.erase(entryIt);
            return;
        }
        entry.networkId = networkId;
        logger->info("[Mirror] Token {} -> entity {} ({})", token, networkId, kind == Kind::Human ? "human" : "car");
    }

    bool WorldMirror::TryAdoptHuman(Modules::Human *human) {
        const auto it = _byToken.find(human->mirrorToken);
        if (it == _byToken.end()) {
            return false;
        }
        auto entryIt = _byGame.find(it->second);
        if (entryIt == _byGame.end() || entryIt->second.kind != Kind::Human || entryIt->second.entity) {
            return false;
        }
        Entry &entry = entryIt->second;
        auto *ped    = reinterpret_cast<SDK::C_Human2 *>(entry.game);

        human->adopted        = true;
        human->isLocalPlayer  = false;
        human->human          = ped;
        // The game's own controller: only base-class reads go through this pointer (CapturePedState,
        // ReadLocal); the override-only setters are never called on an adopted human.
        human->charController = reinterpret_cast<Game::Overrides::CharacterController *>(ped->GetCharacterController());
        entry.entity          = human;
        entry.networkId       = human->GetNetworkID();
        Framework::Logging::GetLogger(kLog)->info("[Mirror] Human {} adopted game ped \"{}\" (token {})", human->GetNetworkID(), SceneName(entry.game), entry.token);
        return true;
    }

    bool WorldMirror::TryAdoptVehicle(Modules::Vehicle *vehicle) {
        const auto it = _byToken.find(vehicle->mirrorToken);
        if (it == _byToken.end()) {
            return false;
        }
        auto entryIt = _byGame.find(it->second);
        if (entryIt == _byGame.end() || entryIt->second.kind != Kind::Car || entryIt->second.entity) {
            return false;
        }
        Entry &entry = entryIt->second;

        vehicle->adopted = true;
        vehicle->car     = reinterpret_cast<SDK::C_Car *>(entry.game);
        entry.entity     = vehicle;
        entry.networkId  = vehicle->GetNetworkID();
        Framework::Logging::GetLogger(kLog)->info("[Mirror] Vehicle {} adopted game car \"{}\" (token {})", vehicle->GetNetworkID(), SceneName(entry.game), entry.token);
        return true;
    }

    void WorldMirror::Detach(Entry &entry) {
        if (!entry.entity) {
            return;
        }
        if (auto *human = dynamic_cast<Modules::Human *>(entry.entity)) {
            human->human          = nullptr;
            human->charController = nullptr;
        }
        else if (auto *vehicle = dynamic_cast<Modules::Vehicle *>(entry.entity)) {
            vehicle->car = nullptr;
        }
        entry.entity = nullptr;
    }

    void WorldMirror::Release(SDK::C_Entity *game, const char *why) {
        auto it = _byGame.find(game);
        if (it == _byGame.end()) {
            return;
        }
        Entry &entry = it->second;
        Framework::Logging::GetLogger(kLog)->info("[Mirror] Releasing {} token {} entity {}: {}", entry.kind == Kind::Human ? "human" : "car", entry.token, entry.networkId, why);
        Detach(entry);
        (entry.kind == Kind::Human ? _humanCount : _carCount)--;
        // With no answer from the server yet the token is simply forgotten: when kMirrorSpawned
        // arrives for an unknown token, OnMirrorSpawned asks for the entity's removal.
        if (entry.networkId != 0) {
            RequestDespawn(entry.networkId);
        }
        _byToken.erase(entry.token);
        _byGame.erase(it);
    }

    void WorldMirror::Quarantine(SDK::C_Entity *game, const char *why) {
        if (!game) {
            return;
        }
        _ignored.insert(game);
        Release(game, why);
    }

    void WorldMirror::OnEntityGone(Framework::Networking::Replication::NetworkEntity *entity) {
        for (auto it = _byGame.begin(); it != _byGame.end(); ++it) {
            if (it->second.entity != entity) {
                continue;
            }
            Entry &entry = it->second;
            Framework::Logging::GetLogger(kLog)->info("[Mirror] Entity {} (token {}) was destroyed; its game object stays unmirrored", entry.networkId, entry.token);
            (entry.kind == Kind::Human ? _humanCount : _carCount)--;
            if (entry.game) {
                _ignored.insert(entry.game);
            }
            _byToken.erase(entry.token);
            _byGame.erase(it);
            return;
        }
    }

    void WorldMirror::RequestDespawn(uint64_t networkId) {
        auto *rpc = RPC();
        if (!rpc || networkId == 0) {
            return;
        }
        // Wire: <NetworkID>
        MafiaNet::BitStream bs;
        bs.Write(networkId);
        rpc->Signal(Shared::RPC::kMirrorDespawn, &bs, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, MafiaNet::UNASSIGNED_RAKNET_GUID, true, false);
    }

    void WorldMirror::LoadKnownCarModels() {
        if (_knownCarModelsLoaded) {
            return;
        }
        const auto mafiaDB = SDK::mafia::framework::GetMafiaDBs();
        if (!mafiaDB) {
            return;
        }
        const auto vehiclesDB = mafiaDB->GetVehiclesDatabase();
        if (!vehiclesDB.IsValid()) {
            return;
        }
        const auto count = vehiclesDB->GetVehiclesCount();
        for (uint32_t i = 0; i < count; ++i) {
            const auto &accessor = vehiclesDB->GetVehicleByIndex(i);
            const auto *item     = accessor.Get();
            if (!item || item->GetID() == 0) {
                continue;
            }
            const char *modelName = item->GetModelName();
            if (modelName && *modelName) {
                _knownCarModels.insert(modelName);
            }
        }
        _knownCarModelsLoaded = !_knownCarModels.empty();
        Framework::Logging::GetLogger(kLog)->info("[Mirror] {} car model names known from the vehicles database", _knownCarModels.size());
    }

    const char *WorldMirror::ResolveCarModelName(SDK::C_Entity *car) {
        LoadKnownCarModels();
        auto *so = car->GetSceneObject();
        if (!so) {
            return nullptr;
        }
        // 1. The entity wrapper component names the model the car was built from.
        for (size_t i = 0; i < so->GetComponentCount(); ++i) {
            auto *component = so->GetComponentByIndex(i);
            if (!component || !std::strstr(component->GetComponentTypeName(), "EntityWrapperComponent")) {
                continue;
            }
            auto *wrapper         = static_cast<SDK::ue::game::C_EntityWrapperComponent *>(component);
            const char *modelName = wrapper->GetModelName();
            if (modelName && *modelName) {
                if (_knownCarModels.count(modelName)) {
                    return modelName;
                }
                Framework::Logging::GetLogger(kLog)->debug("[Mirror] Wrapper model \"{}\" of car \"{}\" is not a vehicles-database model name", modelName, SceneName(car));
            }
            break;
        }
        // 2. The scene object name itself, or a database model name it starts with.
        const char *sceneName = so->GetName();
        if (!sceneName || !*sceneName) {
            return nullptr;
        }
        if (_knownCarModels.count(sceneName)) {
            return sceneName;
        }
        const std::string scene(sceneName);
        const std::string *best = nullptr;
        for (const auto &model : _knownCarModels) {
            if (scene.size() > model.size() && scene.compare(0, model.size(), model) == 0 && !std::isalnum(static_cast<unsigned char>(scene[model.size()]))) {
                if (!best || model.size() > best->size()) {
                    best = &model;
                }
            }
        }
        return best ? best->c_str() : nullptr;
    }
} // namespace MafiaMP::Core::Mirror
