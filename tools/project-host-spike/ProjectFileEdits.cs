// SPDX-License-Identifier: Apache-2.0
// SLN-6: adding, removing and renaming file items of a .vcxproj and its .vcxproj.filters through the
// MSBuild construction API (ProjectRootElement, preserveFormatting), never through text or regex
// edits. MSBuild's own evaluation of every declared configuration first shows that the edit means
// exactly what it says; TransactionProbe then commits both files with its recovery journal.
/*
 * Request (one JSON line of strings; frost::project::projectFileEditRequest builds it):
 *   Project (absolute .vcxproj), Root (the trusted root), Trusted="true", Commit="true",
 *   TransactionId (a GUID), Configuration and Platform (the solution's active pair; informative),
 *   and exactly one edit:
 *     Add=<abs>|<abs>|...  with Filter=<a filter the .filters file defines, or "" for none>
 *     Remove=<abs>|...
 *     RenameFrom=<abs>|... with RenameTo=<abs>|... (pairwise; the files were renamed on disk)
 *   '|' separates the paths because Windows forbids it in file names.
 * Without Commit the plan is computed and nothing is written ({"Items":null,"Edit":{...}}).
 *
 * What is refused (nothing is written, the answer's Error says why):
 *   - a path outside the trusted root, or with a character MSBuild treats specially (; % $ @ * ?);
 *   - Remove/RenameFrom of a file that, in any declared configuration, is not exactly one evaluated
 *     item defined directly in the project by a literal, unconditional Include outside a target
 *     (a wildcard, an import's item, a conditional item or group, Exclude/Remove/Update);
 *   - Add/RenameTo of a file that any configuration already includes, or a missing file;
 *   - configuration declarations that are conditional, imported or not literal (as before SLN-6);
 *   - an Add filter the .filters file does not define;
 *   - a definition whose line endings mix CRLF and LF, or that is not strict UTF-8.
 *
 * Placement is MSBuild's: ProjectRootElement.AddItem puts a new item into the first unconditional
 * item group that has items of its type (in include order when the group is sorted) or into a new
 * group; .filters entries get <Filter> metadata. A removed item's group is removed with it when it
 * becomes empty and has no attributes. Renames change only the Include attribute (the item type and
 * metadata stay, as in Visual Studio). The XML declaration, byte order mark and line-ending style
 * of each file are kept, and the saved text is read back with the construction API to check that
 * its items are exactly the intended ones.
 */
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;
using System.Xml;
using Microsoft.Build.Construction;
using Microsoft.Build.Evaluation;

// One project-file edit as the request states it, paths absolute and normalised.
sealed class FileEditRequest
{
    public string ProjectPath;
    public string[] Added = new string[0];
    public string Filter = "";
    public bool EarlyProperties;
    public string Configuration;
    public string Platform;
    public Dictionary<string, string> Properties = new Dictionary<string, string>();
    public string[] Removed = new string[0];
    public string[] RenamedFrom = new string[0];
    public string[] RenamedTo = new string[0];
    public bool Adding { get { return Added.Length != 0; } }
    // The files the project lists now and must list exactly once (Remove, RenameFrom).
    public IEnumerable<string> Existing { get { return Removed.Concat(RenamedFrom); } }
    // The files the project must not list yet (Add, RenameTo).
    public IEnumerable<string> Incoming { get { return Added.Concat(RenamedTo); } }
}

static class ProjectFileEdits
{
    // The file item types FrostIDE shows; equal to kItemTypes in src/project/src/vcxproj_reader.cpp.
    static readonly string[] FileItemTypes = {
        "ClCompile", "ClInclude", "None", "ResourceCompile", "Text", "CustomBuild", "Image", "Natvis",
        "MASM", "Midl", "FxCompile", "Manifest", "CopyFileToFolders", "Xml"
    };
    static readonly char[] Special = { ';', '%', '$', '@', '*', '?', '\0', '\r', '\n', '|' };

