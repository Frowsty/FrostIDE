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
using System.Text.RegularExpressions;
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
    public bool BeforeFileMove;
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
        string beforeMove, commit;
        edit.BeforeFileMove = request.TryGetValue("BeforeFileMove", out beforeMove) && beforeMove == "true";
        if (edit.BeforeFileMove && (edit.RenamedFrom.Length == 0 ||
            (request.TryGetValue("Commit", out commit) && commit == "true")))
            throw new InvalidOperationException("BeforeFileMove requires a non-committing rename plan");
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
                    foreach (var metadata in element.Metadata.Where(m => edit.BeforeFileMove && m.Name.Equals("Link", StringComparison.OrdinalIgnoreCase)))
                    {
                        if (!Literal(metadata.Value))
                            throw new InvalidOperationException("Use literal Link metadata before transactional rename");
                        if (Path.GetFileName(metadata.Value).Equals(Path.GetFileName(edit.RenamedFrom[r]), StringComparison.OrdinalIgnoreCase))
                        {
                            string parent = Path.GetDirectoryName(metadata.Value);
                            metadata.Value = String.IsNullOrEmpty(parent) ? Path.GetFileName(edit.RenamedTo[r]) :
                                Path.Combine(parent, Path.GetFileName(edit.RenamedTo[r]));
                        }
                    }
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

    static string ExpandAt(Project project, ProjectRootElement root, string value)
    {
        string directory = Path.GetDirectoryName(root.FullPath) + "\\";
        var reserved = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) {
            { "MSBuildThisFileDirectory", directory },
            { "MSBuildThisFileDirectoryNoRoot", directory.Substring(Path.GetPathRoot(directory).Length) },
            { "MSBuildThisFileFullPath", root.FullPath }, { "MSBuildThisFile", Path.GetFileName(root.FullPath) },
            { "MSBuildThisFileName", Path.GetFileNameWithoutExtension(root.FullPath) },
            { "MSBuildThisFileExtension", Path.GetExtension(root.FullPath) }
        };
        value = Regex.Replace(value,
            @"\$\((MSBuildThisFileDirectory|MSBuildThisFileDirectoryNoRoot|MSBuildThisFileFullPath|MSBuildThisFileName|MSBuildThisFileExtension|MSBuildThisFile)\)",
            m => reserved[m.Groups[1].Value], RegexOptions.IgnoreCase);
        return project.ExpandString(value);
    }

    // Over-approximate declaration-time property values instead of substituting only the final
    // evaluated value. Every captured declaration is considered, even a currently inactive one.
    // Cyclic/massively ambiguous property construction refuses; trailing-slash normalization is
    // idempotent for path identity and needs no recursive expansion of itself.
    static List<string> PossibleValues(Project project, ProjectRootElement root, string value,
        Dictionary<string, List<ProjectPropertyElement>> definitions, HashSet<string> visiting, ref int remaining)
    {
        if (--remaining < 0) throw new InvalidOperationException("Filesystem predicate property expansion is ambiguous");
        if (value.IndexOf("@(", StringComparison.Ordinal) >= 0 || value.IndexOf("%(", StringComparison.Ordinal) >= 0)
            throw new InvalidOperationException("Item/metadata expressions cannot prove declaration-time filesystem paths");
        var token = Regex.Match(value, @"\$\(([A-Za-z_][A-Za-z0-9_.-]*)\)");
        if (!token.Success)
        {
            if (Regex.IsMatch(value, @"\$\((?!\[)"))
                throw new InvalidOperationException("Instance/unresolved property functions in filesystem inputs require explicit literals");
            return new List<string> { ExpandAt(project, root, value) };
        }
        string name = token.Groups[1].Value;
        if (name.IndexOf('.') >= 0)
            throw new InvalidOperationException("Dotted/instance property inputs cannot prove declaration-time filesystem paths");
        // Reserved current-file values are contextual facts, not mutable property declarations.
        if (new[] { "MSBuildThisFileDirectory", "MSBuildThisFileDirectoryNoRoot", "MSBuildThisFileFullPath",
                    "MSBuildThisFileName", "MSBuildThisFileExtension", "MSBuildThisFile" }
            .Contains(name, StringComparer.OrdinalIgnoreCase))
        {
            string expanded = ExpandAt(project, root, token.Value);
            return PossibleValues(project, root, value.Substring(0, token.Index) + expanded +
                value.Substring(token.Index + token.Length), definitions, visiting, ref remaining);
        }
        if (!visiting.Add(name)) throw new InvalidOperationException("Cyclic property in a filesystem predicate");
        var alternatives = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { project.GetPropertyValue(name) };
        List<ProjectPropertyElement> declarations;
        if (definitions.TryGetValue(name, out declarations))
            foreach (var declaration in declarations)
            {
                string trimmed = declaration.Value.Trim();
                if (trimmed.Equals("$(" + name + ")\\", StringComparison.OrdinalIgnoreCase) ||
                    trimmed.Equals("$(" + name + ")/", StringComparison.OrdinalIgnoreCase)) continue;
                foreach (string alternative in PossibleValues(project, declaration.ContainingProject,
                    declaration.Value, definitions, visiting, ref remaining)) alternatives.Add(alternative);
            }
        visiting.Remove(name);
        var result = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (string alternative in alternatives)
            foreach (string expanded in PossibleValues(project, root,
                value.Substring(0, token.Index) + alternative + value.Substring(token.Index + token.Length),
                definitions, visiting, ref remaining)) result.Add(expanded);
        return result.ToList();
    }

    static List<string> PossibleValues(Project project, ProjectRootElement root, string value,
        Dictionary<string, List<ProjectPropertyElement>> definitions)
    {
        int remaining = 1024;
        return PossibleValues(project, root, value, definitions,
            new HashSet<string>(StringComparer.OrdinalIgnoreCase), ref remaining);
    }

    static void CheckNamespaceExpression(Project project, ProjectRootElement root, string expression,
                                         FileEditRequest edit, Dictionary<string, List<ProjectPropertyElement>> definitions)
    {
        // Check original expressions, not just their already-evaluated true/false property value.
        // A normal SDK Exists(toolchain.props) is stable; Exists(old.cpp/new.cpp) is not.
        foreach (Match io in Regex.Matches(expression, @"\[System\.IO\.(?:File|Directory)\]::(\w+)", RegexOptions.IgnoreCase))
            if (!io.Groups[1].Value.Equals("Exists", StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Unmodeled filesystem property functions cannot be planned before a rename");
        foreach (Match function in Regex.Matches(expression,
            @"\b(Exists|GetDirectoryNameOfFileAbove|GetPathOfFileAbove)\s*\(", RegexOptions.IgnoreCase))
        {
            int start = function.Index + function.Length, depth = 1;
            char quote = '\0';
            var arguments = new List<string>();
            for (int i = start; i < expression.Length; ++i)
            {
                char c = expression[i];
                if (quote != '\0') { if (c == quote) quote = '\0'; continue; }
                if (c == '\'' || c == '"') { quote = c; continue; }
                if (c == '(') ++depth;
                if (c == ')') --depth;
                if ((c == ',' && depth == 1) || depth == 0)
                {
                    arguments.Add(expression.Substring(start, i - start).Trim()); start = i + 1;
                    if (depth == 0) break;
                }
            }
            bool exists = function.Groups[1].Value.Equals("Exists", StringComparison.OrdinalIgnoreCase);
            if (depth != 0 || (exists ? arguments.Count != 1 : arguments.Count < 1 || arguments.Count > 2))
                throw new InvalidOperationException("Unresolved filesystem expression in project inputs");
            foreach (string argument in arguments)
            {
                string value = argument;
                if (value.Length >= 2 && (value[0] == '\'' || value[0] == '"') && value[value.Length - 1] == value[0])
                    value = value.Substring(1, value.Length - 2);
                foreach (string candidate in PossibleValues(project, root, value, definitions))
                {
                    if (candidate.Length == 0) continue;
                    if (!Literal(candidate))
                        throw new InvalidOperationException("A filesystem expression could not be resolved for rename planning");
                    string full = FullPathOf(Path.GetDirectoryName(root.FullPath), candidate);
                    if (edit.Existing.Concat(edit.Incoming).Contains(full, StringComparer.OrdinalIgnoreCase) ||
                        (!exists && edit.Existing.Concat(edit.Incoming).Any(p =>
                            Path.GetFileName(p).Equals(Path.GetFileName(candidate), StringComparison.OrdinalIgnoreCase))))
                        throw new InvalidOperationException("A project/import expression depends on the old or new source existing");
                }
            }
        }
    }

    // A pre-move plan cannot evaluate arbitrary file-dependent item creation. Standard explicit
    // C++ items and configuration/platform conditions are supported; dynamic imported item rules
    // are refused rather than guessed from the old item's ownership.
    static void CheckMembershipProfile(Project project, FileEditRequest edit)
    {
        var roots = new[] { project.Xml }.Concat(project.Imports.Select(i => i.ImportedProject)).Distinct().ToArray();
        var definitions = roots.SelectMany(r => r.Properties).Where(p => !InTarget(p))
            .GroupBy(p => p.Name, StringComparer.OrdinalIgnoreCase)
            .ToDictionary(g => g.Key, g => g.ToList(), StringComparer.OrdinalIgnoreCase);
        foreach (var root in roots)
        {
            if (!String.IsNullOrEmpty(root.Sdk) || root.Children.Any(c => c is ProjectSdkElement))
                throw new InvalidOperationException("Implicit SDK resolver inputs are not captured; use explicit VC imports for this rename profile");
            foreach (var property in root.Properties.Where(p => !InTarget(p)))
            {
                CheckNamespaceExpression(project, root, property.Value, edit, definitions);
                for (ProjectElement current = property; current != null && !(current is ProjectRootElement); current = current.Parent)
                    CheckNamespaceExpression(project, root, current.Condition, edit, definitions);
            }
            foreach (var item in root.Items.Where(i => IsFileItemType(i.ItemType) && !InTarget(i)))
            {
                foreach (string value in new[] { item.Include, item.Remove, item.Update, item.Exclude })
                    if (value.Length != 0 && (value.IndexOfAny(new[] { '@', '%' }) >= 0 || !Literal(ExpandAt(project, root, value))))
                        throw new InvalidOperationException("Dynamic or wildcard imported membership requires an explicit source item before rename");
                for (ProjectElement current = item; current != null && !(current is ProjectRootElement); current = current.Parent)
                {
                    string condition = current.Condition.Replace("$(Configuration)", "").Replace("$(Platform)", "");
                    if (condition.IndexOfAny(new[] { '$', '@', '%' }) >= 0 ||
                        condition.IndexOf("Exists", StringComparison.OrdinalIgnoreCase) >= 0)
                        throw new InvalidOperationException("File-dependent item conditions cannot be planned before the physical rename");
                }
            }
            foreach (var import in root.Imports)
            {
                for (ProjectElement current = import; current != null && !(current is ProjectRootElement); current = current.Parent)
                    CheckNamespaceExpression(project, root, current.Condition, edit, definitions);
                CheckNamespaceExpression(project, root, import.Project, edit, definitions);
                foreach (string path in PossibleValues(project, root, import.Project, definitions))
                {
                    if (path.IndexOfAny(new[] { '*', '?' }) >= 0)
                    {
                        // Ordinary VC SDK ImportBefore/After globs live in the toolchain directory,
                        // which this source rename does not touch. A glob rooted at/above either
                        // renamed path can change discovery and requires an explicit import instead.
                        int wildcard = path.IndexOfAny(new[] { '*', '?' });
                        int separator = path.LastIndexOfAny(new[] { '\\', '/' }, wildcard);
                        string prefix = separator < 0 ? "." : path.Substring(0, separator + 1);
                        string directory = FullPathOf(Path.GetDirectoryName(root.FullPath), prefix).TrimEnd('\\', '/') + "\\";
                        if (edit.Existing.Concat(edit.Incoming).Any(p => p.StartsWith(directory, StringComparison.OrdinalIgnoreCase)))
                            throw new InvalidOperationException("A wildcard import can change when this source is renamed; use an explicit import");
                        continue;
                    }
                    if (path.IndexOfAny(new[] { '$', '@', '%', ';' }) >= 0)
                        throw new InvalidOperationException("An import path could not be resolved for this configuration");
                    if (path.Length != 0 && Literal(path))
                    {
                        string full = FullPathOf(Path.GetDirectoryName(root.FullPath), path);
                        if (edit.Existing.Concat(edit.Incoming).Contains(full, StringComparer.OrdinalIgnoreCase))
                            throw new InvalidOperationException("The renamed source also controls an import path");
                    }
                }
            }
        }
    }

    static List<string> Membership(Project project, FileEditRequest edit, bool renamed)
    {
        var result = new List<string>();
        foreach (var item in project.Items.Where(i => IsFileItemType(i.ItemType)))
        {
            string path = FullPathOf(Path.GetDirectoryName(edit.ProjectPath), item.EvaluatedInclude);
            if (renamed)
                for (int i = 0; i < edit.RenamedFrom.Length; ++i)
                    if (path.Equals(edit.RenamedFrom[i], StringComparison.OrdinalIgnoreCase))
                    { path = edit.RenamedTo[i]; break; }
            result.Add(item.ItemType + "\u0001" + path);
        }
        result.Sort(StringComparer.OrdinalIgnoreCase);
        return result;
    }

    // Evaluate transformed construction XML IN MEMORY at its real project path. Ordinary SDK
    // imports see the real directory and properties; no project/source is temporarily written.
    static void CheckAfter(byte[] after, FileEditRequest edit, string[] configurations,
                           SortedDictionary<string, string> imports)
    {
        var settings = new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null };
        foreach (string configuration in configurations)
        using (var beforeCollection = new ProjectCollection())
        using (var afterCollection = new ProjectCollection())
        {
            var before = Load(beforeCollection, edit.ProjectPath, configuration);
            CheckMembershipProfile(before, edit);
            string[] parts = configuration.Split('|');
            afterCollection.SetGlobalProperty("Configuration", parts[0]);
            afterCollection.SetGlobalProperty("Platform", parts[1]);
            ProjectRootElement root;
            using (var reader = XmlReader.Create(new StringReader(new UTF8Encoding(false, true).GetString(after).TrimStart('﻿')), settings))
                root = ProjectRootElement.Create(reader, afterCollection, true);
            root.FullPath = edit.ProjectPath;
            var planned = new Project(root, null, null, afterCollection);
            CheckMembershipProfile(planned, edit);
            if (!Membership(before, edit, true).SequenceEqual(Membership(planned, edit, false), StringComparer.OrdinalIgnoreCase))
                throw new InvalidOperationException("Imported rules change or duplicate the proposed renamed membership");
            var actualImports = planned.Imports.Select(i => i.ImportedProject.FullPath).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
            var beforeImports = before.Imports.Select(i => i.ImportedProject.FullPath).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
            if (!actualImports.OrderBy(i => i, StringComparer.OrdinalIgnoreCase).SequenceEqual(
                    beforeImports.OrderBy(i => i, StringComparer.OrdinalIgnoreCase), StringComparer.OrdinalIgnoreCase) ||
                actualImports.Any(i => !imports.ContainsKey(i)))
                throw new InvalidOperationException("The proposed rename changes the imported input set");
        }
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
                if (edit.BeforeFileMove)
                {
                    if (File.Exists(file) || Directory.Exists(file))
                        throw new InvalidOperationException("The rename destination already exists: " + file);
                    continue;
                }
                // A project may list files beside the trusted root ("..\src\a.cpp"); only the
                // definitions are written, and those must be inside it (TransactionProbe).
                if (!File.Exists(file) || (File.GetAttributes(file) & FileAttributes.ReparsePoint) != 0)
                    throw new InvalidOperationException("The file does not exist or is a link: " + file);
                if (file.StartsWith(rootPrefix, StringComparison.OrdinalIgnoreCase))
                    fences.Add(TransactionProbe.FenceEvaluation(rootPath, file));
            }
            if (edit.BeforeFileMove)
                foreach (string file in edit.Existing)
                {
                    if (!File.Exists(file) || (File.GetAttributes(file) & FileAttributes.ReparsePoint) != 0)
                        throw new InvalidOperationException("The source is missing or is a link: " + file);
                    if (file.StartsWith(rootPrefix, StringComparison.OrdinalIgnoreCase))
                        fences.Add(TransactionProbe.FenceEvaluation(rootPath, file));
                }
            byte[] snapshot = File.ReadAllBytes(edit.ProjectPath);
            if (edit.BeforeFileMove && snapshot.Length > 8 * 1024 * 1024)
                throw new InvalidOperationException("Project exceeds the plan input limit");
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
                if (edit.BeforeFileMove)
                {
                    if (filters != null && filters.Length > 8 * 1024 * 1024)
                        throw new InvalidOperationException("Filters exceed the plan input limit");
                    CheckAfter(projectAfter, edit, configurations, inputs);
                }
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
                if (!edit.BeforeFileMove)
                    return new { Items = (object)null, Error = (string)null, Edit = new {
                        Project = Convert.ToBase64String(projectAfter),
                        Filters = filtersAfter == null ? null : Convert.ToBase64String(filtersAfter),
                        Configurations = configurations, Environment = environment, ImportedInputs = inputs } };
                if (inputs.Count > 512 || configurations.Length > 256)
                    throw new InvalidOperationException("Project configuration/import count exceeds the plan limit");
                long planBytes = snapshot.Length + projectAfter.Length +
                    (filters == null ? 0 : filters.Length + filtersAfter.Length);
                var inputBytes = inputs.Select(pair => {
                    byte[] content = File.ReadAllBytes(pair.Key);
                    if (Sha256(content) != pair.Value) throw new InvalidOperationException("An import changed before plan publication");
                    if (content.Length > 8 * 1024 * 1024) throw new InvalidOperationException("An import exceeds the plan input limit");
                    planBytes += content.Length;
                    if (planBytes > 36 * 1024 * 1024) throw new InvalidOperationException("Combined project plan inputs exceed the publication limit");
                    return new { Path = pair.Key, Before = Convert.ToBase64String(content) };
                }).ToArray();
                if (edit.BeforeFileMove && (!snapshot.SequenceEqual(File.ReadAllBytes(edit.ProjectPath)) ||
                    (filters == null ? File.Exists(filtersPath) : !File.Exists(filtersPath) || !filters.SequenceEqual(File.ReadAllBytes(filtersPath))) ||
                    environment != EnvironmentFingerprint.Current()))
                    throw new InvalidOperationException("Project inputs changed before plan publication");
                return new { Items = (object)null, Error = (string)null, Edit = new {
                    PlanVersion = 1, Mode = edit.BeforeFileMove ? "BeforeFileMoveRename" : "LegacyEdit",
                    ProjectPath = edit.ProjectPath, ProjectBefore = Convert.ToBase64String(snapshot),
                    FiltersBefore = filters == null ? null : Convert.ToBase64String(filters), InputBytes = inputBytes,
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
