// SPDX-License-Identifier: Apache-2.0
// SLN-6: adding a project to and removing one from a .sln or .slnx through
// Microsoft.VisualStudio.SolutionPersistence (MIT; ADR 0082), the library Visual Studio and MSBuild use
// for solution files, loaded from the Visual Studio instance's MSBuild\Current\Bin. Never a text edit.
/*
 * Request (one JSON line of strings; frost::project::solutionEditRequest builds it):
 *   Solution (absolute .sln or .slnx), Root (the trusted root), Trusted="true", Commit="true",
 *   TransactionId (a GUID), and exactly one of AddProject=<absolute .vcxproj> or
 *   RemoveProject=<absolute project path as the solution lists it>.
 *
 * SolutionPersistence reads the snapshot, the model is edited (AddProject puts the project at the
 * solution root with the project's own ProjectGuid; RemoveProject also drops the other projects'
 * dependencies on it) and saved with the same serializer. The saved text keeps the file's byte order
 * mark, line endings and (for .sln) its header lines (Restyle says why).
 *
 * SolutionPersistence normalises a solution while saving: a .sln whose configurations are not the
 * full build-type x platform matrix gains the missing ones (Generated.sln's Profile|x86). FrostIDE
 * writes nothing in that case. Before the commit the result is compared with the original:
 *   .sln:  line by line (as multisets), every added or removed line must belong to the project:
 *          contain its GUID, be its one EndProject line or (removal) an emptied
 *          ProjectSection(ProjectDependencies) header and footer of another project;
 *   .slnx: as XML, the result equals the original with the project's <Project> element and the
 *          <BuildDependency> elements naming it added or removed.
 * Anything else refuses the edit and names the first unexpected line or element.
 */
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Xml;
using System.Xml.Linq;
using Microsoft.Build.Construction;
using Microsoft.Build.Evaluation;
using Microsoft.VisualStudio.SolutionPersistence;
using Microsoft.VisualStudio.SolutionPersistence.Model;
using Microsoft.VisualStudio.SolutionPersistence.Serializer;

static class SolutionEdits
{
    static bool IsSlnx(string path)
    {
        return Path.GetExtension(path).Equals(".slnx", StringComparison.OrdinalIgnoreCase);
    }

    static string Relative(string solutionDirectory, string path, bool slnx)
    {
        string include = ProjectFileEdits.IncludeFor(solutionDirectory, path, false);
        return slnx ? include.Replace('\\', '/') : include;
    }

    static string FullPathOf(string solutionDirectory, string relative)
    {
        string native = relative.Replace('/', '\\');
        return Path.GetFullPath(Path.IsPathRooted(native) ? native : Path.Combine(solutionDirectory, native));
    }