    // The item type Visual Studio gives a new file of this extension.
    internal static string ItemTypeFor(string path)
    {
        switch (Path.GetExtension(path).ToLowerInvariant())
        {
            case ".c": case ".cc": case ".cpp": case ".cxx": case ".c++": case ".ixx": case ".cppm": return "ClCompile";
            case ".h": case ".hh": case ".hpp": case ".hxx": case ".h++": case ".inl": case ".ipp": case ".tpp": return "ClInclude";
            case ".rc": return "ResourceCompile";
            case ".ico": case ".bmp": case ".png": case ".jpg": case ".jpeg": case ".gif": case ".cur": return "Image";
            case ".txt": return "Text";
            case ".natvis": return "Natvis";
            case ".asm": return "MASM";
            case ".idl": return "Midl";
            case ".hlsl": return "FxCompile";
            case ".manifest": return "Manifest";
            case ".xml": return "Xml";
            default: return "None";
        }
    }

    static bool IsFileItemType(string type)
    {
        return FileItemTypes.Contains(type, StringComparer.OrdinalIgnoreCase);
    }

    static string[] Paths(Dictionary<string, string> request, string key)
    {
        string value;
        if (!request.TryGetValue(key, out value) || value == null) return new string[0];
        var paths = value.Split('|');
        foreach (string path in paths)
            if (!Path.IsPathRooted(path) || path.IndexOfAny(Special) >= 0)
                throw new InvalidOperationException("Choose files with absolute paths and no ; % $ @ * ? characters: " + path);
        return paths.Select(Path.GetFullPath).ToArray();
    }

    internal static FileEditRequest Parse(Dictionary<string, string> request, string projectPath)
    {
        var edit = new FileEditRequest {
            ProjectPath = Path.GetFullPath(projectPath), Added = Paths(request, "Add"), Removed = Paths(request, "Remove"),
            RenamedFrom = Paths(request, "RenameFrom"), RenamedTo = Paths(request, "RenameTo")
        };
        string filter;
        if (request.TryGetValue("Filter", out filter) && filter != null)
        {
            if (filter.IndexOfAny(Special) >= 0) throw new InvalidOperationException("Invalid filter name");
            edit.Filter = filter;
        }
        foreach (var pair in request.Where(p => p.Key.StartsWith("Set:", StringComparison.Ordinal)))
        {
            string key = pair.Key.Substring(4);
            string[] parts = key.Split('.');
            if (parts.Length != 2) throw new InvalidOperationException("Invalid property key");
            foreach (string part in parts) XmlConvert.VerifyNCName(part);
            edit.Properties.Add(key, pair.Value ?? "");
        }
        if (edit.Properties.Count != 0)
        {
            string early;
            edit.EarlyProperties = request.TryGetValue("EarlyProperties", out early) && early == "true";
            edit.Configuration = request["Configuration"];
            edit.Platform = request["Platform"];
            if (new[] { edit.Configuration, edit.Platform }.Any(v => String.IsNullOrEmpty(v) || v.IndexOfAny(new[] { '\'', '$', '|', '\r', '\n' }) >= 0))
                throw new InvalidOperationException("Choose a literal configuration and platform");
        }
        int kinds = (edit.Properties.Count != 0 ? 1 : 0) + (edit.Added.Length != 0 ? 1 : 0) + (edit.Removed.Length != 0 ? 1 : 0) + (edit.RenamedFrom.Length != 0 ? 1 : 0);
        if (kinds != 1 || edit.RenamedFrom.Length != edit.RenamedTo.Length || (edit.Filter.Length != 0 && !edit.Adding))
            throw new InvalidOperationException("Choose exactly one edit: add, remove or rename");
        var all = edit.Existing.Concat(edit.Incoming).ToArray();
        if (all.Distinct(StringComparer.OrdinalIgnoreCase).Count() != all.Length)
            throw new InvalidOperationException("A file is named twice in the edit");
        return edit;
    }

    static string FullPathOf(string projectDirectory, string include)
    {
        return Path.GetFullPath(Path.IsPathRooted(include) ? include : Path.Combine(projectDirectory, include));
    }

