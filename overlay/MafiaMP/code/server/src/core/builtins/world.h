#pragma once

#include <v8.h>
#include <v8pp/class.hpp>
#include <v8pp/convert.hpp>

#include "core/server.h"

#include "shared/entities/human_entity.h"
#include "shared/entities/register_entities.h"
#include "shared/entities/vehicle_entity.h"

#include <core_modules.h>
#include <logging/logger.h>
#include <scripting/builtins/entity.h>
#include <scripting/builtins/entity_collection.h>
#include <scripting/builtins/quaternion.h>
#include <scripting/builtins/vector3.h>
#include <networking/replication/replication_manager.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdlib>
#include <string>

#include "human.h"
#include "player.h"
#include "vehicle.h"

namespace MafiaMP::Scripting {
    // Server-spawned NPC humans only (player avatars are World.players).
    struct NpcsOnly {
        bool operator()(const Framework::Networking::Replication::NetworkEntity *e) const {
            auto *human = dynamic_cast<const Shared::Entities::HumanEntity *>(e);
            return human && human->isNpc;
        }
    };

    using PlayerCollection  = Framework::Scripting::Builtins::EntityCollection<Player, Shared::Entities::HumanEntity, Framework::Scripting::Builtins::ViewersOnly>;
    using HumanCollection   = Framework::Scripting::Builtins::EntityCollection<Human, Shared::Entities::HumanEntity, NpcsOnly>;
    using VehicleCollection = Framework::Scripting::Builtins::EntityCollection<Vehicle, Shared::Entities::VehicleEntity>;

    class World final {
      public:
        // Spawn profile used when a script passes no (or an unparsable) model: Tommy's.
        static constexpr uint64_t kDefaultSpawnProfile = 335218123840277515ULL;

        static v8::Local<v8::Value> CreateHuman(v8::Isolate *isolate, uint64_t modelHash, const glm::vec3 &position, const glm::quat &rotation) {
            auto *engine = Framework::CoreModules::GetReplication();
            if (!engine) {
                return v8::Undefined(isolate);
            }
            auto *entity = dynamic_cast<Shared::Entities::HumanEntity *>(engine->CreateEntity(Shared::Entities::HumanTypeId()));
            if (!entity) {
                return v8::Undefined(isolate);
            }
            entity->isNpc           = true;
            entity->modelHash       = modelHash != 0 ? modelHash : kDefaultSpawnProfile;
            entity->nickname        = "";
            entity->playerIndex     = 0xFFFF;
            entity->position        = position;
            entity->rotation        = rotation;
            entity->streaming.range = Shared::Entities::HumanEntity::kStreamRange;
            Framework::Logging::GetLogger("Scripting")->info("[NPC] Created human {} (profile {}) at ({:.1f}, {:.1f}, {:.1f})", entity->GetNetworkID(), entity->modelHash, position.x, position.y, position.z);
            return v8pp::class_<Human>::create_object(isolate, entity->GetNetworkID());
        }

        // A spawn-profile hash from a JS value: a decimal string (the only lossless form above 2^53),
        // a BigInt, or a number. Empty/unparsable yields 0, which CreateHuman maps to the default.
        static uint64_t ParseModelHash(v8::Isolate *isolate, v8::Local<v8::Value> value) {
            if (value->IsBigInt()) {
                bool lossless = false;
                return value.As<v8::BigInt>()->Uint64Value(&lossless);
            }
            if (value->IsNumber()) {
                const double number = value->NumberValue(isolate->GetCurrentContext()).FromMaybe(0.0);
                return number > 0.0 ? static_cast<uint64_t>(number) : 0;
            }
            if (value->IsString()) {
                v8::String::Utf8Value text(isolate, value);
                const std::string digits = *text ? *text : "";
                if (digits.empty()) {
                    return 0;
                }
                char *end           = nullptr;
                const uint64_t hash = std::strtoull(digits.c_str(), &end, 10);
                if (end == digits.c_str() || (end && *end != '\0')) {
                    Framework::Logging::GetLogger("Scripting")->warn("[NPC] createHuman: model \"{}\" is not a decimal spawn-profile hash; using the default profile", digits);
                    return 0;
                }
                return hash;
            }
            return 0;
        }

        static v8::Local<v8::Value> CreateVehicle(v8::Isolate *isolate, std::string modelName) {
            auto *engine = Framework::CoreModules::GetReplication();
            if (!engine) {
                return v8::Undefined(isolate);
            }
            auto *entity = dynamic_cast<Shared::Entities::VehicleEntity *>(engine->CreateEntity(Shared::Entities::VehicleTypeId()));
            if (!entity) {
                return v8::Undefined(isolate);
            }
            entity->modelName       = modelName;
            entity->streaming.range = Shared::Entities::VehicleEntity::kStreamRange;
            return v8pp::class_<Vehicle>::create_object(isolate, entity->GetNetworkID());
        }

        static float GetDayTimeHours() {
            auto *server = Server::_serverRef;
            return server ? server->GetEnvironment().dayTimeHours : 0.0f;
        }

        static void SetDayTimeHours(float dayTimeHours) {
            if (auto *server = Server::_serverRef) {
                server->SetDayTimeHours(dayTimeHours);
            }
        }

        static std::string GetWeatherSet() {
            auto *server = Server::_serverRef;
            return server ? server->GetEnvironment().weatherSet : "";
        }

        static void SetWeatherSet(std::string weatherSetName) {
            if (auto *server = Server::_serverRef) {
                server->SetWeatherSet(weatherSetName);
            }
        }

