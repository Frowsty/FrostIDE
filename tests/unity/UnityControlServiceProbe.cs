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
        File.WriteAllText(args[1], "listener started");
        while (!File.Exists(args[2])) { UnityEditor.EditorApplication.Tick(); Thread.Sleep(10); }
        UnityEditor.EditorApplication.Quit();
    }
}