    // The include Visual Studio writes for `path`: relative to the project directory ("..\src\a.cpp")
    // on the same volume, absolute otherwise; `absoluteStyle` keeps an absolute include absolute.
    internal static string IncludeFor(string projectDirectory, string path, bool absoluteStyle)
    {
        string full = Path.GetFullPath(path);
        string directory = Path.GetFullPath(projectDirectory).TrimEnd('\\');
        if (absoluteStyle || !String.Equals(Path.GetPathRoot(full), Path.GetPathRoot(directory), StringComparison.OrdinalIgnoreCase))
            return full;
        string[] from = directory.Split('\\'), to = full.Split('\\');
        int common = 0;
        while (common < from.Length && common < to.Length - 1 &&
               String.Equals(from[common], to[common], StringComparison.OrdinalIgnoreCase))
            common++;
        var parts = Enumerable.Repeat("..", from.Length - common).Concat(to.Skip(common));
        return String.Join("\\", parts);
    }

    static bool InTarget(ProjectElement element)
    {
        for (var parent = element.Parent; parent != null; parent = parent.Parent)
            if (parent is ProjectTargetElement) return true;
        return false;
    }

    static bool Literal(string include)
    {
        return !String.IsNullOrEmpty(include) && include.IndexOfAny(Special) < 0;
    }

