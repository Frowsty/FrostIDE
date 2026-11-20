// SPDX-License-Identifier: Apache-2.0
// Standalone mock editor loop for the actual Unity control and broker client implementation.
using System;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;
internal static class UnityControlServiceProbe
{
    private static void Main(string[] args)
    {
        UnityEngine.Application.dataPath = Path.Combine(args[0], "Assets");
        var type = Type.GetType("FrostIDE.UnityIntegration.UnityControlService", true);
        RuntimeHelpers.RunClassConstructor(type.TypeHandle);
        if (!(bool)type.GetField("alive", BindingFlags.NonPublic | BindingFlags.Static).GetValue(null))
            throw new Exception("control listener did not start");
        VerifyJournal();
        File.WriteAllText(args[1], "listener started");
        while (!File.Exists(args[2])) { UnityEditor.EditorApplication.Tick(); Thread.Sleep(10); }
        UnityEditor.EditorApplication.Quit();
    }
    private static void VerifyJournal() {
        var journal = FrostIDE.UnityIntegration.UnityBridgeEvents.ReadAfter(0);
        long cursor = journal.last;
        UnityEditor.EditorApplication.ProjectChanged();
        UnityEditor.Compilation.CompilationPipeline.RequestScriptCompilation();
        UnityEditor.Compilation.CompilationPipeline.Finish("actual-fixture.dll", new[] {
            new UnityEditor.Compilation.CompilerMessage { type = UnityEditor.Compilation.CompilerMessageType.Error,
                message = "CS1002: fixture error", file = "Assets/Fixture.cs", line = 7, column = 9 }
        });
        var observed = FrostIDE.UnityIntegration.UnityBridgeEvents.ReadAfter(cursor);
        if (observed.entries.Length != 4 || observed.entries[0].kind != "project" || observed.entries[1].kind != "compilationStarted" ||
            observed.entries[2].kind != "compiler" || observed.entries[2].file != "Assets/Fixture.cs" ||
            observed.entries[2].line != 7 || observed.entries[3].kind != "compilationFinished") throw new Exception("real callback journal mapping changed");
        cursor = observed.last;
        for (int i = 0; i < 530; ++i) UnityEngine.Application.EmitLog("fixture " + i, "stack", UnityEngine.LogType.Warning);
        var bounded = FrostIDE.UnityIntegration.UnityBridgeEvents.ReadAfter(cursor);
        if (bounded.entries.Length != 8 || bounded.lost != 18 || bounded.next <= cursor || bounded.last != cursor + 530)
            throw new Exception("bounded journal gap/continuation contract changed");
        bool refused = false;
        try { FrostIDE.UnityIntegration.UnityBridgeEvents.ReadAfter(bounded.last + 1); } catch (ArgumentOutOfRangeException) { refused = true; }
        if (!refused) throw new Exception("future-domain event cursor accepted");
    }}
