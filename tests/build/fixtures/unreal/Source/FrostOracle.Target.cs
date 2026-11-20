// SPDX-License-Identifier: Apache-2.0
// Original fixture using the documented ModuleRules/TargetRules public API.
using UnrealBuildTool;
public class FrostOracleTarget : TargetRules {
    public FrostOracleTarget(TargetInfo target) : base(target) {
        Type = TargetType.Game;
        ExtraModuleNames.Add("FrostOracle");
    }
}
