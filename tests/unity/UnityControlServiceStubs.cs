// SPDX-License-Identifier: Apache-2.0
// Minimal editor API shapes for a standalone Unity control transport fixture.
using System;
using System.Text.Json;
namespace UnityEngine {
    public enum LogType { Error, Assert, Warning, Log, Exception }
    public static class Application {
        public static string dataPath;
        public static event Action<string, string, LogType> logMessageReceivedThreaded;
        public static void EmitLog(string message, string stack, LogType type) { logMessageReceivedThreaded?.Invoke(message, stack, type); }
    }
    public static class Time { public static int frameCount; }
    public static class Debug { public static void LogError(object message) { Console.Error.WriteLine(message); } }
    public static class JsonUtility {
        private static readonly JsonSerializerOptions Options = new JsonSerializerOptions { IncludeFields = true };
        public static string ToJson(object value) => JsonSerializer.Serialize(value, value.GetType(), Options);
        public static T FromJson<T>(string value) => JsonSerializer.Deserialize<T>(value, Options);
    }
}
namespace UnityEditor {
    [AttributeUsage(AttributeTargets.Class)] public sealed class InitializeOnLoadAttribute : Attribute { }
    public static class SessionState {
        private static readonly System.Collections.Generic.Dictionary<string, object> Values = new System.Collections.Generic.Dictionary<string, object>();
        public static string GetString(string key, string fallback) => Values.TryGetValue(key, out object value) ? (string)value : fallback;
        public static int GetInt(string key, int fallback) => Values.TryGetValue(key, out object value) ? (int)value : fallback;
        public static void SetString(string key, string value) { Values[key] = value; }
        public static void SetInt(string key, int value) { Values[key] = value; }
    }
    public static class EditorApplication {
        public static event Action update;
        public static event Action quitting;
        public static bool isPlaying, isPaused, isCompiling;
        public static event Action projectChanged;
        public static void ProjectChanged() { projectChanged?.Invoke(); }
        public static void Step() { ++UnityEngine.Time.frameCount; }
        public static void Tick() { if (isPlaying && !isPaused) ++UnityEngine.Time.frameCount; update?.Invoke(); }
        public static void Quit() { quitting?.Invoke(); }
    }
    public static class AssetDatabase { public static void Refresh() { EditorApplication.ProjectChanged(); } }
    public static class AssemblyReloadEvents { public static event Action beforeAssemblyReload; public static void Reload() { beforeAssemblyReload?.Invoke(); } }
}
namespace UnityEditor.Compilation {
    public enum CompilerMessageType { Error, Warning }
    public struct CompilerMessage { public string message, file; public int line, column; public CompilerMessageType type; }
    public static class CompilationPipeline {
        public static event Action<object> compilationStarted, compilationFinished;
        public static event Action<string, CompilerMessage[]> assemblyCompilationFinished;
        public static void RequestScriptCompilation() { UnityEditor.EditorApplication.isCompiling = true; compilationStarted?.Invoke(null); }
        public static void Finish(string assembly, CompilerMessage[] messages) {
            assemblyCompilationFinished?.Invoke(assembly, messages);
            UnityEditor.EditorApplication.isCompiling = false;
            compilationFinished?.Invoke(null);
        }
    }
}