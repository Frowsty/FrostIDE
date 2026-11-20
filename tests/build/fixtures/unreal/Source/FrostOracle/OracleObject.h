// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "OracleObject.generated.h"
UCLASS(BlueprintType)
class FROSTORACLE_API UOracleObject : public UObject {
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="FrostOracle")
    int32 AddForBreakpoint(int32 left, int32 right);
};
