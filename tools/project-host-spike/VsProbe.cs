// SPDX-License-Identifier: Apache-2.0
// Experimental .NET Framework ownership probe; an installed VS MSBuild supplies its assemblies.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;
using System.Xml.Linq;
using Microsoft.Build.Evaluation;

sealed class RemovalPlan
{
    public string Sha256 { get; set; }
    public int Offset { get; set; }
    public int Length { get; set; }
    public string Insert { get; set; }
}

static class VsProbe
{
    static void Main(string[] args)
    {
        Console.InputEncoding = new UTF8Encoding(false, true);
        Console.OutputEncoding = new UTF8Encoding(false, true);
        if (args.Length != 1 || !Path.IsPathRooted(args[0]))
            throw new ArgumentException("Pass the absolute VS MSBuild assembly directory");
        string directory = args[0];
        AppDomain.CurrentDomain.AssemblyResolve += (_, e) => {
            string path = Path.Combine(directory, new AssemblyName(e.Name).Name + ".dll");
            return File.Exists(path) ? Assembly.LoadFrom(path) : null;
        };
        Run();
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Run()
    {
        var json = new JavaScriptSerializer();
        string line;
        while ((line = Console.ReadLine()) != null)
        {
            try
            {
                var request = json.Deserialize<Dictionary<string, string>>(line);
                string trustedRoot;
                if (!request.TryGetValue("Trusted", out trustedRoot) || trustedRoot != "true")
                    throw new InvalidOperationException("Trusted root required before evaluation");
                string path = request["Project"];
                if (!Path.IsPathRooted(path)) throw new ArgumentException("Project path must be absolute");
                string rootPath;
                if (!request.TryGetValue("Root", out rootPath)) throw new InvalidOperationException("Trusted root required");
                string recovery;
                if (request.TryGetValue("Recover", out recovery) && recovery == "true")
                {
                    string recoveryId = request["TransactionId"];
                    TransactionProbe.Execute(new TransactionRequest {
                        Root = rootPath, Journal = Path.Combine(rootPath, ".frost-project-edit-journal"),
                        Trusted = true, Recover = true, TransactionId = recoveryId, ExpectedProject = path,
                        JournalDigest = request["JournalDigest"]
                    }, digest => AwaitApproval(json, recoveryId, digest));
                    Console.WriteLine(json.Serialize(new { Error = (string)null }));
                    continue;
                }
                using (var fence = TransactionProbe.FenceEvaluation(rootPath, path))
                {
                string remove;
                request.TryGetValue("Remove", out remove);
                string add;
                request.TryGetValue("Add", out add);
                if (remove != null && add != null) throw new InvalidOperationException("Choose one edit operation");
                if (add != null && (Path.IsPathRooted(add) || add.IndexOfAny(new[] {'*', '?', '$', '@', '%', ';', ':', '\0', '\r', '\n'}) >= 0 ||
                    add.Split(new[] {'/', '\\'}).Any(s => s == "" || s == "." || s == "..") ||
                    !new[] {".c", ".cpp", ".h", ".hpp"}.Contains(Path.GetExtension(add), StringComparer.OrdinalIgnoreCase)))
                    throw new InvalidOperationException("Choose a literal C/C++ source or header inside the project directory");
                string itemType = add != null && new[] {".h", ".hpp"}.Contains(Path.GetExtension(add), StringComparer.OrdinalIgnoreCase)
                    ? "ClInclude" : "ClCompile";
                using (var sourceFence = add == null ? null : TransactionProbe.FenceEvaluation(rootPath, Path.Combine(Path.GetDirectoryName(path), add)))
                {
                string selected = add ?? remove;
                byte[] snapshot = selected == null ? null : File.ReadAllBytes(path);
                string environment = EnvironmentFingerprint.Current();
                var globals = new Dictionary<string, string> {
                    ["Configuration"] = request["Configuration"], ["Platform"] = request["Platform"]
                };
                using (var collection = new ProjectCollection(globals))
                {
                    var project = collection.LoadProject(path);
                    var items = project.AllEvaluatedItems.Select(item => new {
                        Type = item.ItemType, item.EvaluatedInclude, item.UnevaluatedInclude,
                        DefiningProject = item.Xml.ContainingProject.FullPath,
                        Line = item.Xml.Location.Line, Condition = item.Xml.Condition
                    }).ToArray();
                    object edit = null;
                    if (snapshot != null)
                    {
                        if (!snapshot.SequenceEqual(File.ReadAllBytes(path)))
                            throw new InvalidOperationException("Project changed during evaluation");
                        CheckOwnership(project, path, selected, add != null);
                        var definition = XDocument.Parse(new UTF8Encoding(false, true).GetString(snapshot).TrimStart('\ufeff'));
                        var declarations = definition.Descendants().Where(e => e.Name.LocalName == "ProjectConfiguration").ToArray();
                        if (declarations.Any(e => e.Parent == null || e.Parent.Name.LocalName != "ItemGroup" ||
                            e.Parent.Parent != definition.Root || e.AncestorsAndSelf().Any(a => a.Attributes().Any(x =>
                                x.Name.LocalName.Equals("Condition", StringComparison.OrdinalIgnoreCase))) ||
                            e.Attributes().Any(a => a.Name.LocalName != "Include") ||
                            e.Attribute("Include") == null || ((string)e.Attribute("Include")).IndexOfAny(
                                new[] {'*', '?', '$', '@', '%', ';'}) >= 0))
                            throw new InvalidOperationException("Conditional or nonliteral configuration declaration");
                        var configurations = declarations.Select(e => (string)e.Attribute("Include")).ToArray();
                        if (configurations.Length == 0)
                            throw new InvalidOperationException("Project has no declared configurations");
                        if (configurations.Distinct(StringComparer.OrdinalIgnoreCase).Count() != configurations.Length ||
                            !configurations.OrderBy(s => s, StringComparer.OrdinalIgnoreCase).SequenceEqual(
                                project.GetItems("ProjectConfiguration").Select(i => i.EvaluatedInclude)
                                    .OrderBy(s => s, StringComparer.OrdinalIgnoreCase), StringComparer.OrdinalIgnoreCase))
                            throw new InvalidOperationException("Evaluated configurations differ from literal declarations");
                        var inputs = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                        foreach (string configuration in configurations)
                        {
                            string[] parts = configuration.Split('|');
                            if (parts.Length != 2 || parts.Any(String.IsNullOrWhiteSpace))
                                throw new InvalidOperationException("Invalid project configuration");
                            var configGlobals = new Dictionary<string, string> {
                                ["Configuration"] = parts[0], ["Platform"] = parts[1]
                            };
                            using (var configCollection = new ProjectCollection(configGlobals))
                            {
                                var configProject = configCollection.LoadProject(path);
                                foreach (var import in configProject.Imports)
                                {
                                    if (import.ImportedProject.Items.Any(i => i.ItemType.Equals("ProjectConfiguration", StringComparison.OrdinalIgnoreCase)))
                                        throw new InvalidOperationException("Imported configuration declaration");
                                    string importPath = import.ImportedProject.FullPath;
                                    string hash;
                                    using (var sha = SHA256.Create()) hash = BitConverter.ToString(sha.ComputeHash(File.ReadAllBytes(importPath))).Replace("-", "");
                                    string old;
                                    if (inputs.TryGetValue(importPath, out old) && old != hash)
                                        throw new InvalidOperationException("Import changed during evaluation: " + importPath);
                                    inputs[importPath] = hash;
                                }
                                CheckOwnership(configProject, path, selected, add != null);
                            }
                        }
                        if (!snapshot.SequenceEqual(File.ReadAllBytes(path)))
                            throw new InvalidOperationException("Project changed during configuration evaluation");
                        if (environment != EnvironmentFingerprint.Current())
                            throw new InvalidOperationException("Environment changed during evaluation");
                        using (var importFence = TransactionProbe.FenceImports(inputs))
                        {
                        var verifiedInputs = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                        foreach (string configuration in configurations)
                        {
                            string[] parts = configuration.Split('|');
                            var globalsAgain = new Dictionary<string, string> {
                                ["Configuration"] = parts[0], ["Platform"] = parts[1]
                            };
                            using (var collectionAgain = new ProjectCollection(globalsAgain))
                            {
                                var again = collectionAgain.LoadProject(path);
                                foreach (var import in again.Imports)
                                {
                                    string importPath = import.ImportedProject.FullPath;
                                    if (!inputs.ContainsKey(importPath))
                                        throw new InvalidOperationException("Imported inputs changed during evaluation");
                                    verifiedInputs.Add(importPath);
                                }
                                CheckOwnership(again, path, selected, add != null);
                            }
                        }
                        if (verifiedInputs.Count != inputs.Count || environment != EnvironmentFingerprint.Current())
                            throw new InvalidOperationException("Evaluation inputs changed");
                        var projectPlan = add == null ? RemovePlan(snapshot, remove, false) : AddPlan(snapshot, add, itemType, path);
                        string filtersPath = path + ".filters";
                        RemovalPlan filtersPlan = null;
                        byte[] filters = null;
                        if (File.Exists(filtersPath))
                        {
                            filters = File.ReadAllBytes(filtersPath);
                            filtersPlan = add == null ? RemovePlan(filters, remove, true) : AddPlan(filters, add, itemType, path);
                        }
                        string commit;
                        if (request.TryGetValue("Commit", out commit) && commit == "true")
                        {
                            if (filtersPlan == null) throw new InvalidOperationException("A filters assignment is required");
                            string trusted, root, transactionId;
                            if (!request.TryGetValue("Trusted", out trusted) || trusted != "true" ||
                                !request.TryGetValue("Root", out root) || !request.TryGetValue("TransactionId", out transactionId))
                                throw new InvalidOperationException("Trusted root and transaction identifier required");
                            fence.Dispose(); // The transaction reacquires and revalidates the definitions.
                            importFence.Dispose();
                            TransactionProbe.Execute(new TransactionRequest {
                                Root = root, Journal = Path.Combine(root, ".frost-project-edit-journal"), Trusted = true,
                                TransactionId = transactionId, Environment = environment,
                                ImportedInputs = new Dictionary<string, string>(inputs, StringComparer.OrdinalIgnoreCase),
                                Entries = new[] {
                                    Entry(path, snapshot, projectPlan), Entry(filtersPath, filters, filtersPlan)
                                }
                            }, digest => AwaitApproval(json, transactionId, digest));
                        }
                        edit = new { Project = projectPlan, Filters = filtersPlan, Configurations = configurations,
                                     Globals = new { Configuration = request["Configuration"], Platform = request["Platform"] },
                                     Environment = environment, ImportedInputs = inputs };
                        }
                    }
                    Console.WriteLine(json.Serialize(new { Items = items, Edit = edit, Error = (string)null }));
                }
                }
                }
            }
            catch (Exception e)
            {
                Console.WriteLine(json.Serialize(new { Items = (object)null, Error = e.Message }));
            }
        }
    }

    static bool AwaitApproval(JavaScriptSerializer json, string id, string digest)
    {
        Console.WriteLine(json.Serialize(new { Ready = true, TransactionId = id, JournalDigest = digest }));
        string acknowledgement = Console.ReadLine();
        if (acknowledgement == null) throw new InvalidOperationException("Commit was not acknowledged");
        var decision = json.Deserialize<Dictionary<string, string>>(acknowledgement);
        string accepted, decisionId;
        if (decision == null || !decision.TryGetValue("Accept", out accepted) || accepted != "true" ||
            !decision.TryGetValue("TransactionId", out decisionId) || decisionId != id)
            return false;
        return true;
    }

    // This plan represents only the selected evaluation; it is not a production-safe commit.
    static TransactionEntry Entry(string path, byte[] original, RemovalPlan plan)
    {
        byte[] insert = Encoding.UTF8.GetBytes(plan.Insert ?? "");
        var output = new byte[original.Length - plan.Length + insert.Length];
        Buffer.BlockCopy(original, 0, output, 0, plan.Offset);
        Buffer.BlockCopy(insert, 0, output, plan.Offset, insert.Length);
        Buffer.BlockCopy(original, plan.Offset + plan.Length, output, plan.Offset + insert.Length,
                         original.Length - plan.Offset - plan.Length);
        return new TransactionEntry { Path = path, Before = Convert.ToBase64String(original),
                                      After = Convert.ToBase64String(output) };
    }

    static void CheckOwnership(Project project, string projectPath, string include, bool adding)
    {
        if (adding)
        {
            foreach (var definition in new[] { project.Xml }.Concat(project.Imports.Select(i => i.ImportedProject)))
                foreach (var item in definition.Items.Where(i => (i.ItemType.Equals("ClCompile", StringComparison.OrdinalIgnoreCase) ||
                    i.ItemType.Equals("ClInclude", StringComparison.OrdinalIgnoreCase)) &&
                    !(i.Parent.Parent is Microsoft.Build.Construction.ProjectTargetElement)))
                    if (!String.IsNullOrEmpty(item.Remove) || !String.IsNullOrEmpty(item.Update) ||
                        (item.Include ?? "").IndexOfAny(new[] {'*', '?', '$', '@', '%', ';'}) >= 0)
                        throw new InvalidOperationException("Dynamic source items or source removals are unsupported");
            string expected = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(projectPath), include));
            foreach (var item in project.GetItems("ClCompile").Concat(project.GetItems("ClInclude")))
            {
                string actual = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(projectPath), item.EvaluatedInclude));
                if (String.Equals(actual, expected, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("Source is already included in a configuration");
            }
            return;
        }
        var matches = project.GetItems("ClCompile").Where(i => i.EvaluatedInclude == include).ToArray();
        if (matches.Length != 1 || matches[0].UnevaluatedInclude != include ||
            !String.Equals(matches[0].Xml.ContainingProject.FullPath, projectPath, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Source ownership is ambiguous or differs across configurations");
    }

    static RemovalPlan AddPlan(byte[] bytes, string include, string itemType, string projectPath)
    {
        string xml = new UTF8Encoding(false, true).GetString(bytes);
        int bom = xml.Length != 0 && xml[0] == '\ufeff' ? 3 : 0;
        if (bom != 0) xml = xml.Substring(1);
        var doc = XDocument.Parse(xml, LoadOptions.PreserveWhitespace);
        string expected = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(projectPath), include));
        foreach (var item in doc.Descendants().Where(e => (e.Name.LocalName == "ClCompile" || e.Name.LocalName == "ClInclude") &&
                                                           e.Parent != null && e.Parent.Name.LocalName == "ItemGroup"))
        {
            var candidate = item.Attribute("Include");
            if (candidate == null || candidate.Value.IndexOfAny(new[] {'*', '?', '$', '@', '%', ';'}) >= 0)
                throw new InvalidOperationException("Dynamic source items are unsupported");
            string actual = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(projectPath), candidate.Value));
            if (String.Equals(actual, expected, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Source already has a project or filters entry");
        }
        var nodes = doc.Descendants().Where(e => e.Name.LocalName == itemType && e.Parent != null &&
                                                   e.Parent.Name.LocalName == "ItemGroup").ToArray();
        if (nodes.Any(e => e.AncestorsAndSelf().Any(a => a.Attributes().Any(x =>
            x.Name.LocalName.Equals("Condition", StringComparison.OrdinalIgnoreCase))) ||
            e.Attributes().Any(a => a.Name.LocalName != "Include") || e.Attribute("Include") == null ||
            ((string)e.Attribute("Include")).IndexOfAny(new[] {'*', '?', '$', '@', '%', ';'}) >= 0))
            throw new InvalidOperationException("Source items must be unconditional and literal");
        string insertion = "<" + itemType + " " + new XAttribute("Include", include).ToString() + " />";
        if (nodes.Length == 0)
        {
            var ends = Regex.Matches(xml, @"</Project\s*>");
            if (ends.Count == 0) throw new InvalidOperationException("Project closing element is missing");
            int at = ends[ends.Count - 1].Index;
            string group = "<ItemGroup>" + insertion + "</ItemGroup>";
            doc.Root.Add(new XElement(doc.Root.Name.Namespace + "ItemGroup",
                new XElement(doc.Root.Name.Namespace + itemType, new XAttribute("Include", include))));
            var inserted = XDocument.Parse(xml.Insert(at, group), LoadOptions.PreserveWhitespace);
            if (ComparableXml(doc) != ComparableXml(inserted)) throw new InvalidOperationException("Invalid insertion point");
            string emptyHash;
            using (var sha = SHA256.Create()) emptyHash = BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "");
            return new RemovalPlan { Sha256 = emptyHash, Offset = bom + Encoding.UTF8.GetByteCount(xml.Substring(0, at)),
                                     Length = 0, Insert = group };
        }
        var anchor = nodes[0];
        if (anchor.Parent == null || anchor.Parent.Name.LocalName != "ItemGroup" || anchor.Parent.Parent != doc.Root ||
            nodes.Any(e => e.Parent != anchor.Parent))
            throw new InvalidOperationException("Source items must share one direct item group");
        string existing = (string)anchor.Attribute("Include");
        var matches = Regex.Matches(xml, "<" + itemType + "\\s+Include=(['\"])" + Regex.Escape(existing) + "\\1");
        if (matches.Count != 1) throw new InvalidOperationException("Source insertion point is ambiguous");
        var match = matches[0];
        anchor.AddBeforeSelf(new XElement(anchor.Name, new XAttribute("Include", include)));
        var edited = XDocument.Parse(xml.Insert(match.Index, insertion), LoadOptions.PreserveWhitespace);
        if (ComparableXml(doc) != ComparableXml(edited))
            throw new InvalidOperationException("Source insertion point does not match XML");
        string hash;
        using (var sha = SHA256.Create()) hash = BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "");
        return new RemovalPlan { Sha256 = hash, Offset = bom + Encoding.UTF8.GetByteCount(xml.Substring(0, match.Index)),
                                 Length = 0, Insert = insertion };
    }

    static RemovalPlan RemovePlan(byte[] bytes, string include, bool filters)
    {
        string xml = new UTF8Encoding(false, true).GetString(bytes);
        int bom = xml.Length != 0 && xml[0] == '\ufeff' ? 3 : 0;
        if (bom != 0) xml = xml.Substring(1);
        var doc = XDocument.Parse(xml, LoadOptions.PreserveWhitespace);
        var nodes = doc.Descendants().Where(e => e.Name.LocalName == "ClCompile" &&
            (string)e.Attribute("Include") == include).ToArray();
        if (nodes.Length != 1 || nodes[0].AncestorsAndSelf().Any(e => e.Attributes().Any(a =>
            a.Name.LocalName.Equals("Condition", StringComparison.OrdinalIgnoreCase))))
            throw new InvalidOperationException("Conditional or ambiguous source item");
        if (doc.Descendants().Where(e => e.Name.LocalName == "ClCompile").Attributes().Any(a =>
            a.Name.LocalName == "Include" && a.Value.IndexOfAny(new[] {'*', '?', '$', '@', '%', ';'}) >= 0))
            throw new InvalidOperationException("Nonliteral source include");
        if (filters && nodes[0].Elements().Any(e => e.Name.LocalName != "Filter" || e.HasElements || e.HasAttributes))
            throw new InvalidOperationException("Unsupported filter metadata");
        string ending = filters ? @"\s*(?:/>|>\s*<Filter>[^<]*</Filter>\s*</ClCompile>)" : @"\s*/>";
        var matches = Regex.Matches(xml, "<ClCompile\\s+Include=(['\"])" + Regex.Escape(include) + "\\1" + ending);
        if (matches.Count != 1) throw new InvalidOperationException("Expected one simple self-closing source item");
        var match = matches[0];
        nodes[0].Remove();
        var edited = XDocument.Parse(xml.Remove(match.Index, match.Length), LoadOptions.PreserveWhitespace);
        // Removing an element can leave adjacent text nodes in the in-memory tree. Serializing both
        // parsed trees coalesces those nodes for comparison; the returned edit still uses original bytes.
        if (ComparableXml(doc) != ComparableXml(edited))
            throw new InvalidOperationException("Text span does not identify the source item");
        string hash;
        using (var sha = SHA256.Create()) hash = BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "");
        return new RemovalPlan { Sha256 = hash, Offset = bom + Encoding.UTF8.GetByteCount(xml.Substring(0, match.Index)),
                                 Length = Encoding.UTF8.GetByteCount(match.Value) };
    }

    static string ComparableXml(XDocument doc)
    {
        // Empty <x></x> and <x/> are the same XML element; leave all text, comments and attributes intact.
        foreach (var element in doc.Descendants().Where(e => !e.Nodes().Any())) element.RemoveNodes();
        return doc.ToString(SaveOptions.DisableFormatting);
    }
}
