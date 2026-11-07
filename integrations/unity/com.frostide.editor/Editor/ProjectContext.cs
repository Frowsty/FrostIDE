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
        internal const string UnsupportedArgumentsFile = "UnsupportedArguments.txt";

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

        /*
         * The csc options of an assembly's response files (csc.rsp) and AdditionalCompilerArguments
         * that change the language model: -define/-d, -nowarn, -unsafe[+|-], -langversion,
         * -nullable[+|-|:value], -reference/-r and -warnaserror[+|-|:list]. Anything else is kept
         * in Unsupported, published with the generation (UnsupportedArguments.txt) so the IDE can
         * name the options it does not model, instead of refusing the whole project.
         */
        internal sealed class CompilerArguments
        {
            internal readonly List<string> Defines = new List<string>();
            internal readonly List<string> NoWarn = new List<string>();
            internal readonly List<string> WarningsAsErrors = new List<string>();
            internal readonly List<string> References = new List<string>();
            internal readonly List<string> Unsupported = new List<string>();
            internal bool? Unsafe;
            internal bool? TreatWarningsAsErrors;
            internal string LangVersion;
            internal string Nullable;
        }

        // Splits a response file as csc does: whitespace separates arguments, double quotes group
        // (and are dropped), and a line whose first non-blank character is '#' is a comment.
        internal static List<string> SplitResponseFile(string text)
        {
            var arguments = new List<string>();
            foreach (string rawLine in text.Replace("\r\n", "\n").Split('\n'))
            {
                string line = rawLine.Trim();
                if (line.Length == 0 || line[0] == '#') continue;
                var current = new StringBuilder();
                bool quoted = false, any = false;
                foreach (char c in line)
                {
                    if (c == '"') { quoted = !quoted; any = true; continue; }
                    if (!quoted && Char.IsWhiteSpace(c))
                    {
                        if (any) arguments.Add(current.ToString());
                        current.Clear(); any = false;
                        continue;
                    }
                    current.Append(c); any = true;
                }
                if (any) arguments.Add(current.ToString());
            }
            return arguments;
        }

        private static bool IsIdentifier(string value) =>
            value.Length != 0 && (Char.IsLetter(value[0]) || value[0] == '_') && value.All(c => Char.IsLetterOrDigit(c) || c == '_');

        private static IEnumerable<string> ListValues(string value) =>
            value.Split(new[] { ';', ',' }, StringSplitOptions.RemoveEmptyEntries).Select(v => v.Trim()).Where(v => v.Length != 0);

        // Adds one csc argument to `into`; see CompilerArguments.
        internal static void ParseCompilerArgument(string argument, CompilerArguments into)
        {
            if (argument == null || argument.Length < 2 || (argument[0] != '-' && argument[0] != '/'))
            {
                if (!String.IsNullOrEmpty(argument)) into.Unsupported.Add(argument);
                return;
            }
            string body = argument.Substring(1);
            int colon = body.IndexOf(':');
            string name = (colon < 0 ? body : body.Substring(0, colon)).ToLowerInvariant();
            string value = colon < 0 ? null : body.Substring(colon + 1).Trim('"');
            bool? toggle = null;
            if (name.EndsWith("+", StringComparison.Ordinal) || name.EndsWith("-", StringComparison.Ordinal))
            {
                toggle = name[name.Length - 1] == '+';
                name = name.Substring(0, name.Length - 1);
            }
            switch (name)
            {
                case "define":
                case "d":
                {
                    var symbols = value == null ? new List<string>() : ListValues(value).ToList();
                    if (toggle != null || symbols.Count == 0 || !symbols.All(IsIdentifier)) break;
                    foreach (string symbol in symbols)
                        if (!into.Defines.Contains(symbol)) into.Defines.Add(symbol);
                    return;
                }
                case "nowarn":
                {
                    var codes = value == null ? new List<string>() : ListValues(value).ToList();
                    if (toggle != null || codes.Count == 0 || !codes.All(c => c.All(Char.IsLetterOrDigit))) break;
                    foreach (string code in codes)
                        if (!into.NoWarn.Contains(code)) into.NoWarn.Add(code);
                    return;
                }
                case "unsafe":
                    if (value != null) break;
                    into.Unsafe = toggle ?? true;
                    return;
                case "langversion":
                    if (toggle != null || String.IsNullOrEmpty(value) || !value.All(c => Char.IsLetterOrDigit(c) || c == '.')) break;
                    into.LangVersion = value;
                    return;
                case "nullable":
                {
                    string mode = value == null ? (toggle == false ? "disable" : "enable") : value.ToLowerInvariant();
                    if (value != null && toggle != null) break;
                    if (mode != "enable" && mode != "disable" && mode != "warnings" && mode != "annotations") break;
                    into.Nullable = mode;
                    return;
                }
                case "reference":
                case "r":
                {
                    var paths = value == null ? new List<string>() : value.Split(';').Select(p => p.Trim().Trim('"')).Where(p => p.Length != 0).ToList();
                    // An extern alias ("-r:Alias=path") changes name binding; it is not modelled.
                    if (toggle != null || paths.Count == 0 || paths.Any(p => p.IndexOf('=') >= 0)) break;
                    into.References.AddRange(paths);
                    return;
                }
                case "warnaserror":
                    if (value == null)
                    {
                        into.TreatWarningsAsErrors = toggle ?? true;
                        return;
                    }
                    if (toggle == false) break; // -warnaserror-:list (exclusions) is not modelled.
                    var errors = ListValues(value).ToList();
                    if (errors.Count == 0 || !errors.All(c => c.All(Char.IsLetterOrDigit))) break;
                    foreach (string code in errors)
                        if (!into.WarningsAsErrors.Contains(code)) into.WarningsAsErrors.Add(code);
                    return;
            }
            into.Unsupported.Add(argument);
        }

        // The arguments of `options`: its response files in order (paths relative to the project,
        // as Unity resolves them), then AdditionalCompilerArguments. A missing response file is
        // recorded as unsupported; Unity reports it when it compiles.
        internal static CompilerArguments ReadCompilerArguments(ScriptCompilerOptions options)
        {
            var parsed = new CompilerArguments();
            foreach (string file in options.ResponseFiles ?? Array.Empty<string>())
            {
                string path = ResolveAssetPath(file);
                if (!File.Exists(path))
                {
                    parsed.Unsupported.Add("@" + file + " (response file not found)");
                    continue;
                }
                foreach (string argument in SplitResponseFile(File.ReadAllText(path)))
                {
                    if (argument.StartsWith("@", StringComparison.Ordinal)) parsed.Unsupported.Add(argument);
                    else ParseCompilerArgument(argument, parsed);
                }
            }
            foreach (string argument in options.AdditionalCompilerArguments ?? Array.Empty<string>())
                ParseCompilerArgument(argument, parsed);
            return parsed;
        }

        internal static void Sync()
        {
            using (var owned = PinOwnedOutput(Root))
            {
            // A complete generation is made visible by one atomic revision update. The
            // running language server keeps its previous generation while this one is built.
            PublishPinned(owned.Output, GenerateFiles(), null);
            }
        }

        // The files of one generation (Directory.Build.props, a .csproj per Unity assembly,
        // FrostUnity.sln and UnsupportedArguments.txt when needed), by file name. Reads Unity's
        // compilation pipeline and the assemblies' response files; writes nothing.
        internal static SortedDictionary<string, string> GenerateFiles()
        {
            var files = new SortedDictionary<string, string>(StringComparer.Ordinal);
            files.Add("Directory.Build.props",
                "<Project><PropertyGroup><BaseIntermediateOutputPath>obj/$(MSBuildProjectName)/</BaseIntermediateOutputPath>" +
                "<MSBuildProjectExtensionsPath>obj/$(MSBuildProjectName)/</MSBuildProjectExtensionsPath>" +
                "<BaseOutputPath>bin/$(MSBuildProjectName)/</BaseOutputPath></PropertyGroup></Project>");
            Assembly[] assemblies = CompilationPipeline.GetAssemblies(AssembliesType.Editor);
            var byName = assemblies.ToDictionary(a => a.name, a => Id(a.name), StringComparer.Ordinal);
            var unsupported = new StringBuilder();
            foreach (Assembly assembly in assemblies)
            {
                CompilerArguments arguments = ReadCompilerArguments(assembly.compilerOptions);
                foreach (string argument in arguments.Unsupported)
                    unsupported.Append(assembly.name).Append('\t').Append(argument.Replace('\r', ' ').Replace('\n', ' ')).Append('\n');
                string noWarn = String.Join(";", arguments.NoWarn);
                var defines = new List<string>(assembly.defines ?? Array.Empty<string>());
                foreach (string symbol in arguments.Defines)
                    if (!defines.Contains(symbol)) defines.Add(symbol);
                bool allowUnsafe = arguments.Unsafe ?? assembly.compilerOptions.AllowUnsafeCode;
                string langVersion = arguments.LangVersion ?? assembly.compilerOptions.LanguageVersion;
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
                    xml.WriteElementString("DefineConstants", String.Join(";", defines));
                    xml.WriteElementString("AllowUnsafeBlocks", allowUnsafe ? "true" : "false");
                    if (!String.IsNullOrEmpty(langVersion))
                        xml.WriteElementString("LangVersion", MsBuildLiteral(langVersion));
                    if (arguments.Nullable != null) xml.WriteElementString("Nullable", arguments.Nullable);
                    if (arguments.TreatWarningsAsErrors != null)
                        xml.WriteElementString("TreatWarningsAsErrors", arguments.TreatWarningsAsErrors.Value ? "true" : "false");
                    if (arguments.WarningsAsErrors.Count != 0)
                        xml.WriteElementString("WarningsAsErrors", String.Join(";", arguments.WarningsAsErrors));
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
                    // -r from csc.rsp / AdditionalCompilerArguments, after Unity's own references
                    // (Unity usually already passes a framework assembly named this way). A path
                    // that is not a file next to the project (a bare framework name Unity resolves
                    // itself) is reported as unsupported rather than guessed.
                    foreach (string reference in arguments.References)
                    {
                        string absolute = ResolveAssetPath(reference);
                        if (references.ContainsKey(Path.GetFileNameWithoutExtension(absolute))) continue;
                        if (File.Exists(absolute)) AddReference(reference);
                        else unsupported.Append(assembly.name).Append("\t-r:").Append(reference).Append(" (reference not found)\n");
                    }
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
            // One "assembly<TAB>argument" line per compiler option FrostIDE does not model, so the
            // IDE can say which options its C# model ignores.
            if (unsupported.Length != 0)
                files.Add(UnsupportedArgumentsFile, unsupported.ToString());
            return files;
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
