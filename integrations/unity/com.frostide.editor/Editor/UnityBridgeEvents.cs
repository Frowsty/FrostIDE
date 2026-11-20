// SPDX-License-Identifier: Apache-2.0
// Original bounded journal of public Unity log/compiler/project callbacks, scoped to the current domain.
using System;
using System.Collections.Generic;
using UnityEditor;
using UnityEditor.Compilation;
using UnityEngine;
namespace FrostIDE.UnityIntegration {
    internal static class UnityBridgeEvents {
        [Serializable] internal sealed class Entry {
            public long sequence;
            public string kind, level, message, stack, file, assembly;
            public int line, column;
            public bool truncated;
        }
        internal sealed class Batch {
            internal Entry[] entries;
            internal long first, next, last, lost;
        }
        private static readonly object gate = new object();
        private static readonly Queue<Entry> entries = new Queue<Entry>();
        private static long sequence, projectVersion;
        private static bool started;
        private static string Clip(string value, int limit) {
            value = value ?? String.Empty;
            if (value.Length <= limit) return value;
            int length = limit;
            if (length > 0 && Char.IsHighSurrogate(value[length - 1])) --length;
            return value.Substring(0, length);
        }
        internal static long ProjectVersion { get { lock (gate) return projectVersion; } }
        internal static void Start() {
            if (started) return;
            started = true;
            Application.logMessageReceivedThreaded += OnLog;
            EditorApplication.projectChanged += OnProject;
            CompilationPipeline.compilationStarted += OnCompilationStarted;
            CompilationPipeline.compilationFinished += OnCompilationFinished;
            CompilationPipeline.assemblyCompilationFinished += OnAssembly;
        }
        internal static void Stop() {
            if (!started) return;
            started = false;
            Application.logMessageReceivedThreaded -= OnLog;
            EditorApplication.projectChanged -= OnProject;
            CompilationPipeline.compilationStarted -= OnCompilationStarted;
            CompilationPipeline.compilationFinished -= OnCompilationFinished;
            CompilationPipeline.assemblyCompilationFinished -= OnAssembly;
        }
        private static void Add(Entry entry) {
            lock (gate) {
                if (sequence == Int64.MaxValue) return;
                entry.sequence = ++sequence;
                entry.truncated = (entry.message?.Length ?? 0) > 512 || (entry.stack?.Length ?? 0) > 2048 || (entry.file?.Length ?? 0) > 512 || (entry.assembly?.Length ?? 0) > 512;
                entry.message = Clip(entry.message, 512); entry.stack = Clip(entry.stack, 2048);
                entry.file = Clip(entry.file, 512); entry.assembly = Clip(entry.assembly, 512);
                entries.Enqueue(entry);
                while (entries.Count > 512) entries.Dequeue();
            }
        }
        private static void OnLog(string message, string stack, LogType type) {
            Add(new Entry { kind = "log", level = type == LogType.Warning ? "warning" :
                type == LogType.Error || type == LogType.Exception || type == LogType.Assert ? "error" : "info", message = message, stack = stack });
        }
        private static void OnProject() {
            lock (gate) { if (projectVersion != Int64.MaxValue) ++projectVersion; }
            Add(new Entry { kind = "project", level = "info", message = "Unity project changed" });
        }
        private static void OnCompilationStarted(object context) { Add(new Entry { kind = "compilationStarted", level = "info", message = "Unity compilation started" }); }
        private static void OnCompilationFinished(object context) { Add(new Entry { kind = "compilationFinished", level = "info", message = "Unity compilation finished" }); }
        private static void OnAssembly(string assembly, CompilerMessage[] messages) {
            foreach (var message in messages ?? Array.Empty<CompilerMessage>())
                Add(new Entry { kind = "compiler", level = message.type == CompilerMessageType.Error ? "error" : "warning",
                    message = message.message, file = message.file, line = message.line, column = message.column, assembly = assembly });
        }
        // Eight entries keep the worst-case JSON-escaped payload below the 256 KiB response cap.
        internal static Batch ReadAfter(long cursor) {
            lock (gate) {
                if (cursor < 0 || cursor > sequence) throw new ArgumentOutOfRangeException(nameof(cursor));
                long first = entries.Count == 0 ? sequence + 1 : entries.Peek().sequence;
                var result = new List<Entry>();
                foreach (var entry in entries) if (entry.sequence > cursor && result.Count < 8) result.Add(entry);
                return new Batch { entries = result.ToArray(), first = first, last = sequence,
                    next = result.Count == 0 ? cursor : result[result.Count - 1].sequence,
                    lost = cursor < first - 1 ? first - 1 - cursor : 0 };
            }
        }
    }
}
