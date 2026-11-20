// SPDX-License-Identifier: Apache-2.0
using UnrealBuildTool;
public class FrostOracle : ModuleRules {
    public FrostOracle(ReadOnlyTargetRules target) : base(target) {
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
    }
}
