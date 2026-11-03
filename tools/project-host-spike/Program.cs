// SPDX-License-Identifier: Apache-2.0
// Experimental JSON-lines MSBuild item ownership probe; never writes projects.
using System.Text.Json;
using System.Text;
using System.Text.RegularExpressions;
using System.Security.Cryptography;
using System.Xml.Linq;
using Microsoft.Build.Evaluation;

record Request(string Project, string Configuration, string Platform, string? Remove = null);
record Item(string Type, string EvaluatedInclude, string UnevaluatedInclude,
            string DefiningProject, int Line, string Condition);
record Plan(string Sha256, int Offset, int Length);
record Response(Item[]? Items, Plan? Edit, string? Error);

static class Program
{
    static void Main()
    {
        string? line;
        while ((line = Console.ReadLine()) != null)
        {
            try
            {
                var request = JsonSerializer.Deserialize<Request>(line)
                    ?? throw new ArgumentException("Missing request");
                if (!Path.IsPathFullyQualified(request.Project))
                    throw new ArgumentException("Project path must be absolute");
                byte[]? snapshot = request.Remove == null ? null : File.ReadAllBytes(request.Project);
                var properties = new Dictionary<string, string> {
                    ["Configuration"] = request.Configuration,
                    ["Platform"] = request.Platform
                };
                using var collection = new ProjectCollection(properties);
                var project = collection.LoadProject(request.Project);
                var items = project.AllEvaluatedItems.Select(item => new Item(
                    item.ItemType, item.EvaluatedInclude, item.UnevaluatedInclude,
                    item.Xml.ContainingProject.FullPath, item.Xml.Location.Line,
                    item.Xml.Condition)).ToArray();
                if (snapshot != null && !snapshot.AsSpan().SequenceEqual(File.ReadAllBytes(request.Project)))
                    throw new InvalidOperationException("Project changed during evaluation");
                var edit = snapshot == null ? null : MakeRemovePlan(request, items, snapshot);
                Console.WriteLine(JsonSerializer.Serialize(new Response(items, edit, null)));
            }
            catch (Exception ex)
            {
                Console.WriteLine(JsonSerializer.Serialize(new Response(null, null, ex.Message)));
            }
        }
    }

    // This first plan is intentionally limited to UTF-8, import-free projects with literal,
    // unconditioned item includes and a unique self-closing item. It never writes a file.
    static Plan MakeRemovePlan(Request request, Item[] evaluated, byte[] bytes)
    {
        string xml = new UTF8Encoding(false, true).GetString(bytes);
        if (xml.StartsWith('\ufeff')) throw new InvalidOperationException("UTF-8 BOM is not supported by this plan");
        var doc = XDocument.Parse(xml, LoadOptions.PreserveWhitespace);
        var elements = doc.Descendants().ToArray();
        if (elements.Any(e => e.Name.LocalName is "Import" or "ImportGroup" or "Choose" ||
                              e.Attributes().Any(a => a.Name.LocalName.Equals("Condition", StringComparison.OrdinalIgnoreCase))))
            throw new InvalidOperationException("Imports and conditions are not supported by this plan");
        var candidates = evaluated.Where(i => i.EvaluatedInclude == request.Remove).ToArray();
        if (candidates.Length != 1 || candidates[0].UnevaluatedInclude != request.Remove ||
            !string.Equals(candidates[0].DefiningProject, request.Project, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Item ownership is ambiguous or not a literal local item");
        if (elements.SelectMany(e => e.Attributes()).Any(a =>
            a.Name.LocalName.Equals("Include", StringComparison.OrdinalIgnoreCase) &&
            a.Value.IndexOfAny(['*', '?', '$', '@', '%', ';']) >= 0))
            throw new InvalidOperationException("Nonliteral includes are not supported by this plan");
        string type = Regex.Escape(candidates[0].Type);
        string include = Regex.Escape(request.Remove!);
        var matches = Regex.Matches(xml, $"<{type}\\s+Include=(['\"]){include}\\1\\s*/>");
        if (matches.Count != 1)
            throw new InvalidOperationException("Expected one simple self-closing item");
        var match = matches[0];
        var nodes = elements.Where(e => e.Name.LocalName == candidates[0].Type &&
            (string?)e.Attribute("Include") == request.Remove).ToArray();
        if (nodes.Length != 1)
            throw new InvalidOperationException("Item syntax is ambiguous");
        nodes[0].Remove();
        var edited = XDocument.Parse(xml.Remove(match.Index, match.Length), LoadOptions.PreserveWhitespace);
        if (ComparableXml(doc) != ComparableXml(edited))
            throw new InvalidOperationException("Text span does not identify the item");
        int offset = Encoding.UTF8.GetByteCount(xml.AsSpan(0, match.Index));
        int length = Encoding.UTF8.GetByteCount(xml.AsSpan(match.Index, match.Length));
        return new Plan(Convert.ToHexString(SHA256.HashData(bytes)), offset, length);
    }

    static string ComparableXml(XDocument doc)
    {
        foreach (var element in doc.Descendants().Where(e => !e.Nodes().Any())) element.RemoveNodes();
        return doc.ToString(SaveOptions.DisableFormatting);
    }
}
