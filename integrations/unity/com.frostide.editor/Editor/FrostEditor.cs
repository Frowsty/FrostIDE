// SPDX-License-Identifier: Apache-2.0
// Unity external editor registration and location-preserving FrostIDE launch.
using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using Unity.CodeEditor;
using UnityEditor;
using UnityEditor.Compilation;
using UnityEngine;

namespace FrostIDE.UnityIntegration
{
    [InitializeOnLoad]
    internal sealed class FrostEditor : IExternalCodeEditor
    {
        private string executable;

        static FrostEditor()
        {
            CodeEditor.Register(new FrostEditor());
            CompilationPipeline.compilationFinished += _ =>
            {
                if (CodeEditor.Editor.CurrentCodeEditor is FrostEditor) Synchronize();
            };
        }

        public CodeEditor.Installation[] Installations
        {
            get
            {
                string path = Environment.GetEnvironmentVariable("FROSTIDE_EXECUTABLE");
                return IsFrostExecutable(path)
                    ? new[] { new CodeEditor.Installation { Name = "FrostIDE", Path = path } }
                    : Array.Empty<CodeEditor.Installation>();
            }
        }

        public bool TryGetInstallationForPath(string editorPath, out CodeEditor.Installation installation)
        {
            installation = default;
            if (!IsFrostExecutable(editorPath)) return false;
            installation = new CodeEditor.Installation { Name = "FrostIDE", Path = editorPath };
            return true;
        }

        private static bool IsFrostExecutable(string path)
        {
            if (String.IsNullOrEmpty(path) || !Path.IsPathRooted(path) || !File.Exists(path)) return false;
            string name = Path.GetFileName(path);
            return String.Equals(name, "frost.exe", StringComparison.OrdinalIgnoreCase)
                || String.Equals(name, "frost", StringComparison.Ordinal);
        }

        public void Initialize(string editorPath) { executable = editorPath; }
        public void OnGUI() { }
        public void SyncAll() { Synchronize(); }
        private static void Synchronize()
        {
            try { ProjectContext.Sync(); }
            catch (Exception e) { UnityEngine.Debug.LogError("FrostIDE: project synchronization failed: " + e); }
        }
        public void SyncIfNeeded(string[] addedFiles, string[] deletedFiles, string[] movedFiles,
            string[] movedFromFiles, string[] importedFiles)
        {
            EditorApplication.delayCall -= Synchronize;
            EditorApplication.delayCall += Synchronize;
        }

        public bool OpenProject(string filePath = "", int line = -1, int column = -1)
        {
            try
            {
                if (!IsFrostExecutable(executable)) return false;
                string root = Directory.GetParent(Application.dataPath).FullName;
                var args = new StringBuilder();
                args.Append("--project ");
                args.Append(CodeEditor.QuoteForProcessStart(root));
                if (!String.IsNullOrEmpty(filePath))
                {
                    if (line > 0) args.Append(" --line ").Append(line);
                    if (column > 0) args.Append(" --column ").Append(column).Append(" --column-utf16");
                    args.Append(' ');
                    args.Append(CodeEditor.QuoteForProcessStart(ProjectContext.ResolveAssetPath(filePath)));
                }
                Process.Start(new ProcessStartInfo(executable, args.ToString()) { UseShellExecute = false });
                return true;
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException ||
                                      e is System.ComponentModel.Win32Exception || e is ArgumentException)
            {
                UnityEngine.Debug.LogError("FrostIDE: could not open project: " + e.Message);
                return false;
            }
        }
    }
}
