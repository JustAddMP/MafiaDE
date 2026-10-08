#pragma once

#include <integrations/server/instance.h>

#include <mafianet/types.h>

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace MafiaMP {
    class Server: public Framework::Integrations::Server::Instance {
      public:
        // Global world state pushed to clients via the SetEnvironment RPC.
        struct Environment {
            std::string weatherSet = "_default_game";
            float dayTimeHours     = 11.0f;
        };

        void PostInit() override;
        void PostUpdate() override;
        void PreShutdown() override;
        void ModuleRegister(Framework::Scripting::Engine *engine) override;

        void OnPlayerConnect(const Framework::Integrations::Server::PlayerConnectionData &info) override;
        void OnPlayerDisconnect(MafiaNet::PeerGuid guid) override;
        void OnChatMessage(uint64_t senderId, const std::string &text) override;
        void OnChatCommand(uint64_t senderId, const std::string &text, const std::string &command, const std::vector<std::string> &args) override;

        const Environment &GetEnvironment() const {
            return _environment;
        }
        void SetWeatherSet(const std::string &weatherSet);
        void SetDayTimeHours(float dayTimeHours);
        // Send the current environment to one system (UNASSIGNED = broadcast to all).
        void SendEnvironment(MafiaNet::RakNetGUID target, bool broadcast) const;

        // Script-initiated entity removal is deferred to the next tick: a script destroys from inside
        // a collection walk (World.humans.forEach(h => h.destroy())), which indexes the live replica
        // list. Returns false when the entity is already queued, so a caller raises its "destroyed"
        // event exactly once.
        bool QueueEntityDestroy(uint64_t networkId);
        bool IsEntityDestroyQueued(uint64_t networkId) const {
            return _pendingDestroy.count(networkId) != 0;
        }

        // --- Story host world mirror ---
        // The connection running the genuine campaign (kStoryHostHello), or UNASSIGNED.
        MafiaNet::PeerGuid GetStoryHostGuid() const {
            return _storyHostGuid;
        }
        bool IsStoryHost(MafiaNet::PeerGuid guid) const {
            return _storyHostGuid != MafiaNet::UNASSIGNED_PEER_GUID && _storyHostGuid == guid;
        }
        // Entities created through kMirrorSpawn: owned by the host for their whole life.
        bool IsMirrored(uint64_t networkId) const {
            return _mirrored.count(networkId) != 0;
        }
        void SetStoryHost(MafiaNet::PeerGuid guid);
        void AddMirrored(uint64_t networkId);
        void RemoveMirrored(uint64_t networkId);
        // Queue the destruction of every mirrored entity (host gone, or told to).
        void DestroyAllMirrored();
        size_t MirroredCount() const {
            return _mirrored.size();
        }

        static inline Server *_serverRef = nullptr;

      private:
        void InitNetworkingMessages();
        void InitRPCs();

        void FlushPendingDestroys();

        Environment _environment {};
        std::unordered_set<uint64_t> _pendingDestroy;

        MafiaNet::PeerGuid _storyHostGuid = MafiaNet::UNASSIGNED_PEER_GUID;
        std::unordered_set<uint64_t> _mirrored;
    };
} // namespace MafiaMP
