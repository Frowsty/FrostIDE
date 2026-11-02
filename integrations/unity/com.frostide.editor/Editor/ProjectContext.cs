// SPDX-License-Identifier: Apache-2.0
// Generate an isolated C# workspace from Unity's editor compilation assemblies.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Xml;
using UnityEditor;
using UnityEditor.Compilation;
using PackageInfo = UnityEditor.PackageManager.PackageInfo;
using UnityEngine;
using Assembly = UnityEditor.Compilation.Assembly;

namespace FrostIDE.UnityIntegration
{
    internal static class ProjectContext
    {
        private static string Root => Directory.GetParent(Application.dataPath).FullName;
        private static string Output => Path.Combine(Root, ".frost", "unity");

        private static string Id(string name)
        {
            using (var sha = SHA256.Create())
                return BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(name))).Replace("-", "").Substring(0, 32);
        }

        internal static string ResolveAssetPath(string path)
        {
            path = path.Replace('\\', '/');
            if (path.StartsWith("Packages/", StringComparison.Ordinal))
            {
                PackageInfo info = PackageInfo.FindForAssetPath(path);
                if (info != null)
                {
                    string prefix = "Packages/" + info.name;
                    if (path.Length == prefix.Length || path.StartsWith(prefix + "/", StringComparison.Ordinal))
                        return Path.GetFullPath(Path.Combine(info.resolvedPath, path.Substring(prefix.Length).TrimStart('/')));
                }
            }
            return Path.GetFullPath(Path.Combine(Root, path));
        }

        private const string Owner = "FrostIDE Unity project context v1";

        private static void RejectLink(string path)
        {
            try
            {
                // GetAttributes also examines dangling links on Unity's supported runtimes;
                // Exists alone returns false for them and is not an adequate link check.
                if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("Unity context path is a symbolic link: " + path);
            }
            catch (FileNotFoundException) { }
            catch (DirectoryNotFoundException) { }
        }

        private sealed class OwnedOutput : IDisposable
        {
            internal SafeControlFile.DirectoryPin Root, Parent, Output;
            public void Dispose() { Output?.Dispose(); Parent?.Dispose(); Root?.Dispose(); }
        }

        private static OwnedOutput PinOwnedOutput(string root)
        {
            var owned = new OwnedOutput();
            try
            {
                owned.Root = SafeControlFile.DirectoryPin.OpenRoot(root);
                owned.Parent = owned.Root.Child(".frost", true);
                bool existed = true;
                try { owned.Output = owned.Parent.Child("unity", false); }
                catch (IOException) { owned.Output = owned.Parent.Child("unity", true, true); existed = false; }
                string owner = Path.Combine(owned.Output.Path, "owner");
                RejectLink(owner);
                if (!existed)
                {
                    using (var stream = new FileStream(owner, FileMode.CreateNew, FileAccess.Write, FileShare.Read))
                    using (var writer = new StreamWriter(stream, new UTF8Encoding(false))) writer.Write(Owner);
                }
                if (SafeControlFile.ReadRegular(owner) != Owner)
                    throw new IOException("Refusing to modify an unowned Unity context directory: " + root);
                return owned;
            }
            catch { owned.Dispose(); throw; }
        }

        private static void EnsureOwnedOutput(string root)
        {
            using (PinOwnedOutput(root)) { }
        }

        // Write only changed generated files; never rewrite sources, assets, or Unity metadata.
        private static bool Write(string path, string contents)
        {
            RejectLink(path);
            string existing = SafeControlFile.ReadRegular(path);
            if (existing == contents) return false;
            string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                using (var writer = new StreamWriter(stream, new UTF8Encoding(false))) writer.Write(contents);
                RejectLink(path);
                if (File.Exists(path)) File.Replace(temporary, path, null);
                else File.Move(temporary, path);
                return true;
            }
            finally
            {
                if (File.Exists(temporary)) File.Delete(temporary);
            }
        }

        // MSBuild unescapes literals after XML parsing and treats several characters as
        // item separators or expressions. Escape them before handing text to XmlWriter.
        private static string MsBuildLiteral(string value)
        {
            const string special = "%$@'();?*";
            var escaped = new StringBuilder(value.Length);
            foreach (char character in value)
            {
                if (special.IndexOf(character) >= 0)
                    escaped.Append('%').Append(((int)character).ToString("X2"));
                else
                    escaped.Append(character);
            }
            return escaped.ToString();
        }

        // Unknown arguments can change language semantics; retain the previous generation
        // instead of publishing a silently inaccurate model.
        private static string CompilerWarnings(ScriptCompilerOptions options)
        {
            if (options.ResponseFiles != null && options.ResponseFiles.Length != 0)
                throw new InvalidOperationException("Unity response files are not yet supported by FrostIDE");
            var warnings = new List<string>();
            foreach (string argument in options.AdditionalCompilerArguments ?? Array.Empty<string>())
            {
                if (!(argument.StartsWith("/nowarn:", StringComparison.OrdinalIgnoreCase) ||
                      argument.StartsWith("-nowarn:", StringComparison.OrdinalIgnoreCase)))
                    throw new InvalidOperationException("Unsupported Unity compiler argument: " + argument);
                string value = argument.Substring(8);
                if (value.Length == 0 || value.Any(c => !Char.IsLetterOrDigit(c) && c != ',' && c != ';'))
                    throw new InvalidOperationException("Unsupported Unity warning list: " + argument);
                warnings.Add(value.Replace(',', ';'));
            }
            return String.Join(";", warnings);
        }

        internal static void Sync()
        {
            using (var owned = PinOwnedOutput(Root))
            {
            // A complete generation is made visible by one atomic revision update. The
            // running language server keeps its previous generation while this one is built.
            var files = new SortedDictionary<string, string>(StringComparer.Ordinal);
            files.Add("Directory.Build.props",
                "<Project><PropertyGroup><BaseIntermediateOutputPath>obj/$(MSBuildProjectName)/</BaseIntermediateOutputPath>" +
                "<MSBuildProjectExtensionsPath>obj/$(MSBuildProjectName)/</MSBuildProjectExtensionsPath>" +
                "<BaseOutputPath>bin/$(MSBuildProjectName)/</BaseOutputPath></PropertyGroup></Project>");
            Assembly[] assemblies = CompilationPipeline.GetAssemblies(AssembliesType.Editor);
            var byName = assemblies.ToDictionary(a => a.name, a => Id(a.name), StringComparer.Ordinal);
            foreach (Assembly assembly in assemblies)
            {
                string noWarn = CompilerWarnings(assembly.compilerOptions);
                bool editorOnly = (assembly.flags & AssemblyFlags.EditorAssembly) != 0;
                ApiCompatibilityLevel profile = assembly.compilerOptions.ApiCompatibilityLevel;
                if (editorOnly)
                    profile = assembly.compilerOptions.EditorAssembliesCompatibilityLevel == EditorAssembliesCompatibilityLevel.NET_Standard
                        ? ApiCompatibilityLevel.NET_Standard : ApiCompatibilityLevel.NET_Unity_4_8;
                var settings = new XmlWriterSettings { Indent = true, OmitXmlDeclaration = true };
                var text = new StringBuilder();
                using (XmlWriter xml = XmlWriter.Create(text, settings))
                {
                    xml.WriteStartElement("Project"); xml.WriteAttributeString("Sdk", "Microsoft.NET.Sdk");
                    xml.WriteStartElement("PropertyGroup");
                    xml.WriteElementString("TargetFramework", profile.ToString().StartsWith("NET_Standard", StringComparison.Ordinal)
                        ? "netstandard2.1" : "net48");
                    // Unity supplies the matching system references below. Avoid silently substituting
                    // the host SDK's broader API profile or requiring an unrelated targeting-pack download.
                    xml.WriteElementString("DisableImplicitFrameworkReferences", "true");
                    xml.WriteElementString("NoStdLib", "true");
                    xml.WriteElementString("AutomaticallyUseReferenceAssemblyPackages", "false");
                    xml.WriteElementString("EnableDefaultCompileItems", "false");
                    xml.WriteElementString("GenerateAssemblyInfo", "false");
                    if (!String.IsNullOrEmpty(noWarn)) xml.WriteElementString("NoWarn", noWarn);
                    xml.WriteElementString("AssemblyName", MsBuildLiteral(assembly.name));
                    xml.WriteElementString("DefineConstants", String.Join(";", assembly.defines));
                    xml.WriteElementString("AllowUnsafeBlocks", assembly.compilerOptions.AllowUnsafeCode ? "true" : "false");
                    if (!String.IsNullOrEmpty(assembly.compilerOptions.LanguageVersion))
                        xml.WriteElementString("LangVersion", assembly.compilerOptions.LanguageVersion);
                    xml.WriteEndElement();
                    xml.WriteStartElement("ItemGroup");
                    foreach (string file in assembly.sourceFiles.OrderBy(p => p, StringComparer.Ordinal))
                    {
                        xml.WriteStartElement("Compile"); xml.WriteAttributeString("Include", MsBuildLiteral(ResolveAssetPath(file))); xml.WriteEndElement();
                    }
                    foreach (Assembly reference in assembly.assemblyReferences.OrderBy(a => a.name, StringComparer.Ordinal))
                    {
                        if (!byName.TryGetValue(reference.name, out string id))
                            throw new InvalidOperationException("Unity assembly reference has no source project: " + reference.name);
                        xml.WriteStartElement("ProjectReference"); xml.WriteAttributeString("Include", id + ".csproj");
                        // Unity permits editor assemblies to reference player assemblies even when
                        // their compatibility profiles differ; do not let MSBuild reject that graph.
                        xml.WriteAttributeString("SkipGetTargetFrameworkProperties", "true"); xml.WriteEndElement();
                    }
                    var references = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                    var literalReferences = new List<string>();
                    void AddReference(string reference)
                    {
                        string absolute = ResolveAssetPath(reference);
                        string name = Path.GetFileNameWithoutExtension(absolute);
                        if (references.TryGetValue(name, out string previous))
                        {
                            if (!String.Equals(previous, absolute, Application.platform == RuntimePlatform.WindowsEditor
                                    ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
                                throw new InvalidOperationException("Ambiguous Unity assembly reference: " + name);
                            return;
                        }
                        references.Add(name, absolute);
                        // RAR and the C# task each unescape their inputs. A metacharacter in
                        // a real path can therefore be decoded twice; bypass RAR for those
                        // explicit Unity references and preserve their original paths below.
                        if (absolute.IndexOfAny("%$@'();?*".ToCharArray()) >= 0)
                        {
                            literalReferences.Add(absolute);
                            return;
                        }
                        xml.WriteStartElement("Reference"); xml.WriteAttributeString("Include", MsBuildLiteral(name));
                        xml.WriteElementString("HintPath", MsBuildLiteral(absolute)); xml.WriteEndElement();
                    }
                    foreach (string reference in assembly.compiledAssemblyReferences.OrderBy(p => p, StringComparer.Ordinal))
                        AddReference(reference);
                    xml.WriteEndElement();
                    if (literalReferences.Count != 0)
                    {
                        xml.WriteStartElement("Target");
                        xml.WriteAttributeString("Name", "FrostUnityLiteralReferences");
                        xml.WriteAttributeString("AfterTargets", "ResolveAssemblyReferences");
                        xml.WriteStartElement("ItemGroup");
                        foreach (string reference in literalReferences)
                        {
                            xml.WriteStartElement("ReferencePath");
                            xml.WriteAttributeString("Include", MsBuildLiteral(MsBuildLiteral(reference)));
                            xml.WriteElementString("ReferenceSourceTarget", "ResolveAssemblyReference");
                            xml.WriteEndElement();
                        }
                        xml.WriteEndElement(); xml.WriteEndElement();
                    }
                    xml.WriteEndElement();
                }
                files.Add(byName[assembly.name] + ".csproj", text.ToString());
            }
            var solution = new StringBuilder("\r\nMicrosoft Visual Studio Solution File, Format Version 12.00\r\n");
            foreach (Assembly assembly in assemblies.OrderBy(a => a.name, StringComparer.Ordinal))
            {
                string id = byName[assembly.name];
                string guid = new Guid(id).ToString("B").ToUpperInvariant();
                solution.Append("Project(\"{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}\") = \"")
                    .Append(id).Append("\", \"").Append(id).Append(".csproj\", \"").Append(guid)
                    .Append("\"\r\nEndProject\r\n");
            }
            solution.Append("Global\r\n\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\r\n")
                .Append("\t\tDebug|Any CPU = Debug|Any CPU\r\n\tEndGlobalSection\r\n")
                .Append("\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\r\n");
            foreach (Assembly assembly in assemblies.OrderBy(a => a.name, StringComparer.Ordinal))
            {
                string guid = new Guid(byName[assembly.name]).ToString("B").ToUpperInvariant();
                solution.Append("\t\t").Append(guid).Append(".Debug|Any CPU.ActiveCfg = Debug|Any CPU\r\n")
                    .Append("\t\t").Append(guid).Append(".Debug|Any CPU.Build.0 = Debug|Any CPU\r\n");
            }
            solution.Append("\tEndGlobalSection\r\nEndGlobal\r\n");
            files.Add("FrostUnity.sln", solution.ToString());
            PublishPinned(owned.Output, files, null);
            }
        }

        // The optional callback is a deterministic fault seam; publication never exposes
        // a new descriptor until all writes in its fresh generation have completed.
        internal static void Publish(string output, SortedDictionary<string, string> files, Action<string> beforeWrite)
        {
            string full = Path.GetFullPath(output);
            using (var parent = SafeControlFile.DirectoryPin.OpenRoot(Path.GetDirectoryName(full)))
            using (var pinned = parent.Child(Path.GetFileName(full), true))
                PublishPinned(pinned, files, beforeWrite);
        }

        private static void PublishPinned(SafeControlFile.DirectoryPin outputPin, SortedDictionary<string, string> files,
                                          Action<string> beforeWrite)
        {
            string output = outputPin.Path;
            var fingerprint = new StringBuilder();
            foreach (var file in files)
                fingerprint.Append(file.Key.Length).Append(':').Append(file.Key)
                    .Append(file.Value.Length).Append(':').Append(file.Value);
            string digest = Id(fingerprint.ToString()).ToLowerInvariant();
            string revision = Path.Combine(output, "revision");
            RejectLink(revision);
            string current = SafeControlFile.ReadRegular(revision);
            string generationsPath = Path.Combine(output, "generations");
            RejectLink(generationsPath);
            using (var generations = outputPin.Child("generations", true))
            {
                if (current != null && current.Length == 67 && current.StartsWith("1:", StringComparison.Ordinal) &&
                    current[34] == ':' && current.Substring(2, 32).All(c => (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) &&
                    current.Substring(35) == digest)
                {
                    string existingId = current.Substring(2, 32);
                    string existingPath = Path.Combine(generations.Path, existingId);
                    RejectLink(existingPath);
                    if (Directory.Exists(existingPath))
                    {
                        using (var existing = generations.Child(existingId, false))
                        {
                            string solution = Path.Combine(existing.Path, "FrostUnity.sln");
                            RejectLink(solution);
                            if (SafeControlFile.ReadRegular(solution, false) != null)
                                return;
                        }
                    }
                }
                string generation = Guid.NewGuid().ToString("N");
                using (var generationDirectory = generations.Child(generation, true, true))
                {
                    foreach (var file in files)
                    {
                        beforeWrite?.Invoke(file.Key);
                        Write(Path.Combine(generationDirectory.Path, file.Key), file.Value);
                    }
                }
                Write(revision, "1:" + generation + ":" + digest);
            }
        }
    }
}