        static void Register(v8::Isolate *isolate, v8::Local<v8::Object> global) {
            if (!isolate || global.IsEmpty()) {
                return;
            }

            // Ensure the entity classes exist for v8pp type conversion.
            Framework::Scripting::Builtins::Entity::GetClass(isolate);
            Vehicle::GetClass(isolate);
            Human::GetClass(isolate);
            Player::GetClass(isolate);

            auto ctx      = isolate->GetCurrentContext();
            auto worldObj = v8::Object::New(isolate);

            worldObj->Set(ctx, v8pp::to_v8(isolate, "createVehicle"),
                          v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value> &info) {
                              auto isolate = info.GetIsolate();
                              if (info.Length() < 1 || !info[0]->IsString()) {
                                  isolate->ThrowException(v8::Exception::TypeError(v8pp::to_v8(isolate, "createVehicle requires a model name string")));
                                  return;
                              }
                              v8::String::Utf8Value modelName(isolate, info[0]);
                              info.GetReturnValue().Set(World::CreateVehicle(isolate, *modelName));
                          }).ToLocalChecked()).Check();

            // createHuman(model: string | bigint | number, position: Vector3, rotation?: Vector3 (euler
            // degrees) | Quaternion) -> Human. The model is a spawn-profile hash; pass it as a decimal
            // string, since a JS number cannot hold it exactly.
            worldObj->Set(ctx, v8pp::to_v8(isolate, "createHuman"),
                          v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value> &info) {
                              auto isolate = info.GetIsolate();
                              if (info.Length() < 2) {
                                  isolate->ThrowException(v8::Exception::TypeError(v8pp::to_v8(isolate, "createHuman(model, position: Vector3, rotation?)")));
                                  return;
                              }
                              const uint64_t modelHash = World::ParseModelHash(isolate, info[0]);
                              auto *position           = v8pp::class_<Framework::Scripting::Builtins::Vector3>::unwrap_object(isolate, info[1]);
                              if (!position) {
                                  isolate->ThrowException(v8::Exception::TypeError(v8pp::to_v8(isolate, "createHuman: position must be a Vector3")));
                                  return;
                              }
                              glm::quat rotation = glm::identity<glm::quat>();
                              if (info.Length() >= 3 && !info[2]->IsNullOrUndefined()) {
                                  if (auto *euler = v8pp::class_<Framework::Scripting::Builtins::Vector3>::unwrap_object(isolate, info[2])) {
                                      rotation = glm::quat(glm::radians(euler->vec()));
                                  }
                                  else if (auto *quat = v8pp::class_<Framework::Scripting::Builtins::Quaternion>::unwrap_object(isolate, info[2])) {
                                      rotation = quat->quat();
                                  }
                                  else {
                                      isolate->ThrowException(v8::Exception::TypeError(v8pp::to_v8(isolate, "createHuman: rotation must be a Vector3 (euler degrees) or Quaternion")));
                                      return;
                                  }
                              }
                              info.GetReturnValue().Set(World::CreateHuman(isolate, modelHash, position->vec(), rotation));
                          }).ToLocalChecked()).Check();

            worldObj->Set(ctx, v8pp::to_v8(isolate, "getDayTimeHours"),
                          v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value> &info) {
                              info.GetReturnValue().Set(World::GetDayTimeHours());
                          }).ToLocalChecked()).Check();

            worldObj->Set(ctx, v8pp::to_v8(isolate, "setDayTimeHours"),
                          v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value> &info) {
                              auto isolate = info.GetIsolate();
                              if (info.Length() < 1 || !info[0]->IsNumber()) {
                                  isolate->ThrowException(v8::Exception::TypeError(v8pp::to_v8(isolate, "setDayTimeHours requires a number")));
                                  return;
                              }
                              World::SetDayTimeHours(static_cast<float>(info[0]->NumberValue(isolate->GetCurrentContext()).FromMaybe(0.0)));
                          }).ToLocalChecked()).Check();

            worldObj->Set(ctx, v8pp::to_v8(isolate, "getWeatherSet"),
                          v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value> &info) {
                              info.GetReturnValue().Set(v8pp::to_v8(info.GetIsolate(), World::GetWeatherSet()));
                          }).ToLocalChecked()).Check();

            worldObj->Set(ctx, v8pp::to_v8(isolate, "setWeatherSet"),
                          v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value> &info) {
                              auto isolate = info.GetIsolate();
                              if (info.Length() < 1 || !info[0]->IsString()) {
                                  isolate->ThrowException(v8::Exception::TypeError(v8pp::to_v8(isolate, "setWeatherSet requires a string")));
                                  return;
                              }
                              v8::String::Utf8Value weatherSet(isolate, info[0]);
                              World::SetWeatherSet(*weatherSet);
                          }).ToLocalChecked()).Check();

            worldObj->Set(ctx, v8pp::to_v8(isolate, "players"), Framework::Scripting::Builtins::CreateCollectionObject<PlayerCollection>(isolate)).Check();
            worldObj->Set(ctx, v8pp::to_v8(isolate, "humans"), Framework::Scripting::Builtins::CreateCollectionObject<HumanCollection>(isolate)).Check();
            worldObj->Set(ctx, v8pp::to_v8(isolate, "vehicles"), Framework::Scripting::Builtins::CreateCollectionObject<VehicleCollection>(isolate)).Check();

            global->Set(ctx, v8pp::to_v8(isolate, "World"), worldObj).Check();
        }
    };
} // namespace MafiaMP::Scripting
