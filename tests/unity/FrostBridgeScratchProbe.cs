// SPDX-License-Identifier: Apache-2.0
// Scratch-project probe for generated Unity assembly context; run with Unity -executeMethod.
using System;
using System.IO;
using System.Collections.Generic;
using System.Reflection;
using System.Linq;
using System.Threading;
using Unity.CodeEditor;
using UnityEngine;
using UnityEditor.Compilation;
public static class FrostBridgeScratchProbe
{
    public static void OpenIDE()
    {
        var type = Type.GetType("FrostIDE.UnityIntegration.FrostEditor, FrostIDE.UnityIntegration.Editor", true);
        var editor = (IExternalCodeEditor)Activator.CreateInstance(type, true);
        string executable = Environment.GetEnvironmentVariable("FROST_UNITY_CAPTURE_EXE");
        string sentinel = Environment.GetEnvironmentVariable("FROST_UNITY_OPEN_SENTINEL");
        if (String.IsNullOrEmpty(executable) || String.IsNullOrEmpty(sentinel))
            throw new Exception("FrostIDE executable and sentinel are required");
        editor.Initialize(executable);
        if (!editor.OpenProject("Assets/Probe/FrostUnityProbe.cs", 3, 40))
            throw new Exception("FrostIDE open failed");
        File.WriteAllText(sentinel, "open dispatched");
    }
    public static void Run()
    {
        string root = Directory.GetParent(Application.dataPath).FullName;
        var type = Type.GetType("FrostIDE.UnityIntegration.FrostEditor, FrostIDE.UnityIntegration.Editor", true);
        var editor = (IExternalCodeEditor)Activator.CreateInstance(type, true);
        editor.SyncAll();
        var unityAssembly = CompilationPipeline.GetAssemblies(AssembliesType.Editor).Single(a => a.name == "Frost.Unity.Probe");
        File.WriteAllLines(Path.Combine(root, "Library", "FrostPublicReferences.txt"), unityAssembly.allReferences);
        foreach (string name in new[] { "Frost.Unity.Probe", "Assembly-CSharp-Editor" })
        {
            var item = CompilationPipeline.GetAssemblies(AssembliesType.Editor).Single(a => a.name == name);
            File.WriteAllLines(Path.Combine(root, "Library", name + ".compiled.txt"), item.compiledAssemblyReferences);
            File.WriteAllLines(Path.Combine(root, "Library", name + ".projects.txt"), item.assemblyReferences.Select(a => a.name));
            File.WriteAllLines(Path.Combine(root, "Library", name + ".compiler-options.txt"),
                item.compilerOptions.AdditionalCompilerArguments.Concat(item.compilerOptions.ResponseFiles));
        }
        string unity = Path.Combine(root, ".frost", "unity");
        string descriptor = File.ReadAllText(Path.Combine(unity, "revision"));
        if (descriptor.Length != 67 || !descriptor.StartsWith("1:") || descriptor[34] != ':') throw new Exception("invalid generation descriptor");
        string generation = descriptor.Substring(2, 32);
        string dir = Path.Combine(unity, "generations", generation);
        string sln = Path.Combine(dir, "FrostUnity.sln");
        if (!File.Exists(sln)) throw new Exception("missing solution");
        string[] projects = Directory.GetFiles(dir, "*.csproj");
        string project = projects.Select(File.ReadAllText).Single(x => x.Contains("FrostUnityProbe.cs"));
        if (!project.Contains("Frost.Unity.Probe") || !project.Contains("UnityEngine.CoreModule") || !project.Contains("netstandard.dll") || !project.Contains("DisableImplicitFrameworkReferences"))
            throw new Exception("missing assembly context");
        string editorProject = projects.Select(File.ReadAllText).Single(x => x.Contains("<AssemblyName>Assembly-CSharp-Editor</AssemblyName>"));
        if (!editorProject.Contains("<TargetFramework>net48</TargetFramework>") || !editorProject.Contains("mscorlib.dll") || !editorProject.Contains("SkipGetTargetFrameworkProperties=\"true\""))
            throw new Exception("editor assembly profile was not modeled");
        if (Environment.GetEnvironmentVariable("FROST_UNITY_EXPECT_REFRESH") == "1")
        {
            if (!project.Contains("FrostUnityExtra.cs")) throw new Exception("added source was not synchronized");
            string other = projects.Select(File.ReadAllText).Single(x => x.Contains("FrostOther.cs"));
            if (!other.Contains("Frost.Unity.Other") || !other.Contains("ProjectReference"))
                throw new Exception("added assembly definition was not synchronized");
        }
        var contextType = Type.GetType("FrostIDE.UnityIntegration.ProjectContext, FrostIDE.UnityIntegration.Editor", true);
        var compilerWarnings = contextType.GetMethod("CompilerWarnings", BindingFlags.Static | BindingFlags.NonPublic);
        var safeOptions = new ScriptCompilerOptions { AdditionalCompilerArguments = new[] { "/nowarn:0169,0649" } };
        if ((string)compilerWarnings.Invoke(null, new object[] { safeOptions }) != "0169;0649")
            throw new Exception("supported warning settings were not preserved");
        var unsupportedOptions = new ScriptCompilerOptions { AdditionalCompilerArguments = new[] { "/nullable:enable" } };
        bool unsupported = false;
        try { compilerWarnings.Invoke(null, new object[] { unsupportedOptions }); }
        catch (TargetInvocationException e) when (e.InnerException is InvalidOperationException) { unsupported = true; }
        if (!unsupported) throw new Exception("unsupported compiler settings were silently ignored");
        string dangling = Environment.GetEnvironmentVariable("FROST_UNITY_DANGLING_LINK");
        if (!String.IsNullOrEmpty(dangling))
        {
            var rejectLink = contextType.GetMethod("RejectLink", BindingFlags.Static | BindingFlags.NonPublic);
            bool rejected = false;
            try { rejectLink.Invoke(null, new object[] { dangling }); }
            catch (TargetInvocationException e) when (e.InnerException is IOException) { rejected = true; }
            if (!rejected) throw new Exception("dangling reparse point was not rejected");
        }
        var ensureOwned = contextType.GetMethod("EnsureOwnedOutput", BindingFlags.Static | BindingFlags.NonPublic);
        string unownedRoot = Path.Combine(root, "Library", "FrostUnownedProbe");
        string unowned = Path.Combine(unownedRoot, ".frost", "unity");
        Directory.CreateDirectory(unowned);
        string userFile = Path.Combine(unowned, "user.txt");
        File.WriteAllText(userFile, "keep");
        bool refused = false;
        try { ensureOwned.Invoke(null, new object[] { unownedRoot }); }
        catch (TargetInvocationException e) when (e.InnerException is IOException) { refused = true; }
        if (!refused || File.ReadAllText(userFile) != "keep" || File.Exists(Path.Combine(unowned, "owner")))
            throw new Exception("unowned directory was modified");
        var publish = contextType.GetMethod("Publish", BindingFlags.Static | BindingFlags.NonPublic);
        string danglingCases = Environment.GetEnvironmentVariable("FROST_UNITY_DANGLING_CASES");
        if (!String.IsNullOrEmpty(danglingCases))
        {
            foreach (string name in new[] { "frost", "unity", "owner", "revision", "revision.tmp", "generations" })
            {
                string caseRoot = Path.Combine(danglingCases, name);
                string output = Path.Combine(caseRoot, ".frost", "unity");
                bool rejected = false;
                try
                {
                    if (name == "frost" || name == "unity" || name == "owner")
                        ensureOwned.Invoke(null, new object[] { caseRoot });
                    else
                        publish.Invoke(null, new object[] { output,
                            new SortedDictionary<string, string> { { "FrostUnity.sln", "test" } }, null });
                }
                catch (TargetInvocationException e) when (e.InnerException is IOException) { rejected = true; }
                if ((name != "revision.tmp" && !rejected) || (name == "revision.tmp" && rejected) ||
                    Directory.Exists(Path.Combine(caseRoot, "target-existing")))
                    throw new Exception("dangling junction case failed: " + name);
            }
        }
        string liveCases = Environment.GetEnvironmentVariable("FROST_UNITY_LIVE_CASES");
        if (!String.IsNullOrEmpty(liveCases))
        {
            foreach (string name in new[] { "owner", "revision", "generations", "generation", "solution" })
            {
                string caseRoot = Path.Combine(liveCases, name);
                string output = Path.Combine(caseRoot, ".frost", "unity");
                bool rejected = false;
                try
                {
                    if (name == "owner") ensureOwned.Invoke(null, new object[] { caseRoot });
                    else publish.Invoke(null, new object[] { output,
                        new SortedDictionary<string, string> { { "FrostUnity.sln", "test" } }, null });
                }
                catch (TargetInvocationException e) when (e.InnerException is IOException) { rejected = true; }
                if (!rejected || !Directory.Exists(Path.Combine(caseRoot, "target-existing")))
                    throw new Exception("live junction case failed: " + name);
            }
        }
        string failureRoot = Path.Combine(root, "Library", "FrostPublishProbe");
        var original = new SortedDictionary<string, string> { { "FrostUnity.sln", "old solution" }, { "a.csproj", "old project" } };
        publish.Invoke(null, new object[] { failureRoot, original, null });
        string oldDescriptor = File.ReadAllText(Path.Combine(failureRoot, "revision"));
        string oldSolution = Path.Combine(failureRoot, "generations", oldDescriptor.Substring(2, 32), "FrostUnity.sln");
        var updated = new SortedDictionary<string, string> { { "FrostUnity.sln", "new solution" }, { "a.csproj", "new project" } };
        int writes = 0;
        Action<string> fail = _ => { if (++writes == 2) throw new IOException("injected write failure"); };
        bool failed = false;
        try { publish.Invoke(null, new object[] { failureRoot, updated, fail }); }
        catch (TargetInvocationException e) when (e.InnerException is IOException) { failed = true; }
        if (!failed || File.ReadAllText(Path.Combine(failureRoot, "revision")) != oldDescriptor ||
            File.ReadAllText(oldSolution) != "old solution")
            throw new Exception("failed generation replaced the previously published context");
        string executable = Environment.GetEnvironmentVariable("FROST_UNITY_CAPTURE_EXE");
        string capture = Environment.GetEnvironmentVariable("FROST_CAPTURE");
        if (String.IsNullOrEmpty(executable) || String.IsNullOrEmpty(capture))
            throw new Exception("capture executable is not configured");
        if (!editor.TryGetInstallationForPath(executable, out var installation) || installation.Path != executable)
            throw new Exception("FrostIDE installation was not recognized");
        editor.Initialize(executable);
        if (!editor.OpenProject("Assets/Probe/FrostUnityProbe.cs", 3, 5))
            throw new Exception("open request failed");
        for (int i = 0; i < 100 && !File.Exists(capture); ++i) Thread.Sleep(50);
        if (!File.Exists(capture)) throw new Exception("capture executable did not run");
        string[] args = File.ReadAllLines(capture);
        string[] expected = { "--project", root, "--line", "3", "--column", "5", "--column-utf16",
            Path.Combine(root, "Assets", "Probe", "FrostUnityProbe.cs") };
        if (!args.SequenceEqual(expected)) throw new Exception("open arguments did not round-trip");
        File.WriteAllText(Path.Combine(root, "bridge-probe-success.txt"), "generated context and open arguments inspected");
    }
}
