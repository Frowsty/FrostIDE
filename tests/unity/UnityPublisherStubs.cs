// SPDX-License-Identifier: Apache-2.0
// Minimal Unity public API shapes used only to compile the publisher in a portable test harness.
namespace UnityEngine { public enum RuntimePlatform { WindowsEditor } public static class Application { public static string dataPath; public static RuntimePlatform platform; } }
namespace UnityEditor { public enum ApiCompatibilityLevel { NET_Standard, NET_Unity_4_8 } public enum EditorAssembliesCompatibilityLevel { NET_Standard, NET_Unity_4_8 } }
namespace UnityEditor.PackageManager { public class PackageInfo { public string name, resolvedPath; public static PackageInfo FindForAssetPath(string path) => null; } }
namespace UnityEditor.Compilation {
    using UnityEditor;
    public enum AssembliesType { Editor }
    [System.Flags] public enum AssemblyFlags { EditorAssembly = 1 }
    public class ScriptCompilerOptions { public string[] ResponseFiles, AdditionalCompilerArguments; public bool AllowUnsafeCode; public string LanguageVersion; public ApiCompatibilityLevel ApiCompatibilityLevel; public EditorAssembliesCompatibilityLevel EditorAssembliesCompatibilityLevel; }
    public class Assembly { public string name; public string[] sourceFiles, compiledAssemblyReferences, defines; public Assembly[] assemblyReferences; public AssemblyFlags flags; public ScriptCompilerOptions compilerOptions; }
    public static class CompilationPipeline { public static Assembly[] GetAssemblies(AssembliesType type) => System.Array.Empty<Assembly>(); }
}
