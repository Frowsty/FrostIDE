// SPDX-License-Identifier: Apache-2.0
// Runs only in the isolated .NET test harness (tests/unity/test_compiler_arguments.py), never as
// part of the Unity package: checks that ProjectContext models csc.rsp and
// AdditionalCompilerArguments (-define, -nowarn, -unsafe, -langversion, -nullable, -r,
// -warnaserror) and records the options it does not model instead of refusing the project.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using FrostIDE.UnityIntegration;
using UnityEditor;
using UnityEditor.Compilation;
using UnityEngine;

internal static class UnityCompilerArgumentsProbe
{
    private static int s_failures;

    private static void Check(bool condition, string label)
    {
        Console.WriteLine((condition ? "ok   " : "FAIL ") + label);
        if (!condition) ++s_failures;
    }

    private static void ParsesEachModelledOption()
    {
        var parsed = new ProjectContext.CompilerArguments();
        foreach (string argument in new[] { "-define:FOO;BAR", "/d:BAZ", "-nowarn:0168,CS0219", "-unsafe", "-langversion:9.0",
                                            "-nullable:enable", "-r:Libs/Extra.dll", "-warnaserror+", "-warnaserror:CS0618",
                                            "-optimize+", "-define:not-an-identifier", "-r:Alias=Libs/Extra.dll" })
            ProjectContext.ParseCompilerArgument(argument, parsed);
        Check(parsed.Defines.SequenceEqual(new[] { "FOO", "BAR", "BAZ" }), "defines from -define and /d");
        Check(parsed.NoWarn.SequenceEqual(new[] { "0168", "CS0219" }), "nowarn list");
        Check(parsed.Unsafe == true, "-unsafe");
        Check(parsed.LangVersion == "9.0", "-langversion");
        Check(parsed.Nullable == "enable", "-nullable:enable");
        Check(parsed.References.SequenceEqual(new[] { "Libs/Extra.dll" }), "-r path");
        Check(parsed.TreatWarningsAsErrors == true, "-warnaserror+");
        Check(parsed.WarningsAsErrors.SequenceEqual(new[] { "CS0618" }), "-warnaserror:list");
        Check(parsed.Unsupported.SequenceEqual(new[] { "-optimize+", "-define:not-an-identifier", "-r:Alias=Libs/Extra.dll" }),
              "unknown and malformed options are recorded, not applied");

        var toggles = new ProjectContext.CompilerArguments();
        foreach (string argument in new[] { "-unsafe-", "-nullable-", "-warnaserror-" })
            ProjectContext.ParseCompilerArgument(argument, toggles);
        Check(toggles.Unsafe == false && toggles.Nullable == "disable" && toggles.TreatWarningsAsErrors == false,
              "minus toggles turn options off");
    }

    private static void SplitsResponseFiles()
    {
        List<string> arguments = ProjectContext.SplitResponseFile(
            "# comment line\r\n-define:ONE  -nowarn:0414\n\n  \"-r:Path With Spaces/Lib.dll\" -unsafe\n");
        Check(arguments.SequenceEqual(new[] { "-define:ONE", "-nowarn:0414", "-r:Path With Spaces/Lib.dll", "-unsafe" }),
              "response file split: whitespace, quotes and comments");
    }

    private static void SyncWritesTheModel(string root)
    {
        string project = Path.Combine(root, "project");
        Directory.CreateDirectory(Path.Combine(project, "Assets"));
        Directory.CreateDirectory(Path.Combine(project, "Libs"));
        File.WriteAllText(Path.Combine(project, "Libs", "Extra.dll"), "not a real assembly");
        File.WriteAllText(Path.Combine(project, "Assets", "csc.rsp"),
                          "-define:FROM_RSP\n-nowarn:0649\n-unsafe\n-nullable:enable\n-r:Libs/Extra.dll\n-optimize+\n");
        File.WriteAllText(Path.Combine(project, "Assets", "Game.cs"), "class Game {}");
        Application.dataPath = Path.Combine(project, "Assets");
        CompilationPipeline.Assemblies = new[]
        {
            new UnityEditor.Compilation.Assembly
            {
                name = "Game",
                sourceFiles = new[] { "Assets/Game.cs" },
                compiledAssemblyReferences = Array.Empty<string>(),
                defines = new[] { "UNITY_EDITOR" },
                assemblyReferences = Array.Empty<UnityEditor.Compilation.Assembly>(),
                compilerOptions = new ScriptCompilerOptions
                {
                    ResponseFiles = new[] { "Assets/csc.rsp" },
                    AdditionalCompilerArguments = new[] { "-langversion:9.0", "-warnaserror:CS0618", "-checked+" },
                    LanguageVersion = "latest",
                },
            },
        };
        // GenerateFiles is what Sync publishes; it threw "Unity response files are not yet
        // supported" before. Publication itself is UnityPublisherProbe's.
        SortedDictionary<string, string> files = ProjectContext.GenerateFiles();
        string xml = files.Single(file => file.Key.EndsWith(".csproj", StringComparison.Ordinal)).Value;
        Check(xml.Contains("<DefineConstants>UNITY_EDITOR;FROM_RSP</DefineConstants>"), "csproj defines merge Unity's and csc.rsp's");
        Check(xml.Contains("<NoWarn>0649</NoWarn>"), "csproj NoWarn");
        Check(xml.Contains("<AllowUnsafeBlocks>true</AllowUnsafeBlocks>"), "csproj AllowUnsafeBlocks");
        Check(xml.Contains("<LangVersion>9.0</LangVersion>"), "csproj LangVersion from the arguments");
        Check(xml.Contains("<Nullable>enable</Nullable>"), "csproj Nullable");
        Check(xml.Contains("<WarningsAsErrors>CS0618</WarningsAsErrors>"), "csproj WarningsAsErrors");
        Check(xml.Contains("Extra.dll</HintPath>"), "csproj reference from -r");
        Check(files.ContainsKey("FrostUnity.sln"), "the generation has its solution");
        string unsupported = files.TryGetValue(ProjectContext.UnsupportedArgumentsFile, out string listed) ? listed : "";
        Check(unsupported == "Game\t-optimize+\nGame\t-checked+\n", "unmodelled options are published for the IDE: " + unsupported.Replace("\n", "|"));
    }

    private static int Main(string[] args)
    {
        if (args.Length != 1 || !Path.IsPathRooted(args[0]))
        {
            Console.Error.WriteLine("usage: UnityCompilerArgumentsProbe <absolute scratch directory>");
            return 2;
        }
        Directory.CreateDirectory(args[0]);
        ParsesEachModelledOption();
        SplitsResponseFiles();
        SyncWritesTheModel(args[0]);
        Console.WriteLine(s_failures == 0 ? "all checks passed" : s_failures + " check(s) failed");
        return s_failures == 0 ? 0 : 1;
    }
}
