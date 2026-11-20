// SPDX-License-Identifier: Apache-2.0
// Read-only MSBuild construction planner. Native REF1 owns preview, admission, persistence, recovery and undo.
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using System.Text;
using System.Text.Json;
using System.Xml;
using Microsoft.Build.Construction;

sealed class Utf8Writer : StringWriter { public override Encoding Encoding => new UTF8Encoding(false); }

static class Program
{
    static int Main(string[] args)
    {
        try
        {
            if (args.Length != 2 || args[0] != "--sdk" || !Path.IsPathFullyQualified(args[1])) throw new InvalidDataException("An absolute user-installed .NET SDK directory is required");
            var sdk = Path.GetFullPath(args[1]);
            AssemblyLoadContext.Default.Resolving += (_, name) => { var path = Path.Combine(sdk, name.Name + ".dll"); return File.Exists(path) ? AssemblyLoadContext.Default.LoadFromAssemblyPath(path) : null; };
            return Plan();
        }
        catch (Exception error) { Console.WriteLine(JsonSerializer.Serialize(new { error = error.Message })); return 1; }
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    static int Plan()
    {
        var line = Console.ReadLine() ?? throw new InvalidDataException("Missing plan request");
        if (line.Length > 4 * 1024 * 1024) throw new InvalidDataException("Package plan request is too large");
        using var json = JsonDocument.Parse(line);
        var request = json.RootElement;
        string Text(string key) => request.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString()! : "";
        if (!request.TryGetProperty("trusted", out var trusted) || trusted.ValueKind != JsonValueKind.True) throw new InvalidDataException("Workspace is not trusted");
        var root = Path.GetFullPath(Text("root"));
        string Definition(string path)
        {
            if (!Path.IsPathFullyQualified(path)) throw new InvalidDataException("Definition path must be absolute");
            var full = Path.GetFullPath(path);
            if (Path.GetRelativePath(root, full).Split(Path.DirectorySeparatorChar).Contains("..")) throw new InvalidDataException("Package definition leaves the workspace");
            for (var current = new FileInfo(full).FullName; !string.IsNullOrEmpty(current); current = Path.GetDirectoryName(current))
                if (File.Exists(current) || Directory.Exists(current)) if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0) throw new InvalidDataException("Linked package definitions require an explicit supported physical ownership model");
            return full;
        }
        var project = Definition(Text("project"));
        if (!project.EndsWith(".csproj", StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Select a C# SDK project");
        var package = Text("package"); var version = Text("version"); var operation = Text("operation");
        bool Literal(string text) => text.Length != 0 && text.All(c => char.IsAsciiLetterOrDigit(c) || ".-_+[](), ".Contains(c));
        if (!Literal(package) || package.Any(c => !char.IsAsciiLetterOrDigit(c) && c != '.' && c != '-' && c != '_')) throw new InvalidDataException("Invalid package ID");
        if (operation != "remove" && !Literal(version)) throw new InvalidDataException("An explicit literal package version or range is required");
        if (operation != "install" && operation != "update" && operation != "remove") throw new InvalidDataException("Unknown package operation");
        var files = new List<object>();
        (ProjectRootElement Xml, byte[] Before, bool Bom, string Newline) Load(string file)
        {
            var bytes = File.ReadAllBytes(file); if (bytes.Length > 2 * 1024 * 1024) throw new InvalidDataException("Package definition is too large");
            bool bom = bytes.AsSpan().StartsWith(new byte[] { 239, 187, 191 });
            var content = new UTF8Encoding(false, true).GetString(bytes, bom ? 3 : 0, bytes.Length - (bom ? 3 : 0));
            if (content.Contains("\r\n") && content.Replace("\r\n", "").Contains('\n')) throw new InvalidDataException("Mixed line endings are unsupported");
            using var reader = XmlReader.Create(new StringReader(content), new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null });
            var xml = ProjectRootElement.Create(reader, null, true); xml.FullPath = file;
            return (xml, bytes, bom, content.Contains("\r\n") ? "\r\n" : "\n");
        }
        ProjectItemElement? Find(ProjectRootElement xml, string type)
        {
            var matches = xml.Items.Where(item => item.ItemType == type && string.Equals(item.Include, package, StringComparison.OrdinalIgnoreCase)).ToArray();
            if (matches.Length > 1) throw new InvalidDataException("Multiple declarations of this package require a scoped edit");
            var item = matches.SingleOrDefault();
            if (item != null && (item.Condition.Length != 0 || item.Parent.Condition.Length != 0 || item.Parent is not ProjectItemGroupElement || item.Parent.Parent != xml || item.Update.Length != 0 || item.Remove.Length != 0)) throw new InvalidDataException("Conditional or indirect package declarations require a scoped edit");
            if (xml.Items.Any(item => item.ItemType == type && (item.Include.Contains("$(") || item.Include.Contains("@(") || item.Update.Equals(package, StringComparison.OrdinalIgnoreCase) || item.Remove.Equals(package, StringComparison.OrdinalIgnoreCase)))) throw new InvalidDataException("Dynamic package declarations prevent an unambiguous edit");
            return item;
        }
        void SetVersion(ProjectItemElement item)
        {
            if (item.Metadata.Any(m => m.Name == "VersionOverride" || m.Condition.Length != 0)) throw new InvalidDataException("Version overrides or conditional metadata require a scoped edit");
            var versions = item.Metadata.Where(m => m.Name == "Version").ToArray();
            if (versions.Length > 1) throw new InvalidDataException("Ambiguous package version metadata");
            if (versions.Length == 0) item.AddMetadata("Version", version, true); else versions[0].Value = version;
        }
        void Emit(string file, (ProjectRootElement Xml, byte[] Before, bool Bom, string Newline) state)
        {
            using var output = new Utf8Writer(); state.Xml.Save(output);
            var after = output.ToString().Replace("\r\n", "\n").Replace("\r", "\n").Replace("\n", state.Newline);
            if (state.Bom) after = "\uFEFF" + after;
            var bytes = new UTF8Encoding(false, true).GetBytes(after);
            if (!bytes.AsSpan().SequenceEqual(state.Before)) files.Add(new { path = file, beforeBase64 = Convert.ToBase64String(state.Before), afterBase64 = Convert.ToBase64String(bytes) });
        }
        var state = Load(project); var reference = Find(state.Xml, "PackageReference");
        var centralFile = Text("centralFile");
        if (operation == "remove") { if (reference == null) throw new InvalidDataException("Package reference is not directly owned by the selected project"); reference.Parent.RemoveChild(reference); }
        else
        {
            if (operation == "install" && reference != null) throw new InvalidDataException("Package is already referenced; choose Update");
            if (operation == "update" && reference == null) throw new InvalidDataException("Package reference is not directly owned by the selected project");
            reference ??= state.Xml.AddItem("PackageReference", package);
            if (centralFile.Length == 0) SetVersion(reference);
            else
            {
                if (reference.Metadata.Any(m => m.Name == "Version" || m.Name == "VersionOverride")) throw new InvalidDataException("Central package has a project-local version override");
                var central = Definition(centralFile); var centralState = Load(central); var entry = Find(centralState.Xml, "PackageVersion");
                if (operation == "install" && entry != null && entry.Metadata.Any(m => m.Name == "Version" && m.Value != version)) throw new InvalidDataException("Central version is shared; explicitly update its declaration");
                entry ??= centralState.Xml.AddItem("PackageVersion", package); SetVersion(entry); Emit(central, centralState);
            }
        }
        Emit(project, state);
        Console.WriteLine(JsonSerializer.Serialize(new { version = 1, project, package, operation, files })); return 0;
    }
}
