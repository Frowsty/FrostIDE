// SPDX-License-Identifier: Apache-2.0
// Minimal editor API shapes for a standalone Unity control transport fixture.
using System;
using System.Text.Json;
namespace UnityEngine {
    public static class Application { public static string dataPath; }
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
        public static bool isPlaying, isPaused;
        public static void Step() { ++UnityEngine.Time.frameCount; }
        public static void Tick() { if (isPlaying && !isPaused) ++UnityEngine.Time.frameCount; update?.Invoke(); }
        public static void Quit() { quitting?.Invoke(); }
    }
    public static class AssemblyReloadEvents { public static event Action beforeAssemblyReload; public static void Reload() { beforeAssemblyReload?.Invoke(); } }
}