    // The <ProjectGuid> of a project file (its Globals), or null.
    static Guid? ProjectGuidOf(string projectPath)
    {
        using (var collection = new ProjectCollection())
        {
            var settings = new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null };
            using (var reader = XmlReader.Create(projectPath, settings))
            {
                var root = ProjectRootElement.Create(reader, collection, false);
                foreach (var property in root.Properties)
                {
                    Guid guid;
                    if (property.Name.Equals("ProjectGuid", StringComparison.OrdinalIgnoreCase) && property.Condition.Length == 0 &&
                        Guid.TryParse(property.Value, out guid))
                        return guid;
                }
            }
        }
        return null;
    }

    static SolutionModel Open(ISolutionSerializer serializer, byte[] bytes)
    {
        using (var stream = new MemoryStream(bytes, false))
        {
            var sln = serializer as ISolutionSingleFileSerializer<Microsoft.VisualStudio.SolutionPersistence.Serializer.SlnV12.SlnV12SerializerSettings>;
            var slnx = serializer as ISolutionSingleFileSerializer<Microsoft.VisualStudio.SolutionPersistence.Serializer.Xml.SlnxSerializerSettings>;
            if (sln != null) return sln.OpenAsync(stream, CancellationToken.None).GetAwaiter().GetResult();
            if (slnx != null) return slnx.OpenAsync(stream, CancellationToken.None).GetAwaiter().GetResult();
        }
        throw new InvalidOperationException("SolutionPersistence has no single-file serializer for this solution");
    }

    static byte[] Save(ISolutionSerializer serializer, SolutionModel model)
    {
        using (var stream = new MemoryStream())
        {
            var sln = serializer as ISolutionSingleFileSerializer<Microsoft.VisualStudio.SolutionPersistence.Serializer.SlnV12.SlnV12SerializerSettings>;
            var slnx = serializer as ISolutionSingleFileSerializer<Microsoft.VisualStudio.SolutionPersistence.Serializer.Xml.SlnxSerializerSettings>;
            if (sln != null) sln.SaveAsync(stream, model, CancellationToken.None).GetAwaiter().GetResult();
            else slnx.SaveAsync(stream, model, CancellationToken.None).GetAwaiter().GetResult();
            return stream.ToArray();
        }
    }

    static string Decode(byte[] bytes, out bool bom)
    {
        bom = bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF;
        return new UTF8Encoding(false, true).GetString(bytes, bom ? 3 : 0, bytes.Length - (bom ? 3 : 0));
    }

    // The quoted fields of a .sln project line, Project("{type}") = "name", "path", "{guid}"
    // (.sln fields never contain quotes), or null for any other line.
    static string[] ProjectLineFields(string line)
    {
        if (!line.StartsWith("Project(\"", StringComparison.Ordinal)) return null;
        string[] pieces = line.Split('"');
        if (pieces.Length != 9) return null;
        return new[] { pieces[1], pieces[3], pieces[5], pieces[7] };
    }

    // The index of the first line of a .sln's body (its first Project or Global line), or -1.
    static int BodyStart(string[] lines)
    {
        for (int i = 0; i < lines.Length; i++)
            if (lines[i].StartsWith("Project(", StringComparison.Ordinal) || lines[i] == "Global") return i;
        return -1;
    }

    /*
     * The saved bytes given the original's byte order mark, line endings and, for a .sln, its header:
     * the lines before the first Project or Global line (the format line, the "# Visual Studio
     * Version" comment, VisualStudioVersion, MinimumVisualStudioVersion and a leading blank line).
     * An edit never changes them, but SolutionPersistence renders them anew: it drops the leading
     * blank line and Visual Studio's suffix of VisualStudioVersion ("17.14.36109.1 d17.14"). A kept
     * project's Project line is the original too when only its display name differs.
     */
    static byte[] Restyle(byte[] original, byte[] saved, bool slnx)
    {
        bool bom, ignored;
        string before = Decode(original, out bom);
        string after = Decode(saved, out ignored).Replace("\r\n", "\n");
        string newline = before.Contains("\r\n") ? "\r\n" : "\n";
        if (!slnx)
        {
            string[] oldLines = before.Replace("\r\n", "\n").Split('\n'), newLines = after.Split('\n');
            int oldBody = BodyStart(oldLines), newBody = BodyStart(newLines);
            if (oldBody < 0 || newBody < 0) throw new InvalidOperationException("The solution has no Project or Global section");
            // SolutionPersistence writes a project's file name where the solution gave it another
            // display name ("Notepad++" for notepadPlus.vcxproj); keep the solution's own line.
            var originals = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (string line in oldLines.Skip(oldBody))
            {
                var fields = ProjectLineFields(line);
                if (fields != null) originals[fields[0] + "\u0001" + fields[2] + "\u0001" + fields[3]] = line;
            }
            var body = newLines.Skip(newBody).Select(line => {
                var fields = ProjectLineFields(line);
                string kept;
                return fields != null && originals.TryGetValue(fields[0] + "\u0001" + fields[2] + "\u0001" + fields[3], out kept)
                    ? kept : line;
            });
            after = String.Join("\n", oldLines.Take(oldBody).Concat(body));
        }
        if (newline == "\r\n") after = after.Replace("\n", "\r\n");
        byte[] encoded = new UTF8Encoding(false, true).GetBytes(after);
        return bom ? new byte[] { 0xEF, 0xBB, 0xBF }.Concat(encoded).ToArray() : encoded;
    }

    static Dictionary<string, int> Lines(string text)
    {
        var counts = new Dictionary<string, int>(StringComparer.Ordinal);
        foreach (string line in text.Replace("\r\n", "\n").Split('\n'))
        {
            int count;
            counts.TryGetValue(line, out count);
            counts[line] = count + 1;
        }
        return counts;
    }

    // The lines `a` has more often than `b`, with the surplus counts.
    static List<KeyValuePair<string, int>> Surplus(Dictionary<string, int> a, Dictionary<string, int> b)
    {
        var surplus = new List<KeyValuePair<string, int>>();
        foreach (var entry in a)
        {
            int other;
            b.TryGetValue(entry.Key, out other);
            if (entry.Value > other) surplus.Add(new KeyValuePair<string, int>(entry.Key, entry.Value - other));
        }
        return surplus;
    }

    // The .sln check of the header comment; throws naming the first unexpected line.
    static void CheckSln(string before, string after, Guid project, bool adding)
    {
        var a = Lines(before);
        var b = Lines(after);
        var gained = Surplus(b, a);
        var lost = Surplus(a, b);
        var changed = adding ? gained : lost;
        var other = adding ? lost : gained;
        if (other.Count != 0)
            throw new InvalidOperationException("SolutionPersistence would also " + (adding ? "remove" : "add") +
                                                " the line '" + other[0].Key.Trim() + "'; no file was changed");
        string guid = project.ToString("B");
        int endProject = 0, sections = 0, sectionEnds = 0;
        foreach (var line in changed)
        {
            string text = line.Key.Trim();
            if (text.IndexOf(guid, StringComparison.OrdinalIgnoreCase) >= 0) continue;
            if (text == "EndProject") { endProject += line.Value; continue; }
            if (!adding && text == "ProjectSection(ProjectDependencies) = postProject") { sections += line.Value; continue; }
            if (!adding && text == "EndProjectSection") { sectionEnds += line.Value; continue; }
            throw new InvalidOperationException("SolutionPersistence would also " + (adding ? "add" : "remove") +
                                                " the line '" + text + "'; no file was changed");
        }
        if (endProject != 1 || sections != sectionEnds)
            throw new InvalidOperationException("SolutionPersistence changed the solution's project blocks unexpectedly; no file was changed");
    }

    // Removes from `doc` the <Project> of `project` and every <BuildDependency> naming it.
    static void StripProject(XDocument doc, string solutionDirectory, string project)
    {
        Func<XElement, string, bool> names = (element, attribute) => {
            var value = (string)element.Attribute(attribute);
            return value != null && String.Equals(FullPathOf(solutionDirectory, value), project, StringComparison.OrdinalIgnoreCase);
        };
        foreach (var element in doc.Descendants().Where(e => (e.Name.LocalName == "Project" && names(e, "Path")) ||
                                                               (e.Name.LocalName == "BuildDependency" && names(e, "Project"))).ToList())
            element.Remove();
    }

    static string Comparable(string xml)
    {
        var doc = XDocument.Parse(xml, LoadOptions.None);
        foreach (var element in doc.Descendants().Where(e => !e.Nodes().Any())) element.RemoveNodes();
        return doc.ToString(SaveOptions.DisableFormatting);
    }

    static void CheckSlnx(string before, string after, string solutionDirectory, string project, bool adding)
    {
        var withProject = XDocument.Parse(adding ? after : before, LoadOptions.None);
        StripProject(withProject, solutionDirectory, project);
        string without = adding ? before : after;
        if (Comparable(withProject.ToString(SaveOptions.DisableFormatting)) != Comparable(without))
            throw new InvalidOperationException("SolutionPersistence would also change other parts of the solution; no file was changed");
        var present = XDocument.Parse(adding ? after : before);
        if (!present.Descendants().Any(e => e.Name.LocalName == "Project" && e.Attribute("Path") != null &&
                                            String.Equals(FullPathOf(solutionDirectory, (string)e.Attribute("Path")), project,
                                                          StringComparison.OrdinalIgnoreCase)))
            throw new InvalidOperationException("The solution edit did not " + (adding ? "add" : "remove") + " the project");
    }

    internal static object Handle(Dictionary<string, string> request, string rootPath, string solutionPath,
                                  Func<string, string, bool> approve)
    {
        string solution = Path.GetFullPath(solutionPath);
        string ext = Path.GetExtension(solution);
        if (!ext.Equals(".sln", StringComparison.OrdinalIgnoreCase) && !IsSlnx(solution))
            throw new InvalidOperationException("Choose a .sln or .slnx solution");
        string add, remove;
        request.TryGetValue("AddProject", out add);
        request.TryGetValue("RemoveProject", out remove);
        if ((add == null) == (remove == null)) throw new InvalidOperationException("Choose exactly one edit: add or remove a project");
        string project = Path.GetFullPath(add ?? remove);
        if (!Path.IsPathRooted(add ?? remove)) throw new InvalidOperationException("The project path must be absolute");
        string directory = Path.GetDirectoryName(solution);
        bool slnx = IsSlnx(solution);
        var fence = TransactionProbe.FenceEvaluation(rootPath, solution);
        try
        {
            byte[] snapshot = File.ReadAllBytes(solution);
            var serializer = slnx ? (ISolutionSerializer)SolutionSerializers.SlnXml : SolutionSerializers.SlnFileV12;
            var model = Open(serializer, snapshot);
            Func<SolutionProjectModel, bool> isProject = p =>
                String.Equals(FullPathOf(directory, p.FilePath), project, StringComparison.OrdinalIgnoreCase);
            Guid id;
            if (add != null)
            {
                if (!File.Exists(project) || !Path.GetExtension(project).Equals(".vcxproj", StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("Choose an existing .vcxproj project");
                if (model.SolutionProjects.Any(isProject)) throw new InvalidOperationException("The solution already contains the project");
                var added = model.AddProject(Relative(directory, project, slnx), null, null);
                Guid? guid = ProjectGuidOf(project);
                if (guid.HasValue)
                {
                    if (model.SolutionProjects.Any(p => p != added && p.Id == guid.Value))
                        throw new InvalidOperationException("Another project of the solution has the same ProjectGuid");
                    added.Id = guid.Value;
                }
                id = added.Id;
            }
            else
            {
                var found = model.SolutionProjects.Where(isProject).ToList();
                if (found.Count != 1) throw new InvalidOperationException("The solution does not list the project exactly once");
                id = found[0].Id;
                if (!model.RemoveProject(found[0])) throw new InvalidOperationException("SolutionPersistence did not remove the project");
            }
            byte[] after = Restyle(snapshot, Save(serializer, model), slnx);
            bool bom;
            string beforeText = Decode(snapshot, out bom), afterText = Decode(after, out bom);
            if (slnx) CheckSlnx(beforeText, afterText, directory, project, add != null);
            else CheckSln(beforeText, afterText, id, add != null);
            string commit, transactionId;
            if (request.TryGetValue("Commit", out commit) && commit == "true")
            {
                if (!request.TryGetValue("TransactionId", out transactionId))
                    throw new InvalidOperationException("Trusted root and transaction identifier required");
                fence.Dispose(); // The transaction reacquires the solution exclusively and revalidates it.
                TransactionProbe.Execute(new TransactionRequest {
                    Root = rootPath, Journal = Path.Combine(rootPath, ".frost-project-edit-journal"), Trusted = true,
                    TransactionId = transactionId, Entries = new[] { ProjectFileEdits.Entry(solution, snapshot, after) }
                }, digest => approve(transactionId, digest));
            }
            return new { Items = (object)null, Error = (string)null,
                         Edit = new { Solution = Convert.ToBase64String(after), ProjectId = id.ToString("B").ToUpperInvariant() } };
        }
        finally
        {
            fence.Dispose();
        }
    }
}
