// SPDX-License-Identifier: Apache-2.0
#include "Modules/ModuleManager.h"
#include "OracleObject.h"
class FFrostOracleModule final : public FDefaultGameModuleImpl {
    void StartupModule() override {
        UOracleObject* object = NewObject<UOracleObject>();
        volatile int32 observed = object->AddForBreakpoint(19, 23);
        (void)observed;
    }
};
IMPLEMENT_PRIMARY_GAME_MODULE(FFrostOracleModule, FrostOracle, "FrostOracle");
