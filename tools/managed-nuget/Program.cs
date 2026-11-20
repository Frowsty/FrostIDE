// SPDX-License-Identifier: Apache-2.0
// Read-only MSBuild construction planner. Native REF1 owns preview, admission, persistence, recovery and undo.
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using System.Text;
using System.Text.Json;
using System.Xml;
using Microsoft.Build.Construction;
using Microsoft.Build.Evaluation;
using NuGet.Versioning;

sealed class Utf8Writer : StringWriter { public override Encoding Encoding => new UTF8Encoding(false); }

static class Program
{
    static int Main(string[] args)
    {
        try
        {
            if (args.Length != 2 || args[0] != "--sdk" || !Path.IsPathFullyQualified(args[1])) throw new InvalidDataException("An absolute user-installed .NET SDK directory is required");
            var sdk = Path.GetFullPath(args[1]);
            Environment.SetEnvironmentVariable("MSBUILD_EXE_PATH", Path.Combine(sdk, "MSBuild.dll"));
            Environment.SetEnvironmentVariable("MSBuildSDKsPath", Path.Combine(sdk, "Sdks"));
            Environment.SetEnvironmentVariable("MSBuildExtensionsPath", sdk);
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
        using var collection = new ProjectCollection();
        string Text(string key) => request.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString()! : "";
        if (!request.TryGetProperty("trusted", out var trusted) || trusted.ValueKind != JsonValueKind.True) throw new InvalidDataException("Workspace is not trusted");
        if (!Path.IsPathFullyQualified(Text("root"))) throw new InvalidDataException("Workspace root must be absolute");
        var root = Path.GetFullPath(Text("root"));
        string Definition(string path)
        {
            if (!Path.IsPathFullyQualified(path)) throw new InvalidDataException("Definition path must be absolute");
            var full = Path.GetFullPath(path);
            var relative = Path.GetRelativePath(root, full);
            if (Path.IsPathFullyQualified(relative) || relative.Split(Path.DirectorySeparatorChar).Contains("..")) throw new InvalidDataException("Package definition leaves the workspace");
            for (var current = new FileInfo(full).FullName; !string.IsNullOrEmpty(current); current = Path.GetDirectoryName(current))
                if (File.Exists(current) || Directory.Exists(current)) if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0) throw new InvalidDataException("Linked package definitions require an explicit supported physical ownership model");
            return full;
        }
        var project = Definition(Text("project"));
        if (!project.EndsWith(".csproj", StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Select a C# SDK project");
        var package = Text("package"); var version = Text("version"); var operation = Text("operation");
        bool Literal(string text) => text.Length != 0 && text.All(c => char.IsAsciiLetterOrDigit(c) || ".-_+[](), ".Contains(c));
        if (package.Length > 100 || !Literal(package) || package.Any(c => !char.IsAsciiLetterOrDigit(c) && c != '.' && c != '-' && c != '_')) throw new InvalidDataException("Invalid package ID");
        if (operation != "remove" && (version.Length > 256 || !Literal(version) || !VersionRange.TryParse(version, out _))) throw new InvalidDataException("An explicit literal package version or range is required");
        if (operation != "install" && operation != "update" && operation != "remove") throw new InvalidDataException("Unknown package operation");
        var files = new List<object>();
        (ProjectRootElement Xml, byte[] Before, bool Bom, string Newline) Load(string file)
        {
            var bytes = File.ReadAllBytes(file); if (bytes.Length > 2 * 1024 * 1024) throw new InvalidDataException("Package definition is too large");
            bool bom = bytes.AsSpan().StartsWith(new byte[] { 239, 187, 191 });
            var content = new UTF8Encoding(false, true).GetString(bytes, bom ? 3 : 0, bytes.Length - (bom ? 3 : 0));
            if (content.Contains("\r\n") && content.Replace("\r\n", "").Contains('\n')) throw new InvalidDataException("Mixed line endings are unsupported");
            using var reader = XmlReader.Create(new StringReader(content), new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null });
            while (reader.Read()) { }
            var xml = ProjectRootElement.Open(file, collection, true) ?? throw new InvalidDataException("Cannot load package definition");
            return (xml, bytes, bom, content.Contains("\r\n") ? "\r\n" : "\n");
        }
        ProjectItemElement? Find(ProjectRootElement xml, string type)
        {
            var matches = xml.Items.Where(item => item.ItemType == type && string.Equals(item.Include, package, StringComparison.OrdinalIgnoreCase)).ToArray();
            if (matches.Length > 1) throw new InvalidDataException("Multiple declarations of this package require a scoped edit");
            var item = matches.SingleOrDefault();
            if (item != null && (item.Condition.Length != 0 || item.Parent.Condition.Length != 0 || item.Parent is not ProjectItemGroupElement || item.Parent.Parent != xml || item.Update.Length != 0 || item.Remove.Length != 0)) throw new InvalidDataException("Conditional or indirect package declarations require a scoped edit");
            if (xml.Items.Any(item => item.ItemType == type && (item.Include.IndexOfAny(new[] { '$', '@', '*', '?', ';' }) >= 0 || item.Exclude.Length != 0 || item.Update.Equals(package, StringComparison.OrdinalIgnoreCase) || item.Remove.Equals(package, StringComparison.OrdinalIgnoreCase)))) throw new InvalidDataException("Dynamic package declarations prevent an unambiguous edit");
            return item;
        }
        void SetVersion(ProjectItemElement item)
        {
            if (item.Metadata.Any(m => m.Name == "VersionOverride" || m.Condition.Length != 0)) throw new InvalidDataException("Version overrides or conditional metadata require a scoped edit");
            var versions = item.Metadata.Where(m => m.Name == "Version").ToArray();
            if (versions.Any(m => m.Value.Contains("$(") || m.Value.Contains("@("))) throw new InvalidDataException("Package version comes from an expression; edit its evaluated owning property instead");
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
        var state = Load(project);
        var centralFile = Text("centralFile");
        var centralPath = centralFile.Length == 0 ? "" : Definition(centralFile);
        (ProjectRootElement Xml, byte[] Before, bool Bom, string Newline)? centralState = centralPath.Length == 0 ? null : Load(centralPath);
        if (Text("configuration").Length == 0 || Text("targetFramework").Length == 0) throw new InvalidDataException("Explicit evaluated configuration and framework are required");
        var globals = new Dictionary<string, string> { ["Configuration"] = Text("configuration"), ["TargetFramework"] = Text("targetFramework") };
        var evaluated = new Project(state.Xml, globals, null, collection);
        if (evaluated.GetPropertyValue("UsingMicrosoftNETSdk") != "true") throw new InvalidDataException("Select an SDK-style C# project");
        if ((evaluated.GetPropertyValue("ManagePackageVersionsCentrally") == "true") != (centralState != null)) throw new InvalidDataException("Central package ownership differs from the evaluated project");
        var existing = evaluated.GetItems("PackageReference").Where(item => item.EvaluatedInclude.Equals(package, StringComparison.OrdinalIgnoreCase)).ToArray();
        if ((operation == "install" && existing.Length != 0) || (operation != "install" && existing.Length != 1)) throw new InvalidDataException("Evaluated package identity is absent, duplicated or already installed");
        if (existing.Any(item => !item.Xml.ContainingProject.FullPath.Equals(project, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))) throw new InvalidDataException("Package reference is owned by an import");
        var reference = Find(state.Xml, "PackageReference");
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
                var central = centralPath; var centralDefinition = centralState!.Value; var entry = Find(centralDefinition.Xml, "PackageVersion");
                var activeVersions = evaluated.GetItems("PackageVersion").Where(item => item.EvaluatedInclude.Equals(package, StringComparison.OrdinalIgnoreCase)).ToArray();
                if (activeVersions.Length > 1 || activeVersions.Any(item => !item.Xml.ContainingProject.FullPath.Equals(central, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))) throw new InvalidDataException("Central package version is duplicated or owned by an import");
                if (operation == "install" && entry != null && entry.Metadata.Any(m => m.Name == "Version" && m.Value != version)) throw new InvalidDataException("Central version is shared; explicitly update its declaration");
                entry ??= centralDefinition.Xml.AddItem("PackageVersion", package); SetVersion(entry);
            }
        }
        evaluated.MarkDirty();
        evaluated.ReevaluateIfNecessary();
        var projected = evaluated.GetItems("PackageReference").Where(item => item.EvaluatedInclude.Equals(package, StringComparison.OrdinalIgnoreCase)).ToArray();
        if (operation == "remove") { if (projected.Length != 0) throw new InvalidDataException("An imported declaration still supplies the removed package"); }
        else
        {
            if (projected.Length != 1) throw new InvalidDataException("Proposed package is absent or duplicated after actual MSBuild reevaluation");
            if (centralState == null) { if (projected[0].GetMetadataValue("Version") != version) throw new InvalidDataException("An imported declaration overrides the proposed package version"); }
            else
            {
                var versions = evaluated.GetItems("PackageVersion").Where(item => item.EvaluatedInclude.Equals(package, StringComparison.OrdinalIgnoreCase)).ToArray();
                if (projected[0].GetMetadataValue("Version").Length != 0 || projected[0].GetMetadataValue("VersionOverride").Length != 0 || versions.Length != 1 || versions[0].GetMetadataValue("Version") != version) throw new InvalidDataException("Central version proposal is overridden by evaluated project logic");
                Emit(centralPath, centralState.Value);
            }
        }
        Emit(project, state);
        Console.WriteLine(JsonSerializer.Serialize(new { version = 1, project, package, operation, files })); return 0;
    }
}
