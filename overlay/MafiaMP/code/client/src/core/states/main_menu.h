#pragma once

#include <utils/states/state.h>

namespace MafiaMP::Core::States {
    class MainMenuState: public Framework::Utils::States::IState {
      private:
        bool _shouldProceedConnection;
        bool _shouldProceedOfflineDebug;
        bool _storyAuto      = false; // story host: no connect page, no control lock, auto-connect
        double _storyRetryAt = 0.0;   // story host: next auto-connect attempt (seconds since boot)

      public:
        MainMenuState();
        ~MainMenuState() override;

        virtual const char *GetName() const override;
        virtual int32_t GetId() const override;

        virtual bool OnEnter(Framework::Utils::States::Machine *) override;
        virtual bool OnExit(Framework::Utils::States::Machine *) override;

        virtual bool OnUpdate(Framework::Utils::States::Machine *) override;
    };
} // namespace MafiaMP::Core::States
