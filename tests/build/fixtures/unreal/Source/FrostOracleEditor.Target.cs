// SPDX-License-Identifier: Apache-2.0
using UnrealBuildTool;
public class FrostOracleEditorTarget : TargetRules {
    public FrostOracleEditorTarget(TargetInfo target) : base(target) {
        Type = TargetType.Editor;
        ExtraModuleNames.Add("FrostOracle");
    }
}