    /*
     * The checks of one configuration's evaluation: every Existing file is exactly one evaluated
     * file item, defined directly in the project by a literal, unconditional element of an
     * unconditional top-level item group; no Incoming file is evaluated as an item yet.
     */
    internal static void CheckConfiguration(Project project, FileEditRequest edit)
    {
        // Check the requested pair in both evaluations (including the import-fenced pass).
        // A hook supplied by Directory.Build.props is not visible in the main project's XML.
        if (String.Equals(project.GetPropertyValue("Configuration"), edit.Configuration, StringComparison.OrdinalIgnoreCase) &&
            String.Equals(project.GetPropertyValue("Platform"), edit.Platform, StringComparison.OrdinalIgnoreCase))
        {
            foreach (string name in new[] { "ForceImportAfterCppProps", "ForceImportAfterCppTargets" })
            {
                var requested = edit.Properties.FirstOrDefault(p => String.Equals(p.Key, "Prop." + name, StringComparison.OrdinalIgnoreCase));
                if (requested.Key == null) continue;
                string current = project.GetPropertyValue(name);
                if (!String.IsNullOrWhiteSpace(current) && !String.Equals(current, requested.Value, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("Refusing to overwrite evaluated import hook " + name + "; preserve its existing import chain manually");
            }
        }
        string directory = Path.GetDirectoryName(edit.ProjectPath);
        var byPath = new Dictionary<string, List<ProjectItem>>(StringComparer.OrdinalIgnoreCase);
        foreach (var item in project.Items.Where(i => IsFileItemType(i.ItemType)))
        {
            string full;
            try { full = FullPathOf(directory, item.EvaluatedInclude); }
            catch (ArgumentException) { continue; }
            catch (NotSupportedException) { continue; }
            List<ProjectItem> list;
            if (!byPath.TryGetValue(full, out list)) byPath[full] = list = new List<ProjectItem>();
            list.Add(item);
        }
        foreach (string path in edit.Existing)
        {
            List<ProjectItem> matches;
            if (!byPath.TryGetValue(path, out matches) || matches.Count != 1)
                throw new InvalidOperationException("The project does not list " + Path.GetFileName(path) +
                                                    " exactly once in every configuration");
            var xml = matches[0].Xml;
            if (!String.Equals(xml.ContainingProject.FullPath, edit.ProjectPath, StringComparison.OrdinalIgnoreCase) ||
                !Literal(xml.Include) || !String.Equals(FullPathOf(directory, xml.Include), path, StringComparison.OrdinalIgnoreCase) ||
                xml.Condition.Length != 0 || xml.Parent.Condition.Length != 0 || !(xml.Parent.Parent is ProjectRootElement) ||
                xml.Exclude.Length != 0 || xml.Remove.Length != 0 || xml.Update.Length != 0)
                throw new InvalidOperationException(Path.GetFileName(path) + " comes from a wildcard, an import or a " +
                                                    "conditional item; edit it in Visual Studio or by hand");
        }
        foreach (string path in edit.Incoming)
            if (byPath.ContainsKey(path))
                throw new InvalidOperationException("The project already includes " + Path.GetFileName(path));
    }

    sealed class Utf8StringWriter : StringWriter
    {
        public override Encoding Encoding { get { return new UTF8Encoding(false); } }
    }

    // The (type, include, metadata) of every item of `root` outside targets, in document order.
    static List<string> Signature(ProjectRootElement root)
    {
        return root.Items.Where(i => !InTarget(i)).Select(i => i.ItemType + "\u0001" + i.Include + "\u0001" +
            String.Join("\u0002", i.Metadata.Select(m => m.Name + "=" + m.Value))).ToList();
    }

    /*
     * The planned bytes of one definition (`filters`: the .vcxproj.filters file). The text is decoded
     * as strict UTF-8 (a BOM is kept), parsed by ProjectRootElement with preserveFormatting, edited,
     * saved, given the original line endings and checked by reading it back.
     */
    internal static byte[] Plan(byte[] bytes, FileEditRequest edit, bool filters)
    {
        bool bom = bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF;
        string text = new UTF8Encoding(false, true).GetString(bytes, bom ? 3 : 0, bytes.Length - (bom ? 3 : 0));
        int crlf = 0, lf = 0;
        for (int i = 0; i < text.Length; i++)
            if (text[i] == '\n') { if (i > 0 && text[i - 1] == '\r') crlf++; else lf++; }
        if (crlf != 0 && lf != 0) throw new InvalidOperationException("Mixed line endings in " + Path.GetFileName(edit.ProjectPath) + (filters ? ".filters" : ""));
        if (text.Replace("\r\n", "").IndexOf('\r') >= 0) throw new InvalidOperationException("Bare carriage returns in a definition");
        string directory = Path.GetDirectoryName(edit.ProjectPath);
        using (var collection = new ProjectCollection())
        {
            var settings = new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null };
            ProjectRootElement root;
            using (var reader = XmlReader.Create(new StringReader(text), settings))
                root = ProjectRootElement.Create(reader, collection, true);
            var expected = Signature(root);
            if (!filters && edit.Properties.Count != 0)
            {
                string condition = "'$(Configuration)|$(Platform)'=='" + edit.Configuration + "|" + edit.Platform + "'";
                // Selectors must exist before the imports that consume them. Insert after existing
                // configuration groups, not before them, so this edit is the final local value.
                var groups = new Dictionary<string, ProjectPropertyGroupElement>();
                foreach (var pair in edit.Properties.Where(p => p.Key.StartsWith("Prop.", StringComparison.Ordinal)))
                {
                    string name = pair.Key.Substring(5);
                    string phase = edit.EarlyProperties ? "Microsoft.Cpp.props" : "";
                    if (new[] { "WindowsTargetPlatformVersion", "WindowsSDKVersion" }.Contains(name, StringComparer.OrdinalIgnoreCase))
                        phase = "Microsoft.Cpp.Default.props";
                    else if (new[] { "ConfigurationType", "PlatformToolset", "CharacterSet", "WholeProgramOptimization" }.Contains(name, StringComparer.OrdinalIgnoreCase))
                        phase = "Microsoft.Cpp.props";
                    ProjectPropertyGroupElement group;
                    if (!groups.TryGetValue(phase, out group))
                    {
                        group = root.AddPropertyGroup();
                        group.Condition = condition;
                        group.Label = "FrostIDE Property Pages";
                        if (phase.Length != 0)
                        {
                            var import = root.Imports.FirstOrDefault(i => i.Parent == root &&
                                i.Project.Replace('/', '\\').EndsWith("\\" + phase, StringComparison.OrdinalIgnoreCase));
                            if (import == null)
                                throw new InvalidOperationException("Property edit requires a direct " + phase + " import");
                            root.RemoveChild(group);
                            root.InsertBeforeChild(group, import);
                        }
                        groups.Add(phase, group);
                    }
                    group.AddProperty(name, pair.Value);
                }
                var definitions = root.AddItemDefinitionGroup();
                definitions.Condition = condition;
                definitions.Label = "FrostIDE Property Pages";
                foreach (var pair in edit.Properties)
                {
                    string[] key = pair.Key.Split('.');
                    if (key[0] != "Prop")
                    {
                        var definition = definitions.ItemDefinitions.FirstOrDefault(d => d.ItemType == key[0])
                                         ?? definitions.AddItemDefinition(key[0]);
                        definition.AddMetadata(key[1], pair.Value);
                    }
                }
                if (definitions.Count == 0) root.RemoveChild(definitions);
            }
            var elements = root.Items.Where(i => !InTarget(i)).ToList();
            Func<string, List<ProjectItemElement>> listing = path => elements.Where(i => Literal(i.Include) &&
                String.Equals(FullPathOf(directory, i.Include), path, StringComparison.OrdinalIgnoreCase)).ToList();
            foreach (string path in edit.Removed)
            {
                var found = listing(path);
                if (!filters && found.Count != 1) throw new InvalidOperationException("Expected one project item for " + Path.GetFileName(path));
                foreach (var element in found)
                {
                    expected.Remove(Signature(element));
                    var group = element.Parent;
                    group.RemoveChild(element);
                    if (group.Count == 0 && group.Condition.Length == 0 && group.Label.Length == 0 && group.Parent is ProjectRootElement)
                        group.Parent.RemoveChild(group);
                }
            }
            for (int r = 0; r < edit.RenamedFrom.Length; r++)
            {
                var found = listing(edit.RenamedFrom[r]);
                if (!filters && found.Count != 1) throw new InvalidOperationException("Expected one project item for " + Path.GetFileName(edit.RenamedFrom[r]));
                foreach (var element in found)
                {
                    int at = expected.IndexOf(Signature(element));
                    element.Include = IncludeFor(directory, edit.RenamedTo[r], Path.IsPathRooted(element.Include));
                    expected[at] = Signature(element);
                }
            }
            foreach (string path in edit.Added)
            {
                if (listing(path).Count != 0) throw new InvalidOperationException("The definition already lists " + Path.GetFileName(path));
                string include = IncludeFor(directory, path, false);
                var metadata = new List<KeyValuePair<string, string>>();
                if (filters && edit.Filter.Length != 0)
                {
                    if (!root.Items.Any(i => i.ItemType == "Filter" && String.Equals(i.Include, edit.Filter, StringComparison.OrdinalIgnoreCase)))
                        throw new InvalidOperationException("The filters file defines no filter '" + edit.Filter + "'");
                    metadata.Add(new KeyValuePair<string, string>("Filter", edit.Filter));
                }
                var added = root.AddItem(ItemTypeFor(path), include, metadata);
                expected.Add(Signature(added));
            }
            var writer = new Utf8StringWriter();
            root.Save(writer);
            string output = writer.ToString();
            // XML parsing turns CRLF into LF; give the file its own line endings back.
            output = output.Replace("\r\n", "\n");
            if (crlf != 0) output = output.Replace("\n", "\r\n");
            using (var reader = XmlReader.Create(new StringReader(output), settings))
            using (var checkCollection = new ProjectCollection())
            {
                var check = Signature(ProjectRootElement.Create(reader, checkCollection, true));
                if (!check.OrderBy(s => s, StringComparer.Ordinal).SequenceEqual(expected.OrderBy(s => s, StringComparer.Ordinal)))
                    throw new InvalidOperationException("The saved definition does not have the intended items");
            }
            byte[] encoded = new UTF8Encoding(false, true).GetBytes(output);
            return bom ? new byte[] { 0xEF, 0xBB, 0xBF }.Concat(encoded).ToArray() : encoded;
        }
    }

    static string Signature(ProjectItemElement i)
    {
        return i.ItemType + "\u0001" + i.Include + "\u0001" + String.Join("\u0002", i.Metadata.Select(m => m.Name + "=" + m.Value));
    }

    static string Sha256(byte[] bytes)
    {
        using (var sha = SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "");
    }

    // The literal, unconditional, non-imported configuration declarations of the project file.
    static string[] Configurations(byte[] snapshot, Project project)
    {
        var settings = new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null };
        ProjectRootElement root;
        using (var collection = new ProjectCollection())
        using (var reader = XmlReader.Create(new StringReader(new UTF8Encoding(false, true).GetString(snapshot).TrimStart('﻿')), settings))
            root = ProjectRootElement.Create(reader, collection, false);
        var declarations = root.Items.Where(i => i.ItemType.Equals("ProjectConfiguration", StringComparison.OrdinalIgnoreCase)).ToArray();
        if (declarations.Any(e => InTarget(e) || !(e.Parent.Parent is ProjectRootElement) || e.Condition.Length != 0 ||
                                  e.Parent.Condition.Length != 0 || String.IsNullOrEmpty(e.Include) ||
                                  e.Include.IndexOfAny(new[] { ';', '%', '$', '@', '*', '?' }) >= 0))
            throw new InvalidOperationException("Conditional or nonliteral configuration declaration");
        var configurations = declarations.Select(e => e.Include).ToArray();
        if (configurations.Length == 0) throw new InvalidOperationException("Project has no declared configurations");
        if (configurations.Distinct(StringComparer.OrdinalIgnoreCase).Count() != configurations.Length ||
            !configurations.OrderBy(s => s, StringComparer.OrdinalIgnoreCase).SequenceEqual(
                project.GetItems("ProjectConfiguration").Select(i => i.EvaluatedInclude).OrderBy(s => s, StringComparer.OrdinalIgnoreCase),
                StringComparer.OrdinalIgnoreCase))
            throw new InvalidOperationException("Evaluated configurations differ from literal declarations");
        return configurations;
    }

    static Project Load(ProjectCollection collection, string path, string configuration)
    {
        string[] parts = configuration.Split('|');
        if (parts.Length != 2 || parts.Any(String.IsNullOrWhiteSpace)) throw new InvalidOperationException("Invalid project configuration");
        collection.SetGlobalProperty("Configuration", parts[0]);
        collection.SetGlobalProperty("Platform", parts[1]);
        return collection.LoadProject(path);
    }

    /*
     * Handles one request: fences the trusted root, the project and the edited files, evaluates every
     * declared configuration twice (the second time with the imports fenced and hashed), plans both
     * definitions and, with Commit, runs TransactionProbe with the two-phase approval. Returns the
     * answer object; throws with the refusal's reason.
     */
    internal static object Handle(Dictionary<string, string> request, string rootPath, string path,
                                  Func<string, string, bool> approve)
    {
        var edit = Parse(request, path);
        string rootPrefix = Path.GetFullPath(rootPath).TrimEnd('\\') + "\\";
        var fences = new List<IDisposable>();
        try
        {
            fences.Add(TransactionProbe.FenceEvaluation(rootPath, edit.ProjectPath));
            foreach (string file in edit.Incoming)
            {
                // A project may list files beside the trusted root ("..\src\a.cpp"); only the
                // definitions are written, and those must be inside it (TransactionProbe).
                if (!File.Exists(file) || (File.GetAttributes(file) & FileAttributes.ReparsePoint) != 0)
                    throw new InvalidOperationException("The file does not exist or is a link: " + file);
                if (file.StartsWith(rootPrefix, StringComparison.OrdinalIgnoreCase))
                    fences.Add(TransactionProbe.FenceEvaluation(rootPath, file));
            }
            byte[] snapshot = File.ReadAllBytes(edit.ProjectPath);
            string environment = EnvironmentFingerprint.Current();
            string[] configurations;
            var inputs = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            using (var collection = new ProjectCollection())
            {
                string configuration0, platform0;
                if (!request.TryGetValue("Configuration", out configuration0) || !request.TryGetValue("Platform", out platform0))
                    throw new InvalidOperationException("The request names no configuration");
                configurations = Configurations(snapshot, Load(collection, edit.ProjectPath, configuration0 + "|" + platform0));
                collection.UnloadAllProjects();
                foreach (string configuration in configurations)
                {
                    var project = Load(collection, edit.ProjectPath, configuration);
                    foreach (var import in project.Imports)
                    {
                        if (import.ImportedProject.Items.Any(i => i.ItemType.Equals("ProjectConfiguration", StringComparison.OrdinalIgnoreCase)))
                            throw new InvalidOperationException("Imported configuration declaration");
                        string importPath = import.ImportedProject.FullPath;
                        string hash = Sha256(File.ReadAllBytes(importPath)), old;
                        if (inputs.TryGetValue(importPath, out old) && old != hash)
                            throw new InvalidOperationException("Import changed during evaluation: " + importPath);
                        inputs[importPath] = hash;
                    }
                    CheckConfiguration(project, edit);
                    collection.UnloadAllProjects();
                }
            }
            if (!snapshot.SequenceEqual(File.ReadAllBytes(edit.ProjectPath)))
                throw new InvalidOperationException("Project changed during configuration evaluation");
            if (environment != EnvironmentFingerprint.Current())
                throw new InvalidOperationException("Environment changed during evaluation");
            using (var importFence = TransactionProbe.FenceImports(inputs))
            {
                var verified = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                using (var collection = new ProjectCollection())
                    foreach (string configuration in configurations)
                    {
                        var again = Load(collection, edit.ProjectPath, configuration);
                        foreach (var import in again.Imports)
                        {
                            if (!inputs.ContainsKey(import.ImportedProject.FullPath))
                                throw new InvalidOperationException("Imported inputs changed during evaluation");
                            verified.Add(import.ImportedProject.FullPath);
                        }
                        CheckConfiguration(again, edit);
                        collection.UnloadAllProjects();
                    }
                if (verified.Count != inputs.Count || environment != EnvironmentFingerprint.Current())
                    throw new InvalidOperationException("Evaluation inputs changed");
                byte[] projectAfter = Plan(snapshot, edit, false);
                string filtersPath = edit.ProjectPath + ".filters";
                byte[] filters = edit.Properties.Count == 0 && File.Exists(filtersPath) ? File.ReadAllBytes(filtersPath) : null;
                byte[] filtersAfter = filters == null ? null : Plan(filters, edit, true);
                if (filters == null && edit.Filter.Length != 0)
                    throw new InvalidOperationException("The project has no .filters file to place the file in");
                string commit;
                if (request.TryGetValue("Commit", out commit) && commit == "true")
                {
                    string transactionId;
                    if (!request.TryGetValue("TransactionId", out transactionId))
                        throw new InvalidOperationException("Trusted root and transaction identifier required");
                    foreach (var fence in fences) fence.Dispose(); // The transaction reacquires and revalidates the definitions.
                    fences.Clear();
                    importFence.Dispose();
                    var entries = new List<TransactionEntry> { Entry(edit.ProjectPath, snapshot, projectAfter) };
                    if (filters != null) entries.Add(Entry(filtersPath, filters, filtersAfter));
                    TransactionProbe.Execute(new TransactionRequest {
                        Root = rootPath, Journal = Path.Combine(rootPath, ".frost-project-edit-journal"), Trusted = true,
                        TransactionId = transactionId, Environment = environment,
                        ImportedInputs = new Dictionary<string, string>(inputs, StringComparer.OrdinalIgnoreCase),
                        Entries = entries.ToArray()
                    }, digest => approve(transactionId, digest));
                }
                return new { Items = (object)null, Error = (string)null, Edit = new {
                    Project = Convert.ToBase64String(projectAfter),
                    Filters = filtersAfter == null ? null : Convert.ToBase64String(filtersAfter),
                    Configurations = configurations, Environment = environment, ImportedInputs = inputs } };
            }
        }
        finally
        {
            foreach (var fence in fences) fence.Dispose();
        }
    }

    internal static TransactionEntry Entry(string path, byte[] before, byte[] after)
    {
        return new TransactionEntry { Path = path, Before = Convert.ToBase64String(before), After = Convert.ToBase64String(after) };
    }
}
